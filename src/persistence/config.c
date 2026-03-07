#include "config.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

Config g_config;

void config_defaults(void)
{
    memset(&g_config, 0, sizeof(g_config));
    g_config.listen_port          = 8080;
    g_config.cleanup_ttl_seconds  = 3600;
    g_config.scheduler_poll_ms    = 500;
    g_config.job_timeout_seconds  = 0;
    strncpy(g_config.ssh_remote_work_dir, "/tmp/orch", sizeof(g_config.ssh_remote_work_dir)-1);
    strncpy(g_config.log_level,         "info",                       sizeof(g_config.log_level)-1);
#ifdef _WIN32
    strncpy(g_config.apps_dir,          "config\\apps",                sizeof(g_config.apps_dir)-1);
    strncpy(g_config.work_dir,          "jobs",                       sizeof(g_config.work_dir)-1);
    strncpy(g_config.db_path,           "orchestrator.db",            sizeof(g_config.db_path)-1);
    strncpy(g_config.provisioning_json, "config\\provisioning.json",  sizeof(g_config.provisioning_json)-1);
#else
    strncpy(g_config.work_dir,          "jobs",                       sizeof(g_config.work_dir)-1);
    strncpy(g_config.db_path,           "orchestrator.db",            sizeof(g_config.db_path)-1);
    strncpy(g_config.apps_dir,          "config/apps",                sizeof(g_config.apps_dir)-1);
    strncpy(g_config.provisioning_json, "config/provisioning.json",   sizeof(g_config.provisioning_json)-1);
    strncpy(g_config.pid_file,          "/var/run/orchestrator.pid",  sizeof(g_config.pid_file)-1);
#endif
}

static void trim(char *s)
{
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
        else if (strcmp(key, "cleanup_ttl_seconds")    == 0) g_config.cleanup_ttl_seconds      = atoi(val);
        else if (strcmp(key, "scheduler_poll_ms")      == 0) g_config.scheduler_poll_ms        = atoi(val);
        else if (strcmp(key, "pre_job_script_win")     == 0) strncpy(g_config.pre_job_script_win,   val, sizeof(g_config.pre_job_script_win)-1);
        else if (strcmp(key, "pre_job_script_linux")   == 0) strncpy(g_config.pre_job_script_linux, val, sizeof(g_config.pre_job_script_linux)-1);
        else if (strcmp(key, "job_timeout_seconds")    == 0) g_config.job_timeout_seconds         = atoi(val);
        else if (strcmp(key, "ssh_user")               == 0) strncpy(g_config.ssh_user,              val, sizeof(g_config.ssh_user)-1);
        else if (strcmp(key, "ssh_key")                == 0) strncpy(g_config.ssh_key,               val, sizeof(g_config.ssh_key)-1);
        else if (strcmp(key, "ssh_remote_work_dir")    == 0) strncpy(g_config.ssh_remote_work_dir,   val, sizeof(g_config.ssh_remote_work_dir)-1);
        else if (strcmp(key, "apps_dir")               == 0) strncpy(g_config.apps_dir,              val, sizeof(g_config.apps_dir)-1);
        else if (strcmp(key, "temp_dir")               == 0) strncpy(g_config.temp_dir,              val, sizeof(g_config.temp_dir)-1);
        else if (strcmp(key, "pid_file")               == 0) strncpy(g_config.pid_file,              val, sizeof(g_config.pid_file)-1);
        else fprintf(stderr, "[config] Unknown key: %s\n", key);
    }
    fclose(f);
    return 0;
}
