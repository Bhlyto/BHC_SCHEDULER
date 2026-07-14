#include "resources.h"
#include "log.h"
#include "cJSON.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#ifdef _WIN32
#  include <windows.h>
static SRWLOCK s_registry_lock = SRWLOCK_INIT;
static void registry_lock(void) { AcquireSRWLockExclusive(&s_registry_lock); }
static void registry_unlock(void) { ReleaseSRWLockExclusive(&s_registry_lock); }
#else
#  include <pthread.h>
static pthread_mutex_t s_registry_lock = PTHREAD_MUTEX_INITIALIZER;
static void registry_lock(void) { pthread_mutex_lock(&s_registry_lock); }
static void registry_unlock(void) { pthread_mutex_unlock(&s_registry_lock); }
#endif

static Machine s_machines[MAX_MACHINES];
static int     s_count = 0;

static int read_int(cJSON *object, const char *key, int default_value,
                    int minimum, int maximum, int *out)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!item) {
        *out = default_value;
        return 0;
    }
    if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble) ||
        floor(item->valuedouble) != item->valuedouble ||
        item->valuedouble < minimum || item->valuedouble > maximum) return -1;
    *out = (int)item->valuedouble;
    return 0;
}

static int copy_string(cJSON *object, const char *key, char *out, size_t out_len)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!item) return 0;
    if (!cJSON_IsString(item) || strlen(item->valuestring) >= out_len) return -1;
    memcpy(out, item->valuestring, strlen(item->valuestring) + 1);
    return 0;
}

static int safe_machine_text(const char *value)
{
    if (!value) return 0;
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '.' || *p == '_' ||
              *p == '-' || *p == ':')) return 0;
    }
    return 1;
}

static int valid_pool_format(const char *format)
{
    if (!format || !format[0] || strlen(format) >= 256) return 0;
    int conversions = 0;
    for (const char *p = format; *p; p++) {
        if (*p != '%') continue;
        p++;
        if (*p == '%') continue;
        if (*p == '0') p++;
        int width_digits = 0;
        while (*p >= '0' && *p <= '9' && width_digits < 2) {
            width_digits++;
            p++;
        }
        if ((*p >= '0' && *p <= '9') || *p != 'd' || ++conversions > 1) return 0;
    }
    return conversions == 1;
}

static int registry_contains_unlocked(const char *machine_id)
{
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_machines[i].id, machine_id) == 0) return 1;
    }
    return 0;
}

int registry_load(const char *json_path)
{
    FILE *f = fopen(json_path, "r");
    if (!f) { log_error("registry", "Cannot open %s", json_path); return -1; }

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long sz = ftell(f);
    if (sz <= 0 || sz > 4 * 1024 * 1024 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        log_error("registry", "Invalid provisioning file size");
        return -1;
    }
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return -1; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf);
        fclose(f);
        return -1;
    }
    buf[sz] = '\0';
    fclose(f);

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) { log_error("registry", "JSON parse error"); return -1; }

    cJSON *machines = cJSON_GetObjectItemCaseSensitive(root, "machines");
    if (!cJSON_IsArray(machines)) {
        log_error("registry", "provisioning.json: 'machines' array required");
        cJSON_Delete(root); return -1;
    }

    registry_lock();
    s_count = 0;
    cJSON *m;
    cJSON_ArrayForEach(m, machines) {
        if (s_count >= MAX_MACHINES) break;
        Machine *M = &s_machines[s_count];
        memset(M, 0, sizeof(*M));

        int invalid = 0;
        invalid |= copy_string(m, "id", M->id, sizeof(M->id)) != 0;
        invalid |= copy_string(m, "hostname", M->hostname, sizeof(M->hostname)) != 0;
        invalid |= copy_string(m, "ip", M->ip, sizeof(M->ip)) != 0;
        invalid |= copy_string(m, "mac_address", M->mac_address, sizeof(M->mac_address)) != 0;
        invalid |= copy_string(m, "cloud_provider", M->cloud_provider, sizeof(M->cloud_provider)) != 0;
        invalid |= copy_string(m, "cloud_instance_id", M->cloud_instance_id,
                               sizeof(M->cloud_instance_id)) != 0;
        invalid |= read_int(m, "cores", 0, 1, 10000, &M->cores_total) != 0;
        invalid |= read_int(m, "gpu_count", 0, 0, 1000, &M->gpu_count_total) != 0;
        invalid |= read_int(m, "ram_mb", 0, 0, 10000000, &M->ram_mb_total) != 0;
        invalid |= read_int(m, "disk_mb", 0, 0, 10000000, &M->disk_mb_total) != 0;
        invalid |= read_int(m, "cores_min", 0, 0, 10000, &M->cores_min) != 0;
        invalid |= read_int(m, "ram_mb_min", 0, 0, 10000000, &M->ram_mb_min) != 0;
        invalid |= read_int(m, "disk_mb_min", 0, 0, 10000000, &M->disk_mb_min) != 0;

        cJSON *j_type = cJSON_GetObjectItemCaseSensitive(m, "type");
        if (cJSON_IsString(j_type) && strcmp(j_type->valuestring, "cloud") == 0)
            M->type = MACHINE_TYPE_CLOUD;
        else
            M->type = MACHINE_TYPE_STATIC;

        M->probe_status = MACHINE_PROBING;

        M->enabled = 1; /* default enabled */
        cJSON *en = cJSON_GetObjectItemCaseSensitive(m, "enabled");
        if (cJSON_IsBool(en)) M->enabled = cJSON_IsTrue(en) ? 1 : 0;

        if (invalid || !M->id[0] || M->cores_total < 1 ||
            registry_contains_unlocked(M->id) || !safe_machine_text(M->id) ||
            !safe_machine_text(M->hostname) || !safe_machine_text(M->ip) ||
            M->cores_min > M->cores_total || M->ram_mb_min > M->ram_mb_total ||
            M->disk_mb_min > M->disk_mb_total ||
            (M->type == MACHINE_TYPE_CLOUD &&
             ((strcmp(M->cloud_provider, "aws") != 0 &&
               strcmp(M->cloud_provider, "gcp") != 0 &&
               strcmp(M->cloud_provider, "azure") != 0) ||
              !M->cloud_instance_id[0] || !safe_machine_text(M->cloud_instance_id)))) {
            log_warn("registry", "Skipping invalid machine entry");
            continue;
        }

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
            int range_start = 0;
            int range_end = 0;
            if (!safe_machine_text(prefix) || !prefix[0] || strlen(prefix) > 48 ||
                !isfinite(j_start->valuedouble) || !isfinite(j_end->valuedouble) ||
                floor(j_start->valuedouble) != j_start->valuedouble ||
                floor(j_end->valuedouble) != j_end->valuedouble ||
                j_start->valuedouble < 0 || j_end->valuedouble > 100000000 ||
                j_start->valuedouble > j_end->valuedouble) continue;
            range_start = (int)j_start->valuedouble;
            range_end = (int)j_end->valuedouble;

            /* Optional format strings */
            cJSON *j_hfmt = cJSON_GetObjectItemCaseSensitive(p, "hostname_format");
            cJSON *j_ifmt = cJSON_GetObjectItemCaseSensitive(p, "ip_format");
            const char *hostname_fmt = cJSON_IsString(j_hfmt) ? j_hfmt->valuestring : NULL;
            const char *ip_fmt       = cJSON_IsString(j_ifmt) ? j_ifmt->valuestring : NULL;
            if ((hostname_fmt && !valid_pool_format(hostname_fmt)) ||
                (ip_fmt && !valid_pool_format(ip_fmt))) {
                log_warn("registry", "Skipping pool '%s': invalid format string", prefix);
                continue;
            }

            int pool_cores = 0, pool_gpu = 0, pool_ram = 0, pool_disk = 0, pool_enabled = 1;
            if (read_int(p, "cores", 0, 1, 10000, &pool_cores) != 0 ||
                read_int(p, "gpu_count", 0, 0, 1000, &pool_gpu) != 0 ||
                read_int(p, "ram_mb", 0, 0, 10000000, &pool_ram) != 0 ||
                read_int(p, "disk_mb", 0, 0, 10000000, &pool_disk) != 0 ||
                pool_cores < 1) continue;
            cJSON *en = cJSON_GetObjectItemCaseSensitive(p, "enabled");
            if (cJSON_IsBool(en)) pool_enabled = cJSON_IsTrue(en) ? 1 : 0;
            else if (en) continue;

            int width = 1, tmp = range_end;
            while (tmp >= 10) { width++; tmp /= 10; }

            for (int i = range_start; i <= range_end && s_count < MAX_MACHINES; i++) {
                Machine *M = &s_machines[s_count];
                memset(M, 0, sizeof(*M));

                snprintf(M->id,       sizeof(M->id),       "%s%0*d", prefix, width, i);

                if (hostname_fmt)
                    snprintf(M->hostname, sizeof(M->hostname), hostname_fmt, i);
                else
                    snprintf(M->hostname, sizeof(M->hostname), "%s%0*d", prefix, width, i);

                if (ip_fmt)
                    snprintf(M->ip, sizeof(M->ip), ip_fmt, i);

                if (!safe_machine_text(M->id) || !safe_machine_text(M->hostname) ||
                    !safe_machine_text(M->ip) || registry_contains_unlocked(M->id)) continue;

                M->enabled         = pool_enabled;
                M->cores_total     = pool_cores;
                M->gpu_count_total = pool_gpu;
                M->ram_mb_total    = pool_ram;
                M->disk_mb_total   = pool_disk;
                M->type            = MACHINE_TYPE_STATIC;
                M->probe_status    = MACHINE_PROBING;
                s_count++;
            }

            log_info("registry", "Pool '%s': expanded %d-%d (%d machines)",
                     prefix, range_start, range_end, range_end - range_start + 1);
        }
    }

    int loaded_count = s_count;
    registry_unlock();
    cJSON_Delete(root);
    log_info("registry", "Loaded %d machine(s) from %s", loaded_count, json_path);
    return loaded_count;
}

int registry_get_copy(const char *machine_id, Machine *out)
{
    if (!machine_id || !out) return -1;
    int result = -1;
    registry_lock();
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_machines[i].id, machine_id) == 0) {
            *out = s_machines[i];
            result = 0;
            break;
        }
    }
    registry_unlock();
    return result;
}

int registry_snapshot(Machine **out_machines)
{
    if (!out_machines) return -1;
    *out_machines = NULL;
    registry_lock();
    int count = s_count;
    if (count > 0) {
        *out_machines = (Machine *)malloc((size_t)count * sizeof(Machine));
        if (!*out_machines) {
            registry_unlock();
            return -1;
        }
        memcpy(*out_machines, s_machines, (size_t)count * sizeof(Machine));
    }
    registry_unlock();
    return count;
}

int registry_upsert(const Machine *m)
{
    if (!m || !m->id[0] || !safe_machine_text(m->id) ||
        m->cores_total < 1 || m->gpu_count_total < 0 ||
        m->ram_mb_total < 0 || m->disk_mb_total < 0 ||
        m->cores_min < 0 || m->ram_mb_min < 0 || m->disk_mb_min < 0 ||
        m->cores_min > m->cores_total || m->ram_mb_min > m->ram_mb_total ||
        m->disk_mb_min > m->disk_mb_total) return -1;
    registry_lock();
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_machines[i].id, m->id) == 0) {
            int cores_reserved = s_machines[i].cores_reserved;
            int gpu_reserved = s_machines[i].gpu_count_reserved;
            int ram_reserved = s_machines[i].ram_mb_reserved;
            int disk_reserved = s_machines[i].disk_mb_reserved;
            MachineStatus probe_status = s_machines[i].probe_status;
            time_t last_probe_time = s_machines[i].last_probe_time;
            int probe_fail_count = s_machines[i].probe_fail_count;
            if ((long long)m->cores_total < (long long)cores_reserved + m->cores_min ||
                m->gpu_count_total < gpu_reserved ||
                (long long)m->ram_mb_total < (long long)ram_reserved + m->ram_mb_min ||
                (long long)m->disk_mb_total < (long long)disk_reserved + m->disk_mb_min) {
                registry_unlock();
                return -2;
            }
            s_machines[i] = *m;
            s_machines[i].cores_reserved = cores_reserved;
            s_machines[i].gpu_count_reserved = gpu_reserved;
            s_machines[i].ram_mb_reserved = ram_reserved;
            s_machines[i].disk_mb_reserved = disk_reserved;
            s_machines[i].probe_status = probe_status;
            s_machines[i].last_probe_time = last_probe_time;
            s_machines[i].probe_fail_count = probe_fail_count;
            registry_unlock();
            return 0;
        }
    }
    if (s_count >= MAX_MACHINES) {
        registry_unlock();
        return -1;
    }
    s_machines[s_count++] = *m;
    registry_unlock();
    return 0;
}

int registry_remove(const char *machine_id)
{
    if (!machine_id) return -1;
    registry_lock();
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_machines[i].id, machine_id) == 0) {
            if (s_machines[i].cores_reserved > 0 ||
                s_machines[i].gpu_count_reserved > 0 ||
                s_machines[i].ram_mb_reserved > 0 ||
                s_machines[i].disk_mb_reserved > 0) {
                registry_unlock();
                return -2;
            }
            s_machines[i] = s_machines[--s_count];
            registry_unlock();
            return 0;
        }
    }
    registry_unlock();
    return -1;
}

int registry_reserve(const char *machine_id, int cores, int gpu,
                     int ram_mb, int disk_mb)
{
    if (!machine_id || cores < 0 || gpu < 0 || ram_mb < 0 || disk_mb < 0)
        return -1;
    int result = -1;
    registry_lock();
    for (int i = 0; i < s_count; i++) {
        Machine *machine = &s_machines[i];
        if (strcmp(machine->id, machine_id) != 0) continue;
        int free_cores = machine->cores_total - machine->cores_reserved - machine->cores_min;
        int free_gpu = machine->gpu_count_total - machine->gpu_count_reserved;
        int free_ram = machine->ram_mb_total - machine->ram_mb_reserved - machine->ram_mb_min;
        int free_disk = machine->disk_mb_total - machine->disk_mb_reserved - machine->disk_mb_min;
        if (machine->enabled && machine->probe_status == MACHINE_ONLINE &&
            free_cores >= cores && free_gpu >= gpu &&
            free_ram >= ram_mb && free_disk >= disk_mb) {
            machine->cores_reserved += cores;
            machine->gpu_count_reserved += gpu;
            machine->ram_mb_reserved += ram_mb;
            machine->disk_mb_reserved += disk_mb;
            result = 0;
        }
        break;
    }
    registry_unlock();
    return result;
}

int registry_release(const char *machine_id, int cores, int gpu,
                     int ram_mb, int disk_mb)
{
    if (!machine_id || cores < 0 || gpu < 0 || ram_mb < 0 || disk_mb < 0)
        return -1;
    int result = -1;
    registry_lock();
    for (int i = 0; i < s_count; i++) {
        Machine *machine = &s_machines[i];
        if (strcmp(machine->id, machine_id) != 0) continue;
        machine->cores_reserved -= cores;
        machine->gpu_count_reserved -= gpu;
        machine->ram_mb_reserved -= ram_mb;
        machine->disk_mb_reserved -= disk_mb;
        if (machine->cores_reserved < 0) machine->cores_reserved = 0;
        if (machine->gpu_count_reserved < 0) machine->gpu_count_reserved = 0;
        if (machine->ram_mb_reserved < 0) machine->ram_mb_reserved = 0;
        if (machine->disk_mb_reserved < 0) machine->disk_mb_reserved = 0;
        result = 0;
        break;
    }
    registry_unlock();
    return result;
}

int registry_update_probe(const char *machine_id, MachineStatus status,
                          time_t probe_time, int reachable)
{
    if (!machine_id) return -1;
    int result = -1;
    registry_lock();
    for (int i = 0; i < s_count; i++) {
        Machine *machine = &s_machines[i];
        if (strcmp(machine->id, machine_id) != 0) continue;
        machine->probe_status = status;
        machine->last_probe_time = probe_time;
        if (reachable > 0) machine->probe_fail_count = 0;
        else if (reachable == 0) machine->probe_fail_count++;
        result = 0;
        break;
    }
    registry_unlock();
    return result;
}
