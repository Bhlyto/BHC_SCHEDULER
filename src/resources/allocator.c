#include "resources.h"
#include "db.h"
#include "log.h"
#include <string.h>
#include <stdlib.h>

#ifdef _WIN32
#  include <windows.h>
   static CRITICAL_SECTION s_lock;
   static int s_lock_init = 0;
   static void lock_init(void)   { if (!s_lock_init) { InitializeCriticalSection(&s_lock); s_lock_init=1; } }
   static void lock_acquire(void){ lock_init(); EnterCriticalSection(&s_lock); }
   static void lock_release(void){ LeaveCriticalSection(&s_lock); }
#else
#  include <pthread.h>
#include <stdio.h>
   static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
   static void lock_acquire(void){ pthread_mutex_lock(&s_lock); }
   static void lock_release(void){ pthread_mutex_unlock(&s_lock); }
#endif

typedef struct {
    char machine_id[64];
    int  cores; int gpu; int ram_mb; int disk_mb;
} AllocSlot;

typedef struct {
    char      job_id[37];
    AllocSlot slot;
    int       active;
} AllocRecord;

#define MAX_ALLOC 1024
static AllocRecord s_allocs[MAX_ALLOC];
static int         s_alloc_count = 0;

/* Called with s_lock held. Reuse released records before growing the high
   water mark so a long-running scheduler is not limited to 1024 total jobs. */
static AllocRecord *alloc_record_acquire(void)
{
    for (int i = 0; i < s_alloc_count; i++)
        if (!s_allocs[i].active) return &s_allocs[i];
    if (s_alloc_count >= MAX_ALLOC) return NULL;
    return &s_allocs[s_alloc_count++];
}

static int free_cores(const Machine *m)
{
    return m->cores_total - m->cores_reserved - m->cores_min;
}

static int free_ram_mb(const Machine *m)
{
    return m->ram_mb_total - m->ram_mb_reserved - m->ram_mb_min;
}

static int free_disk_mb(const Machine *m)
{
    return m->disk_mb_total - m->disk_mb_reserved - m->disk_mb_min;
}

static int machine_can_fit(const Machine *M,
                            int cores, int gpu, int ram_mb, int disk_mb)
{
    if (!M->enabled) return 0;
    if (M->probe_status == MACHINE_OFFLINE) return 0;
    if (free_cores(M)                                  < cores)   return 0;
    if (M->gpu_count_total - M->gpu_count_reserved < gpu)     return 0;
    if (free_ram_mb(M)                                  < ram_mb)  return 0;
    if (free_disk_mb(M)                                 < disk_mb) return 0;
    return 1;
}

int alloc_can_fit(int req_cores, int req_gpu, int req_ram_mb, int req_disk_mb)
{
    int result = 0;
    lock_acquire();
    int count;
    Machine *ms = registry_all(&count);
    for (int i = 0; i < count; i++)
        if (machine_can_fit(&ms[i], req_cores, req_gpu, req_ram_mb, req_disk_mb)) {
            result = 1;
            break;
        }
    lock_release();
    return result;
}

/*
 * Fill `out` with a human-readable explanation of why no machine can satisfy
 * the requirements. Helps diagnose jobs stuck QUEUED.
 */
void alloc_diagnose(int req_cores, int req_gpu, int req_ram_mb, int req_disk_mb,
                    char *out, int out_len)
{
    int count;
    Machine *ms = registry_all(&count);

    if (count == 0) {
        snprintf(out, out_len, "No machines registered (check provisioning.json)");
        return;
    }

    int n_enabled = 0, n_cores = 0, n_gpu = 0, n_ram = 0, n_disk = 0;
    int best_free_cores = 0, best_free_ram = 0, best_free_disk = 0;

    for (int i = 0; i < count; i++) {
        Machine *M = &ms[i];
        if (!M->enabled) continue;
        n_enabled++;
        int fc = free_cores(M);
        int fg = M->gpu_count_total - M->gpu_count_reserved;
        int fr = free_ram_mb(M);
        int fd = free_disk_mb(M);
        if (fc > best_free_cores) best_free_cores = fc;
        if (fr > best_free_ram)   best_free_ram   = fr;
        if (fd > best_free_disk)  best_free_disk  = fd;
        if (fc >= req_cores)  n_cores++;
        if (fg >= req_gpu)    n_gpu++;
        if (fr >= req_ram_mb) n_ram++;
        if (fd >= req_disk_mb)n_disk++;
    }

    if (n_enabled == 0) {
        snprintf(out, out_len,
            "All %d registered machine(s) are disabled", count);
        return;
    }

    /* Build a constraint failure description */
    char parts[4][80]; int np = 0;
    if (n_cores == 0)
        snprintf(parts[np++], 80, "cores: need %d, best available %d",
                 req_cores, best_free_cores);
    if (req_gpu > 0 && n_gpu == 0)
        snprintf(parts[np++], 80, "GPU: need %d, none available", req_gpu);
    if (n_ram == 0)
        snprintf(parts[np++], 80, "RAM: need %d MB, best available %d MB",
                 req_ram_mb, best_free_ram);
    if (n_disk == 0)
        snprintf(parts[np++], 80, "disk: need %d MB, best available %d MB",
                 req_disk_mb, best_free_disk);

    if (np == 0) {
        /* Individual constraints pass but combined don't (race between reserve checks) */
        snprintf(out, out_len, "Resources temporarily unavailable (contention)");
        return;
    }

    int pos = snprintf(out, out_len, "No machine satisfies: ");
    for (int i = 0; i < np && pos < out_len - 2; i++) {
        if (i > 0) pos += snprintf(out + pos, out_len - pos, "; ");
        pos += snprintf(out + pos, out_len - pos, "%s", parts[i]);
    }
}

int alloc_reserve(const char *job_id,
                  int req_cores, int req_gpu,
                  int req_ram_mb, int req_disk_mb,
                  char *out_machine_id)
{
    lock_acquire();

    int count;
    Machine *ms = registry_all(&count);
    Machine *chosen = NULL;
    for (int i = 0; i < count; i++) {
        if (machine_can_fit(&ms[i], req_cores, req_gpu, req_ram_mb, req_disk_mb)) {
            chosen = &ms[i];
            break;
        }
    }
    if (!chosen) { lock_release(); return -1; }

    AllocRecord *r = alloc_record_acquire();
    if (!r) { lock_release(); return -1; }
    memset(r, 0, sizeof(*r));
    strncpy(r->job_id, job_id, sizeof(r->job_id)-1);
    strncpy(r->slot.machine_id, chosen->id, sizeof(r->slot.machine_id)-1);
    r->slot.cores   = req_cores;
    r->slot.gpu     = req_gpu;
    r->slot.ram_mb  = req_ram_mb;
    r->slot.disk_mb = req_disk_mb;
    r->active = 1;

    chosen->cores_reserved     += req_cores;
    chosen->gpu_count_reserved += req_gpu;
    chosen->ram_mb_reserved    += req_ram_mb;
    chosen->disk_mb_reserved   += req_disk_mb;

    strncpy(out_machine_id, chosen->id, 63);
    out_machine_id[63] = '\0';

    lock_release();

    db_insert_allocation(job_id, chosen->id,
                         req_cores, req_gpu, req_ram_mb, req_disk_mb);
    log_info("allocator", "Reserved on %s for job %s (cores=%d gpu=%d ram=%dMB disk=%dMB)",
             chosen->id, job_id, req_cores, req_gpu, req_ram_mb, req_disk_mb);
    return 0;
}

/* Reserve on a specific machine (for same-machine affinity). */
int alloc_reserve_on(const char *job_id,
                     const char *target_machine_id,
                     int req_cores, int req_gpu,
                     int req_ram_mb, int req_disk_mb)
{
    lock_acquire();
    Machine *m = registry_get(target_machine_id);
    if (!m || !machine_can_fit(m, req_cores, req_gpu, req_ram_mb, req_disk_mb)) {
        lock_release();
        return -1;
    }

    AllocRecord *r = alloc_record_acquire();
    if (!r) { lock_release(); return -1; }
    memset(r, 0, sizeof(*r));
    strncpy(r->job_id, job_id, sizeof(r->job_id)-1);
    strncpy(r->slot.machine_id, m->id, sizeof(r->slot.machine_id)-1);
    r->slot.cores   = req_cores;
    r->slot.gpu     = req_gpu;
    r->slot.ram_mb  = req_ram_mb;
    r->slot.disk_mb = req_disk_mb;
    r->active = 1;

    m->cores_reserved     += req_cores;
    m->gpu_count_reserved += req_gpu;
    m->ram_mb_reserved    += req_ram_mb;
    m->disk_mb_reserved   += req_disk_mb;

    lock_release();

    db_insert_allocation(job_id, m->id,
                         req_cores, req_gpu, req_ram_mb, req_disk_mb);
    log_info("allocator", "Reserved on %s (pinned) for job %s (cores=%d gpu=%d ram=%dMB disk=%dMB)",
             m->id, job_id, req_cores, req_gpu, req_ram_mb, req_disk_mb);
    return 0;
}

int alloc_release(const char *job_id)
{
    lock_acquire();
    for (int i = 0; i < s_alloc_count; i++) {
        AllocRecord *r = &s_allocs[i];
        if (!r->active || strcmp(r->job_id, job_id) != 0) continue;
        Machine *M = registry_get(r->slot.machine_id);
        if (M) {
            M->cores_reserved     -= r->slot.cores;
            M->gpu_count_reserved -= r->slot.gpu;
            M->ram_mb_reserved    -= r->slot.ram_mb;
            M->disk_mb_reserved   -= r->slot.disk_mb;
            if (M->cores_reserved < 0) M->cores_reserved = 0;
            if (M->gpu_count_reserved < 0) M->gpu_count_reserved = 0;
            if (M->ram_mb_reserved < 0) M->ram_mb_reserved = 0;
            if (M->disk_mb_reserved < 0) M->disk_mb_reserved = 0;
        }
        r->active = 0;
        lock_release();
        db_release_allocation(job_id);
        log_info("allocator", "Released resources for job %s on %s",
                 job_id, r->slot.machine_id);
        return 0;
    }
    lock_release();
    return -1;
}
