#include "http.h"
#include "db.h"
#include "cJSON.h"

#include <stdlib.h>

#define ARTIFACT_API_LIMIT 4096

void artifacts_list(struct mg_connection *c, const char *job_id)
{
    ArtifactRecord *items = calloc(ARTIFACT_API_LIMIT, sizeof(*items));
    if (!items) { http_error(c, 500, "Out of memory"); return; }

    int count = db_list_artifacts(job_id, items, ARTIFACT_API_LIMIT);
    if (count < 0) {
        free(items);
        http_error(c, 500, "Failed to list artifacts");
        return;
    }

    cJSON *array = cJSON_CreateArray();
    for (int i = 0; i < count; i++) {
        cJSON *object = cJSON_CreateObject();
        cJSON_AddNumberToObject(object, "id", (double)items[i].id);
        cJSON_AddStringToObject(object, "job_id", items[i].job_id);
        cJSON_AddStringToObject(object, "type", items[i].type);
        cJSON_AddStringToObject(object, "uri", items[i].uri);
        cJSON_AddNumberToObject(object, "size_bytes", (double)items[i].size_bytes);
        if (items[i].checksum[0])
            cJSON_AddStringToObject(object, "checksum", items[i].checksum);
        else
            cJSON_AddNullToObject(object, "checksum");
        cJSON_AddNumberToObject(object, "created_at", (double)items[i].created_at);
        cJSON_AddItemToArray(array, object);
    }
    free(items);

    char *json = cJSON_PrintUnformatted(array);
    cJSON_Delete(array);
    if (!json) { http_error(c, 500, "Failed to encode artifacts"); return; }
    http_json_reply(c, 200, json);
    free(json);
}
