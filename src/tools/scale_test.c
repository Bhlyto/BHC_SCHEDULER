#include "config.h"
#include "db.h"
#include "events.h"
#include "executor.h"
#include "job.h"
#include "resources.h"
#include "scheduler.h"
#include "transfer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#  include <windows.h>
#  define SCALE_COMMAND "cmd.exe /C exit 0"
#else
#  include <unistd.h>
#  define SCALE_COMMAND "/bin/true"
#endif

#define SCALE_JOB_COUNT 1000

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

static void sleep_ms(int milliseconds)
{
#ifdef _WIN32
    Sleep((DWORD)milliseconds);
#else
    usleep((useconds_t)milliseconds * 1000);
#endif
}

static void remove_db(void)
{
    remove("scale_test.db");
    remove("scale_test.db-wal");
    remove("scale_test.db-shm");
    remove("scale_workers.json");
}

int main(void)
{
    remove_db();
    FILE *workers = fopen("scale_workers.json", "wb");
    if (!workers) return 1;
    fputs("{\"machines\":[{\"id\":\"local-scale\","
          "\"hostname\":\"localhost\",\"enabled\":true,"
          "\"cores\":64,\"ram_mb\":65536,\"disk_mb\":1048576}]}", workers);
    fclose(workers);

    config_defaults();
    strcpy(g_config.work_dir, "scale_test_work");
    g_config.scheduler_poll_ms = 1;
    g_config.cleanup_ttl_seconds = 0;
    events_init();
    if (db_open("scale_test.db") != 0 ||
        registry_load("scale_workers.json") != 1)
        return 1;

    BatchRecord batch;
    memset(&batch, 0, sizeof(batch));
    strcpy(batch.id, "scale-1000");
    strcpy(batch.name, "ShardSim v1 scale proof");
    strcpy(batch.user_id, "scale-test");
    batch.created_at = time(NULL);

    char first_job_id[JOB_ID_LEN] = {0};
    if (db_begin() != 0 || db_insert_batch(&batch) != 0) return 1;
    for (int i = 0; i < SCALE_JOB_COUNT; i++) {
        Job *job = job_create_ex(SCALE_COMMAND, 50, 1, 0, 16, 1,
                                 "scale-test", "shardsim");
        if (!job) { db_rollback(); return 1; }
        strncpy(job->batch_id, batch.id, sizeof(job->batch_id) - 1);
        if (i == 0) strncpy(first_job_id, job->id, sizeof(first_job_id) - 1);
        if (db_update_job_batch_id(job->id, batch.id) != 0) {
            job_free(job);
            db_rollback();
            return 1;
        }
        job_free(job);
    }
    if (db_commit() != 0) return 1;

    if (!scheduler_init()) return 1;
    scheduler_start();
    BatchStats stats;
    memset(&stats, 0, sizeof(stats));
    int completed = 0;
    for (int waited_ms = 0; waited_ms < 120000; waited_ms += 50) {
        if (db_get_batch_stats(batch.id, &stats) == 0 &&
            stats.succeeded + stats.failed + stats.cancelled == SCALE_JOB_COUNT) {
            completed = 1;
            break;
        }
        sleep_ms(50);
    }

    scheduler_stop();
    if (executor_shutdown() != 0) completed = 0;

    ArtifactRecord artifacts[4];
    int artifact_count = db_list_artifacts(first_job_id, artifacts, 4);
    if (!completed || stats.total != SCALE_JOB_COUNT ||
        stats.succeeded != SCALE_JOB_COUNT || stats.failed != 0 ||
        artifact_count != 2) {
        fprintf(stderr,
                "scale proof failed: total=%d queued=%d running=%d "
                "succeeded=%d failed=%d artifacts=%d\n",
                stats.total, stats.queued, stats.running,
                stats.succeeded, stats.failed, artifact_count);
        db_close();
        return 1;
    }

    Job *jobs = calloc(SCALE_JOB_COUNT, sizeof(Job));
    if (jobs) {
        int count = db_list_jobs(jobs, SCALE_JOB_COUNT);
        for (int i = 0; i < count; i++) store_cleanup_job(jobs[i].id);
        free(jobs);
    }
    db_close();
    remove_db();
    return 0;
}
