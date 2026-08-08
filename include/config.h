#ifndef CONFIG_H
#define CONFIG_H

#include <stddef.h>

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
    char command_mode[16];       /* "app_only" (default) or "free" */
    char cors_allowed_origin[256]; /* exact allowed origin; empty disables CORS */
    int  require_https;          /* require X-Forwarded-Proto=https */

    /* ── Web UI / Bastion mode ── */
    int  web_ui_enabled;         /* 1 = serve legacy UI, 0 = API-only v1 default */
    int  experimental_features_enabled; /* workflows/cloud/WoL/presim, off in v1 */
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
    char presim_domains[256];   /* comma-separated domains that trigger presim automatically */
    double presim_threshold_max; /* conservative upper bound for presim threshold */
    double presim_refine_multiplier; /* multiplier for refine cutoff (priority > threshold * mult) */
    double presim_high_multiplier;   /* multiplier for high-fidelity cutoff */
    double presim_uncertainty_weight; /* weight for zone uncertainty in priority */
    char   presim_fidelity_map[64]; /* CSV map for fidelity -> cores, index 0 unused e.g. "0,1,3,6" */
} Config;

extern Config g_config;

int  config_load(const char *path);
void config_defaults(void);
int  config_validate(char *error, size_t error_len);

#endif /* CONFIG_H */
