#include "http.h"
#include "job.h"
#include "queue.h"
#include "scheduler.h"
#include "resources.h"
#include "transfer.h"
#include "db.h"
#include "log.h"
#include "cJSON.h"
#include "mongoose.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

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
    return obj;
}

/* ── Handlers ────────────────────────────────────────────────────── */

static void submit_job(struct mg_connection *c, struct mg_http_message *hm)
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

    Job *job = job_create(
        jcmd->valuestring,
        cJSON_IsNumber(jpri)  ? (int)jpri->valuedouble  : 50,
        cJSON_IsNumber(jcor)  ? (int)jcor->valuedouble  : 1,
        cJSON_IsNumber(jgpu)  ? (int)jgpu->valuedouble  : 0,
        cJSON_IsNumber(jram)  ? (int)jram->valuedouble  : 0,
        cJSON_IsNumber(jdisk) ? (int)jdisk->valuedouble : 0
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

void routes_handler(struct mg_connection *c, int ev, void *ev_data)
{
    if (ev != MG_EV_HTTP_MSG) return;
    struct mg_http_message *hm = (struct mg_http_message *)ev_data;

    if (!auth_check(c, hm)) {
        http_error(c, 401, "Unauthorized");
        return;
    }

    char uri[256] = {0};
    size_t ulen = hm->uri.len < sizeof(uri)-1 ? hm->uri.len : sizeof(uri)-1;
    memcpy(uri, hm->uri.buf, ulen);

    char method[16] = {0};
    size_t mlen = hm->method.len < sizeof(method)-1 ? hm->method.len : sizeof(method)-1;
    memcpy(method, hm->method.buf, mlen);

    char seg[5][128] = {{0}};
    for (int i = 0; i < 5; i++) extract_segment(uri, i, seg[i], 128);

    /* /jobs */
    if (strcmp(seg[0], "jobs") == 0) {
        if (strcmp(method, "POST") == 0 && seg[1][0] == '\0') {
            submit_job(c, hm); return;
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
            if (strcmp(method, "GET") == 0 && strcmp(seg[2], "log") == 0) {
                get_job_log(c, hm, seg[1], strcmp(seg[3], "stderr") == 0); return;
            }
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

    http_error(c, 404, "Not found");
}
