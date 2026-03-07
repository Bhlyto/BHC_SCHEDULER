#include "http.h"
#include "job.h"
#include "queue.h"
#include "scheduler.h"
#include "resources.h"
#include "transfer.h"
#include "config.h"
#include "db.h"
#include "log.h"
#include "cJSON.h"
#include "mongoose.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

#ifdef _WIN32
#  include <windows.h>
#  include <wincrypt.h>
#  include <direct.h>
#else
#  include <dirent.h>
#endif

/*
 * routes.c
 * Maps HTTP method + URI to handler functions.
 *
 * Route table:
 *   POST   /jobs                      → submit_job
 *   GET    /jobs                      → list_jobs
 *   DELETE /jobs                      → purge_jobs  (delete all FINISHED/FAILED/CANCELLED)
 *   GET    /jobs/events               → sse_subscribe  (Server-Sent Events)
 *   GET    /jobs/:id                  → get_job
 *   DELETE /jobs/:id                  → cancel_job
 *   POST   /jobs/:id/input/:filename  → upload_input
 *   GET    /jobs/:id/output/:filename → download_output
 *   GET    /jobs/:id/log              → get_job_log  (stdout.log)
 *   GET    /jobs/:id/log/stderr       → get_job_log_err (stderr.log)
 *   GET    /resources                 → get_resources
 *   GET    /stats                     → get_stats
 *   POST   /provision                 → add_machine
 *   DELETE /provision/:id             → remove_machine
 */

/* ── Helpers ─────────────────────────────────────────────────────── */

static void extract_segment(const char *uri, int seg_index, char *out, int out_len)
{
    const char *p = uri;
    while (*p == '/') p++;
    for (int i = 0; i < seg_index; i++) {
        while (*p && *p != '/') p++;
        while (*p == '/') p++;
    }
    int i = 0;
    while (*p && *p != '/' && i < out_len - 1)
        out[i++] = *p++;
    out[i] = '\0';
}

static cJSON *job_to_json(const Job *job)
{
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "id",         job->id);
    cJSON_AddStringToObject(obj, "command",    job->command);
    cJSON_AddStringToObject(obj, "status",     job_status_str(job->status));
    cJSON_AddNumberToObject(obj, "priority",   job->priority);
    cJSON_AddNumberToObject(obj, "req_cores",  job->req_cores);
    cJSON_AddNumberToObject(obj, "req_gpu",    job->req_gpu);
    cJSON_AddNumberToObject(obj, "req_ram_mb", job->req_ram_mb);
    cJSON_AddNumberToObject(obj, "req_disk_mb",job->req_disk_mb);
    cJSON_AddStringToObject(obj, "machine_id", job->machine_id);
    cJSON_AddNumberToObject(obj, "submitted_at",(double)job->submitted_at);
    cJSON_AddNumberToObject(obj, "started_at",  (double)job->started_at);
    cJSON_AddNumberToObject(obj, "ended_at",    (double)job->ended_at);
    cJSON_AddNumberToObject(obj, "exit_code",        job->exit_code);
    cJSON_AddNumberToObject(obj, "timeout_seconds",   job->timeout_seconds);
    cJSON_AddStringToObject(obj, "status_reason",     job->status_reason);
    cJSON_AddStringToObject(obj, "input_files",       job->input_files);
    cJSON_AddStringToObject(obj, "user_id",           job->user_id);
    cJSON_AddStringToObject(obj, "app_id",            job->app_id);
    return obj;
}

/* ── Handlers ────────────────────────────────────────────────────── */

/* ── Web UI (public, no auth) ────────────────────────────────────── */

/* Resolve exe-relative path into buf */
static void exe_relative_path(const char *suffix, char *buf, int buf_len)
{
#ifdef _WIN32
    char exe[MAX_PATH] = {0};
    GetModuleFileNameA(NULL, exe, sizeof(exe));
    char *sep = strrchr(exe, '\\');
    if (sep) { *(sep + 1) = '\0'; _snprintf(buf, buf_len - 1, "%s%s", exe, suffix); }
    else     { strncpy(buf, suffix, buf_len - 1); }
#else
    char exe[512] = {0};
    ssize_t len = readlink("/proc/self/exe", exe, sizeof(exe)-1);
    if (len > 0) {
        exe[len] = '\0';
        char *sep = strrchr(exe, '/');
        if (sep) { *(sep + 1) = '\0'; snprintf(buf, buf_len - 1, "%s%s", exe, suffix); }
        else     { strncpy(buf, suffix, buf_len - 1); }
    } else { strncpy(buf, suffix, buf_len - 1); }
#endif
}

static const char *mime_for_ext(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    if (strcmp(dot, ".html") == 0) return "text/html; charset=utf-8";
    if (strcmp(dot, ".css")  == 0) return "text/css; charset=utf-8";
    if (strcmp(dot, ".js")   == 0) return "application/javascript; charset=utf-8";
    if (strcmp(dot, ".json") == 0) return "application/json; charset=utf-8";
    if (strcmp(dot, ".png")  == 0) return "image/png";
    if (strcmp(dot, ".svg")  == 0) return "image/svg+xml";
    if (strcmp(dot, ".ico")  == 0) return "image/x-icon";
    return "application/octet-stream";
}

static void serve_web_file(struct mg_connection *c, const char *file_path)
{
    /* Reject path traversal */
    if (strstr(file_path, "..")) { http_error(c, 403, "Forbidden"); return; }

    char suffix[512] = {0};
#ifdef _WIN32
    _snprintf(suffix, sizeof(suffix) - 1, "web\\%s", file_path);
    for (char *p = suffix; *p; p++) if (*p == '/') *p = '\\';
#else
    snprintf(suffix, sizeof(suffix) - 1, "web/%s", file_path);
#endif
    char path[512] = {0};
    exe_relative_path(suffix, path, sizeof(path));

    FILE *f = fopen(path, "rb");
    if (!f) { http_error(c, 404, "Not found"); return; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc(sz + 1);
    if (!buf) { fclose(f); http_error(c, 500, "Out of memory"); return; }
    fread(buf, 1, sz, f);
    fclose(f);
    buf[sz] = '\0';

    char hdrs[256];
    snprintf(hdrs, sizeof(hdrs), "Content-Type: %s\r\nCache-Control: no-cache\r\n", mime_for_ext(file_path));
    mg_http_reply(c, 200, hdrs, "%s", buf);
    free(buf);
}

static void serve_web_ui(struct mg_connection *c)
{
    serve_web_file(c, "index.html");
}

/* ── Public auth handlers (no API key required) ──────────────────── */

static void generate_api_key_for_user(const char *user_id, const char *role,
                                      char *out_raw_hex, char *out_hash)
{
    unsigned char raw[32];
#ifdef _WIN32
    {
        HCRYPTPROV hprov;
        CryptAcquireContextA(&hprov, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT);
        CryptGenRandom(hprov, sizeof(raw), raw);
        CryptReleaseContext(hprov, 0);
    }
#else
    {
        FILE *urnd = fopen("/dev/urandom", "rb");
        if (urnd) { fread(raw, 1, sizeof(raw), urnd); fclose(urnd); }
    }
#endif
    for (int i = 0; i < 32; i++) sprintf(out_raw_hex + i*2, "%02x", raw[i]);
    out_raw_hex[64] = '\0';
    auth_hash_key(out_raw_hex, out_hash);
    time_t expires = time(NULL) + 86400; /* 24 hours */
    db_insert_api_key_full(out_hash, "auto-login", role, user_id, expires);
}

/* POST /auth/login — public, no API key needed */
static void auth_login(struct mg_connection *c, struct mg_http_message *hm)
{
    char body[2048] = {0};
    size_t blen = hm->body.len < sizeof(body)-1 ? hm->body.len : sizeof(body)-1;
    memcpy(body, hm->body.buf, blen);

    cJSON *req = cJSON_Parse(body);
    if (!req) { http_error(c, 400, "Invalid JSON"); return; }

    cJSON *juid = cJSON_GetObjectItemCaseSensitive(req, "user_id");
    cJSON *jpwd = cJSON_GetObjectItemCaseSensitive(req, "password");
    if (!cJSON_IsString(juid) || !cJSON_IsString(jpwd) ||
        !juid->valuestring[0] || !jpwd->valuestring[0]) {
        cJSON_Delete(req);
        http_error(c, 400, "Missing 'user_id' and/or 'password'");
        return;
    }

    /* Hash the password */
    char pwd_hash[65];
    auth_hash_key(jpwd->valuestring, pwd_hash);

    /* Copy user_id before freeing JSON */
    char uid[128] = {0};
    strncpy(uid, juid->valuestring, sizeof(uid)-1);

    int rc = db_check_user_password(uid, pwd_hash);
    cJSON_Delete(req);

    if (rc == -2) {
        http_error(c, 403, "Account disabled");
        return;
    }
    if (rc != 0) {
        http_error(c, 401, "Invalid user_id or password");
        return;
    }

    /* Find existing valid key or generate a new one */
    char key_hash[65] = {0};
    char raw_hex[65] = {0};
    if (db_find_user_key(uid, key_hash, sizeof(key_hash))) {
        db_revoke_api_key(key_hash);
    }
    char hash[65];
    generate_api_key_for_user(uid, "user", raw_hex, hash);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "user_id", uid);
    cJSON_AddStringToObject(resp, "api_key", raw_hex);
    cJSON_AddStringToObject(resp, "role",    "user");
    char *s = cJSON_PrintUnformatted(resp);
    http_json_reply(c, 200, s);
    free(s);
    cJSON_Delete(resp);
    log_info("auth", "User '%s' logged in via password", uid);
}

/* POST /auth/change-password — requires valid API key */
static void auth_change_password(struct mg_connection *c,
                                 struct mg_http_message *hm,
                                 const char *auth_user_id)
{
    if (!auth_user_id[0]) {
        http_error(c, 403, "API key not bound to a user");
        return;
    }

    char body[2048] = {0};
    size_t blen = hm->body.len < sizeof(body)-1 ? hm->body.len : sizeof(body)-1;
    memcpy(body, hm->body.buf, blen);

    cJSON *req = cJSON_Parse(body);
    if (!req) { http_error(c, 400, "Invalid JSON"); return; }

    cJSON *jold = cJSON_GetObjectItemCaseSensitive(req, "old_password");
    cJSON *jnew = cJSON_GetObjectItemCaseSensitive(req, "new_password");
    if (!cJSON_IsString(jold) || !cJSON_IsString(jnew) ||
        !jold->valuestring[0] || !jnew->valuestring[0]) {
        cJSON_Delete(req);
        http_error(c, 400, "Missing 'old_password' and/or 'new_password'");
        return;
    }

    /* Verify old password */
    char old_hash[65];
    auth_hash_key(jold->valuestring, old_hash);
    if (db_check_user_password(auth_user_id, old_hash) != 0) {
        cJSON_Delete(req);
        http_error(c, 401, "Old password is incorrect");
        return;
    }

    /* Set new password */
    char new_hash[65];
    auth_hash_key(jnew->valuestring, new_hash);
    int rc = db_set_user_password(auth_user_id, new_hash);
    cJSON_Delete(req);

    if (rc != 0) {
        http_error(c, 500, "Failed to update password");
        return;
    }
    http_json_reply(c, 200, "{\"ok\":true}");
    log_info("auth", "User '%s' changed password", auth_user_id);
}

/* GET /auth/methods — public, returns available auth methods */
static void auth_methods(struct mg_connection *c, struct mg_http_message *hm)
{
    (void)hm;
    cJSON *resp = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    cJSON_AddItemToArray(arr, cJSON_CreateString("password"));
    cJSON_AddItemToArray(arr, cJSON_CreateString("api_key"));
    cJSON_AddItemToObject(resp, "methods", arr);
    char *s = cJSON_PrintUnformatted(resp);
    http_json_reply(c, 200, s);
    free(s);
    cJSON_Delete(resp);
}

static void submit_job(struct mg_connection *c, struct mg_http_message *hm,
                       const char *auth_user_id)
{
    char body[4096] = {0};
    size_t blen = hm->body.len < sizeof(body)-1 ? hm->body.len : sizeof(body)-1;
    memcpy(body, hm->body.buf, blen);

    cJSON *req = cJSON_Parse(body);
    if (!req) { http_error(c, 400, "Invalid JSON"); return; }

    cJSON *jcmd  = cJSON_GetObjectItemCaseSensitive(req, "command");
    cJSON *jpri  = cJSON_GetObjectItemCaseSensitive(req, "priority");
    cJSON *jcor  = cJSON_GetObjectItemCaseSensitive(req, "req_cores");
    cJSON *jgpu  = cJSON_GetObjectItemCaseSensitive(req, "req_gpu");
    cJSON *jram  = cJSON_GetObjectItemCaseSensitive(req, "req_ram_mb");
    cJSON *jdisk = cJSON_GetObjectItemCaseSensitive(req, "req_disk_mb");
    cJSON *jtout = cJSON_GetObjectItemCaseSensitive(req, "timeout_seconds");
    cJSON *juser = cJSON_GetObjectItemCaseSensitive(req, "user_id");
    cJSON *japp  = cJSON_GetObjectItemCaseSensitive(req, "app_id");
    if (!jcor)  jcor  = cJSON_GetObjectItemCaseSensitive(req, "cores");
    if (!jgpu)  jgpu  = cJSON_GetObjectItemCaseSensitive(req, "gpu");
    if (!jram)  jram  = cJSON_GetObjectItemCaseSensitive(req, "ram_mb");
    if (!jdisk) jdisk = cJSON_GetObjectItemCaseSensitive(req, "disk_mb");

    if (!cJSON_IsString(jcmd)) {
        cJSON_Delete(req);
        http_error(c, 400, "Missing 'command'");
        return;
    }

    char input_files_str[2048] = {0};
    cJSON *jfiles = cJSON_GetObjectItemCaseSensitive(req, "input_files");
    if (cJSON_IsArray(jfiles)) {
        cJSON *f;
        cJSON_ArrayForEach(f, jfiles) {
            if (cJSON_IsString(f) && f->valuestring[0]) {
                if (input_files_str[0])
                    strncat(input_files_str, ",", sizeof(input_files_str) - strlen(input_files_str) - 1);
                strncat(input_files_str, f->valuestring, sizeof(input_files_str) - strlen(input_files_str) - 1);
            }
        }
    }

    /* user_id: prefer the one bound to the API key; fall back to body */
    const char *user_id = (auth_user_id && auth_user_id[0])
                        ? auth_user_id
                        : (cJSON_IsString(juser) ? juser->valuestring : "");
    const char *app_id  = cJSON_IsString(japp)  ? japp->valuestring  : "";

    /* Quotas are enforced at dispatch time in the scheduler, not here.
       Jobs are always accepted and queued; the scheduler holds them back
       until the user/app is within quota limits. */

    Job *job = job_create_ex(
        jcmd->valuestring,
        cJSON_IsNumber(jpri)  ? (int)jpri->valuedouble  : 50,
        cJSON_IsNumber(jcor)  ? (int)jcor->valuedouble  : 1,
        cJSON_IsNumber(jgpu)  ? (int)jgpu->valuedouble  : 0,
        cJSON_IsNumber(jram)  ? (int)jram->valuedouble  : 0,
        cJSON_IsNumber(jdisk) ? (int)jdisk->valuedouble : 0,
        user_id, app_id
    );
    cJSON_Delete(req);
    if (!job) { http_error(c, 500, "Failed to create job"); return; }

    int is_held = (input_files_str[0] != '\0');

    /* Apply per-job timeout if provided */
    int job_timeout = cJSON_IsNumber(jtout) ? (int)jtout->valuedouble : 0;
    if (job_timeout > 0) {
        job->timeout_seconds = job_timeout;
        db_update_job_timeout(job->id, job_timeout);
    }

    if (is_held) {
        strncpy(job->input_files, input_files_str, sizeof(job->input_files) - 1);
        job->status = JOB_STATUS_HELD;
        db_update_job_status(job->id, JOB_STATUS_HELD, 0, 0);
        db_update_input_files(job->id, input_files_str);
        char held_reason[256];
        snprintf(held_reason, sizeof(held_reason),
            "Waiting for input files: %s", input_files_str);
        strncpy(job->status_reason, held_reason, sizeof(job->status_reason) - 1);
        db_update_status_reason(job->id, held_reason);
        store_init_job_dirs(job->id);
        log_info("routes", "Job %s held, expecting: %s", job->id, input_files_str);
    } else {
        queue_push(scheduler_queue(), job);
    }

    cJSON *resp = job_to_json(job);
    char *s = cJSON_PrintUnformatted(resp);
    http_json_reply(c, 201, s);
    free(s);
    cJSON_Delete(resp);
    if (is_held) job_free(job);
}

static void release_job(struct mg_connection *c, struct mg_http_message *hm,
                        const char *job_id)
{
    (void)hm;
    Job *job = db_get_job(job_id);
    if (!job) { http_error(c, 404, "Job not found"); return; }
    if (job->status != JOB_STATUS_HELD) {
        http_error(c, 409, "Job is not in HELD state");
        job_free(job);
        return;
    }
    job_set_status_r(job, JOB_STATUS_IN_QUEUE, "Released manually by user");
    queue_push(scheduler_queue(), job);
    cJSON *resp = job_to_json(job);
    char *s = cJSON_PrintUnformatted(resp);
    http_json_reply(c, 200, s);
    free(s);
    cJSON_Delete(resp);
}

static void list_jobs(struct mg_connection *c, struct mg_http_message *hm)
{
    (void)hm;
    Job *jobs = (Job *)malloc(256 * sizeof(Job));
    if (!jobs) { http_error(c, 500, "Out of memory"); return; }
    int count = db_list_jobs(jobs, 256);
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < count; i++) {
        cJSON *obj = job_to_json(&jobs[i]);
        cJSON_AddItemToArray(arr, obj);
    }
    free(jobs);
    char *s = cJSON_PrintUnformatted(arr);
    http_json_reply(c, 200, s);
    free(s);
    cJSON_Delete(arr);
}

static void get_job(struct mg_connection *c, struct mg_http_message *hm,
                    const char *job_id)
{
    (void)hm;
    Job *job = db_get_job(job_id);
    if (!job) { http_error(c, 404, "Job not found"); return; }
    cJSON *resp = job_to_json(job);
    char *s = cJSON_PrintUnformatted(resp);
    http_json_reply(c, 200, s);
    free(s);
    cJSON_Delete(resp);
    job_free(job);
}

static void cancel_job(struct mg_connection *c, struct mg_http_message *hm,
                        const char *job_id)
{
    (void)hm;
    Job *job = db_get_job(job_id);
    if (!job) { http_error(c, 404, "Job not found"); return; }
    if (job->status == JOB_STATUS_FINISHED ||
        job->status == JOB_STATUS_FAILED   ||
        job->status == JOB_STATUS_CANCELLED) {
        http_error(c, 409, "Job already in terminal state");
        job_free(job);
        return;
    }
    job_set_status_r(job, JOB_STATUS_CANCELLED, "Cancelled by user");
    alloc_release(job_id);
    cJSON *resp = job_to_json(job);
    char *s = cJSON_PrintUnformatted(resp);
    http_json_reply(c, 200, s);
    free(s);
    cJSON_Delete(resp);
    job_free(job);
}

static void purge_jobs(struct mg_connection *c, struct mg_http_message *hm)
{
    (void)hm;
    Job *jobs = (Job *)malloc(4096 * sizeof(Job));
    if (!jobs) { http_error(c, 500, "Out of memory"); return; }
    int count = db_list_jobs(jobs, 4096);
    int cleaned = 0;
    for (int i = 0; i < count; i++) {
        if (jobs[i].status == JOB_STATUS_FINISHED ||
            jobs[i].status == JOB_STATUS_FAILED   ||
            jobs[i].status == JOB_STATUS_CANCELLED) {
            store_cleanup_job(jobs[i].id);
            cleaned++;
        }
    }
    free(jobs);
    int deleted = db_purge_jobs();
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"deleted\":%d,\"cleaned\":%d}", deleted, cleaned);
    http_json_reply(c, 200, buf);
    log_info("routes", "Purged %d terminal jobs (%d work dirs cleaned)", deleted, cleaned);
}

static void check_and_auto_release(const char *job_id)
{
    Job *job = db_get_job(job_id);
    if (!job || job->status != JOB_STATUS_HELD || !job->input_files[0]) {
        job_free(job);
        return;
    }

    char input_dir[512];
    store_input_dir(job_id, input_dir, sizeof(input_dir));

    char files_copy[2048];
    strncpy(files_copy, job->input_files, sizeof(files_copy) - 1);

    int all_present = 1;
    char *tok = strtok(files_copy, ",");
    while (tok) {
        while (*tok == ' ') tok++;
        char path[768];
#ifdef _WIN32
        snprintf(path, sizeof(path), "%s\\%s", input_dir, tok);
#else
        snprintf(path, sizeof(path), "%s/%s", input_dir, tok);
#endif
        FILE *f = fopen(path, "rb");
        if (!f) { all_present = 0; break; }
        fclose(f);
        tok = strtok(NULL, ",");
    }

    if (all_present) {
        log_info("routes", "All input files received for job %s — releasing to queue", job_id);
        job_set_status_r(job, JOB_STATUS_IN_QUEUE, "All input files received via upload");
        queue_push(scheduler_queue(), job);
    } else {
        job_free(job);
    }
}

static void upload_input(struct mg_connection *c, struct mg_http_message *hm,
                          const char *job_id, const char *filename)
{
    long written = upload_handle(job_id, filename,
                                 hm->body.buf, (long)hm->body.len);
    if (written < 0) { http_error(c, 500, "Upload failed"); return; }
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"bytes\":%ld}", written);
    http_json_reply(c, 200, buf);
    check_and_auto_release(job_id);
}

static void download_output(struct mg_connection *c, struct mg_http_message *hm,
                             const char *job_id, const char *filename)
{
    if (download_handle(c, hm, job_id, filename) != 0)
        http_error(c, 404, "Output file not found");
}

static void get_job_log(struct mg_connection *c, struct mg_http_message *hm,
                        const char *job_id, int use_stderr)
{
    (void)hm;
    char path[512];
    if (use_stderr) store_stderr_path(job_id, path, sizeof(path));
    else            store_stdout_path(job_id, path, sizeof(path));

    FILE *f = fopen(path, "rb");
    if (!f) { http_error(c, 404, use_stderr ? "stderr.log not found" : "stdout.log not found"); return; }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);

    mg_printf(c,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain; charset=utf-8\r\n"
        "Content-Length: %ld\r\n"
        "\r\n", sz);

    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        mg_send(c, buf, n);
    fclose(f);
}

/* ── List job files (input + output) ─────────────────────────────── */
static void list_job_files(struct mg_connection *c, struct mg_http_message *hm,
                           const char *job_id)
{
    (void)hm;
    cJSON *root = cJSON_CreateObject();
    cJSON *in_arr  = cJSON_CreateArray();
    cJSON *out_arr = cJSON_CreateArray();

    char dir[512];

    /* Input files */
    store_input_dir(job_id, dir, sizeof(dir));
#ifdef _WIN32
    {
        char pattern[520];
        _snprintf(pattern, sizeof(pattern), "%s\\*", dir);
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(pattern, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (strcmp(fd.cFileName, ".") && strcmp(fd.cFileName, "..") &&
                    !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                    cJSON *fi = cJSON_CreateObject();
                    cJSON_AddStringToObject(fi, "name", fd.cFileName);
                    LARGE_INTEGER sz; sz.HighPart = fd.nFileSizeHigh; sz.LowPart = fd.nFileSizeLow;
                    cJSON_AddNumberToObject(fi, "size", (double)sz.QuadPart);
                    cJSON_AddItemToArray(in_arr, fi);
                }
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
    }
#else
    {
        DIR *d = opendir(dir);
        if (d) {
            struct dirent *ent;
            while ((ent = readdir(d))) {
                if (ent->d_name[0] == '.') continue;
                char fpath[768];
                snprintf(fpath, sizeof(fpath), "%s/%s", dir, ent->d_name);
                struct stat st;
                if (stat(fpath, &st) == 0 && S_ISREG(st.st_mode)) {
                    cJSON *fi = cJSON_CreateObject();
                    cJSON_AddStringToObject(fi, "name", ent->d_name);
                    cJSON_AddNumberToObject(fi, "size", (double)st.st_size);
                    cJSON_AddItemToArray(in_arr, fi);
                }
            }
            closedir(d);
        }
    }
#endif

    /* Output files */
    store_output_dir(job_id, dir, sizeof(dir));
#ifdef _WIN32
    {
        char pattern[520];
        _snprintf(pattern, sizeof(pattern), "%s\\*", dir);
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(pattern, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (strcmp(fd.cFileName, ".") && strcmp(fd.cFileName, "..") &&
                    !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                    cJSON *fi = cJSON_CreateObject();
                    cJSON_AddStringToObject(fi, "name", fd.cFileName);
                    LARGE_INTEGER sz; sz.HighPart = fd.nFileSizeHigh; sz.LowPart = fd.nFileSizeLow;
                    cJSON_AddNumberToObject(fi, "size", (double)sz.QuadPart);
                    cJSON_AddItemToArray(out_arr, fi);
                }
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
    }
#else
    {
        DIR *d = opendir(dir);
        if (d) {
            struct dirent *ent;
            while ((ent = readdir(d))) {
                if (ent->d_name[0] == '.') continue;
                char fpath[768];
                snprintf(fpath, sizeof(fpath), "%s/%s", dir, ent->d_name);
                struct stat st;
                if (stat(fpath, &st) == 0 && S_ISREG(st.st_mode)) {
                    cJSON *fi = cJSON_CreateObject();
                    cJSON_AddStringToObject(fi, "name", ent->d_name);
                    cJSON_AddNumberToObject(fi, "size", (double)st.st_size);
                    cJSON_AddItemToArray(out_arr, fi);
                }
            }
            closedir(d);
        }
    }
#endif

    /* Logs */
    cJSON *logs = cJSON_CreateObject();
    char logpath[512];
    store_stdout_path(job_id, logpath, sizeof(logpath));
    { FILE *f = fopen(logpath, "rb"); if (f) { fseek(f,0,SEEK_END); cJSON_AddNumberToObject(logs,"stdout_size",(double)ftell(f)); fclose(f); cJSON_AddBoolToObject(logs,"has_stdout",1); } else { cJSON_AddBoolToObject(logs,"has_stdout",0); } }
    store_stderr_path(job_id, logpath, sizeof(logpath));
    { FILE *f = fopen(logpath, "rb"); if (f) { fseek(f,0,SEEK_END); cJSON_AddNumberToObject(logs,"stderr_size",(double)ftell(f)); fclose(f); cJSON_AddBoolToObject(logs,"has_stderr",1); } else { cJSON_AddBoolToObject(logs,"has_stderr",0); } }

    cJSON_AddItemToObject(root, "input",  in_arr);
    cJSON_AddItemToObject(root, "output", out_arr);
    cJSON_AddItemToObject(root, "logs",   logs);

    char *s = cJSON_PrintUnformatted(root);
    http_json_reply(c, 200, s);
    free(s);
    cJSON_Delete(root);
}

static void sse_subscribe(struct mg_connection *c, struct mg_http_message *hm)
{
    (void)hm;
    mg_printf(c,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: keep-alive\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "\r\n");
    httpd_sse_add(c);

    Job *jobs = (Job *)malloc(256 * sizeof(Job));
    int count = jobs ? db_list_jobs(jobs, 256) : 0;
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < count; i++) cJSON_AddItemToArray(arr, job_to_json(&jobs[i]));
    free(jobs);
    char *s = cJSON_PrintUnformatted(arr);
    mg_printf(c, "event: snapshot\ndata: %s\n\n", s);
    free(s);
    cJSON_Delete(arr);
}

static void get_stats(struct mg_connection *c, struct mg_http_message *hm)
{
    (void)hm;
    JobStats js; memset(&js, 0, sizeof(js));
    db_job_stats(&js);

    int mcount;
    Machine *ms = registry_all(&mcount);
    int enabled = 0, cores_total = 0, cores_used = 0, ram_total = 0, ram_used = 0;
    for (int i = 0; i < mcount; i++) {
        if (ms[i].enabled) enabled++;
        cores_total += ms[i].cores_total;  cores_used += ms[i].cores_reserved;
        ram_total   += ms[i].ram_mb_total; ram_used   += ms[i].ram_mb_reserved;
    }

    cJSON *root = cJSON_CreateObject();

    cJSON *jobs = cJSON_CreateObject();
    cJSON_AddNumberToObject(jobs, "total",     js.total);
    cJSON_AddNumberToObject(jobs, "held",      js.held);
    cJSON_AddNumberToObject(jobs, "in_queue",  js.in_queue);
    cJSON_AddNumberToObject(jobs, "starting",  js.starting);
    cJSON_AddNumberToObject(jobs, "running",   js.running);
    cJSON_AddNumberToObject(jobs, "finished",  js.finished);
    cJSON_AddNumberToObject(jobs, "failed",    js.failed);
    cJSON_AddNumberToObject(jobs, "cancelled", js.cancelled);
    cJSON_AddItemToObject(root, "jobs", jobs);

    cJSON *machines = cJSON_CreateObject();
    cJSON_AddNumberToObject(machines, "total",   mcount);
    cJSON_AddNumberToObject(machines, "enabled", enabled);
    cJSON_AddItemToObject(root, "machines", machines);

    cJSON *resources = cJSON_CreateObject();
    cJSON_AddNumberToObject(resources, "cores_total",  cores_total);
    cJSON_AddNumberToObject(resources, "cores_used",   cores_used);
    cJSON_AddNumberToObject(resources, "ram_mb_total", ram_total);
    cJSON_AddNumberToObject(resources, "ram_mb_used",  ram_used);
    cJSON_AddItemToObject(root, "resources", resources);

    char *s = cJSON_PrintUnformatted(root);
    http_json_reply(c, 200, s);
    free(s);
    cJSON_Delete(root);
}

static void get_resources(struct mg_connection *c, struct mg_http_message *hm)
{
    (void)hm;
    int count;
    Machine *ms = registry_all(&count);
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < count; i++) {
        cJSON *m = cJSON_CreateObject();
        cJSON_AddStringToObject(m, "id",       ms[i].id);
        cJSON_AddStringToObject(m, "hostname", ms[i].hostname);
        cJSON_AddStringToObject(m, "ip",       ms[i].ip);
        cJSON_AddBoolToObject  (m, "enabled",  ms[i].enabled);
        cJSON_AddNumberToObject(m, "cores_total",     ms[i].cores_total);
        cJSON_AddNumberToObject(m, "cores_reserved",  ms[i].cores_reserved);
        cJSON_AddNumberToObject(m, "gpu_total",       ms[i].gpu_count_total);
        cJSON_AddNumberToObject(m, "gpu_reserved",    ms[i].gpu_count_reserved);
        cJSON_AddNumberToObject(m, "ram_mb_total",    ms[i].ram_mb_total);
        cJSON_AddNumberToObject(m, "ram_mb_reserved", ms[i].ram_mb_reserved);
        cJSON_AddNumberToObject(m, "disk_mb_total",   ms[i].disk_mb_total);
        cJSON_AddNumberToObject(m, "disk_mb_reserved",ms[i].disk_mb_reserved);
        cJSON_AddItemToArray(arr, m);
    }
    char *s = cJSON_PrintUnformatted(arr);
    http_json_reply(c, 200, s);
    free(s);
    cJSON_Delete(arr);
}

static void add_machine(struct mg_connection *c, struct mg_http_message *hm)
{
    char body[1024] = {0};
    size_t blen = hm->body.len < sizeof(body)-1 ? hm->body.len : sizeof(body)-1;
    memcpy(body, hm->body.buf, blen);

    cJSON *req = cJSON_Parse(body);
    if (!req) { http_error(c, 400, "Invalid JSON"); return; }

    Machine m; memset(&m, 0, sizeof(m));
    cJSON *j;
#define PICK_STR(f,k) if((j=cJSON_GetObjectItemCaseSensitive(req,k)) && cJSON_IsString(j)) strncpy(m.f, j->valuestring, sizeof(m.f)-1)
#define PICK_INT(f,k) if((j=cJSON_GetObjectItemCaseSensitive(req,k)) && cJSON_IsNumber(j)) m.f = (int)j->valuedouble
    PICK_STR(id,       "id");
    PICK_STR(hostname, "hostname");
    PICK_STR(ip,       "ip");
    PICK_INT(cores_total,     "cores");
    PICK_INT(gpu_count_total, "gpu_count");
    PICK_INT(ram_mb_total,    "ram_mb");
    PICK_INT(disk_mb_total,   "disk_mb");
    m.enabled = 1;
    j = cJSON_GetObjectItemCaseSensitive(req, "enabled");
    if (cJSON_IsBool(j)) m.enabled = cJSON_IsTrue(j) ? 1 : 0;

    cJSON_Delete(req);

    if (!m.id[0]) { http_error(c, 400, "Missing 'id'"); return; }
    registry_upsert(&m);
    http_json_reply(c, 201, "{\"ok\":true}");
}

static void remove_machine(struct mg_connection *c, struct mg_http_message *hm,
                            const char *machine_id)
{
    (void)hm;
    if (registry_remove(machine_id) != 0) {
        http_error(c, 404, "Machine not found");
        return;
    }
    http_json_reply(c, 200, "{\"ok\":true}");
}

/* ── Quota helpers ───────────────────────────────────────────────── */

static cJSON *quota_to_json(const Quota *q)
{
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "user_id",        q->user_id);
    cJSON_AddStringToObject(obj, "app_id",         q->app_id);
    cJSON_AddNumberToObject(obj, "max_jobs",       q->max_jobs);
    cJSON_AddNumberToObject(obj, "max_ram_mb",     q->max_ram_mb);
    cJSON_AddNumberToObject(obj, "max_cores",      q->max_cores);
    cJSON_AddNumberToObject(obj, "max_concurrent", q->max_concurrent);
    return obj;
}

static void list_quotas(struct mg_connection *c, struct mg_http_message *hm)
{
    (void)hm;
    Quota *qs = (Quota *)malloc(256 * sizeof(Quota));
    if (!qs) { http_error(c, 500, "Out of memory"); return; }
    int count = db_list_quotas(qs, 256);
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < count; i++)
        cJSON_AddItemToArray(arr, quota_to_json(&qs[i]));
    free(qs);
    char *s = cJSON_PrintUnformatted(arr);
    http_json_reply(c, 200, s);
    free(s);
    cJSON_Delete(arr);
}

static void upsert_quota(struct mg_connection *c, struct mg_http_message *hm)
{
    char body[2048] = {0};
    size_t blen = hm->body.len < sizeof(body)-1 ? hm->body.len : sizeof(body)-1;
    memcpy(body, hm->body.buf, blen);

    cJSON *req = cJSON_Parse(body);
    if (!req) { http_error(c, 400, "Invalid JSON"); return; }

    Quota q; memset(&q, 0, sizeof(q));
    cJSON *j;
    j = cJSON_GetObjectItemCaseSensitive(req, "user_id");
    if (cJSON_IsString(j)) strncpy(q.user_id, j->valuestring, sizeof(q.user_id)-1);
    j = cJSON_GetObjectItemCaseSensitive(req, "app_id");
    if (cJSON_IsString(j)) strncpy(q.app_id, j->valuestring, sizeof(q.app_id)-1);
    j = cJSON_GetObjectItemCaseSensitive(req, "max_jobs");
    if (cJSON_IsNumber(j)) q.max_jobs = (int)j->valuedouble;
    j = cJSON_GetObjectItemCaseSensitive(req, "max_ram_mb");
    if (cJSON_IsNumber(j)) q.max_ram_mb = (int)j->valuedouble;
    j = cJSON_GetObjectItemCaseSensitive(req, "max_cores");
    if (cJSON_IsNumber(j)) q.max_cores = (int)j->valuedouble;
    j = cJSON_GetObjectItemCaseSensitive(req, "max_concurrent");
    if (cJSON_IsNumber(j)) q.max_concurrent = (int)j->valuedouble;
    cJSON_Delete(req);

    /* Try update first; if no row matched, insert */
    int rc = db_update_quota(&q);
    if (rc != 0) rc = db_insert_quota(&q);

    if (rc != 0) {
        http_error(c, 500, "Failed to save quota");
        return;
    }

    cJSON *resp = quota_to_json(&q);
    char *s = cJSON_PrintUnformatted(resp);
    http_json_reply(c, 200, s);
    free(s);
    cJSON_Delete(resp);
    log_info("routes", "Quota upserted: user=%s app=%s max_jobs=%d max_cores=%d max_ram=%d max_concurrent=%d",
             q.user_id, q.app_id, q.max_jobs, q.max_cores, q.max_ram_mb, q.max_concurrent);
}

static void delete_quota(struct mg_connection *c, struct mg_http_message *hm)
{
    char body[1024] = {0};
    size_t blen = hm->body.len < sizeof(body)-1 ? hm->body.len : sizeof(body)-1;
    memcpy(body, hm->body.buf, blen);

    cJSON *req = cJSON_Parse(body);
    if (!req) { http_error(c, 400, "Invalid JSON"); return; }

    const char *uid = "", *aid = "";
    cJSON *j;
    j = cJSON_GetObjectItemCaseSensitive(req, "user_id");
    if (cJSON_IsString(j)) uid = j->valuestring;
    j = cJSON_GetObjectItemCaseSensitive(req, "app_id");
    if (cJSON_IsString(j)) aid = j->valuestring;

    int rc = db_delete_quota(uid, aid);
    cJSON_Delete(req);

    if (rc != 0) {
        http_error(c, 404, "Quota not found");
        return;
    }
    http_json_reply(c, 200, "{\"ok\":true}");
    log_info("routes", "Quota deleted: user=%s app=%s", uid, aid);
}

/* ── API Key management handlers ─────────────────────────────────── */

static void list_keys(struct mg_connection *c, struct mg_http_message *hm)
{
    (void)hm;
    ApiKeyInfo *keys = (ApiKeyInfo *)malloc(256 * sizeof(ApiKeyInfo));
    if (!keys) { http_error(c, 500, "Out of memory"); return; }
    int count = db_list_api_keys(keys, 256);
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < count; i++) {
        cJSON *obj = cJSON_CreateObject();
        /* Show only first 8 + last 4 chars of hash for security */
        char masked[20];
        snprintf(masked, sizeof(masked), "%.8s...%s",
                 keys[i].key_hash,
                 strlen(keys[i].key_hash) > 4
                     ? keys[i].key_hash + strlen(keys[i].key_hash) - 4
                     : keys[i].key_hash);
        cJSON_AddStringToObject(obj, "key_hash_masked", masked);
        cJSON_AddStringToObject(obj, "label",      keys[i].label);
        cJSON_AddStringToObject(obj, "role",       keys[i].role);
        cJSON_AddNumberToObject(obj, "created_at", (double)keys[i].created_at);
        cJSON_AddNumberToObject(obj, "expires_at", (double)keys[i].expires_at);
        cJSON_AddBoolToObject  (obj, "revoked",    keys[i].revoked);
        cJSON_AddItemToArray(arr, obj);
    }
    free(keys);
    char *s = cJSON_PrintUnformatted(arr);
    http_json_reply(c, 200, s);
    free(s);
    cJSON_Delete(arr);
}

static void create_key(struct mg_connection *c, struct mg_http_message *hm)
{
    char body[2048] = {0};
    size_t blen = hm->body.len < sizeof(body)-1 ? hm->body.len : sizeof(body)-1;
    memcpy(body, hm->body.buf, blen);

    cJSON *req = cJSON_Parse(body);
    if (!req) { http_error(c, 400, "Invalid JSON"); return; }

    const char *label = "default";
    const char *role  = "user";
    const char *user_id = "";
    time_t expires_at = 0;

    cJSON *j;
    j = cJSON_GetObjectItemCaseSensitive(req, "label");
    if (cJSON_IsString(j)) label = j->valuestring;
    j = cJSON_GetObjectItemCaseSensitive(req, "role");
    if (cJSON_IsString(j)) role = j->valuestring;
    j = cJSON_GetObjectItemCaseSensitive(req, "user_id");
    if (cJSON_IsString(j)) user_id = j->valuestring;
    j = cJSON_GetObjectItemCaseSensitive(req, "expires_at");
    if (cJSON_IsNumber(j)) expires_at = (time_t)j->valuedouble;

    /* Validate role */
    if (strcmp(role, "admin") != 0 && strcmp(role, "user") != 0) {
        cJSON_Delete(req);
        http_error(c, 400, "role must be 'admin' or 'user'");
        return;
    }

    /* Generate cryptographic random key */
    unsigned char raw[32];
#ifdef _WIN32
    {
        HCRYPTPROV hprov;
        CryptAcquireContextA(&hprov, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT);
        CryptGenRandom(hprov, sizeof(raw), raw);
        CryptReleaseContext(hprov, 0);
    }
#else
    {
        FILE *urnd = fopen("/dev/urandom", "rb");
        if (!urnd || fread(raw, 1, sizeof(raw), urnd) != sizeof(raw)) {
            if (urnd) fclose(urnd);
            cJSON_Delete(req);
            http_error(c, 500, "Cannot generate random key");
            return;
        }
        fclose(urnd);
    }
#endif
    char raw_hex[65] = {0};
    for (int i = 0; i < 32; i++) sprintf(raw_hex + i*2, "%02x", raw[i]);

    extern void auth_hash_key(const char *raw_key, char *out_hex_65);
    char hash[65];
    auth_hash_key(raw_hex, hash);

    int rc = db_insert_api_key_full(hash, label, role, user_id, expires_at);
    cJSON_Delete(req);

    if (rc != 0) {
        http_error(c, 500, "Failed to create key (duplicate?)");
        return;
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "api_key", raw_hex);
    cJSON_AddStringToObject(resp, "label",   label);
    cJSON_AddStringToObject(resp, "role",    role);
    cJSON_AddStringToObject(resp, "user_id", user_id);
    cJSON_AddNumberToObject(resp, "expires_at", (double)expires_at);
    char *s = cJSON_PrintUnformatted(resp);
    http_json_reply(c, 201, s);
    free(s);
    cJSON_Delete(resp);
    log_info("routes", "API key created: label=%s role=%s user_id=%s", label, role, user_id);
}

static void revoke_key(struct mg_connection *c, struct mg_http_message *hm)
{
    char body[1024] = {0};
    size_t blen = hm->body.len < sizeof(body)-1 ? hm->body.len : sizeof(body)-1;
    memcpy(body, hm->body.buf, blen);

    cJSON *req = cJSON_Parse(body);
    if (!req) { http_error(c, 400, "Invalid JSON"); return; }

    cJSON *jkey = cJSON_GetObjectItemCaseSensitive(req, "api_key");
    if (!cJSON_IsString(jkey) || !jkey->valuestring[0]) {
        cJSON_Delete(req);
        http_error(c, 400, "Missing 'api_key' field");
        return;
    }

    extern void auth_hash_key(const char *raw_key, char *out_hex_65);
    char hash[65];
    auth_hash_key(jkey->valuestring, hash);
    cJSON_Delete(req);

    int rc = db_revoke_api_key(hash);
    if (rc != 0) {
        http_error(c, 404, "Key not found");
        return;
    }
    http_json_reply(c, 200, "{\"ok\":true}");
    log_info("routes", "API key revoked (hash=%.8s...)", hash);
}

/* ── User management handlers ────────────────────────────────────── */

static cJSON *user_record_to_json(const UserRecord *u)
{
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "user_id",      u->user_id);
    cJSON_AddStringToObject(obj, "display_name", u->display_name);
    cJSON_AddStringToObject(obj, "email",        u->email);
    cJSON_AddBoolToObject  (obj, "enabled",      u->enabled);
    cJSON_AddNumberToObject(obj, "created_at",   (double)u->created_at);
    return obj;
}

static void list_users(struct mg_connection *c, struct mg_http_message *hm)
{
    (void)hm;
    /* Return user records enriched with job stats */
    UserRecord *recs = (UserRecord *)malloc(512 * sizeof(UserRecord));
    UserInfo   *info = (UserInfo *)malloc(512 * sizeof(UserInfo));
    if (!recs || !info) { free(recs); free(info); http_error(c, 500, "Out of memory"); return; }

    int n_recs = db_list_user_records(recs, 512);
    int n_info = db_list_users(info, 512);

    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < n_recs; i++) {
        cJSON *obj = user_record_to_json(&recs[i]);
        /* Find matching job stats */
        for (int j = 0; j < n_info; j++) {
            if (strcmp(info[j].user_id, recs[i].user_id) == 0) {
                cJSON_AddNumberToObject(obj, "total_jobs", info[j].total_jobs);
                cJSON_AddNumberToObject(obj, "running",    info[j].running);
                cJSON_AddNumberToObject(obj, "in_queue",   info[j].in_queue);
                cJSON_AddNumberToObject(obj, "held",       info[j].held);
                cJSON_AddNumberToObject(obj, "finished",   info[j].finished);
                cJSON_AddNumberToObject(obj, "failed",     info[j].failed);
                break;
            }
        }
        cJSON_AddItemToArray(arr, obj);
    }
    free(recs);
    free(info);
    char *s = cJSON_PrintUnformatted(arr);
    http_json_reply(c, 200, s);
    free(s);
    cJSON_Delete(arr);
}

/* POST /admin/users — create one or many users (batch).
   Body can be a single object or an array of objects. */
static void create_users(struct mg_connection *c, struct mg_http_message *hm)
{
    char *body = (char *)malloc(hm->body.len + 1);
    if (!body) { http_error(c, 500, "Out of memory"); return; }
    memcpy(body, hm->body.buf, hm->body.len);
    body[hm->body.len] = '\0';

    cJSON *req = cJSON_Parse(body);
    free(body);
    if (!req) { http_error(c, 400, "Invalid JSON"); return; }

    /* Normalize: wrap single object in an array */
    cJSON *list = req;
    int wrapped = 0;
    if (!cJSON_IsArray(req)) {
        list = cJSON_CreateArray();
        cJSON_AddItemToArray(list, cJSON_Duplicate(req, 1));
        cJSON_Delete(req);
        wrapped = 1;
    }

    int created = 0, failed = 0;
    cJSON *results = cJSON_CreateArray();
    cJSON *item;
    cJSON_ArrayForEach(item, list) {
        cJSON *juid = cJSON_GetObjectItemCaseSensitive(item, "user_id");
        if (!cJSON_IsString(juid) || !juid->valuestring[0]) {
            cJSON *r = cJSON_CreateObject();
            cJSON_AddStringToObject(r, "error", "missing user_id");
            cJSON_AddItemToArray(results, r);
            failed++;
            continue;
        }
        UserRecord u;
        memset(&u, 0, sizeof(u));
        strncpy(u.user_id, juid->valuestring, sizeof(u.user_id)-1);
        u.enabled = 1;

        cJSON *j;
        j = cJSON_GetObjectItemCaseSensitive(item, "display_name");
        if (cJSON_IsString(j)) strncpy(u.display_name, j->valuestring, sizeof(u.display_name)-1);
        j = cJSON_GetObjectItemCaseSensitive(item, "email");
        if (cJSON_IsString(j)) strncpy(u.email, j->valuestring, sizeof(u.email)-1);
        j = cJSON_GetObjectItemCaseSensitive(item, "enabled");
        if (cJSON_IsBool(j)) u.enabled = cJSON_IsTrue(j) ? 1 : 0;

        if (db_create_user(&u) == 0) {
            /* Set password if provided */
            cJSON *jpwd = cJSON_GetObjectItemCaseSensitive(item, "password");
            if (cJSON_IsString(jpwd) && jpwd->valuestring[0]) {
                char pwd_hash[65];
                auth_hash_key(jpwd->valuestring, pwd_hash);
                db_set_user_password(u.user_id, pwd_hash);
            }
            /* Re-read to get created_at */
            UserRecord saved;
            if (db_get_user(u.user_id, &saved) == 0)
                cJSON_AddItemToArray(results, user_record_to_json(&saved));
            else
                cJSON_AddItemToArray(results, user_record_to_json(&u));
            created++;
        } else {
            cJSON *r = cJSON_CreateObject();
            cJSON_AddStringToObject(r, "user_id", u.user_id);
            cJSON_AddStringToObject(r, "error", "already exists or DB error");
            cJSON_AddItemToArray(results, r);
            failed++;
        }
    }

    if (wrapped) cJSON_Delete(list);
    else         cJSON_Delete(list);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddNumberToObject(resp, "created", created);
    cJSON_AddNumberToObject(resp, "failed",  failed);
    cJSON_AddItemToObject(resp, "users", results);
    char *s = cJSON_PrintUnformatted(resp);
    http_json_reply(c, created > 0 ? 201 : 400, s);
    free(s);
    cJSON_Delete(resp);
    log_info("routes", "Batch create users: %d created, %d failed", created, failed);
}

/* PUT /admin/users — update a user */
static void update_user(struct mg_connection *c, struct mg_http_message *hm)
{
    char body[2048] = {0};
    size_t blen = hm->body.len < sizeof(body)-1 ? hm->body.len : sizeof(body)-1;
    memcpy(body, hm->body.buf, blen);

    cJSON *req = cJSON_Parse(body);
    if (!req) { http_error(c, 400, "Invalid JSON"); return; }

    cJSON *juid = cJSON_GetObjectItemCaseSensitive(req, "user_id");
    if (!cJSON_IsString(juid) || !juid->valuestring[0]) {
        cJSON_Delete(req);
        http_error(c, 400, "Missing 'user_id'");
        return;
    }

    /* Read existing record first so unset fields keep their value */
    UserRecord u;
    if (db_get_user(juid->valuestring, &u) != 0) {
        cJSON_Delete(req);
        http_error(c, 404, "User not found");
        return;
    }

    cJSON *j;
    j = cJSON_GetObjectItemCaseSensitive(req, "display_name");
    if (cJSON_IsString(j)) strncpy(u.display_name, j->valuestring, sizeof(u.display_name)-1);
    j = cJSON_GetObjectItemCaseSensitive(req, "email");
    if (cJSON_IsString(j)) strncpy(u.email, j->valuestring, sizeof(u.email)-1);
    j = cJSON_GetObjectItemCaseSensitive(req, "enabled");
    if (cJSON_IsBool(j)) u.enabled = cJSON_IsTrue(j) ? 1 : 0;
    cJSON_Delete(req);

    if (db_update_user(&u) != 0) {
        http_error(c, 500, "Failed to update user");
        return;
    }

    cJSON *resp = user_record_to_json(&u);
    char *s = cJSON_PrintUnformatted(resp);
    http_json_reply(c, 200, s);
    free(s);
    cJSON_Delete(resp);
    log_info("routes", "User updated: %s", u.user_id);
}

/* DELETE /admin/users — delete a user */
static void delete_user(struct mg_connection *c, struct mg_http_message *hm)
{
    char body[1024] = {0};
    size_t blen = hm->body.len < sizeof(body)-1 ? hm->body.len : sizeof(body)-1;
    memcpy(body, hm->body.buf, blen);

    cJSON *req = cJSON_Parse(body);
    if (!req) { http_error(c, 400, "Invalid JSON"); return; }

    cJSON *juid = cJSON_GetObjectItemCaseSensitive(req, "user_id");
    if (!cJSON_IsString(juid) || !juid->valuestring[0]) {
        cJSON_Delete(req);
        http_error(c, 400, "Missing 'user_id'");
        return;
    }

    const char *uid = juid->valuestring;
    int rc = db_delete_user(uid);
    cJSON_Delete(req);

    if (rc != 0) {
        http_error(c, 404, "User not found");
        return;
    }
    http_json_reply(c, 200, "{\"ok\":true}");
    log_info("routes", "User deleted: %s", uid);
}

/* ── Application definitions ─────────────────────────────────────── */

static void apps_dir_path(char *buf, int len)
{
    if (g_config.apps_dir[0] == '/' || g_config.apps_dir[0] == '\\'
        || (g_config.apps_dir[0] && g_config.apps_dir[1] == ':')) {
        strncpy(buf, g_config.apps_dir, len - 1);
    } else {
        char prefix[512] = {0};
        exe_relative_path(g_config.apps_dir, prefix, sizeof(prefix));
        strncpy(buf, prefix, len - 1);
    }
}

static void list_apps(struct mg_connection *c, struct mg_http_message *hm)
{
    (void)hm;
    char dir[512]; apps_dir_path(dir, sizeof(dir));

    cJSON *arr = cJSON_CreateArray();
#ifdef _WIN32
    {
        char pattern[520];
        _snprintf(pattern, sizeof(pattern), "%s\\*.json", dir);
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(pattern, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                char fpath[768];
                _snprintf(fpath, sizeof(fpath), "%s\\%s", dir, fd.cFileName);
                FILE *f = fopen(fpath, "rb");
                if (f) {
                    fseek(f, 0, SEEK_END); long sz = ftell(f); rewind(f);
                    char *data = (char *)malloc(sz + 1);
                    if (data) { fread(data, 1, sz, f); data[sz] = '\0';
                        cJSON *obj = cJSON_Parse(data);
                        if (obj) cJSON_AddItemToArray(arr, obj);
                        free(data);
                    }
                    fclose(f);
                }
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
    }
#else
    {
        DIR *d = opendir(dir);
        if (d) {
            struct dirent *ent;
            while ((ent = readdir(d))) {
                const char *ext = strrchr(ent->d_name, '.');
                if (!ext || strcmp(ext, ".json") != 0) continue;
                char fpath[768];
                snprintf(fpath, sizeof(fpath), "%s/%s", dir, ent->d_name);
                FILE *f = fopen(fpath, "rb");
                if (f) {
                    fseek(f, 0, SEEK_END); long sz = ftell(f); rewind(f);
                    char *data = (char *)malloc(sz + 1);
                    if (data) { fread(data, 1, sz, f); data[sz] = '\0';
                        cJSON *obj = cJSON_Parse(data);
                        if (obj) cJSON_AddItemToArray(arr, obj);
                        free(data);
                    }
                    fclose(f);
                }
            }
            closedir(d);
        }
    }
#endif
    char *s = cJSON_PrintUnformatted(arr);
    http_json_reply(c, 200, s);
    free(s);
    cJSON_Delete(arr);
}

static void get_app(struct mg_connection *c, struct mg_http_message *hm,
                    const char *app_id)
{
    (void)hm;
    /* Validate app_id: alphanumeric + underscore + dash only */
    for (const char *p = app_id; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '_' || *p == '-')) {
            http_error(c, 400, "Invalid app_id"); return;
        }
    }
    char dir[512]; apps_dir_path(dir, sizeof(dir));
    char fpath[768];
#ifdef _WIN32
    _snprintf(fpath, sizeof(fpath), "%s\\%s.json", dir, app_id);
#else
    snprintf(fpath, sizeof(fpath), "%s/%s.json", dir, app_id);
#endif
    FILE *f = fopen(fpath, "rb");
    if (!f) { http_error(c, 404, "App not found"); return; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); rewind(f);
    char *data = (char *)malloc(sz + 1);
    if (!data) { fclose(f); http_error(c, 500, "Out of memory"); return; }
    fread(data, 1, sz, f); fclose(f); data[sz] = '\0';
    http_json_reply(c, 200, data);
    free(data);
}

static void upsert_app(struct mg_connection *c, struct mg_http_message *hm)
{
    char body[8192] = {0};
    size_t blen = hm->body.len < sizeof(body)-1 ? hm->body.len : sizeof(body)-1;
    memcpy(body, hm->body.buf, blen);

    cJSON *req = cJSON_Parse(body);
    if (!req) { http_error(c, 400, "Invalid JSON"); return; }

    cJSON *jid = cJSON_GetObjectItemCaseSensitive(req, "app_id");
    if (!cJSON_IsString(jid) || !jid->valuestring[0]) {
        cJSON_Delete(req); http_error(c, 400, "Missing 'app_id'"); return;
    }
    /* Validate app_id */
    for (const char *p = jid->valuestring; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '_' || *p == '-')) {
            cJSON_Delete(req); http_error(c, 400, "Invalid app_id characters"); return;
        }
    }

    char dir[512]; apps_dir_path(dir, sizeof(dir));
    /* Ensure directory exists */
#ifdef _WIN32
    _mkdir(dir);
#else
    mkdir(dir, 0755);
#endif

    char fpath[768];
#ifdef _WIN32
    _snprintf(fpath, sizeof(fpath), "%s\\%s.json", dir, jid->valuestring);
#else
    snprintf(fpath, sizeof(fpath), "%s/%s.json", dir, jid->valuestring);
#endif
    char *pretty = cJSON_Print(req);
    cJSON_Delete(req);
    if (!pretty) { http_error(c, 500, "JSON format error"); return; }

    FILE *f = fopen(fpath, "wb");
    if (!f) { free(pretty); http_error(c, 500, "Cannot write app file"); return; }
    fwrite(pretty, 1, strlen(pretty), f);
    fclose(f);
    free(pretty);

    http_json_reply(c, 200, "{\"ok\":true}");
    log_info("routes", "App definition saved: %s", fpath);
}

static void delete_app(struct mg_connection *c, struct mg_http_message *hm,
                       const char *app_id)
{
    (void)hm;
    for (const char *p = app_id; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '_' || *p == '-')) {
            http_error(c, 400, "Invalid app_id"); return;
        }
    }
    char dir[512]; apps_dir_path(dir, sizeof(dir));
    char fpath[768];
#ifdef _WIN32
    _snprintf(fpath, sizeof(fpath), "%s\\%s.json", dir, app_id);
#else
    snprintf(fpath, sizeof(fpath), "%s/%s.json", dir, app_id);
#endif
    if (remove(fpath) != 0) { http_error(c, 404, "App not found"); return; }
    http_json_reply(c, 200, "{\"ok\":true}");
    log_info("routes", "App definition deleted: %s", app_id);
}

void routes_handler(struct mg_connection *c, int ev, void *ev_data)
{
    if (ev != MG_EV_HTTP_MSG) return;
    struct mg_http_message *hm = (struct mg_http_message *)ev_data;

    char uri[256] = {0};
    size_t ulen = hm->uri.len < sizeof(uri)-1 ? hm->uri.len : sizeof(uri)-1;
    memcpy(uri, hm->uri.buf, ulen);

    char method[16] = {0};
    size_t mlen = hm->method.len < sizeof(method)-1 ? hm->method.len : sizeof(method)-1;
    memcpy(method, hm->method.buf, mlen);

    char seg[5][128] = {{0}};
    for (int i = 0; i < 5; i++) extract_segment(uri, i, seg[i], 128);

    /* ── CORS preflight ────────────────────────────────────────── */
    if (strcmp(method, "OPTIONS") == 0) {
        mg_http_reply(c, 204,
            "Access-Control-Allow-Origin: *\r\n"
            "Access-Control-Allow-Methods: GET, POST, PUT, DELETE, OPTIONS\r\n"
            "Access-Control-Allow-Headers: Content-Type, X-API-Key\r\n"
            "Access-Control-Max-Age: 86400\r\n", "");
        return;
    }

    /* ── Web UI (public) ───────────────────────────────────────── */
    if (seg[0][0] == '\0' || strcmp(seg[0], "ui") == 0) {
        serve_web_ui(c); return;
    }
    /* ── Static web assets (public, no auth) ─────────────────── */
    if (strcmp(seg[0], "web") == 0 && strcmp(method, "GET") == 0 && seg[1][0]) {
        char rel[256] = {0};
        /* Build relative path from seg[1..4] */
        strncpy(rel, seg[1], sizeof(rel) - 1);
        for (int i = 2; i < 5 && seg[i][0]; i++) {
            strncat(rel, "/", sizeof(rel) - strlen(rel) - 1);
            strncat(rel, seg[i], sizeof(rel) - strlen(rel) - 1);
        }
        serve_web_file(c, rel); return;
    }

    /* ── Public auth routes (no API key required) ──────────────── */
    if (strcmp(seg[0], "auth") == 0) {
        if (strcmp(method, "POST") == 0 && strcmp(seg[1], "login") == 0) {
            auth_login(c, hm); return;
        }
        if (strcmp(method, "GET") == 0 && strcmp(seg[1], "methods") == 0) {
            auth_methods(c, hm); return;
        }
        /* change-password requires auth — handled below */
    }

    /* ── Authenticated routes ──────────────────────────────────── */
    char auth_role[16] = {0};
    char auth_user_id[128] = {0};
    if (!auth_check_role_user(c, hm, auth_role, auth_user_id)) {
        http_error(c, 401, "Unauthorized");
        return;
    }

    /* POST /auth/change-password (authenticated) */
    if (strcmp(seg[0], "auth") == 0) {
        if (strcmp(method, "POST") == 0 && strcmp(seg[1], "change-password") == 0) {
            auth_change_password(c, hm, auth_user_id); return;
        }
        http_error(c, 404, "Not found"); return;
    }

    /* ── Admin routes require role=admin ────────────────────────── */
    if (strcmp(seg[0], "admin") == 0) {
        if (strcmp(auth_role, "admin") != 0) {
            http_error(c, 403, "Forbidden: admin role required");
            return;
        }
    }

    /* /jobs */
    if (strcmp(seg[0], "jobs") == 0) {
        if (strcmp(method, "POST") == 0 && seg[1][0] == '\0') {
            submit_job(c, hm, auth_user_id); return;
        }
        if (strcmp(method, "GET") == 0 && seg[1][0] == '\0') {
            list_jobs(c, hm); return;
        }
        /* DELETE /jobs — purge all terminal jobs */
        if (strcmp(method, "DELETE") == 0 && seg[1][0] == '\0') {
            purge_jobs(c, hm); return;
        }
        if (strcmp(method, "GET") == 0 && strcmp(seg[1], "events") == 0) {
            sse_subscribe(c, hm); return;
        }
        if (seg[1][0] != '\0') {
            if (strcmp(method, "GET") == 0 && seg[2][0] == '\0') {
                get_job(c, hm, seg[1]); return;
            }
            if (strcmp(method, "DELETE") == 0 && seg[2][0] == '\0') {
                cancel_job(c, hm, seg[1]); return;
            }
            if (strcmp(method, "POST") == 0 && strcmp(seg[2], "release") == 0 && seg[3][0] == '\0') {
                release_job(c, hm, seg[1]); return;
            }
            if (strcmp(method, "POST") == 0 && strcmp(seg[2], "input") == 0 && seg[3][0]) {
                upload_input(c, hm, seg[1], seg[3]); return;
            }
            if (strcmp(method, "GET") == 0 && strcmp(seg[2], "output") == 0 && seg[3][0]) {
                download_output(c, hm, seg[1], seg[3]); return;
            }
            if (strcmp(method, "GET") == 0 && strcmp(seg[2], "files") == 0 && seg[3][0] == '\0') {
                list_job_files(c, hm, seg[1]); return;
            }
            if (strcmp(method, "GET") == 0 && strcmp(seg[2], "log") == 0) {
                get_job_log(c, hm, seg[1], strcmp(seg[3], "stderr") == 0); return;
            }
        }
    }

    /* /apps — available to all authenticated users */
    if (strcmp(seg[0], "apps") == 0) {
        if (strcmp(method, "GET") == 0 && seg[1][0] == '\0') {
            list_apps(c, hm); return;
        }
        if (strcmp(method, "GET") == 0 && seg[1][0] != '\0' && seg[2][0] == '\0') {
            get_app(c, hm, seg[1]); return;
        }
    }

    if (strcmp(seg[0], "resources") == 0 && strcmp(method, "GET") == 0) {
        get_resources(c, hm); return;
    }

    if (strcmp(seg[0], "stats") == 0 && strcmp(method, "GET") == 0) {
        get_stats(c, hm); return;
    }

    if (strcmp(seg[0], "provision") == 0) {
        if (strcmp(method, "POST") == 0 && seg[1][0] == '\0') {
            add_machine(c, hm); return;
        }
        if (strcmp(method, "DELETE") == 0 && seg[1][0] != '\0') {
            remove_machine(c, hm, seg[1]); return;
        }
    }

    /* /admin/quotas */
    if (strcmp(seg[0], "admin") == 0 && strcmp(seg[1], "quotas") == 0) {
        if (strcmp(method, "GET") == 0 && seg[2][0] == '\0') {
            list_quotas(c, hm); return;
        }
        if (strcmp(method, "POST") == 0 && seg[2][0] == '\0') {
            upsert_quota(c, hm); return;
        }
        if (strcmp(method, "PUT") == 0 && seg[2][0] == '\0') {
            upsert_quota(c, hm); return;
        }
        if (strcmp(method, "DELETE") == 0 && seg[2][0] == '\0') {
            delete_quota(c, hm); return;
        }
    }

    /* /admin/keys */
    if (strcmp(seg[0], "admin") == 0 && strcmp(seg[1], "keys") == 0) {
        if (strcmp(method, "GET") == 0 && seg[2][0] == '\0') {
            list_keys(c, hm); return;
        }
        if (strcmp(method, "POST") == 0 && seg[2][0] == '\0') {
            create_key(c, hm); return;
        }
        if (strcmp(method, "DELETE") == 0 && seg[2][0] == '\0') {
            revoke_key(c, hm); return;
        }
    }

    /* /admin/apps */
    if (strcmp(seg[0], "admin") == 0 && strcmp(seg[1], "apps") == 0) {
        if (strcmp(method, "POST") == 0 && seg[2][0] == '\0') {
            upsert_app(c, hm); return;
        }
        if (strcmp(method, "PUT") == 0 && seg[2][0] == '\0') {
            upsert_app(c, hm); return;
        }
        if (strcmp(method, "DELETE") == 0 && seg[2][0] != '\0') {
            delete_app(c, hm, seg[2]); return;
        }
    }

    /* /admin/users */
    if (strcmp(seg[0], "admin") == 0 && strcmp(seg[1], "users") == 0) {
        if (strcmp(method, "GET") == 0 && seg[2][0] == '\0') {
            list_users(c, hm); return;
        }
        if (strcmp(method, "POST") == 0 && seg[2][0] == '\0') {
            create_users(c, hm); return;
        }
        if (strcmp(method, "PUT") == 0 && seg[2][0] == '\0') {
            update_user(c, hm); return;
        }
        if (strcmp(method, "DELETE") == 0 && seg[2][0] == '\0') {
            delete_user(c, hm); return;
        }
    }

    http_error(c, 404, "Not found");
}
