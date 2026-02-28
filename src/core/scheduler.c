#include "queue.h"
#include "job.h"
#include "resources.h"
#include "config.h"
#include "log.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

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


#ifdef _WIN32
static DWORD WINAPI scheduler_thread(LPVOID arg)
#else
#include <pthread.h>
static void *scheduler_thread(void *arg)
#endif
{
    (void)arg;
    log_info("scheduler", "Scheduler started (poll=%dms)", g_config.scheduler_poll_ms);

    while (s_running) {
        sleep_ms(g_config.scheduler_poll_ms);

        Job *job = queue_try_pop(s_queue);
        if (!job) continue;

        int can_single = alloc_can_fit(job->req_cores, job->req_gpu,
                                        job->req_ram_mb, job->req_disk_mb);
        int can_multi  = !can_single
                       ? alloc_can_fit_multi(job->req_cores, job->req_gpu,
                                             job->req_ram_mb, job->req_disk_mb)
                       : 0;

        if (!can_single && !can_multi) {
            queue_push(s_queue, job);
            log_debug("scheduler", "No resources for job %s, re-queued", job->id);
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
