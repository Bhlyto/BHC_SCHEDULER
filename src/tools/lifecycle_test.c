#include "config.h"
#include "db.h"
#include "executor.h"
#include "events.h"
#include "job.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  include <windows.h>
#  define sleep_ms(ms) Sleep(ms)
#  define MARKER_PATH "lifecycle_marker.txt"
#  define LONG_COMMAND "ping -n 4 127.0.0.1 > nul && echo leaked > lifecycle_marker.txt"
#else
#  include <unistd.h>
#  define sleep_ms(ms) usleep((ms) * 1000)
#  define MARKER_PATH "lifecycle_marker.txt"
#  define LONG_COMMAND "sh -c 'sleep 3; echo leaked > lifecycle_marker.txt'"
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

static void remove_test_files(void)
{
    remove("lifecycle_test.db");
    remove("lifecycle_test.db-wal");
    remove("lifecycle_test.db-shm");
    remove(MARKER_PATH);
}

int main(void)
{
    remove_test_files();
    config_defaults();
    events_init();
    strncpy(g_config.db_path, "lifecycle_test.db", sizeof(g_config.db_path) - 1);
    strncpy(g_config.work_dir, "lifecycle_test_jobs", sizeof(g_config.work_dir) - 1);
    g_config.pre_job_script_win[0] = '\0';
    g_config.pre_job_script_linux[0] = '\0';
    g_config.ssh_user[0] = '\0';

    if (db_open(g_config.db_path) != 0) {
        fprintf(stderr, "db_open failed\n");
        return 1;
    }

    Job *job = job_create(LONG_COMMAND, 50, 1, 0, 0, 0);
    if (!job) {
        fprintf(stderr, "job_create failed\n");
        db_close();
        return 1;
    }
    char job_id[JOB_ID_LEN];
    strncpy(job_id, job->id, sizeof(job_id) - 1);
    job_id[sizeof(job_id) - 1] = '\0';
    if (executor_spawn(job) != 0) {
        fprintf(stderr, "executor_spawn failed\n");
        db_close();
        return 1;
    }

    Job *persisted = NULL;
    for (int i = 0; i < 100; i++) {
        persisted = db_get_job(job_id);
        if (persisted && persisted->status == JOB_STATUS_RUNNING) break;
        job_free(persisted);
        persisted = NULL;
        sleep_ms(20);
    }
    if (!persisted) {
        fprintf(stderr, "job never reached RUNNING\n");
        db_close();
        return 1;
    }

    if (job_set_status_r(persisted, JOB_STATUS_CANCELLED, "Lifecycle test") != 0) {
        fprintf(stderr, "RUNNING -> CANCELLED transition failed\n");
        job_free(persisted);
        db_close();
        return 1;
    }
    job_free(persisted);

    if (!executor_terminate(job_id)) {
        fprintf(stderr, "active process was not found\n");
        db_close();
        return 1;
    }

    for (int i = 0; i < 100 && executor_terminate(job_id); i++)
        sleep_ms(20);

    sleep_ms(3500);
    persisted = db_get_job(job_id);
    int ok = persisted && persisted->status == JOB_STATUS_CANCELLED;
    job_free(persisted);

    FILE *marker = fopen(MARKER_PATH, "rb");
    if (marker) {
        fclose(marker);
        ok = 0;
        fprintf(stderr, "cancelled process produced its marker\n");
    }

    db_close();
    remove_test_files();
    return ok ? 0 : 1;
}
