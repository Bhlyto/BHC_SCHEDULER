#include "job.h"
#include "resources.h"
#include "transfer.h"
#include "db.h"
#include "log.h"
#include "config.h"
#include "events.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <pthread.h>
#  include <signal.h>
#  include <sys/types.h>
#  include <unistd.h>
#endif

#define MAX_ACTIVE_PROCESSES 4096

typedef struct {
    char job_id[JOB_ID_LEN];
    int active;
#ifdef _WIN32
    HANDLE process_handle;
    HANDLE job_object;
#else
    pid_t pid;
#endif
} ActiveProcess;

static ActiveProcess s_active_processes[MAX_ACTIVE_PROCESSES];
static int s_inflight_jobs = 0;
static int s_shutting_down = 0;

#ifdef _WIN32
static SRWLOCK s_active_lock = SRWLOCK_INIT;
#  define active_lock()   AcquireSRWLockExclusive(&s_active_lock)
#  define active_unlock() ReleaseSRWLockExclusive(&s_active_lock)
#else
static pthread_mutex_t s_active_lock = PTHREAD_MUTEX_INITIALIZER;
#  define active_lock()   pthread_mutex_lock(&s_active_lock)
#  define active_unlock() pthread_mutex_unlock(&s_active_lock)
#endif

#ifdef _WIN32
static int active_process_register(const char *job_id, HANDLE process_handle,
                                   HANDLE job_object)
#else
static int active_process_register(const char *job_id, pid_t pid)
#endif
{
    int registered = 0;
    active_lock();
    for (int i = 0; i < MAX_ACTIVE_PROCESSES; i++) {
        ActiveProcess *p = &s_active_processes[i];
        if (p->active) continue;
        memset(p, 0, sizeof(*p));
        strncpy(p->job_id, job_id, sizeof(p->job_id) - 1);
#ifdef _WIN32
        p->process_handle = process_handle;
        p->job_object = job_object;
#else
        p->pid = pid;
#endif
        p->active = 1;
        registered = 1;
        break;
    }
    active_unlock();
    return registered ? 0 : -1;
}

static void active_process_unregister(const char *job_id)
{
    active_lock();
    for (int i = 0; i < MAX_ACTIVE_PROCESSES; i++) {
        ActiveProcess *p = &s_active_processes[i];
        if (p->active && strcmp(p->job_id, job_id) == 0) {
            p->active = 0;
            break;
        }
    }
    active_unlock();
}

static void inflight_increment(void)
{
    active_lock();
    s_inflight_jobs++;
    active_unlock();
}

static void inflight_decrement(void)
{
    active_lock();
    if (s_inflight_jobs > 0) s_inflight_jobs--;
    active_unlock();
}

static int executor_is_shutting_down(void)
{
    int value;
    active_lock();
    value = s_shutting_down;
    active_unlock();
    return value;
}

int executor_terminate(const char *job_id)
{
    int found = 0;
    active_lock();
    for (int i = 0; i < MAX_ACTIVE_PROCESSES; i++) {
        ActiveProcess *p = &s_active_processes[i];
        if (!p->active || strcmp(p->job_id, job_id) != 0) continue;
        found = 1;
#ifdef _WIN32
        if (p->job_object)
            TerminateJobObject(p->job_object, 1);
        else if (p->process_handle)
            TerminateProcess(p->process_handle, 1);
#else
        if (p->pid > 0) {
            if (kill(-p->pid, SIGTERM) != 0)
                kill(p->pid, SIGTERM);
        }
#endif
        break;
    }
    active_unlock();
    return found;
}

int executor_shutdown(void)
{
    active_lock();
    s_shutting_down = 1;
    for (int i = 0; i < MAX_ACTIVE_PROCESSES; i++) {
        ActiveProcess *p = &s_active_processes[i];
        if (!p->active) continue;
#ifdef _WIN32
        if (p->job_object)
            TerminateJobObject(p->job_object, 1);
        else if (p->process_handle)
            TerminateProcess(p->process_handle, 1);
#else
        if (p->pid > 0 && kill(-p->pid, SIGTERM) != 0)
            kill(p->pid, SIGTERM);
#endif
    }
    active_unlock();

    for (int waited_ms = 0; waited_ms < 30000; waited_ms += 50) {
        int remaining;
        active_lock();
        remaining = s_inflight_jobs;
        active_unlock();
        if (remaining == 0) return 0;
#ifdef _WIN32
        Sleep(50);
#else
        usleep(50 * 1000);
#endif
    }

    log_error("executor", "Timed out waiting for executor threads to stop");
    return -1;
}

static int job_is_terminal_in_db(const char *job_id)
{
    Job *persisted = db_get_job(job_id);
    if (!persisted) return 1;
    int terminal = persisted->status == JOB_STATUS_FINISHED ||
                   persisted->status == JOB_STATUS_FAILED ||
                   persisted->status == JOB_STATUS_CANCELLED;
    job_free(persisted);
    return terminal;
}

/* ── Auto-deprovision helper ──────────────────────────────────────────── */
static void maybe_auto_deprovision(const char *machine_id)
{
    if (!g_config.cloud_auto_deprovision) return;
    if (!machine_id || !machine_id[0]) return;

    Machine *m = registry_get(machine_id);
    if (!m || m->type != MACHINE_TYPE_CLOUD) return;

    /* Check if the machine still has reserved resources */
    if (m->cores_reserved > 0 || m->ram_mb_reserved > 0 ||
        m->disk_mb_reserved > 0 || m->gpu_count_reserved > 0)
        return;

    /* Machine is cloud + completely idle → deprovision */
    log_info("executor",
        "Auto-deprovisioning idle cloud machine %s (%s/%s)",
        m->id, m->cloud_provider, m->cloud_instance_id);

    char detail[256];
    snprintf(detail, sizeof(detail), "Auto-deprovisioned %s (provider=%s, instance=%s)",
             m->id, m->cloud_provider, m->cloud_instance_id);

    if (cloud_deprovision(m->cloud_provider, m->cloud_instance_id) == 0) {
        events_push_persistent("cloud", "auto_deprovision", detail, "");
        db_insert_event("cloud", "auto_deprovision", detail, "", "", m->id);
    } else {
        log_warn("executor", "Auto-deprovision failed for %s", m->id);
    }
}

/* ═══════════════════════════════════════════════════════════════════
 *  Shared helpers (platform-independent)
 * ═══════════════════════════════════════════════════════════════════ */

/* Shell-escape a string for safe use inside double quotes in sh scripts.
 * Escapes: \ " $ ` ! newlines */
static void shell_escape(const char *src, char *dst, int dst_len)
{
    int j = 0;
    for (int i = 0; src[i] && j < dst_len - 2; i++) {
        switch (src[i]) {
            case '\\': case '"': case '$': case '`': case '!':
                dst[j++] = '\\';
                if (j < dst_len - 1) dst[j++] = src[i];
                break;
            case '\n':
                /* Skip newlines in env values */
                break;
            default:
                dst[j++] = src[i];
                break;
        }
    }
    dst[j] = '\0';
}

/* Validate env variable key: only [A-Za-z0-9_], must start with letter/underscore */
static int valid_env_key(const char *k)
{
    if (!k || !k[0]) return 0;
    if (!(k[0] == '_' || (k[0] >= 'A' && k[0] <= 'Z') || (k[0] >= 'a' && k[0] <= 'z')))
        return 0;
    for (const char *p = k + 1; *p; p++) {
        if (!(*p == '_' || (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
              (*p >= '0' && *p <= '9')))
            return 0;
    }
    return 1;
}

/*
 * Resolve machine_id to a network address (IP preferred over hostname).
 * Returns 1 if the target is local (localhost / 127.0.0.1 / ::1),
 * 0 if remote.
 */
static int machine_host(const char *machine_id, char *out, int out_len)
{
    if (!machine_id || !machine_id[0]) {
        strncpy(out, "localhost", out_len - 1);
        out[out_len - 1] = '\0';
        return 1;
    }
    Machine *m = registry_get(machine_id);
    if (m) {
        if      (m->ip[0])       strncpy(out, m->ip,       out_len - 1);
        else if (m->hostname[0]) strncpy(out, m->hostname, out_len - 1);
        else                     strncpy(out, machine_id,  out_len - 1);
    } else {
        strncpy(out, machine_id, out_len - 1);
    }
    out[out_len - 1] = '\0';
    return (strcmp(out, "localhost") == 0 ||
            strcmp(out, "127.0.0.1") == 0 ||
            strcmp(out, "::1")       == 0);
}

/* Build the SSH / SCP option string shared by every remote call.
 * known_hosts_path: path to a writable known_hosts file for this job.
 * Passing a per-job file (created empty before first SSH call) ensures
 * all connections within a job share the same host-key cache and avoids
 * the "Host key verification failed" issue with NUL on Windows OpenSSH. */
static void build_ssh_opts(char *out, int out_len, const char *known_hosts_path)
{
    if (g_config.ssh_key[0])
        snprintf(out, out_len,
            "-i \"%s\""
            " -o BatchMode=yes"
            " -o PasswordAuthentication=no"
            " -o StrictHostKeyChecking=accept-new"
            " -o UserKnownHostsFile=%s"
            " -o GlobalKnownHostsFile=%s"
            " -o LogLevel=ERROR"
            " -o ConnectTimeout=10"
            " -o ServerAliveInterval=15"
            " -o ServerAliveCountMax=2",
            g_config.ssh_key, known_hosts_path, known_hosts_path);
    else
        snprintf(out, out_len,
            "-o BatchMode=yes"
            " -o PasswordAuthentication=no"
            " -o StrictHostKeyChecking=accept-new"
            " -o UserKnownHostsFile=%s"
            " -o GlobalKnownHostsFile=%s"
            " -o LogLevel=ERROR"
            " -o ConnectTimeout=10"
            " -o ServerAliveInterval=15"
            " -o ServerAliveCountMax=2",
            known_hosts_path, known_hosts_path);
}

/*
 * Write a self-contained POSIX shell script that exports ORCH_* variables
 * and runs job->command on the remote host.  The file is stored under the
 * job's local input directory and will be SCP-uploaded to the remote.
 */
static int write_run_script(const Job *job,
                            const char *remote_input,
                            const char *remote_output,
                            char *script_path, int script_path_len)
{
#ifdef _WIN32
    snprintf(script_path, script_path_len, "%s\\.run.sh", job->input_dir);
#else
    snprintf(script_path, script_path_len, "%s/.run.sh",  job->input_dir);
#endif
    FILE *f = fopen(script_path, "wb"); /* "wb" preserves Unix LF on Windows */
    if (!f) {
        log_error("executor", "Cannot write run script: %s", script_path);
        return -1;
    }
    char escaped_mid[512];
    shell_escape(job->machine_id, escaped_mid, sizeof(escaped_mid));
    fprintf(f, "#!/bin/sh\n");
    fprintf(f, "export ORCH_JOB_ID=\"%s\"\n",        job->id);
    fprintf(f, "export ORCH_INPUT_DIR=\"%s\"\n",     remote_input);
    fprintf(f, "export ORCH_OUTPUT_DIR=\"%s\"\n",    remote_output);
    fprintf(f, "export ORCH_WORKER_ID=\"%s\"\n",    escaped_mid);
    /* Compatibility aliases remain single-valued in v1. */
    fprintf(f, "export ORCH_MACHINE_IDS=\"%s\"\n",  escaped_mid);
    fprintf(f, "export ORCH_MACHINE_COUNT=\"1\"\n");

    /* Export app-specific environment from .app_env.json if present */
    {
        char env_path[768];
#ifdef _WIN32
        snprintf(env_path, sizeof(env_path), "%s\\.app_env.json", job->input_dir);
#else
        snprintf(env_path, sizeof(env_path), "%s/.app_env.json", job->input_dir);
#endif
        FILE *ef = fopen(env_path, "rb");
        if (ef) {
            fseek(ef, 0, SEEK_END); long esz = ftell(ef); rewind(ef);
            if (esz > 0 && esz < 65536) {
                char *edata = (char *)malloc(esz + 1);
                fread(edata, 1, esz, ef);
                edata[esz] = '\0';
                cJSON *env = cJSON_Parse(edata);
                if (env) {
                    cJSON *kv = NULL;
                    cJSON_ArrayForEach(kv, env) {
                        if (cJSON_IsString(kv) && valid_env_key(kv->string)) {
                            char escaped[4096];
                            shell_escape(kv->valuestring, escaped, sizeof(escaped));
                            fprintf(f, "export %s=\"%s\"\n",
                                    kv->string, escaped);
                        }
                    }
                    cJSON_Delete(env);
                }
                free(edata);
            }
            fclose(ef);
        }
    }

    fprintf(f, "mkdir -p \"$ORCH_OUTPUT_DIR\"\n");
    fprintf(f, "cd \"$ORCH_OUTPUT_DIR\"\n");
    fprintf(f, "exec %s\n", job->command);
    fclose(f);
    return 0;
}

#ifdef _WIN32
#include <windows.h>

typedef struct {
    Job    *job;
    HANDLE  proc_handle;
    HANDLE  job_object;
    int     is_remote;
    char    remote_host[256];
    char    remote_output[512];
    char    known_hosts_path[512];
} WatchArg;

/* Carries just the Job pointer across into the launcher thread. */
typedef struct { Job *job; } LaunchArg;

/* Run a command synchronously; returns process exit code, -1 on launch failure. */
static int run_sync_win(const char *cmd)
{
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));
    char *buf = _strdup(cmd);
    if (!CreateProcessA(NULL, buf, NULL, NULL, FALSE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        free(buf);
        return -1;
    }
    free(buf);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD rc = 1;
    GetExitCodeProcess(pi.hProcess, &rc);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)rc;
}

/*
 * Like run_sync_win but routes stdout+stderr to the job's open log handles
 * so SSH errors are visible in the job's stderr.log instead of silently lost.
 */
static int run_sync_win_log(const char *cmd, HANDLE hOut, HANDLE hErr)
{
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = (hOut != INVALID_HANDLE_VALUE) ? hOut : GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError  = (hErr != INVALID_HANDLE_VALUE) ? hErr : GetStdHandle(STD_ERROR_HANDLE);
    char *buf = _strdup(cmd);
    if (!CreateProcessA(NULL, buf, NULL, NULL, TRUE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        free(buf);
        return -1;
    }
    free(buf);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD rc = 1;
    GetExitCodeProcess(pi.hProcess, &rc);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)rc;
}

static DWORD WINAPI watcher_thread(LPVOID arg)
{
    WatchArg *wa = (WatchArg *)arg;
    WaitForSingleObject(wa->proc_handle, INFINITE);
    DWORD exit_code = 1;
    GetExitCodeProcess(wa->proc_handle, &exit_code);
    active_process_unregister(wa->job->id);
    CloseHandle(wa->proc_handle);

    int already_terminal = job_is_terminal_in_db(wa->job->id);

    /* For remote jobs: retrieve output files after the SSH process exits */
    if (!already_terminal && wa->is_remote && g_config.ssh_user[0]) {
        char ssh_opts[512];
        build_ssh_opts(ssh_opts, sizeof(ssh_opts), wa->known_hosts_path);
        char scp_cmd[1024];
        _snprintf(scp_cmd, sizeof(scp_cmd),
            "scp %s -r %s@%s:%s/. \"%s\"",
            ssh_opts, g_config.ssh_user, wa->remote_host,
            wa->remote_output, wa->job->output_dir);
        log_info("executor", "Retrieving output for job %s from %s",
                 wa->job->id, wa->remote_host);
        int scp_rc = run_sync_win(scp_cmd);
        if (scp_rc != 0)
            log_warn("executor",
                "Output SCP returned %d for job %s (output dir may be empty)",
                scp_rc, wa->job->id);
    }

    if (!already_terminal) {
        wa->job->exit_code = (int)exit_code;
        char reason[280];
        if (exit_code == 0) {
            job_set_status_r(wa->job, JOB_STATUS_FINISHED, "Completed successfully");
        } else if (wa->is_remote) {
            _snprintf(reason, sizeof(reason), "Process exited with code %lu on %s",
                      exit_code, wa->remote_host);
            job_set_status_r(wa->job, JOB_STATUS_FAILED, reason);
        } else {
            _snprintf(reason, sizeof(reason), "Process exited with code %lu", exit_code);
            job_set_status_r(wa->job, JOB_STATUS_FAILED, reason);
        }
    }

    if (artifact_collect_job(wa->job->id) < 0)
        log_warn("executor", "Artifact collection failed for job %s", wa->job->id);

    db_release_allocation(wa->job->id);
    alloc_release(wa->job->id);
    maybe_auto_deprovision(wa->job->machine_id);
    /* Clean up temp known_hosts file */
    if (wa->known_hosts_path[0]) DeleteFileA(wa->known_hosts_path);
    if (wa->job_object) CloseHandle(wa->job_object);
    job_free(wa->job);
    free(wa);
    inflight_decrement();
    return 0;
}

static int spawn_process(Job *job)
{
    /* ── Determine target host ────────────────────────────────────── */
    char host[256];
    int local = machine_host(job->machine_id, host, sizeof(host));

    /* Force local execution when no SSH user is configured */
    if (!g_config.ssh_user[0]) local = 1;

    /* ── Open log files ──────────────────────────────────────────── */
    char stdout_path[512], stderr_path[512];
    store_stdout_path(job->id, stdout_path, sizeof(stdout_path));
    store_stderr_path(job->id, stderr_path, sizeof(stderr_path));

    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE hStdout = CreateFileA(stdout_path, GENERIC_WRITE,
        FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    HANDLE hStderr = CreateFileA(stderr_path, GENERIC_WRITE,
        FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = (hStdout != INVALID_HANDLE_VALUE) ? hStdout : GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError  = (hStderr != INVALID_HANDLE_VALUE) ? hStderr : GetStdHandle(STD_ERROR_HANDLE);

    WatchArg *wa  = NULL;
    char      cmd[JOB_CMD_LEN + 2048];

    if (local) {
        /* ══ LOCAL EXECUTION (fallback / exception) ═════════════════ */
        LPCH parent_env = GetEnvironmentStrings();
        size_t parent_sz = 0;
        if (parent_env) {
            const char *p = parent_env;
            while (*p) { size_t l = strlen(p) + 1; parent_sz += l; p += l; }
        }
        size_t extra_sz =
              strlen("ORCH_JOB_ID=")        + strlen(job->id)          + 1
            + strlen("ORCH_INPUT_DIR=")     + strlen(job->input_dir)   + 1
            + strlen("ORCH_OUTPUT_DIR=")    + strlen(job->output_dir)  + 1
            + strlen("ORCH_WORKER_ID=")     + strlen(job->machine_id)  + 1
            + strlen("ORCH_MACHINE_IDS=")   + strlen(job->machine_id)  + 1
            + strlen("ORCH_MACHINE_COUNT=1") + 1
            + 2;
        size_t env_capacity = parent_sz + extra_sz;
        char *env_block = (char *)malloc(env_capacity);
        if (!env_block) {
            if (parent_env) FreeEnvironmentStrings(parent_env);
            if (hStdout != INVALID_HANDLE_VALUE) CloseHandle(hStdout);
            if (hStderr != INVALID_HANDLE_VALUE) CloseHandle(hStderr);
            return -1;
        }
        int env_pos = 0;
        if (parent_env && parent_sz > 0) {
            memcpy(env_block, parent_env, parent_sz);
            env_pos = (int)parent_sz;
        }
        if (parent_env) FreeEnvironmentStrings(parent_env);

#define ADD_ENV(k, v) do { \
    int _n = _snprintf(env_block + env_pos, env_capacity - (size_t)env_pos, "%s=%s", k, v); \
    if (_n > 0) env_pos += _n + 1; \
} while(0)
        ADD_ENV("ORCH_JOB_ID",        job->id);
        ADD_ENV("ORCH_INPUT_DIR",     job->input_dir);
        ADD_ENV("ORCH_OUTPUT_DIR",    job->output_dir);
        ADD_ENV("ORCH_WORKER_ID",     job->machine_id);
        ADD_ENV("ORCH_MACHINE_IDS",   job->machine_id);
        ADD_ENV("ORCH_MACHINE_COUNT", "1");
        env_block[env_pos++] = '\0';
#undef ADD_ENV

        /*
         * Generate .run.sh with Linux-side ORCH_* paths BEFORE the pre_job
         * script runs.  The pre_job script SCP-uploads the whole input dir,
         * so the file lands at <ssh_remote_work_dir>/<job_id>/input/.run.sh
         * on the Linux node.  Users / the job command can then source it:
         *   . "$ORCH_INPUT_DIR/.run.sh"   (after the exec line – just the exports)
         * This is also the path the native SSH path uses, so behaviour is
         * consistent whether ssh_user is set or not.
         */
        if (g_config.ssh_remote_work_dir[0]) {
            char r_input[512], r_output[512], script_local[512];
            _snprintf(r_input,  sizeof(r_input),  "%s/%s/input",
                      g_config.ssh_remote_work_dir, job->id);
            _snprintf(r_output, sizeof(r_output), "%s/%s/output",
                      g_config.ssh_remote_work_dir, job->id);
            if (write_run_script(job, r_input, r_output,
                                 script_local, sizeof(script_local)) == 0)
                log_debug("executor",
                    "Pre-job .run.sh written to %s (remote vars: %s)",
                    script_local, r_input);
        }

        if (g_config.pre_job_script_win[0]) {
            char pre_cmd[512 + 10];
            _snprintf(pre_cmd, sizeof(pre_cmd), "cmd /c \"%s\"", g_config.pre_job_script_win);
            PROCESS_INFORMATION pre_pi; STARTUPINFOA pre_si;
            ZeroMemory(&pre_si, sizeof(pre_si)); pre_si.cb = sizeof(pre_si);
            ZeroMemory(&pre_pi, sizeof(pre_pi));
            pre_si.dwFlags    = STARTF_USESTDHANDLES;
            pre_si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);
            pre_si.hStdOutput = (hStdout != INVALID_HANDLE_VALUE) ? hStdout : GetStdHandle(STD_OUTPUT_HANDLE);
            pre_si.hStdError  = (hStderr != INVALID_HANDLE_VALUE) ? hStderr : GetStdHandle(STD_ERROR_HANDLE);
            if (CreateProcessA(NULL, pre_cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW,
                               env_block, NULL, &pre_si, &pre_pi)) {
                WaitForSingleObject(pre_pi.hProcess, INFINITE);
                DWORD pre_exit = 1;
                GetExitCodeProcess(pre_pi.hProcess, &pre_exit);
                CloseHandle(pre_pi.hProcess);
                CloseHandle(pre_pi.hThread);
                if (pre_exit != 0) {
                    log_error("executor", "Pre-job script failed (exit %lu) for job %s",
                              pre_exit, job->id);
                    if (hStdout != INVALID_HANDLE_VALUE) CloseHandle(hStdout);
                    if (hStderr != INVALID_HANDLE_VALUE) CloseHandle(hStderr);
                    free(env_block);
                    return -1;
                }
            } else {
                log_error("executor", "Cannot start pre-job script: %lu", GetLastError());
            }
        }

        _snprintf(cmd, sizeof(cmd), "cmd /c %s", job->command);
        if (!CreateProcessA(NULL, cmd, NULL, NULL, TRUE,
                            CREATE_NO_WINDOW, env_block, NULL, &si, &pi)) {
            log_error("executor", "CreateProcess failed: %lu", GetLastError());
            if (hStdout != INVALID_HANDLE_VALUE) CloseHandle(hStdout);
            if (hStderr != INVALID_HANDLE_VALUE) CloseHandle(hStderr);
            free(env_block);
            return -1;
        }
        free(env_block);
        wa = (WatchArg *)malloc(sizeof(WatchArg));
        if (!wa) {
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, INFINITE);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            if (hStdout != INVALID_HANDLE_VALUE) CloseHandle(hStdout);
            if (hStderr != INVALID_HANDLE_VALUE) CloseHandle(hStderr);
            return -1;
        }
        wa->is_remote = 0;
        wa->remote_host[0]        = '\0';
        wa->remote_output[0]      = '\0';
        wa->known_hosts_path[0]   = '\0';

    } else {
        /* ══ REMOTE EXECUTION via SSH (normal path) ══════════════════ */
        /* Per-job known_hosts in TEMP: short path, no backslashes, no spaces.
         * All 4 SSH/SCP calls share it so the key is trusted after first connect. */
        char known_hosts_path[512];
        {
            char tmp[256];
            if (g_config.temp_dir[0]) {
                strncpy(tmp, g_config.temp_dir, sizeof(tmp) - 1);
                tmp[sizeof(tmp)-1] = '\0';
            } else {
                strcpy(tmp, "C:/Temp");
                GetTempPathA(sizeof(tmp), tmp);
            }
            /* Replace backslashes with forward slashes for OpenSSH */
            for (char *p = tmp; *p; p++) if (*p == '\\') *p = '/';
            /* Strip trailing slash */
            int tl = (int)strlen(tmp);
            if (tl > 0 && tmp[tl-1] == '/') tmp[tl-1] = '\0';
            _snprintf(known_hosts_path, sizeof(known_hosts_path),
                      "%s/orch-%s.kh", tmp, job->id);
        }
        { FILE *_kh = fopen(known_hosts_path, "ab"); if (_kh) fclose(_kh); }
        log_info("executor", "Job %s known_hosts: %s (exists:%d)",
                 job->id, known_hosts_path,
                 (int)(GetFileAttributesA(known_hosts_path) != INVALID_FILE_ATTRIBUTES));

        char ssh_opts[512];
        build_ssh_opts(ssh_opts, sizeof(ssh_opts), known_hosts_path);

        char remote_base[512], remote_input[512], remote_output[512];
        _snprintf(remote_base,   sizeof(remote_base),
            "%s/%s",        g_config.ssh_remote_work_dir, job->id);
        _snprintf(remote_input,  sizeof(remote_input),
            "%s/%s/input",  g_config.ssh_remote_work_dir, job->id);
        _snprintf(remote_output, sizeof(remote_output),
            "%s/%s/output", g_config.ssh_remote_work_dir, job->id);

        log_info("executor", "Job %s → remote %s (%s)",
                 job->id, job->machine_id, host);

        /* 1. Create remote directories */
        {
            char mkdir_cmd[1024];
            _snprintf(mkdir_cmd, sizeof(mkdir_cmd),
                "ssh %s %s@%s \"mkdir -p '%s/input' '%s/output'\"",
                ssh_opts, g_config.ssh_user, host, remote_base, remote_base);
            log_info("executor", "SSH mkdir cmd: %s", mkdir_cmd);
            int rc = run_sync_win_log(mkdir_cmd, hStdout, hStderr);
            if (rc != 0) {
                log_error("executor",
                    "Cannot create remote dirs on %s (exit %d) for job %s"
                    " — see stderr.log for SSH error detail",
                    host, rc, job->id);
                _snprintf(job->status_reason, sizeof(job->status_reason),
                    "SSH: cannot create remote directories on %s (exit %d)", host, rc);
                if (hStdout != INVALID_HANDLE_VALUE) CloseHandle(hStdout);
                if (hStderr != INVALID_HANDLE_VALUE) CloseHandle(hStderr);
                return -1;
            }
        }

        /* 2. Copy input files to remote (best-effort; dir may be empty) */
        {
            char scp_in[1024];
            _snprintf(scp_in, sizeof(scp_in),
                "scp %s -r \"%s/.\" %s@%s:%s",
                ssh_opts, job->input_dir, g_config.ssh_user, host, remote_input);
            int rc = run_sync_win_log(scp_in, hStdout, hStderr);
            if (rc != 0)
                log_debug("executor",
                    "Input SCP returned %d for job %s (input dir may be empty)",
                    rc, job->id);
        }

        /* 3. Generate and upload run script */
        char script_local[512];
        if (write_run_script(job, remote_input, remote_output,
                             script_local, sizeof(script_local)) != 0) {
            if (hStdout != INVALID_HANDLE_VALUE) CloseHandle(hStdout);
            if (hStderr != INVALID_HANDLE_VALUE) CloseHandle(hStderr);
            return -1;
        }
        {
            char scp_script[1024];
            _snprintf(scp_script, sizeof(scp_script),
                "scp %s \"%s\" %s@%s:%s/.run.sh",
                ssh_opts, script_local, g_config.ssh_user, host, remote_base);
            int rc = run_sync_win_log(scp_script, hStdout, hStderr);
            if (rc != 0) {
                log_error("executor",
                    "Cannot upload run script to %s (exit %d) for job %s",
                    host, rc, job->id);
                _snprintf(job->status_reason, sizeof(job->status_reason),
                    "SSH: failed to upload run script to %s (exit %d)", host, rc);
                if (hStdout != INVALID_HANDLE_VALUE) CloseHandle(hStdout);
                if (hStderr != INVALID_HANDLE_VALUE) CloseHandle(hStderr);
                return -1;
            }
        }

        /* 4. Launch SSH asynchronously — stdout/stderr piped to log files */
        const char *rshell = g_config.ssh_shell[0] ? g_config.ssh_shell : "sh";
        _snprintf(cmd, sizeof(cmd),
            "ssh %s %s@%s \"%s '%s/.run.sh'\"",
            ssh_opts, g_config.ssh_user, host, rshell, remote_base);
        if (!CreateProcessA(NULL, cmd, NULL, NULL, TRUE,
                            CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
            log_error("executor", "SSH CreateProcess failed: %lu", GetLastError());
            _snprintf(job->status_reason, sizeof(job->status_reason),
                "SSH: failed to start connection to %s (win32 error %lu)",
                host, GetLastError());
            if (hStdout != INVALID_HANDLE_VALUE) CloseHandle(hStdout);
            if (hStderr != INVALID_HANDLE_VALUE) CloseHandle(hStderr);
            return -1;
        }
        wa = (WatchArg *)malloc(sizeof(WatchArg));
        if (!wa) {
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, INFINITE);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            if (hStdout != INVALID_HANDLE_VALUE) CloseHandle(hStdout);
            if (hStderr != INVALID_HANDLE_VALUE) CloseHandle(hStderr);
            return -1;
        }
        wa->is_remote = 1;
        strncpy(wa->remote_host,        host,             sizeof(wa->remote_host)        - 1);
        strncpy(wa->remote_output,      remote_output,    sizeof(wa->remote_output)      - 1);
        strncpy(wa->known_hosts_path,   known_hosts_path, sizeof(wa->known_hosts_path)   - 1);
    }

    if (hStdout != INVALID_HANDLE_VALUE) CloseHandle(hStdout);
    if (hStderr != INVALID_HANDLE_VALUE) CloseHandle(hStderr);
    CloseHandle(pi.hThread);
    wa->job = job;
    wa->proc_handle = pi.hProcess;
    wa->job_object = CreateJobObjectA(NULL, NULL);
    if (wa->job_object) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits;
        ZeroMemory(&limits, sizeof(limits));
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(wa->job_object,
                                     JobObjectExtendedLimitInformation,
                                     &limits, sizeof(limits)) ||
            !AssignProcessToJobObject(wa->job_object, pi.hProcess)) {
            CloseHandle(wa->job_object);
            wa->job_object = NULL;
        }
    }
    if (active_process_register(job->id, pi.hProcess, wa->job_object) != 0) {
        if (wa->job_object) TerminateJobObject(wa->job_object, 1);
        else TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, INFINITE);
        CloseHandle(pi.hProcess);
        if (wa->job_object) CloseHandle(wa->job_object);
        free(wa);
        return -1;
    }
    if (!job_is_terminal_in_db(job->id))
        job_set_status(job, JOB_STATUS_RUNNING);
    else
        executor_terminate(job->id);
    HANDLE th = CreateThread(NULL, 0, watcher_thread, wa, 0, NULL);
    if (!th) {
        executor_terminate(job->id);
        WaitForSingleObject(pi.hProcess, INFINITE);
        active_process_unregister(job->id);
        CloseHandle(pi.hProcess);
        if (wa->job_object) CloseHandle(wa->job_object);
        free(wa);
        return -1;
    }
    CloseHandle(th);
    return 0;
}

/*
 * Launcher thread: runs spawn_process off the scheduler thread so the
 * scheduler is never blocked by slow SSH/SCP setup operations.
 */
static DWORD WINAPI launcher_thread(LPVOID arg)
{
    LaunchArg *la = (LaunchArg *)arg;
    Job *job = la->job;
    free(la);
    if (executor_is_shutting_down() || job_is_terminal_in_db(job->id)) {
        alloc_release(job->id);
        job_free(job);
        inflight_decrement();
        return 0;
    }
    if (spawn_process(job) != 0) {
        if (!job_is_terminal_in_db(job->id)) {
            const char *r = job->status_reason[0]
                ? job->status_reason : "Failed to spawn process";
            job_set_status_r(job, JOB_STATUS_FAILED, r);
        }
        alloc_release(job->id);
        job_free(job);
        inflight_decrement();
        return 1;
    }
    return 0;
}

#else  /* POSIX */

#include <unistd.h>
#include <sys/wait.h>
#include <pthread.h>
#include <wordexp.h>
#include <fcntl.h>

typedef struct {
    Job   *job;
    pid_t  pid;
    int    is_remote;
    char   remote_host[256];
    char   remote_output[512];
    char   known_hosts_path[512];
} WatchArg;

typedef struct { Job *job; } LaunchArg;

static void *watcher_thread(void *arg)
{
    WatchArg *wa = (WatchArg *)arg;
    int wstatus = 0;
    waitpid(wa->pid, &wstatus, 0);
    int exit_code = WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : 1;
    active_process_unregister(wa->job->id);
    int already_terminal = job_is_terminal_in_db(wa->job->id);

    /* For remote jobs: retrieve output files after SSH exits */
    if (!already_terminal && wa->is_remote && g_config.ssh_user[0]) {
        char ssh_opts[512];
        build_ssh_opts(ssh_opts, sizeof(ssh_opts), wa->known_hosts_path);
        char scp_cmd[1024];
        snprintf(scp_cmd, sizeof(scp_cmd),
            "scp %s -r %s@%s:%s/. '%s'",
            ssh_opts, g_config.ssh_user, wa->remote_host,
            wa->remote_output, wa->job->output_dir);
        log_info("executor", "Retrieving output for job %s from %s",
                 wa->job->id, wa->remote_host);
        int scp_rc = system(scp_cmd);
        if (scp_rc != 0)
            log_warn("executor",
                "Output SCP returned %d for job %s (output dir may be empty)",
                scp_rc, wa->job->id);
    }

    if (!already_terminal) {
        wa->job->exit_code = exit_code;
        char reason[280];
        if (exit_code == 0) {
            job_set_status_r(wa->job, JOB_STATUS_FINISHED, "Completed successfully");
        } else if (wa->is_remote) {
            snprintf(reason, sizeof(reason), "Process exited with code %d on %s",
                     exit_code, wa->remote_host);
            job_set_status_r(wa->job, JOB_STATUS_FAILED, reason);
        } else {
            snprintf(reason, sizeof(reason), "Process exited with code %d", exit_code);
            job_set_status_r(wa->job, JOB_STATUS_FAILED, reason);
        }
    }

    if (artifact_collect_job(wa->job->id) < 0)
        log_warn("executor", "Artifact collection failed for job %s", wa->job->id);

    db_release_allocation(wa->job->id);
    alloc_release(wa->job->id);
    maybe_auto_deprovision(wa->job->machine_id);
    /* Clean up temp known_hosts file */
    if (wa->known_hosts_path[0]) remove(wa->known_hosts_path);
    job_free(wa->job);
    free(wa);
    inflight_decrement();
    return NULL;
}

static int spawn_process(Job *job)
{
    /* ── Determine target host ────────────────────────────────────── */
    char host[256];
    int local = machine_host(job->machine_id, host, sizeof(host));

    /* Force local execution when no SSH user is configured */
    if (!g_config.ssh_user[0]) local = 1;

    char stdout_path[512], stderr_path[512];
    store_stdout_path(job->id, stdout_path, sizeof(stdout_path));
    store_stderr_path(job->id, stderr_path, sizeof(stderr_path));

    if (local) {
        /* ══ LOCAL EXECUTION (fallback / exception) ═════════════════ */
        wordexp_t we;
        if (wordexp(job->command, &we, WRDE_NOCMD) != 0) {
            log_error("executor", "wordexp failed for: %s", job->command);
            return -1;
        }
        pid_t pid = fork();
        if (pid < 0) { log_error("executor", "fork() failed"); wordfree(&we); return -1; }
        if (pid == 0) {
            setpgid(0, 0);
            int fdo = open(stdout_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            int fde = open(stderr_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fdo >= 0) { dup2(fdo, STDOUT_FILENO); close(fdo); }
            if (fde >= 0) { dup2(fde, STDERR_FILENO); close(fde); }
            setenv("ORCH_JOB_ID",        job->id,         1);
            setenv("ORCH_INPUT_DIR",     job->input_dir,  1);
            setenv("ORCH_OUTPUT_DIR",    job->output_dir, 1);
            setenv("ORCH_WORKER_ID",     job->machine_id, 1);
            setenv("ORCH_MACHINE_IDS",   job->machine_id, 1);
            setenv("ORCH_MACHINE_COUNT", "1", 1);
            if (g_config.pre_job_script_linux[0]) {
                int pre_ret = system(g_config.pre_job_script_linux);
                if (pre_ret != 0) {
                    fprintf(stderr, "[executor] Pre-job script failed (exit %d)\n",
                            WEXITSTATUS(pre_ret));
                    _exit(126);
                }
            }
            execvp(we.we_wordv[0], we.we_wordv);
            _exit(127);
        }
        wordfree(&we);
        setpgid(pid, pid);
        if (active_process_register(job->id, pid) != 0) {
            kill(-pid, SIGTERM);
            waitpid(pid, NULL, 0);
            return -1;
        }
        WatchArg *wa = (WatchArg *)malloc(sizeof(WatchArg));
        if (!wa) {
            executor_terminate(job->id);
            waitpid(pid, NULL, 0);
            active_process_unregister(job->id);
            return -1;
        }
        wa->job = job; wa->pid = pid;
        wa->is_remote = 0;
        wa->remote_host[0]      = '\0';
        wa->remote_output[0]    = '\0';
        wa->known_hosts_path[0] = '\0';
        if (!job_is_terminal_in_db(job->id))
            job_set_status(job, JOB_STATUS_RUNNING);
        else
            executor_terminate(job->id);
        pthread_t th;
        if (pthread_create(&th, NULL, watcher_thread, wa) != 0) {
            executor_terminate(job->id);
            waitpid(pid, NULL, 0);
            active_process_unregister(job->id);
            free(wa);
            return -1;
        }
        pthread_detach(th);
        return 0;
    }

    /* ══ REMOTE EXECUTION via SSH (normal path) ══════════════════════ */
    char known_hosts_path[512];
    {
        const char *tmpdir = g_config.temp_dir[0] ? g_config.temp_dir : "/tmp";
        snprintf(known_hosts_path, sizeof(known_hosts_path),
                 "%s/orch-%s.kh", tmpdir, job->id);
    }
    { FILE *_kh = fopen(known_hosts_path, "ab"); if (_kh) fclose(_kh); }

    char ssh_opts[512];
    build_ssh_opts(ssh_opts, sizeof(ssh_opts), known_hosts_path);

    char remote_base[512], remote_input[512], remote_output[512];
    snprintf(remote_base,   sizeof(remote_base),
        "%s/%s",        g_config.ssh_remote_work_dir, job->id);
    snprintf(remote_input,  sizeof(remote_input),
        "%s/%s/input",  g_config.ssh_remote_work_dir, job->id);
    snprintf(remote_output, sizeof(remote_output),
        "%s/%s/output", g_config.ssh_remote_work_dir, job->id);

    log_info("executor", "Job %s → remote %s (%s)",
             job->id, job->machine_id, host);

#define RUN_SYNC_FAIL(label, ...) do { \
    char _c[1024]; snprintf(_c, sizeof(_c), __VA_ARGS__); \
    int _rc = system(_c); \
    if (_rc != 0) { log_error("executor", label " (exit %d)", _rc); return -1; } \
} while(0)
#define RUN_SYNC_WARN(label, ...) do { \
    char _c[1024]; snprintf(_c, sizeof(_c), __VA_ARGS__); \
    int _rc = system(_c); \
    if (_rc != 0) log_debug("executor", label " returned %d", _rc); \
} while(0)

    /* 1. Create remote directories */
    RUN_SYNC_FAIL("mkdir remote dirs",
        "ssh %s %s@%s \"mkdir -p '%s/input' '%s/output'\"",
        ssh_opts, g_config.ssh_user, host, remote_base, remote_base);

    /* 2. Copy input files to remote (best-effort; input dir may be empty) */
    RUN_SYNC_WARN("input SCP",
        "scp %s -r '%s/.' %s@%s:%s",
        ssh_opts, job->input_dir, g_config.ssh_user, host, remote_input);

    /* 3. Generate and upload run script */
    char script_local[512];
    if (write_run_script(job, remote_input, remote_output,
                         script_local, sizeof(script_local)) != 0)
        return -1;
    RUN_SYNC_FAIL("upload run script",
        "scp %s '%s' %s@%s:%s/.run.sh",
        ssh_opts, script_local, g_config.ssh_user, host, remote_base);

#undef RUN_SYNC_FAIL
#undef RUN_SYNC_WARN

    /* 4. Fork and exec SSH asynchronously — stdout/stderr piped to log files */
    const char *rshell = g_config.ssh_shell[0] ? g_config.ssh_shell : "sh";
    char ssh_cmd[1024];
    snprintf(ssh_cmd, sizeof(ssh_cmd),
        "ssh %s %s@%s \"%s '%s/.run.sh'\"",
        ssh_opts, g_config.ssh_user, host, rshell, remote_base);

    wordexp_t we;
    if (wordexp(ssh_cmd, &we, WRDE_NOCMD) != 0) {
        log_error("executor", "wordexp failed for SSH command: %s", ssh_cmd);
        return -1;
    }
    int fdo = open(stdout_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    int fde = open(stderr_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    pid_t pid = fork();
    if (pid < 0) {
        log_error("executor", "fork() failed for SSH");
        wordfree(&we);
        if (fdo >= 0) close(fdo);
        if (fde >= 0) close(fde);
        return -1;
    }
    if (pid == 0) {
        setpgid(0, 0);
        if (fdo >= 0) { dup2(fdo, STDOUT_FILENO); close(fdo); }
        if (fde >= 0) { dup2(fde, STDERR_FILENO); close(fde); }
        execvp(we.we_wordv[0], we.we_wordv);
        _exit(127);
    }
    setpgid(pid, pid);
    wordfree(&we);
    if (fdo >= 0) close(fdo);
    if (fde >= 0) close(fde);

    if (active_process_register(job->id, pid) != 0) {
        kill(-pid, SIGTERM);
        waitpid(pid, NULL, 0);
        return -1;
    }
    WatchArg *wa = (WatchArg *)malloc(sizeof(WatchArg));
    if (!wa) {
        executor_terminate(job->id);
        waitpid(pid, NULL, 0);
        active_process_unregister(job->id);
        return -1;
    }
    wa->job = job; wa->pid = pid;
    wa->is_remote = 1;
    strncpy(wa->remote_host,        host,             sizeof(wa->remote_host)      - 1);
    strncpy(wa->remote_output,      remote_output,    sizeof(wa->remote_output)    - 1);
    strncpy(wa->known_hosts_path,   known_hosts_path, sizeof(wa->known_hosts_path) - 1);
    if (!job_is_terminal_in_db(job->id))
        job_set_status(job, JOB_STATUS_RUNNING);
    else
        executor_terminate(job->id);
    pthread_t th;
    if (pthread_create(&th, NULL, watcher_thread, wa) != 0) {
        executor_terminate(job->id);
        waitpid(pid, NULL, 0);
        active_process_unregister(job->id);
        free(wa);
        return -1;
    }
    pthread_detach(th);
    return 0;
}

/* Launcher thread (POSIX): same purpose as Windows counterpart. */
static void *launcher_thread(void *arg)
{
    LaunchArg *la = (LaunchArg *)arg;
    Job *job = la->job;
    free(la);
    if (executor_is_shutting_down() || job_is_terminal_in_db(job->id)) {
        alloc_release(job->id);
        job_free(job);
        inflight_decrement();
        return NULL;
    }
    if (spawn_process(job) != 0) {
        if (!job_is_terminal_in_db(job->id)) {
            const char *r = job->status_reason[0]
                ? job->status_reason : "Failed to spawn process";
            job_set_status_r(job, JOB_STATUS_FAILED, r);
        }
        alloc_release(job->id);
        job_free(job);
        inflight_decrement();
        return NULL;
    }
    return NULL;
}

#endif  /* platform */

/* ── Public API ──────────────────────────────────────────────────── */
int executor_spawn(Job *job)
{
    if (executor_is_shutting_down()) {
        job_set_status_r(job, JOB_STATUS_FAILED, "Executor is shutting down");
        alloc_release(job->id);
        job_free(job);
        return -1;
    }
    store_init_job_dirs(job->id);
    store_input_dir (job->id, job->input_dir,  sizeof(job->input_dir));
    store_output_dir(job->id, job->output_dir, sizeof(job->output_dir));

    job_set_status(job, JOB_STATUS_STARTING);
    db_update_job_started(job->id, job->machine_id, time(NULL));
    job->started_at = time(NULL);

    /* Hand off to a launcher thread so the scheduler is never blocked
     * by SSH mkdir / SCP upload operations (which can each take seconds). */
    LaunchArg *la = (LaunchArg *)malloc(sizeof(LaunchArg));
    if (!la) {
        job_set_status_r(job, JOB_STATUS_FAILED, "Out of memory");
        alloc_release(job->id);
        job_free(job);
        return -1;
    }
    la->job = job;
    inflight_increment();

#ifdef _WIN32
    HANDLE th = CreateThread(NULL, 0, launcher_thread, la, 0, NULL);
    if (!th) {
        free(la);
        job_set_status_r(job, JOB_STATUS_FAILED, "Failed to create launcher thread");
        alloc_release(job->id);
        job_free(job);
        inflight_decrement();
        return -1;
    }
    CloseHandle(th);
#else
    pthread_t th;
    if (pthread_create(&th, NULL, launcher_thread, la) != 0) {
        free(la);
        job_set_status_r(job, JOB_STATUS_FAILED, "Failed to create launcher thread");
        alloc_release(job->id);
        job_free(job);
        inflight_decrement();
        return -1;
    }
    pthread_detach(th);
#endif
    return 0;
}
