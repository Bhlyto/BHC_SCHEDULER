#include "config.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <errno.h>
#include <limits.h>

Config g_config;

void config_defaults(void)
{
    memset(&g_config, 0, sizeof(g_config));
    g_config.listen_port          = 8080;
    g_config.cleanup_ttl_seconds  = 3600;
    g_config.scheduler_poll_ms    = 500;
    g_config.job_timeout_seconds  = 0;
    strncpy(g_config.ssh_remote_work_dir, "/tmp/orch", sizeof(g_config.ssh_remote_work_dir)-1);
    strncpy(g_config.command_mode,       "app_only",  sizeof(g_config.command_mode)-1);
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
    strncpy(g_config.presim_domains, "thermal", sizeof(g_config.presim_domains)-1);
    g_config.presim_threshold_max = 0.03;
    g_config.presim_refine_multiplier = 0.8;
    g_config.presim_high_multiplier = 2.0;
    g_config.presim_uncertainty_weight = 1.0;
    strncpy(g_config.presim_fidelity_map, "0,1,3,6", sizeof(g_config.presim_fidelity_map)-1);
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

static int safe_token(const char *value, const char *extra)
{
    if (!value || !value[0] || value[0] == '-') return 0;
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || strchr(extra, *p))) return 0;
    }
    return 1;
}

static int validation_error(char *error, size_t error_len, const char *message)
{
    if (error && error_len > 0) snprintf(error, error_len, "%s", message);
    return -1;
}

static int parse_int_value(const char *text, int *out)
{
    char *end = NULL;
    errno = 0;
    long value = strtol(text, &end, 10);
    if (errno == ERANGE || end == text || *end != '\0' ||
        value < INT_MIN || value > INT_MAX) return -1;
    *out = (int)value;
    return 0;
}

static int parse_double_value(const char *text, double *out)
{
    char *end = NULL;
    errno = 0;
    double value = strtod(text, &end);
    if (errno == ERANGE || end == text || *end != '\0' || !isfinite(value)) return -1;
    *out = value;
    return 0;
}

int config_validate(char *error, size_t error_len)
{
    if (error && error_len > 0) error[0] = '\0';
    if (g_config.listen_port < 1 || g_config.listen_port > 65535)
        return validation_error(error, error_len, "listen_port must be between 1 and 65535");
    if (!g_config.work_dir[0] || !g_config.db_path[0] ||
        !g_config.provisioning_json[0] || !g_config.apps_dir[0])
        return validation_error(error, error_len, "work_dir, db_path, provisioning_json and apps_dir are required");
    if (strcmp(g_config.log_level, "debug") != 0 && strcmp(g_config.log_level, "info") != 0 &&
        strcmp(g_config.log_level, "warn") != 0 && strcmp(g_config.log_level, "error") != 0)
        return validation_error(error, error_len, "log_level must be debug, info, warn or error");
    if (g_config.cleanup_ttl_seconds < 0 || g_config.scheduler_poll_ms < 10 ||
        g_config.scheduler_poll_ms > 3600000 || g_config.job_timeout_seconds < 0)
        return validation_error(error, error_len, "scheduler and timeout values are outside supported ranges");
    if (strcmp(g_config.command_mode, "app_only") != 0 && strcmp(g_config.command_mode, "free") != 0)
        return validation_error(error, error_len, "command_mode must be app_only or free");
    if ((g_config.require_https != 0 && g_config.require_https != 1) ||
        (g_config.web_ui_enabled != 0 && g_config.web_ui_enabled != 1) ||
        (g_config.cloud_auto_provision != 0 && g_config.cloud_auto_provision != 1) ||
        (g_config.cloud_auto_deprovision != 0 && g_config.cloud_auto_deprovision != 1))
        return validation_error(error, error_len, "boolean configuration values must be 0 or 1");
    if (!safe_token(g_config.listen_address, ".:") ||
        strchr(g_config.cors_allowed_origin, '\r') || strchr(g_config.cors_allowed_origin, '\n'))
        return validation_error(error, error_len, "listen_address or CORS origin is invalid");
    if (g_config.cors_allowed_origin[0] &&
        strncmp(g_config.cors_allowed_origin, "http://", 7) != 0 &&
        strncmp(g_config.cors_allowed_origin, "https://", 8) != 0)
        return validation_error(error, error_len, "cors_allowed_origin must be an absolute HTTP(S) origin");
    if (strcmp(g_config.probe_method, "tcp") != 0 && strcmp(g_config.probe_method, "ping") != 0 &&
        strcmp(g_config.probe_method, "ssh") != 0)
        return validation_error(error, error_len, "probe_method must be tcp, ping or ssh");
    if (g_config.probe_port < 1 || g_config.probe_port > 65535 ||
        g_config.probe_timeout_ms < 100 || g_config.probe_timeout_ms > 600000 ||
        g_config.probe_retries < 0 || g_config.probe_retries > 10 ||
        g_config.probe_interval_ms < 100 || g_config.probe_interval_ms > 86400000)
        return validation_error(error, error_len, "probe settings are outside supported ranges");
    if (g_config.ssh_user[0] && !safe_token(g_config.ssh_user, "._-"))
        return validation_error(error, error_len, "ssh_user contains unsupported characters");
    if (!safe_token(g_config.ssh_remote_work_dir, "/._-") ||
        (g_config.ssh_shell[0] && !safe_token(g_config.ssh_shell, "/._-")))
        return validation_error(error, error_len, "SSH remote path or shell contains unsupported characters");
    if (!isfinite(g_config.presim_threshold_max) || g_config.presim_threshold_max <= 0.0 ||
        !isfinite(g_config.presim_refine_multiplier) || g_config.presim_refine_multiplier <= 0.0 ||
        !isfinite(g_config.presim_high_multiplier) || g_config.presim_high_multiplier <= 0.0 ||
        !isfinite(g_config.presim_uncertainty_weight) || g_config.presim_uncertainty_weight < 0.0)
        return validation_error(error, error_len, "presimulation numeric settings must be finite and positive");
    if (g_config.cloud_auto_provision) {
        int provider_valid = strcmp(g_config.cloud_default_provider, "aws") == 0 ||
                             strcmp(g_config.cloud_default_provider, "gcp") == 0 ||
                             strcmp(g_config.cloud_default_provider, "azure") == 0;
        if (!provider_valid || !safe_token(g_config.cloud_default_instance_type, "-_.") ||
            !safe_token(g_config.cloud_default_region, "-_./:") ||
            (strcmp(g_config.cloud_default_provider, "aws") == 0 &&
             !safe_token(g_config.cloud_default_image_id, "-_./:")))
            return validation_error(error, error_len, "cloud auto-provision defaults are incomplete or invalid");
    }
    return 0;
}

int config_load(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "[config] Cannot open %s, using defaults\n", path);
        return 0; /* non-fatal */
    }

    int parse_failed = 0;
    int line_number = 0;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        line_number++;
        if (!strchr(line, '\n') && !feof(f)) {
            int character;
            while ((character = fgetc(f)) != '\n' && character != EOF) { }
            fprintf(stderr, "[config] Line %d is too long\n", line_number);
            parse_failed = 1;
            continue;
        }
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
        int line_failed = 0;
        int line_error_reported = 0;

#define LOAD_INT(field) do { if (parse_int_value(val, &(field)) != 0) line_failed = 1; } while (0)
#define LOAD_DOUBLE(field) do { if (parse_double_value(val, &(field)) != 0) line_failed = 1; } while (0)
#define LOAD_STRING(field) do { \
    if (strlen(val) >= sizeof(field)) line_failed = 1; \
    else memcpy((field), val, strlen(val) + 1); \
} while (0)

        if      (strcmp(key, "listen_port") == 0) LOAD_INT(g_config.listen_port);
        else if (strcmp(key, "work_dir") == 0) LOAD_STRING(g_config.work_dir);
        else if (strcmp(key, "db_path") == 0) LOAD_STRING(g_config.db_path);
        else if (strcmp(key, "provisioning_json") == 0) LOAD_STRING(g_config.provisioning_json);
        else if (strcmp(key, "log_level") == 0) LOAD_STRING(g_config.log_level);
        else if (strcmp(key, "cleanup_ttl_seconds") == 0) LOAD_INT(g_config.cleanup_ttl_seconds);
        else if (strcmp(key, "scheduler_poll_ms") == 0) LOAD_INT(g_config.scheduler_poll_ms);
        else if (strcmp(key, "pre_job_script_win") == 0) LOAD_STRING(g_config.pre_job_script_win);
        else if (strcmp(key, "pre_job_script_linux") == 0) LOAD_STRING(g_config.pre_job_script_linux);
        else if (strcmp(key, "job_timeout_seconds") == 0) LOAD_INT(g_config.job_timeout_seconds);
        else if (strcmp(key, "ssh_user") == 0) LOAD_STRING(g_config.ssh_user);
        else if (strcmp(key, "ssh_key") == 0) LOAD_STRING(g_config.ssh_key);
        else if (strcmp(key, "ssh_remote_work_dir") == 0) LOAD_STRING(g_config.ssh_remote_work_dir);
        else if (strcmp(key, "apps_dir") == 0) LOAD_STRING(g_config.apps_dir);
        else if (strcmp(key, "temp_dir") == 0) LOAD_STRING(g_config.temp_dir);
        else if (strcmp(key, "pid_file") == 0) LOAD_STRING(g_config.pid_file);
        else if (strcmp(key, "ssh_shell") == 0) LOAD_STRING(g_config.ssh_shell);
        else if (strcmp(key, "command_mode") == 0) LOAD_STRING(g_config.command_mode);
        else if (strcmp(key, "cors_allowed_origin") == 0) LOAD_STRING(g_config.cors_allowed_origin);
        else if (strcmp(key, "require_https") == 0) LOAD_INT(g_config.require_https);
        else if (strcmp(key, "web_ui_enabled") == 0) LOAD_INT(g_config.web_ui_enabled);
        else if (strcmp(key, "listen_address") == 0) LOAD_STRING(g_config.listen_address);
        else if (strcmp(key, "probe_method") == 0) LOAD_STRING(g_config.probe_method);
        else if (strcmp(key, "probe_port") == 0) LOAD_INT(g_config.probe_port);
        else if (strcmp(key, "probe_timeout_ms") == 0) LOAD_INT(g_config.probe_timeout_ms);
        else if (strcmp(key, "probe_retries") == 0) LOAD_INT(g_config.probe_retries);
        else if (strcmp(key, "probe_interval_ms") == 0) LOAD_INT(g_config.probe_interval_ms);
        else if (strcmp(key, "cloud_credentials_file") == 0) LOAD_STRING(g_config.cloud_credentials_file);
        else if (strcmp(key, "cloud_auto_provision") == 0) LOAD_INT(g_config.cloud_auto_provision);
        else if (strcmp(key, "cloud_auto_deprovision") == 0) LOAD_INT(g_config.cloud_auto_deprovision);
        else if (strcmp(key, "cloud_default_provider") == 0) LOAD_STRING(g_config.cloud_default_provider);
        else if (strcmp(key, "cloud_default_instance_type") == 0) LOAD_STRING(g_config.cloud_default_instance_type);
        else if (strcmp(key, "cloud_default_region") == 0) LOAD_STRING(g_config.cloud_default_region);
        else if (strcmp(key, "cloud_default_image_id") == 0) LOAD_STRING(g_config.cloud_default_image_id);
        else if (strcmp(key, "presim_threshold_max") == 0) LOAD_DOUBLE(g_config.presim_threshold_max);
        else if (strcmp(key, "presim_refine_multiplier") == 0) LOAD_DOUBLE(g_config.presim_refine_multiplier);
        else if (strcmp(key, "presim_high_multiplier") == 0) LOAD_DOUBLE(g_config.presim_high_multiplier);
        else if (strcmp(key, "presim_uncertainty_weight") == 0) LOAD_DOUBLE(g_config.presim_uncertainty_weight);
        else if (strcmp(key, "presim_fidelity_map") == 0) LOAD_STRING(g_config.presim_fidelity_map);
        else if (strcmp(key, "presim_domains") == 0) LOAD_STRING(g_config.presim_domains);
        else {
            fprintf(stderr, "[config] Unknown key on line %d: %s\n", line_number, key);
            line_failed = 1;
            line_error_reported = 1;
        }
        if (line_failed && !line_error_reported)
            fprintf(stderr, "[config] Invalid value on line %d\n", line_number);
        if (line_failed) parse_failed = 1;
#undef LOAD_INT
#undef LOAD_DOUBLE
#undef LOAD_STRING
    }
    fclose(f);
    return parse_failed ? -1 : 0;
}
