#include "http.h"
#include "job.h"
#include "queue.h"
#include "scheduler.h"
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

/* Clamp a double to int within [lo, hi] */
static int clamp_int(double v, int lo, int hi)
{
    if (v < (double)lo) return lo;
    if (v > (double)hi) return hi;
    return (int)v;
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
    if (strstr(file_path, "..") || strchr(file_path, '\0') ||
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
        !juid->valuestring[0] || !jpwd->valuestring[0]) {
        cJSON_Delete(req);
        http_error(c, 400, "Missing 'user_id' and/or 'password'");
        return;
    }

    /* Copy values before freeing JSON */
    char uid[128] = {0};
    strncpy(uid, juid->valuestring, sizeof(uid)-1);
    char password[256] = {0};
    strncpy(password, jpwd->valuestring, sizeof(password)-1);
    cJSON_Delete(req);

    /* Verify password (supports salted + legacy unsalted hashes) */
    char stored_hash[128] = {0};
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
    char stored_hash[128] = {0};
    int _enabled = 0;
    if (db_get_user_auth(auth_user_id, stored_hash, sizeof(stored_hash), &_enabled) != 0 ||
        !auth_verify_password(jold->valuestring, stored_hash)) {
        cJSON_Delete(req);
        http_error(c, 401, "Old password is incorrect");
        return;
    }

    /* Set new password (salted) */
    char new_hash[98];
    auth_hash_password(jnew->valuestring, new_hash);
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

/* forward declaration — defined below with other app routes */
static void apps_dir_path(char *buf, int len);

/* ── App template resolution ─────────────────────────────────────── */

/*
 * Load an app JSON file by app_id, resolve command_template with user params.
 * Writes resolved command into out_cmd (max out_cmd_len).
 * Writes app env JSON string into out_env_json (caller must free).
 * Returns 0 on success, -1 on error (writes error into err_buf).
 */
static int resolve_app_command(const char *app_id, cJSON *user_params,
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
    fread(data, 1, sz, f); fclose(f); data[sz] = '\0';

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
                if (!decl) {
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
    cJSON *jfields = cJSON_GetObjectItemCaseSensitive(app, "fields");
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
    cJSON *jparams = cJSON_GetObjectItemCaseSensitive(req, "parameters");
    if (!jcor)  jcor  = cJSON_GetObjectItemCaseSensitive(req, "cores");
    if (!jgpu)  jgpu  = cJSON_GetObjectItemCaseSensitive(req, "gpu");
    if (!jram)  jram  = cJSON_GetObjectItemCaseSensitive(req, "ram_mb");
    if (!jdisk) jdisk = cJSON_GetObjectItemCaseSensitive(req, "disk_mb");

    int app_only = (strcmp(g_config.command_mode, "app_only") == 0);
    char resolved_cmd[JOB_CMD_LEN] = {0};
    char *app_env_json = NULL;

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
                                &app_env_json, err, sizeof(err)) != 0) {
            cJSON_Delete(req);
            http_error(c, 400, err);
            return;
        }
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
                                &app_env_json, err, sizeof(err));
            /* Ignore errors — env is best-effort in free mode */
        }
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

    /* ── Dependency handling ─────────────────────────────────── */
    char depends_on_str[2048] = {0};
    cJSON *jdeps = cJSON_GetObjectItemCaseSensitive(req, "depends_on");
    if (cJSON_IsArray(jdeps)) {
        cJSON *dep;
        cJSON_ArrayForEach(dep, jdeps) {
            if (cJSON_IsString(dep) && dep->valuestring[0]) {
                /* Validate that each dep job exists */
                Job *depjob = db_get_job(dep->valuestring);
                if (!depjob) {
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
        if (!depjob) {
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
        cJSON_IsNumber(jcor)  ? clamp_int(jcor->valuedouble,  1, 10000)   : 1,
        cJSON_IsNumber(jgpu)  ? clamp_int(jgpu->valuedouble,  0, 1000)    : 0,
        cJSON_IsNumber(jram)  ? clamp_int(jram->valuedouble,  0, 10000000) : 0,
        cJSON_IsNumber(jdisk) ? clamp_int(jdisk->valuedouble, 0, 10000000) : 0,
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

    if (is_held) {
        strncpy(job->input_files, input_files_str, sizeof(job->input_files) - 1);
        job->status = JOB_STATUS_HELD;
        db_update_job_status(job->id, JOB_STATUS_HELD, 0, 0);
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

static void submit_workflow(struct mg_connection *c, struct mg_http_message *hm,
                            const char *auth_user_id)
{
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

    /* Track created job IDs so we can map step indices to real IDs */
    char job_ids[MAX_WORKFLOW_STEPS][64] = {{0}};
    Job *created_jobs[MAX_WORKFLOW_STEPS] = {NULL};
    int created = 0;

    /* Generate a batch workflow_id to group these jobs */
    char wf_batch_id[64];
    gen_wf_id(wf_batch_id, sizeof(wf_batch_id));

    cJSON *step = NULL;
    int idx = 0;
    cJSON_ArrayForEach(step, jsteps) {
        if (!cJSON_IsObject(step)) {
            /* Rollback: cancel already-created jobs */
            for (int r = 0; r < created; r++) {
                job_set_status_r(created_jobs[r], JOB_STATUS_CANCELLED,
                                 "Workflow submission failed");
                job_free(created_jobs[r]);
            }
            cJSON_Delete(req);
            http_error(c, 400, "Each step must be a JSON object");
            return;
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

        /* Resolve command */
        char resolved_cmd[JOB_CMD_LEN] = {0};
        char *app_env_json = NULL;

        if (app_only) {
            if (cJSON_IsString(jcmd)) {
                for (int r = 0; r < created; r++) {
                    job_set_status_r(created_jobs[r], JOB_STATUS_CANCELLED, "Workflow submission failed");
                    job_free(created_jobs[r]);
                }
                cJSON_Delete(req);
                http_error(c, 400, "Raw 'command' not allowed in app_only mode");
                return;
            }
            if (!cJSON_IsString(japp) || !japp->valuestring[0]) {
                for (int r = 0; r < created; r++) {
                    job_set_status_r(created_jobs[r], JOB_STATUS_CANCELLED, "Workflow submission failed");
                    job_free(created_jobs[r]);
                }
                cJSON_Delete(req);
                char err[128];
                snprintf(err, sizeof(err), "Step %d: missing 'app_id' (required in app_only mode)", idx);
                http_error(c, 400, err);
                return;
            }
            char err[256];
            if (resolve_app_command(japp->valuestring, jparams,
                                    resolved_cmd, sizeof(resolved_cmd),
                                    &app_env_json, err, sizeof(err)) != 0) {
                for (int r = 0; r < created; r++) {
                    job_set_status_r(created_jobs[r], JOB_STATUS_CANCELLED, "Workflow submission failed");
                    job_free(created_jobs[r]);
                }
                cJSON_Delete(req);
                char msg[320];
                snprintf(msg, sizeof(msg), "Step %d: %s", idx, err);
                http_error(c, 400, msg);
                return;
            }
        } else {
            if (cJSON_IsString(jcmd)) {
                strncpy(resolved_cmd, jcmd->valuestring, sizeof(resolved_cmd) - 1);
            } else if (cJSON_IsString(japp) && japp->valuestring[0]) {
                char err[256];
                if (resolve_app_command(japp->valuestring, jparams,
                                        resolved_cmd, sizeof(resolved_cmd),
                                        &app_env_json, err, sizeof(err)) != 0) {
                    for (int r = 0; r < created; r++) {
                        job_set_status_r(created_jobs[r], JOB_STATUS_CANCELLED, "Workflow submission failed");
                        job_free(created_jobs[r]);
                    }
                    cJSON_Delete(req);
                    char msg[320];
                    snprintf(msg, sizeof(msg), "Step %d: %s", idx, err);
                    http_error(c, 400, msg);
                    return;
                }
            } else {
                for (int r = 0; r < created; r++) {
                    job_set_status_r(created_jobs[r], JOB_STATUS_CANCELLED, "Workflow submission failed");
                    job_free(created_jobs[r]);
                }
                cJSON_Delete(req);
                char msg[128];
                snprintf(msg, sizeof(msg), "Step %d: missing 'command' or 'app_id'", idx);
                http_error(c, 400, msg);
                return;
            }
        }

        /* Build depends_on from step indices -> real job IDs */
        char depends_on_str[2048] = {0};
        if (cJSON_IsArray(jdep_s)) {
            cJSON *di = NULL;
            cJSON_ArrayForEach(di, jdep_s) {
                if (!cJSON_IsNumber(di)) continue;
                int dep_idx = (int)di->valuedouble;
                if (dep_idx < 0 || dep_idx >= created) {
                    free(app_env_json);
                    for (int r = 0; r < created; r++) {
                        job_set_status_r(created_jobs[r], JOB_STATUS_CANCELLED, "Workflow submission failed");
                        job_free(created_jobs[r]);
                    }
                    cJSON_Delete(req);
                    char msg[128];
                    snprintf(msg, sizeof(msg),
                        "Step %d: depends_on_steps[%d] references invalid step", idx, dep_idx);
                    http_error(c, 400, msg);
                    return;
                }
                if (depends_on_str[0])
                    strncat(depends_on_str, ",", sizeof(depends_on_str) - strlen(depends_on_str) - 1);
                strncat(depends_on_str, job_ids[dep_idx],
                        sizeof(depends_on_str) - strlen(depends_on_str) - 1);
            }
        }

        /* Also support explicit depends_on with external job IDs */
        cJSON *jdeps_ext = cJSON_GetObjectItemCaseSensitive(step, "depends_on");
        if (cJSON_IsArray(jdeps_ext)) {
            cJSON *dep = NULL;
            cJSON_ArrayForEach(dep, jdeps_ext) {
                if (!cJSON_IsString(dep) || !dep->valuestring[0]) continue;
                Job *depjob = db_get_job(dep->valuestring);
                if (!depjob) {
                    free(app_env_json);
                    for (int r = 0; r < created; r++) {
                        job_set_status_r(created_jobs[r], JOB_STATUS_CANCELLED, "Workflow submission failed");
                        job_free(created_jobs[r]);
                    }
                    cJSON_Delete(req);
                    char msg[256];
                    snprintf(msg, sizeof(msg), "Step %d: dependency job not found: %s",
                             idx, dep->valuestring);
                    http_error(c, 400, msg);
                    return;
                }
                job_free(depjob);
                if (depends_on_str[0])
                    strncat(depends_on_str, ",", sizeof(depends_on_str) - strlen(depends_on_str) - 1);
                strncat(depends_on_str, dep->valuestring,
                        sizeof(depends_on_str) - strlen(depends_on_str) - 1);
            }
        }

        /* Build input_files string from step */
        char input_files_str[2048] = {0};
        cJSON *jinfiles = cJSON_GetObjectItemCaseSensitive(step, "input_files");
        if (cJSON_IsArray(jinfiles)) {
            cJSON *f = NULL;
            cJSON_ArrayForEach(f, jinfiles) {
                if (cJSON_IsString(f) && f->valuestring[0]) {
                    if (input_files_str[0])
                        strncat(input_files_str, ",", sizeof(input_files_str) - strlen(input_files_str) - 1);
                    strncat(input_files_str, f->valuestring, sizeof(input_files_str) - strlen(input_files_str) - 1);
                }
            }
        }

        const char *app_id = cJSON_IsString(japp) ? japp->valuestring : "";
        int job_timeout = cJSON_IsNumber(jtout) ? clamp_int(jtout->valuedouble, 0, 604800) : 0;

        Job *job = job_create_ex(
            resolved_cmd,
            cJSON_IsNumber(jpri)  ? clamp_int(jpri->valuedouble,  0, 100)     : 50,
            cJSON_IsNumber(jcor)  ? clamp_int(jcor->valuedouble,  1, 10000)   : 1,
            cJSON_IsNumber(jgpu)  ? clamp_int(jgpu->valuedouble,  0, 1000)    : 0,
            cJSON_IsNumber(jram)  ? clamp_int(jram->valuedouble,  0, 10000000) : 0,
            cJSON_IsNumber(jdisk) ? clamp_int(jdisk->valuedouble, 0, 10000000) : 0,
            auth_user_id, app_id
        );
        if (!job) {
            free(app_env_json);
            for (int r = 0; r < created; r++) {
                job_set_status_r(created_jobs[r], JOB_STATUS_CANCELLED, "Workflow submission failed");
                job_free(created_jobs[r]);
            }
            cJSON_Delete(req);
            http_error(c, 500, "Failed to create job");
            return;
        }

        /* Store app env */
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

        /* Same-machine affinity: pin to first dependency's machine */
        cJSON *jsm = cJSON_GetObjectItemCaseSensitive(step, "same_machine");
        if (cJSON_IsTrue(jsm) && depends_on_str[0]) {
            /* Use the first dependency job ID as the affinity target */
            char first_dep[64] = {0};
            const char *comma = strchr(depends_on_str, ',');
            size_t clen = comma ? (size_t)(comma - depends_on_str)
                                : strlen(depends_on_str);
            if (clen >= sizeof(first_dep)) clen = sizeof(first_dep) - 1;
            memcpy(first_dep, depends_on_str, clen);
            first_dep[clen] = '\0';
            strncpy(job->same_machine_as, first_dep, sizeof(job->same_machine_as) - 1);
            db_update_same_machine_as(job->id, first_dep);
        }

        if (job_timeout > 0) {
            job->timeout_seconds = job_timeout;
            db_update_job_timeout(job->id, job_timeout);
        }

        /* Store input files */
        if (input_files_str[0]) {
            strncpy(job->input_files, input_files_str, sizeof(job->input_files) - 1);
            db_update_input_files(job->id, input_files_str);
        }

        int is_held = (depends_on_str[0] != '\0' || input_files_str[0] != '\0');
        if (is_held) {
            job->status = JOB_STATUS_HELD;
            db_update_job_status(job->id, JOB_STATUS_HELD, 0, 0);
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
        } else {
            queue_push(scheduler_queue(), job);
        }

        strncpy(job_ids[idx], job->id, sizeof(job_ids[idx]) - 1);
        /* Tag job with workflow batch ID */
        strncpy(job->workflow_id, wf_batch_id, sizeof(job->workflow_id) - 1);
        db_update_workflow_id(job->id, wf_batch_id);
        created_jobs[created] = job;
        created++;
        idx++;
    }

    /* Build response */
    cJSON *resp = cJSON_CreateObject();
    cJSON *jname = cJSON_GetObjectItemCaseSensitive(req, "name");
    if (cJSON_IsString(jname))
        cJSON_AddStringToObject(resp, "name", jname->valuestring);
    cJSON_AddStringToObject(resp, "workflow_id", wf_batch_id);
    cJSON *arr = cJSON_AddArrayToObject(resp, "jobs");
    for (int i = 0; i < created; i++) {
        cJSON_AddItemToArray(arr, job_to_json(created_jobs[i]));
        /* Free held jobs; queued ones are owned by the queue */
        if (created_jobs[i]->status == JOB_STATUS_HELD)
            job_free(created_jobs[i]);
    }

    cJSON_Delete(req);
    char *s = cJSON_PrintUnformatted(resp);
    http_json_reply(c, 201, s);
    free(s);
    cJSON_Delete(resp);
    log_info("routes", "Workflow submitted: %d steps by user %s", created, auth_user_id);
}

static void list_jobs(struct mg_connection *c, struct mg_http_message *hm,
                      const char *auth_user_id, const char *auth_role)
{
    (void)hm;
    int is_admin = (strcmp(auth_role, "admin") == 0);
    Job *jobs = (Job *)malloc(256 * sizeof(Job));
    if (!jobs) { http_error(c, 500, "Out of memory"); return; }
    int count = db_list_jobs(jobs, 256);
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < count; i++) {
        if (!is_admin &&
            strcmp(jobs[i].user_id, auth_user_id) != 0)
            continue;
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
    /* Limit upload size to 512 MB */
    if (hm->body.len > (size_t)(512 * 1024 * 1024)) {
        http_error(c, 413, "Upload too large (max 512 MB)");
        return;
    }
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

static void sse_subscribe(struct mg_connection *c, struct mg_http_message *hm,
                          const char *auth_user_id, const char *auth_role)
{
    (void)hm;
    int is_admin = (strcmp(auth_role, "admin") == 0);
    mg_printf(c,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: keep-alive\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "\r\n");
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

    /* Validate id, hostname, and ip: allow only safe characters */
    static const char *safe_host = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-:";
    const char *fields[] = { m.id, m.hostname, m.ip };
    const char *fnames[] = { "id", "hostname", "ip" };
    for (int vi = 0; vi < 3; vi++) {
        if (!fields[vi][0]) continue;
        for (const char *vp = fields[vi]; *vp; vp++) {
            if (!strchr(safe_host, *vp)) {
                http_error(c, 400, "Invalid characters in machine field");
                return;
            }
        }
    }

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
                char pwd_hash[98];
                auth_hash_password(jpwd->valuestring, pwd_hash);
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
    int count = 0;
    Machine *machines = registry_all(&count);

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

    cJSON *j;
    j = cJSON_GetObjectItemCaseSensitive(body, "provider");
    if (cJSON_IsString(j)) strncpy(spec.provider, j->valuestring, sizeof(spec.provider)-1);
    j = cJSON_GetObjectItemCaseSensitive(body, "instance_type");
    if (cJSON_IsString(j)) strncpy(spec.instance_type, j->valuestring, sizeof(spec.instance_type)-1);
    j = cJSON_GetObjectItemCaseSensitive(body, "region");
    if (cJSON_IsString(j)) strncpy(spec.region, j->valuestring, sizeof(spec.region)-1);
    j = cJSON_GetObjectItemCaseSensitive(body, "image_id");
    if (cJSON_IsString(j)) strncpy(spec.image_id, j->valuestring, sizeof(spec.image_id)-1);
    j = cJSON_GetObjectItemCaseSensitive(body, "cores");
    if (cJSON_IsNumber(j)) spec.cores = (int)j->valuedouble;
    j = cJSON_GetObjectItemCaseSensitive(body, "gpu_count");
    if (cJSON_IsNumber(j)) spec.gpu_count = (int)j->valuedouble;
    j = cJSON_GetObjectItemCaseSensitive(body, "ram_mb");
    if (cJSON_IsNumber(j)) spec.ram_mb = (int)j->valuedouble;
    j = cJSON_GetObjectItemCaseSensitive(body, "disk_mb");
    if (cJSON_IsNumber(j)) spec.disk_mb = (int)j->valuedouble;
    j = cJSON_GetObjectItemCaseSensitive(body, "tags");
    if (cJSON_IsString(j)) strncpy(spec.tags, j->valuestring, sizeof(spec.tags)-1);

    /* Flexible minimums */
    j = cJSON_GetObjectItemCaseSensitive(body, "cores_min");
    if (cJSON_IsNumber(j)) spec.cores_min = (int)j->valuedouble;
    j = cJSON_GetObjectItemCaseSensitive(body, "ram_mb_min");
    if (cJSON_IsNumber(j)) spec.ram_mb_min = (int)j->valuedouble;
    j = cJSON_GetObjectItemCaseSensitive(body, "disk_mb_min");
    if (cJSON_IsNumber(j)) spec.disk_mb_min = (int)j->valuedouble;

    cJSON_Delete(body);

    if (!spec.provider[0]) { http_error(c, 400, "Missing provider"); return; }

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
    cJSON *j;
    j = cJSON_GetObjectItemCaseSensitive(body, "provider");
    if (cJSON_IsString(j)) strncpy(provider, j->valuestring, sizeof(provider)-1);
    j = cJSON_GetObjectItemCaseSensitive(body, "instance_id");
    if (cJSON_IsString(j)) strncpy(instance_id, j->valuestring, sizeof(instance_id)-1);
    cJSON_Delete(body);

    if (!provider[0] || !instance_id[0]) {
        http_error(c, 400, "Missing provider or instance_id"); return;
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

    Machine *m = registry_get(machine_id);
    if (!m) { http_error(c, 404, "Machine not found"); return; }
    if (!m->mac_address[0]) { http_error(c, 400, "Machine has no MAC address configured"); return; }

    if (wol_send(m->mac_address, broadcast_ip[0] ? broadcast_ip : NULL) != 0) {
        http_error(c, 500, "WoL send failed"); return;
    }

    events_push_persistent("machine", "wol", m->id, "");
    http_json_reply(c, 200, "{\"ok\":true,\"message\":\"WoL packet sent\"}");
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
            /* Enforce job ownership for non-admin users */
            if (strcmp(auth_role, "admin") != 0) {
                Job *_chk = db_get_job(seg[1]);
                if (_chk && _chk->user_id[0] &&
                    strcmp(_chk->user_id, auth_user_id) != 0) {
                    job_free(_chk);
                    http_error(c, 404, "Job not found");
                    return;
                }
                if (_chk) job_free(_chk);
            }
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
            if (strcmp(method, "GET") == 0 && strcmp(seg[2], "deps") == 0 && seg[3][0] == '\0') {
                get_job_deps(c, hm, seg[1]); return;
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

    /* /workflows — saved workflow templates + submit */
    if (strcmp(seg[0], "workflows") == 0) {
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
            submit_workflow(c, hm, auth_user_id); return;
        }
        if (strcmp(method, "DELETE") == 0 && seg[1][0] != '\0' && seg[2][0] == '\0') {
            delete_saved_workflow(c, seg[1], auth_user_id, auth_role); return;
        }
        if (strcmp(method, "POST") == 0 && seg[1][0] != '\0' &&
            strcmp(seg[2], "favorite") == 0 && seg[3][0] == '\0') {
            toggle_workflow_favorite(c, hm, seg[1], auth_user_id); return;
        }
    }

    if (strcmp(seg[0], "resources") == 0 && strcmp(method, "GET") == 0) {
        get_resources(c, hm); return;
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
        if (strcmp(method, "POST") == 0 && strcmp(seg[2], "provision") == 0) {
            admin_cloud_provision(c, hm); return;
        }
        if (strcmp(method, "POST") == 0 && strcmp(seg[2], "deprovision") == 0) {
            admin_cloud_deprovision(c, hm); return;
        }
    }

    /* /admin/wol — Wake-on-LAN */
    if (strcmp(seg[0], "admin") == 0 && strcmp(seg[1], "wol") == 0) {
        if (strcmp(method, "POST") == 0) {
            admin_wol(c, hm); return;
        }
    }

    http_error(c, 404, "Not found");
}
