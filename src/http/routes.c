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
    /* URI looks like /segment0/segment1/segment2/...
       seg_index 0 = first segment after leading '/' */
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
    cJSON_AddNumberToObject(obj, "exit_code",   job->exit_code);
    return obj;
}

/* ── Handlers ────────────────────────────────────────────────────── */

/* POST /jobs */
static void submit_job(struct mg_connection *c, struct mg_http_message *hm)
{
    char body[1024] = {0};
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
    /* Also accept short names for convenience */
    if (!jcor)  jcor  = cJSON_GetObjectItemCaseSensitive(req, "cores");
    if (!jgpu)  jgpu  = cJSON_GetObjectItemCaseSensitive(req, "gpu");
    if (!jram)  jram  = cJSON_GetObjectItemCaseSensitive(req, "ram_mb");
    if (!jdisk) jdisk = cJSON_GetObjectItemCaseSensitive(req, "disk_mb");

    if (!cJSON_IsString(jcmd)) {
        cJSON_Delete(req);
        http_error(c, 400, "Missing 'command'");
        return;
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

    queue_push(scheduler_queue(), job);

    cJSON *resp = job_to_json(job);
    char *s = cJSON_PrintUnformatted(resp);
    http_json_reply(c, 201, s);
    free(s);
    cJSON_Delete(resp);
}

/* GET /jobs */
static void list_jobs(struct mg_connection *c, struct mg_http_message *hm)
{
    (void)hm;
    Job jobs[256];
    int count = db_list_jobs(jobs, 256);
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < count; i++) {
        cJSON *obj = job_to_json(&jobs[i]);
        cJSON_AddItemToArray(arr, obj);
    }
    char *s = cJSON_PrintUnformatted(arr);
    http_json_reply(c, 200, s);
    free(s);
    cJSON_Delete(arr);
}

/* GET /jobs/:id */
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

/* DELETE /jobs/:id */
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
    job_set_status(job, JOB_STATUS_CANCELLED);
    alloc_release(job_id);
    cJSON *resp = job_to_json(job);
    char *s = cJSON_PrintUnformatted(resp);
    http_json_reply(c, 200, s);
    free(s);
    cJSON_Delete(resp);
    job_free(job);
}

/* POST /jobs/:id/input/:filename */
static void upload_input(struct mg_connection *c, struct mg_http_message *hm,
                          const char *job_id, const char *filename)
{
    long written = upload_handle(job_id, filename,
                                 hm->body.buf, (long)hm->body.len);
    if (written < 0) { http_error(c, 500, "Upload failed"); return; }
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"bytes\":%ld}", written);
    http_json_reply(c, 200, buf);
}

/* GET /jobs/:id/output/:filename */
static void download_output(struct mg_connection *c, struct mg_http_message *hm,
                             const char *job_id, const char *filename)
{
    if (download_handle(c, hm, job_id, filename) != 0)
        http_error(c, 404, "Output file not found");
}

/* GET /jobs/:id/log[/stderr]  — stream a captured log file */
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

/* GET /jobs/events — Server-Sent Events stream */
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

    /* Send an immediate snapshot so the client starts up-to-date */
    Job jobs[256];
    int count = db_list_jobs(jobs, 256);
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < count; i++) cJSON_AddItemToArray(arr, job_to_json(&jobs[i]));
    char *s = cJSON_PrintUnformatted(arr);
    mg_printf(c, "event: snapshot\ndata: %s\n\n", s);
    free(s);
    cJSON_Delete(arr);
}

/* GET /stats */
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

/* GET /resources */
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

/* POST /provision */
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

/* DELETE /provision/:id */
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

/* ── Main dispatcher ─────────────────────────────────────────────── */
void routes_handler(struct mg_connection *c, int ev, void *ev_data)
{
    if (ev != MG_EV_HTTP_MSG) return;
    struct mg_http_message *hm = (struct mg_http_message *)ev_data;

    /* Authentication — reject unauthenticated requests */
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
        /* GET /jobs/events — must be checked before /jobs/:id */
        if (strcmp(method, "GET") == 0 && strcmp(seg[1], "events") == 0) {
            sse_subscribe(c, hm); return;
        }
        if (seg[1][0] != '\0') {
            /* /jobs/:id */
            if (strcmp(method, "GET") == 0 && seg[2][0] == '\0') {
                get_job(c, hm, seg[1]); return;
            }
            if (strcmp(method, "DELETE") == 0 && seg[2][0] == '\0') {
                cancel_job(c, hm, seg[1]); return;
            }
            /* /jobs/:id/input/:filename */
            if (strcmp(method, "POST") == 0 && strcmp(seg[2], "input") == 0 && seg[3][0]) {
                upload_input(c, hm, seg[1], seg[3]); return;
            }
            /* /jobs/:id/output/:filename */
            if (strcmp(method, "GET") == 0 && strcmp(seg[2], "output") == 0 && seg[3][0]) {
                download_output(c, hm, seg[1], seg[3]); return;
            }
            /* /jobs/:id/log and /jobs/:id/log/stderr */
            if (strcmp(method, "GET") == 0 && strcmp(seg[2], "log") == 0) {
                get_job_log(c, hm, seg[1], strcmp(seg[3], "stderr") == 0); return;
            }
        }
    }

    /* /resources */
    if (strcmp(seg[0], "resources") == 0 && strcmp(method, "GET") == 0) {
        get_resources(c, hm); return;
    }

    /* /stats */
    if (strcmp(seg[0], "stats") == 0 && strcmp(method, "GET") == 0) {
        get_stats(c, hm); return;
    }

    /* /provision */
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
