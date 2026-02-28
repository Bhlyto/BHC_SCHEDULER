#include "config.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/*
 * config.c
 * Parses key=value config file and populates g_config.
 */

Config g_config;

void config_defaults(void)
{
    memset(&g_config, 0, sizeof(g_config));
    g_config.listen_port          = 8080;
    g_config.cleanup_ttl_seconds  = 3600;
    g_config.scheduler_poll_ms    = 500;
    strncpy(g_config.log_level,         "info",                       sizeof(g_config.log_level)-1);
#ifdef _WIN32
    strncpy(g_config.work_dir,          "C:\\orch\\jobs",             sizeof(g_config.work_dir)-1);
    strncpy(g_config.db_path,           "C:\\orch\\orchestrator.db",  sizeof(g_config.db_path)-1);
    strncpy(g_config.provisioning_json, "C:\\orch\\provisioning.json",sizeof(g_config.provisioning_json)-1);
#else
    strncpy(g_config.work_dir,          "/var/orch/jobs",             sizeof(g_config.work_dir)-1);
    strncpy(g_config.db_path,           "/var/orch/orchestrator.db",  sizeof(g_config.db_path)-1);
    strncpy(g_config.provisioning_json, "/etc/orch/provisioning.json",sizeof(g_config.provisioning_json)-1);
#endif
}

static void trim(char *s)
{
    /* Remove leading/trailing whitespace in-place */
    char *start = s;
    while (*start == ' ' || *start == '\t') start++;
    if (start != s) memmove(s, start, strlen(start) + 1);
    int len = (int)strlen(s);
    while (len > 0 && (s[len-1] == ' ' || s[len-1] == '\t' ||
                        s[len-1] == '\r'|| s[len-1] == '\n'))
        s[--len] = '\0';
}

int config_load(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "[config] Cannot open %s, using defaults\n", path);
        return 0; /* non-fatal */
    }

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        /* Strip comments */
        char *hash = strchr(line, '#');
        if (hash) *hash = '\0';
        trim(line);
        if (!line[0]) continue;

        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = line;
        char *val = eq + 1;
        trim(key);
        trim(val);

        if      (strcmp(key, "listen_port")          == 0) g_config.listen_port           = atoi(val);
        else if (strcmp(key, "work_dir")             == 0) strncpy(g_config.work_dir,           val, sizeof(g_config.work_dir)-1);
        else if (strcmp(key, "db_path")              == 0) strncpy(g_config.db_path,             val, sizeof(g_config.db_path)-1);
        else if (strcmp(key, "provisioning_json")    == 0) strncpy(g_config.provisioning_json,   val, sizeof(g_config.provisioning_json)-1);
        else if (strcmp(key, "log_level")            == 0) strncpy(g_config.log_level,           val, sizeof(g_config.log_level)-1);
        else if (strcmp(key, "cleanup_ttl_seconds")  == 0) g_config.cleanup_ttl_seconds    = atoi(val);
        else if (strcmp(key, "scheduler_poll_ms")    == 0) g_config.scheduler_poll_ms      = atoi(val);
        else fprintf(stderr, "[config] Unknown key: %s\n", key);
    }
    fclose(f);
    return 0;
}
