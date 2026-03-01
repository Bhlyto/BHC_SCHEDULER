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
    "  key_hash  TEXT PRIMARY KEY,"
    "  label     TEXT NOT NULL,"
    "  created_at INTEGER NOT NULL,"
    "  revoked   INTEGER NOT NULL DEFAULT 0"
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
        "input_dir,output_dir,submitted_at,input_files,timeout_seconds)"
        " VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?);";
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
    return job;
}

Job *db_get_job(const char *job_id)
{
    const char *sql =
        "SELECT id,command,status,priority,req_cores,req_gpu,req_ram_mb,"
        "req_disk_mb,machine_id,input_dir,output_dir,exit_code,"
        "submitted_at,started_at,ended_at,input_files,timeout_seconds,status_reason FROM jobs WHERE id=?;";
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
        "submitted_at,started_at,ended_at,input_files,timeout_seconds,status_reason"
        " FROM jobs ORDER BY submitted_at DESC LIMIT ?;";
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
        "submitted_at,started_at,ended_at,input_files,timeout_seconds,status_reason"
        " FROM jobs WHERE status=6 AND input_files!='' ORDER BY submitted_at ASC LIMIT ?;";
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
        "submitted_at,started_at,ended_at,input_files,timeout_seconds,status_reason"
        " FROM jobs WHERE status IN (1,2) ORDER BY started_at ASC LIMIT ?;";
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
    const char *sql =
        "INSERT INTO api_keys(key_hash,label,created_at,revoked) VALUES(?,?,strftime('%s','now'),0);";
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(s_db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(st, 1, key_hash, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, label,    -1, SQLITE_STATIC);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int db_validate_api_key(const char *key_hash)
{
    const char *sql =
        "SELECT 1 FROM api_keys WHERE key_hash=? AND revoked=0 LIMIT 1;";
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(s_db, sql, -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_text(st, 1, key_hash, -1, SQLITE_STATIC);
    int found = (sqlite3_step(st) == SQLITE_ROW);
    sqlite3_finalize(st);
    return found;
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