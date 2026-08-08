#include "decision_core.h"
#include "log.h"
#include "db.h"
#include "transfer.h"
#include "job.h"
#include "config.h"
#include "cJSON.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <errno.h>

#define DC_MAX_ZONES 64

/* Simple pre-simulation-based decision strategy
 * - looks for <work_dir>/<job_id>/input/presim.json
 * - presim.json (optional) fields: { "error_threshold": 0.05, "zones": 8 }
 * - if present, compute a deterministic per-zone error estimate and decide
 *   which zones to refine (error > threshold).
 * - produce allocation JSON in out->allocation_json describing zones,
 *   priorities and requested cores.
 */

static int s_initialized = 0;

typedef struct {
    int idx;
    double ratio;
    int req;
} dc_pick_t;

static int compare_picks_desc(const void *left, const void *right)
{
    const dc_pick_t *a = (const dc_pick_t *)left;
    const dc_pick_t *b = (const dc_pick_t *)right;
    if (a->ratio < b->ratio) return 1;
    if (a->ratio > b->ratio) return -1;
    return 0;
}

static double deterministic_zone_error(const char *job_id, int zone_idx, double base)
{
    /* Deterministic pseudo-random variation using job id and zone_idx */
    unsigned int h = 2166136261u;
    const char *p = job_id ? job_id : "";
    while (*p) h = (h ^ (unsigned char)(*p++)) * 16777619u;
    h = (h ^ (unsigned int)zone_idx) * 16777619u;
    /* Map to [0.6, 1.4] multiplier */
    double m = 0.6 + (double)(h % 8000) / 10000.0; /* 0.6..1.3999 */
    return base * m;
}

static int json_number(cJSON *item, double minimum, double maximum, double *out)
{
    if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble) ||
        item->valuedouble < minimum || item->valuedouble > maximum) return -1;
    *out = item->valuedouble;
    return 0;
}

static int parse_fidelity_map(int values[8], int *max_fidelity)
{
    memset(values, 0, sizeof(int) * 8);
    const char *cursor = g_config.presim_fidelity_map;
    int count = 0;
    while (cursor && *cursor && count < 8) {
        char *end = NULL;
        errno = 0;
        long value = strtol(cursor, &end, 10);
        if (errno == ERANGE || end == cursor || value < 0 || value > 10000 ||
            (count > 0 && value == 0)) return -1;
        values[count++] = (int)value;
        if (*end == '\0') {
            cursor = end;
            break;
        }
        if (*end != ',') return -1;
        cursor = end + 1;
    }
    if (count < 2 || (cursor && *cursor)) return -1;
    *max_fidelity = count - 1;
    return 0;
}

int decision_core_init(const char *config_path)
{
    (void)config_path;
    if (s_initialized) return 0;
    log_info("decision_core", "Decision core (presim strategy) initialized");
    s_initialized = 1;
    return 0;
}

int decision_core_decide(const dc_context_t *ctx, dc_result_t *out)
{
    if (!s_initialized) decision_core_init(NULL);
    if (!ctx || !out) return -1;

    memset(out, 0, sizeof(*out));
    out->action = DC_ACTION_NONE;
    out->target_cores = 0;
    out->notes = NULL;
    out->allocation_json[0] = '\0';

    if (!ctx->job_id) return 0;

    Job *job = db_get_job(ctx->job_id);
    if (!job) return 0;

    char input_dir[512]; store_input_dir(job->id, input_dir, sizeof(input_dir));
    char presim_path[768];
#ifdef _WIN32
    snprintf(presim_path, sizeof(presim_path), "%s\\presim.json", input_dir);
#else
    snprintf(presim_path, sizeof(presim_path), "%s/presim.json", input_dir);
#endif

    FILE *f = fopen(presim_path, "rb");
    int zones = 8;
    /* Conservative default threshold: prefer refining more zones when unsure */
    double threshold = isfinite(ctx->local_error_estimate) && ctx->local_error_estimate > 0.0
                     ? ctx->local_error_estimate : 0.03;
    double refine_multiplier = isfinite(g_config.presim_refine_multiplier) &&
                               g_config.presim_refine_multiplier > 0.0
                             ? g_config.presim_refine_multiplier : 0.8;
    double high_multiplier = isfinite(g_config.presim_high_multiplier) &&
                             g_config.presim_high_multiplier > 0.0
                           ? g_config.presim_high_multiplier : 2.0;
    double uncertainty_weight = isfinite(g_config.presim_uncertainty_weight) &&
                                g_config.presim_uncertainty_weight >= 0.0
                              ? g_config.presim_uncertainty_weight : 1.0;

    cJSON *presim = NULL;
    if (f) {
        if (fseek(f, 0, SEEK_END) != 0) { fclose(f); job_free(job); return -1; }
        long length = ftell(f);
        if (length <= 0 || length > 256 * 1024 || fseek(f, 0, SEEK_SET) != 0) {
            fclose(f);
            job_free(job);
            return -1;
        }
        char *data = (char *)malloc((size_t)length + 1);
        if (!data || fread(data, 1, (size_t)length, f) != (size_t)length) {
            free(data);
            fclose(f);
            job_free(job);
            return -1;
        }
        fclose(f);
        data[length] = '\0';
        presim = cJSON_Parse(data);
        free(data);
        if (!cJSON_IsObject(presim)) {
            cJSON_Delete(presim);
            job_free(job);
            return -1;
        }

        cJSON *threshold_item = cJSON_GetObjectItemCaseSensitive(presim, "error_threshold");
        if (threshold_item && json_number(threshold_item, 0.000001, 1000000.0, &threshold) != 0) {
            cJSON_Delete(presim);
            job_free(job);
            return -1;
        }
        cJSON *zones_item = cJSON_GetObjectItemCaseSensitive(presim, "zones");
        if (cJSON_IsArray(zones_item)) {
            zones = cJSON_GetArraySize(zones_item);
        } else if (cJSON_IsNumber(zones_item) && isfinite(zones_item->valuedouble) &&
                   floor(zones_item->valuedouble) == zones_item->valuedouble) {
            zones = (int)zones_item->valuedouble;
        } else {
            cJSON *errors = cJSON_GetObjectItemCaseSensitive(presim, "zone_errors");
            cJSON *sizes = cJSON_GetObjectItemCaseSensitive(presim, "zone_sizes");
            if (cJSON_IsArray(errors)) zones = cJSON_GetArraySize(errors);
            else if (cJSON_IsArray(sizes)) zones = cJSON_GetArraySize(sizes);
        }
        if (zones < 1 || zones > DC_MAX_ZONES) {
            cJSON_Delete(presim);
            job_free(job);
            return -1;
        }
    }

    /* Enforce a configurable upper bound on threshold to prefer refinement */
    if (g_config.presim_threshold_max > 0.0 && threshold > g_config.presim_threshold_max) threshold = g_config.presim_threshold_max;

    /* Allocate arrays sized to detected zones */
    double *zone_sizes = (double*)malloc(sizeof(double) * zones);
    if (!zone_sizes) { cJSON_Delete(presim); job_free(job); return -1; }
    for (int i = 0; i < zones; i++) zone_sizes[i] = 1.0;

    double *zone_errors = (double*)malloc(sizeof(double) * zones);
    if (!zone_errors) { free(zone_sizes); cJSON_Delete(presim); job_free(job); return -1; }
    for (int i = 0; i < zones; i++) zone_errors[i] = -1.0; /* sentinel -> use deterministic */

    double *zone_uncertainty = (double*)malloc(sizeof(double) * zones);
    if (!zone_uncertainty) {
        free(zone_sizes); free(zone_errors); cJSON_Delete(presim); job_free(job); return -1;
    }
    for (int i = 0; i < zones; i++) zone_uncertainty[i] = 0.0;

    int invalid_presim = 0;
    if (presim) {
        cJSON *zones_item = cJSON_GetObjectItemCaseSensitive(presim, "zones");
        if (cJSON_IsArray(zones_item)) {
            for (int i = 0; i < zones; i++) {
                cJSON *zone = cJSON_GetArrayItem(zones_item, i);
                if (!cJSON_IsObject(zone)) { invalid_presim = 1; break; }
                cJSON *error = cJSON_GetObjectItemCaseSensitive(zone, "error");
                cJSON *size = cJSON_GetObjectItemCaseSensitive(zone, "size");
                cJSON *uncertainty = cJSON_GetObjectItemCaseSensitive(zone, "uncertainty");
                if ((error && json_number(error, 0.0, 1000000.0, &zone_errors[i]) != 0) ||
                    (size && json_number(size, 0.000001, 1000000000.0, &zone_sizes[i]) != 0) ||
                    (uncertainty && json_number(uncertainty, 0.0, 1000000.0,
                                                &zone_uncertainty[i]) != 0)) {
                    invalid_presim = 1;
                    break;
                }
            }
        }
        cJSON *arrays[3] = {
            cJSON_GetObjectItemCaseSensitive(presim, "zone_errors"),
            cJSON_GetObjectItemCaseSensitive(presim, "zone_sizes"),
            cJSON_GetObjectItemCaseSensitive(presim, "zone_uncertainty")
        };
        double *targets[3] = { zone_errors, zone_sizes, zone_uncertainty };
        double minimums[3] = { 0.0, 0.000001, 0.0 };
        double maximums[3] = { 1000000.0, 1000000000.0, 1000000.0 };
        for (int array_index = 0; array_index < 3 && !invalid_presim; array_index++) {
            if (!arrays[array_index]) continue;
            if (!cJSON_IsArray(arrays[array_index]) ||
                cJSON_GetArraySize(arrays[array_index]) > zones) {
                invalid_presim = 1;
                break;
            }
            int count = cJSON_GetArraySize(arrays[array_index]);
            for (int i = 0; i < count; i++) {
                if (json_number(cJSON_GetArrayItem(arrays[array_index], i),
                                minimums[array_index], maximums[array_index],
                                &targets[array_index][i]) != 0) {
                    invalid_presim = 1;
                    break;
                }
            }
        }
        cJSON_Delete(presim);
    }
    if (invalid_presim) {
        free(zone_sizes); free(zone_errors); free(zone_uncertainty); job_free(job);
        return -1;
    }

    /* Fidelity -> cores mapping: parse from g_config.presim_fidelity_map CSV (e.g. "0,1,3,6") */
    int fidelity_map_parsed[8];
    int max_fidelity = 3;
    if (parse_fidelity_map(fidelity_map_parsed, &max_fidelity) != 0) {
        int defaults[] = {0, 1, 3, 6};
        memcpy(fidelity_map_parsed, defaults, sizeof(defaults));
        max_fidelity = 3;
        log_warn("decision_core", "Invalid fidelity map; using 0,1,3,6");
    }

    /* Build allocation JSON: include size and priority = error/size */
    char alloc[sizeof(out->allocation_json)]; alloc[0] = '\0';
    char zone_buf[512];
    int refine_count = 0;
    size_t off = 0;
    off += snprintf(alloc + off, sizeof(alloc) - off, "{\"job_id\":\"%s\",\"zones\":[", job->id);
    double base_err = isfinite(ctx->local_error_estimate) && ctx->local_error_estimate > 0.0
                    ? ctx->local_error_estimate : 0.05;
    uint32_t total_req_cores = 0;
    for (int i = 0; i < zones; i++) {
        double err = zone_errors[i] >= 0.0 ? zone_errors[i] : deterministic_zone_error(job->id, i, base_err);
        double size = zone_sizes[i] > 0.0 ? zone_sizes[i] : 1.0;
        double uncertainty = zone_uncertainty[i] >= 0.0 ? zone_uncertainty[i] : 0.0;
        double priority = (err / size) * (1.0 + uncertainty_weight * uncertainty);

        /* Decide per-zone fidelity using configured multipliers */
        int fidelity = 1;
        if (priority > threshold * high_multiplier) fidelity = max_fidelity;
        else if (priority > threshold * refine_multiplier) fidelity = 2;
        else fidelity = 1;
        if (fidelity > max_fidelity) fidelity = max_fidelity;

        int req_cores = 0;
        if (fidelity >= 1 && fidelity <= max_fidelity) req_cores = fidelity_map_parsed[fidelity];

        int must_refine = fidelity > 1 ? 1 : 0;
        if (must_refine) refine_count++;
        total_req_cores += req_cores;

        snprintf(zone_buf, sizeof(zone_buf),
                 "%s{\"zone\":%d,\"error\":%.6f,\"size\":%.6f,\"priority\":%.6f,\"fidelity\":%d,\"req_cores\":%d}",
                 (i==0)?"":"", i, err, size, priority, fidelity, req_cores);
        if (off < sizeof(alloc)) off += snprintf(alloc + off, sizeof(alloc) - off, "%s", zone_buf);
        if (i + 1 < zones) { if (off < sizeof(alloc)) off += snprintf(alloc + off, sizeof(alloc) - off, ","); }
    }
    if (off < sizeof(alloc)) off += snprintf(alloc + off, sizeof(alloc) - off, "],\"threshold\":%.6f,\"zones_count\":%d}" , threshold, zones);
    if (off >= sizeof(alloc)) {
        free(zone_sizes); free(zone_errors); free(zone_uncertainty); job_free(job);
        return -1;
    }

    /* Copy initial allocation JSON into result buffer by default */
    strncpy(out->allocation_json, alloc, sizeof(out->allocation_json)-1);
    out->allocation_json[sizeof(out->allocation_json)-1] = '\0';

    /* If over budget, select subset to refine using greedy priority per core ratio */
    uint32_t want = total_req_cores;
    uint32_t cap = ctx->available_cpus > 0 ? ctx->available_cpus : want;
    if (want > cap && cap > 0) {
        /* build array of indices and ratios */
        dc_pick_t *picks = (dc_pick_t*)malloc(sizeof(dc_pick_t) * zones);
        if (!picks) {
            free(zone_sizes);
            free(zone_errors);
            free(zone_uncertainty);
            job_free(job);
            return -1;
        }
        int pcount = 0;
        for (int i = 0; i < zones; i++) {
            /* parse the per-zone req_cores from alloc JSON is cumbersome; instead recompute quickly */
            /* Recompute fidelity decision to get req_cores used earlier */
            double err = zone_errors[i] >= 0.0 ? zone_errors[i] : deterministic_zone_error(job->id, i, base_err);
            double size = zone_sizes[i] > 0.0 ? zone_sizes[i] : 1.0;
            double uncertainty = zone_uncertainty[i] >= 0.0 ? zone_uncertainty[i] : 0.0;
            double priority = (err / size) * (1.0 + uncertainty_weight * uncertainty);
            int fidelity = 1;
            if (priority > threshold * high_multiplier) fidelity = max_fidelity;
            else if (priority > threshold * refine_multiplier) fidelity = 2;
            int req = (fidelity >=1 && fidelity <= max_fidelity) ? fidelity_map_parsed[fidelity] : 1;
            if (req <= 0) req = 1;
            picks[pcount].idx = i; picks[pcount].ratio = priority / (double)req; picks[pcount].req = req; pcount++;
        }
        /* sort descending by ratio */
        qsort(picks, pcount, sizeof(dc_pick_t), compare_picks_desc);
        /* Select the highest affordable fidelity per zone until the budget is full. */
        uint32_t used = 0;
        int *selected_fidelity = (int*)calloc((size_t)zones, sizeof(int));
        int *selected_req = (int*)calloc((size_t)zones, sizeof(int));
        if (!selected_fidelity || !selected_req) {
            free(selected_fidelity);
            free(selected_req);
            free(picks);
            free(zone_sizes);
            free(zone_errors);
            free(zone_uncertainty);
            job_free(job);
            return -1;
        }
        for (int i = 0; i < pcount; i++) {
            int zone_index = picks[i].idx;
            double err = zone_errors[zone_index] >= 0.0
                       ? zone_errors[zone_index]
                       : deterministic_zone_error(job->id, zone_index, base_err);
            double size = zone_sizes[zone_index] > 0.0 ? zone_sizes[zone_index] : 1.0;
            double uncertainty = zone_uncertainty[zone_index] >= 0.0
                               ? zone_uncertainty[zone_index] : 0.0;
            double priority = (err / size) * (1.0 + uncertainty_weight * uncertainty);
            int desired_fidelity = 1;
            if (priority > threshold * high_multiplier)
                desired_fidelity = max_fidelity;
            else if (priority > threshold * refine_multiplier)
                desired_fidelity = max_fidelity >= 2 ? 2 : 1;
            for (int fidelity = desired_fidelity; fidelity >= 1; fidelity--) {
                int requirement = fidelity_map_parsed[fidelity];
                if (requirement > 0 && used + (uint32_t)requirement <= cap) {
                    selected_fidelity[zone_index] = fidelity;
                    selected_req[zone_index] = requirement;
                    used += (uint32_t)requirement;
                    break;
                }
            }
        }
        /* Rebuild allocation JSON to reflect downgraded or deferred zones. */
        off = 0;
        alloc[0] = '\0';
        off += snprintf(alloc + off, sizeof(alloc) - off, "{\"job_id\":\"%s\",\"zones\":[", job->id);
        int new_refine_count = 0;
        for (int i = 0; i < zones; i++) {
            double err = zone_errors[i] >= 0.0 ? zone_errors[i] : deterministic_zone_error(job->id, i, base_err);
            double size = zone_sizes[i] > 0.0 ? zone_sizes[i] : 1.0;
            double uncertainty = zone_uncertainty[i] >= 0.0 ? zone_uncertainty[i] : 0.0;
            double priority = (err / size) * (1.0 + uncertainty_weight * uncertainty);
            int fidelity = selected_fidelity[i] > 0 ? selected_fidelity[i] : 1;
            int req_cores = selected_req[i];
            if (fidelity > 1) new_refine_count++;
            snprintf(zone_buf, sizeof(zone_buf),
                     "%s{\"zone\":%d,\"error\":%.6f,\"size\":%.6f,\"priority\":%.6f,\"fidelity\":%d,\"req_cores\":%d}",
                     (i==0)?"":"", i, err, size, priority, fidelity, req_cores);
            if (off < sizeof(alloc)) off += snprintf(alloc + off, sizeof(alloc) - off, "%s", zone_buf);
            if (i + 1 < zones) { if (off < sizeof(alloc)) off += snprintf(alloc + off, sizeof(alloc) - off, ","); }
        }
        if (off < sizeof(alloc)) off += snprintf(alloc + off, sizeof(alloc) - off, "],\"threshold\":%.6f,\"zones_count\":%d}" , threshold, zones);
        if (off >= sizeof(alloc)) {
            free(picks); free(selected_fidelity); free(selected_req);
            free(zone_sizes); free(zone_errors); free(zone_uncertainty); job_free(job);
            return -1;
        }
        strncpy(out->allocation_json, alloc, sizeof(out->allocation_json) - 1);
        out->allocation_json[sizeof(out->allocation_json) - 1] = '\0';

        want = used;
        refine_count = new_refine_count;
        free(picks); free(selected_fidelity); free(selected_req);
    } else {
        /* within budget, keep original allocation JSON */
        /* out->allocation_json already set above */
    }

    /* Decide overall strategy */
    if (want == 0) {
        out->action = DC_ACTION_DEFER;
    } else if (refine_count == 0) {
        out->action = DC_ACTION_RUN_COARSE_SIM; /* Full coarse */
    } else if (refine_count >= zones) {
        out->action = DC_ACTION_RUN_FINE_SIM;   /* Full refined */
    } else {
        out->action = DC_ACTION_REFINE;         /* Partially refined */
    }

    out->target_cores = want;

    log_info("decision_core", "Job %s: presim decided %d/%d zones refine, action=%d, cores=%u",
             job->id, refine_count, zones, out->action, out->target_cores);

    free(zone_sizes); free(zone_errors);
    if (zone_uncertainty) free(zone_uncertainty);

    job_free(job);
    return 0;
}

void decision_core_shutdown(void)
{
    if (!s_initialized) return;
    log_info("decision_core", "Decision core (presim strategy) shutting down");
    s_initialized = 0;
}
