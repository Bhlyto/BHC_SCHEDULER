#ifndef _WIN32
#include "platform.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>

void orchestrator_run(void);

static volatile int s_stop_requested = 0;

int platform_stop_requested(void) { return s_stop_requested; }
void platform_request_stop(void)  { s_stop_requested = 1; }

static void sig_handler(int sig)
{
    if (sig == SIGTERM || sig == SIGINT) {
        log_info("platform", "Signal %d received — stopping", sig);
        s_stop_requested = 1;
    }
}

static void write_pidfile(const char *path)
{
    FILE *f = fopen(path, "w");
    if (f) { fprintf(f, "%d\n", getpid()); fclose(f); }
}

static void daemonise(void)
{
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); exit(1); }
    if (pid > 0) exit(0);  /* parent exits */

    if (setsid() < 0) { perror("setsid"); exit(1); }

    /* second fork prevents reacquiring a terminal */
    pid = fork();
    if (pid < 0) exit(1);
    if (pid > 0) exit(0);

    umask(0);
    chdir("/");

    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) {
        dup2(devnull, STDIN_FILENO);
        dup2(devnull, STDOUT_FILENO);
        if (devnull > STDERR_FILENO) close(devnull);
    }
}

void platform_service_start(int argc, char **argv)
{
    int do_daemon = 0;
    const char *pidfile = "/var/run/orchestrator.pid";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--daemon") == 0) do_daemon = 1;
        if (strcmp(argv[i], "--pidfile") == 0 && i+1 < argc) pidfile = argv[++i];
    }

    if (do_daemon) daemonise();

    write_pidfile(pidfile);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sig_handler;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);
    signal(SIGHUP, SIG_IGN);

    log_info("platform", "Process started (pid=%d)", getpid());
    orchestrator_run();
}
#endif /* !_WIN32 */
