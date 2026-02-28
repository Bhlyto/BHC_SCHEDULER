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

/* Per-allocation tracking so we can release resources when a job ends. */
typedef struct {
    char job_id[37];
    char machine_id[64];
    int  cores; int gpu; int ram_mb; int disk_mb;
    int  active;
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

    /* Track in memory */
    if (s_alloc_count < MAX_ALLOC) {
        AllocRecord *r = &s_allocs[s_alloc_count++];
        strncpy(r->job_id,     job_id,     sizeof(r->job_id)-1);
        strncpy(r->machine_id, chosen->id, sizeof(r->machine_id)-1);
        r->cores  = req_cores;
        r->gpu    = req_gpu;
        r->ram_mb = req_ram_mb;
        r->disk_mb= req_disk_mb;
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
        if (r->active && strcmp(r->job_id, job_id) == 0) {
            Machine *M = registry_get(r->machine_id);
            if (M) {
                M->cores_reserved     -= r->cores;
                M->gpu_count_reserved -= r->gpu;
                M->ram_mb_reserved    -= r->ram_mb;
                M->disk_mb_reserved   -= r->disk_mb;
            }
            r->active = 0;
            lock_release();
            db_release_allocation(job_id);
            log_info("allocator", "Released resources for job %s", job_id);
            return 0;
        }
    }
    lock_release();
    return -1;
}
