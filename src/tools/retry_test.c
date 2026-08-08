#include "config.h"
#include "db.h"
#include "events.h"
#include "job.h"
#include "transfer.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#  define TEST_SEP "\\"
#else
#  define TEST_SEP "/"
#endif

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

static int exists(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (!file) return 0;
    fclose(file);
    return 1;
}

static int write_file(const char *path, const char *text)
{
    FILE *file = fopen(path, "wb");
    if (!file) return -1;
    size_t length = strlen(text);
    int result = fwrite(text, 1, length, file) == length ? 0 : -1;
    fclose(file);
    return result;
}

static void cleanup(const char *job_id)
{
    if (job_id && job_id[0]) store_cleanup_job(job_id);
    remove("retry_test.db");
    remove("retry_test.db-wal");
    remove("retry_test.db-shm");
}

int main(void)
{
    char job_id[JOB_ID_LEN] = {0};
    config_defaults();
    strcpy(g_config.work_dir, "retry_test_work");
    cleanup(NULL);
    events_init();
    if (db_open("retry_test.db") != 0) return 1;

    Job *job = job_create("retry-me", 50, 1, 0, 64, 64);
    if (!job) return 1;
    strncpy(job_id, job->id, sizeof(job_id) - 1);
    if (job_set_status(job, JOB_STATUS_RUNNING) != 0 ||
        job_set_status_r(job, JOB_STATUS_FAILED, "first attempt failed") != 0)
        return 1;
    job_free(job);

    store_init_job_dirs(job_id);
    char input_dir[1024], output_dir[1024], input_path[1024], output_path[1024];
    char stdout_path[1024];
    store_input_dir(job_id, input_dir, sizeof(input_dir));
    store_output_dir(job_id, output_dir, sizeof(output_dir));
    store_stdout_path(job_id, stdout_path, sizeof(stdout_path));
    snprintf(input_path, sizeof(input_path), "%s%sinput.dat", input_dir, TEST_SEP);
    snprintf(output_path, sizeof(output_path), "%s%sresult.dat", output_dir, TEST_SEP);
    if (write_file(input_path, "input") != 0 ||
        write_file(output_path, "stale output") != 0 ||
        write_file(stdout_path, "stale log") != 0 ||
        artifact_collect_job(job_id) != 2)
        return 1;

    if (db_retry_job(job_id) != 0 || store_reset_job_outputs(job_id) != 0)
        return 1;
    ArtifactRecord artifacts[4];
    Job *retried = db_get_job(job_id);
    int artifact_count = db_list_artifacts(job_id, artifacts, 4);
    int ok = retried && retried->status == JOB_STATUS_QUEUED &&
             retried->started_at == 0 && retried->ended_at == 0 &&
             artifact_count == 0 && exists(input_path) &&
             !exists(output_path) && !exists(stdout_path);
    job_free(retried);
    db_close();
    cleanup(job_id);
    return ok ? 0 : 1;
}
