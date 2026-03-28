#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "solver.h"

/* Simple thermal presim: deterministic pseudo-random error estimates and sizes.
 * Writes a JSON file at out_path with fields compatible with decision_core.
 */

static double deterministic_zone_error(const char *job_id, int zone_idx, double base)
{
    unsigned int h = 2166136261u;
    const char *p = job_id ? job_id : "";
    while (*p) h = (h ^ (unsigned char)(*p++)) * 16777619u;
    h = (h ^ (unsigned int)zone_idx) * 16777619u;
    double m = 0.6 + (double)(h % 8000) / 10000.0;
    return base * m;
}

int solver_presim_run(const char *case_name,
                      const char *parent_job_id,
                      const char *parent_input_dir,
                      const char *out_path)
{
    (void)case_name; (void)parent_input_dir;
    if (!parent_job_id || !out_path) return -1;

    /* For thermal default: 8 zones, base error 0.05, zone sizes vary */
    int zones = 8;
    double base_err = 0.05;

    FILE *f = fopen(out_path, "wb");
    if (!f) return -1;

    fprintf(f, "{\n");
    fprintf(f, "  \"parent_job_id\":\"%s\",\n", parent_job_id);
    fprintf(f, "  \"case\":\"thermal\",\n");
    fprintf(f, "  \"method\":\"thermal_presim_v1\",\n");
    fprintf(f, "  \"timestamp\":\"%ld\",\n", (long)time(NULL));
    fprintf(f, "  \"error_threshold\": %.6f,\n", 0.05);
    fprintf(f, "  \"zones\": [\n");
    for (int i = 0; i < zones; i++) {
        double err = deterministic_zone_error(parent_job_id, i, base_err);
        double size = 1.0 + (i % 3) * 0.5; /* small variety */
        fprintf(f, "    {\"zone\":%d,\"error\":%.6f,\"size\":%.6f}%s\n",
                i, err, size, (i+1<zones)?",":"");
    }
    fprintf(f, "  ],\n");
    fprintf(f, "  \"zone_errors\": [");
    for (int i = 0; i < zones; i++) {
        double err = deterministic_zone_error(parent_job_id, i, base_err);
        fprintf(f, "%.6f%s", err, (i+1<zones)?",":"");
    }
    fprintf(f, "],\n");
    fprintf(f, "  \"zone_sizes\": [");
    for (int i = 0; i < zones; i++) {
        double size = 1.0 + (i % 3) * 0.5;
        fprintf(f, "%.6f%s", size, (i+1<zones)?",":"");
    }
    fprintf(f, "],\n");
    /* synthetic per-zone uncertainty (0..1) */
    fprintf(f, "  \"zone_uncertainty\": [");
    for (int i = 0; i < zones; i++) {
        double err = deterministic_zone_error(parent_job_id, i, base_err);
        double unc = err / (base_err * 2.0) * (0.5 + (i % 2) * 0.5);
        if (unc < 0.0) unc = 0.0;
        if (unc > 1.0) unc = 1.0;
        fprintf(f, "%.4f%s", unc, (i+1<zones)?",":"");
    }
    fprintf(f, "],\n");
    fprintf(f, "  \"notes\": \"thermal presim (synthetic)\"\n");
    fprintf(f, "}\n");

    fclose(f);
    return 0;
}

int solver_simulate(const double *zone_errors, const double *zone_sizes, int zones,
                    const int *fidelities, unsigned int *est_runtime_ms, double *final_error_avg)
{
    if (!zone_errors || !zone_sizes || zones <= 0 || !fidelities) return -1;
    const int fidelity_map[] = {0, 1, 2, 4};
    const double reduction[] = {1.0, 1.0, 0.6, 0.25}; /* fidelity -> error multiplier */
    const double base_ms_per_size = 10.0; /* heuristic base time */

    double weighted_error = 0.0, total_size = 0.0;
    unsigned int total_ms = 0;
    for (int i = 0; i < zones; i++) {
        double size = zone_sizes[i] > 0.0 ? zone_sizes[i] : 1.0;
        int f = fidelities[i]; if (f < 1) f = 1; if (f > 3) f = 3;
        double err_final = zone_errors[i] * reduction[f];
        weighted_error += err_final * size;
        total_size += size;
        int req = fidelity_map[f];
        double t = size * base_ms_per_size * (double)req;
        total_ms += (unsigned int)t;
    }
    if (total_size <= 0.0) total_size = 1.0;
    if (final_error_avg) *final_error_avg = weighted_error / total_size;
    if (est_runtime_ms) *est_runtime_ms = total_ms;
    return 0;
}
