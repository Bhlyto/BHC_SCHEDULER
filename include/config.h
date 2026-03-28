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
    char ssh_shell[64];          /* remote shell, e.g. "/bin/rbash" (empty = default) */

    /* ── Security ── */
    char command_mode[16];       /* "free" (default) or "app_only" */

    /* ── Web UI / Bastion mode ── */
    int  web_ui_enabled;         /* 1 = serve web UI (default), 0 = API-only bastion */
    char listen_address[64];     /* bind address, e.g. "0.0.0.0" or "127.0.0.1" */

    /* ── Machine probe / availability ── */
    char probe_method[16];       /* "tcp" (default), "ping", "ssh" */
    int  probe_port;             /* target port for tcp/ssh (default 22) */
    int  probe_timeout_ms;       /* ms before marking unreachable (default 3000) */
    int  probe_retries;          /* retries before OFFLINE (default 2) */
    int  probe_interval_ms;      /* background re-probe interval (default 60000) */

    /* ── Cloud ── */
    char cloud_credentials_file[512]; /* path to cloud credentials JSON */

    /* ── Cloud auto-scaling ── */
    int  cloud_auto_provision;        /* 1 = auto-provision when no machine fits */
    int  cloud_auto_deprovision;      /* 1 = auto-deprovision idle cloud machines */
    char cloud_default_provider[32];  /* default provider for auto-prov */
    char cloud_default_instance_type[64];
    char cloud_default_region[64];
    char cloud_default_image_id[128];
} Config;

extern Config g_config;

int  config_load(const char *path);
void config_defaults(void);

#endif /* CONFIG_H */
