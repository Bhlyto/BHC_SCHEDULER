#include "queue.h"
#include "job.h"
#include "resources.h"
#include "config.h"
#include "log.h"
#include "db.h"
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
        if (!j->input_files[0]) continue;

        int expected = count_expected_files(j->input_files);
        char input_dir[512];
        store_input_dir(j->id, input_dir, sizeof(input_dir));
        int present = count_present_files(input_dir, j->input_files);

        log_debug("scheduler",
            "HELD job %s: expecting %d files, %d present",
            j->id, expected, present);

        if (present >= expected) {
            log_info("scheduler",
                "All %d input file(s) ready for job %s — releasing to queue",
                expected, j->id);
            Job *full = db_get_job(j->id);
            if (full) {
                char reason[80];
                snprintf(reason, sizeof(reason),
                    "All %d input file(s) received", expected);
                job_set_status_r(full, JOB_STATUS_IN_QUEUE, reason);
                queue_push(s_queue, full);
            }
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

        int can_single = alloc_can_fit(job->req_cores, job->req_gpu,
                                        job->req_ram_mb, job->req_disk_mb);
        int can_multi  = !can_single
                       ? alloc_can_fit_multi(job->req_cores, job->req_gpu,
                                             job->req_ram_mb, job->req_disk_mb)
                       : 0;

        if (!can_single && !can_multi) {
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
                strncpy(job->machine_id, machine_id, sizeof(job->machine_id) - 1);
                job->n_machines = 1;
                log_info("scheduler", "Dispatching job %s to %s", job->id, machine_id);
                dispatched = 1;
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
