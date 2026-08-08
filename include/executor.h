#ifndef EXECUTOR_H
#define EXECUTOR_H

#include "job.h"

int executor_spawn(Job *job);

/* Terminate the active process tree for job_id. Returns 1 when an active
   process was found, 0 when the job has not started or already exited. */
int executor_terminate(const char *job_id);
int executor_is_active(const char *job_id);

/* Stop accepting launches, terminate active process trees and wait for all
   launcher/watcher threads to finish. Returns 0 on clean shutdown. */
int executor_shutdown(void);

#endif /* EXECUTOR_H */
