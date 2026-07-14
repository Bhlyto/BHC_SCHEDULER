#ifndef SCHEDULER_H
#define SCHEDULER_H

#include "queue.h"

Queue *scheduler_init(void);
void   scheduler_start(void);
void   scheduler_stop(void);
int    scheduler_cancel_job(const char *job_id, const char *reason);
Queue *scheduler_queue(void);

#endif /* SCHEDULER_H */
