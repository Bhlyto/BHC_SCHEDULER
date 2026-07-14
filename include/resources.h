#ifndef RESOURCES_H
#define RESOURCES_H

#include <time.h>

#define MAX_MACHINES 1024

typedef enum {
    MACHINE_ONLINE  = 0,
    MACHINE_OFFLINE = 1,
    MACHINE_PROBING = 2
} MachineStatus;

typedef enum {
    MACHINE_TYPE_STATIC = 0,
    MACHINE_TYPE_CLOUD  = 1
} MachineType;

typedef struct {
    char id[64];
    char hostname[256];
    char ip[46];
    int  enabled;

    int  cores_total;
    int  gpu_count_total;
    int  ram_mb_total;
    int  disk_mb_total;

    int  cores_reserved;
    int  gpu_count_reserved;
    int  ram_mb_reserved;
    int  disk_mb_reserved;

    /* ── Availability / probe ── */
    MachineStatus probe_status;     /* ONLINE, OFFLINE, PROBING */
    time_t        last_probe_time;
    int           probe_fail_count;

    /* ── Type & cloud ── */
    MachineType   type;             /* STATIC or CLOUD */
    char          cloud_provider[32]; /* "aws", "gcp", "azure" or "" */
    char          cloud_instance_id[128];

    /* ── Flexible resource limits ── */
    int  cores_min;   /* minimum cores to keep reserved for system (0=none) */
    int  ram_mb_min;  /* minimum RAM to keep free */
    int  disk_mb_min; /* minimum disk to keep free */

    /* ── Wake-on-LAN ── */
    char mac_address[18]; /* "AA:BB:CC:DD:EE:FF" or "" */
} Machine;

/* ── Registry ────────────────────────────────── */

/* Load machines from provisioning.json. Returns number loaded, -1 on error. */
int  registry_load(const char *json_path);

/* Copy a machine by id into out. Returns 0 if found, -1 otherwise. */
int registry_get_copy(const char *machine_id, Machine *out);

/* Allocate a consistent machine snapshot. Caller frees *out_machines.
   Returns the machine count, or -1 on allocation failure. */
int registry_snapshot(Machine **out_machines);

/* Add or update a machine entry at runtime. */
int  registry_upsert(const Machine *m);

/* Remove a machine by id. Returns 0 ok, -1 not found, -2 while reserved. */
int  registry_remove(const char *machine_id);

/* Atomically reserve or release machine resources. */
int registry_reserve(const char *machine_id, int cores, int gpu,
                     int ram_mb, int disk_mb);
int registry_release(const char *machine_id, int cores, int gpu,
                     int ram_mb, int disk_mb);

/* Atomically update availability information from a probe result. */
int registry_update_probe(const char *machine_id, MachineStatus status,
                          time_t probe_time, int reachable);

/* ── Allocator ───────────────────────────────── */

#define ALLOC_MAX_MACHINES_PER_JOB 16

/* Find a single machine that can satisfy the requirements and reserve.
   Writes the chosen machine_id into out_machine_id (must be >=64 bytes).
   Returns 0 on success, -1 if no single machine has enough free resources. */
int  alloc_reserve(const char *job_id,
                   int req_cores, int req_gpu,
                   int req_ram_mb, int req_disk_mb,
                   char *out_machine_id);

/* Reserve on a specific machine (for same-machine affinity).
   Returns 0 on success, -1 if the machine doesn't have enough resources. */
int  alloc_reserve_on(const char *job_id,
                      const char *target_machine_id,
                      int req_cores, int req_gpu,
                      int req_ram_mb, int req_disk_mb);

/* Spread req_cores across multiple machines when no single machine fits.
   out_machine_ids : comma-separated list, buffer must be >= 1024 bytes.
   out_n_machines  : number of machines selected.
   RAM/disk split equally across selected machines.
   GPU jobs (req_gpu > 0) cannot use multi-machine mode.
   Returns 0 on success, -1 if even combined resources are insufficient. */
int  alloc_reserve_multi(const char *job_id,
                         int req_cores, int req_gpu,
                         int req_ram_mb, int req_disk_mb,
                         char *out_machine_ids,
                         int  *out_n_machines);

/* Release resources previously reserved for job_id (single or multi). */
int  alloc_release(const char *job_id);
void alloc_shutdown(void);

/* Check (without reserving) whether any single machine can satisfy requirements. */
int  alloc_can_fit(int req_cores, int req_gpu,
                   int req_ram_mb, int req_disk_mb);

/* Explain why no machine can satisfy the requirements (for diagnostics). */
void alloc_diagnose(int req_cores, int req_gpu,
                    int req_ram_mb, int req_disk_mb,
                    char *out, int out_len);

/* Check whether combined free cores across all machines can satisfy req_cores.
   Returns estimated number of machines that would be needed, 0 if impossible. */
int  alloc_can_fit_multi(int req_cores, int req_gpu,
                         int req_ram_mb, int req_disk_mb);

/* ── Machine Probe / Availability ────────────── */
typedef enum { PROBE_PING, PROBE_TCP, PROBE_SSH } ProbeMethod;

/* Check if a machine is reachable. Returns 1 if reachable, 0 if not. */
int  machine_is_reachable(const char *host, ProbeMethod method,
                          int port, int timeout_ms);

/* Start background probe thread that periodically checks all static machines. */
void probe_start_background(int interval_ms, ProbeMethod method,
                            int port, int timeout_ms, int retries);
void probe_stop_background(void);

/* Refresh probe status of all machines (called from background thread). */
void probe_refresh_all(ProbeMethod method, int port, int timeout_ms, int retries);

/* ── Wake-on-LAN ─────────────────────────────── */

/* Send a WoL magic packet to the given MAC address.
   broadcast_ip can be "255.255.255.255" or a subnet broadcast.
   Returns 0 on success, -1 on error. */
int  wol_send(const char *mac_address, const char *broadcast_ip);

/* ── Cloud Provider Interface ────────────────── */

typedef struct {
    char provider[32];       /* "aws", "gcp", "azure" */
    char instance_type[64];  /* e.g. "t3.xlarge" */
    char region[64];
    char image_id[128];      /* AMI, image family, etc. */
    int  cores;
    int  gpu_count;
    int  ram_mb;
    int  disk_mb;
    char tags[512];          /* JSON string of key-value tags */
    /* Flexible resource override (cloud) */
    int  cores_min;
    int  ram_mb_min;
    int  disk_mb_min;
} CloudMachineSpec;

int  cloud_provision(const CloudMachineSpec *spec, char *out_machine_id, int id_len);
int  cloud_deprovision(const char *provider, const char *instance_id);
int  cloud_status(const char *provider, const char *instance_id,
                  char *out_status, int status_len);

#endif /* RESOURCES_H */
