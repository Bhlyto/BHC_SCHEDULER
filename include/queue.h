#ifndef QUEUE_H
#define QUEUE_H

/*
 * queue.h
 * Thread-safe priority queue for Job pointers.
 */

#include "job.h"

typedef struct Queue Queue;

/* Create a new queue with the given initial capacity. */
Queue *queue_create(int initial_capacity);
void   queue_destroy(Queue *q);

/* Push a job. Thread-safe. */
int  queue_push(Queue *q, Job *job);

/* Pop the highest-priority job (lowest priority number).
   Blocks until a job is available or queue_shutdown() is called.
   Returns NULL on shutdown. */
Job *queue_pop(Queue *q);

/* Non-blocking pop. Returns NULL if queue is empty. */
Job *queue_try_pop(Queue *q);

/* Current number of jobs waiting. */
int  queue_size(Queue *q);

/* Wake any blocked queue_pop() calls so threads can exit. */
void queue_shutdown(Queue *q);

#endif /* QUEUE_H */
