#include "decision_core.h"
#include "log.h"
#include "db.h"
#include "transfer.h"
#include "job.h"
#include "config.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

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
} Pick;

static int compare_pick_ratio_desc(const void *a, const void *b)
{
    const Pick *pa = (const Pick *)a;
    const Pick *pb = (const Pick *)b;
    if (pa->ratio < pb->ratio) return 1;
    if (pa->ratio > pb->ratio) return -1;
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
    double threshold = ctx->local_error_estimate > 0.0 ? ctx->local_error_estimate : 0.03;

    /* Read presim.json into buffer once (if present) and detect zone count and arrays */
    char *buf = NULL; long len = 0;
    if (f) {
        fseek(f, 0, SEEK_END); len = ftell(f); fseek(f, 0, SEEK_SET);
        if (len > 0 && len < 256*1024) {
            buf = (char*)malloc(len + 1);
            if (!buf) { fclose(f); return -1; }
            if (fread(buf, 1, len, f) != (size_t)len) {
                free(buf); buf = NULL; fclose(f); f = NULL;
            } else {
                buf[len] = '\0';
                /* forgiving JSON parse: look for error_threshold */
                const char *p = strstr(buf, "\"error_threshold\"");
                if (p) {
                    const char *num = strchr(p, ':');
                    if (num) threshold = atof(num + 1);
                }
                /* detect number of entries in zone_errors or zone_sizes arrays */
                const char *pcount = strstr(buf, "\"zone_errors\"");
                int detected = 0;
                if (!pcount) pcount = strstr(buf, "\"zone_sizes\"");
                if (pcount) {
                    const char *arr = strchr(pcount, '[');
                    if (arr) {
                        const char *q = arr + 1;
                        while (*q && *q != ']') {
                            while (*q && !((*q>='0' && *q<='9') || *q=='-' || *q=='.')) q++;
                            if (!*q || *q==']') break;
                            detected++;
                            while (*q && *q!=',' && *q!=']') q++;
                            if (*q==',') q++;
                        }
                        if (detected > 0) zones = detected;
                    }
                }
            }
        }
        fclose(f);
    }

    if (zones <= 0) zones = 8;
    if (threshold <= 0.0) threshold = 1e-6;
    /* Enforce a configurable upper bound on threshold to prefer refinement */
    if (g_config.presim_threshold_max > 0.0 && threshold > g_config.presim_threshold_max) threshold = g_config.presim_threshold_max;

    /* Allocate arrays sized to detected zones */
    double *zone_sizes = (double*)malloc(sizeof(double) * zones);
    if (!zone_sizes) { if (buf) free(buf); return -1; }
    for (int i = 0; i < zones; i++) zone_sizes[i] = 1.0;

    double *zone_errors = (double*)malloc(sizeof(double) * zones);
    if (!zone_errors) { free(zone_sizes); if (buf) free(buf); return -1; }
    for (int i = 0; i < zones; i++) zone_errors[i] = -1.0; /* sentinel -> use deterministic */

    double *zone_uncertainty = (double*)malloc(sizeof(double) * zones);
    if (!zone_uncertainty) { free(zone_sizes); free(zone_errors); if (buf) free(buf); return -1; }
    for (int i = 0; i < zones; i++) zone_uncertainty[i] = 0.0;

    /* If we have JSON buffer, parse arrays into allocated storage */
    if (buf) {
        const char *p = strstr(buf, "\"zone_sizes\"");
        if (p) {
            const char *arr = strchr(p, '[');
            if (arr) {
                arr++;
                for (int i = 0; i < zones && *arr; i++) {
                    while (*arr && (*arr==' ' || *arr=='\n' || *arr=='\r' || *arr=='\t' || *arr==',')) arr++;
                    if (!*arr || *arr==']') break;
                    zone_sizes[i] = atof(arr);
                    const char *next = arr;
                    while (*next && *next!=',' && *next!=']') next++;
                    if (*next==',') arr = next + 1; else { arr = next; }
                }
            }
        }
        p = strstr(buf, "\"zone_errors\"");
        if (p) {
            const char *arr = strchr(p, '[');
            if (arr) {
                arr++;
                for (int i = 0; i < zones && *arr; i++) {
                    while (*arr && (*arr==' ' || *arr=='\n' || *arr=='\r' || *arr=='\t' || *arr==',')) arr++;
                    if (!*arr || *arr==']') break;
                    zone_errors[i] = atof(arr);
                    const char *next = arr;
                    while (*next && *next!=',' && *next!=']') next++;
                    if (*next==',') arr = next + 1; else { arr = next; }
                }
            }
        }
        p = strstr(buf, "\"zone_uncertainty\"");
        if (p) {
            const char *arr = strchr(p, '[');
            if (arr) {
                arr++;
                for (int i = 0; i < zones && *arr; i++) {
                    while (*arr && (*arr==' ' || *arr=='\n' || *arr=='\r' || *arr=='\t' || *arr==',')) arr++;
                    if (!*arr || *arr==']') break;
                    zone_uncertainty[i] = atof(arr);
                    const char *next = arr;
                    while (*next && *next!=',' && *next!=']') next++;
                    if (*next==',') arr = next + 1; else { arr = next; }
                }
            }
        }
        free(buf);
    }

    /* Fidelity -> cores mapping: parse from g_config.presim_fidelity_map CSV (e.g. "0,1,3,6") */
    int fidelity_map_parsed[8];
    for (int i = 0; i < (int)(sizeof(fidelity_map_parsed)/sizeof(fidelity_map_parsed[0])); i++) fidelity_map_parsed[i] = 0;
    int max_fidelity = 3;
    {
        char fmap[128]; strncpy(fmap, g_config.presim_fidelity_map, sizeof(fmap)-1); fmap[sizeof(fmap)-1] = '\0';
        int idx = 0;
        char *t = strtok(fmap, ",");
        while (t && idx < (int)(sizeof(fidelity_map_parsed)/sizeof(fidelity_map_parsed[0]))) {
            fidelity_map_parsed[idx++] = atoi(t);
            t = strtok(NULL, ",");
        }
        if (idx > 1) max_fidelity = idx - 1;
        if (max_fidelity < 1) max_fidelity = 1;
    }

    /* Build allocation JSON: include size and priority = error/size */
    char alloc[4096]; alloc[0] = '\0';
    char zone_buf[512];
    int refine_count = 0;
    size_t off = 0;
    off += snprintf(alloc + off, sizeof(alloc) - off, "{\"job_id\":\"%s\",\"zones\":[", job->id);
    double base_err = ctx->local_error_estimate > 0.0 ? ctx->local_error_estimate : 0.05;
    uint32_t total_req_cores = 0;
    for (int i = 0; i < zones; i++) {
        double err = zone_errors[i] >= 0.0 ? zone_errors[i] : deterministic_zone_error(job->id, i, base_err);
        double size = zone_sizes[i] > 0.0 ? zone_sizes[i] : 1.0;
        double uncertainty = zone_uncertainty[i] >= 0.0 ? zone_uncertainty[i] : 0.0;
        double priority = (err / size) * (1.0 + g_config.presim_uncertainty_weight * uncertainty);

        /* Decide per-zone fidelity using configured multipliers */
        int fidelity = 1;
        if (priority > threshold * g_config.presim_high_multiplier) fidelity = max_fidelity;
        else if (priority > threshold * g_config.presim_refine_multiplier) fidelity = 2;
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

    /* Copy initial allocation JSON into result buffer by default */
    strncpy(out->allocation_json, alloc, sizeof(out->allocation_json)-1);

    /* If over budget, select subset to refine using greedy priority per core ratio */
    uint32_t want = total_req_cores;
    uint32_t cap = ctx->available_cpus > 0 ? ctx->available_cpus : want;
    if (want > cap && cap > 0) {
        /* build array of indices and ratios */
        Pick *picks = (Pick *)malloc(sizeof(Pick) * zones);
        int pcount = 0;
        for (int i = 0; i < zones; i++) {
            /* parse the per-zone req_cores from alloc JSON is cumbersome; instead recompute quickly */
            /* Recompute fidelity decision to get req_cores used earlier */
            double err = zone_errors[i] >= 0.0 ? zone_errors[i] : deterministic_zone_error(job->id, i, base_err);
            double size = zone_sizes[i] > 0.0 ? zone_sizes[i] : 1.0;
            double uncertainty = zone_uncertainty[i] >= 0.0 ? zone_uncertainty[i] : 0.0;
            double priority = (err / size) * (1.0 + g_config.presim_uncertainty_weight * uncertainty);
            int fidelity = 1;
            if (priority > threshold * g_config.presim_high_multiplier) fidelity = max_fidelity;
            else if (priority > threshold * g_config.presim_refine_multiplier) fidelity = 2;
            int req = (fidelity >=1 && fidelity <= max_fidelity) ? fidelity_map_parsed[fidelity] : 1;
            picks[pcount].idx = i; picks[pcount].ratio = priority / (double)req; picks[pcount].req = req; pcount++;
        }
        /* sort descending by ratio */
        qsort(picks, pcount, sizeof(Pick), compare_pick_ratio_desc);
        /* select until cap */
        uint32_t used = 0;
        int *selected = (int*)calloc(zones, sizeof(int));
        for (int i = 0; i < pcount; i++) {
            if (used + (uint32_t)picks[i].req <= cap) {
                selected[picks[i].idx] = 1; used += picks[i].req;
            } else {
                selected[picks[i].idx] = 0;
            }
        }
        /* rebuild allocation JSON to reflect selection (zero req_cores for unselected refinements) */
        off = 0;
        alloc[0] = '\0';
        off += snprintf(alloc + off, sizeof(alloc) - off, "{\"job_id\":\"%s\",\"zones\":[", job->id);
        int new_refine_count = 0;
        for (int i = 0; i < zones; i++) {
            double err = zone_errors[i] >= 0.0 ? zone_errors[i] : deterministic_zone_error(job->id, i, base_err);
            double size = zone_sizes[i] > 0.0 ? zone_sizes[i] : 1.0;
            double uncertainty = zone_uncertainty[i] >= 0.0 ? zone_uncertainty[i] : 0.0;
            double priority = (err / size) * (1.0 + g_config.presim_uncertainty_weight * uncertainty);
            int fidelity = 1;
            if (priority > threshold * g_config.presim_high_multiplier) fidelity = max_fidelity;
            else if (priority > threshold * g_config.presim_refine_multiplier) fidelity = 2;
            if (!selected[i]) fidelity = 1; /* force coarse if not selected */
            int req_cores = (fidelity >=1 && fidelity <= max_fidelity) ? fidelity_map_parsed[fidelity] : 1;
            if (fidelity > 1) new_refine_count++;
            snprintf(zone_buf, sizeof(zone_buf),
                     "%s{\"zone\":%d,\"error\":%.6f,\"size\":%.6f,\"priority\":%.6f,\"fidelity\":%d,\"req_cores\":%d}",
                     (i==0)?"":"", i, err, size, priority, fidelity, req_cores);
            if (off < sizeof(alloc)) off += snprintf(alloc + off, sizeof(alloc) - off, "%s", zone_buf);
            if (i + 1 < zones) { if (off < sizeof(alloc)) off += snprintf(alloc + off, sizeof(alloc) - off, ","); }
        }
        if (off < sizeof(alloc)) off += snprintf(alloc + off, sizeof(alloc) - off, "],\"threshold\":%.6f,\"zones_count\":%d}" , threshold, zones);
        strncpy(out->allocation_json, alloc, sizeof(out->allocation_json) - 1);

        want = used;
        refine_count = new_refine_count;
        free(picks); free(selected);
    } else {
        /* within budget, keep original allocation JSON */
        /* out->allocation_json already set above */
    }

    /* Decide overall strategy */
    if (refine_count == 0) {
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
