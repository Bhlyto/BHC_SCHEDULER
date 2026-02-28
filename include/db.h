#ifndef DB_H
#define DB_H

/*
 * db.h
 * SQLite persistence layer.
 */

#include "job.h"
#include "resources.h"

/* Open (or create) the database at the given path.
   Creates all tables if they do not exist.
   Returns 0 on success, -1 on error. */
int db_open(const char *path);
void db_close(void);

/* ── Jobs ──────────────────────────────────────── */
int db_insert_job(const Job *job);
int db_update_job_status(const char *job_id, JobStatus status,
                         int exit_code, time_t ended_at);
int db_update_job_started(const char *job_id, const char *machine_id,
                           time_t started_at);
/* Returns a heap-allocated Job (caller must call job_free()), or NULL. */
Job *db_get_job(const char *job_id);
/* Fills jobs[] array (max_count entries). Returns count written. */
int  db_list_jobs(Job *jobs, int max_count);

/* ── API keys ──────────────────────────────────── */
/* Inserts a hashed key (SHA-256 hex). Returns 0 ok. */
int  db_insert_api_key(const char *key_hash, const char *label);
/* Returns 1 if the hash matches an active key, 0 otherwise. */
int  db_validate_api_key(const char *key_hash);
int  db_revoke_api_key(const char *key_hash);

/* ── Stats ─────────────────────────────────────── */

typedef struct {
    int total;
    int in_queue;
    int starting;
    int running;
    int finished;
    int cancelled;
    int failed;
} JobStats;

/* Count jobs by status. Returns 0 on success. */
int db_job_stats(JobStats *out);

/* ── Allocations ───────────────────────────────── */
int db_insert_allocation(const char *job_id, const char *machine_id,
                          int cores, int gpu, int ram_mb, int disk_mb);
int db_release_allocation(const char *job_id);

/* ── Audit log ─────────────────────────────────── */
int db_audit(const char *event, const char *detail);

#endif /* DB_H */
