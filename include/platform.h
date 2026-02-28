#ifndef PLATFORM_H
#define PLATFORM_H

/*
 * platform.h
 * Abstracts Windows Service / Linux daemon lifecycle.
 */

/* Called from main() — registers with OS service manager (if needed),
   then calls orchestrator_run() which blocks until stop is requested. */
void platform_service_start(int argc, char **argv);

/* Signal the service loop to exit cleanly (called from signal handler or
   Windows SERVICE_CONTROL_STOP handler). */
void platform_request_stop(void);

/* Returns 1 after platform_request_stop() has been called. */
int  platform_stop_requested(void);

#endif /* PLATFORM_H */
