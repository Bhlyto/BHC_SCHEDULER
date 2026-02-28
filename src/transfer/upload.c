#include "transfer.h"
#include "log.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/*
 * upload.c
 * Write raw request body to work_dir/<job_id>/input/<filename>.
 */

long upload_handle(const char *job_id, const char *filename,
                   const char *data, long data_len)
{
    char path[768];
    char input_dir[512];

    store_input_dir(job_id, input_dir, sizeof(input_dir));

    /* Sanitise filename: strip any path component */
    const char *base = filename;
    const char *p;
    for (p = filename; *p; p++)
        if (*p == '/' || *p == '\\') base = p + 1;
    if (!*base || strcmp(base, "..") == 0) {
        log_warn("upload", "Invalid filename: %s", filename);
        return -1;
    }

#ifdef _WIN32
    snprintf(path, sizeof(path), "%s\\%s", input_dir, base);
#else
    snprintf(path, sizeof(path), "%s/%s", input_dir, base);
#endif

    /* Ensure input dir exists */
    store_init_job_dirs(job_id);

    FILE *f = fopen(path, "wb");
    if (!f) {
        log_error("upload", "Cannot open %s for write", path);
        return -1;
    }
    long written = (long)fwrite(data, 1, data_len, f);
    fclose(f);

    log_info("upload", "Saved %ld bytes to %s (job %s)", written, path, job_id);
    return written;
}
