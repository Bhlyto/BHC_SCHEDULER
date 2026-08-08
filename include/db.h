#ifndef DB_H
#define DB_H

#include <time.h>
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
int db_begin(void);
int db_commit(void);
int db_rollback(void);

/* ── Jobs ──────────────────────────────────────── */
int db_insert_job(const Job *job);
int db_update_job_status(const char *job_id, JobStatus status,
                         int exit_code, time_t ended_at);
int db_update_status_reason(const char *job_id, const char *reason);
int db_update_job_started(const char *job_id, const char *machine_id,
                           time_t started_at);
Job *db_get_job(const char *job_id);
int  db_list_jobs(Job *jobs, int max_count);
int  db_query_jobs(const char *user_id, int status, const char *app_id,
                   int limit, int offset, Job **out_jobs);
int  db_get_submission_job(const char *user_id, const char *idempotency_key,
                           char *out_job_id, int out_len);
int  db_store_submission_key(const char *user_id, const char *idempotency_key,
                             const char *job_id);
int  db_list_held_jobs(Job *jobs, int max_count);
int  db_list_running_jobs(Job *jobs, int max_count);
int  db_list_queued_jobs(Job *jobs, int max_count, int offset);
int  db_retry_job(const char *job_id);

/* Mark RUNNING jobs (and legacy pre-v1 STARTING rows) as failed after a
   scheduler restart and release allocations. Returns the recovered count. */
int  db_recover_incomplete_jobs(void);
int  db_update_job_batch_id(const char *job_id, const char *batch_id);

typedef struct {
    char id[64];
    char name[256];
    char user_id[128];
    time_t created_at;
} BatchRecord;

typedef struct {
    int total;
    int created;
    int queued;
    int running;
    int succeeded;
    int failed;
    int cancelled;
} BatchStats;

int db_insert_batch(const BatchRecord *batch);
int db_get_batch(const char *batch_id, BatchRecord *out);
int db_get_batch_stats(const char *batch_id, BatchStats *out);

typedef struct {
    long long id;
    char job_id[37];
    char type[32];
    char uri[1024];
    long long size_bytes;
    char checksum[65];
    time_t created_at;
} ArtifactRecord;

/* Insert or refresh metadata for one physical artifact. The (job,type,uri)
   tuple is stable, so repeated collection after a restart is idempotent. */
int db_upsert_artifact(ArtifactRecord *artifact);
int db_list_artifacts(const char *job_id, ArtifactRecord *out, int max_count);
int  db_load_jobs_by_status(JobStatus status, Job **out_jobs);
/* Reconcile interrupted jobs and allocations; returns affected job count, -1 on error. */
int  db_recover_after_restart(void);

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
    int created;
    int queued;
    int running;
    int succeeded;
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
int  db_get_user_auth(const char *user_id, char *out_hash, int hash_len,
                      int *out_enabled);
int  db_find_user_key(const char *user_id, char *out_key_hash, int hash_len);

int db_insert_allocation(const char *job_id, const char *machine_id,
                          int cores, int gpu, int ram_mb, int disk_mb);
int db_release_allocation(const char *job_id);

int db_update_input_files(const char *job_id, const char *input_files);int  db_update_job_timeout(const char *job_id, int timeout_seconds);int db_audit(const char *event, const char *detail);
int db_update_depends_on(const char *job_id, const char *depends_on);
int db_update_workflow_id(const char *job_id, const char *workflow_id);
int db_update_same_machine_as(const char *job_id, const char *ref_id);
int db_update_job_submission(const Job *job);
/* Check all dependency job statuses.
   Returns: 0 = all SUCCEEDED, 1 = still waiting, -1 = a dep FAILED/CANCELLED */
int db_check_deps_status(const char *depends_on_csv);

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

/* ── Workflows ─────────────────────────────────── */
typedef struct {
    char id[64];
    char name[256];
    char owner_id[128];
    int  is_global;
    char steps_json[16384];
    time_t created_at;
    time_t updated_at;
} WorkflowDef;

int  db_insert_workflow(const WorkflowDef *w);
int  db_update_workflow(const WorkflowDef *w);
int  db_delete_workflow(const char *wf_id);
int  db_get_workflow(const char *wf_id, WorkflowDef *out);
int  db_list_workflows(const char *user_id, WorkflowDef *out, int max_count);

/* Favorites */
int  db_add_workflow_favorite(const char *user_id, const char *wf_id);
int  db_remove_workflow_favorite(const char *user_id, const char *wf_id);
int  db_is_workflow_favorite(const char *user_id, const char *wf_id);

/* ── Persistent Events (for reporting) ─────────── */
typedef struct {
    int    id;
    char   category[64];    /* "job", "auth", "machine", "cloud", "system" */
    char   event_type[64];  /* "submitted", "started", "finished", "failed", "login", etc. */
    char   detail[1024];
    char   user_id[128];
    char   job_id[37];
    char   machine_id[64];
    time_t created_at;
} EventRecord;

int  db_insert_event(const char *category, const char *event_type,
                     const char *detail, const char *user_id,
                     const char *job_id, const char *machine_id);
int  db_list_events(EventRecord *out, int max_count,
                    const char *category_filter,
                    time_t from_ts, time_t to_ts);
int  db_count_events_by_type(const char *category, time_t from_ts, time_t to_ts,
                             char types[][64], int counts[], int max_types);

/* ── Reporting / Analytics ──────────────────────── */
typedef struct {
    char   period[32]; /* "2026-03-19" or "2026-03" or hour bucket */
    int    total;
    int    finished;
    int    failed;
    double avg_duration_s;
} JobTimeBucket;

int  db_report_jobs_over_time(JobTimeBucket *out, int max_buckets,
                              const char *granularity,
                              time_t from_ts, time_t to_ts);

typedef struct {
    char user_id[128];
    int  total_jobs;
    int  finished;
    int  failed;
    double avg_duration_s;
    int  total_cores_used;
    int  total_ram_mb_used;
} UserReport;

int  db_report_per_user(UserReport *out, int max_count,
                        time_t from_ts, time_t to_ts);

typedef struct {
    char app_id[128];
    int  total_jobs;
    int  finished;
    int  failed;
    double avg_duration_s;
} AppReport;

int  db_report_per_app(AppReport *out, int max_count,
                       time_t from_ts, time_t to_ts);

typedef struct {
    char machine_id[64];
    int  total_allocations;
    int  total_cores_reserved;
    int  total_ram_mb_reserved;
    double avg_utilization_pct;
} MachineReport;

int  db_report_per_machine(MachineReport *out, int max_count,
                           time_t from_ts, time_t to_ts);

#endif /* DB_H */
