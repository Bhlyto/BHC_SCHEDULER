#include "config.h"
#include "db.h"
#include "events.h"
#include "job.h"
#include "queue.h"
#include "scheduler.h"

#include <stdio.h>
#include <string.h>

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
    remove("recovery_test.db");
    remove("recovery_test.db-wal");
    remove("recovery_test.db-shm");
}

int main(void)
{
    cleanup();
    config_defaults();
    events_init();
    if (db_open("recovery_test.db") != 0) return 1;

    Job *queued = job_create("queued", 50, 1, 0, 0, 0);
    Job *starting = job_create("starting", 50, 1, 0, 0, 0);
    Job *running = job_create("running", 50, 1, 0, 0, 0);
    if (!queued || !starting || !running) return 1;

    char queued_id[JOB_ID_LEN], starting_id[JOB_ID_LEN], running_id[JOB_ID_LEN];
    strncpy(queued_id, queued->id, sizeof(queued_id));
    strncpy(starting_id, starting->id, sizeof(starting_id));
    strncpy(running_id, running->id, sizeof(running_id));
    db_update_job_status(starting->id, (JobStatus)1, 0, 0);
    job_set_status(running, JOB_STATUS_RUNNING);
    job_free(queued);
    job_free(starting);
    job_free(running);
    db_close();

    if (db_open("recovery_test.db") != 0) return 1;
    Queue *queue = scheduler_init();
    if (!queue || queue_size(queue) != 1) {
        fprintf(stderr, "queued jobs were not recovered\n");
        return 1;
    }

    Job *q = db_get_job(queued_id);
    Job *s = db_get_job(starting_id);
    Job *r = db_get_job(running_id);
    int ok = q && q->status == JOB_STATUS_QUEUED &&
             s && s->status == JOB_STATUS_FAILED &&
             r && r->status == JOB_STATUS_FAILED &&
             strstr(s->status_reason, "restarted") != NULL &&
             strstr(r->status_reason, "restarted") != NULL;
    job_free(q);
    job_free(s);
    job_free(r);

    scheduler_stop();
    db_close();
    cleanup();
    return ok ? 0 : 1;
}
