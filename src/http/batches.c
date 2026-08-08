#include "http.h"
#include "cJSON.h"
#include "db.h"
#include "job.h"
#include "queue.h"
#include "scheduler.h"
#include "transfer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#  include <windows.h>
#  include <wincrypt.h>
#endif

#define MAX_BATCH_JOBS 1000
#define MAX_BATCH_BODY (16 * 1024 * 1024)

static int bounded_int(cJSON *value, int default_value, int min, int max)
{
    if (!cJSON_IsNumber(value)) return default_value;
    if (value->valuedouble < min) return min;
    if (value->valuedouble > max) return max;
    return (int)value->valuedouble;
}

static int generate_batch_id(char *out, int out_len)
{
    unsigned char raw[16] = {0};
#ifdef _WIN32
    HCRYPTPROV provider = 0;
    if (!CryptAcquireContextA(&provider, NULL, NULL, PROV_RSA_FULL,
                              CRYPT_VERIFYCONTEXT)) return -1;
    int ok = CryptGenRandom(provider, sizeof(raw), raw) ? 0 : -1;
    CryptReleaseContext(provider, 0);
    if (ok != 0) return -1;
#else
    FILE *random = fopen("/dev/urandom", "rb");
    if (!random) return -1;
    int ok = fread(raw, 1, sizeof(raw), random) == sizeof(raw) ? 0 : -1;
    fclose(random);
    if (ok != 0) return -1;
#endif
    int position = 0;
    for (int i = 0; i < 16 && position < out_len - 1; i++)
        position += snprintf(out + position, out_len - position, "%02x", raw[i]);
    out[position] = '\0';
    return 0;
}

static cJSON *batch_to_json(const BatchRecord *batch, const BatchStats *stats)
{
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "id", batch->id);
    cJSON_AddStringToObject(json, "name", batch->name);
    cJSON_AddStringToObject(json, "user_id", batch->user_id);
    cJSON_AddNumberToObject(json, "created_at", (double)batch->created_at);
    cJSON_AddNumberToObject(json, "total", stats->total);
    cJSON_AddNumberToObject(json, "created", stats->created);
    cJSON_AddNumberToObject(json, "queued", stats->queued);
    cJSON_AddNumberToObject(json, "running", stats->running);
    cJSON_AddNumberToObject(json, "succeeded", stats->succeeded);
    cJSON_AddNumberToObject(json, "failed", stats->failed);
    cJSON_AddNumberToObject(json, "cancelled", stats->cancelled);
    int completed = stats->succeeded + stats->failed + stats->cancelled;
    cJSON_AddNumberToObject(json, "completed", completed);
    cJSON_AddNumberToObject(json, "progress",
        stats->total > 0 ? (double)completed / (double)stats->total : 0.0);
    return json;
}

void batches_submit(struct mg_connection *c, struct mg_http_message *hm,
                    const char *auth_user_id)
{
    if (hm->body.len == 0 || hm->body.len > MAX_BATCH_BODY) {
        http_error(c, 413, "Batch body must be between 1 byte and 16 MB");
        return;
    }
    char *body = (char *)malloc(hm->body.len + 1);
    if (!body) { http_error(c, 500, "Out of memory"); return; }
    memcpy(body, hm->body.buf, hm->body.len);
    body[hm->body.len] = '\0';
    cJSON *request = cJSON_Parse(body);
    free(body);
    if (!request) { http_error(c, 400, "Invalid JSON"); return; }

    cJSON *job_defs = cJSON_GetObjectItemCaseSensitive(request, "jobs");
    int count = cJSON_IsArray(job_defs) ? cJSON_GetArraySize(job_defs) : 0;
    if (count < 1 || count > MAX_BATCH_JOBS) {
        cJSON_Delete(request);
        http_error(c, 400, "'jobs' must contain between 1 and 1000 jobs");
        return;
    }
    for (int i = 0; i < count; i++) {
        cJSON *definition = cJSON_GetArrayItem(job_defs, i);
        cJSON *command = cJSON_GetObjectItemCaseSensitive(definition, "command");
        if (!cJSON_IsObject(definition) || !cJSON_IsString(command) ||
            !command->valuestring[0] || strlen(command->valuestring) >= JOB_CMD_LEN) {
            cJSON_Delete(request);
            http_error(c, 400, "Every batch job requires a command shorter than 512 bytes");
            return;
        }
    }

    BatchRecord batch;
    memset(&batch, 0, sizeof(batch));
    if (generate_batch_id(batch.id, sizeof(batch.id)) != 0) {
        cJSON_Delete(request);
        http_error(c, 500, "Failed to generate batch id");
        return;
    }
    cJSON *name = cJSON_GetObjectItemCaseSensitive(request, "name");
    if (cJSON_IsString(name)) strncpy(batch.name, name->valuestring, sizeof(batch.name)-1);
    if (auth_user_id) strncpy(batch.user_id, auth_user_id, sizeof(batch.user_id)-1);
    batch.created_at = time(NULL);

    Job **jobs = (Job **)calloc((size_t)count, sizeof(Job *));
    if (!jobs) {
        cJSON_Delete(request);
        http_error(c, 500, "Out of memory");
        return;
    }
    if (db_begin() != 0 || db_insert_batch(&batch) != 0) goto transaction_failed;

    for (int i = 0; i < count; i++) {
        cJSON *definition = cJSON_GetArrayItem(job_defs, i);
        cJSON *command = cJSON_GetObjectItemCaseSensitive(definition, "command");
        cJSON *priority = cJSON_GetObjectItemCaseSensitive(definition, "priority");
        cJSON *cores = cJSON_GetObjectItemCaseSensitive(definition, "req_cores");
        cJSON *gpu = cJSON_GetObjectItemCaseSensitive(definition, "req_gpu");
        cJSON *ram = cJSON_GetObjectItemCaseSensitive(definition, "req_ram_mb");
        cJSON *disk = cJSON_GetObjectItemCaseSensitive(definition, "req_disk_mb");
        cJSON *timeout = cJSON_GetObjectItemCaseSensitive(definition, "timeout_seconds");
        cJSON *app = cJSON_GetObjectItemCaseSensitive(definition, "app_id");
        jobs[i] = job_create_ex(command->valuestring,
            bounded_int(priority, 50, 0, 100),
            bounded_int(cores, 1, 1, 10000),
            bounded_int(gpu, 0, 0, 1000),
            bounded_int(ram, 0, 0, 10000000),
            bounded_int(disk, 0, 0, 10000000),
            batch.user_id, cJSON_IsString(app) ? app->valuestring : "");
        if (!jobs[i]) goto transaction_failed;
        strncpy(jobs[i]->batch_id, batch.id, sizeof(jobs[i]->batch_id) - 1);
        jobs[i]->timeout_seconds = bounded_int(timeout, 0, 0, 604800);
        if (db_update_job_batch_id(jobs[i]->id, batch.id) != 0 ||
            (jobs[i]->timeout_seconds > 0 &&
             db_update_job_timeout(jobs[i]->id, jobs[i]->timeout_seconds) != 0))
            goto transaction_failed;
    }
    if (db_commit() != 0) goto transaction_failed;

    for (int i = 0; i < count; i++) {
        store_init_job_dirs(jobs[i]->id);
        if (queue_push(scheduler_queue(), jobs[i]) != 0) job_free(jobs[i]);
    }
    free(jobs);
    cJSON_Delete(request);

    BatchStats stats;
    db_get_batch_stats(batch.id, &stats);
    cJSON *response = batch_to_json(&batch, &stats);
    char *serialized = cJSON_PrintUnformatted(response);
    http_json_reply(c, 201, serialized);
    free(serialized);
    cJSON_Delete(response);
    return;

transaction_failed:
    db_rollback();
    for (int i = 0; i < count; i++) job_free(jobs[i]);
    free(jobs);
    cJSON_Delete(request);
    http_error(c, 500, "Batch submission failed; no jobs were committed");
}

void batches_get(struct mg_connection *c, const char *batch_id,
                 const char *auth_user_id, const char *auth_role)
{
    BatchRecord batch;
    if (db_get_batch(batch_id, &batch) != 0 ||
        (strcmp(auth_role, "admin") != 0 &&
         strcmp(batch.user_id, auth_user_id) != 0)) {
        http_error(c, 404, "Batch not found");
        return;
    }
    BatchStats stats;
    if (db_get_batch_stats(batch_id, &stats) != 0) {
        http_error(c, 500, "Failed to read batch stats");
        return;
    }
    cJSON *response = batch_to_json(&batch, &stats);
    char *serialized = cJSON_PrintUnformatted(response);
    http_json_reply(c, 200, serialized);
    free(serialized);
    cJSON_Delete(response);
}
