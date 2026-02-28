#ifndef DB_H
#define DB_H

#include "job.h"
#include "resources.h"

int db_open(const char *path);
void db_close(void);

/* ── Jobs ──────────────────────────────────────── */
int db_insert_job(const Job *job);
int db_update_job_status(const char *job_id, JobStatus status,
                         int exit_code, time_t ended_at);
int db_update_job_started(const char *job_id, const char *machine_id,
                           time_t started_at);
Job *db_get_job(const char *job_id);
int  db_list_jobs(Job *jobs, int max_count);

int  db_insert_api_key(const char *key_hash, const char *label);
int  db_validate_api_key(const char *key_hash);
int  db_revoke_api_key(const char *key_hash);


typedef struct {
    int total;
    int in_queue;
    int starting;
    int running;
    int finished;
    int cancelled;
    int failed;
} JobStats;

int db_job_stats(JobStats *out);
int db_purge_jobs(void);

int db_insert_allocation(const char *job_id, const char *machine_id,
                          int cores, int gpu, int ram_mb, int disk_mb);
int db_release_allocation(const char *job_id);

int db_audit(const char *event, const char *detail);

#endif /* DB_H */
