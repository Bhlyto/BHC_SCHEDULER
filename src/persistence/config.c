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
    strncpy(g_config.command_mode,       "free",      sizeof(g_config.command_mode)-1);
    strncpy(g_config.log_level,         "info",                       sizeof(g_config.log_level)-1);
    g_config.web_ui_enabled = 1;
    strncpy(g_config.listen_address, "0.0.0.0", sizeof(g_config.listen_address)-1);
    strncpy(g_config.probe_method, "tcp", sizeof(g_config.probe_method)-1);
    g_config.probe_port        = 22;
    g_config.probe_timeout_ms  = 3000;
    g_config.probe_retries     = 2;
    g_config.probe_interval_ms = 60000;
    g_config.cloud_auto_provision   = 0;
    g_config.cloud_auto_deprovision = 0;
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
        strncpy(g_config.presim_domains, "thermal", sizeof(g_config.presim_domains)-1);
        g_config.presim_threshold_max = 0.03;
        g_config.presim_refine_multiplier = 0.8;
        g_config.presim_high_multiplier = 2.0;
        g_config.presim_uncertainty_weight = 1.0;
        strncpy(g_config.presim_fidelity_map, "0,1,3,6", sizeof(g_config.presim_fidelity_map)-1);
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
        else if (strcmp(key, "ssh_shell")              == 0) strncpy(g_config.ssh_shell,             val, sizeof(g_config.ssh_shell)-1);
        else if (strcmp(key, "command_mode")           == 0) strncpy(g_config.command_mode,          val, sizeof(g_config.command_mode)-1);
        else if (strcmp(key, "web_ui_enabled")         == 0) g_config.web_ui_enabled                = atoi(val);
        else if (strcmp(key, "listen_address")         == 0) strncpy(g_config.listen_address,        val, sizeof(g_config.listen_address)-1);
        else if (strcmp(key, "probe_method")           == 0) strncpy(g_config.probe_method,          val, sizeof(g_config.probe_method)-1);
        else if (strcmp(key, "probe_port")             == 0) g_config.probe_port                    = atoi(val);
        else if (strcmp(key, "probe_timeout_ms")       == 0) g_config.probe_timeout_ms              = atoi(val);
        else if (strcmp(key, "probe_retries")          == 0) g_config.probe_retries                 = atoi(val);
        else if (strcmp(key, "probe_interval_ms")      == 0) g_config.probe_interval_ms             = atoi(val);
        else if (strcmp(key, "cloud_credentials_file") == 0) strncpy(g_config.cloud_credentials_file, val, sizeof(g_config.cloud_credentials_file)-1);
        else if (strcmp(key, "cloud_auto_provision")   == 0) g_config.cloud_auto_provision   = atoi(val);
        else if (strcmp(key, "cloud_auto_deprovision") == 0) g_config.cloud_auto_deprovision = atoi(val);
        else if (strcmp(key, "cloud_default_provider")      == 0) strncpy(g_config.cloud_default_provider,      val, sizeof(g_config.cloud_default_provider)-1);
        else if (strcmp(key, "cloud_default_instance_type") == 0) strncpy(g_config.cloud_default_instance_type, val, sizeof(g_config.cloud_default_instance_type)-1);
        else if (strcmp(key, "cloud_default_region")        == 0) strncpy(g_config.cloud_default_region,        val, sizeof(g_config.cloud_default_region)-1);
        else if (strcmp(key, "cloud_default_image_id")      == 0) strncpy(g_config.cloud_default_image_id,      val, sizeof(g_config.cloud_default_image_id)-1);
            else if (strcmp(key, "presim_threshold_max")         == 0) g_config.presim_threshold_max = atof(val);
        else if (strcmp(key, "presim_refine_multiplier")     == 0) g_config.presim_refine_multiplier = atof(val);
        else if (strcmp(key, "presim_high_multiplier")       == 0) g_config.presim_high_multiplier = atof(val);
        else if (strcmp(key, "presim_uncertainty_weight")    == 0) g_config.presim_uncertainty_weight = atof(val);
        else if (strcmp(key, "presim_fidelity_map")          == 0) strncpy(g_config.presim_fidelity_map, val, sizeof(g_config.presim_fidelity_map)-1);
            else if (strcmp(key, "presim_domains")              == 0) strncpy(g_config.presim_domains,              val, sizeof(g_config.presim_domains)-1);
        else fprintf(stderr, "[config] Unknown key: %s\n", key);
    }
    fclose(f);
    return 0;
}
