#ifndef JOB_H
#define JOB_H

#include <time.h>

/*
 * job.h
 * Job struct and state machine.
 */

#define JOB_ID_LEN   37   /* UUID v4 string + null terminator */
#define JOB_CMD_LEN  512

typedef enum {
    JOB_STATUS_IN_QUEUE  = 0,
    JOB_STATUS_STARTING  = 1,
    JOB_STATUS_RUNNING   = 2,
    JOB_STATUS_FINISHED  = 3,
    JOB_STATUS_CANCELLED = 4,
    JOB_STATUS_FAILED    = 5
} JobStatus;

typedef struct {
    char       id[JOB_ID_LEN];
    char       command[JOB_CMD_LEN];
    JobStatus  status;
    int        priority;        /* lower number = higher priority */

    /* Resource requirements */
    int        req_cores;
    int        req_gpu;
    int        req_ram_mb;
    int        req_disk_mb;

    /* Assigned machine */
    char       machine_id[64];

    /* File transfer paths (set by transfer layer before executor launch) */
    char       input_dir[512];
    char       output_dir[512];

    /* Timing */
    time_t     submitted_at;
    time_t     started_at;
    time_t     ended_at;

    /* Exit code from the spawned process */
    int        exit_code;
} Job;

/* Allocate and initialise a new Job. Caller must free with job_free(). */
Job *job_create(const char *command, int priority,
                int req_cores, int req_gpu,
                int req_ram_mb, int req_disk_mb);

void job_free(Job *job);

/* Transition to new_status. Returns 0 on success, -1 if the transition is
   not allowed by the state machine rules. Persists to DB automatically. */
int job_set_status(Job *job, JobStatus new_status);

/* Human-readable status string. */
const char *job_status_str(JobStatus s);

#endif /* JOB_H */
