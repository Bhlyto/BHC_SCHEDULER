#ifndef CONFIG_H
#define CONFIG_H

/*
 * config.h
 * Runtime configuration loaded from orchestrator.conf.
 */

typedef struct {
    int  listen_port;           /* HTTP port (default 8080)          */
    char work_dir[512];         /* Base dir for job I/O staging      */
    char db_path[512];          /* Path to SQLite database file      */
    char provisioning_json[512];/* Path to provisioning.json         */
    char log_level[16];         /* "debug" | "info" | "warn" | "error" */
    int  cleanup_ttl_seconds;   /* Seconds after FINISHED to delete job dirs */
    int  scheduler_poll_ms;     /* Scheduler loop interval (ms)      */
} Config;

/* Global config instance — populated by config_load(). */
extern Config g_config;

/* Parse key=value config file. Returns 0 ok, -1 on error. */
int config_load(const char *path);

/* Apply compile-time defaults (called before config_load). */
void config_defaults(void);

#endif /* CONFIG_H */
