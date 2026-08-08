#ifndef QUEUE_H
#define QUEUE_H

#include "job.h"

typedef struct Queue Queue;
typedef int (*QueuePredicate)(const Job *job, void *context);

Queue *queue_create(int initial_capacity);
void   queue_destroy(Queue *q);
int    queue_push(Queue *q, Job *job);
Job   *queue_pop(Queue *q);
Job   *queue_try_pop(Queue *q);
Job   *queue_try_pop_matching(Queue *q, QueuePredicate predicate, void *context);
Job   *queue_remove(Queue *q, const char *job_id);
Job   *queue_remove_by_id(Queue *q, const char *job_id);
int    queue_size(Queue *q);
void   queue_shutdown(Queue *q);

#endif /* QUEUE_H */
