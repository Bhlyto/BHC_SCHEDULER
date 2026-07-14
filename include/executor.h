#ifndef EXECUTOR_H
#define EXECUTOR_H

#include "job.h"

int executor_spawn(Job *job);
int executor_cancel(const char *job_id, const char *reason);
int executor_timeout(const char *job_id, const char *reason);
void executor_shutdown(void);

#endif /* EXECUTOR_H */
