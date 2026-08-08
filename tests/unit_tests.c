#include "config.h"
#include "db.h"
#include "events.h"
#include "http.h"
#include "job.h"
#include "platform.h"
#include "queue.h"
#include "resources.h"
#include "transfer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(condition, message) do { \
    if (condition) printf("PASS: %s\n", message); \
    else { fprintf(stderr, "FAIL: %s\n", message); failures++; } \
} while (0)

void platform_service_start(int argc, char **argv)
{
    (void)argc;
    (void)argv;
}

void platform_request_stop(void)
{
}

int platform_stop_requested(void)
{
    return 1;
}

static Job *test_job(const char *id, int priority)
{
    Job *job = (Job *)calloc(1, sizeof(Job));
    if (!job) return NULL;
    strncpy(job->id, id, sizeof(job->id) - 1);
    strncpy(job->command, "test", sizeof(job->command) - 1);
    strncpy(job->user_id, "tester", sizeof(job->user_id) - 1);
    job->priority = priority;
    job->status = JOB_STATUS_QUEUED;
    job->req_cores = 1;
    job->submitted_at = 1;
    return job;
}

static void test_secure_defaults(void)
{
    config_defaults();
    CHECK(strcmp(g_config.command_mode, "app_only") == 0,
          "app_only is the default command mode");
    CHECK(g_config.cors_allowed_origin[0] == '\0',
          "CORS is disabled by default");
    CHECK(g_config.require_https == 0,
          "HTTPS proxy enforcement is opt-in");
    CHECK(strcmp(g_config.presim_fidelity_map, "0,1,3,6") == 0,
          "presimulation fidelity defaults are cross-platform");
    char error[256];
    CHECK(config_validate(error, sizeof(error)) == 0,
          "default configuration validates");
    g_config.listen_port = 0;
    CHECK(config_validate(error, sizeof(error)) != 0,
          "invalid configuration is rejected");
    const char *invalid_path = "unit_invalid_config.conf";
    FILE *invalid_file = fopen(invalid_path, "wb");
    if (invalid_file) {
        const char *invalid_text = "listen_port = not-a-number\n";
        fwrite(invalid_text, 1, strlen(invalid_text), invalid_file);
        fclose(invalid_file);
        config_defaults();
        CHECK(config_load(invalid_path) != 0,
              "malformed numeric configuration is rejected");
        remove(invalid_path);
    } else {
        CHECK(0, "invalid configuration fixture can be created");
    }
    config_defaults();
}

static void test_repository_configuration(void)
{
    char path[1024];
    char error[256];
    config_defaults();
    snprintf(path, sizeof(path), "%s/config/orchestrator.conf", ORCH_SOURCE_DIR);
    CHECK(config_load(path) == 0, "repository orchestrator config parses");
    snprintf(path, sizeof(path), "%s/config/presim.conf", ORCH_SOURCE_DIR);
    CHECK(config_load(path) == 0, "repository presimulation config parses");
    CHECK(config_validate(error, sizeof(error)) == 0,
          "repository configuration validates");
    config_defaults();
}

static void test_password_hashing(void)
{
    char hash[AUTH_PASSWORD_HASH_LEN] = {0};
    CHECK(auth_hash_password("correct horse battery staple", hash) == 0,
          "PBKDF2 password hashing succeeds");
    CHECK(strncmp(hash, "pbkdf2-sha256$120000$", 21) == 0,
          "password hash records algorithm and work factor");
    CHECK(auth_verify_password("correct horse battery staple", hash) == 1,
          "PBKDF2 password verification accepts the password");
    CHECK(auth_verify_password("wrong password", hash) == 0,
          "PBKDF2 password verification rejects a wrong password");
    CHECK(auth_password_needs_rehash(hash) == 0,
          "current PBKDF2 hashes do not need migration");
    CHECK(auth_verify_password(
              "password",
              "5e884898da28047151d0e56f8dc6292773603d0d6aabbdd62a11ef721d1542d8") == 1,
          "legacy SHA-256 passwords remain verifiable");
    CHECK(auth_password_needs_rehash(
              "5e884898da28047151d0e56f8dc6292773603d0d6aabbdd62a11ef721d1542d8") == 1,
          "legacy hashes are flagged for migration");
}

static void test_filename_validation(void)
{
    CHECK(transfer_valid_filename("input data-01.csv") == 1,
          "safe input filenames are accepted");
    CHECK(transfer_valid_filename("../secret") == 0,
          "path traversal filenames are rejected");
    CHECK(transfer_valid_filename(".app_env.json") == 0,
          "internal hidden files cannot be overwritten");
    CHECK(transfer_valid_filename("file:name.txt") == 0,
          "Windows alternate data streams are rejected");
}

static void test_queue_order_and_removal(void)
{
    Queue *queue = queue_create(2);
    Job *low = test_job("low", 90);
    Job *high = test_job("high", 1);
    Job *middle = test_job("middle", 50);
    CHECK(queue && low && high && middle, "queue test allocations succeed");
    if (!queue || !low || !high || !middle) {
        free(low); free(high); free(middle); queue_destroy(queue);
        return;
    }
    CHECK(queue_push(queue, low) == 0 && queue_push(queue, high) == 0 &&
          queue_push(queue, middle) == 0, "jobs can be queued");
    Job *removed = queue_remove_by_id(queue, "middle");
    CHECK(removed == middle, "an arbitrary queued job can be removed");
    CHECK(queue_try_pop(queue) == high, "lowest priority value runs first");
    CHECK(queue_try_pop(queue) == low, "remaining queue order is preserved");
    CHECK(queue_try_pop(queue) == NULL, "empty queue is reported");
    queue_shutdown(queue);
    CHECK(queue_push(queue, middle) != 0, "shutdown queue rejects new jobs");
    queue_destroy(queue);
    free(low);
    free(high);
    free(middle);
}

static void test_events_metadata(void)
{
    EventMessage messages[2];
    events_init();
    events_push_user("{\"type\":\"job_status\"}", "alice");
    CHECK(events_drain(messages, 2) == 1, "event ring drains queued events");
    CHECK(strcmp(messages[0].user_id, "alice") == 0,
          "event ownership metadata is preserved");
}

static void test_database_recovery(void)
{
    CHECK(db_open(":memory:") == 0, "in-memory database opens");

    Job rollback_job = {0};
    strncpy(rollback_job.id, "rollback-job", sizeof(rollback_job.id) - 1);
    strncpy(rollback_job.command, "test", sizeof(rollback_job.command) - 1);
    rollback_job.status = JOB_STATUS_QUEUED;
    rollback_job.req_cores = 1;
    rollback_job.submitted_at = 1;
    CHECK(db_begin() == 0, "database transaction begins");
    CHECK(db_insert_job(&rollback_job) == 0, "job inserts inside transaction");
    CHECK(db_rollback() == 0, "database transaction rolls back");
    Job *missing = db_get_job(rollback_job.id);
    CHECK(missing == NULL, "rolled-back job is absent");
    job_free(missing);

    Job interrupted = {0};
    strncpy(interrupted.id, "interrupted-job", sizeof(interrupted.id) - 1);
    strncpy(interrupted.command, "test", sizeof(interrupted.command) - 1);
    interrupted.status = JOB_STATUS_RUNNING;
    interrupted.req_cores = 2;
    interrupted.submitted_at = 1;
    CHECK(db_insert_job(&interrupted) == 0, "running job persists");
    CHECK(db_insert_allocation(interrupted.id, "machine-1", 2, 0, 128, 0) == 0,
          "running allocation persists");
    CHECK(db_recover_after_restart() == 1, "restart recovery reports one interrupted job");
    Job *recovered = db_get_job(interrupted.id);
    CHECK(recovered && recovered->status == JOB_STATUS_FAILED,
          "interrupted running job becomes failed");
    CHECK(recovered && strstr(recovered->status_reason, "restart") != NULL,
          "recovered job records a restart reason");
    job_free(recovered);

    CHECK(db_store_submission_key("tester", "request-1", interrupted.id) == 0,
          "idempotency key persists");
    char idempotent_job_id[JOB_ID_LEN];
    CHECK(db_get_submission_job("tester", "request-1", idempotent_job_id,
                                sizeof(idempotent_job_id)) == 1 &&
          strcmp(idempotent_job_id, interrupted.id) == 0,
          "idempotency key resolves the original job");

    Job *failed_jobs = NULL;
    int failed_count = db_query_jobs("", JOB_STATUS_FAILED, "", 10, 0, &failed_jobs);
    CHECK(failed_count == 1 && failed_jobs &&
          strcmp(failed_jobs[0].id, interrupted.id) == 0,
          "job query filters by status with pagination");
    free(failed_jobs);
    db_close();
}

static void test_machine_registry(void)
{
    const char *path = "unit_registry_test.json";
    FILE *file = fopen(path, "wb");
    CHECK(file != NULL, "registry fixture can be created");
    if (!file) return;
    const char *json =
        "{\r\n"
        "  \"machines\": [{\r\n"
        "    \"id\": \"test-machine\",\r\n"
        "    \"hostname\": \"localhost\",\r\n"
        "    \"enabled\": true,\r\n"
        "    \"cores\": 4,\r\n"
        "    \"gpu_count\": 0,\r\n"
        "    \"ram_mb\": 4096,\r\n"
        "    \"disk_mb\": 10000,\r\n"
        "    \"type\": \"static\"\r\n"
        "  }]\r\n"
        "}\r\n";
    fwrite(json, 1, strlen(json), file);
    fclose(file);

    CHECK(registry_load(path) == 1, "machine registry loads a fixture");
    remove(path);
    Machine *snapshot = NULL;
    int count = registry_snapshot(&snapshot);
    CHECK(count == 1 && snapshot && strcmp(snapshot[0].id, "test-machine") == 0,
          "machine snapshots are consistent copies");
    free(snapshot);

    CHECK(registry_update_probe("test-machine", MACHINE_ONLINE, 1, 1) == 0,
          "probe state updates atomically");
    CHECK(registry_reserve("test-machine", 2, 0, 1024, 0) == 0,
          "resources reserve atomically");
    Machine machine;
    CHECK(registry_get_copy("test-machine", &machine) == 0 &&
          machine.cores_reserved == 2 && machine.ram_mb_reserved == 1024,
          "reserved resources are visible in copied state");

    Machine updated = machine;
    updated.cores_total = 8;
    updated.cores_reserved = 0;
    updated.ram_mb_reserved = 0;
    updated.probe_status = MACHINE_OFFLINE;
    CHECK(registry_upsert(&updated) == 0,
          "machine metadata can be updated");
    CHECK(registry_get_copy("test-machine", &machine) == 0 &&
          machine.cores_total == 8 && machine.cores_reserved == 2 &&
          machine.probe_status == MACHINE_ONLINE,
          "runtime reservation and probe state survive metadata updates");
    CHECK(registry_remove("test-machine") == -2,
          "reserved machines cannot be removed");
    CHECK(registry_release("test-machine", 2, 0, 1024, 0) == 0 &&
          registry_remove("test-machine") == 0,
          "idle machines can be released and removed");
}

int main(void)
{
    test_secure_defaults();
    test_repository_configuration();
    test_password_hashing();
    test_filename_validation();
    test_queue_order_and_removal();
    test_events_metadata();
    test_database_recovery();
    test_machine_registry();
    if (failures) {
        fprintf(stderr, "%d unit test(s) failed\n", failures);
        return 1;
    }
    printf("All unit tests passed\n");
    return 0;
}
