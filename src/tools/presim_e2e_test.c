#include "config.h"
#include "db.h"
#include "job.h"
#include "transfer.h"
#include "decision_core.h"
#include "platform.h"
#include "solver.h"
#include "cJSON.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

void platform_service_start(int a, char **b) { (void)a; (void)b; }
void platform_request_stop(void) { }
int platform_stop_requested(void) { return 1; }

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    config_defaults();
    config_load("config/orchestrator.conf");
    if (db_open(g_config.db_path) != 0) { fprintf(stderr, "db_open failed\n"); return 1; }

    Job *job = job_create_ex("run thermal main", 50, 1, 0, 0, 0, "tester", "presim_e2e");
    if (!job) { fprintf(stderr, "job_create failed\n"); return 1; }
    store_init_job_dirs(job->id);

#if defined(_WIN32)
#define SEP "\\"
#else
#define SEP "/"
#endif

    /* Run presim to produce presim.json in job input dir */
    char presim_path[1024]; store_input_dir(job->id, presim_path, sizeof(presim_path));
#ifdef _WIN32
    strcat(presim_path, "\\presim.json");
#else
    strcat(presim_path, "/presim.json");
#endif
    /* Allow overriding the presim solver via environment variable PRESIM_SOLVER_CMD
     * Example: PRESIM_SOLVER_CMD="python3 tools/python_solvers/thermal_high_fidelity.py --parent {parent} --out {out} --input-dir {input_dir}"
     */
    const char *env_cmd = getenv("PRESIM_SOLVER_CMD");
    if (env_cmd && env_cmd[0]) {
        char cmd[2048];
        const char *templ = env_cmd;
        /* Replace placeholders {parent}, {out}, {input_dir} */
        snprintf(cmd, sizeof(cmd), "%s", templ);
        /* naive replacement: handle common placeholders */
        char tmp[2048]; strncpy(tmp, cmd, sizeof(tmp)-1); tmp[sizeof(tmp)-1] = '\0';
        /* replace {parent} */
        char *p = strstr(tmp, "{parent}");
        if (p) {
            char buf2[2048]; *p = '\0'; snprintf(buf2, sizeof(buf2), "%s%s%s", tmp, job->id, p + 8);
            strncpy(tmp, buf2, sizeof(tmp)-1); tmp[sizeof(tmp)-1] = '\0';
        }
        /* replace {out} */
        p = strstr(tmp, "{out}");
        if (p) {
            char buf2[2048]; *p = '\0'; snprintf(buf2, sizeof(buf2), "%s%s%s", tmp, presim_path, p + 5);
            strncpy(tmp, buf2, sizeof(tmp)-1); tmp[sizeof(tmp)-1] = '\0';
        }
        /* replace {input_dir} */
        char input_dir[1024]; store_input_dir(job->id, input_dir, sizeof(input_dir));
        p = strstr(tmp, "{input_dir}");
        if (p) {
            char buf2[2048]; *p = '\0'; snprintf(buf2, sizeof(buf2), "%s%s%s", tmp, input_dir, p + 11);
            strncpy(tmp, buf2, sizeof(tmp)-1); tmp[sizeof(tmp)-1] = '\0';
        }
        /* run command */
        printf("Running external presim command: %s\n", tmp);
        int rc = system(tmp);
        if (rc != 0) { fprintf(stderr, "external presim command failed (%d)\n", rc); return 1; }
    } else {
        if (solver_presim_run("thermal", job->id, NULL, presim_path) != 0) {
            fprintf(stderr, "solver_presim_run failed\n"); return 1;
        }
    }
    printf("presim written: %s\n", presim_path);

    /* Prepare decision core context */
    decision_core_init(NULL);
    dc_context_t ctx; memset(&ctx,0,sizeof(ctx));
    ctx.job_id = job->id; ctx.available_cpus = 4; ctx.available_mem_mb = 8192; ctx.local_error_estimate = 0.05;
    dc_result_t out; memset(&out,0,sizeof(out));
    if (decision_core_decide(&ctx, &out) != 0) { fprintf(stderr, "decision_core_decide failed\n"); return 1; }

    printf("Decision action=%d target_cores=%u\n", out.action, out.target_cores);
    printf("Allocation JSON:\n%s\n", out.allocation_json);

    /* Parse presim.json to get errors and sizes */
    FILE *f = fopen(presim_path, "rb"); if (!f) { fprintf(stderr, "cannot open presim.json\n"); return 1; }
    fseek(f,0,SEEK_END); long len = ftell(f); fseek(f,0,SEEK_SET);
    char *buf = malloc(len+1); if (!buf) { fclose(f); return 1; }
    fread(buf,1,len,f); buf[len]='\0'; fclose(f);
    cJSON *root = cJSON_Parse(buf); if (!root) { fprintf(stderr, "invalid presim.json\n"); free(buf); return 1; }
    cJSON *zones = cJSON_GetObjectItem(root, "zones"); int nz = cJSON_GetArraySize(zones);
    double *zone_errors = calloc(nz, sizeof(double)); double *zone_sizes = calloc(nz, sizeof(double)); int *presim_fidelity = calloc(nz, sizeof(int));
    for (int i=0;i<nz;i++) {
        cJSON *z = cJSON_GetArrayItem(zones, i);
        zone_errors[i] = cJSON_GetObjectItem(z, "error") ? cJSON_GetObjectItem(z, "error")->valuedouble : 0.0;
        zone_sizes[i] = cJSON_GetObjectItem(z, "size") ? cJSON_GetObjectItem(z, "size")->valuedouble : 1.0;
        presim_fidelity[i] = 1; /* default */
    }

    /* Parse allocation_json to extract fidelities */
    cJSON *alloc = cJSON_Parse(out.allocation_json);
    if (alloc) {
        cJSON *a_z = cJSON_GetObjectItem(alloc, "zones"); int an = cJSON_GetArraySize(a_z);
        for (int i=0;i<an && i<nz;i++) {
            cJSON *az = cJSON_GetArrayItem(a_z, i);
            cJSON *fitem = cJSON_GetObjectItem(az, "fidelity"); if (fitem) presim_fidelity[i] = fitem->valueint;
        }
        cJSON_Delete(alloc);
    }

    /* Simulate full run (all fidelity=3) */
    int *full_fid = malloc(sizeof(int)*nz); for (int i=0;i<nz;i++) full_fid[i]=3;
    unsigned int rt_full=0; double err_full=0.0;
    solver_simulate(zone_errors, zone_sizes, nz, full_fid, &rt_full, &err_full);

    /* Simulate presim-guided run (respect fidelities from allocation) */
    unsigned int rt_ref=0; double err_ref=0.0; unsigned int presim_ms = nz * 5; /* heuristic */
    solver_simulate(zone_errors, zone_sizes, nz, presim_fidelity, &rt_ref, &err_ref);
    unsigned int total_presim_ms = presim_ms + rt_ref;

    printf("\nPer-zone selection:\n");
    printf("zone\terror\tsize\tfidelity\n");
    for (int i=0;i<nz;i++) printf("%d\t%.6f\t%.2f\t%d\n", i, zone_errors[i], zone_sizes[i], presim_fidelity[i]);

    printf("\nEstimated runtimes (ms): full=%u, presim_run=%u (presim=%u + refine=%u)\n", rt_full, total_presim_ms, presim_ms, rt_ref);
    printf("Estimated final avg error: full=%.6f, presim=%.6f\n", err_full, err_ref);

    /* human readable summary */
    printf("\nSummary:\n");
    if (total_presim_ms < rt_full) printf("Presim-guided workflow is faster by %d ms\n", rt_full - (int)total_presim_ms);
    else printf("Full run is faster by %d ms\n", (int)total_presim_ms - rt_full);
    if (err_ref < err_full) printf("Presim-guided yields lower avg error (%.6f vs %.6f)\n", err_ref, err_full);
    else printf("Full run yields lower avg error (%.6f vs %.6f)\n", err_full, err_ref);

    /* CI assertions: fail if presim-guided runtime is >10% slower OR avg error is >20% worse */
    double max_slowdown = 0.10; /* 10% */
    double max_error_increase = 0.20; /* 20% */
    int fail = 0;
    if ((double)total_presim_ms > (1.0 + max_slowdown) * (double)rt_full) {
        fprintf(stderr, "CI assertion failed: presim-guided runtime is > %.0f%% slower\n", max_slowdown*100);
        fail = 1;
    }
    if (err_ref > (1.0 + max_error_increase) * err_full) {
        fprintf(stderr, "CI assertion failed: presim-guided avg error is > %.0f%% worse\n", max_error_increase*100);
        fail = 1;
    }
    if (fail) {
        decision_core_shutdown(); db_close(); return 2;
    }

    free(full_fid); free(zone_errors); free(zone_sizes); free(presim_fidelity); free(buf);
    cJSON_Delete(root);
    decision_core_shutdown(); db_close();
    return 0;
}
