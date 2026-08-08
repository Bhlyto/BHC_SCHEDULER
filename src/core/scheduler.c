#include "queue.h"
#include "job.h"
#include "resources.h"
#include "config.h"
#include "log.h"
#include "decision_core.h"
#include "config.h"
#include "cJSON.h"
#include "db.h"
#include "events.h"
#include "transfer.h"
#include "executor.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdio.h>

/* local trim helper (like the one in config.c) */
static void trim_local(char *s)
{
    char *start = s;
    while (*start == ' ' || *start == '\t') start++;
    if (start != s) memmove(s, start, strlen(start) + 1);
    int len = (int)strlen(s);
    while (len > 0 && (s[len-1] == ' ' || s[len-1] == '\t' || s[len-1] == '\r' || s[len-1] == '\n'))
        s[--len] = '\0';
}

#ifdef _WIN32
#  include <windows.h>
#  define sleep_ms(ms) Sleep(ms)
#else
#  include <unistd.h>
#  include <pthread.h>
#  define sleep_ms(ms) usleep((ms) * 1000)
#endif

static Queue *s_queue    = NULL;
static int    s_running  = 0;
#ifdef _WIN32
static HANDLE s_scheduler_thread = NULL;
#else
static pthread_t s_scheduler_thread;
static int s_scheduler_thread_started = 0;
#endif

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

static int job_fits_available_resources(const Job *job, void *context)
{
    (void)context;
    return alloc_can_fit(job->req_cores, job->req_gpu,
                         job->req_ram_mb, job->req_disk_mb);
}

/* Periodic: release CREATED jobs once their required inputs are ready. */
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

            /* After forwarding outputs (e.g., presim.json), re-run decision core
             * so the parent job is re-evaluated immediately.
             */
            {
                dc_context_t dctx;
                memset(&dctx, 0, sizeof(dctx));
                dctx.job_id = j->id;
                /* compute available cpus across registry */
                int mcount = 0;
                Machine *ml = registry_all(&mcount);
                uint32_t freecpus = 0;
                uint32_t freemem = 0;
                for (int mi = 0; ml && mi < mcount; mi++) {
                    Machine *m = &ml[mi];
                    if (m->enabled && m->probe_status == MACHINE_ONLINE) {
                        int free = m->cores_total - m->cores_reserved - m->cores_min;
                        if (free > 0) freecpus += (uint32_t)free;
                        int memfree = m->ram_mb_total - m->ram_mb_reserved - m->ram_mb_min;
                        if (memfree > 0) freemem += (uint32_t)memfree;
                    }
                }
                dctx.available_cpus = freecpus;
                dctx.available_mem_mb = freemem;
                dc_result_t dres;
                if (decision_core_decide(&dctx, &dres) == 0) {
                    log_info("scheduler", "Post-presim decision for job %s -> action=%d, target_cores=%u",
                             j->id, dres.action, dres.target_cores);
                    if (dres.allocation_json[0]) db_update_status_reason(j->id, dres.allocation_json);
                    if (dres.target_cores > 0 && dres.target_cores > (uint32_t)j->req_cores) j->req_cores = (int)dres.target_cores;

                    /* Handle REFINE similarly to main loop: create refinement child jobs */
                    if (dres.action == DC_ACTION_REFINE && dres.allocation_json[0]) {
                        cJSON *root = cJSON_Parse(dres.allocation_json);
                        if (root) {
                            cJSON *zones = cJSON_GetObjectItemCaseSensitive(root, "zones");
                            if (cJSON_IsArray(zones)) {
                                int n = cJSON_GetArraySize(zones);
                                char depends_buf[4096] = {0};
                                for (int zi = 0; zi < n; zi++) {
                                    cJSON *zone = cJSON_GetArrayItem(zones, zi);
                                    if (!cJSON_IsObject(zone)) continue;
                                    cJSON *jreq = cJSON_GetObjectItemCaseSensitive(zone, "req_cores");
                                    cJSON *jf   = cJSON_GetObjectItemCaseSensitive(zone, "fidelity");
                                    cJSON *jz   = cJSON_GetObjectItemCaseSensitive(zone, "zone");
                                    int reqc = (cJSON_IsNumber(jreq) ? (int)jreq->valuedouble : 0);
                                    int fidelity = (cJSON_IsNumber(jf) ? (int)jf->valuedouble : 1);
                                    int zoneid = (cJSON_IsNumber(jz) ? (int)jz->valuedouble : zi);
                                    if (reqc <= 0 && fidelity <= 1) continue;

                                    char child_cmd[1024];
                                    snprintf(child_cmd, sizeof(child_cmd), "%s --refine-zone=%d --fidelity=%d",
                                             j->command, zoneid, fidelity);
                                    Job *child = job_create_ex(child_cmd, j->priority + 5,
                                                                reqc > 0 ? reqc : 1, j->req_gpu,
                                                                j->req_ram_mb, j->req_disk_mb,
                                                                j->user_id, j->app_id);
                                    if (!child) continue;
                                    store_init_job_dirs(child->id);
                                    queue_push(s_queue, child);
                                    if (depends_buf[0]) strncat(depends_buf, ",", sizeof(depends_buf)-strlen(depends_buf)-1);
                                    strncat(depends_buf, child->id, sizeof(depends_buf)-strlen(depends_buf)-1);
                                }
                                if (depends_buf[0]) {
                                    db_update_depends_on(j->id, depends_buf);
                                    db_update_job_status(j->id, JOB_STATUS_CREATED, 0, 0);
                                    db_update_status_reason(j->id, "Waiting for refinement subjobs");
                                }
                            }
                            cJSON_Delete(root);
                        }
                    } else if (dres.action == DC_ACTION_DEFER) {
                        db_update_status_reason(j->id, "Deferred by decision core");
                    } else if (dres.action == DC_ACTION_MIGRATE) {
                        db_update_status_reason(j->id, "Migration requested by decision core");
                    } else if (dres.action == DC_ACTION_RUN_COARSE_SIM) {
                        db_update_status_reason(j->id, "Strategy: run coarse simulation");
                    } else if (dres.action == DC_ACTION_RUN_FINE_SIM) {
                        db_update_status_reason(j->id, "Strategy: run fine simulation");
                    }
                }
            }
        }

        /* ── Check input files ──────────────────────────────────── */
        if (j->input_files[0]) {
            int expected = count_expected_files(j->input_files);
            char input_dir[512];
            store_input_dir(j->id, input_dir, sizeof(input_dir));
            int present = count_present_files(input_dir, j->input_files);

            log_debug("scheduler",
                "CREATED job %s: expecting %d files, %d present",
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
            job_set_status_r(full, JOB_STATUS_QUEUED, reason);
            queue_push(s_queue, full);
        }
    }
    free(jobs);
}

/* ── Periodic: kill jobs that have exceeded their timeout ───────────────── */
static void scheduler_check_timeouts(void)
{
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
            j->exit_code = -1;
            job_set_status_r(j, JOB_STATUS_FAILED, reason);
            if (!executor_terminate(j->id)) {
                db_release_allocation(j->id);
                alloc_release(j->id);
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

        /* Every 10 ticks: check CREATED jobs and enforce timeouts. */
        if (++tick % 10 == 0) {
            scheduler_check_held_jobs();
            scheduler_check_timeouts();
        }

        /* Prefer the highest-priority job that can run now. If none fits,
           inspect the queue head so diagnostics and auto-provisioning still run. */
        Job *job = queue_try_pop_matching(s_queue, job_fits_available_resources, NULL);
        if (!job) job = queue_try_pop(s_queue);
        if (!job) continue;

        /* The database is authoritative. A queued object may be stale when a
           cancellation raced with the in-memory queue. */
        Job *persisted = db_get_job(job->id);
        if (!persisted || persisted->status != JOB_STATUS_QUEUED) {
            if (persisted) job_free(persisted);
            job_free(job);
            continue;
        }
        job_free(persisted);

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

        /* If this job's domain supports presim and no presim.json exists,
         * submit a presim job and hold the parent until presim completes.
         * Supported domains: thermal (extendable).
         */
        {
            char input_dir[512];
            store_input_dir(job->id, input_dir, sizeof(input_dir));
            char presim_path[768];
#ifdef _WIN32
            snprintf(presim_path, sizeof(presim_path), "%s\\presim.json", input_dir);
#else
            snprintf(presim_path, sizeof(presim_path), "%s/presim.json", input_dir);
#endif
            FILE *pf = fopen(presim_path, "rb");
            int has_presim = pf ? (fclose(pf), 1) : 0;
            /* Trigger presim only for configured domains and when presim missing */
            int trigger_presim = 0;
            if (!has_presim && job->app_id && g_config.presim_domains[0]) {
                /* presim_domains is a comma-separated list; match exact domain names */
                char pd[256]; strncpy(pd, g_config.presim_domains, sizeof(pd)-1); pd[sizeof(pd)-1] = '\0';
                char *tok = strtok(pd, ",");
                while (tok) {
                    trim_local(tok);
                    if (strcmp(tok, job->app_id) == 0) { trigger_presim = 1; break; }
                    tok = strtok(NULL, ",");
                }
            }
            if (trigger_presim) {
                log_info("scheduler", "Submitting presim job for parent %s (domain=%s)", job->id, job->app_id);
                /* create presim child job that runs the Python presim solver */
                char child_cmd[1024];
                /* write presim into parent's input dir */
                /* Allow domain-specific solver paths under tools/python_solvers/<domain>_presim.py
                 * Default to <domain>_presim.py for convenience. */
                char solver_path[512];
                snprintf(solver_path, sizeof(solver_path), "tools/python_solvers/%s_presim.py", job->app_id);
                /* Allow overriding solver command via environment variable PRESIM_SOLVER_CMD
                 * Supported placeholders: {parent}, {out}, {domain}, {input_dir}
                 */
                const char *env_cmd = getenv("PRESIM_SOLVER_CMD");
                if (env_cmd && env_cmd[0]) {
                    /* perform simple placeholder replacement into child_cmd */
                    const char *src = env_cmd;
                    size_t left = sizeof(child_cmd) - 1;
                    child_cmd[0] = '\0';
                    while (*src && left > 0) {
                        const char *p;
                        if ((p = strstr(src, "{parent}")) == src) {
                            int n = snprintf(child_cmd + strlen(child_cmd), left+1, "%s", job->id);
                            if (n < 0 || (size_t)n > left) break; left -= (size_t)n; src += 8; continue;
                        } else if ((p = strstr(src, "{out}")) == src) {
                            int n = snprintf(child_cmd + strlen(child_cmd), left+1, "%s", presim_path);
                            if (n < 0 || (size_t)n > left) break; left -= (size_t)n; src += 5; continue;
                        } else if ((p = strstr(src, "{domain}")) == src) {
                            int n = snprintf(child_cmd + strlen(child_cmd), left+1, "%s", job->app_id);
                            if (n < 0 || (size_t)n > left) break; left -= (size_t)n; src += 8; continue;
                        } else if ((p = strstr(src, "{input_dir}")) == src) {
                            int n = snprintf(child_cmd + strlen(child_cmd), left+1, "%s", input_dir);
                            if (n < 0 || (size_t)n > left) break; left -= (size_t)n; src += 11; continue;
                        }
                        /* copy one char */
                        size_t to_copy = 1;
                        strncat(child_cmd, src, to_copy);
                        src += to_copy; left -= to_copy;
                    }
                } else {
                    snprintf(child_cmd, sizeof(child_cmd), "python3 %s --parent %s --out %s", solver_path, job->id, presim_path);
                }
                Job *child = job_create_ex(child_cmd, job->priority + 5, 1, 0, 0, 0, job->user_id, "presim");
                if (child) {
                    store_init_job_dirs(child->id);
                    queue_push(s_queue, child);
                    /* mark parent as held and depends_on child */
                    db_update_depends_on(job->id, child->id);
                    db_update_job_status(job->id, JOB_STATUS_CREATED, 0, 0);
                    db_update_status_reason(job->id, "Waiting for presim subjob");
                    continue; /* don't run decision core yet */
                } else {
                    log_warn("scheduler", "Failed to create presim child for job %s", job->id);
                }
            }
        }

        /* Ask decision core for strategy / allocation hints */
        {
            dc_context_t dctx;
            memset(&dctx, 0, sizeof(dctx));
            dctx.job_id = job->id;
            /* compute available cpus across registry */
            {
                int mcount = 0;
                Machine *ml = registry_all(&mcount);
                uint32_t freecpus = 0;
                for (int mi = 0; mi < mcount; mi++) {
                    if (!ml) break;
                    Machine *m = &ml[mi];
                    if (m->enabled && m->probe_status == MACHINE_ONLINE) {
                        int free = m->cores_total - m->cores_reserved - m->cores_min;
                        if (free > 0) freecpus += (uint32_t)free;
                    }
                }
                dctx.available_cpus = freecpus;
                /* sum RAM */
                uint32_t freemem = 0;
                for (int mi = 0; mi < mcount; mi++) {
                    Machine *m = &ml[mi];
                    if (m->enabled && m->probe_status == MACHINE_ONLINE) {
                        int free = m->ram_mb_total - m->ram_mb_reserved - m->ram_mb_min;
                        if (free > 0) freemem += (uint32_t)free;
                    }
                }
                dctx.available_mem_mb = freemem;
            }
            dc_result_t dres;
            if (decision_core_decide(&dctx, &dres) == 0) {
                if (dres.allocation_json[0]) {
                    db_update_status_reason(job->id, dres.allocation_json);
                }
                if (dres.target_cores > 0) {
                    /* Respect decision by requesting at least target cores */
                    if (dres.target_cores > (uint32_t)job->req_cores)
                        job->req_cores = (int)dres.target_cores;
                }

                log_info("scheduler", "Decision core for job %s -> action=%d, target_cores=%u",
                         job->id, dres.action, dres.target_cores);

                /* Act on decision: REFINE -> create refinement child jobs and hold parent */
                if (dres.action == DC_ACTION_REFINE && dres.allocation_json[0]) {
                    cJSON *root = cJSON_Parse(dres.allocation_json);
                    if (root) {
                        cJSON *zones = cJSON_GetObjectItemCaseSensitive(root, "zones");
                        if (cJSON_IsArray(zones)) {
                            int n = cJSON_GetArraySize(zones);
                            char depends_buf[4096] = {0};
                            for (int zi = 0; zi < n; zi++) {
                                cJSON *zone = cJSON_GetArrayItem(zones, zi);
                                if (!cJSON_IsObject(zone)) continue;
                                cJSON *jreq = cJSON_GetObjectItemCaseSensitive(zone, "req_cores");
                                cJSON *jf   = cJSON_GetObjectItemCaseSensitive(zone, "fidelity");
                                cJSON *jz   = cJSON_GetObjectItemCaseSensitive(zone, "zone");
                                int reqc = (cJSON_IsNumber(jreq) ? (int)jreq->valuedouble : 0);
                                int fidelity = (cJSON_IsNumber(jf) ? (int)jf->valuedouble : 1);
                                int zoneid = (cJSON_IsNumber(jz) ? (int)jz->valuedouble : zi);
                                if (reqc <= 0 && fidelity <= 1) continue; /* nothing to do */

                                /* Create a refinement child job that will produce refined data for the zone */
                                char child_cmd[1024];
                                snprintf(child_cmd, sizeof(child_cmd), "%s --refine-zone=%d --fidelity=%d",
                                         job->command, zoneid, fidelity);
                                Job *child = job_create_ex(child_cmd, job->priority + 5,
                                                            reqc > 0 ? reqc : 1, job->req_gpu,
                                                            job->req_ram_mb, job->req_disk_mb,
                                                            job->user_id, job->app_id);
                                if (!child) continue;
                                /* Initialize child job dirs so it can run immediately */
                                store_init_job_dirs(child->id);
                                /* push child into queue for execution */
                                queue_push(s_queue, child);
                                if (depends_buf[0]) strncat(depends_buf, ",", sizeof(depends_buf)-strlen(depends_buf)-1);
                                strncat(depends_buf, child->id, sizeof(depends_buf)-strlen(depends_buf)-1);
                            }
                            if (depends_buf[0]) {
                                /* Return the parent to CREATED until refinements finish. */
                                db_update_depends_on(job->id, depends_buf);
                                db_update_job_status(job->id, JOB_STATUS_CREATED, 0, 0);
                                db_update_status_reason(job->id, "Waiting for refinement subjobs");
                                /* Do not dispatch parent now */
                                continue;
                            }
                        }
                        cJSON_Delete(root);
                    }
                }

                /* Migrate: leave for allocator to choose another machine (no-op here), Defer: requeue */
                if (dres.action == DC_ACTION_DEFER) {
                    db_update_status_reason(job->id, "Deferred by decision core");
                    queue_push(s_queue, job);
                    continue;
                }
                if (dres.action == DC_ACTION_MIGRATE) {
                    db_update_status_reason(job->id, "Migration requested by decision core");
                    queue_push(s_queue, job);
                    continue;
                }
                /* For RUN_COARSE / RUN_FINE we just tag the job and proceed to allocation */
                if (dres.action == DC_ACTION_RUN_COARSE_SIM) {
                    db_update_status_reason(job->id, "Strategy: run coarse simulation");
                } else if (dres.action == DC_ACTION_RUN_FINE_SIM) {
                    db_update_status_reason(job->id, "Strategy: run fine simulation");
                }
            }
        }

        int can_run = alloc_can_fit(job->req_cores, job->req_gpu,
                                    job->req_ram_mb, job->req_disk_mb);

        if (!can_run) {
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
        if (can_run) {
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
                    log_info("scheduler", "Dispatching job %s to %s", job->id, machine_id);
                    dispatched = 1;
                }
            }
        }

        if (!dispatched) {
            queue_push(s_queue, job);
            continue;
        }

        /* ── Record dispatch event ──────────────────────────── */
        {
            char detail[256];
            snprintf(detail, sizeof(detail), "Dispatched to worker %s",
                     job->machine_id);
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
    if (!s_queue) return NULL;

    int recovered = db_recover_incomplete_jobs();
    if (recovered > 0)
        log_warn("scheduler", "Marked %d interrupted job(s) as FAILED", recovered);

    enum { RECOVERY_PAGE_SIZE = 256 };
    Job *page = (Job *)malloc(RECOVERY_PAGE_SIZE * sizeof(Job));
    if (!page) {
        queue_destroy(s_queue);
        s_queue = NULL;
        return NULL;
    }
    int offset = 0;
    for (;;) {
        int count = db_list_queued_jobs(page, RECOVERY_PAGE_SIZE, offset);
        for (int i = 0; i < count; i++) {
            Job *job = (Job *)malloc(sizeof(Job));
            if (!job) continue;
            *job = page[i];
            if (queue_push(s_queue, job) != 0) job_free(job);
        }
        offset += count;
        if (count < RECOVERY_PAGE_SIZE) break;
    }
    free(page);
    if (offset > 0)
        log_info("scheduler", "Recovered %d queued job(s) from SQLite", offset);

    /* Initialize decision core (config_path optional) */
    decision_core_init(NULL);
    return s_queue;
}

void scheduler_start(void)
{
    s_running = 1;
#ifdef _WIN32
    s_scheduler_thread = CreateThread(NULL, 0, scheduler_thread, NULL, 0, NULL);
    if (!s_scheduler_thread) s_running = 0;
#else
    if (pthread_create(&s_scheduler_thread, NULL, scheduler_thread, NULL) == 0)
        s_scheduler_thread_started = 1;
    else
        s_running = 0;
#endif
}

void scheduler_stop(void)
{
    s_running = 0;
    if (s_queue) queue_shutdown(s_queue);
#ifdef _WIN32
    if (s_scheduler_thread) {
        WaitForSingleObject(s_scheduler_thread, INFINITE);
        CloseHandle(s_scheduler_thread);
        s_scheduler_thread = NULL;
    }
#else
    if (s_scheduler_thread_started) {
        pthread_join(s_scheduler_thread, NULL);
        s_scheduler_thread_started = 0;
    }
#endif
    if (s_queue) {
        Job *job;
        while ((job = queue_try_pop(s_queue)) != NULL) job_free(job);
        queue_destroy(s_queue);
        s_queue = NULL;
    }
    /* Shutdown decision core */
    decision_core_shutdown();
}

Queue *scheduler_queue(void)
{
    return s_queue;
}
