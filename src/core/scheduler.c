#include "queue.h"
#include "job.h"
#include "resources.h"
#include "config.h"
#include "log.h"
#include "db.h"
#include "events.h"
#include "transfer.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdio.h>

#ifdef _WIN32
#  include <windows.h>
#  define sleep_ms(ms) Sleep(ms)
#else
#  include <unistd.h>
#  define sleep_ms(ms) usleep((ms) * 1000)
#endif

int executor_spawn(Job *job);

static Queue *s_queue    = NULL;
static int    s_running  = 0;

/* ── Auto-provision rate limiter: track job IDs that already triggered ──── */
#define AUTO_PROV_MAX 256
static char  s_auto_prov_jobs[AUTO_PROV_MAX][37]; /* UUID = 36 chars + NUL */
static int   s_auto_prov_count = 0;

static int auto_prov_already_tried(const char *job_id)
{
    for (int i = 0; i < s_auto_prov_count; i++)
        if (strcmp(s_auto_prov_jobs[i], job_id) == 0) return 1;
    return 0;
}

static void auto_prov_mark(const char *job_id)
{
    if (s_auto_prov_count < AUTO_PROV_MAX) {
        strncpy(s_auto_prov_jobs[s_auto_prov_count], job_id, 36);
        s_auto_prov_jobs[s_auto_prov_count][36] = '\0';
        s_auto_prov_count++;
    }
}

/* ── Count expected files from comma-separated input_files string ────────── */
static int count_expected_files(const char *input_files)
{
    if (!input_files || !input_files[0]) return 0;
    int count = 1;
    for (const char *p = input_files; *p; p++)
        if (*p == ',') count++;
    return count;
}

/* ── Count files actually present in a directory that match expected names ── */
static int count_present_files(const char *input_dir, const char *input_files)
{
    char files_copy[2048];
    strncpy(files_copy, input_files, sizeof(files_copy) - 1);
    files_copy[sizeof(files_copy) - 1] = '\0';

    int present = 0;
    char *tok = strtok(files_copy, ",");
    while (tok) {
        while (*tok == ' ') tok++;
        /* strip trailing spaces */
        int tlen = (int)strlen(tok);
        while (tlen > 0 && tok[tlen-1] == ' ') tok[--tlen] = '\0';

        char path[768];
#ifdef _WIN32
        snprintf(path, sizeof(path), "%s\\%s", input_dir, tok);
#else
        snprintf(path, sizeof(path), "%s/%s", input_dir, tok);
#endif
        FILE *f = fopen(path, "rb");
        if (f) { fclose(f); present++; }
        tok = strtok(NULL, ",");
    }
    return present;
}

/* ── Periodic: check all HELD jobs and release those whose files are ready ── */
static void scheduler_check_held_jobs(void)
{
    Job *jobs = (Job *)malloc(256 * sizeof(Job));
    if (!jobs) return;

    int count = db_list_held_jobs(jobs, 256);
    for (int i = 0; i < count; i++) {
        Job *j = &jobs[i];

        /* ── Check dependency status first ──────────────────────── */
        if (j->depends_on[0]) {
            int dep_status = db_check_deps_status(j->depends_on);
            if (dep_status == -1) {
                /* A dependency failed or was cancelled — cascade failure */
                log_warn("scheduler",
                    "Job %s dependency failed/cancelled — marking FAILED", j->id);
                time_t now = time(NULL);
                db_update_job_status(j->id, JOB_STATUS_FAILED, -1, now);
                db_update_status_reason(j->id,
                    "Dependency failed or was cancelled");
                continue;
            }
            if (dep_status == 1) {
                /* Still waiting for deps — skip file check too */
                continue;
            }
            /* dep_status == 0: all deps finished — forward outputs then clear
               depends_on so we don't re-check every tick */
            log_info("scheduler",
                "All dependencies satisfied for job %s — forwarding outputs", j->id);

            /* Copy output files from each parent into this job's input dir */
            {
                char depbuf[2048];
                strncpy(depbuf, j->depends_on, sizeof(depbuf) - 1);
                depbuf[sizeof(depbuf) - 1] = '\0';
                store_init_job_dirs(j->id);
                char *tok = strtok(depbuf, ",");
                while (tok) {
                    while (*tok == ' ') tok++;
                    int tlen = (int)strlen(tok);
                    while (tlen > 0 && tok[tlen-1] == ' ') tok[--tlen] = '\0';
                    if (tok[0])
                        store_forward_outputs(tok, j->id);
                    tok = strtok(NULL, ",");
                }
            }

            j->depends_on[0] = '\0';
            db_update_depends_on(j->id, "");
        }

        /* ── Check input files ──────────────────────────────────── */
        if (j->input_files[0]) {
            int expected = count_expected_files(j->input_files);
            char input_dir[512];
            store_input_dir(j->id, input_dir, sizeof(input_dir));
            int present = count_present_files(input_dir, j->input_files);

            log_debug("scheduler",
                "HELD job %s: expecting %d files, %d present",
                j->id, expected, present);

            if (present < expected)
                continue; /* still waiting for files */

            log_info("scheduler",
                "All %d input file(s) ready for job %s — releasing to queue",
                expected, j->id);
        }

        /* All conditions met (deps done + files present) — release */
        Job *full = db_get_job(j->id);
        if (full) {
            char reason[128];
            if (j->input_files[0])
                snprintf(reason, sizeof(reason),
                    "Dependencies satisfied, all input files received");
            else
                snprintf(reason, sizeof(reason),
                    "All dependencies satisfied");
            job_set_status_r(full, JOB_STATUS_IN_QUEUE, reason);
            queue_push(s_queue, full);
        }
    }
    free(jobs);
}

/* ── Periodic: kill jobs that have exceeded their timeout ───────────────── */
static void scheduler_check_timeouts(void)
{
    if (g_config.job_timeout_seconds <= 0) return;

    Job *jobs = (Job *)malloc(256 * sizeof(Job));
    if (!jobs) return;

    int count = db_list_running_jobs(jobs, 256);
    time_t now = time(NULL);

    for (int i = 0; i < count; i++) {
        Job *j = &jobs[i];
        /* Per-job timeout wins if set, otherwise use global config */
        int limit = (j->timeout_seconds > 0)
                     ? j->timeout_seconds
                     : g_config.job_timeout_seconds;
        if (limit <= 0) continue;
        if (j->started_at <= 0) continue;

        double elapsed = difftime(now, j->started_at);
        if (elapsed >= (double)limit) {
            log_warn("scheduler",
                "Job %s timed out after %.0fs (limit=%ds) — marking FAILED",
                j->id, elapsed, limit);
            char reason[128];
            snprintf(reason, sizeof(reason),
                "Timed out after %.0f seconds (limit=%d s)", elapsed, limit);
            db_update_job_status(j->id, JOB_STATUS_FAILED, -1, now);
            db_update_status_reason(j->id, reason);
            db_release_allocation(j->id);
            alloc_release(j->id);
            /* Auto-deprovision cloud machine if now idle */
            if (g_config.cloud_auto_deprovision && j->machine_id[0]) {
                Machine *tm = registry_get(j->machine_id);
                if (tm && tm->type == MACHINE_TYPE_CLOUD &&
                    tm->cores_reserved <= 0 && tm->ram_mb_reserved <= 0) {
                    log_info("scheduler",
                        "Auto-deprovisioning idle cloud machine %s after timeout",
                        tm->id);
                    cloud_deprovision(tm->cloud_provider, tm->cloud_instance_id);
                }
            }
        }
    }
    free(jobs);
}


#ifdef _WIN32
static DWORD WINAPI scheduler_thread(LPVOID arg)
#else
#include <pthread.h>
static void *scheduler_thread(void *arg)
#endif
{
    (void)arg;
    log_info("scheduler", "Scheduler started (poll=%dms)", g_config.scheduler_poll_ms);

    int tick = 0;

    while (s_running) {
        sleep_ms(g_config.scheduler_poll_ms);

        /* Every 10 ticks: check HELD jobs + enforce timeouts */
        if (++tick % 10 == 0) {
            scheduler_check_held_jobs();
            scheduler_check_timeouts();
        }

        Job *job = queue_try_pop(s_queue);
        if (!job) continue;

        /* ── Quota enforcement ─────────────────────────────────── */
        {
            char quota_reason[256] = {0};
            if (db_quota_check(job->user_id, job->app_id,
                               job->req_cores, job->req_ram_mb,
                               quota_reason, sizeof(quota_reason)) != 0) {
                if (tick % 20 == 1) {
                    log_warn("scheduler", "Job %s blocked by quota: %s",
                             job->id, quota_reason);
                }
                db_update_status_reason(job->id, quota_reason);
                queue_push(s_queue, job);
                continue;
            }
        }

        /* ── Same-machine affinity ──────────────────────────── */
        if (job->same_machine_as[0]) {
            Job *ref = db_get_job(job->same_machine_as);
            if (ref && ref->machine_id[0]) {
                if (alloc_reserve_on(job->id, ref->machine_id,
                                     job->req_cores, job->req_gpu,
                                     job->req_ram_mb, job->req_disk_mb) == 0) {
                    strncpy(job->machine_id, ref->machine_id, sizeof(job->machine_id) - 1);
                    job->n_machines = 1;
                    log_info("scheduler", "Dispatching job %s to %s (same-machine affinity with %s)",
                             job->id, ref->machine_id, ref->id);
                    free(ref);

                    /* Record dispatch event */
                    {
                        char detail[256];
                        snprintf(detail, sizeof(detail), "Dispatched to %s (same-machine as %s)",
                                 job->machine_id, job->same_machine_as);
                        events_push_persistent("job", "dispatch", detail, job->user_id);
                        db_insert_event("job", "dispatch", detail,
                                        job->user_id, job->id, job->machine_id);
                    }
                    executor_spawn(job);
                    continue;
                } else {
                    /* Target machine lacks capacity — re-queue */
                    if (tick % 20 == 1) {
                        log_warn("scheduler",
                            "Job %s waiting: same-machine %s has insufficient resources",
                            job->id, ref->machine_id);
                    }
                    db_update_status_reason(job->id,
                        "Waiting for resources on pinned machine (same-machine affinity)");
                    free(ref);
                    queue_push(s_queue, job);
                    continue;
                }
            } else if (ref && !ref->machine_id[0]) {
                /* Referenced job hasn't started yet — re-queue */
                free(ref);
                db_update_status_reason(job->id,
                    "Waiting for affinity target job to be assigned a machine");
                queue_push(s_queue, job);
                continue;
            }
            if (ref) free(ref);
            /* If ref job not found, fall through to normal allocation */
        }

        int can_single = alloc_can_fit(job->req_cores, job->req_gpu,
                                        job->req_ram_mb, job->req_disk_mb);
        int can_multi  = !can_single
                       ? alloc_can_fit_multi(job->req_cores, job->req_gpu,
                                             job->req_ram_mb, job->req_disk_mb)
                       : 0;

        if (!can_single && !can_multi) {
            /* ── Auto-provision a cloud machine if enabled ──────── */
            if (g_config.cloud_auto_provision &&
                g_config.cloud_default_provider[0] &&
                !auto_prov_already_tried(job->id)) {
                auto_prov_mark(job->id);
                log_info("scheduler",
                    "No machine fits job %s — auto-provisioning cloud instance",
                    job->id);
                CloudMachineSpec spec;
                memset(&spec, 0, sizeof(spec));
                strncpy(spec.provider, g_config.cloud_default_provider,
                        sizeof(spec.provider) - 1);
                strncpy(spec.instance_type, g_config.cloud_default_instance_type,
                        sizeof(spec.instance_type) - 1);
                strncpy(spec.region, g_config.cloud_default_region,
                        sizeof(spec.region) - 1);
                strncpy(spec.image_id, g_config.cloud_default_image_id,
                        sizeof(spec.image_id) - 1);
                /* Size the VM to at least satisfy the job's requirements */
                spec.cores    = job->req_cores;
                spec.gpu_count = job->req_gpu;
                spec.ram_mb   = job->req_ram_mb;
                spec.disk_mb  = job->req_disk_mb;

                char new_mid[128] = {0};
                if (cloud_provision(&spec, new_mid, sizeof(new_mid)) == 0) {
                    char detail[256];
                    snprintf(detail, sizeof(detail),
                        "Auto-provisioned %s for job %s",
                        new_mid, job->id);
                    events_push_persistent("cloud", "auto_provision",
                                           detail, job->user_id);
                    db_insert_event("cloud", "auto_provision",
                                    detail, job->user_id, job->id, new_mid);
                    log_info("scheduler", "%s", detail);
                } else {
                    log_warn("scheduler",
                        "Auto-provision failed for job %s", job->id);
                }
                /* Re-queue regardless — machine may take a moment to register */
                queue_push(s_queue, job);
                continue;
            }

            /* Diagnose and surface the reason so the user can understand the block */
            char diag[256];
            alloc_diagnose(job->req_cores, job->req_gpu,
                           job->req_ram_mb, job->req_disk_mb,
                           diag, sizeof(diag));
            /* Only log once every 20 ticks (~10s) to avoid flooding */
            if (tick % 20 == 1) {
                log_warn("scheduler", "Job %s waiting in queue: %s", job->id, diag);
            }
            db_update_status_reason(job->id, diag);
            queue_push(s_queue, job);
            continue;
        }

        int dispatched = 0;
        if (can_single) {
            char machine_id[64] = {0};
            if (alloc_reserve(job->id,
                              job->req_cores, job->req_gpu,
                              job->req_ram_mb, job->req_disk_mb,
                              machine_id) == 0) {
                /* Skip dispatch if machine is probed offline */
                Machine *mach = registry_get(machine_id);
                if (mach && mach->type == MACHINE_TYPE_STATIC &&
                    mach->probe_status == MACHINE_OFFLINE) {
                    log_warn("scheduler",
                        "Machine %s offline, releasing reservation for job %s",
                        machine_id, job->id);
                    alloc_release(job->id);
                } else {
                    strncpy(job->machine_id, machine_id, sizeof(job->machine_id) - 1);
                    job->n_machines = 1;
                    log_info("scheduler", "Dispatching job %s to %s", job->id, machine_id);
                    dispatched = 1;
                }
            }
        }

        if (!dispatched && can_multi) {
            char machine_ids[1024] = {0};
            int  n_machines = 0;
            if (alloc_reserve_multi(job->id,
                                    job->req_cores, job->req_gpu,
                                    job->req_ram_mb, job->req_disk_mb,
                                    machine_ids, &n_machines) == 0) {
                strncpy(job->machine_id, machine_ids, sizeof(job->machine_id) - 1);
                job->n_machines = n_machines;
                log_info("scheduler",
                         "Dispatching job %s across %d machines: %s",
                         job->id, n_machines, machine_ids);
                dispatched = 1;
            }
        }

        if (!dispatched) {
            queue_push(s_queue, job);
            continue;
        }

        /* ── Record dispatch event ──────────────────────────── */
        {
            char detail[256];
            snprintf(detail, sizeof(detail), "Dispatched to %s (%d machine(s))",
                     job->machine_id, job->n_machines);
            events_push_persistent("job", "dispatch", detail, job->user_id);
            db_insert_event("job", "dispatch", detail,
                            job->user_id, job->id, job->machine_id);
        }

        executor_spawn(job);
    }

    log_info("scheduler", "Scheduler stopped");
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

Queue *scheduler_init(void)
{
    s_queue = queue_create(64);
    return s_queue;
}

void scheduler_start(void)
{
    s_running = 1;
#ifdef _WIN32
    HANDLE th = CreateThread(NULL, 0, scheduler_thread, NULL, 0, NULL);
    if (th) CloseHandle(th);
#else
    pthread_t th;
    pthread_create(&th, NULL, scheduler_thread, NULL);
    pthread_detach(th);
#endif
}

void scheduler_stop(void)
{
    s_running = 0;
    if (s_queue) queue_shutdown(s_queue);
}

Queue *scheduler_queue(void)
{
    return s_queue;
}
