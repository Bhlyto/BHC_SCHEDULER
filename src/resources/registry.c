#include "resources.h"
#include "log.h"
#include "cJSON.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/*
 * registry.c
 * Loads machine definitions from provisioning.json and provides
 * a simple in-memory registry.
 */

static Machine s_machines[MAX_MACHINES];
static int     s_count = 0;

int registry_load(const char *json_path)
{
    FILE *f = fopen(json_path, "r");
    if (!f) { log_error("registry", "Cannot open %s", json_path); return -1; }

    fseek(f, 0, SEEK_END); long sz = ftell(f); rewind(f);
    char *buf = (char *)malloc(sz + 1);
    if (!buf) { fclose(f); return -1; }
    fread(buf, 1, sz, f); buf[sz] = '\0'; fclose(f);

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) { log_error("registry", "JSON parse error"); return -1; }

    cJSON *machines = cJSON_GetObjectItemCaseSensitive(root, "machines");
    if (!cJSON_IsArray(machines)) {
        log_error("registry", "provisioning.json: 'machines' array required");
        cJSON_Delete(root); return -1;
    }

    s_count = 0;
    cJSON *m;
    cJSON_ArrayForEach(m, machines) {
        if (s_count >= MAX_MACHINES) break;
        Machine *M = &s_machines[s_count];
        memset(M, 0, sizeof(*M));

#define GET_STR(field, key) do { \
    cJSON *_j = cJSON_GetObjectItemCaseSensitive(m, key); \
    if (cJSON_IsString(_j)) strncpy(M->field, _j->valuestring, sizeof(M->field)-1); \
} while(0)

#define GET_INT(field, key) do { \
    cJSON *_j = cJSON_GetObjectItemCaseSensitive(m, key); \
    if (cJSON_IsNumber(_j)) M->field = (int)_j->valuedouble; \
} while(0)

        GET_STR(id,       "id");
        GET_STR(hostname, "hostname");
        GET_STR(ip,       "ip");
        GET_INT(enabled,  "enabled");
        GET_INT(cores_total,     "cores");
        GET_INT(gpu_count_total, "gpu_count");
        GET_INT(ram_mb_total,    "ram_mb");
        GET_INT(disk_mb_total,   "disk_mb");

        M->enabled = 1; /* default enabled */
        cJSON *en = cJSON_GetObjectItemCaseSensitive(m, "enabled");
        if (cJSON_IsBool(en)) M->enabled = cJSON_IsTrue(en) ? 1 : 0;

        s_count++;
    }

    /* ── Pools: expand ranges into individual Machine entries ──────── */
    cJSON *pools = cJSON_GetObjectItemCaseSensitive(root, "pools");
    if (cJSON_IsArray(pools)) {
        cJSON *p;
        cJSON_ArrayForEach(p, pools) {
            if (s_count >= MAX_MACHINES) break;

            /* Required fields */
            cJSON *j_prefix = cJSON_GetObjectItemCaseSensitive(p, "id_prefix");
            cJSON *j_start  = cJSON_GetObjectItemCaseSensitive(p, "range_start");
            cJSON *j_end    = cJSON_GetObjectItemCaseSensitive(p, "range_end");
            if (!cJSON_IsString(j_prefix) || !cJSON_IsNumber(j_start) || !cJSON_IsNumber(j_end))
                continue;

            const char *prefix     = j_prefix->valuestring;
            int         range_start = (int)j_start->valuedouble;
            int         range_end   = (int)j_end->valuedouble;
            if (range_start > range_end) continue;

            /* Optional format strings */
            cJSON *j_hfmt = cJSON_GetObjectItemCaseSensitive(p, "hostname_format");
            cJSON *j_ifmt = cJSON_GetObjectItemCaseSensitive(p, "ip_format");
            const char *hostname_fmt = cJSON_IsString(j_hfmt) ? j_hfmt->valuestring : NULL;
            const char *ip_fmt       = cJSON_IsString(j_ifmt) ? j_ifmt->valuestring : NULL;

            /* Resources shared by all machines in the pool */
            int pool_cores = 0, pool_gpu = 0, pool_ram = 0, pool_disk = 0, pool_enabled = 1;
#define POOL_INT(var, key) do { \
    cJSON *_j = cJSON_GetObjectItemCaseSensitive(p, key); \
    if (cJSON_IsNumber(_j)) var = (int)_j->valuedouble; \
} while(0)
            POOL_INT(pool_cores,   "cores");
            POOL_INT(pool_gpu,     "gpu_count");
            POOL_INT(pool_ram,     "ram_mb");
            POOL_INT(pool_disk,    "disk_mb");
            cJSON *en = cJSON_GetObjectItemCaseSensitive(p, "enabled");
            if (cJSON_IsBool(en)) pool_enabled = cJSON_IsTrue(en) ? 1 : 0;
            else if (cJSON_IsNumber(en)) pool_enabled = (int)en->valuedouble;
#undef POOL_INT

            /* Compute zero-padding width from range_end */
            int width = 1, tmp = range_end;
            while (tmp >= 10) { width++; tmp /= 10; }

            for (int i = range_start; i <= range_end && s_count < MAX_MACHINES; i++) {
                Machine *M = &s_machines[s_count];
                memset(M, 0, sizeof(*M));

                /* id = prefix + zero-padded number */
                snprintf(M->id,       sizeof(M->id),       "%s%0*d", prefix, width, i);

                /* hostname */
                if (hostname_fmt)
                    snprintf(M->hostname, sizeof(M->hostname), hostname_fmt, i);
                else
                    snprintf(M->hostname, sizeof(M->hostname), "%s%0*d", prefix, width, i);

                /* ip (optional) */
                if (ip_fmt)
                    snprintf(M->ip, sizeof(M->ip), ip_fmt, i);

                M->enabled         = pool_enabled;
                M->cores_total     = pool_cores;
                M->gpu_count_total = pool_gpu;
                M->ram_mb_total    = pool_ram;
                M->disk_mb_total   = pool_disk;
                s_count++;
            }

            log_info("registry", "Pool '%s': expanded %d-%d (%d machines)",
                     prefix, range_start, range_end, range_end - range_start + 1);
        }
    }

    cJSON_Delete(root);
    log_info("registry", "Loaded %d machine(s) from %s", s_count, json_path);
    return s_count;
}

Machine *registry_get(const char *machine_id)
{
    for (int i = 0; i < s_count; i++)
        if (strcmp(s_machines[i].id, machine_id) == 0)
            return &s_machines[i];
    return NULL;
}

Machine *registry_all(int *count)
{
    *count = s_count;
    return s_machines;
}

int registry_upsert(const Machine *m)
{
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_machines[i].id, m->id) == 0) {
            s_machines[i] = *m;
            return 0;
        }
    }
    if (s_count >= MAX_MACHINES) return -1;
    s_machines[s_count++] = *m;
    return 0;
}

int registry_remove(const char *machine_id)
{
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_machines[i].id, machine_id) == 0) {
            s_machines[i] = s_machines[--s_count];
            return 0;
        }
    }
    return -1;
}
