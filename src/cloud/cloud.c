#include "resources.h"
#include "config.h"
#include "log.h"
#include "cJSON.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

/*
 * cloud.c
 * Cloud provider interface — provisions and deprovisions machines
 * using provider CLIs (aws, gcloud, az). The credentials file path
 * is read from g_config.cloud_credentials_file.
 *
 * This delegates to the installed CLI tools rather than embedding
 * SDK libraries, keeping the binary small and portable.
 */

#ifdef _WIN32
#  include <windows.h>
#  include <process.h>
#else
#  include <unistd.h>
#  include <sys/wait.h>
#endif

/* ── Helpers ──────────────────────────────────────────────────────── */

/* Run a CLI command and capture stdout into buf. Returns exit code. */
static int run_command(const char *cmd, char *buf, int buf_len)
{
    if (!cmd || !buf || buf_len <= 0) return -1;
    buf[0] = '\0';

    FILE *fp = NULL;
#ifdef _WIN32
    fp = _popen(cmd, "r");
#else
    fp = popen(cmd, "r");
#endif
    if (!fp) {
        log_error("cloud", "Failed to run: %s", cmd);
        return -1;
    }

    int total = 0;
    while (total < buf_len - 1) {
        int ch = fgetc(fp);
        if (ch == EOF) break;
        buf[total++] = (char)ch;
    }
    buf[total] = '\0';

#ifdef _WIN32
    int rc = _pclose(fp);
#else
    int rc = pclose(fp);
    if (WIFEXITED(rc)) rc = WEXITSTATUS(rc);
#endif
    return rc;
}

/* Validate that a string contains only safe characters for CLI args */
static int is_safe_arg(const char *s)
{
    if (!s) return 1;
    if (s[0] == '-') return 0;
    for (const char *p = s; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' ||
              *p == '.' || *p == '/' || *p == ':' || *p == ',' ||
              *p == '='))
            return 0;
    }
    return 1;
}

/* ── AWS EC2 ──────────────────────────────────────────────────────── */

static int aws_provision(const CloudMachineSpec *spec, char *out_id, int id_len)
{
    if (!is_safe_arg(spec->instance_type) || !is_safe_arg(spec->region) ||
        !is_safe_arg(spec->image_id)) {
        log_error("cloud", "Invalid characters in AWS spec fields");
        return -1;
    }

    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
        "aws ec2 run-instances"
        " --instance-type %s"
        " --region %s"
        " --image-id %s"
        " --count 1"
        " --output json"
        " --query \"Instances[0].InstanceId\"",
        spec->instance_type[0] ? spec->instance_type : "t3.medium",
        spec->region[0] ? spec->region : "us-east-1",
        spec->image_id);

    char result[2048] = {0};
    int rc = run_command(cmd, result, sizeof(result));
    if (rc != 0) {
        log_error("cloud", "AWS provision failed (rc=%d): %s", rc, result);
        return -1;
    }

    /* Parse instance ID from JSON output (quoted string) */
    char *start = strchr(result, '"');
    if (start) {
        start++;
        char *end = strchr(start, '"');
        if (end) {
            int len = (int)(end - start);
            if (len >= id_len) len = id_len - 1;
            strncpy(out_id, start, len);
            out_id[len] = '\0';
        }
    }

    if (!out_id[0]) {
        log_error("cloud", "Could not parse instance ID from: %s", result);
        return -1;
    }

    /* Register the new machine in the registry */
    Machine m;
    memset(&m, 0, sizeof(m));
    snprintf(m.id, sizeof(m.id), "cloud-%s", out_id);
    strncpy(m.cloud_instance_id, out_id, sizeof(m.cloud_instance_id)-1);
    strncpy(m.cloud_provider, "aws", sizeof(m.cloud_provider)-1);
    m.type = MACHINE_TYPE_CLOUD;
    m.enabled = 1;
    m.cores_total = spec->cores > 0 ? spec->cores : 2;
    m.ram_mb_total = spec->ram_mb > 0 ? spec->ram_mb : 4096;
    m.disk_mb_total = spec->disk_mb > 0 ? spec->disk_mb : 50000;
    m.gpu_count_total = spec->gpu_count;
    m.cores_min = spec->cores_min;
    m.ram_mb_min = spec->ram_mb_min;
    m.disk_mb_min = spec->disk_mb_min;
    m.probe_status = MACHINE_PROBING;
    if (registry_upsert(&m) != 0) {
        char cleanup_cmd[512];
        snprintf(cleanup_cmd, sizeof(cleanup_cmd),
            "aws ec2 terminate-instances --instance-ids %s --output json", out_id);
        run_command(cleanup_cmd, result, sizeof(result));
        log_error("cloud", "Registry rejected AWS instance %s; termination requested", out_id);
        return -1;
    }

    /* Copy the machine registry id to out */
    strncpy(out_id, m.id, id_len - 1);

    log_info("cloud", "AWS instance provisioned: %s (%s)",
             m.id, m.cloud_instance_id);
    return 0;
}

static int aws_deprovision(const char *instance_id)
{
    if (!is_safe_arg(instance_id)) return -1;

    char cmd[512];
    snprintf(cmd, sizeof(cmd),
        "aws ec2 terminate-instances --instance-ids %s --output json",
        instance_id);

    char result[2048] = {0};
    int rc = run_command(cmd, result, sizeof(result));
    if (rc != 0) {
        log_error("cloud", "AWS deprovision failed (rc=%d): %s", rc, result);
        return -1;
    }

    /* Remove from registry */
    char id[128];
    snprintf(id, sizeof(id), "cloud-%s", instance_id);
    registry_remove(id);

    log_info("cloud", "AWS instance terminated: %s", instance_id);
    return 0;
}

static int aws_status(const char *instance_id, char *out_status, int status_len)
{
    if (!is_safe_arg(instance_id)) return -1;

    char cmd[512];
    snprintf(cmd, sizeof(cmd),
        "aws ec2 describe-instance-status --instance-ids %s"
        " --query \"InstanceStatuses[0].InstanceState.Name\""
        " --output text",
        instance_id);

    char result[256] = {0};
    int rc = run_command(cmd, result, sizeof(result));
    if (rc != 0) {
        strncpy(out_status, "unknown", status_len - 1);
        return -1;
    }

    /* Trim whitespace */
    int len = (int)strlen(result);
    while (len > 0 && (result[len-1] == '\n' || result[len-1] == '\r' || result[len-1] == ' '))
        result[--len] = '\0';

    strncpy(out_status, result, status_len - 1);
    return 0;
}

/* ── GCP ──────────────────────────────────────────────────────────── */

static int gcp_provision(const CloudMachineSpec *spec, char *out_id, int id_len)
{
    if (!is_safe_arg(spec->instance_type) || !is_safe_arg(spec->region) ||
        !is_safe_arg(spec->image_id)) return -1;

    /* Generate unique instance name */
    char name[64];
    snprintf(name, sizeof(name), "orch-%ld", (long)time(NULL));

    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
        "gcloud compute instances create %s"
        " --machine-type=%s"
        " --zone=%s"
        " --image=%s"
        " --format=json"
        " --quiet",
        name,
        spec->instance_type[0] ? spec->instance_type : "e2-medium",
        spec->region[0] ? spec->region : "us-central1-a",
        spec->image_id[0] ? spec->image_id : "debian-11");

    char result[4096] = {0};
    int rc = run_command(cmd, result, sizeof(result));
    if (rc != 0) {
        log_error("cloud", "GCP provision failed (rc=%d)", rc);
        return -1;
    }

    Machine m;
    memset(&m, 0, sizeof(m));
    snprintf(m.id, sizeof(m.id), "cloud-%s", name);
    strncpy(m.cloud_instance_id, name, sizeof(m.cloud_instance_id)-1);
    strncpy(m.cloud_provider, "gcp", sizeof(m.cloud_provider)-1);
    m.type = MACHINE_TYPE_CLOUD;
    m.enabled = 1;
    m.cores_total = spec->cores > 0 ? spec->cores : 2;
    m.ram_mb_total = spec->ram_mb > 0 ? spec->ram_mb : 4096;
    m.disk_mb_total = spec->disk_mb > 0 ? spec->disk_mb : 50000;
    m.gpu_count_total = spec->gpu_count;
    m.cores_min = spec->cores_min;
    m.ram_mb_min = spec->ram_mb_min;
    m.disk_mb_min = spec->disk_mb_min;
    m.probe_status = MACHINE_PROBING;
    if (registry_upsert(&m) != 0) {
        char cleanup_cmd[512];
        snprintf(cleanup_cmd, sizeof(cleanup_cmd),
            "gcloud compute instances delete %s --quiet", name);
        run_command(cleanup_cmd, result, sizeof(result));
        log_error("cloud", "Registry rejected GCP instance %s; deletion requested", name);
        return -1;
    }

    strncpy(out_id, m.id, id_len - 1);
    log_info("cloud", "GCP instance provisioned: %s", m.id);
    return 0;
}

static int gcp_deprovision(const char *instance_id)
{
    if (!is_safe_arg(instance_id)) return -1;

    char cmd[512];
    snprintf(cmd, sizeof(cmd),
        "gcloud compute instances delete %s --quiet", instance_id);

    char result[2048] = {0};
    int rc = run_command(cmd, result, sizeof(result));

    if (rc == 0) {
        char id[128];
        snprintf(id, sizeof(id), "cloud-%s", instance_id);
        registry_remove(id);
    }

    log_info("cloud", "GCP instance deleted: %s (rc=%d)", instance_id, rc);
    return (rc == 0) ? 0 : -1;
}

/* ── Azure ────────────────────────────────────────────────────────── */

static int azure_provision(const CloudMachineSpec *spec, char *out_id, int id_len)
{
    if (!is_safe_arg(spec->instance_type) || !is_safe_arg(spec->region) ||
        !is_safe_arg(spec->image_id)) return -1;

    char name[64];
    snprintf(name, sizeof(name), "orch-%ld", (long)time(NULL));

    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
        "az vm create --name %s"
        " --size %s"
        " --location %s"
        " --image %s"
        " --resource-group orchestrator"
        " --output json --no-wait",
        name,
        spec->instance_type[0] ? spec->instance_type : "Standard_B2s",
        spec->region[0] ? spec->region : "eastus",
        spec->image_id[0] ? spec->image_id : "UbuntuLTS");

    char result[4096] = {0};
    int rc = run_command(cmd, result, sizeof(result));
    if (rc != 0) {
        log_error("cloud", "Azure provision failed (rc=%d)", rc);
        return -1;
    }

    Machine m;
    memset(&m, 0, sizeof(m));
    snprintf(m.id, sizeof(m.id), "cloud-%s", name);
    strncpy(m.cloud_instance_id, name, sizeof(m.cloud_instance_id)-1);
    strncpy(m.cloud_provider, "azure", sizeof(m.cloud_provider)-1);
    m.type = MACHINE_TYPE_CLOUD;
    m.enabled = 1;
    m.cores_total = spec->cores > 0 ? spec->cores : 2;
    m.ram_mb_total = spec->ram_mb > 0 ? spec->ram_mb : 4096;
    m.disk_mb_total = spec->disk_mb > 0 ? spec->disk_mb : 50000;
    m.gpu_count_total = spec->gpu_count;
    m.cores_min = spec->cores_min;
    m.ram_mb_min = spec->ram_mb_min;
    m.disk_mb_min = spec->disk_mb_min;
    m.probe_status = MACHINE_PROBING;
    if (registry_upsert(&m) != 0) {
        char cleanup_cmd[512];
        snprintf(cleanup_cmd, sizeof(cleanup_cmd),
            "az vm delete --name %s --resource-group orchestrator --yes --no-wait", name);
        run_command(cleanup_cmd, result, sizeof(result));
        log_error("cloud", "Registry rejected Azure VM %s; deletion requested", name);
        return -1;
    }

    strncpy(out_id, m.id, id_len - 1);
    log_info("cloud", "Azure VM provisioned: %s", m.id);
    return 0;
}

static int azure_deprovision(const char *instance_id)
{
    if (!is_safe_arg(instance_id)) return -1;

    char cmd[512];
    snprintf(cmd, sizeof(cmd),
        "az vm delete --name %s --resource-group orchestrator --yes --no-wait",
        instance_id);

    char result[2048] = {0};
    int rc = run_command(cmd, result, sizeof(result));

    if (rc == 0) {
        char id[128];
        snprintf(id, sizeof(id), "cloud-%s", instance_id);
        registry_remove(id);
    }

    log_info("cloud", "Azure VM deleted: %s (rc=%d)", instance_id, rc);
    return (rc == 0) ? 0 : -1;
}

/* ── Public API ───────────────────────────────────────────────────── */

int cloud_provision(const CloudMachineSpec *spec, char *out_machine_id, int id_len)
{
    if (!spec || !out_machine_id || id_len <= 0) return -1;
    out_machine_id[0] = '\0';

    if (strcmp(spec->provider, "aws") == 0)
        return aws_provision(spec, out_machine_id, id_len);
    if (strcmp(spec->provider, "gcp") == 0)
        return gcp_provision(spec, out_machine_id, id_len);
    if (strcmp(spec->provider, "azure") == 0)
        return azure_provision(spec, out_machine_id, id_len);

    log_error("cloud", "Unknown provider: %s", spec->provider);
    return -1;
}

int cloud_deprovision(const char *provider, const char *instance_id)
{
    if (!provider || !instance_id) return -1;

    if (strcmp(provider, "aws") == 0)
        return aws_deprovision(instance_id);
    if (strcmp(provider, "gcp") == 0)
        return gcp_deprovision(instance_id);
    if (strcmp(provider, "azure") == 0)
        return azure_deprovision(instance_id);

    log_error("cloud", "Unknown provider: %s", provider);
    return -1;
}

int cloud_status(const char *provider, const char *instance_id,
                 char *out_status, int status_len)
{
    if (!provider || !instance_id || !out_status || status_len <= 0) return -1;
    out_status[0] = '\0';

    if (strcmp(provider, "aws") == 0)
        return aws_status(instance_id, out_status, status_len);

    /* GCP and Azure status check — simplified version */
    strncpy(out_status, "unknown", status_len - 1);
    return 0;
}
