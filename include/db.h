#ifndef DB_H
#define DB_H

#include "job.h"
#include "resources.h"

#define QUOTA_ID_LEN 128

typedef struct {
    char user_id[QUOTA_ID_LEN];   /* "" means wildcard / any user   */
    char app_id[QUOTA_ID_LEN];    /* "" means wildcard / any app    */
    int  max_jobs;                /* total job count limit (0=unlimited)     */
    int  max_ram_mb;              /* total RAM across active jobs (0=unlim)  */
    int  max_cores;               /* total cores across active jobs (0=unlim)*/
    int  max_concurrent;          /* max running/starting jobs (0=unlim)     */
} Quota;

int db_open(const char *path);
void db_close(void);

/* ── Jobs ──────────────────────────────────────── */
int db_insert_job(const Job *job);
int db_update_job_status(const char *job_id, JobStatus status,
                         int exit_code, time_t ended_at);
int db_update_status_reason(const char *job_id, const char *reason);
int db_update_job_started(const char *job_id, const char *machine_id,
                           time_t started_at);
Job *db_get_job(const char *job_id);
int  db_list_jobs(Job *jobs, int max_count);
int  db_list_held_jobs(Job *jobs, int max_count);
int  db_list_running_jobs(Job *jobs, int max_count);

int  db_insert_api_key(const char *key_hash, const char *label);
int  db_insert_api_key_ex(const char *key_hash, const char *label,
                         const char *role, time_t expires_at);
int  db_insert_api_key_full(const char *key_hash, const char *label,
                           const char *role, const char *user_id,
                           time_t expires_at);
int  db_validate_api_key(const char *key_hash);
int  db_revoke_api_key(const char *key_hash);

/* Resolve the role for a key hash. Returns 1 if valid (writes "admin" or
   "user" into out_role, must be >= 16 bytes), 0 if invalid/expired/revoked. */
int  db_resolve_api_key_role(const char *key_hash, char *out_role);

/* Same as above but also resolves the user_id tied to the key. */
int  db_resolve_api_key_full(const char *key_hash, char *out_role,
                             char *out_user_id, int uid_len);

typedef struct {
    char key_hash[65];
    char label[128];
    char role[16];
    char user_id[128];
    time_t created_at;
    time_t expires_at;
    int  revoked;
} ApiKeyInfo;

int  db_list_api_keys(ApiKeyInfo *out, int max_count);


typedef struct {
    int total;
    int held;
    int in_queue;
    int starting;
    int running;
    int finished;
    int cancelled;
    int failed;
} JobStats;

int db_job_stats(JobStats *out);
int db_purge_jobs(void);

typedef struct {
    char user_id[128];
    int  total_jobs;
    int  running;
    int  in_queue;
    int  held;
    int  finished;
    int  failed;
} UserInfo;

int db_list_users(UserInfo *out, int max_count);

/* ── User management ─────────────────────────── */

typedef struct {
    char   user_id[128];
    char   display_name[256];
    char   email[256];
    int    enabled;
    time_t created_at;
} UserRecord;

int  db_create_user(const UserRecord *u);
int  db_update_user(const UserRecord *u);
int  db_delete_user(const char *user_id);
int  db_get_user(const char *user_id, UserRecord *out);
int  db_list_user_records(UserRecord *out, int max_count);

/* Password auth: set/check password, find existing key for user */
int  db_set_user_password(const char *user_id, const char *password_hash);
int  db_check_user_password(const char *user_id, const char *password_hash);
int  db_find_user_key(const char *user_id, char *out_key_hash, int hash_len);

int db_insert_allocation(const char *job_id, const char *machine_id,
                          int cores, int gpu, int ram_mb, int disk_mb);
int db_release_allocation(const char *job_id);

int db_update_input_files(const char *job_id, const char *input_files);int  db_update_job_timeout(const char *job_id, int timeout_seconds);int db_audit(const char *event, const char *detail);

/* ── Quotas ────────────────────────────────────── */
int  db_insert_quota(const Quota *q);
int  db_update_quota(const Quota *q);
int  db_delete_quota(const char *user_id, const char *app_id);
int  db_get_quota(const char *user_id, const char *app_id, Quota *out);
int  db_list_quotas(Quota *out, int max_count);

/* Check if a new job from (user_id, app_id) would violate quota limits.
   Returns 0 if allowed, -1 if quota exceeded; writes reason into `out_reason`. */
int  db_quota_check(const char *user_id, const char *app_id,
                    int req_cores, int req_ram_mb,
                    char *out_reason, int reason_len);

#endif /* DB_H */
