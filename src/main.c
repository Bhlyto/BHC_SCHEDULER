#include "platform.h"
#include "config.h"
#include "log.h"
#include "db.h"
#include "resources.h"
#include "scheduler.h"
#include "http.h"
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#  include <windows.h>
#  include <wincrypt.h>
#  define sleep_ms(ms) Sleep(ms)

static void exe_relative_path(const char *rel, char *out, size_t out_sz)
{
    char exe[MAX_PATH] = {0};
    GetModuleFileNameA(NULL, exe, sizeof(exe));
    char *sep = strrchr(exe, '\\');
    if (sep) { *(sep + 1) = '\0'; _snprintf(out, out_sz, "%s%s", exe, rel); }
    else      { strncpy(out, rel, out_sz - 1); }
}

static void resolve_config_paths(void)
{
    char tmp[MAX_PATH];
#define RESOLVE(field) \
    if (g_config.field[0] && !strchr(g_config.field, ':')) { \
        exe_relative_path(g_config.field, tmp, sizeof(tmp)); \
        strncpy(g_config.field, tmp, sizeof(g_config.field) - 1); \
    }
    RESOLVE(db_path);
    RESOLVE(work_dir);
    RESOLVE(provisioning_json);
    RESOLVE(pre_job_script_win);
    RESOLVE(ssh_key);
    RESOLVE(temp_dir);
#undef RESOLVE
}
#else
#  include <unistd.h>
#  define sleep_ms(ms) usleep((ms) * 1000)
#endif

void orchestrator_run(void)
{
    log_info("main", "Orchestrator starting up");

    if (db_open(g_config.db_path) != 0) {
        log_error("main", "Failed to open database at %s", g_config.db_path);
        return;
    }
    log_info("main", "Database opened: %s", g_config.db_path);

    int machine_count = registry_load(g_config.provisioning_json);
    if (machine_count < 0)
        log_warn("main", "No provisioning.json loaded (%s) — add machines via POST /provision", g_config.provisioning_json);
    else
        log_info("main", "%d machine(s) loaded from %s", machine_count, g_config.provisioning_json);

    scheduler_init();
    scheduler_start();
    log_info("main", "Scheduler started");

    if (httpd_start(g_config.listen_port) != 0) {
        log_error("main", "Failed to start HTTP server on port %d", g_config.listen_port);
        scheduler_stop();
        db_close();
        return;
    }
    log_info("main", "HTTP API listening on port %d", g_config.listen_port);
    log_info("main", "Ready. Send jobs to POST http://localhost:%d/jobs", g_config.listen_port);

    while (!platform_stop_requested())
        sleep_ms(250);

    log_info("main", "Shutting down …");
    httpd_stop();
    scheduler_stop();
    db_close();
    log_info("main", "Shutdown complete");
}

int main(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "keygen") == 0) {
        const char *label  = "default";
#ifdef _WIN32
        char _conf_buf[MAX_PATH]; exe_relative_path("config\\orchestrator.conf", _conf_buf, sizeof(_conf_buf));
        const char *conf = _conf_buf;
#else
        const char *conf   = "config/orchestrator.conf";
#endif
        for (int i = 2; i < argc - 1; i++) {
            if (strcmp(argv[i], "--label") == 0) label = argv[i+1];
            if (strcmp(argv[i], "--conf")  == 0) conf  = argv[i+1];
        }
        config_defaults();
        config_load(conf);
        log_set_level(g_config.log_level);
#ifdef _WIN32
        resolve_config_paths();
#endif

        if (db_open(g_config.db_path) != 0) {
            fprintf(stderr, "Cannot open DB at %s\n", g_config.db_path);
            return 1;
        }

        unsigned char raw[32];
#ifdef _WIN32
        HCRYPTPROV hprov;
        CryptAcquireContextA(&hprov, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT);
        CryptGenRandom(hprov, sizeof(raw), raw);
        CryptReleaseContext(hprov, 0);
#else
        FILE *urnd = fopen("/dev/urandom", "rb");
        if (!urnd || fread(raw, 1, sizeof(raw), urnd) != sizeof(raw)) {
            fprintf(stderr, "Cannot read /dev/urandom\n"); return 1;
        }
        if (urnd) fclose(urnd);
#endif
        char raw_hex[65] = {0};
        for (int i = 0; i < 32; i++) sprintf(raw_hex + i*2, "%02x", raw[i]);

        extern void auth_hash_key(const char *raw_key, char *out_hex_65);
        char hash[65];
        auth_hash_key(raw_hex, hash);

        if (db_insert_api_key(hash, label) == 0) {
            printf("API Key generated for label '%s':\n%s\n", label, raw_hex);
            printf("Store this key — it will not be shown again.\n");
        } else {
            fprintf(stderr, "Failed to insert key (duplicate?)\n");
        }
        db_close();
        return 0;
    }

#ifdef _WIN32
    char _conf_buf2[MAX_PATH]; exe_relative_path("config\\orchestrator.conf", _conf_buf2, sizeof(_conf_buf2));
    const char *conf = _conf_buf2;
#else
    const char *conf = "config/orchestrator.conf";
#endif
    for (int i = 1; i < argc - 1; i++)
        if (strcmp(argv[i], "--conf") == 0) conf = argv[i+1];

    config_defaults();
    config_load(conf);
    log_set_level(g_config.log_level);
#ifdef _WIN32
    resolve_config_paths();
#endif

    log_info("main", "Config loaded from %s", conf);
    platform_service_start(argc, argv);
    return 0;
}
