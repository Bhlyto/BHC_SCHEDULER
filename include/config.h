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
    int  job_timeout_seconds;   /* 0 = no timeout */

    char apps_dir[512];          /* directory containing app definition JSON files */
    char temp_dir[512];          /* temp dir for known_hosts etc. (empty = system default) */
    char pid_file[512];          /* PID file path (Linux daemon only) */

    /* ── Remote execution (SSH) ── */
    char ssh_user[64];           /* remote login, e.g. "deploy" */
    char ssh_key[512];           /* path to private key, no passphrase */
    char ssh_remote_work_dir[512]; /* base dir on remote, e.g. /tmp/orch */
} Config;

extern Config g_config;

int  config_load(const char *path);
void config_defaults(void);

#endif /* CONFIG_H */
