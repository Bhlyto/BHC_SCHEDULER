#include "config.h"
#include "db.h"
#include "job.h"
#include "transfer.h"
#include "decision_core.h"
#include "platform.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* Minimal platform stubs required by the core sources linked into this test. */
void platform_service_start(int argc, char **argv)
{
    (void)argc;
    (void)argv;
}

void platform_request_stop(void)
{
}

int platform_stop_requested(void)
{
    return 1;
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    config_defaults();
    config_load("config/orchestrator.conf");
    if (db_open(g_config.db_path) != 0) {
        fprintf(stderr, "db_open failed\n");
        return 1;
    }

    Job *job = job_create_ex("echo test", 50, 1, 0, 0, 0, "tester", "dc_test");
    if (!job) { fprintf(stderr, "job_create failed\n"); return 1; }

    /* create input dir and write presim.json */
    store_init_job_dirs(job->id);
    char presim_path[768]; store_input_dir(job->id, presim_path, sizeof(presim_path));
#ifdef _WIN32
    strcat(presim_path, "\\presim.json");
#else
    strcat(presim_path, "/presim.json");
#endif
    FILE *f = fopen(presim_path, "wb");
    if (f) {
        const char *js = "{\"error_threshold\": 0.05, \"zones\": 8}";
        fwrite(js, 1, strlen(js), f); fclose(f);
    }

    decision_core_init(NULL);
    dc_context_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.job_id = job->id;
    ctx.available_cpus = 4;
    ctx.available_mem_mb = 8192;
    ctx.local_error_estimate = 0.05;
    dc_result_t out;
    if (decision_core_decide(&ctx, &out) == 0) {
        printf("action=%d target_cores=%u\nallocation_json=%s\n",
               out.action, out.target_cores, out.allocation_json);
    } else {
        fprintf(stderr, "decision_core_decide failed\n");
    }

    decision_core_shutdown();
    db_close();
    return 0;
}
