#include "db.h"
#include "sqlite3.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static sqlite3 *s_db = NULL;


static const char *SCHEMA =
    "CREATE TABLE IF NOT EXISTS jobs ("
    "  id            TEXT PRIMARY KEY,"
    "  command       TEXT NOT NULL,"
    "  status        INTEGER NOT NULL DEFAULT 0,"
    "  priority      INTEGER NOT NULL DEFAULT 50,"
    "  req_cores     INTEGER NOT NULL DEFAULT 1,"
    "  req_gpu       INTEGER NOT NULL DEFAULT 0,"
    "  req_ram_mb    INTEGER NOT NULL DEFAULT 0,"
    "  req_disk_mb   INTEGER NOT NULL DEFAULT 0,"
    "  machine_id    TEXT,"
    "  input_dir     TEXT,"
    "  output_dir    TEXT,"
    "  exit_code     INTEGER,"
    "  submitted_at  INTEGER,"
    "  started_at    INTEGER,"
    "  ended_at      INTEGER,"
    "  input_files   TEXT NOT NULL DEFAULT '',"
    "  timeout_seconds INTEGER NOT NULL DEFAULT 0,"
    "  status_reason TEXT NOT NULL DEFAULT ''"
    ");"

    "CREATE TABLE IF NOT EXISTS api_keys ("
    "  key_hash   TEXT PRIMARY KEY,"
    "  label      TEXT NOT NULL,"
    "  role       TEXT NOT NULL DEFAULT 'user',"
    "  user_id    TEXT NOT NULL DEFAULT '',"
    "  created_at INTEGER NOT NULL,"
    "  expires_at INTEGER NOT NULL DEFAULT 0,"
    "  revoked    INTEGER NOT NULL DEFAULT 0"
    ");"

    "CREATE TABLE IF NOT EXISTS allocations ("
    "  job_id      TEXT PRIMARY KEY,"
    "  machine_id  TEXT NOT NULL,"
    "  cores       INTEGER NOT NULL DEFAULT 0,"
    "  gpu         INTEGER NOT NULL DEFAULT 0,"
    "  ram_mb      INTEGER NOT NULL DEFAULT 0,"
    "  disk_mb     INTEGER NOT NULL DEFAULT 0,"
    "  allocated_at INTEGER,"
    "  released_at  INTEGER"
    ");"

    "CREATE TABLE IF NOT EXISTS audit_log ("
    "  id        INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  event     TEXT NOT NULL,"
    "  detail    TEXT,"
    "  ts        INTEGER NOT NULL"
    ");"

    "CREATE TABLE IF NOT EXISTS quotas ("
    "  user_id       TEXT NOT NULL DEFAULT '',"
    "  app_id        TEXT NOT NULL DEFAULT '',"
    "  max_jobs      INTEGER NOT NULL DEFAULT 0,"
    "  max_ram_mb    INTEGER NOT NULL DEFAULT 0,"
    "  max_cores     INTEGER NOT NULL DEFAULT 0,"
    "  max_concurrent INTEGER NOT NULL DEFAULT 0,"
    "  PRIMARY KEY (user_id, app_id)"
    ");"

    "CREATE TABLE IF NOT EXISTS users ("
    "  user_id       TEXT PRIMARY KEY,"
    "  display_name  TEXT NOT NULL DEFAULT '',"
    "  email         TEXT NOT NULL DEFAULT '',"
    "  password_hash TEXT NOT NULL DEFAULT '',"
    "  enabled       INTEGER NOT NULL DEFAULT 1,"
    "  created_at    INTEGER NOT NULL DEFAULT 0"
    ");";

int db_open(const char *path)
{
    if (sqlite3_open(path, &s_db) != SQLITE_OK) {
        fprintf(stderr, "[db] sqlite3_open: %s\n", sqlite3_errmsg(s_db));
        return -1;
    }
    sqlite3_exec(s_db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
    char *errmsg = NULL;
    if (sqlite3_exec(s_db, SCHEMA, NULL, NULL, &errmsg) != SQLITE_OK) {
        fprintf(stderr, "[db] schema: %s\n", errmsg);
        sqlite3_free(errmsg);
        return -1;
    }
    /* Migration: ignored on fresh DBs where column already exists */
    sqlite3_exec(s_db,
        "ALTER TABLE jobs ADD COLUMN input_files TEXT NOT NULL DEFAULT '';",
        NULL, NULL, NULL);
    sqlite3_exec(s_db,
        "ALTER TABLE jobs ADD COLUMN timeout_seconds INTEGER NOT NULL DEFAULT 0;",
        NULL, NULL, NULL);
    sqlite3_exec(s_db,
        "ALTER TABLE jobs ADD COLUMN status_reason TEXT NOT NULL DEFAULT '';",
        NULL, NULL, NULL);
    sqlite3_exec(s_db,
        "ALTER TABLE jobs ADD COLUMN user_id TEXT NOT NULL DEFAULT '';",
        NULL, NULL, NULL);
    sqlite3_exec(s_db,
        "ALTER TABLE jobs ADD COLUMN app_id TEXT NOT NULL DEFAULT '';",
        NULL, NULL, NULL);
    /* api_keys migrations (role + expires_at + user_id) */
    sqlite3_exec(s_db,
        "ALTER TABLE api_keys ADD COLUMN role TEXT NOT NULL DEFAULT 'admin';",
        NULL, NULL, NULL);
    sqlite3_exec(s_db,
        "ALTER TABLE api_keys ADD COLUMN expires_at INTEGER NOT NULL DEFAULT 0;",
        NULL, NULL, NULL);
    sqlite3_exec(s_db,
        "ALTER TABLE api_keys ADD COLUMN user_id TEXT NOT NULL DEFAULT '';",
        NULL, NULL, NULL);
    /* users migrations */
    sqlite3_exec(s_db,
        "ALTER TABLE users ADD COLUMN password_hash TEXT NOT NULL DEFAULT '';",
        NULL, NULL, NULL);
    return 0;
}

void db_close(void)
{
    if (s_db) { sqlite3_close(s_db); s_db = NULL; }
}

int db_insert_job(const Job *job)
{
    const char *sql =
        "INSERT INTO jobs(id,command,status,priority,"
        "req_cores,req_gpu,req_ram_mb,req_disk_mb,"
        "input_dir,output_dir,submitted_at,input_files,timeout_seconds,"
        "user_id,app_id)"
        " VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);";
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(s_db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(st, 1, job->id, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, job->command, -1, SQLITE_STATIC);
    sqlite3_bind_int (st, 3, job->status);
    sqlite3_bind_int (st, 4, job->priority);
    sqlite3_bind_int (st, 5, job->req_cores);
    sqlite3_bind_int (st, 6, job->req_gpu);
    sqlite3_bind_int (st, 7, job->req_ram_mb);
    sqlite3_bind_int (st, 8, job->req_disk_mb);
    sqlite3_bind_text(st, 9,  job->input_dir,       -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 10, job->output_dir,      -1, SQLITE_STATIC);
    sqlite3_bind_int64(st,11, (sqlite3_int64)job->submitted_at);
    sqlite3_bind_text(st, 12, job->input_files,     -1, SQLITE_STATIC);
    sqlite3_bind_int (st, 13, job->timeout_seconds);
    sqlite3_bind_text(st, 14, job->user_id,         -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 15, job->app_id,          -1, SQLITE_STATIC);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int db_update_job_status(const char *job_id, JobStatus status,
                          int exit_code, time_t ended_at)
{
    const char *sql =
        "UPDATE jobs SET status=?, exit_code=?, ended_at=? WHERE id=?;";
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(s_db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int  (st, 1, status);
    sqlite3_bind_int  (st, 2, exit_code);
    sqlite3_bind_int64(st, 3, (sqlite3_int64)ended_at);
    sqlite3_bind_text (st, 4, job_id, -1, SQLITE_STATIC);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int db_update_job_started(const char *job_id, const char *machine_id,
                           time_t started_at)
{
    const char *sql =
        "UPDATE jobs SET machine_id=?, started_at=?, status=1 WHERE id=?;";
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(s_db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text (st, 1, machine_id, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)started_at);
    sqlite3_bind_text (st, 3, job_id, -1, SQLITE_STATIC);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

static Job *row_to_job(sqlite3_stmt *st)
{
    Job *job = (Job *)calloc(1, sizeof(Job));
    if (!job) return NULL;
    const char *s;
    s = (const char *)sqlite3_column_text(st, 0); if (s) strncpy(job->id, s, sizeof(job->id)-1);
    s = (const char *)sqlite3_column_text(st, 1); if (s) strncpy(job->command, s, sizeof(job->command)-1);
    job->status     = (JobStatus)sqlite3_column_int(st, 2);
    job->priority   = sqlite3_column_int(st, 3);
    job->req_cores  = sqlite3_column_int(st, 4);
    job->req_gpu    = sqlite3_column_int(st, 5);
    job->req_ram_mb = sqlite3_column_int(st, 6);
    job->req_disk_mb= sqlite3_column_int(st, 7);
    s = (const char *)sqlite3_column_text(st, 8);  if (s) strncpy(job->machine_id, s, sizeof(job->machine_id)-1);
    s = (const char *)sqlite3_column_text(st, 9);  if (s) strncpy(job->input_dir,  s, sizeof(job->input_dir)-1);
    s = (const char *)sqlite3_column_text(st, 10); if (s) strncpy(job->output_dir, s, sizeof(job->output_dir)-1);
    job->exit_code    = sqlite3_column_int  (st, 11);
    job->submitted_at = (time_t)sqlite3_column_int64(st, 12);
    job->started_at   = (time_t)sqlite3_column_int64(st, 13);
    job->ended_at     = (time_t)sqlite3_column_int64(st, 14);
    s = (const char *)sqlite3_column_text(st, 15); if (s) strncpy(job->input_files,    s, sizeof(job->input_files)-1);
    job->timeout_seconds = sqlite3_column_int(st, 16);
    s = (const char *)sqlite3_column_text(st, 17); if (s) strncpy(job->status_reason, s, sizeof(job->status_reason)-1);
    s = (const char *)sqlite3_column_text(st, 18); if (s) strncpy(job->user_id, s, sizeof(job->user_id)-1);
    s = (const char *)sqlite3_column_text(st, 19); if (s) strncpy(job->app_id,  s, sizeof(job->app_id)-1);
    return job;
}

Job *db_get_job(const char *job_id)
{
    const char *sql =
        "SELECT id,command,status,priority,req_cores,req_gpu,req_ram_mb,"
        "req_disk_mb,machine_id,input_dir,output_dir,exit_code,"
        "submitted_at,started_at,ended_at,input_files,timeout_seconds,status_reason,"
        "user_id,app_id FROM jobs WHERE id=?;";
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(s_db, sql, -1, &st, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_text(st, 1, job_id, -1, SQLITE_STATIC);
    Job *job = NULL;
    if (sqlite3_step(st) == SQLITE_ROW) job = row_to_job(st);
    sqlite3_finalize(st);
    return job;
}

int db_list_jobs(Job *jobs, int max_count)
{
    const char *sql =
        "SELECT id,command,status,priority,req_cores,req_gpu,req_ram_mb,"
        "req_disk_mb,machine_id,input_dir,output_dir,exit_code,"
        "submitted_at,started_at,ended_at,input_files,timeout_seconds,status_reason,"
        "user_id,app_id FROM jobs ORDER BY submitted_at DESC LIMIT ?;";
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(s_db, sql, -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int(st, 1, max_count);
    int count = 0;
    while (sqlite3_step(st) == SQLITE_ROW && count < max_count) {
        Job *tmp = row_to_job(st);
        if (tmp) { jobs[count++] = *tmp; free(tmp); }
    }
    sqlite3_finalize(st);
    return count;
}

int db_list_held_jobs(Job *jobs, int max_count)
{
    const char *sql =
        "SELECT id,command,status,priority,req_cores,req_gpu,req_ram_mb,"
        "req_disk_mb,machine_id,input_dir,output_dir,exit_code,"
        "submitted_at,started_at,ended_at,input_files,timeout_seconds,status_reason,"
        "user_id,app_id FROM jobs WHERE status=6 AND input_files!='' ORDER BY submitted_at ASC LIMIT ?;";
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(s_db, sql, -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int(st, 1, max_count);
    int count = 0;
    while (sqlite3_step(st) == SQLITE_ROW && count < max_count) {
        Job *tmp = row_to_job(st);
        if (tmp) { jobs[count++] = *tmp; free(tmp); }
    }
    sqlite3_finalize(st);
    return count;
}

int db_list_running_jobs(Job *jobs, int max_count)
{
    const char *sql =
        "SELECT id,command,status,priority,req_cores,req_gpu,req_ram_mb,"
        "req_disk_mb,machine_id,input_dir,output_dir,exit_code,"
        "submitted_at,started_at,ended_at,input_files,timeout_seconds,status_reason,"
        "user_id,app_id FROM jobs WHERE status IN (1,2) ORDER BY started_at ASC LIMIT ?;";
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(s_db, sql, -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int(st, 1, max_count);
    int count = 0;
    while (sqlite3_step(st) == SQLITE_ROW && count < max_count) {
        Job *tmp = row_to_job(st);
        if (tmp) { jobs[count++] = *tmp; free(tmp); }
    }
    sqlite3_finalize(st);
    return count;
}

int db_insert_api_key(const char *key_hash, const char *label)
{
    return db_insert_api_key_ex(key_hash, label, "admin", 0);
}

int db_insert_api_key_ex(const char *key_hash, const char *label,
                         const char *role, time_t expires_at)
{
    return db_insert_api_key_full(key_hash, label, role, "", expires_at);
}

int db_insert_api_key_full(const char *key_hash, const char *label,
                           const char *role, const char *user_id,
                           time_t expires_at)
{
    const char *sql =
        "INSERT INTO api_keys(key_hash,label,role,user_id,created_at,expires_at,revoked)"
        " VALUES(?,?,?,?,strftime('%s','now'),?,0);";
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(s_db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text (st, 1, key_hash, -1, SQLITE_STATIC);
    sqlite3_bind_text (st, 2, label,    -1, SQLITE_STATIC);
    sqlite3_bind_text (st, 3, role && role[0] ? role : "user", -1, SQLITE_STATIC);
    sqlite3_bind_text (st, 4, user_id ? user_id : "", -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 5, (sqlite3_int64)expires_at);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int db_validate_api_key(const char *key_hash)
{
    char role[16];
    return db_resolve_api_key_role(key_hash, role);
}

int db_resolve_api_key_role(const char *key_hash, char *out_role)
{
    char uid[128];
    return db_resolve_api_key_full(key_hash, out_role, uid, sizeof(uid));
}

int db_resolve_api_key_full(const char *key_hash, char *out_role,
                            char *out_user_id, int uid_len)
{
    const char *sql =
        "SELECT role, expires_at, user_id FROM api_keys"
        " WHERE key_hash=? AND revoked=0 LIMIT 1;";
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(s_db, sql, -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_text(st, 1, key_hash, -1, SQLITE_STATIC);
    int found = 0;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const char *r = (const char *)sqlite3_column_text(st, 0);
        time_t exp    = (time_t)sqlite3_column_int64(st, 1);
        const char *u = (const char *)sqlite3_column_text(st, 2);
        /* Check expiration (0 = never expires) */
        if (exp > 0 && exp < (time_t)time(NULL)) {
            found = 0;
        } else {
            if (r) strncpy(out_role, r, 15);
            else   strncpy(out_role, "user", 15);
            out_role[15] = '\0';
            if (out_user_id && uid_len > 0) {
                if (u) strncpy(out_user_id, u, uid_len - 1);
                else   out_user_id[0] = '\0';
                out_user_id[uid_len - 1] = '\0';
            }
            found = 1;
        }
    }
    sqlite3_finalize(st);
    return found;
}

int db_list_api_keys(ApiKeyInfo *out, int max_count)
{
    const char *sql =
        "SELECT key_hash, label, role, created_at, expires_at, revoked, user_id"
        " FROM api_keys ORDER BY created_at DESC LIMIT ?;";
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(s_db, sql, -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int(st, 1, max_count);
    int count = 0;
    while (sqlite3_step(st) == SQLITE_ROW && count < max_count) {
        ApiKeyInfo *k = &out[count];
        memset(k, 0, sizeof(*k));
        const char *s;
        s = (const char *)sqlite3_column_text(st, 0); if (s) strncpy(k->key_hash, s, sizeof(k->key_hash)-1);
        s = (const char *)sqlite3_column_text(st, 1); if (s) strncpy(k->label,    s, sizeof(k->label)-1);
        s = (const char *)sqlite3_column_text(st, 2); if (s) strncpy(k->role,     s, sizeof(k->role)-1);
        k->created_at = (time_t)sqlite3_column_int64(st, 3);
        k->expires_at = (time_t)sqlite3_column_int64(st, 4);
        k->revoked    = sqlite3_column_int(st, 5);
        s = (const char *)sqlite3_column_text(st, 6); if (s) strncpy(k->user_id, s, sizeof(k->user_id)-1);
        count++;
    }
    sqlite3_finalize(st);
    return count;
}

int db_revoke_api_key(const char *key_hash)
{
    const char *sql = "UPDATE api_keys SET revoked=1 WHERE key_hash=?;";
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(s_db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(st, 1, key_hash, -1, SQLITE_STATIC);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int db_insert_allocation(const char *job_id, const char *machine_id,
                          int cores, int gpu, int ram_mb, int disk_mb)
{
    const char *sql =
        "INSERT OR REPLACE INTO allocations"
        "(job_id,machine_id,cores,gpu,ram_mb,disk_mb,allocated_at)"
        " VALUES(?,?,?,?,?,?,strftime('%s','now'));";
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(s_db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(st, 1, job_id,     -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, machine_id, -1, SQLITE_STATIC);
    sqlite3_bind_int (st, 3, cores);
    sqlite3_bind_int (st, 4, gpu);
    sqlite3_bind_int (st, 5, ram_mb);
    sqlite3_bind_int (st, 6, disk_mb);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int db_release_allocation(const char *job_id)
{
    const char *sql =
        "UPDATE allocations SET released_at=strftime('%s','now') WHERE job_id=?;";
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(s_db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(st, 1, job_id, -1, SQLITE_STATIC);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int db_update_input_files(const char *job_id, const char *input_files)
{
    const char *sql = "UPDATE jobs SET input_files=? WHERE id=?;";
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(s_db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(st, 1, input_files, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, job_id,      -1, SQLITE_STATIC);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int db_update_job_timeout(const char *job_id, int timeout_seconds)
{
    const char *sql = "UPDATE jobs SET timeout_seconds=? WHERE id=?;";
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(s_db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int (st, 1, timeout_seconds);
    sqlite3_bind_text(st, 2, job_id, -1, SQLITE_STATIC);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int db_update_status_reason(const char *job_id, const char *reason)
{
    const char *sql = "UPDATE jobs SET status_reason=? WHERE id=?;";
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(s_db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(st, 1, reason, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, job_id, -1, SQLITE_STATIC);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int db_audit(const char *event, const char *detail)
{
    const char *sql =
        "INSERT INTO audit_log(event,detail,ts) VALUES(?,?,strftime('%s','now'));";
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(s_db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(st, 1, event,  -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, detail, -1, SQLITE_STATIC);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return (rc == SQLITE_DONE) ? 0 : -1;
}
int db_job_stats(JobStats *out)
{
    memset(out, 0, sizeof(*out));
    const char *sql = "SELECT status, COUNT(*) FROM jobs GROUP BY status;";
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(s_db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    while (sqlite3_step(st) == SQLITE_ROW) {
        int status = sqlite3_column_int(st, 0);
        int count  = sqlite3_column_int(st, 1);
        out->total += count;
        switch ((JobStatus)status) {
            case JOB_STATUS_HELD:      out->held      += count; break;
            case JOB_STATUS_IN_QUEUE:  out->in_queue  += count; break;
            case JOB_STATUS_STARTING:  out->starting  += count; break;
            case JOB_STATUS_RUNNING:   out->running   += count; break;
            case JOB_STATUS_FINISHED:  out->finished  += count; break;
            case JOB_STATUS_CANCELLED: out->cancelled += count; break;
            case JOB_STATUS_FAILED:    out->failed    += count; break;
        }
    }
    sqlite3_finalize(st);
    return 0;
}

int db_purge_jobs(void)
{
    const char *sql = "DELETE FROM jobs WHERE status IN (3, 4, 5);";
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(s_db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_step(st);
    int deleted = sqlite3_changes(s_db);
    sqlite3_finalize(st);
    return deleted;
}

int db_list_users(UserInfo *out, int max_count)
{
    const char *sql =
        "SELECT user_id,"
        " COUNT(*),"
        " SUM(CASE WHEN status IN (1,2) THEN 1 ELSE 0 END),"
        " SUM(CASE WHEN status = 0 THEN 1 ELSE 0 END),"
        " SUM(CASE WHEN status = 6 THEN 1 ELSE 0 END),"
        " SUM(CASE WHEN status = 3 THEN 1 ELSE 0 END),"
        " SUM(CASE WHEN status = 5 THEN 1 ELSE 0 END)"
        " FROM jobs WHERE user_id != ''"
        " GROUP BY user_id ORDER BY user_id LIMIT ?;";
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(s_db, sql, -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int(st, 1, max_count);
    int count = 0;
    while (sqlite3_step(st) == SQLITE_ROW && count < max_count) {
        UserInfo *u = &out[count];
        memset(u, 0, sizeof(*u));
        const char *s = (const char *)sqlite3_column_text(st, 0);
        if (s) strncpy(u->user_id, s, sizeof(u->user_id)-1);
        u->total_jobs = sqlite3_column_int(st, 1);
        u->running    = sqlite3_column_int(st, 2);
        u->in_queue   = sqlite3_column_int(st, 3);
        u->held       = sqlite3_column_int(st, 4);
        u->finished   = sqlite3_column_int(st, 5);
        u->failed     = sqlite3_column_int(st, 6);
        count++;
    }
    sqlite3_finalize(st);
    return count;
}

/* ── User management ─────────────────────────────────────────────── */

int db_create_user(const UserRecord *u)
{
    const char *sql =
        "INSERT INTO users(user_id,display_name,email,enabled,created_at)"
        " VALUES(?,?,?,?,strftime('%s','now'));";
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(s_db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(st, 1, u->user_id,      -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, u->display_name,  -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 3, u->email,         -1, SQLITE_STATIC);
    sqlite3_bind_int (st, 4, u->enabled);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int db_set_user_password(const char *user_id, const char *password_hash)
{
    const char *sql = "UPDATE users SET password_hash=? WHERE user_id=?;";
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(s_db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(st, 1, password_hash, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, user_id,       -1, SQLITE_STATIC);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) return -1;
    return sqlite3_changes(s_db) > 0 ? 0 : -1;
}

int db_check_user_password(const char *user_id, const char *password_hash)
{
    const char *sql =
        "SELECT enabled FROM users WHERE user_id=? AND password_hash=?;";
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(s_db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(st, 1, user_id,       -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, password_hash, -1, SQLITE_STATIC);
    int result = -1;
    if (sqlite3_step(st) == SQLITE_ROW) {
        int enabled = sqlite3_column_int(st, 0);
        result = enabled ? 0 : -2; /* -2 = account disabled */
    }
    sqlite3_finalize(st);
    return result;
}

int db_find_user_key(const char *user_id, char *out_key_hash, int hash_len)
{
    const char *sql =
        "SELECT key_hash FROM api_keys"
        " WHERE user_id=? AND revoked=0"
        " AND (expires_at=0 OR expires_at > strftime('%s','now'))"
        " LIMIT 1;";
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(s_db, sql, -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_text(st, 1, user_id, -1, SQLITE_STATIC);
    int found = 0;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const char *h = (const char *)sqlite3_column_text(st, 0);
        if (h) { strncpy(out_key_hash, h, hash_len - 1); out_key_hash[hash_len-1] = '\0'; found = 1; }
    }
    sqlite3_finalize(st);
    return found;
}

int db_update_user(const UserRecord *u)
{
    const char *sql =
        "UPDATE users SET display_name=?,email=?,enabled=? WHERE user_id=?;";
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(s_db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(st, 1, u->display_name, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, u->email,        -1, SQLITE_STATIC);
    sqlite3_bind_int (st, 3, u->enabled);
    sqlite3_bind_text(st, 4, u->user_id,      -1, SQLITE_STATIC);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) return -1;
    return sqlite3_changes(s_db) > 0 ? 0 : -1;
}

int db_delete_user(const char *user_id)
{
    const char *sql = "DELETE FROM users WHERE user_id=?;";
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(s_db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(st, 1, user_id, -1, SQLITE_STATIC);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) return -1;
    return sqlite3_changes(s_db) > 0 ? 0 : -1;
}

int db_get_user(const char *user_id, UserRecord *out)
{
    const char *sql =
        "SELECT user_id,display_name,email,enabled,created_at"
        " FROM users WHERE user_id=?;";
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(s_db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(st, 1, user_id, -1, SQLITE_STATIC);
    if (sqlite3_step(st) != SQLITE_ROW) { sqlite3_finalize(st); return -1; }
    memset(out, 0, sizeof(*out));
    const char *s;
    s = (const char *)sqlite3_column_text(st, 0); if (s) strncpy(out->user_id,      s, sizeof(out->user_id)-1);
    s = (const char *)sqlite3_column_text(st, 1); if (s) strncpy(out->display_name, s, sizeof(out->display_name)-1);
    s = (const char *)sqlite3_column_text(st, 2); if (s) strncpy(out->email,        s, sizeof(out->email)-1);
    out->enabled    = sqlite3_column_int(st, 3);
    out->created_at = (time_t)sqlite3_column_int64(st, 4);
    sqlite3_finalize(st);
    return 0;
}

int db_list_user_records(UserRecord *out, int max_count)
{
    const char *sql =
        "SELECT user_id,display_name,email,enabled,created_at"
        " FROM users ORDER BY user_id LIMIT ?;";
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(s_db, sql, -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int(st, 1, max_count);
    int count = 0;
    while (sqlite3_step(st) == SQLITE_ROW && count < max_count) {
        UserRecord *u = &out[count];
        memset(u, 0, sizeof(*u));
        const char *s;
        s = (const char *)sqlite3_column_text(st, 0); if (s) strncpy(u->user_id,      s, sizeof(u->user_id)-1);
        s = (const char *)sqlite3_column_text(st, 1); if (s) strncpy(u->display_name, s, sizeof(u->display_name)-1);
        s = (const char *)sqlite3_column_text(st, 2); if (s) strncpy(u->email,        s, sizeof(u->email)-1);
        u->enabled    = sqlite3_column_int(st, 3);
        u->created_at = (time_t)sqlite3_column_int64(st, 4);
        count++;
    }
    sqlite3_finalize(st);
    return count;
}

/* ── Quotas ──────────────────────────────────────────────────────── */

int db_insert_quota(const Quota *q)
{
    const char *sql =
        "INSERT INTO quotas(user_id,app_id,max_jobs,max_ram_mb,max_cores,max_concurrent)"
        " VALUES(?,?,?,?,?,?);";
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(s_db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(st, 1, q->user_id, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, q->app_id,  -1, SQLITE_STATIC);
    sqlite3_bind_int (st, 3, q->max_jobs);
    sqlite3_bind_int (st, 4, q->max_ram_mb);
    sqlite3_bind_int (st, 5, q->max_cores);
    sqlite3_bind_int (st, 6, q->max_concurrent);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int db_update_quota(const Quota *q)
{
    const char *sql =
        "UPDATE quotas SET max_jobs=?,max_ram_mb=?,max_cores=?,max_concurrent=?"
        " WHERE user_id=? AND app_id=?;";
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(s_db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int (st, 1, q->max_jobs);
    sqlite3_bind_int (st, 2, q->max_ram_mb);
    sqlite3_bind_int (st, 3, q->max_cores);
    sqlite3_bind_int (st, 4, q->max_concurrent);
    sqlite3_bind_text(st, 5, q->user_id, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 6, q->app_id,  -1, SQLITE_STATIC);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) return -1;
    return sqlite3_changes(s_db) > 0 ? 0 : -1;
}

int db_delete_quota(const char *user_id, const char *app_id)
{
    const char *sql = "DELETE FROM quotas WHERE user_id=? AND app_id=?;";
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(s_db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(st, 1, user_id, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, app_id,  -1, SQLITE_STATIC);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) return -1;
    return sqlite3_changes(s_db) > 0 ? 0 : -1;
}

int db_get_quota(const char *user_id, const char *app_id, Quota *out)
{
    const char *sql =
        "SELECT user_id,app_id,max_jobs,max_ram_mb,max_cores,max_concurrent"
        " FROM quotas WHERE user_id=? AND app_id=?;";
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(s_db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(st, 1, user_id, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, app_id,  -1, SQLITE_STATIC);
    if (sqlite3_step(st) != SQLITE_ROW) { sqlite3_finalize(st); return -1; }
    memset(out, 0, sizeof(*out));
    const char *s;
    s = (const char *)sqlite3_column_text(st, 0); if (s) strncpy(out->user_id, s, sizeof(out->user_id)-1);
    s = (const char *)sqlite3_column_text(st, 1); if (s) strncpy(out->app_id,  s, sizeof(out->app_id)-1);
    out->max_jobs       = sqlite3_column_int(st, 2);
    out->max_ram_mb     = sqlite3_column_int(st, 3);
    out->max_cores      = sqlite3_column_int(st, 4);
    out->max_concurrent = sqlite3_column_int(st, 5);
    sqlite3_finalize(st);
    return 0;
}

int db_list_quotas(Quota *out, int max_count)
{
    const char *sql =
        "SELECT user_id,app_id,max_jobs,max_ram_mb,max_cores,max_concurrent"
        " FROM quotas ORDER BY user_id, app_id LIMIT ?;";
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(s_db, sql, -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int(st, 1, max_count);
    int count = 0;
    while (sqlite3_step(st) == SQLITE_ROW && count < max_count) {
        Quota *q = &out[count];
        memset(q, 0, sizeof(*q));
        const char *s;
        s = (const char *)sqlite3_column_text(st, 0); if (s) strncpy(q->user_id, s, sizeof(q->user_id)-1);
        s = (const char *)sqlite3_column_text(st, 1); if (s) strncpy(q->app_id,  s, sizeof(q->app_id)-1);
        q->max_jobs       = sqlite3_column_int(st, 2);
        q->max_ram_mb     = sqlite3_column_int(st, 3);
        q->max_cores      = sqlite3_column_int(st, 4);
        q->max_concurrent = sqlite3_column_int(st, 5);
        count++;
    }
    sqlite3_finalize(st);
    return count;
}

/*
 * Look up the most specific quota that matches (user_id, app_id).
 * Resolution order (first match wins):
 *   1. Exact (user_id, app_id)
 *   2. (user_id, "")          — user-wide limit
 *   3. ("", app_id)           — app-wide limit
 *   4. ("", "")               — global default
 * Returns 0 if a quota was found, -1 if none exists (meaning unlimited).
 */
static int resolve_quota(const char *user_id, const char *app_id, Quota *out)
{
    if (user_id && user_id[0] && app_id && app_id[0]) {
        if (db_get_quota(user_id, app_id, out) == 0) return 0;
    }
    if (user_id && user_id[0]) {
        if (db_get_quota(user_id, "", out) == 0) return 0;
    }
    if (app_id && app_id[0]) {
        if (db_get_quota("", app_id, out) == 0) return 0;
    }
    if (db_get_quota("", "", out) == 0) return 0;
    return -1;
}

int db_quota_check(const char *user_id, const char *app_id,
                   int req_cores, int req_ram_mb,
                   char *out_reason, int reason_len)
{
    Quota q;
    if (resolve_quota(user_id, app_id, &q) != 0)
        return 0;  /* no quota configured → allow */

    /* Count only STARTING(1) and RUNNING(2) jobs — those that are actually
       consuming resources right now.  Queued / held jobs are not counted;
       they will wait their turn until a slot opens up. */
    const char *sql_total =
        "SELECT COUNT(*), COALESCE(SUM(req_cores),0), COALESCE(SUM(req_ram_mb),0)"
        " FROM jobs WHERE status IN (1,2)"
        " AND (? = '' OR user_id = ?)"
        " AND (? = '' OR app_id  = ?);";

    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(s_db, sql_total, -1, &st, NULL) != SQLITE_OK) return 0;
    const char *uid = (user_id && user_id[0]) ? user_id : "";
    const char *aid = (app_id  && app_id[0])  ? app_id  : "";
    sqlite3_bind_text(st, 1, uid, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, uid, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 3, aid, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 4, aid, -1, SQLITE_STATIC);

    int running_jobs = 0, used_cores = 0, used_ram = 0;
    if (sqlite3_step(st) == SQLITE_ROW) {
        running_jobs = sqlite3_column_int(st, 0);
        used_cores   = sqlite3_column_int(st, 1);
        used_ram     = sqlite3_column_int(st, 2);
    }
    sqlite3_finalize(st);

    /* max_jobs — max simultaneously running jobs (same as max_concurrent) */
    if (q.max_jobs > 0 && running_jobs + 1 > q.max_jobs) {
        snprintf(out_reason, reason_len,
            "Quota: max_jobs=%d, running=%d — waiting in queue (user=%s, app=%s)",
            q.max_jobs, running_jobs, uid, aid);
        return -1;
    }
    /* max_concurrent — running/starting jobs */
    if (q.max_concurrent > 0 && running_jobs + 1 > q.max_concurrent) {
        snprintf(out_reason, reason_len,
            "Quota: max_concurrent=%d, running=%d — waiting in queue (user=%s, app=%s)",
            q.max_concurrent, running_jobs, uid, aid);
        return -1;
    }
    /* max_cores — total cores across running jobs */
    if (q.max_cores > 0 && used_cores + req_cores > q.max_cores) {
        snprintf(out_reason, reason_len,
            "Quota: max_cores=%d, used=%d, requested=%d — waiting in queue (user=%s, app=%s)",
            q.max_cores, used_cores, req_cores, uid, aid);
        return -1;
    }
    /* max_ram_mb — total RAM across running jobs */
    if (q.max_ram_mb > 0 && used_ram + req_ram_mb > q.max_ram_mb) {
        snprintf(out_reason, reason_len,
            "Quota: max_ram_mb=%d, used=%d, requested=%d — waiting in queue (user=%s, app=%s)",
            q.max_ram_mb, used_ram, req_ram_mb, uid, aid);
        return -1;
    }

    return 0;
}