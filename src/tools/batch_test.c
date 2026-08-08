#include "config.h"
#include "db.h"
#include "events.h"
#include "job.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

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

static void cleanup(void)
{
    remove("batch_test.db");
    remove("batch_test.db-wal");
    remove("batch_test.db-shm");
}

int main(void)
{
    cleanup();
    config_defaults();
    events_init();
    if (db_open("batch_test.db") != 0) return 1;

    BatchRecord batch;
    memset(&batch, 0, sizeof(batch));
    strcpy(batch.id, "batch-1000");
    strcpy(batch.name, "ShardSim scale test");
    strcpy(batch.user_id, "tester");
    batch.created_at = time(NULL);

    if (db_begin() != 0 || db_insert_batch(&batch) != 0) return 1;
    for (int i = 0; i < 1000; i++) {
        Job *job = job_create_ex("echo shardsim", 50, 1, 0, 64, 64,
                                 "tester", "shardsim");
        if (!job) return 1;
        strncpy(job->batch_id, batch.id, sizeof(job->batch_id) - 1);
        if (db_update_job_batch_id(job->id, batch.id) != 0) return 1;
        if (i == 0) {
            job_set_status(job, JOB_STATUS_STARTING);
            job_set_status(job, JOB_STATUS_RUNNING);
            job_set_status(job, JOB_STATUS_FINISHED);
        }
        job_free(job);
    }
    if (db_commit() != 0) return 1;

    BatchStats stats;
    if (db_get_batch_stats(batch.id, &stats) != 0 ||
        stats.total != 1000 || stats.queued != 999 || stats.succeeded != 1) {
        fprintf(stderr, "incorrect batch aggregation\n");
        return 1;
    }

    BatchRecord loaded;
    if (db_get_batch(batch.id, &loaded) != 0 ||
        strcmp(loaded.name, batch.name) != 0) return 1;

    BatchRecord rolled_back = batch;
    strcpy(rolled_back.id, "rolled-back");
    if (db_begin() != 0 || db_insert_batch(&rolled_back) != 0) return 1;
    db_rollback();
    if (db_get_batch(rolled_back.id, &loaded) == 0) {
        fprintf(stderr, "rolled-back batch was persisted\n");
        return 1;
    }

    db_close();
    cleanup();
    return 0;
}
