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
    int       n_slots;
    AllocSlot slots[ALLOC_MAX_MACHINES_PER_JOB];
    int       active;
} AllocRecord;

#define MAX_ALLOC 1024
static AllocRecord s_allocs[MAX_ALLOC];
static int         s_alloc_count = 0;

static int machine_can_fit(const Machine *M,
                            int cores, int gpu, int ram_mb, int disk_mb)
{
    if (!M->enabled) return 0;
    if (M->cores_total     - M->cores_reserved     < cores)   return 0;
    if (M->gpu_count_total - M->gpu_count_reserved < gpu)     return 0;
    if (M->ram_mb_total    - M->ram_mb_reserved    < ram_mb)  return 0;
    if (M->disk_mb_total   - M->disk_mb_reserved   < disk_mb) return 0;
    return 1;
}

int alloc_can_fit(int req_cores, int req_gpu, int req_ram_mb, int req_disk_mb)
{
    int count;
    Machine *ms = registry_all(&count);
    for (int i = 0; i < count; i++)
        if (machine_can_fit(&ms[i], req_cores, req_gpu, req_ram_mb, req_disk_mb))
            return 1;
    return 0;
}

/*
 * Fill `out` with a human-readable explanation of why no machine can satisfy
 * the requirements. Helps diagnose jobs stuck IN_QUEUE.
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
        int fc = M->cores_total     - M->cores_reserved;
        int fg = M->gpu_count_total - M->gpu_count_reserved;
        int fr = M->ram_mb_total    - M->ram_mb_reserved;
        int fd = M->disk_mb_total   - M->disk_mb_reserved;
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

int alloc_can_fit_multi(int req_cores, int req_gpu,
                        int req_ram_mb, int req_disk_mb)
{
    if (req_gpu > 0) return 0; /* GPU jobs cannot span multiple machines */

    int count;
    Machine *ms = registry_all(&count);

    /* Pass 1: collect candidate machines greedily until cores covered */
    int total_free_cores = 0;
    int n_cands = 0;
    int cand_idx[ALLOC_MAX_MACHINES_PER_JOB];
    for (int i = 0; i < count && n_cands < ALLOC_MAX_MACHINES_PER_JOB; i++) {
        if (!ms[i].enabled) continue;
        int fc = ms[i].cores_total - ms[i].cores_reserved;
        if (fc <= 0) continue;
        cand_idx[n_cands++] = i;
        total_free_cores += fc;
        if (total_free_cores >= req_cores) break;
    }
    if (total_free_cores < req_cores) return 0;

    /* Pass 2: check RAM/disk split across selected machines */
    int ram_per  = (req_ram_mb  + n_cands - 1) / n_cands;
    int disk_per = (req_disk_mb + n_cands - 1) / n_cands;
    for (int i = 0; i < n_cands; i++) {
        Machine *M = &ms[cand_idx[i]];
        if ((M->ram_mb_total  - M->ram_mb_reserved)  < ram_per)  return 0;
        if ((M->disk_mb_total - M->disk_mb_reserved) < disk_per) return 0;
    }
    return n_cands;
}

int alloc_reserve_multi(const char *job_id,
                        int req_cores, int req_gpu,
                        int req_ram_mb, int req_disk_mb,
                        char *out_machine_ids,
                        int  *out_n_machines)
{
    if (req_gpu > 0) return -1;

    lock_acquire();

    int count;
    Machine *ms = registry_all(&count);

    /* Greedy: pick machines with free cores until req_cores covered */
    typedef struct { int idx; int cores_take; } TmpSlot;
    TmpSlot tmp[ALLOC_MAX_MACHINES_PER_JOB];
    int n_slots = 0;
    int remaining = req_cores;

    for (int i = 0; i < count && n_slots < ALLOC_MAX_MACHINES_PER_JOB; i++) {
        if (remaining <= 0) break;
        Machine *M = &ms[i];
        if (!M->enabled) continue;
        int fc = M->cores_total - M->cores_reserved;
        if (fc <= 0) continue;
        tmp[n_slots].idx        = i;
        tmp[n_slots].cores_take = (fc < remaining) ? fc : remaining;
        remaining -= tmp[n_slots].cores_take;
        n_slots++;
    }
    if (remaining > 0) { lock_release(); return -1; }

    int ram_per  = (req_ram_mb  + n_slots - 1) / n_slots;
    int disk_per = (req_disk_mb + n_slots - 1) / n_slots;
    for (int i = 0; i < n_slots; i++) {
        Machine *M = &ms[tmp[i].idx];
        if ((M->ram_mb_total  - M->ram_mb_reserved)  < ram_per ||
            (M->disk_mb_total - M->disk_mb_reserved) < disk_per) {
            lock_release(); return -1;
        }
    }

    out_machine_ids[0] = '\0';
    AllocRecord *r = NULL;
    if (s_alloc_count < MAX_ALLOC) {
        r = &s_allocs[s_alloc_count++];
        memset(r, 0, sizeof(*r));
        strncpy(r->job_id, job_id, sizeof(r->job_id)-1);
        r->n_slots = n_slots;
        r->active  = 1;
    }

    for (int i = 0; i < n_slots; i++) {
        Machine *M = &ms[tmp[i].idx];
        M->cores_reserved    += tmp[i].cores_take;
        M->ram_mb_reserved   += ram_per;
        M->disk_mb_reserved  += disk_per;
        if (i > 0) strncat(out_machine_ids, ",", 1023);
        strncat(out_machine_ids, M->id, 1023);
        if (r) {
            strncpy(r->slots[i].machine_id, M->id, sizeof(r->slots[i].machine_id)-1);
            r->slots[i].cores   = tmp[i].cores_take;
            r->slots[i].gpu     = 0;
            r->slots[i].ram_mb  = ram_per;
            r->slots[i].disk_mb = disk_per;
        }
    }
    *out_n_machines = n_slots;

    lock_release();

    for (int i = 0; i < n_slots; i++) {
        char mid[64];
        strncpy(mid, r->slots[i].machine_id, sizeof(mid)-1);
        mid[sizeof(mid)-1] = '\0';
        db_insert_allocation(job_id, mid,
                             r->slots[i].cores, r->slots[i].gpu,
                             r->slots[i].ram_mb, r->slots[i].disk_mb);
    }

    log_info("allocator",
             "Multi-machine reserved %d machine(s) [%s] for job %s "
             "(cores=%d ram=%dMB disk=%dMB)",
             n_slots, out_machine_ids, job_id,
             req_cores, req_ram_mb, req_disk_mb);
    return 0;
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

    chosen->cores_reserved     += req_cores;
    chosen->gpu_count_reserved += req_gpu;
    chosen->ram_mb_reserved    += req_ram_mb;
    chosen->disk_mb_reserved   += req_disk_mb;

    strncpy(out_machine_id, chosen->id, 63);
    out_machine_id[63] = '\0';

    if (s_alloc_count < MAX_ALLOC) {
        AllocRecord *r = &s_allocs[s_alloc_count++];
        memset(r, 0, sizeof(*r));
        strncpy(r->job_id, job_id, sizeof(r->job_id)-1);
        r->n_slots = 1;
        strncpy(r->slots[0].machine_id, chosen->id, sizeof(r->slots[0].machine_id)-1);
        r->slots[0].cores   = req_cores;
        r->slots[0].gpu     = req_gpu;
        r->slots[0].ram_mb  = req_ram_mb;
        r->slots[0].disk_mb = req_disk_mb;
        r->active = 1;
    }

    lock_release();

    db_insert_allocation(job_id, chosen->id,
                         req_cores, req_gpu, req_ram_mb, req_disk_mb);
    log_info("allocator", "Reserved on %s for job %s (cores=%d gpu=%d ram=%dMB disk=%dMB)",
             chosen->id, job_id, req_cores, req_gpu, req_ram_mb, req_disk_mb);
    return 0;
}

int alloc_release(const char *job_id)
{
    lock_acquire();
    for (int i = 0; i < s_alloc_count; i++) {
        AllocRecord *r = &s_allocs[i];
        if (!r->active || strcmp(r->job_id, job_id) != 0) continue;
        for (int s = 0; s < r->n_slots; s++) {
            Machine *M = registry_get(r->slots[s].machine_id);
            if (M) {
                M->cores_reserved     -= r->slots[s].cores;
                M->gpu_count_reserved -= r->slots[s].gpu;
                M->ram_mb_reserved    -= r->slots[s].ram_mb;
                M->disk_mb_reserved   -= r->slots[s].disk_mb;
            }
        }
        r->active = 0;
        lock_release();
        db_release_allocation(job_id);
        log_info("allocator", "Released resources for job %s (%d machine(s))",
                 job_id, r->n_slots);
        return 0;
    }
    lock_release();
    return -1;
}
