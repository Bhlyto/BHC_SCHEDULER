#include "job.h"
#include "queue.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static Job *make_job(const char *id, int priority, time_t submitted_at,
                     int req_cores)
{
    Job *job = (Job *)calloc(1, sizeof(Job));
    if (!job) return NULL;
    strncpy(job->id, id, sizeof(job->id) - 1);
    job->priority = priority;
    job->submitted_at = submitted_at;
    job->req_cores = req_cores;
    return job;
}

static int fits_one_core(const Job *job, void *context)
{
    (void)context;
    return job->req_cores <= 1;
}

int main(void)
{
    Queue *queue = queue_create(8);
    if (!queue) return 1;

    queue_push(queue, make_job("job-c", 50, 20, 1));
    queue_push(queue, make_job("job-b", 50, 10, 1));
    queue_push(queue, make_job("job-a", 50, 10, 1));

    const char *expected[] = {"job-a", "job-b", "job-c"};
    for (int i = 0; i < 3; i++) {
        Job *job = queue_try_pop(queue);
        if (!job || strcmp(job->id, expected[i]) != 0) {
            fprintf(stderr, "non-deterministic queue order at index %d\n", i);
            return 1;
        }
        free(job);
    }

    queue_push(queue, make_job("blocked-priority", 1, 1, 8));
    queue_push(queue, make_job("runnable-later", 50, 2, 1));
    Job *selected = queue_try_pop_matching(queue, fits_one_core, NULL);
    if (!selected || strcmp(selected->id, "runnable-later") != 0) {
        fprintf(stderr, "runnable job was starved by blocked priority job\n");
        return 1;
    }
    free(selected);
    selected = queue_try_pop(queue);
    if (!selected || strcmp(selected->id, "blocked-priority") != 0) return 1;
    free(selected);

    queue_push(queue, make_job("keep", 50, 4, 1));
    queue_push(queue, make_job("remove", 10, 3, 1));
    Job *removed = queue_remove(queue, "remove");
    if (!removed || strcmp(removed->id, "remove") != 0 || queue_size(queue) != 1)
        return 1;
    free(removed);
    selected = queue_try_pop(queue);
    if (!selected || strcmp(selected->id, "keep") != 0) return 1;
    free(selected);

    queue_destroy(queue);
    return 0;
}
