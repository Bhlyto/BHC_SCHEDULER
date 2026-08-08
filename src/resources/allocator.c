#include "resources.h"
#include "db.h"
#include "job.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>

#define ALLOC_MAX_MACHINES_PER_JOB 16
#include <string.h>

#ifdef _WIN32
#  include <windows.h>
static SRWLOCK s_lock = SRWLOCK_INIT;
static void lock_acquire(void) { AcquireSRWLockExclusive(&s_lock); }
static void lock_release(void) { ReleaseSRWLockExclusive(&s_lock); }
#else
#  include <pthread.h>
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static void lock_acquire(void) { pthread_mutex_lock(&s_lock); }
static void lock_release(void) { pthread_mutex_unlock(&s_lock); }
#endif

typedef struct {
    char machine_id[64];
    int cores;
    int gpu;
    int ram_mb;
    int disk_mb;
} AllocSlot;

typedef struct {
    char job_id[JOB_ID_LEN];
    int n_slots;
    AllocSlot slots[ALLOC_MAX_MACHINES_PER_JOB];
    int active;
} AllocRecord;

static AllocRecord *s_allocs = NULL;
static size_t s_alloc_count = 0;
static size_t s_alloc_capacity = 0;

static int non_negative(int value)
{
    return value > 0 ? value : 0;
}

static int free_cores(const Machine *machine)
{
    return non_negative(machine->cores_total - machine->cores_reserved - machine->cores_min);
}

static int free_gpu(const Machine *machine)
{
    return non_negative(machine->gpu_count_total - machine->gpu_count_reserved);
}

static int free_ram(const Machine *machine)
{
    return non_negative(machine->ram_mb_total - machine->ram_mb_reserved - machine->ram_mb_min);
}

static int free_disk(const Machine *machine)
{
    return non_negative(machine->disk_mb_total - machine->disk_mb_reserved - machine->disk_mb_min);
}

static int machine_can_fit(const Machine *machine,
                           int cores, int gpu, int ram_mb, int disk_mb)
{
    if (!machine || !machine->enabled || machine->probe_status != MACHINE_ONLINE) return 0;
    return free_cores(machine) >= cores && free_gpu(machine) >= gpu &&
           free_ram(machine) >= ram_mb && free_disk(machine) >= disk_mb;
}

static AllocRecord *alloc_record_acquire(const char *job_id)
{
    for (size_t i = 0; i < s_alloc_count; i++) {
        if (s_allocs[i].active && strcmp(s_allocs[i].job_id, job_id) == 0)
            return NULL;
    }
    for (size_t i = 0; i < s_alloc_count; i++) {
        if (!s_allocs[i].active) {
            memset(&s_allocs[i], 0, sizeof(s_allocs[i]));
            strncpy(s_allocs[i].job_id, job_id, sizeof(s_allocs[i].job_id) - 1);
            s_allocs[i].active = 1;
            return &s_allocs[i];
        }
    }
    if (s_alloc_count == s_alloc_capacity) {
        size_t new_capacity = s_alloc_capacity ? s_alloc_capacity * 2 : 64;
        AllocRecord *grown = (AllocRecord *)realloc(s_allocs, new_capacity * sizeof(AllocRecord));
        if (!grown) return NULL;
        memset(grown + s_alloc_capacity, 0,
               (new_capacity - s_alloc_capacity) * sizeof(AllocRecord));
        s_allocs = grown;
        s_alloc_capacity = new_capacity;
    }
    AllocRecord *record = &s_allocs[s_alloc_count++];
    memset(record, 0, sizeof(*record));
    strncpy(record->job_id, job_id, sizeof(record->job_id) - 1);
    record->active = 1;
    return record;
}

static int apply_slot(const AllocSlot *slot, int direction)
{
    if (direction > 0) {
        return registry_reserve(slot->machine_id, slot->cores, slot->gpu,
                                slot->ram_mb, slot->disk_mb);
    }
    return registry_release(slot->machine_id, slot->cores, slot->gpu,
                            slot->ram_mb, slot->disk_mb);
}

static int persist_record(const AllocRecord *record)
{
    if (db_begin() != 0) return -1;
    for (int i = 0; i < record->n_slots; i++) {
        const AllocSlot *slot = &record->slots[i];
        if (db_insert_allocation(record->job_id, slot->machine_id,
                                 slot->cores, slot->gpu,
                                 slot->ram_mb, slot->disk_mb) != 0) {
            db_rollback();
            return -1;
        }
    }
    return db_commit();
}

static void discard_record(AllocRecord *record)
{
    if (!record) return;
    for (int i = 0; i < record->n_slots; i++) apply_slot(&record->slots[i], -1);
    memset(record, 0, sizeof(*record));
}

int alloc_can_fit(int req_cores, int req_gpu, int req_ram_mb, int req_disk_mb)
{
    int result = 0;
    lock_acquire();
    Machine *machines = NULL;
    int count = registry_snapshot(&machines);
    for (int i = 0; i < count; i++) {
        if (machine_can_fit(&machines[i], req_cores, req_gpu, req_ram_mb, req_disk_mb)) {
            result = 1;
            break;
        }
    }
    free(machines);
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
    if (!out || out_len <= 0) return;
    lock_acquire();
    Machine *machines = NULL;
    int count = registry_snapshot(&machines);
    if (count <= 0) {
        snprintf(out, (size_t)out_len, "No machines registered (check provisioning.json)");
        free(machines);
        lock_release();
        return;
    }
    int enabled = 0;
    int online = 0;
    int best_cores = 0, best_gpu = 0, best_ram = 0, best_disk = 0;
    for (int i = 0; i < count; i++) {
        Machine *machine = &machines[i];
        if (!machine->enabled) continue;
        enabled++;
        if (machine->probe_status != MACHINE_ONLINE) continue;
        online++;
        if (free_cores(machine) > best_cores) best_cores = free_cores(machine);
        if (free_gpu(machine) > best_gpu) best_gpu = free_gpu(machine);
        if (free_ram(machine) > best_ram) best_ram = free_ram(machine);
        if (free_disk(machine) > best_disk) best_disk = free_disk(machine);
    }
    if (enabled == 0)
        snprintf(out, (size_t)out_len, "All %d registered machine(s) are disabled", count);
    else if (online == 0)
        snprintf(out, (size_t)out_len, "No enabled machine is online");
    else
        snprintf(out, (size_t)out_len,
                 "No online machine fits: need cores=%d gpu=%d ram=%dMB disk=%dMB; "
                 "best free cores=%d gpu=%d ram=%dMB disk=%dMB",
                 req_cores, req_gpu, req_ram_mb, req_disk_mb,
                 best_cores, best_gpu, best_ram, best_disk);
    free(machines);
    lock_release();
}

int alloc_can_fit_multi(int req_cores, int req_gpu,
                        int req_ram_mb, int req_disk_mb)
{
    if (req_gpu > 0) return 0;
    lock_acquire();
    Machine *machines = NULL;
    int count = registry_snapshot(&machines);
    int candidates[ALLOC_MAX_MACHINES_PER_JOB];
    int candidate_count = 0;
    int available_cores = 0;
    for (int i = 0; i < count && candidate_count < ALLOC_MAX_MACHINES_PER_JOB; i++) {
        if (!machines[i].enabled || machines[i].probe_status != MACHINE_ONLINE || free_cores(&machines[i]) <= 0)
            continue;
        candidates[candidate_count++] = i;
        available_cores += free_cores(&machines[i]);
        if (available_cores >= req_cores) break;
    }
    if (available_cores < req_cores || candidate_count == 0) {
        free(machines);
        lock_release();
        return 0;
    }
    int ram_per = (req_ram_mb + candidate_count - 1) / candidate_count;
    int disk_per = (req_disk_mb + candidate_count - 1) / candidate_count;
    for (int i = 0; i < candidate_count; i++) {
        Machine *machine = &machines[candidates[i]];
        if (free_ram(machine) < ram_per || free_disk(machine) < disk_per) {
            free(machines);
            lock_release();
            return 0;
        }
    }
    free(machines);
    lock_release();
    return candidate_count;
}

int alloc_reserve_multi(const char *job_id,
                        int req_cores, int req_gpu,
                        int req_ram_mb, int req_disk_mb,
                        char *out_machine_ids,
                        int *out_n_machines)
{
    if (!job_id || !out_machine_ids || !out_n_machines || req_gpu > 0 || req_cores <= 0)
        return -1;
    out_machine_ids[0] = '\0';
    *out_n_machines = 0;
    lock_acquire();
    Machine *machines = NULL;
    int count = registry_snapshot(&machines);
    AllocRecord *record = alloc_record_acquire(job_id);
    if (!record || count < 0) {
        if (record) memset(record, 0, sizeof(*record));
        free(machines);
        lock_release();
        return -1;
    }
    int remaining = req_cores;
    for (int i = 0; i < count && record->n_slots < ALLOC_MAX_MACHINES_PER_JOB && remaining > 0; i++) {
        Machine *machine = &machines[i];
        int available = free_cores(machine);
        if (!machine->enabled || machine->probe_status != MACHINE_ONLINE || available <= 0) continue;
        AllocSlot *slot = &record->slots[record->n_slots++];
        strncpy(slot->machine_id, machine->id, sizeof(slot->machine_id) - 1);
        slot->cores = available < remaining ? available : remaining;
        remaining -= slot->cores;
    }
    if (remaining > 0 || record->n_slots == 0) {
        memset(record, 0, sizeof(*record));
        free(machines);
        lock_release();
        return -1;
    }
    int ram_per = (req_ram_mb + record->n_slots - 1) / record->n_slots;
    int disk_per = (req_disk_mb + record->n_slots - 1) / record->n_slots;
    for (int i = 0; i < record->n_slots; i++) {
        Machine *machine = NULL;
        for (int j = 0; j < count; j++) {
            if (strcmp(machines[j].id, record->slots[i].machine_id) == 0) {
                machine = &machines[j];
                break;
            }
        }
        if (!machine || free_ram(machine) < ram_per || free_disk(machine) < disk_per) {
            memset(record, 0, sizeof(*record));
            free(machines);
            lock_release();
            return -1;
        }
        record->slots[i].ram_mb = ram_per;
        record->slots[i].disk_mb = disk_per;
    }
    free(machines);
    int applied = 0;
    for (int i = 0; i < record->n_slots; i++) {
        if (apply_slot(&record->slots[i], 1) != 0) {
            for (int j = 0; j < applied; j++) apply_slot(&record->slots[j], -1);
            memset(record, 0, sizeof(*record));
            lock_release();
            return -1;
        }
        applied++;
    }
    if (persist_record(record) != 0) {
        discard_record(record);
        lock_release();
        return -1;
    }
    size_t used = 0;
    for (int i = 0; i < record->n_slots; i++) {
        int written = snprintf(out_machine_ids + used, 1024 - used, "%s%s",
                               i ? "," : "", record->slots[i].machine_id);
        if (written < 0 || (size_t)written >= 1024 - used) {
            discard_record(record);
            db_release_allocation(job_id);
            out_machine_ids[0] = '\0';
            lock_release();
            return -1;
        }
        used += (size_t)written;
    }
    *out_n_machines = record->n_slots;
    lock_release();
    log_info("allocator", "Reserved %d machines [%s] for job %s",
             *out_n_machines, out_machine_ids, job_id);
    return 0;
}

static int reserve_single(const char *job_id, Machine *machine,
                          int req_cores, int req_gpu,
                          int req_ram_mb, int req_disk_mb)
{
    AllocRecord *record = alloc_record_acquire(job_id);
    if (!record) return -1;
    record->n_slots = 1;
    AllocSlot *slot = &record->slots[0];
    strncpy(slot->machine_id, machine->id, sizeof(slot->machine_id) - 1);
    slot->cores = req_cores;
    slot->gpu = req_gpu;
    slot->ram_mb = req_ram_mb;
    slot->disk_mb = req_disk_mb;
    if (apply_slot(slot, 1) != 0) {
        memset(record, 0, sizeof(*record));
        return -1;
    }
    if (persist_record(record) != 0) {
        discard_record(record);
        return -1;
    }
    return 0;
}

int alloc_reserve(const char *job_id,
                  int req_cores, int req_gpu,
                  int req_ram_mb, int req_disk_mb,
                  char *out_machine_id)
{
    if (!job_id || !out_machine_id || req_cores <= 0) return -1;
    lock_acquire();
    Machine *machines = NULL;
    int count = registry_snapshot(&machines);
    Machine *chosen = NULL;
    for (int i = 0; i < count; i++) {
        if (machine_can_fit(&machines[i], req_cores, req_gpu, req_ram_mb, req_disk_mb)) {
            chosen = &machines[i];
            break;
        }
    }
    if (!chosen || reserve_single(job_id, chosen, req_cores, req_gpu, req_ram_mb, req_disk_mb) != 0) {
        free(machines);
        lock_release();
        return -1;
    }
    strncpy(out_machine_id, chosen->id, 63);
    out_machine_id[63] = '\0';
    free(machines);
    lock_release();
    log_info("allocator", "Reserved on %s for job %s", out_machine_id, job_id);
    return 0;
}

int alloc_reserve_on(const char *job_id,
                     const char *target_machine_id,
                     int req_cores, int req_gpu,
                     int req_ram_mb, int req_disk_mb)
{
    if (!job_id || !target_machine_id || req_cores <= 0) return -1;
    lock_acquire();
    Machine machine;
    int result = -1;
    if (registry_get_copy(target_machine_id, &machine) == 0 &&
        machine_can_fit(&machine, req_cores, req_gpu, req_ram_mb, req_disk_mb))
        result = reserve_single(job_id, &machine, req_cores, req_gpu, req_ram_mb, req_disk_mb);
    lock_release();
    if (result == 0)
        log_info("allocator", "Reserved on %s (pinned) for job %s", target_machine_id, job_id);
    return result;
}

int alloc_release(const char *job_id)
{
    if (!job_id) return -1;
    int released_slots = 0;
    lock_acquire();
    for (size_t i = 0; i < s_alloc_count; i++) {
        AllocRecord *record = &s_allocs[i];
        if (!record->active || strcmp(record->job_id, job_id) != 0) continue;
        released_slots = record->n_slots;
        discard_record(record);
        break;
    }
    lock_release();
    int db_result = db_release_allocation(job_id);
    if (released_slots > 0) {
        if (db_result != 0) log_warn("allocator", "Failed to persist release for job %s", job_id);
        log_info("allocator", "Released resources for job %s (%d machine(s))", job_id, released_slots);
        return 0;
    }
    return db_result == 0 ? 0 : -1;
}

void alloc_shutdown(void)
{
    lock_acquire();
    free(s_allocs);
    s_allocs = NULL;
    s_alloc_count = 0;
    s_alloc_capacity = 0;
    lock_release();
}
