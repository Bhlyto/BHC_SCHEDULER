#ifndef JOB_H
#define JOB_H

#include <time.h>

#define JOB_ID_LEN   37
#define JOB_CMD_LEN  512
#define JOB_USER_LEN 128
#define JOB_APP_LEN  128

typedef enum {
    JOB_STATUS_IN_QUEUE  = 0,
    JOB_STATUS_STARTING  = 1,
    JOB_STATUS_RUNNING   = 2,
    JOB_STATUS_FINISHED  = 3,
    JOB_STATUS_CANCELLED = 4,
    JOB_STATUS_FAILED    = 5,
    JOB_STATUS_HELD      = 6
} JobStatus;

typedef struct {
    char       id[JOB_ID_LEN];
    char       command[JOB_CMD_LEN];
    char       user_id[JOB_USER_LEN];
    char       app_id[JOB_APP_LEN];
    JobStatus  status;
    int        priority;

    int        req_cores;
    int        req_gpu;
    int        req_ram_mb;
    int        req_disk_mb;

    char       machine_id[1024]; /* comma-separated; multi-machine jobs */
    int        n_machines;

    char       input_dir[512];
    char       output_dir[512];
    char       input_files[2048]; /* comma-separated expected filenames, empty = no hold */

    time_t     submitted_at;
    time_t     started_at;
    time_t     ended_at;

    int        exit_code;
    int        timeout_seconds; /* 0 = no timeout */
    char       status_reason[256]; /* human-readable cause of the last status transition */
    char       depends_on[2048]; /* comma-separated job IDs this job depends on */
    char       workflow_id[64];  /* groups jobs submitted together via a workflow */
    char       same_machine_as[JOB_ID_LEN]; /* job ID whose machine this job must reuse */
} Job;

Job *job_create(const char *command, int priority,
                int req_cores, int req_gpu,
                int req_ram_mb, int req_disk_mb);
Job *job_create_ex(const char *command, int priority,
                   int req_cores, int req_gpu,
                   int req_ram_mb, int req_disk_mb,
                   const char *user_id, const char *app_id);
void job_free(Job *job);
int  job_set_status(Job *job, JobStatus new_status);
int  job_set_status_r(Job *job, JobStatus new_status, const char *reason);
const char *job_status_str(JobStatus s);

#endif /* JOB_H */
