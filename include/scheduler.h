#ifndef SCHEDULER_H
#define SCHEDULER_H

#include "queue.h"

/* Initialise the internal queue. Returns the queue pointer. */
Queue *scheduler_init(void);

/* Start the scheduler loop in a background thread. */
void scheduler_start(void);

/* Signal the scheduler thread to stop. */
void scheduler_stop(void);

/* Access the queue to push jobs onto it. */
Queue *scheduler_queue(void);

#endif /* SCHEDULER_H */
