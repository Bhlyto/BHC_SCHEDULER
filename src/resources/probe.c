#include "resources.h"
#include "log.h"

/*
 * probe.c
 * Optional: poll actual CPU/RAM usage from the OS and compare against
 * reserved values. Currently a stub — extend per-platform as needed.
 */

void probe_refresh(void)
{
    /* TODO: On Linux read /proc/meminfo and /proc/stat.
             On Windows use GlobalMemoryStatusEx() and GetSystemTimes().
       For now we rely purely on the reservation model in allocator.c. */
    log_debug("probe", "probe_refresh called (stub)");
}
