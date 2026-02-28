#ifndef CONFIG_H
#define CONFIG_H

typedef struct {
    int  listen_port;
    char work_dir[512];
    char db_path[512];
    char provisioning_json[512];
    char log_level[16];
    int  cleanup_ttl_seconds;
    int  scheduler_poll_ms;
    char pre_job_script_win[512];
    char pre_job_script_linux[512];
} Config;

extern Config g_config;

int  config_load(const char *path);
void config_defaults(void);

#endif /* CONFIG_H */
