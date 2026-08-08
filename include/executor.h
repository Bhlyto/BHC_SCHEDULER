#ifndef EXECUTOR_H
#define EXECUTOR_H

#include "job.h"

int executor_spawn(Job *job);

/* Terminate the active process tree for job_id. Returns 1 when an active
   process was found, 0 when the job has not started or already exited. */
int executor_terminate(const char *job_id);

#endif /* EXECUTOR_H */
