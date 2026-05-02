#ifndef SCHEDULER_H
#define SCHEDULER_H

#include "queue.h"

Queue *scheduler_init(void);
void   scheduler_start(void);
void   scheduler_stop(void);
Queue *scheduler_queue(void);

#endif /* SCHEDULER_H */
