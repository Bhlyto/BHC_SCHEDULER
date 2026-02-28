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
    /* Build env block: inherit parent env + 3 custom vars */
    char env_block[4096];
    int  env_pos = 0;

#define ADD_ENV(k, v) do { \
    int n = snprintf(env_block + env_pos, sizeof(env_block) - env_pos, "%s=%s", k, v); \
    if (n > 0) env_pos += n + 1; \
} while(0)

    ADD_ENV("ORCH_JOB_ID",     job->id);
    ADD_ENV("ORCH_INPUT_DIR",  job->input_dir);
    ADD_ENV("ORCH_OUTPUT_DIR", job->output_dir);
    /* double-null terminate */
    env_block[env_pos++] = '\0';

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    if (!CreateProcessA(NULL, job->command, NULL, NULL, FALSE,
                        CREATE_NEW_CONSOLE | CREATE_UNICODE_ENVIRONMENT,
                        env_block, NULL, &si, &pi)) {
        log_error("executor", "CreateProcess failed: %lu", GetLastError());
        return -1;
    }
    CloseHandle(pi.hThread);

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
        /* Child */
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
