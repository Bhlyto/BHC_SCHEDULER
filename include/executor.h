#ifndef EXECUTOR_H
#define EXECUTOR_H

#include "job.h"

/* Spawn a child process for the given job.
   Sets status to STARTING, then RUNNING on success or FAILED on error.
   Releases resources and updates status to FINISHED/FAILED when the process exits.
   Returns 0 if the process was launched, -1 on immediate error. */
int executor_spawn(Job *job);

#endif /* EXECUTOR_H */
