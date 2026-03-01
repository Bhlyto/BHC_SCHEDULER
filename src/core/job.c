#include "job.h"
#include "db.h"
#include "log.h"
#include "events.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdio.h>

#ifdef _WIN32
#  include <rpc.h>
   static void gen_uuid(char *out) {
       UUID uuid; UuidCreate(&uuid);
       unsigned char *str;
       UuidToStringA(&uuid, &str);
       strncpy(out, (char *)str, JOB_ID_LEN - 1);
       RpcStringFreeA(&str);
   }
#else
#  include <fcntl.h>
#  include <unistd.h>
   static void gen_uuid(char *out) {

       unsigned char b[16];
       int fd = open("/dev/urandom", O_RDONLY);
       if (fd < 0 || read(fd, b, 16) != 16) {
           snprintf(out, JOB_ID_LEN, "%lx-%lx", (long)time(NULL), (long)rand());
           if (fd >= 0) close(fd);
           return;
       }
       close(fd);
       b[6] = (b[6] & 0x0f) | 0x40;   /* version 4 */
       b[8] = (b[8] & 0x3f) | 0x80;   /* variant */
       snprintf(out, JOB_ID_LEN,
           "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
           b[0],b[1],b[2],b[3], b[4],b[5], b[6],b[7],
           b[8],b[9], b[10],b[11],b[12],b[13],b[14],b[15]);
   }
#endif

static int transition_allowed(JobStatus from, JobStatus to)
{
    switch (from) {
        case JOB_STATUS_HELD:      return to == JOB_STATUS_IN_QUEUE   || to == JOB_STATUS_CANCELLED;
        case JOB_STATUS_IN_QUEUE:  return to == JOB_STATUS_STARTING  || to == JOB_STATUS_CANCELLED;
        case JOB_STATUS_STARTING:  return to == JOB_STATUS_RUNNING   || to == JOB_STATUS_FAILED || to == JOB_STATUS_CANCELLED;
        case JOB_STATUS_RUNNING:   return to == JOB_STATUS_FINISHED  || to == JOB_STATUS_FAILED;
        default:                   return 0; /* terminal states */
    }
}

const char *job_status_str(JobStatus s)
{
    switch (s) {
        case JOB_STATUS_HELD:      return "HELD";
        case JOB_STATUS_IN_QUEUE:  return "IN_QUEUE";
        case JOB_STATUS_STARTING:  return "STARTING";
        case JOB_STATUS_RUNNING:   return "RUNNING";
        case JOB_STATUS_FINISHED:  return "FINISHED";
        case JOB_STATUS_CANCELLED: return "CANCELLED";
        case JOB_STATUS_FAILED:    return "FAILED";
        default:                   return "UNKNOWN";
    }
}

Job *job_create(const char *command, int priority,
                int req_cores, int req_gpu,
                int req_ram_mb, int req_disk_mb)
{
    Job *job = (Job *)calloc(1, sizeof(Job));
    if (!job) return NULL;

    gen_uuid(job->id);
    strncpy(job->command, command, sizeof(job->command) - 1);
    job->status      = JOB_STATUS_IN_QUEUE;
    job->priority    = priority;
    job->req_cores   = req_cores  > 0 ? req_cores  : 1;
    job->req_gpu     = req_gpu    > 0 ? req_gpu     : 0;
    job->req_ram_mb  = req_ram_mb > 0 ? req_ram_mb  : 0;
    job->req_disk_mb = req_disk_mb> 0 ? req_disk_mb : 0;
    job->submitted_at = time(NULL);

    db_insert_job(job);
    log_info("job", "Created job %s: %s", job->id, job->command);
    return job;
}

void job_free(Job *job)
{
    free(job);
}

int job_set_status(Job *job, JobStatus new_status)
{
    return job_set_status_r(job, new_status, "");
}

int job_set_status_r(Job *job, JobStatus new_status, const char *reason)
{
    if (!transition_allowed(job->status, new_status)) {
        log_warn("job", "Invalid transition %s -> %s for job %s",
                 job_status_str(job->status), job_status_str(new_status), job->id);
        return -1;
    }

    if (reason && reason[0])
        log_info("job", "Job %s: %s -> %s — %s",
                 job->id, job_status_str(job->status),
                 job_status_str(new_status), reason);
    else
        log_info("job", "Job %s: %s -> %s",
                 job->id, job_status_str(job->status), job_status_str(new_status));

    job->status = new_status;
    if (reason) strncpy(job->status_reason, reason, sizeof(job->status_reason) - 1);

    time_t now = time(NULL);
    switch (new_status) {
        case JOB_STATUS_FINISHED:
        case JOB_STATUS_CANCELLED:
        case JOB_STATUS_FAILED:
            job->ended_at = now;
            db_update_job_status(job->id, new_status, job->exit_code, now);
            break;
        default:
            db_update_job_status(job->id, new_status, 0, 0);
            break;
    }

    if (reason && reason[0])
        db_update_status_reason(job->id, reason);

    char evt[EVENTS_JSON_MAX];
    snprintf(evt, sizeof(evt),
        "{\"event\":\"job_status\",\"id\":\"%s\",\"status\":\"%s\","
        "\"machine_id\":\"%s\",\"reason\":\"%s\"}",
        job->id, job_status_str(new_status), job->machine_id,
        reason ? reason : "");
    events_push(evt);

    return 0;
}
