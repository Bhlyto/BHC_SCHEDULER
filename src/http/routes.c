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
 *   GET    /jobs/:id                  → get_job
 *   DELETE /jobs/:id                  → cancel_job
 *   POST   /jobs/:id/input/:filename  → upload_input
 *   GET    /jobs/:id/output/:filename → download_output
 *   GET    /resources                 → get_resources
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
    cJSON *jcor  = cJSON_GetObjectItemCaseSensitive(req, "cores");
    cJSON *jgpu  = cJSON_GetObjectItemCaseSensitive(req, "gpu");
    cJSON *jram  = cJSON_GetObjectItemCaseSensitive(req, "ram_mb");
    cJSON *jdisk = cJSON_GetObjectItemCaseSensitive(req, "disk_mb");

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
        }
    }

    /* /resources */
    if (strcmp(seg[0], "resources") == 0 && strcmp(method, "GET") == 0) {
        get_resources(c, hm); return;
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
