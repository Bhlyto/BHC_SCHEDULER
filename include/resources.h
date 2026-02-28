#ifndef RESOURCES_H
#define RESOURCES_H

/*
 * resources.h
 * Machine registry and resource allocator.
 */

#define MAX_MACHINES 64

typedef struct {
    char id[64];
    char hostname[256];
    char ip[46];         /* IPv4 or IPv6 */
    int  enabled;

    /* Total pool */
    int  cores_total;
    int  gpu_count_total;
    int  ram_mb_total;
    int  disk_mb_total;

    /* Currently reserved */
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

/* Find a machine that can satisfy the requirements and reserve the resources.
   Writes the chosen machine_id into out_machine_id (must be >=64 bytes).
   Returns 0 on success, -1 if no machine has enough free resources. */
int  alloc_reserve(const char *job_id,
                   int req_cores, int req_gpu,
                   int req_ram_mb, int req_disk_mb,
                   char *out_machine_id);

/* Release resources previously reserved for job_id. */
int  alloc_release(const char *job_id);

/* Check (without reserving) whether any machine can satisfy requirements. */
int  alloc_can_fit(int req_cores, int req_gpu,
                   int req_ram_mb, int req_disk_mb);

#endif /* RESOURCES_H */
