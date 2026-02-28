#include "job.h"
#include "resources.h"
#include "transfer.h"
#include "db.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/*
 * executor.c
 * Spawn a child process for a job, inject env vars, and monitor its exit.
 * Calls job_set_status(RUNNING / FINISHED / FAILED) and alloc_release().
 */

/* ── Platform-specific process spawning ─────────────────────────── */

#ifdef _WIN32
#include <windows.h>

typedef struct {
    Job    *job;
    HANDLE  proc_handle;
} WatchArg;

static DWORD WINAPI watcher_thread(LPVOID arg)
{
    WatchArg *wa = (WatchArg *)arg;
    WaitForSingleObject(wa->proc_handle, INFINITE);
    DWORD exit_code = 1;
    GetExitCodeProcess(wa->proc_handle, &exit_code);
    CloseHandle(wa->proc_handle);

    wa->job->exit_code = (int)exit_code;
    if (exit_code == 0)
        job_set_status(wa->job, JOB_STATUS_FINISHED);
    else
        job_set_status(wa->job, JOB_STATUS_FAILED);

    alloc_release(wa->job->id);
    free(wa);
    return 0;
}

static int spawn_process(Job *job)
{
    /* Build env block: inherit parent environment + 3 custom vars.
       GetEnvironmentStrings() returns a doubly-null-terminated block of
       "KEY=VALUE\0KEY=VALUE\0\0". We copy it, then append our vars. */
    LPCH parent_env = GetEnvironmentStrings();
    /* Measure parent block size */
    size_t parent_sz = 0;
    if (parent_env) {
        const char *p = parent_env;
        while (*p) { size_t l = strlen(p) + 1; parent_sz += l; p += l; }
    }
    /* 3 extra vars + generous padding */
    size_t extra_sz = strlen("ORCH_JOB_ID=") + strlen(job->id) + 1
                    + strlen("ORCH_INPUT_DIR=") + strlen(job->input_dir) + 1
                    + strlen("ORCH_OUTPUT_DIR=") + strlen(job->output_dir) + 1
                    + 2;  /* final double-null */
    char *env_block = (char *)malloc(parent_sz + extra_sz);
    int   env_pos   = 0;

    if (parent_env && parent_sz > 0) {
        memcpy(env_block, parent_env, parent_sz);
        env_pos = (int)parent_sz;
    }
    if (parent_env) FreeEnvironmentStrings(parent_env);

#define ADD_ENV(k, v) do { \
    int n = snprintf(env_block + env_pos, extra_sz, "%s=%s", k, v); \
    if (n > 0) env_pos += n + 1; \
} while(0)

    ADD_ENV("ORCH_JOB_ID",     job->id);
    ADD_ENV("ORCH_INPUT_DIR",  job->input_dir);
    ADD_ENV("ORCH_OUTPUT_DIR", job->output_dir);
    /* double-null terminate */
    env_block[env_pos++] = '\0';

    /* Open log files for stdout and stderr */
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

    /* Always run via cmd /c so that built-in commands (echo, dir, set…)
       and shell features (pipes, redirections) work correctly. */
    char wrapped[JOB_CMD_LEN + 10];
    _snprintf(wrapped, sizeof(wrapped), "cmd /c %s", job->command);

    if (!CreateProcessA(NULL, wrapped, NULL, NULL, TRUE,
                        CREATE_NO_WINDOW,
                        env_block, NULL, &si, &pi)) {
        log_error("executor", "CreateProcess failed: %lu", GetLastError());
        if (hStdout != INVALID_HANDLE_VALUE) CloseHandle(hStdout);
        if (hStderr != INVALID_HANDLE_VALUE) CloseHandle(hStderr);
        free(env_block);
        return -1;
    }
    if (hStdout != INVALID_HANDLE_VALUE) CloseHandle(hStdout);
    if (hStderr != INVALID_HANDLE_VALUE) CloseHandle(hStderr);
    CloseHandle(pi.hThread);
    free(env_block);

    log_info("executor", "Job %s logs: %s / %s", job->id, stdout_path, stderr_path);

    WatchArg *wa = (WatchArg *)malloc(sizeof(WatchArg));
    wa->job = job;
    wa->proc_handle = pi.hProcess;
    HANDLE th = CreateThread(NULL, 0, watcher_thread, wa, 0, NULL);
    if (th) CloseHandle(th);
    return 0;
}

#else  /* POSIX */

#include <unistd.h>
#include <sys/wait.h>
#include <pthread.h>
#include <wordexp.h>
#include <fcntl.h>

typedef struct {
    Job *job;
    pid_t pid;
} WatchArg;

static void *watcher_thread(void *arg)
{
    WatchArg *wa = (WatchArg *)arg;
    int wstatus = 0;
    waitpid(wa->pid, &wstatus, 0);
    int exit_code = WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : 1;
    wa->job->exit_code = exit_code;

    if (exit_code == 0)
        job_set_status(wa->job, JOB_STATUS_FINISHED);
    else
        job_set_status(wa->job, JOB_STATUS_FAILED);

    alloc_release(wa->job->id);
    free(wa);
    return NULL;
}

static int spawn_process(Job *job)
{
    /* Split command string into argv using wordexp */
    wordexp_t we;
    if (wordexp(job->command, &we, WRDE_NOCMD) != 0) {
        log_error("executor", "wordexp failed for: %s", job->command);
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        log_error("executor", "fork() failed");
        wordfree(&we);
        return -1;
    }
    if (pid == 0) {
        /* Child: redirect stdout and stderr to log files */
        char stdout_path[512], stderr_path[512];
        store_stdout_path(job->id, stdout_path, sizeof(stdout_path));
        store_stderr_path(job->id, stderr_path, sizeof(stderr_path));
        int fdo = open(stdout_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        int fde = open(stderr_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fdo >= 0) { dup2(fdo, STDOUT_FILENO); close(fdo); }
        if (fde >= 0) { dup2(fde, STDERR_FILENO); close(fde); }
        setenv("ORCH_JOB_ID",     job->id,         1);
        setenv("ORCH_INPUT_DIR",  job->input_dir,  1);
        setenv("ORCH_OUTPUT_DIR", job->output_dir, 1);
        execvp(we.we_wordv[0], we.we_wordv);
        _exit(127);
    }

    wordfree(&we);

    /* Parent: start watcher thread */
    WatchArg *wa = (WatchArg *)malloc(sizeof(WatchArg));
    wa->job = job;
    wa->pid = pid;
    pthread_t th;
    pthread_create(&th, NULL, watcher_thread, wa);
    pthread_detach(th);
    return 0;
}

#endif /* platform */

/* ── Public API ──────────────────────────────────────────────────── */
int executor_spawn(Job *job)
{
    /* Ensure I/O directories exist */
    store_init_job_dirs(job->id);
    store_input_dir (job->id, job->input_dir,  sizeof(job->input_dir));
    store_output_dir(job->id, job->output_dir, sizeof(job->output_dir));

    job_set_status(job, JOB_STATUS_STARTING);
    db_update_job_started(job->id, job->machine_id, time(NULL));
    job->started_at = time(NULL);

    if (spawn_process(job) != 0) {
        job_set_status(job, JOB_STATUS_FAILED);
        alloc_release(job->id);
        return -1;
    }

    job_set_status(job, JOB_STATUS_RUNNING);
    return 0;
}
