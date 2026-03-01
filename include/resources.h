#ifndef RESOURCES_H
#define RESOURCES_H

#define MAX_MACHINES 1024

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
} Machine;

/* ── Registry ────────────────────────────────── */

/* Load machines from provisioning.json. Returns number loaded, -1 on error. */
int  registry_load(const char *json_path);

/* Find a machine by id. Returns pointer into internal array (do not free). */
Machine *registry_get(const char *machine_id);

/* Returns array of all known machines and sets *count. */
Machine *registry_all(int *count);

/* Add or update a machine entry at runtime. */
int  registry_upsert(const Machine *m);

/* Remove a machine by id. Returns 0 ok, -1 not found. */
int  registry_remove(const char *machine_id);

/* ── Allocator ───────────────────────────────── */

#define ALLOC_MAX_MACHINES_PER_JOB 16

/* Find a single machine that can satisfy the requirements and reserve.
   Writes the chosen machine_id into out_machine_id (must be >=64 bytes).
   Returns 0 on success, -1 if no single machine has enough free resources. */
int  alloc_reserve(const char *job_id,
                   int req_cores, int req_gpu,
                   int req_ram_mb, int req_disk_mb,
                   char *out_machine_id);

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

#endif /* RESOURCES_H */
