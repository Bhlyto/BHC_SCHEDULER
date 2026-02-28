#ifndef PLATFORM_H
#define PLATFORM_H

void platform_service_start(int argc, char **argv);
void platform_request_stop(void);
int  platform_stop_requested(void);

#endif /* PLATFORM_H */
