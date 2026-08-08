#include "http.h"
#include "job.h"
#include "queue.h"
#include "scheduler.h"
#include "executor.h"
#include "resources.h"
#include "transfer.h"
#include "config.h"
#include "db.h"
#include "events.h"
#include "log.h"
#include "cJSON.h"
#include "mongoose.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <ctype.h>
#include <math.h>
#include <errno.h>
#include <limits.h>

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
 *   DELETE /jobs                      → purge_jobs  (delete all terminal jobs)
 *   GET    /jobs/events               → sse_subscribe  (Server-Sent Events)
 *   GET    /jobs/:id                  → get_job
 *   DELETE /jobs/:id                  → cancel_job
 *   POST   /jobs/:id/input/:filename  → upload_input
 *   GET    /jobs/:id/output/:filename → download_output
 *   GET    /jobs/:id/log              → get_job_log  (stdout.log)
 *   GET    /jobs/:id/log/stderr       → get_job_log_err (stderr.log)
 *   GET    /workers                   → list_workers
 *   GET    /queue                     → list_queue
 *   GET    /stats                     → get_stats
 *   POST   /provision                 → add_machine
 *   DELETE /provision/:id             → remove_machine
 */

/* ── Helpers ─────────────────────────────────────────────────────── */

/* Clamp a double to int within [lo, hi] */
static int s_accepting_jobs = 1;

static int clamp_int(double v, int lo, int hi)
{
    if (v < (double)lo) return lo;
    if (v > (double)hi) return hi;
    return (int)v;
}

static int parse_query_int(struct mg_http_message *hm, const char *name,
                           int default_value, int min_value, int max_value,
                           int *out_value)
{
    char value[64] = {0};
    int length = mg_http_get_var(&hm->query, name, value, sizeof(value));
    if (length <= 0) {
        *out_value = default_value;
        return 0;
    }
    char *end = NULL;
    errno = 0;
    long parsed = strtol(value, &end, 10);
    if (errno == ERANGE || end == value || *end != '\0' ||
        parsed < min_value || parsed > max_value || parsed > INT_MAX) {
        return -1;
    }
    *out_value = (int)parsed;
    return 0;
}

static int json_optional_int(cJSON *object, const char *name,
                             int min_value, int max_value, int *out_value)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!item) return 0;
    if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble) ||
        floor(item->valuedouble) != item->valuedouble ||
        item->valuedouble < min_value || item->valuedouble > max_value) {
        return -1;
    }
    *out_value = (int)item->valuedouble;
    return 1;
}

static int copy_optional_string(cJSON *object, const char *name,
                                char *out, size_t out_len)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!item) return 0;
    if (!cJSON_IsString(item) || strlen(item->valuestring) >= out_len) return -1;
    memcpy(out, item->valuestring, strlen(item->valuestring) + 1);
    return 1;
}

static int safe_cloud_arg(const char *value)
{
    if (!value || !value[0] || value[0] == '-') return 0;
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        if (!isalnum(*p) && !strchr("-_./:,=", *p)) return 0;
    }
    return 1;
}

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
    cJSON_AddStringToObject(obj, "depends_on",        job->depends_on);
    cJSON_AddStringToObject(obj, "workflow_id",       job->workflow_id);
    cJSON_AddStringToObject(obj, "batch_id",          job->batch_id);
    cJSON_AddStringToObject(obj, "same_machine_as",   job->same_machine_as);
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
    /* Reject path traversal: encoded dots, null bytes, absolute paths, backslashes */
    if (strstr(file_path, "..") ||
        strchr(file_path, '\\') || file_path[0] == '/' ||
        strstr(file_path, "%2e") || strstr(file_path, "%2E") ||
        strstr(file_path, "%00")) {
        http_error(c, 403, "Forbidden"); return;
    }

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
    if (sz < 0 || sz > 16 * 1024 * 1024) {
        fclose(f);
        http_error(c, 413, "Static file too large");
        return;
    }
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc(sz > 0 ? (size_t)sz : 1);
    if (!buf) { fclose(f); http_error(c, 500, "Out of memory"); return; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf);
        fclose(f);
        http_error(c, 500, "Failed to read static file");
        return;
    }
    fclose(f);

    char hdrs[1536] = {0};
    http_build_headers(hdrs, sizeof(hdrs), mime_for_ext(file_path));
    strncat(hdrs, "Cache-Control: no-cache\r\n", sizeof(hdrs) - strlen(hdrs) - 1);
    mg_printf(c, "HTTP/1.1 200 OK\r\n%sContent-Length: %ld\r\n\r\n", hdrs, sz);
    if (sz > 0) mg_send(c, buf, (size_t)sz);
    free(buf);
}

static void serve_web_ui(struct mg_connection *c)
{
    serve_web_file(c, "index.html");
}

/* ── Public auth handlers (no API key required) ──────────────────── */

static int safe_cli_name(const char *name);

static int generate_api_key_for_user(const char *user_id, const char *role,
                                     char *out_raw_hex, char *out_hash)
{
    unsigned char raw[32] = {0};
#ifdef _WIN32
    {
        HCRYPTPROV hprov = 0;
        if (!CryptAcquireContextA(&hprov, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT))
            return -1;
        if (!CryptGenRandom(hprov, sizeof(raw), raw)) {
            CryptReleaseContext(hprov, 0);
            return -1;
        }
        CryptReleaseContext(hprov, 0);
    }
#else
    {
        FILE *urnd = fopen("/dev/urandom", "rb");
        if (!urnd || fread(raw, 1, sizeof(raw), urnd) != sizeof(raw)) {
            if (urnd) fclose(urnd);
            return -1;
        }
        fclose(urnd);
    }
#endif
    for (int i = 0; i < 32; i++) sprintf(out_raw_hex + i*2, "%02x", raw[i]);
    out_raw_hex[64] = '\0';
    auth_hash_key(out_raw_hex, out_hash);
    time_t expires = time(NULL) + 86400; /* 24 hours */
    return db_insert_api_key_full(out_hash, "auto-login", role, user_id, expires);
}

/* POST /auth/login — public, no API key needed */
/* ── Login rate limiting ──────────────────────────────────────────── */
#define LOGIN_RL_SLOTS    64
#define LOGIN_RL_MAX      5     /* max attempts per window */
#define LOGIN_RL_WINDOW   60    /* seconds */

typedef struct { uint32_t ip; int attempts; time_t window_start; } LoginRL;
static LoginRL s_login_rl[LOGIN_RL_SLOTS];

static int login_rate_check(struct mg_connection *c)
{
    /* Use peer IPv4 address as rate-limit key */
    uint32_t ip = c->rem.addr.ip4;
    time_t now = time(NULL);

    for (int i = 0; i < LOGIN_RL_SLOTS; i++) {
        if (s_login_rl[i].ip == ip) {
            if (now - s_login_rl[i].window_start > LOGIN_RL_WINDOW) {
                s_login_rl[i].attempts = 1;
                s_login_rl[i].window_start = now;
                return 1;
            }
            s_login_rl[i].attempts++;
            return (s_login_rl[i].attempts <= LOGIN_RL_MAX) ? 1 : 0;
        }
    }
    /* Find empty or oldest slot */
    int oldest = 0;
    for (int i = 0; i < LOGIN_RL_SLOTS; i++) {
        if (s_login_rl[i].ip == 0) { oldest = i; break; }
        if (s_login_rl[i].window_start < s_login_rl[oldest].window_start) oldest = i;
    }
    s_login_rl[oldest].ip = ip;
    s_login_rl[oldest].attempts = 1;
    s_login_rl[oldest].window_start = now;
    return 1;
}

static void auth_login(struct mg_connection *c, struct mg_http_message *hm)
{
    if (!login_rate_check(c)) {
        http_error(c, 429, "Too many login attempts. Try again later.");
        return;
    }

    char body[2048] = {0};
    size_t blen = hm->body.len < sizeof(body)-1 ? hm->body.len : sizeof(body)-1;
    memcpy(body, hm->body.buf, blen);

    cJSON *req = cJSON_Parse(body);
    if (!req) { http_error(c, 400, "Invalid JSON"); return; }

    cJSON *juid = cJSON_GetObjectItemCaseSensitive(req, "user_id");
    cJSON *jpwd = cJSON_GetObjectItemCaseSensitive(req, "password");
    if (!cJSON_IsString(juid) || !cJSON_IsString(jpwd) ||
        !juid->valuestring[0] || !jpwd->valuestring[0] ||
        !safe_cli_name(juid->valuestring) || strlen(jpwd->valuestring) > 128) {
        cJSON_Delete(req);
        http_error(c, 400, "Invalid 'user_id' and/or 'password'");
        return;
    }

    /* Copy values before freeing JSON */
    char uid[128] = {0};
    strncpy(uid, juid->valuestring, sizeof(uid)-1);
    char password[256] = {0};
    strncpy(password, jpwd->valuestring, sizeof(password)-1);
    cJSON_Delete(req);

    /* Verify password (supports salted + legacy unsalted hashes) */
    char stored_hash[AUTH_PASSWORD_HASH_LEN] = {0};
    int enabled = 0;
    if (db_get_user_auth(uid, stored_hash, sizeof(stored_hash), &enabled) != 0 ||
        !auth_verify_password(password, stored_hash)) {
        http_error(c, 401, "Invalid user_id or password");
        return;
    }
    if (!enabled) {
        http_error(c, 403, "Account disabled");
        return;
    }
    if (auth_password_needs_rehash(stored_hash)) {
        char upgraded_hash[AUTH_PASSWORD_HASH_LEN];
        if (auth_hash_password(password, upgraded_hash) != 0 ||
            db_set_user_password(uid, upgraded_hash) != 0) {
            log_warn("auth", "Could not upgrade password hash for user '%s'", uid);
        }
    }

    /* Find existing valid key or generate a new one */
    char key_hash[65] = {0};
    char raw_hex[65] = {0};
    int had_previous_key = db_find_user_key(uid, key_hash, sizeof(key_hash));
    char hash[65];
    if (generate_api_key_for_user(uid, "user", raw_hex, hash) != 0) {
        http_error(c, 500, "Failed to create login session");
        return;
    }
    if (had_previous_key) db_revoke_api_key(key_hash);

    cJSON *resp = cJSON_CreateObject();
    if (!resp ||
        !cJSON_AddStringToObject(resp, "user_id", uid) ||
        !cJSON_AddStringToObject(resp, "api_key", raw_hex) ||
        !cJSON_AddStringToObject(resp, "role", "user")) {
        cJSON_Delete(resp);
        db_revoke_api_key(hash);
        http_error(c, 500, "Out of memory");
        return;
    }
    char *s = cJSON_PrintUnformatted(resp);
    if (!s) {
        cJSON_Delete(resp);
        db_revoke_api_key(hash);
        http_error(c, 500, "Out of memory");
        return;
    }
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
    char stored_hash[AUTH_PASSWORD_HASH_LEN] = {0};
    int _enabled = 0;
    if (db_get_user_auth(auth_user_id, stored_hash, sizeof(stored_hash), &_enabled) != 0 ||
        !auth_verify_password(jold->valuestring, stored_hash)) {
        cJSON_Delete(req);
        http_error(c, 401, "Old password is incorrect");
        return;
    }

    if (strlen(jnew->valuestring) < 12 || strlen(jnew->valuestring) > 128) {
        cJSON_Delete(req);
        http_error(c, 400, "New password must contain 12-128 characters");
        return;
    }
    char new_hash[AUTH_PASSWORD_HASH_LEN];
    if (auth_hash_password(jnew->valuestring, new_hash) != 0) {
        cJSON_Delete(req);
        http_error(c, 500, "Failed to generate password hash");
        return;
    }
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

static int job_status_from_text(const char *text)
{
    if (!text || !text[0]) return -1;
    if (strcmp(text, "IN_QUEUE") == 0 || strcmp(text, "QUEUED") == 0) return JOB_STATUS_QUEUED;
    if (strcmp(text, "STARTING") == 0) return JOB_STATUS_RUNNING;
    if (strcmp(text, "RUNNING") == 0) return JOB_STATUS_RUNNING;
    if (strcmp(text, "FINISHED") == 0 || strcmp(text, "SUCCEEDED") == 0) return JOB_STATUS_SUCCEEDED;
    if (strcmp(text, "CANCELLED") == 0) return JOB_STATUS_CANCELLED;
    if (strcmp(text, "FAILED") == 0) return JOB_STATUS_FAILED;
    if (strcmp(text, "HELD") == 0 || strcmp(text, "CREATED") == 0) return JOB_STATUS_CREATED;
    return -2;
}

static void auth_me(struct mg_connection *c, const char *user_id, const char *role)
{
    cJSON *resp = cJSON_CreateObject();
    if (!resp) { http_error(c, 500, "Out of memory"); return; }
    cJSON_AddStringToObject(resp, "user_id", user_id ? user_id : "");
    cJSON_AddStringToObject(resp, "role", role ? role : "user");
    char *serialized = cJSON_PrintUnformatted(resp);
    if (!serialized) {
        cJSON_Delete(resp);
        http_error(c, 500, "Out of memory");
        return;
    }
    http_json_reply(c, 200, serialized);
    free(serialized);
    cJSON_Delete(resp);
}

/* forward declaration — defined below with other app routes */
static void apps_dir_path(char *buf, int len);

/* ── App template resolution ─────────────────────────────────────── */

/*
 * Load an app JSON file by app_id, resolve command_template with user params.
 * Writes resolved command into out_cmd (max out_cmd_len).
 * Writes app env JSON string into out_env_json (caller must free).
 * Returns 0 on success, -1 on error (writes error into err_buf).
 */
typedef struct {
    int req_cores;
    int req_gpu;
    int req_ram_mb;
    int req_disk_mb;
} AppResources;

static void app_resources_defaults(AppResources *resources)
{
    resources->req_cores = 1;
    resources->req_gpu = 0;
    resources->req_ram_mb = 0;
    resources->req_disk_mb = 0;
}

static int safe_cli_name(const char *name)
{
    if (!name || !name[0] || strlen(name) > 64) return 0;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
        if (!isalnum(*p) && *p != '_' && *p != '-') return 0;
    }
    return 1;
}

static int safe_cli_value(const char *value)
{
    if (!value || strlen(value) > 256) return 0;
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        if (!isalnum(*p) && !strchr(" _-./:@+,=", *p)) return 0;
    }
    return 1;
}

static cJSON *find_app_field(cJSON *fields, const char *name)
{
    if (!cJSON_IsArray(fields)) return NULL;
    cJSON *field = NULL;
    cJSON_ArrayForEach(field, fields) {
        cJSON *field_name = cJSON_GetObjectItemCaseSensitive(field, "name");
        if (cJSON_IsString(field_name) && strcmp(field_name->valuestring, name) == 0)
            return field;
    }
    return NULL;
}

static int parameter_to_text(const char *name, cJSON *definition, cJSON *value,
                             char *out, size_t out_len,
                             char *err_buf, int err_len)
{
    cJSON *type_item = cJSON_GetObjectItemCaseSensitive(definition, "type");
    const char *type = cJSON_IsString(type_item) ? type_item->valuestring : "text";

    if (strcmp(type, "checkbox") == 0) {
        if (!cJSON_IsBool(value)) {
            snprintf(err_buf, err_len, "Parameter '%s' must be a boolean", name);
            return -1;
        }
        snprintf(out, out_len, "%s", cJSON_IsTrue(value) ? "true" : "false");
        return 0;
    }

    if (strcmp(type, "number") == 0) {
        if (!cJSON_IsNumber(value) || !isfinite(value->valuedouble)) {
            snprintf(err_buf, err_len, "Parameter '%s' must be a finite number", name);
            return -1;
        }
        cJSON *minimum = cJSON_GetObjectItemCaseSensitive(definition, "min");
        cJSON *maximum = cJSON_GetObjectItemCaseSensitive(definition, "max");
        if ((cJSON_IsNumber(minimum) && value->valuedouble < minimum->valuedouble) ||
            (cJSON_IsNumber(maximum) && value->valuedouble > maximum->valuedouble)) {
            snprintf(err_buf, err_len, "Parameter '%s' is outside its allowed range", name);
            return -1;
        }
        snprintf(out, out_len, "%.15g", value->valuedouble);
        return 0;
    }

    if (!cJSON_IsString(value)) {
        snprintf(err_buf, err_len, "Parameter '%s' must be a string", name);
        return -1;
    }
    if (!safe_cli_value(value->valuestring)) {
        snprintf(err_buf, err_len, "Parameter '%s' contains unsupported characters", name);
        return -1;
    }

    if (strcmp(type, "select") == 0) {
        cJSON *options = cJSON_GetObjectItemCaseSensitive(definition, "options");
        int matched = 0;
        if (!cJSON_IsArray(options)) {
            snprintf(err_buf, err_len, "Parameter '%s' has no valid option list", name);
            return -1;
        }
        cJSON *option = NULL;
        cJSON_ArrayForEach(option, options) {
            if (cJSON_IsString(option) && strcmp(option->valuestring, value->valuestring) == 0) {
                matched = 1;
                break;
            }
        }
        if (!matched) {
            snprintf(err_buf, err_len, "Parameter '%s' is not an allowed option", name);
            return -1;
        }
    } else if (strcmp(type, "text") != 0) {
        snprintf(err_buf, err_len, "Parameter '%s' has unsupported type '%s'", name, type);
        return -1;
    }

    cJSON *max_length = cJSON_GetObjectItemCaseSensitive(definition, "max_length");
    if (cJSON_IsNumber(max_length) && max_length->valuedouble >= 0 &&
        strlen(value->valuestring) > (size_t)max_length->valuedouble) {
        snprintf(err_buf, err_len, "Parameter '%s' is too long", name);
        return -1;
    }
    snprintf(out, out_len, "%s", value->valuestring);
    return 0;
}

static int read_app_resource(cJSON *app, const char *key, int fallback,
                             int minimum, int maximum, int *out,
                             char *err_buf, int err_len)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(app, key);
    if (!item) {
        *out = fallback;
        return 0;
    }
    if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble) ||
        item->valuedouble < minimum || item->valuedouble > maximum ||
        item->valuedouble != floor(item->valuedouble)) {
        snprintf(err_buf, err_len, "App resource '%s' is invalid", key);
        return -1;
    }
    *out = (int)item->valuedouble;
    return 0;
}

static int safe_display_text(const char *value, size_t maximum_length)
{
    if (!value || strlen(value) > maximum_length) return 0;
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        if (*p < 0x20 || *p == 0x7f) return 0;
    }
    return 1;
}

static int validate_app_parameter(const char *name, cJSON *definition,
                                  int require_type, char *err_buf, int err_len)
{
    if (!safe_cli_name(name) || !cJSON_IsObject(definition)) {
        snprintf(err_buf, err_len, "Invalid app parameter definition");
        return -1;
    }

    cJSON *type_item = cJSON_GetObjectItemCaseSensitive(definition, "type");
    if ((require_type && !cJSON_IsString(type_item)) ||
        (type_item && !cJSON_IsString(type_item))) {
        snprintf(err_buf, err_len, "Parameter '%s' must declare a valid type", name);
        return -1;
    }
    const char *type = cJSON_IsString(type_item) ? type_item->valuestring : "text";
    if (strcmp(type, "checkbox") != 0 && strcmp(type, "select") != 0 &&
        strcmp(type, "text") != 0 && strcmp(type, "number") != 0) {
        snprintf(err_buf, err_len, "Parameter '%s' has unsupported type '%s'", name, type);
        return -1;
    }

    cJSON *label = cJSON_GetObjectItemCaseSensitive(definition, "label");
    cJSON *placeholder = cJSON_GetObjectItemCaseSensitive(definition, "placeholder");
    if ((label && (!cJSON_IsString(label) ||
                   !safe_display_text(label->valuestring, 128))) ||
        (placeholder && (!cJSON_IsString(placeholder) ||
                         !safe_display_text(placeholder->valuestring, 256)))) {
        snprintf(err_buf, err_len, "Parameter '%s' has invalid display text", name);
        return -1;
    }

    cJSON *minimum = cJSON_GetObjectItemCaseSensitive(definition, "min");
    cJSON *maximum = cJSON_GetObjectItemCaseSensitive(definition, "max");
    if ((minimum && (!cJSON_IsNumber(minimum) || !isfinite(minimum->valuedouble))) ||
        (maximum && (!cJSON_IsNumber(maximum) || !isfinite(maximum->valuedouble))) ||
        (cJSON_IsNumber(minimum) && cJSON_IsNumber(maximum) &&
         minimum->valuedouble > maximum->valuedouble)) {
        snprintf(err_buf, err_len, "Parameter '%s' has an invalid numeric range", name);
        return -1;
    }

    cJSON *max_length = cJSON_GetObjectItemCaseSensitive(definition, "max_length");
    if (max_length && (!cJSON_IsNumber(max_length) ||
                       !isfinite(max_length->valuedouble) ||
                       floor(max_length->valuedouble) != max_length->valuedouble ||
                       max_length->valuedouble < 0 || max_length->valuedouble > 256)) {
        snprintf(err_buf, err_len, "Parameter '%s' has an invalid max_length", name);
        return -1;
    }

    if (strcmp(type, "select") == 0) {
        cJSON *options = cJSON_GetObjectItemCaseSensitive(definition, "options");
        int option_count = cJSON_IsArray(options) ? cJSON_GetArraySize(options) : 0;
        if (option_count < 1 || option_count > 100) {
            snprintf(err_buf, err_len, "Parameter '%s' must define 1-100 options", name);
            return -1;
        }
        cJSON *option = NULL;
        cJSON_ArrayForEach(option, options) {
            if (!cJSON_IsString(option) || !safe_cli_value(option->valuestring)) {
                snprintf(err_buf, err_len, "Parameter '%s' contains an invalid option", name);
                return -1;
            }
            for (cJSON *prior = options->child; prior && prior != option; prior = prior->next) {
                if (cJSON_IsString(prior) &&
                    strcmp(prior->valuestring, option->valuestring) == 0) {
                    snprintf(err_buf, err_len, "Parameter '%s' contains duplicate options", name);
                    return -1;
                }
            }
        }
    }

    cJSON *default_value = cJSON_GetObjectItemCaseSensitive(definition, "default");
    if (default_value) {
        char ignored[257];
        if (parameter_to_text(name, definition, default_value, ignored,
                              sizeof(ignored), err_buf, err_len) != 0) return -1;
    }
    return 0;
}

static int validate_app_definition(cJSON *app, char *err_buf, int err_len)
{
    if (!cJSON_IsObject(app)) {
        snprintf(err_buf, err_len, "App definition must be an object");
        return -1;
    }

    cJSON *app_id = cJSON_GetObjectItemCaseSensitive(app, "app_id");
    cJSON *name = cJSON_GetObjectItemCaseSensitive(app, "name");
    cJSON *command = cJSON_GetObjectItemCaseSensitive(app, "command_template");
    if (!cJSON_IsString(app_id) || !safe_cli_name(app_id->valuestring) ||
        !cJSON_IsString(name) || !name->valuestring[0] ||
        !safe_display_text(name->valuestring, 128) ||
        !cJSON_IsString(command) || !command->valuestring[0] ||
        strlen(command->valuestring) >= JOB_CMD_LEN ||
        strchr(command->valuestring, '\r') || strchr(command->valuestring, '\n')) {
        snprintf(err_buf, err_len, "App id, name or command template is invalid");
        return -1;
    }

    AppResources resources;
    if (read_app_resource(app, "req_cores", 1, 1, 10000,
                          &resources.req_cores, err_buf, err_len) != 0 ||
        read_app_resource(app, "req_gpu", 0, 0, 1000,
                          &resources.req_gpu, err_buf, err_len) != 0 ||
        read_app_resource(app, "req_ram_mb", 0, 0, 10000000,
                          &resources.req_ram_mb, err_buf, err_len) != 0 ||
        read_app_resource(app, "req_disk_mb", 0, 0, 10000000,
                          &resources.req_disk_mb, err_buf, err_len) != 0) return -1;

    cJSON *parameters = cJSON_GetObjectItemCaseSensitive(app, "parameters");
    cJSON *fields = cJSON_GetObjectItemCaseSensitive(app, "fields");
    if ((parameters && !cJSON_IsObject(parameters)) ||
        (fields && !cJSON_IsArray(fields)) ||
        (parameters && cJSON_GetArraySize(parameters) > 64) ||
        (fields && cJSON_GetArraySize(fields) > 64)) {
        snprintf(err_buf, err_len, "App fields or parameters are invalid");
        return -1;
    }

    if (parameters) {
        cJSON *definition = NULL;
        cJSON_ArrayForEach(definition, parameters) {
            if (validate_app_parameter(definition->string, definition, 0,
                                       err_buf, err_len) != 0) return -1;
            for (cJSON *prior = parameters->child; prior && prior != definition;
                 prior = prior->next) {
                if (prior->string && strcmp(prior->string, definition->string) == 0) {
                    snprintf(err_buf, err_len, "Duplicate parameter '%s'", definition->string);
                    return -1;
                }
            }
        }
    }

    if (fields) {
        cJSON *field = NULL;
        cJSON_ArrayForEach(field, fields) {
            cJSON *field_name = cJSON_GetObjectItemCaseSensitive(field, "name");
            if (!cJSON_IsString(field_name) ||
                validate_app_parameter(field_name->valuestring, field, 1,
                                       err_buf, err_len) != 0) return -1;
            if (parameters &&
                cJSON_GetObjectItemCaseSensitive(parameters, field_name->valuestring)) {
                snprintf(err_buf, err_len, "Duplicate parameter '%s'", field_name->valuestring);
                return -1;
            }
            for (cJSON *prior = fields->child; prior && prior != field; prior = prior->next) {
                cJSON *prior_name = cJSON_GetObjectItemCaseSensitive(prior, "name");
                if (cJSON_IsString(prior_name) &&
                    strcmp(prior_name->valuestring, field_name->valuestring) == 0) {
                    snprintf(err_buf, err_len, "Duplicate field '%s'", field_name->valuestring);
                    return -1;
                }
            }
        }
    }

    cJSON *env = cJSON_GetObjectItemCaseSensitive(app, "env");
    if (env) {
        if (!cJSON_IsObject(env) || cJSON_GetArraySize(env) > 64) {
            snprintf(err_buf, err_len, "App environment is invalid");
            return -1;
        }
        cJSON *entry = NULL;
        cJSON_ArrayForEach(entry, env) {
            if (!safe_cli_name(entry->string) || !cJSON_IsString(entry) ||
                strlen(entry->valuestring) > 1024 ||
                strchr(entry->valuestring, '\r') || strchr(entry->valuestring, '\n')) {
                snprintf(err_buf, err_len, "App environment contains an invalid entry");
                return -1;
            }
        }
    }
    return 0;
}

static cJSON *load_app_definition(const char *app_id, char *err_buf, int err_len)
{
    if (!safe_cli_name(app_id)) {
        snprintf(err_buf, err_len, "Invalid app_id characters");
        return NULL;
    }
    char dir[512];
    char path[768];
    apps_dir_path(dir, sizeof(dir));
#ifdef _WIN32
    _snprintf(path, sizeof(path), "%s\\%s.json", dir, app_id);
#else
    snprintf(path, sizeof(path), "%s/%s.json", dir, app_id);
#endif
    FILE *file = fopen(path, "rb");
    if (!file) {
        snprintf(err_buf, err_len, "App not found: %s", app_id);
        return NULL;
    }
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    rewind(file);
    if (size <= 0 || size > 65536) {
        fclose(file);
        snprintf(err_buf, err_len, "App file invalid or too large");
        return NULL;
    }
    char *data = (char *)malloc((size_t)size + 1);
    if (!data || fread(data, 1, (size_t)size, file) != (size_t)size) {
        free(data);
        fclose(file);
        snprintf(err_buf, err_len, "Failed to read app definition");
        return NULL;
    }
    fclose(file);
    data[size] = '\0';
    cJSON *app = cJSON_Parse(data);
    free(data);
    if (!app) {
        snprintf(err_buf, err_len, "App JSON parse error");
        return NULL;
    }
    if (validate_app_definition(app, err_buf, err_len) != 0) {
        cJSON_Delete(app);
        return NULL;
    }
    cJSON *stored_id = cJSON_GetObjectItemCaseSensitive(app, "app_id");
    if (!cJSON_IsString(stored_id) || strcmp(stored_id->valuestring, app_id) != 0) {
        cJSON_Delete(app);
        snprintf(err_buf, err_len, "App id does not match its filename");
        return NULL;
    }
    return app;
}

static int resolve_app_command_legacy(const char *app_id, cJSON *user_params,
                               char *out_cmd, int out_cmd_len,
                               char **out_env_json,
                               char *err_buf, int err_len)
{
    *out_env_json = NULL;

    /* Validate app_id characters */
    for (const char *p = app_id; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '_' || *p == '-')) {
            snprintf(err_buf, err_len, "Invalid app_id characters");
            return -1;
        }
    }

    /* Load app JSON */
    char dir[512]; apps_dir_path(dir, sizeof(dir));
    char fpath[768];
#ifdef _WIN32
    _snprintf(fpath, sizeof(fpath), "%s\\%s.json", dir, app_id);
#else
    snprintf(fpath, sizeof(fpath), "%s/%s.json", dir, app_id);
#endif
    FILE *f = fopen(fpath, "rb");
    if (!f) {
        snprintf(err_buf, err_len, "App not found: %s", app_id);
        return -1;
    }
    fseek(f, 0, SEEK_END); long sz = ftell(f); rewind(f);
    if (sz <= 0 || sz > 65536) {
        fclose(f);
        snprintf(err_buf, err_len, "App file invalid or too large");
        return -1;
    }
    char *data = (char *)malloc(sz + 1);
    if (!data || fread(data, 1, sz, f) != (size_t)sz) {
        free(data);
        fclose(f);
        snprintf(err_buf, err_len, "Failed to read app definition");
        return -1;
    }
    fclose(f); data[sz] = '\0';

    cJSON *app = cJSON_Parse(data);
    free(data);
    if (!app) {
        snprintf(err_buf, err_len, "App JSON parse error");
        return -1;
    }

    cJSON *jtpl = cJSON_GetObjectItemCaseSensitive(app, "command_template");
    if (!cJSON_IsString(jtpl) || !jtpl->valuestring[0]) {
        cJSON_Delete(app);
        snprintf(err_buf, err_len, "App missing 'command_template'");
        return -1;
    }

    /* Start with the template */
    char resolved[JOB_CMD_LEN];
    strncpy(resolved, jtpl->valuestring, sizeof(resolved) - 1);
    resolved[sizeof(resolved) - 1] = '\0';

    /* Substitute {{key}} placeholders with user parameter values */
    cJSON *app_params = cJSON_GetObjectItemCaseSensitive(app, "parameters");
    cJSON *jfields = cJSON_GetObjectItemCaseSensitive(app, "fields");
    if (user_params && cJSON_IsObject(user_params)) {
        cJSON *p = NULL;
        cJSON_ArrayForEach(p, user_params) {
            if (!cJSON_IsString(p)) continue;
            const char *val = p->valuestring;

            /* Validate parameter value: reject shell metacharacters */
            static const char *forbidden = ";|&`$(){}\\<>!\n\r";
            for (const char *v = val; *v; v++) {
                if (strchr(forbidden, *v)) {
                    cJSON_Delete(app);
                    snprintf(err_buf, err_len,
                        "Forbidden character in parameter '%s'", p->string);
                    return -1;
                }
            }

            /* Check if parameter is declared in app definition */
            if (app_params) {
                cJSON *decl = cJSON_GetObjectItemCaseSensitive(app_params, p->string);
                if (!decl && !find_app_field(jfields, p->string)) {
                    cJSON_Delete(app);
                    snprintf(err_buf, err_len,
                        "Unknown parameter '%s' for app '%s'", p->string, app_id);
                    return -1;
                }
                /* Validate against pattern if declared */
                /* (pattern regex validation is optional — skipped for now) */
            }

            /* Replace all occurrences of {{key}} */
            char placeholder[140];
            snprintf(placeholder, sizeof(placeholder), "{{%s}}", p->string);
            char tmp[JOB_CMD_LEN];
            char *pos;
            while ((pos = strstr(resolved, placeholder)) != NULL) {
                int before_len = (int)(pos - resolved);
                tmp[0] = '\0';
                if (before_len > 0) {
                    memcpy(tmp, resolved, before_len);
                    tmp[before_len] = '\0';
                }
                strncat(tmp, val, sizeof(tmp) - strlen(tmp) - 1);
                strncat(tmp, pos + strlen(placeholder),
                        sizeof(tmp) - strlen(tmp) - 1);
                strncpy(resolved, tmp, sizeof(resolved) - 1);
                resolved[sizeof(resolved) - 1] = '\0';
            }
        }
    }

    /* Fill any remaining {{key}} with defaults from app definition */
    if (app_params && cJSON_IsObject(app_params)) {
        cJSON *decl = NULL;
        cJSON_ArrayForEach(decl, app_params) {
            char placeholder[140];
            snprintf(placeholder, sizeof(placeholder), "{{%s}}", decl->string);
            if (!strstr(resolved, placeholder)) continue;

            cJSON *jdef = cJSON_GetObjectItemCaseSensitive(decl, "default");
            if (!cJSON_IsString(jdef)) {
                cJSON_Delete(app);
                snprintf(err_buf, err_len,
                    "Required parameter '%s' not provided", decl->string);
                return -1;
            }
            char tmp[JOB_CMD_LEN];
            char *pos;
            while ((pos = strstr(resolved, placeholder)) != NULL) {
                int before_len = (int)(pos - resolved);
                tmp[0] = '\0';
                if (before_len > 0) {
                    memcpy(tmp, resolved, before_len);
                    tmp[before_len] = '\0';
                }
                strncat(tmp, jdef->valuestring,
                        sizeof(tmp) - strlen(tmp) - 1);
                strncat(tmp, pos + strlen(placeholder),
                        sizeof(tmp) - strlen(tmp) - 1);
                strncpy(resolved, tmp, sizeof(resolved) - 1);
                resolved[sizeof(resolved) - 1] = '\0';
            }
        }
    }

    /* Append CLI flags from app "fields" for fields not handled by {{key}} */
    if (jfields && cJSON_IsArray(jfields)) {
        cJSON *field = NULL;
        cJSON_ArrayForEach(field, jfields) {
            cJSON *jfname = cJSON_GetObjectItemCaseSensitive(field, "name");
            cJSON *jftype = cJSON_GetObjectItemCaseSensitive(field, "type");
            if (!cJSON_IsString(jfname) || !cJSON_IsString(jftype)) continue;

            const char *fname = jfname->valuestring;
            const char *ftype = jftype->valuestring;

            /* Skip fields handled by {{key}} placeholder substitution */
            char ph[140];
            snprintf(ph, sizeof(ph), "{{%s}}", fname);
            if (strstr(jtpl->valuestring, ph)) continue;

            /* Get user-supplied value, or fall back to field default */
            cJSON *pval = user_params
                ? cJSON_GetObjectItemCaseSensitive(user_params, fname)
                : NULL;

            if (strcmp(ftype, "checkbox") == 0) {
                int is_true = 0;
                if (pval) {
                    if (cJSON_IsTrue(pval)) is_true = 1;
                    else if (cJSON_IsString(pval) &&
                             strcmp(pval->valuestring, "true") == 0) is_true = 1;
                } else {
                    cJSON *jdef = cJSON_GetObjectItemCaseSensitive(field, "default");
                    if (cJSON_IsTrue(jdef)) is_true = 1;
                }
                if (is_true) {
                    strncat(resolved, " --", sizeof(resolved) - strlen(resolved) - 1);
                    strncat(resolved, fname, sizeof(resolved) - strlen(resolved) - 1);
                }
            } else {
                const char *val = NULL;
                if (pval && cJSON_IsString(pval)) val = pval->valuestring;
                else if (!pval) {
                    cJSON *jdef = cJSON_GetObjectItemCaseSensitive(field, "default");
                    if (cJSON_IsString(jdef)) val = jdef->valuestring;
                }
                if (val && val[0]) {
                    strncat(resolved, " --", sizeof(resolved) - strlen(resolved) - 1);
                    strncat(resolved, fname, sizeof(resolved) - strlen(resolved) - 1);
                    strncat(resolved, " ", sizeof(resolved) - strlen(resolved) - 1);
                    strncat(resolved, val, sizeof(resolved) - strlen(resolved) - 1);
                }
            }
        }
    }

    strncpy(out_cmd, resolved, out_cmd_len - 1);
    out_cmd[out_cmd_len - 1] = '\0';

    /* Extract env block — serialize to JSON string for executor */
    cJSON *jenv = cJSON_GetObjectItemCaseSensitive(app, "env");
    if (jenv && cJSON_IsObject(jenv)) {
        *out_env_json = cJSON_PrintUnformatted(jenv);
    }

    cJSON_Delete(app);
    return 0;
}

static int resolve_app_command(const char *app_id, cJSON *user_params,
                               char *out_cmd, int out_cmd_len,
                               char **out_env_json,
                               AppResources *out_resources,
                               char *err_buf, int err_len)
{
    if (err_len > 0) err_buf[0] = '\0';
    *out_env_json = NULL;
    if (out_resources) app_resources_defaults(out_resources);
    if (user_params && !cJSON_IsObject(user_params)) {
        snprintf(err_buf, err_len, "'parameters' must be an object");
        return -1;
    }

    cJSON *app = load_app_definition(app_id, err_buf, err_len);
    if (!app) return -1;
    cJSON *template_item = cJSON_GetObjectItemCaseSensitive(app, "command_template");
    cJSON *app_params = cJSON_GetObjectItemCaseSensitive(app, "parameters");
    cJSON *fields = cJSON_GetObjectItemCaseSensitive(app, "fields");
    if (!cJSON_IsString(template_item) || !template_item->valuestring[0] ||
        (app_params && !cJSON_IsObject(app_params)) ||
        (fields && !cJSON_IsArray(fields))) {
        cJSON_Delete(app);
        snprintf(err_buf, err_len, "Invalid app definition");
        return -1;
    }

    AppResources resources;
    app_resources_defaults(&resources);
    if (read_app_resource(app, "req_cores", 1, 1, 10000,
                          &resources.req_cores, err_buf, err_len) != 0 ||
        read_app_resource(app, "req_gpu", 0, 0, 1000,
                          &resources.req_gpu, err_buf, err_len) != 0 ||
        read_app_resource(app, "req_ram_mb", 0, 0, 10000000,
                          &resources.req_ram_mb, err_buf, err_len) != 0 ||
        read_app_resource(app, "req_disk_mb", 0, 0, 10000000,
                          &resources.req_disk_mb, err_buf, err_len) != 0) {
        cJSON_Delete(app);
        return -1;
    }

    if (user_params) {
        cJSON *supplied = NULL;
        cJSON_ArrayForEach(supplied, user_params) {
            cJSON *definition = find_app_field(fields, supplied->string);
            if (!definition && app_params)
                definition = cJSON_GetObjectItemCaseSensitive(app_params, supplied->string);
            if (!safe_cli_name(supplied->string) || !cJSON_IsObject(definition)) {
                cJSON_Delete(app);
                snprintf(err_buf, err_len, "Unknown parameter '%s' for app '%s'",
                         supplied->string ? supplied->string : "", app_id);
                return -1;
            }
            char ignored[257];
            if (parameter_to_text(supplied->string, definition, supplied,
                                  ignored, sizeof(ignored), err_buf, err_len) != 0) {
                cJSON_Delete(app);
                return -1;
            }
        }
    }

    cJSON *normalized = cJSON_CreateObject();
    if (!normalized) {
        cJSON_Delete(app);
        snprintf(err_buf, err_len, "Out of memory");
        return -1;
    }

    if (app_params) {
        cJSON *definition = NULL;
        cJSON_ArrayForEach(definition, app_params) {
            if (!safe_cli_name(definition->string) || !cJSON_IsObject(definition)) {
                cJSON_Delete(normalized);
                cJSON_Delete(app);
                snprintf(err_buf, err_len, "Invalid app parameter definition");
                return -1;
            }
            cJSON *value = user_params
                         ? cJSON_GetObjectItemCaseSensitive(user_params, definition->string) : NULL;
            if (!value) value = cJSON_GetObjectItemCaseSensitive(definition, "default");
            if (!value) {
                char placeholder[140];
                snprintf(placeholder, sizeof(placeholder), "{{%s}}", definition->string);
                if (strstr(template_item->valuestring, placeholder)) {
                    cJSON_Delete(normalized);
                    cJSON_Delete(app);
                    snprintf(err_buf, err_len, "Required parameter '%s' not provided", definition->string);
                    return -1;
                }
                continue;
            }
            char value_text[257];
            if (parameter_to_text(definition->string, definition, value,
                                  value_text, sizeof(value_text), err_buf, err_len) != 0 ||
                !cJSON_AddStringToObject(normalized, definition->string, value_text)) {
                if (!err_buf[0]) snprintf(err_buf, err_len, "Out of memory");
                cJSON_Delete(normalized);
                cJSON_Delete(app);
                return -1;
            }
        }
    }

    if (fields) {
        cJSON *field = NULL;
        cJSON_ArrayForEach(field, fields) {
            cJSON *name_item = cJSON_GetObjectItemCaseSensitive(field, "name");
            cJSON *type_item = cJSON_GetObjectItemCaseSensitive(field, "type");
            if (!cJSON_IsString(name_item) || !safe_cli_name(name_item->valuestring) ||
                !cJSON_IsString(type_item)) {
                cJSON_Delete(normalized);
                cJSON_Delete(app);
                snprintf(err_buf, err_len, "Invalid app field definition");
                return -1;
            }
            if (cJSON_GetObjectItemCaseSensitive(normalized, name_item->valuestring)) continue;
            cJSON *value = user_params
                         ? cJSON_GetObjectItemCaseSensitive(user_params, name_item->valuestring) : NULL;
            if (!value) value = cJSON_GetObjectItemCaseSensitive(field, "default");
            char value_text[257];
            if (!value) {
                if (strcmp(type_item->valuestring, "checkbox") == 0) {
                    snprintf(value_text, sizeof(value_text), "false");
                } else {
                    char placeholder[140];
                    snprintf(placeholder, sizeof(placeholder), "{{%s}}", name_item->valuestring);
                    if (strstr(template_item->valuestring, placeholder)) {
                        cJSON_Delete(normalized);
                        cJSON_Delete(app);
                        snprintf(err_buf, err_len, "Required parameter '%s' not provided", name_item->valuestring);
                        return -1;
                    }
                    continue;
                }
            } else if (parameter_to_text(name_item->valuestring, field, value,
                                         value_text, sizeof(value_text), err_buf, err_len) != 0) {
                cJSON_Delete(normalized);
                cJSON_Delete(app);
                return -1;
            }
            if (!cJSON_AddStringToObject(normalized, name_item->valuestring, value_text)) {
                cJSON_Delete(normalized);
                cJSON_Delete(app);
                snprintf(err_buf, err_len, "Out of memory");
                return -1;
            }
        }
    }

    cJSON_Delete(app);
    int result = resolve_app_command_legacy(app_id, normalized,
                                            out_cmd, out_cmd_len,
                                            out_env_json, err_buf, err_len);
    cJSON_Delete(normalized);
    if (result != 0) return result;
    if (strstr(out_cmd, "{{") || strstr(out_cmd, "}}")) {
        free(*out_env_json);
        *out_env_json = NULL;
        snprintf(err_buf, err_len, "Command template contains an undeclared placeholder");
        return -1;
    }
    if (out_resources) *out_resources = resources;
    return 0;
}

static int extract_idempotency_key(struct mg_http_message *hm,
                                   char *out, size_t out_len)
{
    struct mg_str *header = mg_http_get_header(hm, "Idempotency-Key");
    if (!header || header->len == 0) {
        out[0] = '\0';
        return 0;
    }
    if (header->len >= out_len) return -1;
    memcpy(out, header->buf, header->len);
    out[header->len] = '\0';
    for (const unsigned char *p = (const unsigned char *)out; *p; p++) {
        if (!isalnum(*p) && !strchr("_-.:", *p)) return -1;
    }
    return 1;
}

static void submit_job(struct mg_connection *c, struct mg_http_message *hm,
                       const char *auth_user_id, const char *auth_role)
{
    char idempotency_key[129];
    int has_idempotency_key = extract_idempotency_key(hm, idempotency_key,
                                                       sizeof(idempotency_key));
    if (has_idempotency_key < 0 || (has_idempotency_key && !auth_user_id[0])) {
        http_error(c, 400, "Idempotency-Key must be 1-128 safe characters and requires a bound user");
        return;
    }
    if (has_idempotency_key) {
        char existing_job_id[JOB_ID_LEN];
        int lookup = db_get_submission_job(auth_user_id, idempotency_key,
                                           existing_job_id, sizeof(existing_job_id));
        if (lookup < 0) {
            http_error(c, 500, "Failed to check idempotency key");
            return;
        }
        if (lookup > 0) {
            Job *existing_job = db_get_job(existing_job_id);
            if (!existing_job) {
                http_error(c, 500, "Idempotency record is inconsistent");
                return;
            }
            cJSON *response = job_to_json(existing_job);
            char *serialized = response ? cJSON_PrintUnformatted(response) : NULL;
            job_free(existing_job);
            cJSON_Delete(response);
            if (!serialized) { http_error(c, 500, "Out of memory"); return; }
            http_json_reply(c, 200, serialized);
            free(serialized);
            return;
        }
    }
    if (!s_accepting_jobs) {
        http_error(c, 503, "Scheduler is draining and not accepting new jobs");
        return;
    }
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
    cJSON *jparams = cJSON_GetObjectItemCaseSensitive(req, "parameters");
    if (!jcor)  jcor  = cJSON_GetObjectItemCaseSensitive(req, "cores");
    if (!jgpu)  jgpu  = cJSON_GetObjectItemCaseSensitive(req, "gpu");
    if (!jram)  jram  = cJSON_GetObjectItemCaseSensitive(req, "ram_mb");
    if (!jdisk) jdisk = cJSON_GetObjectItemCaseSensitive(req, "disk_mb");

    int app_only = (strcmp(g_config.command_mode, "app_only") == 0);
    char resolved_cmd[JOB_CMD_LEN] = {0};
    char *app_env_json = NULL;
    AppResources app_resources;
    app_resources_defaults(&app_resources);
    int use_app_resources = 0;

    if (app_only) {
        /* App-only mode: require app_id, reject raw command */
        if (cJSON_IsString(jcmd)) {
            cJSON_Delete(req);
            http_error(c, 400, "Raw 'command' not allowed in app_only mode; use 'app_id' + 'parameters'");
            return;
        }
        if (!cJSON_IsString(japp) || !japp->valuestring[0]) {
            cJSON_Delete(req);
            http_error(c, 400, "Missing 'app_id' (required in app_only mode)");
            return;
        }
        char err[256];
        if (resolve_app_command(japp->valuestring, jparams,
                                resolved_cmd, sizeof(resolved_cmd),
                                &app_env_json, &app_resources,
                                err, sizeof(err)) != 0) {
            cJSON_Delete(req);
            http_error(c, 400, err);
            return;
        }
        use_app_resources = 1;
    } else {
        /* Free mode: require command */
        if (!cJSON_IsString(jcmd)) {
            cJSON_Delete(req);
            http_error(c, 400, "Missing 'command'");
            return;
        }
        strncpy(resolved_cmd, jcmd->valuestring, sizeof(resolved_cmd) - 1);

        /* If app_id provided in free mode, try to load env from app definition */
        if (cJSON_IsString(japp) && japp->valuestring[0]) {
            char err[256];
            char _unused[JOB_CMD_LEN];
            resolve_app_command(japp->valuestring, NULL,
                                _unused, sizeof(_unused),
                                &app_env_json, NULL, err, sizeof(err));
            /* Ignore errors — env is best-effort in free mode */
        }
    }

    char input_files_str[2048] = {0};
    cJSON *jfiles = cJSON_GetObjectItemCaseSensitive(req, "input_files");
    if (jfiles && !cJSON_IsArray(jfiles)) {
        free(app_env_json);
        cJSON_Delete(req);
        http_error(c, 400, "'input_files' must be an array");
        return;
    }
    if (cJSON_IsArray(jfiles)) {
        cJSON *f;
        cJSON_ArrayForEach(f, jfiles) {
            if (!cJSON_IsString(f) || !transfer_valid_filename(f->valuestring)) {
                free(app_env_json);
                cJSON_Delete(req);
                http_error(c, 400, "Invalid input filename");
                return;
            }
            size_t used = strlen(input_files_str);
            size_t value_len = strlen(f->valuestring);
            if (used + (used ? 1 : 0) + value_len + 1 > sizeof(input_files_str)) {
                free(app_env_json);
                cJSON_Delete(req);
                http_error(c, 400, "Input file list is too long");
                return;
            }
            if (used) input_files_str[used++] = ',';
            memcpy(input_files_str + used, f->valuestring, value_len + 1);
        }
    }

    /* user_id: prefer the one bound to the API key; fall back to body */
    const char *user_id = (auth_user_id && auth_user_id[0])
                        ? auth_user_id
                        : (cJSON_IsString(juser) ? juser->valuestring : "");
    const char *app_id  = cJSON_IsString(japp)  ? japp->valuestring  : "";

    /* ── Dependency handling ─────────────────────────────────── */
    char depends_on_str[2048] = {0};
    cJSON *jdeps = cJSON_GetObjectItemCaseSensitive(req, "depends_on");
    if (cJSON_IsArray(jdeps)) {
        cJSON *dep;
        cJSON_ArrayForEach(dep, jdeps) {
            if (cJSON_IsString(dep) && dep->valuestring[0]) {
                /* Validate that each dep job exists */
                Job *depjob = db_get_job(dep->valuestring);
                if (!depjob || (strcmp(auth_role, "admin") != 0 &&
                    (!depjob->user_id[0] || strcmp(depjob->user_id, auth_user_id) != 0))) {
                    if (depjob) job_free(depjob);
                    cJSON_Delete(req);
                    char err[256];
                    snprintf(err, sizeof(err), "Dependency job not found: %s", dep->valuestring);
                    http_error(c, 400, err);
                    return;
                }
                job_free(depjob);
                if (depends_on_str[0])
                    strncat(depends_on_str, ",", sizeof(depends_on_str) - strlen(depends_on_str) - 1);
                strncat(depends_on_str, dep->valuestring, sizeof(depends_on_str) - strlen(depends_on_str) - 1);
            }
        }
    } else if (cJSON_IsString(jdeps) && jdeps->valuestring[0]) {
        Job *depjob = db_get_job(jdeps->valuestring);
        if (!depjob || (strcmp(auth_role, "admin") != 0 &&
            (!depjob->user_id[0] || strcmp(depjob->user_id, auth_user_id) != 0))) {
            if (depjob) job_free(depjob);
            cJSON_Delete(req);
            char err[256];
            snprintf(err, sizeof(err), "Dependency job not found: %s", jdeps->valuestring);
            http_error(c, 400, err);
            return;
        }
        job_free(depjob);
        strncpy(depends_on_str, jdeps->valuestring, sizeof(depends_on_str) - 1);
    }

    /* Quotas are enforced at dispatch time in the scheduler, not here.
       Jobs are always accepted and queued; the scheduler holds them back
       until the user/app is within quota limits. */

    /* Extract timeout before freeing JSON */
    int job_timeout = cJSON_IsNumber(jtout) ? clamp_int(jtout->valuedouble, 0, 604800) : 0;

    Job *job = job_create_ex(
        resolved_cmd,
        cJSON_IsNumber(jpri)  ? clamp_int(jpri->valuedouble,  0, 100)     : 50,
        use_app_resources ? app_resources.req_cores
                          : (cJSON_IsNumber(jcor) ? clamp_int(jcor->valuedouble, 1, 10000) : 1),
        use_app_resources ? app_resources.req_gpu
                          : (cJSON_IsNumber(jgpu) ? clamp_int(jgpu->valuedouble, 0, 1000) : 0),
        use_app_resources ? app_resources.req_ram_mb
                          : (cJSON_IsNumber(jram) ? clamp_int(jram->valuedouble, 0, 10000000) : 0),
        use_app_resources ? app_resources.req_disk_mb
                          : (cJSON_IsNumber(jdisk) ? clamp_int(jdisk->valuedouble, 0, 10000000) : 0),
        user_id, app_id
    );
    cJSON_Delete(req);
    if (!job) {
        free(app_env_json);
        http_error(c, 500, "Failed to create job");
        return;
    }

    /* Store app env JSON file alongside the job for the executor */
    if (app_env_json) {
        store_init_job_dirs(job->id);
        char env_path[768];
#ifdef _WIN32
        snprintf(env_path, sizeof(env_path), "%s\\.app_env.json", job->input_dir);
#else
        snprintf(env_path, sizeof(env_path), "%s/.app_env.json", job->input_dir);
#endif
        FILE *ef = fopen(env_path, "wb");
        if (ef) { fwrite(app_env_json, 1, strlen(app_env_json), ef); fclose(ef); }
        free(app_env_json);
    }

    /* Store dependencies */
    if (depends_on_str[0]) {
        strncpy(job->depends_on, depends_on_str, sizeof(job->depends_on) - 1);
        db_update_depends_on(job->id, depends_on_str);
    }

    int is_held = (input_files_str[0] != '\0' || depends_on_str[0] != '\0');
    if (job_timeout > 0) {
        job->timeout_seconds = job_timeout;
        db_update_job_timeout(job->id, job_timeout);
    }

    if (has_idempotency_key &&
        db_store_submission_key(auth_user_id, idempotency_key, job->id) != 0) {
        job_set_status_r(job, JOB_STATUS_FAILED, "Failed to persist idempotency key");
        store_cleanup_job(job->id);
        job_free(job);
        http_error(c, 500, "Failed to persist idempotency key");
        return;
    }

    if (is_held) {
        strncpy(job->input_files, input_files_str, sizeof(job->input_files) - 1);
        job->status = JOB_STATUS_CREATED;
        db_update_job_status(job->id, JOB_STATUS_CREATED, 0, 0);
        if (input_files_str[0])
            db_update_input_files(job->id, input_files_str);
        char held_reason[256];
        if (depends_on_str[0] && input_files_str[0])
            snprintf(held_reason, sizeof(held_reason),
                "Waiting for dependencies and input files");
        else if (depends_on_str[0])
            snprintf(held_reason, sizeof(held_reason),
                "Waiting for dependencies: %s", depends_on_str);
        else
            snprintf(held_reason, sizeof(held_reason),
                "Waiting for input files: %s", input_files_str);
        strncpy(job->status_reason, held_reason, sizeof(job->status_reason) - 1);
        db_update_status_reason(job->id, held_reason);
        store_init_job_dirs(job->id);
        log_info("routes", "Job %s held: %s", job->id, held_reason);
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
    if (job->status != JOB_STATUS_CREATED) {
        http_error(c, 409, "Job is not in CREATED state");
        job_free(job);
        return;
    }
    job_set_status_r(job, JOB_STATUS_QUEUED, "Released manually by user");
    queue_push(scheduler_queue(), job);
    cJSON *resp = job_to_json(job);
    char *s = cJSON_PrintUnformatted(resp);
    http_json_reply(c, 200, s);
    free(s);
    cJSON_Delete(resp);
}

/* ── Saved Workflow CRUD ─────────────────────────────────────────── */

static void gen_wf_id(char *out, int len)
{
    unsigned char raw[16];
#ifdef _WIN32
    {
        HCRYPTPROV hprov;
        CryptAcquireContextA(&hprov, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT);
        CryptGenRandom(hprov, sizeof(raw), raw);
        CryptReleaseContext(hprov, 0);
    }
#else
    {
        FILE *f = fopen("/dev/urandom", "rb");
        if (f) { fread(raw, 1, sizeof(raw), f); fclose(f); }
    }
#endif
    int pos = 0;
    for (int i = 0; i < 16 && pos < len - 1; i++)
        pos += snprintf(out + pos, len - pos, "%02x", raw[i]);
    out[pos] = '\0';
}

static void list_saved_workflows(struct mg_connection *c,
                                  const char *auth_user_id, const char *auth_role)
{
    WorkflowDef *wfs = (WorkflowDef *)malloc(128 * sizeof(WorkflowDef));
    if (!wfs) { http_error(c, 500, "Out of memory"); return; }

    /* Admin sees all workflows, users see own + global */
    int count;
    if (strcmp(auth_role, "admin") == 0)
        count = db_list_workflows("", wfs, 128);
    else
        count = db_list_workflows(auth_user_id, wfs, 128);

    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < count; i++) {
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "id", wfs[i].id);
        cJSON_AddStringToObject(obj, "name", wfs[i].name);
        cJSON_AddStringToObject(obj, "owner_id", wfs[i].owner_id);
        cJSON_AddBoolToObject(obj, "is_global", wfs[i].is_global);
        /* Parse steps_json string into a JSON array */
        cJSON *steps = cJSON_Parse(wfs[i].steps_json);
        if (steps) cJSON_AddItemToObject(obj, "steps", steps);
        else cJSON_AddItemToObject(obj, "steps", cJSON_CreateArray());
        cJSON_AddNumberToObject(obj, "created_at", (double)wfs[i].created_at);
        cJSON_AddNumberToObject(obj, "updated_at", (double)wfs[i].updated_at);
        int fav = db_is_workflow_favorite(auth_user_id, wfs[i].id);
        cJSON_AddBoolToObject(obj, "is_favorite", fav);
        cJSON_AddItemToArray(arr, obj);
    }
    free(wfs);
    char *s = cJSON_PrintUnformatted(arr);
    http_json_reply(c, 200, s);
    free(s);
    cJSON_Delete(arr);
}

static void save_workflow(struct mg_connection *c, struct mg_http_message *hm,
                          const char *auth_user_id, const char *auth_role)
{
    char body[32768] = {0};
    size_t blen = hm->body.len < sizeof(body)-1 ? hm->body.len : sizeof(body)-1;
    memcpy(body, hm->body.buf, blen);

    cJSON *req = cJSON_Parse(body);
    if (!req) { http_error(c, 400, "Invalid JSON"); return; }

    cJSON *jname = cJSON_GetObjectItemCaseSensitive(req, "name");
    cJSON *jsteps = cJSON_GetObjectItemCaseSensitive(req, "steps");
    cJSON *jglobal = cJSON_GetObjectItemCaseSensitive(req, "is_global");
    cJSON *jid = cJSON_GetObjectItemCaseSensitive(req, "id");

    if (!cJSON_IsString(jname) || !jname->valuestring[0]) {
        cJSON_Delete(req); http_error(c, 400, "Missing 'name'"); return;
    }
    if (!cJSON_IsArray(jsteps) || cJSON_GetArraySize(jsteps) < 1) {
        cJSON_Delete(req); http_error(c, 400, "Missing or empty 'steps' array"); return;
    }

    int want_global = cJSON_IsTrue(jglobal) ? 1 : 0;
    /* Only admins can create global workflows */
    if (want_global && strcmp(auth_role, "admin") != 0) {
        cJSON_Delete(req);
        http_error(c, 403, "Only admins can create global workflows");
        return;
    }

    char *steps_str = cJSON_PrintUnformatted(jsteps);
    time_t now = time(NULL);

    /* If id provided, update existing workflow */
    if (cJSON_IsString(jid) && jid->valuestring[0]) {
        WorkflowDef existing;
        if (db_get_workflow(jid->valuestring, &existing) != 0) {
            free(steps_str); cJSON_Delete(req);
            http_error(c, 404, "Workflow not found"); return;
        }
        /* Only owner or admin can update */
        if (strcmp(existing.owner_id, auth_user_id) != 0 &&
            strcmp(auth_role, "admin") != 0) {
            free(steps_str); cJSON_Delete(req);
            http_error(c, 403, "Cannot edit another user's workflow"); return;
        }
        strncpy(existing.name, jname->valuestring, sizeof(existing.name)-1);
        existing.is_global = want_global;
        strncpy(existing.steps_json, steps_str, sizeof(existing.steps_json)-1);
        existing.updated_at = now;
        db_update_workflow(&existing);
        free(steps_str); cJSON_Delete(req);
        http_json_reply(c, 200, "{\"ok\":true}");
        return;
    }

    /* Create new workflow */
    WorkflowDef w;
    memset(&w, 0, sizeof(w));
    gen_wf_id(w.id, sizeof(w.id));
    strncpy(w.name, jname->valuestring, sizeof(w.name)-1);
    strncpy(w.owner_id, auth_user_id, sizeof(w.owner_id)-1);
    w.is_global = want_global;
    strncpy(w.steps_json, steps_str, sizeof(w.steps_json)-1);
    w.created_at = now;
    w.updated_at = now;
    free(steps_str);

    if (db_insert_workflow(&w) != 0) {
        cJSON_Delete(req);
        http_error(c, 500, "Failed to save workflow"); return;
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "id", w.id);
    cJSON_AddStringToObject(resp, "name", w.name);
    cJSON_AddBoolToObject(resp, "ok", 1);
    cJSON_Delete(req);
    char *s = cJSON_PrintUnformatted(resp);
    http_json_reply(c, 201, s);
    free(s);
    cJSON_Delete(resp);
}

static void delete_saved_workflow(struct mg_connection *c,
                                   const char *wf_id,
                                   const char *auth_user_id,
                                   const char *auth_role)
{
    WorkflowDef existing;
    if (db_get_workflow(wf_id, &existing) != 0) {
        http_error(c, 404, "Workflow not found"); return;
    }
    if (strcmp(existing.owner_id, auth_user_id) != 0 &&
        strcmp(auth_role, "admin") != 0) {
        http_error(c, 403, "Cannot delete another user's workflow"); return;
    }
    db_delete_workflow(wf_id);
    http_json_reply(c, 200, "{\"ok\":true}");
}

static void toggle_workflow_favorite(struct mg_connection *c,
                                      struct mg_http_message *hm,
                                      const char *wf_id,
                                      const char *auth_user_id)
{
    (void)hm;
    WorkflowDef existing;
    if (db_get_workflow(wf_id, &existing) != 0) {
        http_error(c, 404, "Workflow not found"); return;
    }
    if (db_is_workflow_favorite(auth_user_id, wf_id)) {
        db_remove_workflow_favorite(auth_user_id, wf_id);
        http_json_reply(c, 200, "{\"is_favorite\":false}");
    } else {
        db_add_workflow_favorite(auth_user_id, wf_id);
        http_json_reply(c, 200, "{\"is_favorite\":true}");
    }
}

/* ── Workflow submission (chained job bundles) ───────────────────── */

#define MAX_WORKFLOW_STEPS 64

static int append_csv_value(char *buffer, size_t capacity, const char *value)
{
    size_t used = strlen(buffer);
    size_t value_len = strlen(value);
    size_t separator_len = used > 0 ? 1 : 0;
    if (used + separator_len + value_len + 1 > capacity) return -1;
    if (separator_len) buffer[used++] = ',';
    memcpy(buffer + used, value, value_len + 1);
    return 0;
}

static void workflow_discard_jobs(Job **jobs, int count)
{
    for (int i = 0; i < count; i++) {
        if (!jobs[i]) continue;
        store_cleanup_job(jobs[i]->id);
        job_free(jobs[i]);
        jobs[i] = NULL;
    }
}

static void submit_workflow(struct mg_connection *c, struct mg_http_message *hm,
                            const char *auth_user_id, const char *auth_role)
{
    if (!s_accepting_jobs) {
        http_error(c, 503, "Scheduler is draining and not accepting new jobs");
        return;
    }
    char body[32768] = {0};
    size_t blen = hm->body.len < sizeof(body)-1 ? hm->body.len : sizeof(body)-1;
    memcpy(body, hm->body.buf, blen);

    cJSON *req = cJSON_Parse(body);
    if (!req) { http_error(c, 400, "Invalid JSON"); return; }

    cJSON *jsteps = cJSON_GetObjectItemCaseSensitive(req, "steps");
    if (!cJSON_IsArray(jsteps)) {
        cJSON_Delete(req); http_error(c, 400, "Missing 'steps' array"); return;
    }

    int n_steps = cJSON_GetArraySize(jsteps);
    if (n_steps < 1 || n_steps > MAX_WORKFLOW_STEPS) {
        cJSON_Delete(req);
        char err[128];
        snprintf(err, sizeof(err), "'steps' must have 1-%d entries", MAX_WORKFLOW_STEPS);
        http_error(c, 400, err);
        return;
    }

    int app_only = (strcmp(g_config.command_mode, "app_only") == 0);
    char job_ids[MAX_WORKFLOW_STEPS][64] = {{0}};
    Job *created_jobs[MAX_WORKFLOW_STEPS] = {NULL};
    Job *response_jobs = (Job *)calloc((size_t)n_steps, sizeof(Job));
    char *app_env_json = NULL;
    char error_message[320] = {0};
    int error_status = 400;
    int created = 0;
    int idx = 0;

    if (!response_jobs) {
        cJSON_Delete(req);
        http_error(c, 500, "Out of memory");
        return;
    }

    char wf_batch_id[64];
    gen_wf_id(wf_batch_id, sizeof(wf_batch_id));
    if (db_begin() != 0) {
        free(response_jobs);
        cJSON_Delete(req);
        http_error(c, 503, "Database is busy");
        return;
    }

    cJSON *step = NULL;
    cJSON_ArrayForEach(step, jsteps) {
        if (!cJSON_IsObject(step)) {
            snprintf(error_message, sizeof(error_message), "Each step must be a JSON object");
            goto workflow_error;
        }

        cJSON *jcmd    = cJSON_GetObjectItemCaseSensitive(step, "command");
        cJSON *japp    = cJSON_GetObjectItemCaseSensitive(step, "app_id");
        cJSON *jparams = cJSON_GetObjectItemCaseSensitive(step, "parameters");
        cJSON *jpri    = cJSON_GetObjectItemCaseSensitive(step, "priority");
        cJSON *jcor    = cJSON_GetObjectItemCaseSensitive(step, "req_cores");
        cJSON *jgpu    = cJSON_GetObjectItemCaseSensitive(step, "req_gpu");
        cJSON *jram    = cJSON_GetObjectItemCaseSensitive(step, "req_ram_mb");
        cJSON *jdisk   = cJSON_GetObjectItemCaseSensitive(step, "req_disk_mb");
        cJSON *jtout   = cJSON_GetObjectItemCaseSensitive(step, "timeout_seconds");
        cJSON *jdep_s  = cJSON_GetObjectItemCaseSensitive(step, "depends_on_steps");
        if (!jcor)  jcor  = cJSON_GetObjectItemCaseSensitive(step, "cores");
        if (!jgpu)  jgpu  = cJSON_GetObjectItemCaseSensitive(step, "gpu");
        if (!jram)  jram  = cJSON_GetObjectItemCaseSensitive(step, "ram_mb");
        if (!jdisk) jdisk = cJSON_GetObjectItemCaseSensitive(step, "disk_mb");

        char resolved_cmd[JOB_CMD_LEN] = {0};
        app_env_json = NULL;
        AppResources app_resources;
        app_resources_defaults(&app_resources);
        int use_app_resources = 0;
        if (app_only) {
            if (cJSON_IsString(jcmd)) {
                snprintf(error_message, sizeof(error_message),
                         "Step %d: raw 'command' not allowed in app_only mode", idx);
                goto workflow_error;
            }
            if (!cJSON_IsString(japp) || !japp->valuestring[0]) {
                snprintf(error_message, sizeof(error_message),
                         "Step %d: missing 'app_id' (required in app_only mode)", idx);
                goto workflow_error;
            }
            char resolve_error[256];
            if (resolve_app_command(japp->valuestring, jparams,
                                    resolved_cmd, sizeof(resolved_cmd),
                                    &app_env_json, &app_resources,
                                    resolve_error, sizeof(resolve_error)) != 0) {
                snprintf(error_message, sizeof(error_message), "Step %d: %s", idx, resolve_error);
                goto workflow_error;
            }
            use_app_resources = 1;
        } else if (cJSON_IsString(jcmd) && jcmd->valuestring[0]) {
            strncpy(resolved_cmd, jcmd->valuestring, sizeof(resolved_cmd) - 1);
        } else if (cJSON_IsString(japp) && japp->valuestring[0]) {
            char resolve_error[256];
            if (resolve_app_command(japp->valuestring, jparams,
                                    resolved_cmd, sizeof(resolved_cmd),
                                    &app_env_json, &app_resources,
                                    resolve_error, sizeof(resolve_error)) != 0) {
                snprintf(error_message, sizeof(error_message), "Step %d: %s", idx, resolve_error);
                goto workflow_error;
            }
            use_app_resources = 1;
        } else {
            snprintf(error_message, sizeof(error_message),
                     "Step %d: missing 'command' or 'app_id'", idx);
            goto workflow_error;
        }

        char depends_on_str[2048] = {0};
        if (jdep_s && !cJSON_IsArray(jdep_s)) {
            snprintf(error_message, sizeof(error_message),
                     "Step %d: 'depends_on_steps' must be an array", idx);
            goto workflow_error;
        }
        if (cJSON_IsArray(jdep_s)) {
            cJSON *dependency_index = NULL;
            cJSON_ArrayForEach(dependency_index, jdep_s) {
                if (!cJSON_IsNumber(dependency_index)) {
                    snprintf(error_message, sizeof(error_message),
                             "Step %d: dependency indices must be numbers", idx);
                    goto workflow_error;
                }
                int dep_idx = (int)dependency_index->valuedouble;
                if (dep_idx < 0 || dep_idx >= created || dependency_index->valuedouble != dep_idx) {
                    snprintf(error_message, sizeof(error_message),
                             "Step %d: depends_on_steps references invalid step %d", idx, dep_idx);
                    goto workflow_error;
                }
                if (append_csv_value(depends_on_str, sizeof(depends_on_str), job_ids[dep_idx]) != 0) {
                    snprintf(error_message, sizeof(error_message), "Step %d: dependency list is too long", idx);
                    goto workflow_error;
                }
            }
        }

        cJSON *external_deps = cJSON_GetObjectItemCaseSensitive(step, "depends_on");
        if (external_deps && !cJSON_IsArray(external_deps)) {
            snprintf(error_message, sizeof(error_message), "Step %d: 'depends_on' must be an array", idx);
            goto workflow_error;
        }
        if (cJSON_IsArray(external_deps)) {
            cJSON *dependency = NULL;
            cJSON_ArrayForEach(dependency, external_deps) {
                if (!cJSON_IsString(dependency) || !dependency->valuestring[0]) {
                    snprintf(error_message, sizeof(error_message),
                             "Step %d: dependency IDs must be non-empty strings", idx);
                    goto workflow_error;
                }
                Job *dependency_job = db_get_job(dependency->valuestring);
                if (!dependency_job || (strcmp(auth_role, "admin") != 0 &&
                    (!dependency_job->user_id[0] ||
                     strcmp(dependency_job->user_id, auth_user_id) != 0))) {
                    if (dependency_job) job_free(dependency_job);
                    snprintf(error_message, sizeof(error_message),
                             "Step %d: dependency job not found: %s", idx, dependency->valuestring);
                    goto workflow_error;
                }
                job_free(dependency_job);
                if (append_csv_value(depends_on_str, sizeof(depends_on_str), dependency->valuestring) != 0) {
                    snprintf(error_message, sizeof(error_message), "Step %d: dependency list is too long", idx);
                    goto workflow_error;
                }
            }
        }

        char input_files_str[2048] = {0};
        cJSON *input_files = cJSON_GetObjectItemCaseSensitive(step, "input_files");
        if (input_files && !cJSON_IsArray(input_files)) {
            snprintf(error_message, sizeof(error_message), "Step %d: 'input_files' must be an array", idx);
            goto workflow_error;
        }
        if (cJSON_IsArray(input_files)) {
            cJSON *input_file = NULL;
            cJSON_ArrayForEach(input_file, input_files) {
                if (!cJSON_IsString(input_file) || !input_file->valuestring[0] ||
                    !transfer_valid_filename(input_file->valuestring) ||
                    append_csv_value(input_files_str, sizeof(input_files_str), input_file->valuestring) != 0) {
                    snprintf(error_message, sizeof(error_message),
                             "Step %d: invalid or oversized input file list", idx);
                    goto workflow_error;
                }
            }
        }

        const char *app_id = cJSON_IsString(japp) ? japp->valuestring : "";
        Job *job = job_create_ex(
            resolved_cmd,
            cJSON_IsNumber(jpri)  ? clamp_int(jpri->valuedouble, 0, 100) : 50,
            use_app_resources ? app_resources.req_cores
                              : (cJSON_IsNumber(jcor) ? clamp_int(jcor->valuedouble, 1, 10000) : 1),
            use_app_resources ? app_resources.req_gpu
                              : (cJSON_IsNumber(jgpu) ? clamp_int(jgpu->valuedouble, 0, 1000) : 0),
            use_app_resources ? app_resources.req_ram_mb
                              : (cJSON_IsNumber(jram) ? clamp_int(jram->valuedouble, 0, 10000000) : 0),
            use_app_resources ? app_resources.req_disk_mb
                              : (cJSON_IsNumber(jdisk) ? clamp_int(jdisk->valuedouble, 0, 10000000) : 0),
            auth_user_id, app_id);
        if (!job) {
            error_status = 500;
            snprintf(error_message, sizeof(error_message), "Failed to create workflow job");
            goto workflow_error;
        }

        created_jobs[created] = job;
        strncpy(job_ids[idx], job->id, sizeof(job_ids[idx]) - 1);
        created++;

        strncpy(job->workflow_id, wf_batch_id, sizeof(job->workflow_id) - 1);
        strncpy(job->depends_on, depends_on_str, sizeof(job->depends_on) - 1);
        strncpy(job->input_files, input_files_str, sizeof(job->input_files) - 1);
        job->timeout_seconds = cJSON_IsNumber(jtout)
                             ? clamp_int(jtout->valuedouble, 0, 604800) : 0;

        cJSON *same_machine = cJSON_GetObjectItemCaseSensitive(step, "same_machine");
        if (cJSON_IsTrue(same_machine) && depends_on_str[0]) {
            const char *comma = strchr(depends_on_str, ',');
            size_t id_len = comma ? (size_t)(comma - depends_on_str) : strlen(depends_on_str);
            if (id_len >= sizeof(job->same_machine_as)) id_len = sizeof(job->same_machine_as) - 1;
            memcpy(job->same_machine_as, depends_on_str, id_len);
            job->same_machine_as[id_len] = '\0';
        }

        if (depends_on_str[0] || input_files_str[0]) {
            job->status = JOB_STATUS_CREATED;
            if (depends_on_str[0] && input_files_str[0])
                snprintf(job->status_reason, sizeof(job->status_reason),
                         "Waiting for dependencies and input files");
            else if (depends_on_str[0])
                snprintf(job->status_reason, sizeof(job->status_reason),
                         "Waiting for dependencies: %.210s", depends_on_str);
            else
                snprintf(job->status_reason, sizeof(job->status_reason),
                         "Waiting for input files: %.220s", input_files_str);
        }

        if (app_env_json || job->status == JOB_STATUS_CREATED) {
            if (store_init_job_dirs(job->id) != 0) {
                error_status = 500;
                snprintf(error_message, sizeof(error_message), "Failed to initialize workflow job storage");
                goto workflow_error;
            }
        }
        if (app_env_json) {
            char env_path[768];
#ifdef _WIN32
            snprintf(env_path, sizeof(env_path), "%s\\.app_env.json", job->input_dir);
#else
            snprintf(env_path, sizeof(env_path), "%s/.app_env.json", job->input_dir);
#endif
            FILE *env_file = fopen(env_path, "wb");
            size_t env_len = strlen(app_env_json);
            int env_write_failed = !env_file;
            if (env_file) {
                if (fwrite(app_env_json, 1, env_len, env_file) != env_len)
                    env_write_failed = 1;
                if (fclose(env_file) != 0)
                    env_write_failed = 1;
            }
            if (env_write_failed) {
                error_status = 500;
                snprintf(error_message, sizeof(error_message), "Failed to store workflow application environment");
                goto workflow_error;
            }
            free(app_env_json);
            app_env_json = NULL;
        }

        if (db_update_job_submission(job) != 0) {
            error_status = 500;
            snprintf(error_message, sizeof(error_message), "Failed to persist workflow job details");
            goto workflow_error;
        }
        idx++;
    }

    for (int i = 0; i < created; i++) response_jobs[i] = *created_jobs[i];
    if (db_commit() != 0) {
        workflow_discard_jobs(created_jobs, created);
        free(response_jobs);
        cJSON_Delete(req);
        http_error(c, 500, "Failed to commit workflow");
        return;
    }

    for (int i = 0; i < created; i++) {
        if (created_jobs[i]->status == JOB_STATUS_CREATED) {
            job_free(created_jobs[i]);
        } else if (queue_push(scheduler_queue(), created_jobs[i]) != 0) {
            job_set_status_r(created_jobs[i], JOB_STATUS_FAILED, "Scheduler queue unavailable");
            response_jobs[i] = *created_jobs[i];
            job_free(created_jobs[i]);
        }
        created_jobs[i] = NULL;
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON *arr = resp ? cJSON_AddArrayToObject(resp, "jobs") : NULL;
    if (!resp || !arr) {
        cJSON_Delete(resp);
        free(response_jobs);
        cJSON_Delete(req);
        http_error(c, 500, "Out of memory while building workflow response");
        return;
    }
    cJSON *jname = cJSON_GetObjectItemCaseSensitive(req, "name");
    if (cJSON_IsString(jname)) cJSON_AddStringToObject(resp, "name", jname->valuestring);
    cJSON_AddStringToObject(resp, "workflow_id", wf_batch_id);
    for (int i = 0; i < created; i++)
        cJSON_AddItemToArray(arr, job_to_json(&response_jobs[i]));

    char *serialized = cJSON_PrintUnformatted(resp);
    if (serialized) {
        http_json_reply(c, 201, serialized);
        free(serialized);
    } else {
        http_error(c, 500, "Out of memory while serializing workflow response");
    }
    cJSON_Delete(resp);
    free(response_jobs);
    cJSON_Delete(req);
    log_info("routes", "Workflow submitted: %d steps by user %s", created, auth_user_id);
    return;

workflow_error:
    free(app_env_json);
    db_rollback();
    workflow_discard_jobs(created_jobs, created);
    free(response_jobs);
    cJSON_Delete(req);
    http_error(c, error_status, error_message[0] ? error_message : "Workflow submission failed");
}

static void list_jobs(struct mg_connection *c, struct mg_http_message *hm,
                      const char *auth_user_id, const char *auth_role)
{
    int is_admin = (strcmp(auth_role, "admin") == 0);
    int limit = 100;
    int offset = 0;
    int status = -1;
    char value[128] = {0};
    char user_filter[128] = {0};
    char app_filter[128] = {0};

    if (parse_query_int(hm, "limit", 100, 1, 500, &limit) != 0 ||
        parse_query_int(hm, "offset", 0, 0, 100000000, &offset) != 0) {
        http_error(c, 400, "Invalid pagination parameters");
        return;
    }
    if (mg_http_get_var(&hm->query, "status", value, sizeof(value)) > 0) {
        status = job_status_from_text(value);
        if (status == -2) {
            http_error(c, 400, "Invalid job status filter");
            return;
        }
    }
    if (mg_http_get_var(&hm->query, "app_id", app_filter, sizeof(app_filter)) > 0 &&
        !safe_cli_name(app_filter)) {
        http_error(c, 400, "Invalid app_id filter");
        return;
    }
    if (is_admin &&
        mg_http_get_var(&hm->query, "user_id", user_filter, sizeof(user_filter)) > 0 &&
        !safe_cli_name(user_filter)) {
        http_error(c, 400, "Invalid user_id filter");
        return;
    }
    const char *owner_filter = is_admin ? user_filter : auth_user_id;
    Job *jobs = NULL;
    int count = db_query_jobs(owner_filter, status, app_filter, limit, offset, &jobs);
    if (count < 0) { http_error(c, 500, "Failed to query jobs"); return; }
    cJSON *arr = cJSON_CreateArray();
    if (!arr) { free(jobs); http_error(c, 500, "Out of memory"); return; }
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

static void list_queue(struct mg_connection *c, struct mg_http_message *hm,
                       const char *auth_user_id, const char *auth_role)
{
    int limit = 100;
    char value[32];
    if (mg_http_get_var(&hm->query, "limit", value, sizeof(value)) > 0)
        limit = clamp_int(atof(value), 1, 1000);

    Job *jobs = (Job *)calloc(1000, sizeof(Job));
    if (!jobs) { http_error(c, 500, "Out of memory"); return; }
    int count = db_list_queued_jobs(jobs, 1000, 0);
    int is_admin = strcmp(auth_role, "admin") == 0;
    int visible = 0;
    cJSON *array = cJSON_CreateArray();
    for (int i = 0; i < count && visible < limit; i++) {
        if (!is_admin && strcmp(jobs[i].user_id, auth_user_id) != 0) continue;
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "position", visible + 1);
        cJSON_AddStringToObject(item, "id", jobs[i].id);
        cJSON_AddStringToObject(item, "batch_id", jobs[i].batch_id);
        cJSON_AddStringToObject(item, "status", job_status_str(jobs[i].status));
        cJSON_AddNumberToObject(item, "priority", jobs[i].priority);
        cJSON_AddNumberToObject(item, "submitted_at", (double)jobs[i].submitted_at);
        cJSON_AddNumberToObject(item, "req_cores", jobs[i].req_cores);
        cJSON_AddNumberToObject(item, "req_gpu", jobs[i].req_gpu);
        cJSON_AddNumberToObject(item, "req_ram_mb", jobs[i].req_ram_mb);
        cJSON_AddItemToArray(array, item);
        visible++;
    }
    free(jobs);
    char *json = cJSON_PrintUnformatted(array);
    cJSON_Delete(array);
    if (!json) { http_error(c, 500, "Failed to encode queue"); return; }
    http_json_reply(c, 200, json);
    free(json);
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
    if (job->status == JOB_STATUS_SUCCEEDED ||
        job->status == JOB_STATUS_FAILED   ||
        job->status == JOB_STATUS_CANCELLED) {
        http_error(c, 409, "Job already in terminal state");
        job_free(job);
        return;
    }
    job_free(job);
    if (scheduler_cancel_job(job_id, "Cancelled by user") != 0) {
        http_error(c, 409, "Job could not be cancelled");
        return;
    }
    job = db_get_job(job_id);
    if (!job) { http_error(c, 404, "Job not found"); return; }
    cJSON *resp = job_to_json(job);
    char *s = cJSON_PrintUnformatted(resp);
    http_json_reply(c, 200, s);
    free(s);
    cJSON_Delete(resp);
    job_free(job);
}

static void retry_job(struct mg_connection *c, struct mg_http_message *hm,
                      const char *job_id)
{
    (void)hm;
    Job *previous = db_get_job(job_id);
    if (!previous) { http_error(c, 404, "Job not found"); return; }
    if (previous->status != JOB_STATUS_FAILED &&
        previous->status != JOB_STATUS_CANCELLED) {
        job_free(previous);
        http_error(c, 409, "Only FAILED or CANCELLED jobs can be retried");
        return;
    }
    if (executor_is_active(job_id)) {
        job_free(previous);
        http_error(c, 409, "Previous process is still terminating");
        return;
    }
    job_free(previous);

    Job *stale = queue_remove(scheduler_queue(), job_id);
    if (stale) job_free(stale);
    if (db_retry_job(job_id) != 0) {
        http_error(c, 409, "Job could not be retried");
        return;
    }
    store_reset_job_outputs(job_id);

    Job *retry = db_get_job(job_id);
    if (!retry) {
        db_update_job_status(job_id, JOB_STATUS_FAILED, -1, time(NULL));
        http_error(c, 500, "Failed to queue retry");
        return;
    }
    cJSON *response = job_to_json(retry);
    char *json = cJSON_PrintUnformatted(response);
    cJSON_Delete(response);
    if (!json || queue_push(scheduler_queue(), retry) != 0) {
        free(json);
        job_free(retry);
        db_update_job_status(job_id, JOB_STATUS_FAILED, -1, time(NULL));
        http_error(c, 500, "Failed to queue retry");
        return;
    }
    http_json_reply(c, 200, json);
    free(json);
}

static void purge_jobs(struct mg_connection *c, struct mg_http_message *hm)
{
    (void)hm;
    Job *jobs = (Job *)malloc(4096 * sizeof(Job));
    if (!jobs) { http_error(c, 500, "Out of memory"); return; }
    int count = db_list_jobs(jobs, 4096);
    int cleaned = 0;
    for (int i = 0; i < count; i++) {
        if (jobs[i].status == JOB_STATUS_SUCCEEDED ||
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
    if (!job || job->status != JOB_STATUS_CREATED || !job->input_files[0]) {
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
        if (!transfer_valid_filename(tok)) { all_present = 0; break; }
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
        job_set_status_r(job, JOB_STATUS_QUEUED, "All input files received via upload");
        queue_push(scheduler_queue(), job);
    } else {
        job_free(job);
    }
}

static int decode_filename_segment(const char *encoded, char *decoded, size_t decoded_len)
{
    int length = mg_url_decode(encoded, strlen(encoded), decoded, decoded_len, 0);
    return length >= 0 && (size_t)length == strlen(decoded) &&
           transfer_valid_filename(decoded);
}

static void upload_input(struct mg_connection *c, struct mg_http_message *hm,
                          const char *job_id, const char *filename)
{
    /* Limit upload size to 512 MB */
    if (hm->body.len > (size_t)(512 * 1024 * 1024)) {
        http_error(c, 413, "Upload too large (max 512 MB)");
        return;
    }
    char decoded_filename[256];
    if (!decode_filename_segment(filename, decoded_filename, sizeof(decoded_filename))) {
        http_error(c, 400, "Invalid filename");
        return;
    }
    long written = upload_handle(job_id, decoded_filename,
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
    char decoded_filename[256];
    if (!decode_filename_segment(filename, decoded_filename, sizeof(decoded_filename)) ||
        download_handle(c, hm, job_id, decoded_filename) != 0)
        http_error(c, 404, "Output file not found");
}

static void get_job_deps(struct mg_connection *c, struct mg_http_message *hm,
                         const char *job_id)
{
    (void)hm;
    Job *job = db_get_job(job_id);
    if (!job) { http_error(c, 404, "Job not found"); return; }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "job_id", job->id);
    cJSON_AddStringToObject(resp, "depends_on", job->depends_on);

    cJSON *deps_arr = cJSON_CreateArray();
    if (job->depends_on[0]) {
        char buf[2048];
        strncpy(buf, job->depends_on, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        char *tok = strtok(buf, ",");
        while (tok) {
            while (*tok == ' ') tok++;
            if (tok[0]) {
                Job *dep = db_get_job(tok);
                cJSON *d = cJSON_CreateObject();
                cJSON_AddStringToObject(d, "job_id", tok);
                if (dep) {
                    cJSON_AddStringToObject(d, "status", job_status_str(dep->status));
                    cJSON_AddStringToObject(d, "status_reason", dep->status_reason);
                    job_free(dep);
                } else {
                    cJSON_AddStringToObject(d, "status", "not_found");
                }
                cJSON_AddItemToArray(deps_arr, d);
            }
            tok = strtok(NULL, ",");
        }
    }
    cJSON_AddItemToObject(resp, "dependencies", deps_arr);

    int overall = db_check_deps_status(job->depends_on);
    cJSON_AddStringToObject(resp, "deps_status",
        overall == 0 ? "satisfied" :
        overall == -1 ? "failed" : "waiting");

    job_free(job);
    char *s = cJSON_PrintUnformatted(resp);
    http_json_reply(c, 200, s);
    free(s);
    cJSON_Delete(resp);
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

    char headers[1536] = {0};
    http_build_headers(headers, sizeof(headers), "text/plain; charset=utf-8");

    mg_printf(c,
        "HTTP/1.1 200 OK\r\n"
        "%s"
        "Content-Length: %ld\r\n"
        "\r\n", headers, sz);

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

static void sse_subscribe(struct mg_connection *c, struct mg_http_message *hm,
                          const char *auth_user_id, const char *auth_role)
{
    int is_admin = (strcmp(auth_role, "admin") == 0);
    char headers[1536] = {0};
    http_build_headers(headers, sizeof(headers), "text/event-stream");
    mg_printf(c,
        "HTTP/1.1 200 OK\r\n"
        "%s"
        "Cache-Control: no-cache\r\n"
        "Connection: keep-alive\r\n"
        "\r\n", headers);
    httpd_sse_add_user(c, auth_user_id, auth_role);

    Job *jobs = (Job *)malloc(256 * sizeof(Job));
    int count = jobs ? db_list_jobs(jobs, 256) : 0;
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < count; i++) {
        if (is_admin || strcmp(jobs[i].user_id, auth_user_id) == 0)
            cJSON_AddItemToArray(arr, job_to_json(&jobs[i]));
    }
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

    Machine *ms = NULL;
    int mcount = registry_snapshot(&ms);
    if (mcount < 0) { http_error(c, 500, "Failed to snapshot machines"); return; }
    int enabled = 0, cores_total = 0, cores_used = 0, ram_total = 0, ram_used = 0;
    for (int i = 0; i < mcount; i++) {
        if (ms[i].enabled) enabled++;
        cores_total += ms[i].cores_total;  cores_used += ms[i].cores_reserved;
        ram_total   += ms[i].ram_mb_total; ram_used   += ms[i].ram_mb_reserved;
    }
    free(ms);

    cJSON *root = cJSON_CreateObject();

    cJSON *jobs = cJSON_CreateObject();
    cJSON_AddNumberToObject(jobs, "total",     js.total);
    cJSON_AddNumberToObject(jobs, "created",   js.created);
    cJSON_AddNumberToObject(jobs, "queued",    js.queued);
    cJSON_AddNumberToObject(jobs, "running",   js.running);
    cJSON_AddNumberToObject(jobs, "succeeded", js.succeeded);
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

static void get_metrics(struct mg_connection *c, struct mg_http_message *hm)
{
    (void)hm;
    JobStats jobs;
    memset(&jobs, 0, sizeof(jobs));
    if (db_job_stats(&jobs) != 0) {
        http_error(c, 500, "Failed to collect metrics");
        return;
    }

    int machine_count = 0;
    int enabled = 0;
    int online = 0;
    int offline = 0;
    int probing = 0;
    long long cores_total = 0;
    long long cores_reserved = 0;
    long long ram_total = 0;
    long long ram_reserved = 0;
    Machine *machines = NULL;
    machine_count = registry_snapshot(&machines);
    if (machine_count < 0) {
        http_error(c, 500, "Failed to snapshot machines");
        return;
    }
    for (int i = 0; i < machine_count; i++) {
        if (machines[i].enabled) enabled++;
        if (machines[i].probe_status == MACHINE_ONLINE) online++;
        else if (machines[i].probe_status == MACHINE_OFFLINE) offline++;
        else probing++;
        cores_total += machines[i].cores_total;
        cores_reserved += machines[i].cores_reserved;
        ram_total += machines[i].ram_mb_total;
        ram_reserved += machines[i].ram_mb_reserved;
    }
    free(machines);

    char body[4096];
    int length = snprintf(body, sizeof(body),
        "# HELP bhc_jobs Jobs by scheduler state.\n"
        "# TYPE bhc_jobs gauge\n"
        "bhc_jobs{state=\"created\"} %d\n"
        "bhc_jobs{state=\"queued\"} %d\n"
        "bhc_jobs{state=\"running\"} %d\n"
        "bhc_jobs{state=\"succeeded\"} %d\n"
        "bhc_jobs{state=\"failed\"} %d\n"
        "bhc_jobs{state=\"cancelled\"} %d\n"
        "# HELP bhc_machines Machines by availability state.\n"
        "# TYPE bhc_machines gauge\n"
        "bhc_machines{state=\"enabled\"} %d\n"
        "bhc_machines{state=\"online\"} %d\n"
        "bhc_machines{state=\"offline\"} %d\n"
        "bhc_machines{state=\"probing\"} %d\n"
        "# TYPE bhc_accepting_jobs gauge\n"
        "bhc_accepting_jobs %d\n"
        "# TYPE bhc_resource_total gauge\n"
        "bhc_resource_total{resource=\"cores\"} %lld\n"
        "bhc_resource_total{resource=\"ram_mb\"} %lld\n"
        "# TYPE bhc_resource_reserved gauge\n"
        "bhc_resource_reserved{resource=\"cores\"} %lld\n"
        "bhc_resource_reserved{resource=\"ram_mb\"} %lld\n",
        jobs.created, jobs.queued, jobs.running,
        jobs.succeeded, jobs.failed, jobs.cancelled,
        enabled, online, offline, probing, s_accepting_jobs,
        cores_total, ram_total, cores_reserved, ram_reserved);
    if (length < 0 || length >= (int)sizeof(body)) {
        http_error(c, 500, "Metrics output too large");
        return;
    }
    char headers[1536] = {0};
    http_build_headers(headers, sizeof(headers), "text/plain; version=0.0.4; charset=utf-8");
    mg_http_reply(c, 200, headers, "%s", body);
}

static void admin_maintenance(struct mg_connection *c, struct mg_http_message *hm,
                              int update)
{
    if (update) {
        cJSON *request = cJSON_ParseWithLength(hm->body.buf, hm->body.len);
        cJSON *accepting = request
                         ? cJSON_GetObjectItemCaseSensitive(request, "accepting_jobs") : NULL;
        if (!request || !cJSON_IsBool(accepting)) {
            cJSON_Delete(request);
            http_error(c, 400, "'accepting_jobs' boolean is required");
            return;
        }
        s_accepting_jobs = cJSON_IsTrue(accepting) ? 1 : 0;
        cJSON_Delete(request);
        events_push_persistent("system", s_accepting_jobs ? "drain_disabled" : "drain_enabled",
                               s_accepting_jobs ? "Job submissions enabled" : "Job submissions paused",
                               "");
    }

    JobStats jobs;
    memset(&jobs, 0, sizeof(jobs));
    db_job_stats(&jobs);
    cJSON *response = cJSON_CreateObject();
    if (!response) { http_error(c, 500, "Out of memory"); return; }
    cJSON_AddBoolToObject(response, "accepting_jobs", s_accepting_jobs);
    cJSON_AddNumberToObject(response, "active_jobs", jobs.running);
    cJSON_AddNumberToObject(response, "queued_jobs", jobs.queued + jobs.created);
    char *serialized = cJSON_PrintUnformatted(response);
    cJSON_Delete(response);
    if (!serialized) { http_error(c, 500, "Out of memory"); return; }
    http_json_reply(c, 200, serialized);
    free(serialized);
}

static void list_workers(struct mg_connection *c, struct mg_http_message *hm)
{
    (void)hm;
    Machine *ms = NULL;
    int count = registry_snapshot(&ms);
    if (count < 0) { http_error(c, 500, "Failed to snapshot machines"); return; }
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < count; i++) {
        cJSON *m = cJSON_CreateObject();
        cJSON_AddStringToObject(m, "id",       ms[i].id);
        cJSON_AddStringToObject(m, "hostname", ms[i].hostname);
        cJSON_AddStringToObject(m, "ip",       ms[i].ip);
        cJSON_AddBoolToObject  (m, "enabled",  ms[i].enabled);
        cJSON_AddStringToObject(m, "status",
            ms[i].probe_status == MACHINE_ONLINE ? "online" :
            ms[i].probe_status == MACHINE_PROBING ? "probing" : "offline");
        cJSON_AddNumberToObject(m, "last_seen_at", (double)ms[i].last_probe_time);
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
    free(ms);
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
    int invalid = 0;
    invalid |= copy_optional_string(req, "id", m.id, sizeof(m.id)) < 0;
    invalid |= copy_optional_string(req, "hostname", m.hostname, sizeof(m.hostname)) < 0;
    invalid |= copy_optional_string(req, "ip", m.ip, sizeof(m.ip)) < 0;
    invalid |= json_optional_int(req, "cores", 1, 10000, &m.cores_total) < 0;
    invalid |= json_optional_int(req, "gpu_count", 0, 1000, &m.gpu_count_total) < 0;
    invalid |= json_optional_int(req, "ram_mb", 0, 10000000, &m.ram_mb_total) < 0;
    invalid |= json_optional_int(req, "disk_mb", 0, 10000000, &m.disk_mb_total) < 0;
    m.enabled = 1;
    cJSON *j = cJSON_GetObjectItemCaseSensitive(req, "enabled");
    if (cJSON_IsBool(j)) m.enabled = cJSON_IsTrue(j) ? 1 : 0;
    else if (j) invalid = 1;
    m.type = MACHINE_TYPE_STATIC;
    m.probe_status = MACHINE_PROBING;

    cJSON_Delete(req);

    if (invalid || !m.id[0] || m.cores_total < 1) {
        http_error(c, 400, "Invalid machine parameters");
        return;
    }

    /* Validate id, hostname, and ip: allow only safe characters */
    static const char *safe_host = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-:";
    const char *fields[] = { m.id, m.hostname, m.ip };
    for (int vi = 0; vi < 3; vi++) {
        if (!fields[vi][0]) continue;
        for (const char *vp = fields[vi]; *vp; vp++) {
            if (!strchr(safe_host, *vp)) {
                http_error(c, 400, "Invalid characters in machine field");
                return;
            }
        }
    }

    if (registry_upsert(&m) != 0) {
        http_error(c, 409, "Machine registry is full or machine is invalid");
        return;
    }
    http_json_reply(c, 201, "{\"ok\":true}");
}

static void remove_machine(struct mg_connection *c, struct mg_http_message *hm,
                            const char *machine_id)
{
    (void)hm;
    int result = registry_remove(machine_id);
    if (result == -2) {
        http_error(c, 409, "Machine still has reserved resources");
        return;
    }
    if (result != 0) {
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

    char uid_buf[128] = {0}, aid_buf[128] = {0};
    cJSON *j;
    j = cJSON_GetObjectItemCaseSensitive(req, "user_id");
    if (cJSON_IsString(j)) strncpy(uid_buf, j->valuestring, sizeof(uid_buf)-1);
    j = cJSON_GetObjectItemCaseSensitive(req, "app_id");
    if (cJSON_IsString(j)) strncpy(aid_buf, j->valuestring, sizeof(aid_buf)-1);
    cJSON_Delete(req);

    int rc = db_delete_quota(uid_buf, aid_buf);

    if (rc != 0) {
        http_error(c, 404, "Quota not found");
        return;
    }
    http_json_reply(c, 200, "{\"ok\":true}");
    log_info("routes", "Quota deleted: user=%s app=%s", uid_buf, aid_buf);
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
        cJSON_AddStringToObject(obj, "key_hash",   keys[i].key_hash);
        cJSON_AddStringToObject(obj, "label",      keys[i].label);
        cJSON_AddStringToObject(obj, "role",       keys[i].role);
        cJSON_AddStringToObject(obj, "user_id",    keys[i].user_id);
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

    char label[128] = "default";
    char role[16] = "user";
    char user_id[128] = {0};
    time_t expires_at = 0;

    cJSON *j;
    j = cJSON_GetObjectItemCaseSensitive(req, "label");
    if (j && !cJSON_IsString(j)) {
        cJSON_Delete(req);
        http_error(c, 400, "label must be a string");
        return;
    }
    if (cJSON_IsString(j)) {
        if (!j->valuestring[0] || strlen(j->valuestring) >= sizeof(label)) {
            cJSON_Delete(req);
            http_error(c, 400, "label must contain 1-127 characters");
            return;
        }
        for (const unsigned char *p = (const unsigned char *)j->valuestring; *p; p++) {
            if (*p < 0x20 || *p == 0x7f) {
                cJSON_Delete(req);
                http_error(c, 400, "label contains control characters");
                return;
            }
        }
        strncpy(label, j->valuestring, sizeof(label) - 1);
    }
    j = cJSON_GetObjectItemCaseSensitive(req, "role");
    if (j && !cJSON_IsString(j)) {
        cJSON_Delete(req);
        http_error(c, 400, "role must be a string");
        return;
    }
    if (cJSON_IsString(j)) {
        if (strlen(j->valuestring) >= sizeof(role)) {
            cJSON_Delete(req);
            http_error(c, 400, "role is too long");
            return;
        }
        strncpy(role, j->valuestring, sizeof(role) - 1);
    }
    j = cJSON_GetObjectItemCaseSensitive(req, "user_id");
    if (j && !cJSON_IsString(j)) {
        cJSON_Delete(req);
        http_error(c, 400, "user_id must be a string");
        return;
    }
    if (cJSON_IsString(j)) {
        if (strlen(j->valuestring) >= sizeof(user_id)) {
            cJSON_Delete(req);
            http_error(c, 400, "user_id is too long");
            return;
        }
        strncpy(user_id, j->valuestring, sizeof(user_id) - 1);
    }
    j = cJSON_GetObjectItemCaseSensitive(req, "expires_at");
    if (j && (!cJSON_IsNumber(j) || !isfinite(j->valuedouble) ||
              floor(j->valuedouble) != j->valuedouble ||
              j->valuedouble < 0 || j->valuedouble > 4102444800.0)) {
        cJSON_Delete(req);
        http_error(c, 400, "expires_at must be a valid Unix timestamp");
        return;
    }
    if (cJSON_IsNumber(j)) expires_at = (time_t)j->valuedouble;
    if (expires_at != 0 && expires_at <= time(NULL)) {
        cJSON_Delete(req);
        http_error(c, 400, "expires_at must be in the future");
        return;
    }

    /* Validate role */
    if (strcmp(role, "admin") != 0 && strcmp(role, "user") != 0) {
        cJSON_Delete(req);
        http_error(c, 400, "role must be 'admin' or 'user'");
        return;
    }
    if ((user_id[0] && !safe_cli_name(user_id)) ||
        (strcmp(role, "user") == 0 && !user_id[0])) {
        cJSON_Delete(req);
        http_error(c, 400, "A user key requires a valid user_id");
        return;
    }
    if (user_id[0]) {
        UserRecord user;
        if (db_get_user(user_id, &user) != 0 || !user.enabled) {
            cJSON_Delete(req);
            http_error(c, 400, "user_id does not reference an enabled user");
            return;
        }
    }

    /* Generate cryptographic random key */
    unsigned char raw[32] = {0};
#ifdef _WIN32
    {
        HCRYPTPROV hprov = 0;
        if (!CryptAcquireContextA(&hprov, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT) ||
            !CryptGenRandom(hprov, sizeof(raw), raw)) {
            if (hprov) CryptReleaseContext(hprov, 0);
            cJSON_Delete(req);
            http_error(c, 500, "Cannot generate random key");
            return;
        }
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

    char hash[65];
    auth_hash_key(raw_hex, hash);

    int rc = db_insert_api_key_full(hash, label, role, user_id, expires_at);
    cJSON_Delete(req);

    if (rc != 0) {
        http_error(c, 500, "Failed to create key (duplicate?)");
        return;
    }

    cJSON *resp = cJSON_CreateObject();
    if (!resp) { http_error(c, 500, "Out of memory"); return; }
    cJSON_AddStringToObject(resp, "api_key", raw_hex);
    cJSON_AddStringToObject(resp, "label",   label);
    cJSON_AddStringToObject(resp, "role",    role);
    cJSON_AddStringToObject(resp, "user_id", user_id);
    cJSON_AddNumberToObject(resp, "expires_at", (double)expires_at);
    char *s = cJSON_PrintUnformatted(resp);
    if (!s) {
        cJSON_Delete(resp);
        http_error(c, 500, "Out of memory");
        return;
    }
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
    cJSON *jhash = cJSON_GetObjectItemCaseSensitive(req, "key_hash");
    cJSON *identifier = cJSON_IsString(jhash) ? jhash : jkey;
    if (!cJSON_IsString(identifier) || strlen(identifier->valuestring) != 64) {
        cJSON_Delete(req);
        http_error(c, 400, "api_key or key_hash must contain 64 hexadecimal characters");
        return;
    }
    for (const unsigned char *p = (const unsigned char *)identifier->valuestring; *p; p++) {
        if (!isxdigit(*p)) {
            cJSON_Delete(req);
            http_error(c, 400, "api_key or key_hash must contain 64 hexadecimal characters");
            return;
        }
    }

    extern void auth_hash_key(const char *raw_key, char *out_hex_65);
    char hash[65];
    if (cJSON_IsString(jhash)) {
        strncpy(hash, jhash->valuestring, sizeof(hash) - 1);
        hash[sizeof(hash) - 1] = '\0';
    } else {
        auth_hash_key(jkey->valuestring, hash);
    }
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
        if (!safe_cli_name(juid->valuestring)) {
            cJSON *r = cJSON_CreateObject();
            cJSON_AddStringToObject(r, "user_id", juid->valuestring);
            cJSON_AddStringToObject(r, "error", "user_id must contain only letters, digits, '_' or '-'");
            cJSON_AddItemToArray(results, r);
            failed++;
            continue;
        }

        char password_hash[AUTH_PASSWORD_HASH_LEN] = {0};
        int has_password = 0;
        cJSON *jpwd = cJSON_GetObjectItemCaseSensitive(item, "password");
        if (jpwd && (!cJSON_IsString(jpwd) || strlen(jpwd->valuestring) < 12 ||
                     strlen(jpwd->valuestring) > 128 ||
                     auth_hash_password(jpwd->valuestring, password_hash) != 0)) {
            cJSON *r = cJSON_CreateObject();
            cJSON_AddStringToObject(r, "user_id", juid->valuestring);
            cJSON_AddStringToObject(r, "error", "password must contain 12-128 characters");
            cJSON_AddItemToArray(results, r);
            failed++;
            continue;
        }
        if (cJSON_IsString(jpwd)) has_password = 1;

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
            if (has_password && db_set_user_password(u.user_id, password_hash) != 0) {
                db_delete_user(u.user_id);
                cJSON *r = cJSON_CreateObject();
                cJSON_AddStringToObject(r, "user_id", u.user_id);
                cJSON_AddStringToObject(r, "error", "failed to store password");
                cJSON_AddItemToArray(results, r);
                failed++;
                continue;
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

    char uid_buf[128] = {0};
    strncpy(uid_buf, juid->valuestring, sizeof(uid_buf)-1);
    cJSON_Delete(req);

    int rc = db_delete_user(uid_buf);

    if (rc != 0) {
        http_error(c, 404, "User not found");
        return;
    }
    http_json_reply(c, 200, "{\"ok\":true}");
    log_info("routes", "User deleted: %s", uid_buf);
}

/* ── Application definitions ─────────────────────────────────────── */

static void apps_dir_path(char *buf, int len)
{
    if (len <= 0) return;
    if (g_config.apps_dir[0] == '/' || g_config.apps_dir[0] == '\\'
        || (g_config.apps_dir[0] && g_config.apps_dir[1] == ':')) {
        strncpy(buf, g_config.apps_dir, len - 1);
    } else {
        char prefix[512] = {0};
        exe_relative_path(g_config.apps_dir, prefix, sizeof(prefix));
        strncpy(buf, prefix, len - 1);
    }
    buf[len - 1] = '\0';
}

static cJSON *app_for_client(cJSON *app)
{
    cJSON *copy = cJSON_Duplicate(app, 1);
    if (copy) cJSON_DeleteItemFromObjectCaseSensitive(copy, "env");
    return copy;
}

static void list_apps(struct mg_connection *c, struct mg_http_message *hm)
{
    (void)hm;
    char dir[512]; apps_dir_path(dir, sizeof(dir));

    cJSON *arr = cJSON_CreateArray();
    if (!arr) { http_error(c, 500, "Out of memory"); return; }
#ifdef _WIN32
    {
        char pattern[520];
        _snprintf(pattern, sizeof(pattern), "%s\\*.json", dir);
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(pattern, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                size_t name_len = strlen(fd.cFileName);
                if (name_len > 5 && name_len - 5 <= 64) {
                    char app_id[65] = {0};
                    memcpy(app_id, fd.cFileName, name_len - 5);
                    char error[256] = {0};
                    cJSON *app = load_app_definition(app_id, error, sizeof(error));
                    cJSON *obj = app ? app_for_client(app) : NULL;
                    cJSON_Delete(app);
                    if (obj) cJSON_AddItemToArray(arr, obj);
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
                size_t name_len = strlen(ent->d_name);
                if (name_len > 5 && name_len - 5 <= 64) {
                    char app_id[65] = {0};
                    memcpy(app_id, ent->d_name, name_len - 5);
                    char error[256] = {0};
                    cJSON *app = load_app_definition(app_id, error, sizeof(error));
                    cJSON *obj = app ? app_for_client(app) : NULL;
                    cJSON_Delete(app);
                    if (obj) cJSON_AddItemToArray(arr, obj);
                }
            }
            closedir(d);
        }
    }
#endif
    char *s = cJSON_PrintUnformatted(arr);
    if (!s) { cJSON_Delete(arr); http_error(c, 500, "Out of memory"); return; }
    http_json_reply(c, 200, s);
    free(s);
    cJSON_Delete(arr);
}

static void get_app(struct mg_connection *c, struct mg_http_message *hm,
                    const char *app_id)
{
    (void)hm;
    if (!safe_cli_name(app_id)) { http_error(c, 400, "Invalid app_id"); return; }
    char error[256] = {0};
    cJSON *app = load_app_definition(app_id, error, sizeof(error));
    if (!app) { http_error(c, 404, error[0] ? error : "App not found"); return; }
    cJSON *public_app = app_for_client(app);
    cJSON_Delete(app);
    char *data = public_app ? cJSON_PrintUnformatted(public_app) : NULL;
    cJSON_Delete(public_app);
    if (!data) { http_error(c, 500, "Out of memory"); return; }
    http_json_reply(c, 200, data);
    free(data);
}

static void upsert_app(struct mg_connection *c, struct mg_http_message *hm)
{
    if (hm->body.len == 0 || hm->body.len > 65536) {
        http_error(c, 413, "App definition must not exceed 64 KiB");
        return;
    }
    cJSON *req = cJSON_ParseWithLength(hm->body.buf, hm->body.len);
    if (!req) { http_error(c, 400, "Invalid JSON"); return; }
    char validation_error[256] = {0};
    if (validate_app_definition(req, validation_error, sizeof(validation_error)) != 0) {
        cJSON_Delete(req);
        http_error(c, 400, validation_error);
        return;
    }
    cJSON *jid = cJSON_GetObjectItemCaseSensitive(req, "app_id");

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
    if (strlen(pretty) > 65536) {
        free(pretty);
        http_error(c, 413, "Formatted app definition exceeds 64 KiB");
        return;
    }

    char temp_path[800];
    snprintf(temp_path, sizeof(temp_path), "%s.tmp", fpath);
    FILE *f = fopen(temp_path, "wb");
    if (!f) { free(pretty); http_error(c, 500, "Cannot write app file"); return; }
    int write_failed = fwrite(pretty, 1, strlen(pretty), f) != strlen(pretty);
    if (fflush(f) != 0) write_failed = 1;
    if (fclose(f) != 0) write_failed = 1;
    free(pretty);
    if (write_failed) {
        remove(temp_path);
        http_error(c, 500, "Cannot write app file");
        return;
    }
#ifdef _WIN32
    if (!MoveFileExA(temp_path, fpath,
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
#else
    if (rename(temp_path, fpath) != 0) {
#endif
        remove(temp_path);
        http_error(c, 500, "Cannot replace app file");
        return;
    }

    http_json_reply(c, 200, "{\"ok\":true}");
    log_info("routes", "App definition saved: %s", fpath);
}

static void delete_app(struct mg_connection *c, struct mg_http_message *hm,
                       const char *app_id)
{
    (void)hm;
    if (!safe_cli_name(app_id)) { http_error(c, 400, "Invalid app_id"); return; }
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

/* ── Events endpoint ──────────────────────────────────────────────── */
static void admin_list_events(struct mg_connection *c, struct mg_http_message *hm)
{
    char category[64] = {0};
    time_t from_ts = 0, to_ts = 0;
    int limit = 200;

    struct mg_str q = hm->query;
    char tmp[64];
    if (mg_http_get_var(&q, "category", tmp, sizeof(tmp)) > 0)
        strncpy(category, tmp, sizeof(category)-1);
    if (mg_http_get_var(&q, "from", tmp, sizeof(tmp)) > 0)
        from_ts = (time_t)atoll(tmp);
    if (mg_http_get_var(&q, "to", tmp, sizeof(tmp)) > 0)
        to_ts = (time_t)atoll(tmp);
    if (mg_http_get_var(&q, "limit", tmp, sizeof(tmp)) > 0) {
        int l = atoi(tmp);
        if (l > 0 && l <= 1000) limit = l;
    }

    EventRecord *evts = (EventRecord *)calloc(limit, sizeof(EventRecord));
    if (!evts) { http_error(c, 500, "Out of memory"); return; }

    int n = db_list_events(evts, limit, category[0] ? category : NULL, from_ts, to_ts);

    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "id", evts[i].id);
        cJSON_AddStringToObject(o, "category", evts[i].category);
        cJSON_AddStringToObject(o, "event_type", evts[i].event_type);
        cJSON_AddStringToObject(o, "detail", evts[i].detail);
        cJSON_AddStringToObject(o, "user_id", evts[i].user_id);
        cJSON_AddStringToObject(o, "job_id", evts[i].job_id);
        cJSON_AddStringToObject(o, "machine_id", evts[i].machine_id);
        cJSON_AddNumberToObject(o, "created_at", (double)evts[i].created_at);
        cJSON_AddItemToArray(arr, o);
    }
    char *s = cJSON_PrintUnformatted(arr);
    http_json_reply(c, 200, s);
    free(s);
    cJSON_Delete(arr);
    free(evts);
}

/* ── Reporting endpoints ──────────────────────────────────────────── */
static void admin_report_jobs(struct mg_connection *c, struct mg_http_message *hm)
{
    char gran[16] = "day";
    time_t from_ts = 0, to_ts = 0;
    struct mg_str q = hm->query;
    char tmp[64];
    if (mg_http_get_var(&q, "granularity", tmp, sizeof(tmp)) > 0) {
        if (strcmp(tmp,"hour")==0 || strcmp(tmp,"day")==0 || strcmp(tmp,"month")==0)
            strncpy(gran, tmp, sizeof(gran)-1);
    }
    if (mg_http_get_var(&q, "from", tmp, sizeof(tmp)) > 0) from_ts = (time_t)atoll(tmp);
    if (mg_http_get_var(&q, "to", tmp, sizeof(tmp)) > 0)   to_ts = (time_t)atoll(tmp);

    JobTimeBucket buckets[365];
    int n = db_report_jobs_over_time(buckets, 365, gran, from_ts, to_ts);

    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "period", buckets[i].period);
        cJSON_AddNumberToObject(o, "total", buckets[i].total);
        cJSON_AddNumberToObject(o, "finished", buckets[i].finished);
        cJSON_AddNumberToObject(o, "failed", buckets[i].failed);
        cJSON_AddNumberToObject(o, "avg_duration_s", buckets[i].avg_duration_s);
        cJSON_AddItemToArray(arr, o);
    }
    char *s = cJSON_PrintUnformatted(arr);
    http_json_reply(c, 200, s);
    free(s); cJSON_Delete(arr);
}

static void admin_report_users(struct mg_connection *c, struct mg_http_message *hm)
{
    time_t from_ts = 0, to_ts = 0;
    struct mg_str q = hm->query;
    char tmp[64];
    if (mg_http_get_var(&q, "from", tmp, sizeof(tmp)) > 0) from_ts = (time_t)atoll(tmp);
    if (mg_http_get_var(&q, "to", tmp, sizeof(tmp)) > 0)   to_ts = (time_t)atoll(tmp);

    UserReport reports[100];
    int n = db_report_per_user(reports, 100, from_ts, to_ts);

    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "user_id", reports[i].user_id);
        cJSON_AddNumberToObject(o, "total_jobs", reports[i].total_jobs);
        cJSON_AddNumberToObject(o, "finished", reports[i].finished);
        cJSON_AddNumberToObject(o, "failed", reports[i].failed);
        cJSON_AddNumberToObject(o, "avg_duration_s", reports[i].avg_duration_s);
        cJSON_AddNumberToObject(o, "total_cores_used", reports[i].total_cores_used);
        cJSON_AddNumberToObject(o, "total_ram_mb_used", reports[i].total_ram_mb_used);
        cJSON_AddItemToArray(arr, o);
    }
    char *s = cJSON_PrintUnformatted(arr);
    http_json_reply(c, 200, s);
    free(s); cJSON_Delete(arr);
}

static void admin_report_apps(struct mg_connection *c, struct mg_http_message *hm)
{
    time_t from_ts = 0, to_ts = 0;
    struct mg_str q = hm->query;
    char tmp[64];
    if (mg_http_get_var(&q, "from", tmp, sizeof(tmp)) > 0) from_ts = (time_t)atoll(tmp);
    if (mg_http_get_var(&q, "to", tmp, sizeof(tmp)) > 0)   to_ts = (time_t)atoll(tmp);

    AppReport reports[100];
    int n = db_report_per_app(reports, 100, from_ts, to_ts);

    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "app_id", reports[i].app_id);
        cJSON_AddNumberToObject(o, "total_jobs", reports[i].total_jobs);
        cJSON_AddNumberToObject(o, "finished", reports[i].finished);
        cJSON_AddNumberToObject(o, "failed", reports[i].failed);
        cJSON_AddNumberToObject(o, "avg_duration_s", reports[i].avg_duration_s);
        cJSON_AddItemToArray(arr, o);
    }
    char *s = cJSON_PrintUnformatted(arr);
    http_json_reply(c, 200, s);
    free(s); cJSON_Delete(arr);
}

static void admin_report_machines(struct mg_connection *c, struct mg_http_message *hm)
{
    time_t from_ts = 0, to_ts = 0;
    struct mg_str q = hm->query;
    char tmp[64];
    if (mg_http_get_var(&q, "from", tmp, sizeof(tmp)) > 0) from_ts = (time_t)atoll(tmp);
    if (mg_http_get_var(&q, "to", tmp, sizeof(tmp)) > 0)   to_ts = (time_t)atoll(tmp);

    MachineReport reports[100];
    int n = db_report_per_machine(reports, 100, from_ts, to_ts);

    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "machine_id", reports[i].machine_id);
        cJSON_AddNumberToObject(o, "total_allocations", reports[i].total_allocations);
        cJSON_AddNumberToObject(o, "total_cores_reserved", reports[i].total_cores_reserved);
        cJSON_AddNumberToObject(o, "total_ram_mb_reserved", reports[i].total_ram_mb_reserved);
        cJSON_AddNumberToObject(o, "avg_utilization_pct", reports[i].avg_utilization_pct);
        cJSON_AddItemToArray(arr, o);
    }
    char *s = cJSON_PrintUnformatted(arr);
    http_json_reply(c, 200, s);
    free(s); cJSON_Delete(arr);
}

/* ── Machine status endpoint ──────────────────────────────────────── */
static void admin_machines_status(struct mg_connection *c, struct mg_http_message *hm)
{
    (void)hm;
    Machine *machines = NULL;
    int count = registry_snapshot(&machines);
    if (count < 0) { http_error(c, 500, "Failed to snapshot machines"); return; }

    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < count; i++) {
        Machine *m = &machines[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", m->id);
        cJSON_AddStringToObject(o, "hostname", m->hostname);
        cJSON_AddStringToObject(o, "ip", m->ip);
        cJSON_AddBoolToObject(o, "enabled", m->enabled);
        cJSON_AddStringToObject(o, "status",
            m->probe_status == MACHINE_ONLINE  ? "online" :
            m->probe_status == MACHINE_PROBING ? "probing" : "offline");
        cJSON_AddNumberToObject(o, "last_probe_time", (double)m->last_probe_time);
        cJSON_AddNumberToObject(o, "probe_fail_count", m->probe_fail_count);
        cJSON_AddStringToObject(o, "type",
            m->type == MACHINE_TYPE_CLOUD ? "cloud" : "static");
        cJSON_AddStringToObject(o, "cloud_provider", m->cloud_provider);
        cJSON_AddStringToObject(o, "cloud_instance_id", m->cloud_instance_id);
        cJSON_AddStringToObject(o, "mac_address", m->mac_address);
        cJSON_AddNumberToObject(o, "cores_total", m->cores_total);
        cJSON_AddNumberToObject(o, "cores_reserved", m->cores_reserved);
        cJSON_AddNumberToObject(o, "ram_mb_total", m->ram_mb_total);
        cJSON_AddNumberToObject(o, "ram_mb_reserved", m->ram_mb_reserved);
        cJSON_AddItemToArray(arr, o);
    }
    free(machines);
    char *s = cJSON_PrintUnformatted(arr);
    http_json_reply(c, 200, s);
    free(s); cJSON_Delete(arr);
}

/* ── Cloud provisioning endpoints ─────────────────────────────────── */
static void admin_cloud_provision(struct mg_connection *c, struct mg_http_message *hm)
{
    cJSON *body = cJSON_ParseWithLength(hm->body.buf, hm->body.len);
    if (!body) { http_error(c, 400, "Invalid JSON"); return; }

    CloudMachineSpec spec;
    memset(&spec, 0, sizeof(spec));

    int invalid = 0;
    invalid |= copy_optional_string(body, "provider", spec.provider, sizeof(spec.provider)) < 0;
    invalid |= copy_optional_string(body, "instance_type", spec.instance_type, sizeof(spec.instance_type)) < 0;
    invalid |= copy_optional_string(body, "region", spec.region, sizeof(spec.region)) < 0;
    invalid |= copy_optional_string(body, "image_id", spec.image_id, sizeof(spec.image_id)) < 0;
    invalid |= copy_optional_string(body, "tags", spec.tags, sizeof(spec.tags)) < 0;
    invalid |= json_optional_int(body, "cores", 0, 10000, &spec.cores) < 0;
    invalid |= json_optional_int(body, "gpu_count", 0, 1000, &spec.gpu_count) < 0;
    invalid |= json_optional_int(body, "ram_mb", 0, 10000000, &spec.ram_mb) < 0;
    invalid |= json_optional_int(body, "disk_mb", 0, 10000000, &spec.disk_mb) < 0;
    invalid |= json_optional_int(body, "cores_min", 0, 10000, &spec.cores_min) < 0;
    invalid |= json_optional_int(body, "ram_mb_min", 0, 10000000, &spec.ram_mb_min) < 0;
    invalid |= json_optional_int(body, "disk_mb_min", 0, 10000000, &spec.disk_mb_min) < 0;
    cJSON_Delete(body);

    int provider_valid = strcmp(spec.provider, "aws") == 0 ||
                         strcmp(spec.provider, "gcp") == 0 ||
                         strcmp(spec.provider, "azure") == 0;
    if (invalid || !provider_valid ||
        (spec.instance_type[0] && !safe_cloud_arg(spec.instance_type)) ||
        (spec.region[0] && !safe_cloud_arg(spec.region)) ||
        (spec.image_id[0] && !safe_cloud_arg(spec.image_id)) ||
        (strcmp(spec.provider, "aws") == 0 && !spec.image_id[0])) {
        http_error(c, 400, "Invalid cloud provisioning parameters");
        return;
    }
    int effective_cores = spec.cores > 0 ? spec.cores : 2;
    int effective_ram = spec.ram_mb > 0 ? spec.ram_mb : 4096;
    int effective_disk = spec.disk_mb > 0 ? spec.disk_mb : 50000;
    if (spec.cores_min > effective_cores || spec.ram_mb_min > effective_ram ||
        spec.disk_mb_min > effective_disk) {
        http_error(c, 400, "Cloud resource minimums cannot exceed totals");
        return;
    }

    char out_id[128] = {0};
    if (cloud_provision(&spec, out_id, sizeof(out_id)) != 0) {
        http_error(c, 500, "Cloud provisioning failed"); return;
    }

    events_push_persistent("cloud", "provision", out_id, "");

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "machine_id", out_id);
    cJSON_AddStringToObject(resp, "provider", spec.provider);
    cJSON_AddStringToObject(resp, "status", "provisioning");
    char *s = cJSON_PrintUnformatted(resp);
    http_json_reply(c, 201, s);
    free(s); cJSON_Delete(resp);
}

static void admin_cloud_deprovision(struct mg_connection *c, struct mg_http_message *hm)
{
    cJSON *body = cJSON_ParseWithLength(hm->body.buf, hm->body.len);
    if (!body) { http_error(c, 400, "Invalid JSON"); return; }

    char provider[32] = {0}, instance_id[128] = {0};
    int invalid = copy_optional_string(body, "provider", provider, sizeof(provider)) < 0 ||
                  copy_optional_string(body, "instance_id", instance_id, sizeof(instance_id)) < 0;
    cJSON_Delete(body);

    int provider_valid = strcmp(provider, "aws") == 0 ||
                         strcmp(provider, "gcp") == 0 ||
                         strcmp(provider, "azure") == 0;
    if (invalid || !provider_valid || !safe_cloud_arg(instance_id)) {
        http_error(c, 400, "Invalid provider or instance_id"); return;
    }

    char registry_id[160];
    snprintf(registry_id, sizeof(registry_id), "cloud-%s", instance_id);
    Machine registered;
    if (registry_get_copy(registry_id, &registered) == 0) {
        if (strcmp(registered.cloud_provider, provider) != 0) {
            http_error(c, 409, "Cloud provider does not match registered machine");
            return;
        }
        if (registered.cores_reserved > 0 || registered.gpu_count_reserved > 0 ||
            registered.ram_mb_reserved > 0 || registered.disk_mb_reserved > 0) {
            http_error(c, 409, "Cloud machine still has reserved resources");
            return;
        }
    }

    if (cloud_deprovision(provider, instance_id) != 0) {
        http_error(c, 500, "Cloud deprovision failed"); return;
    }

    events_push_persistent("cloud", "deprovision", instance_id, "");
    http_json_reply(c, 200, "{\"ok\":true}");
}

/* ── WoL endpoint ─────────────────────────────────────────────────── */
static void admin_wol(struct mg_connection *c, struct mg_http_message *hm)
{
    cJSON *body = cJSON_ParseWithLength(hm->body.buf, hm->body.len);
    if (!body) { http_error(c, 400, "Invalid JSON"); return; }

    char machine_id[64] = {0}, broadcast_ip[46] = {0};
    cJSON *j;
    j = cJSON_GetObjectItemCaseSensitive(body, "machine_id");
    if (cJSON_IsString(j)) strncpy(machine_id, j->valuestring, sizeof(machine_id)-1);
    j = cJSON_GetObjectItemCaseSensitive(body, "broadcast_ip");
    if (cJSON_IsString(j)) strncpy(broadcast_ip, j->valuestring, sizeof(broadcast_ip)-1);
    cJSON_Delete(body);

    if (!machine_id[0]) { http_error(c, 400, "Missing machine_id"); return; }

    Machine machine;
    if (registry_get_copy(machine_id, &machine) != 0) {
        http_error(c, 404, "Machine not found"); return;
    }
    if (!machine.mac_address[0]) {
        http_error(c, 400, "Machine has no MAC address configured"); return;
    }

    if (wol_send(machine.mac_address, broadcast_ip[0] ? broadcast_ip : NULL) != 0) {
        http_error(c, 500, "WoL send failed"); return;
    }

    events_push_persistent("machine", "wol", machine.id, "");
    http_json_reply(c, 200, "{\"ok\":true,\"message\":\"WoL packet sent\"}");
}

/* GET/POST /admin/presim-config — view or update presim runtime settings (admin only) */
static void admin_get_presim_config(struct mg_connection *c, struct mg_http_message *hm)
{
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(obj, "presim_threshold_max", g_config.presim_threshold_max);
    cJSON_AddNumberToObject(obj, "presim_refine_multiplier", g_config.presim_refine_multiplier);
    cJSON_AddNumberToObject(obj, "presim_high_multiplier", g_config.presim_high_multiplier);
    cJSON_AddNumberToObject(obj, "presim_uncertainty_weight", g_config.presim_uncertainty_weight);
    cJSON_AddStringToObject(obj, "presim_fidelity_map", g_config.presim_fidelity_map);
    cJSON_AddStringToObject(obj, "presim_domains", g_config.presim_domains);
    char *body = cJSON_PrintUnformatted(obj);
    http_json_reply(c, 200, body);
    free(body); cJSON_Delete(obj);
}

static void admin_update_presim_config(struct mg_connection *c, struct mg_http_message *hm)
{
    cJSON *body = cJSON_ParseWithLength(hm->body.buf, hm->body.len);
    if (!body) { http_error(c, 400, "Invalid JSON"); return; }
    cJSON *j;
    j = cJSON_GetObjectItemCaseSensitive(body, "presim_threshold_max");
    if (cJSON_IsNumber(j)) g_config.presim_threshold_max = j->valuedouble;
    j = cJSON_GetObjectItemCaseSensitive(body, "presim_refine_multiplier");
    if (cJSON_IsNumber(j)) g_config.presim_refine_multiplier = j->valuedouble;
    j = cJSON_GetObjectItemCaseSensitive(body, "presim_high_multiplier");
    if (cJSON_IsNumber(j)) g_config.presim_high_multiplier = j->valuedouble;
    j = cJSON_GetObjectItemCaseSensitive(body, "presim_uncertainty_weight");
    if (cJSON_IsNumber(j)) g_config.presim_uncertainty_weight = j->valuedouble;
    j = cJSON_GetObjectItemCaseSensitive(body, "presim_fidelity_map");
    if (cJSON_IsString(j)) strncpy(g_config.presim_fidelity_map, j->valuestring, sizeof(g_config.presim_fidelity_map)-1);
    j = cJSON_GetObjectItemCaseSensitive(body, "presim_domains");
    if (cJSON_IsString(j)) strncpy(g_config.presim_domains, j->valuestring, sizeof(g_config.presim_domains)-1);
    cJSON_Delete(body);
    http_json_reply(c, 200, "{\"ok\":true}");
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

    if (g_config.require_https) {
        struct mg_str *forwarded_proto = mg_http_get_header(hm, "X-Forwarded-Proto");
        if (!forwarded_proto || forwarded_proto->len != 5 ||
            memcmp(forwarded_proto->buf, "https", 5) != 0) {
            http_error(c, 426, "HTTPS required");
            return;
        }
    }

    /* ── CORS preflight ────────────────────────────────────────── */
    if (strcmp(method, "OPTIONS") == 0) {
        struct mg_str *origin = mg_http_get_header(hm, "Origin");
        size_t allowed_len = strlen(g_config.cors_allowed_origin);
        if (!allowed_len || !origin || origin->len != allowed_len ||
            memcmp(origin->buf, g_config.cors_allowed_origin, allowed_len) != 0) {
            http_error(c, 403, "Cross-origin request denied");
            return;
        }
        char headers[1792] = {0};
        http_build_headers(headers, sizeof(headers), "text/plain; charset=utf-8");
        strncat(headers,
            "Access-Control-Allow-Methods: GET, POST, PUT, DELETE, OPTIONS\r\n"
            "Access-Control-Allow-Headers: Content-Type, X-API-Key, Idempotency-Key\r\n"
            "Access-Control-Max-Age: 86400\r\n",
            sizeof(headers) - strlen(headers) - 1);
        mg_http_reply(c, 204, headers, "");
        return;
    }

    /* ── Web UI (public) ───────────────────────────────────────── */
    if (seg[0][0] == '\0' || strcmp(seg[0], "ui") == 0) {
        if (!g_config.web_ui_enabled) {
            http_error(c, 403, "Web UI disabled (bastion/API-only mode)");
            return;
        }
        serve_web_ui(c); return;
    }
    /* ── Static web assets (public, no auth) ─────────────────── */
    if (strcmp(seg[0], "web") == 0 && strcmp(method, "GET") == 0 && seg[1][0]) {
        if (!g_config.web_ui_enabled) {
            http_error(c, 403, "Web UI disabled (bastion/API-only mode)");
            return;
        }
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
        if (strcmp(method, "GET") == 0 && strcmp(seg[1], "me") == 0) {
            auth_me(c, auth_user_id, auth_role); return;
        }
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

    /* Admin: presim config endpoints */
    if (strcmp(seg[0], "admin") == 0 && strcmp(seg[1], "presim-config") == 0) {
        if (!g_config.experimental_features_enabled) {
            http_error(c, 404, "Experimental feature disabled"); return;
        }
        if (strcmp(method, "GET") == 0) { admin_get_presim_config(c, hm); return; }
        if (strcmp(method, "POST") == 0) { admin_update_presim_config(c, hm); return; }
        http_error(c, 404, "Not found"); return;
    }

    if (strcmp(seg[0], "admin") == 0 && strcmp(seg[1], "maintenance") == 0) {
        if (strcmp(method, "GET") == 0) { admin_maintenance(c, hm, 0); return; }
        if (strcmp(method, "POST") == 0) { admin_maintenance(c, hm, 1); return; }
        http_error(c, 404, "Not found"); return;
    }

    if (strcmp(seg[0], "metrics") == 0) {
        if (strcmp(auth_role, "admin") != 0) {
            http_error(c, 403, "Forbidden: admin role required");
            return;
        }
        if (strcmp(method, "GET") == 0 && seg[1][0] == '\0') {
            get_metrics(c, hm);
            return;
        }
        http_error(c, 404, "Not found");
        return;
    }

    /* /jobs */
    if (strcmp(seg[0], "jobs") == 0) {
        if (strcmp(method, "POST") == 0 && seg[1][0] == '\0') {
            submit_job(c, hm, auth_user_id, auth_role); return;
        }
        if (strcmp(method, "GET") == 0 && seg[1][0] == '\0') {
            list_jobs(c, hm, auth_user_id, auth_role); return;
        }
        /* DELETE /jobs — purge all terminal jobs (admin only) */
        if (strcmp(method, "DELETE") == 0 && seg[1][0] == '\0') {
            if (strcmp(auth_role, "admin") != 0) {
                http_error(c, 403, "Forbidden: admin role required");
                return;
            }
            purge_jobs(c, hm); return;
        }
        if (strcmp(method, "GET") == 0 && strcmp(seg[1], "events") == 0) {
            sse_subscribe(c, hm, auth_user_id, auth_role); return;
        }
        if (seg[1][0] != '\0') {
            if (!safe_cli_name(seg[1])) {
                http_error(c, 404, "Job not found");
                return;
            }
            /* Enforce job ownership for non-admin users */
            if (strcmp(auth_role, "admin") != 0) {
                Job *_chk = db_get_job(seg[1]);
                if (!_chk || !_chk->user_id[0] ||
                    strcmp(_chk->user_id, auth_user_id) != 0) {
                    if (_chk) job_free(_chk);
                    http_error(c, 404, "Job not found");
                    return;
                }
                job_free(_chk);
            }
            if (strcmp(method, "GET") == 0 && seg[2][0] == '\0') {
                get_job(c, hm, seg[1]); return;
            }
            if (strcmp(method, "DELETE") == 0 && seg[2][0] == '\0') {
                cancel_job(c, hm, seg[1]); return;
            }
            if (strcmp(method, "POST") == 0 && strcmp(seg[2], "cancel") == 0 && seg[3][0] == '\0') {
                cancel_job(c, hm, seg[1]); return;
            }
            if (strcmp(method, "POST") == 0 && strcmp(seg[2], "retry") == 0 && seg[3][0] == '\0') {
                retry_job(c, hm, seg[1]); return;
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
            if (strcmp(method, "GET") == 0 && strcmp(seg[2], "artifacts") == 0 && seg[3][0] == '\0') {
                artifacts_list(c, seg[1]); return;
            }
            if (strcmp(method, "GET") == 0 && strcmp(seg[2], "deps") == 0 && seg[3][0] == '\0') {
                get_job_deps(c, hm, seg[1]); return;
            }
            if (strcmp(method, "GET") == 0 && strcmp(seg[2], "log") == 0) {
                get_job_log(c, hm, seg[1], strcmp(seg[3], "stderr") == 0); return;
            }
            if (strcmp(method, "GET") == 0 && strcmp(seg[2], "logs") == 0) {
                get_job_log(c, hm, seg[1], strcmp(seg[3], "stderr") == 0); return;
            }
        }
    }

    if (strcmp(seg[0], "queue") == 0 && strcmp(method, "GET") == 0 && seg[1][0] == '\0') {
        list_queue(c, hm, auth_user_id, auth_role); return;
    }

    /* /apps — available to all authenticated users */
    if (strcmp(seg[0], "batches") == 0) {
        if (strcmp(method, "POST") == 0 && seg[1][0] == '\0') {
            batches_submit(c, hm, auth_user_id); return;
        }
        if (strcmp(method, "GET") == 0 && seg[1][0] != '\0' && seg[2][0] == '\0') {
            batches_get(c, seg[1], auth_user_id, auth_role); return;
        }
        http_error(c, 404, "Not found"); return;
    }

    if (strcmp(seg[0], "apps") == 0) {
        if (strcmp(method, "GET") == 0 && seg[1][0] == '\0') {
            list_apps(c, hm); return;
        }
        if (strcmp(method, "GET") == 0 && seg[1][0] != '\0' && seg[2][0] == '\0') {
            get_app(c, hm, seg[1]); return;
        }
    }

    /* /workflows — saved workflow templates + submit */
    if (strcmp(seg[0], "workflows") == 0) {
        if (!g_config.experimental_features_enabled) {
            http_error(c, 404, "Experimental feature disabled"); return;
        }
        if (strcmp(method, "GET") == 0 && seg[1][0] == '\0') {
            list_saved_workflows(c, auth_user_id, auth_role); return;
        }
        if (strcmp(method, "POST") == 0 && seg[1][0] == '\0') {
            /* Check if body has 'steps' at top-level → submit, else save */
            /* We peek at the body to decide: if it has 'name' but no 'steps'
               top-level submit uses 'steps', save also uses 'steps' + 'name'.
               We'll route: POST /workflows/run → submit, POST /workflows → save */
            save_workflow(c, hm, auth_user_id, auth_role); return;
        }
        if (strcmp(method, "POST") == 0 && strcmp(seg[1], "run") == 0 && seg[2][0] == '\0') {
            submit_workflow(c, hm, auth_user_id, auth_role); return;
        }
        if (strcmp(method, "DELETE") == 0 && seg[1][0] != '\0' && seg[2][0] == '\0') {
            delete_saved_workflow(c, seg[1], auth_user_id, auth_role); return;
        }
        if (strcmp(method, "POST") == 0 && seg[1][0] != '\0' &&
            strcmp(seg[2], "favorite") == 0 && seg[3][0] == '\0') {
            toggle_workflow_favorite(c, hm, seg[1], auth_user_id); return;
        }
    }

    if (strcmp(seg[0], "workers") == 0) {
        if (strcmp(method, "GET") == 0 && seg[1][0] == '\0') {
            list_workers(c, hm); return;
        }
        if (strcmp(auth_role, "admin") != 0) {
            http_error(c, 403, "Forbidden: admin role required"); return;
        }
        if (strcmp(method, "POST") == 0 && seg[1][0] == '\0') {
            add_machine(c, hm); return;
        }
        if (strcmp(method, "DELETE") == 0 && seg[1][0] != '\0' && seg[2][0] == '\0') {
            remove_machine(c, hm, seg[1]); return;
        }
        http_error(c, 404, "Not found"); return;
    }

    /* Legacy v0 route retained during the v1 migration. */
    if (strcmp(seg[0], "resources") == 0 && strcmp(method, "GET") == 0) {
        list_workers(c, hm); return;
    }

    if (strcmp(seg[0], "stats") == 0 && strcmp(method, "GET") == 0) {
        get_stats(c, hm); return;
    }

    if (strcmp(seg[0], "provision") == 0) {
        if (strcmp(auth_role, "admin") != 0) {
            http_error(c, 403, "Forbidden: admin role required");
            return;
        }
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

    /* /admin/events — list persistent events */
    if (strcmp(seg[0], "admin") == 0 && strcmp(seg[1], "events") == 0) {
        if (strcmp(method, "GET") == 0) {
            admin_list_events(c, hm); return;
        }
    }

    /* /admin/reports — reporting analytics */
    if (strcmp(seg[0], "admin") == 0 && strcmp(seg[1], "reports") == 0) {
        if (strcmp(method, "GET") == 0 && strcmp(seg[2], "jobs") == 0) {
            admin_report_jobs(c, hm); return;
        }
        if (strcmp(method, "GET") == 0 && strcmp(seg[2], "users") == 0) {
            admin_report_users(c, hm); return;
        }
        if (strcmp(method, "GET") == 0 && strcmp(seg[2], "apps") == 0) {
            admin_report_apps(c, hm); return;
        }
        if (strcmp(method, "GET") == 0 && strcmp(seg[2], "machines") == 0) {
            admin_report_machines(c, hm); return;
        }
    }

    /* /admin/machines/status — probe status of all machines */
    if (strcmp(seg[0], "admin") == 0 && strcmp(seg[1], "machines") == 0 &&
        strcmp(seg[2], "status") == 0 && strcmp(method, "GET") == 0) {
        admin_machines_status(c, hm); return;
    }

    /* /admin/cloud — cloud provisioning */
    if (strcmp(seg[0], "admin") == 0 && strcmp(seg[1], "cloud") == 0) {
        if (!g_config.experimental_features_enabled) {
            http_error(c, 404, "Experimental feature disabled"); return;
        }
        if (strcmp(method, "POST") == 0 && strcmp(seg[2], "provision") == 0) {
            admin_cloud_provision(c, hm); return;
        }
        if (strcmp(method, "POST") == 0 && strcmp(seg[2], "deprovision") == 0) {
            admin_cloud_deprovision(c, hm); return;
        }
    }

    /* /admin/wol — Wake-on-LAN */
    if (strcmp(seg[0], "admin") == 0 && strcmp(seg[1], "wol") == 0) {
        if (!g_config.experimental_features_enabled) {
            http_error(c, 404, "Experimental feature disabled"); return;
        }
        if (strcmp(method, "POST") == 0) {
            admin_wol(c, hm); return;
        }
    }

    http_error(c, 404, "Not found");
}
