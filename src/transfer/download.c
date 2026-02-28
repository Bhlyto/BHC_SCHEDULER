#include "transfer.h"
#include "log.h"
#include "mongoose.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>

/*
 * download.c
 * Open a file from work_dir/<job_id>/output/ and stream it via Mongoose.
 */

int download_handle(struct mg_connection *c,
                    struct mg_http_message *hm,
                    const char *job_id, const char *filename)
{
    /* Sanitise filename */
    const char *base = filename;
    const char *p;
    for (p = filename; *p; p++)
        if (*p == '/' || *p == '\\') base = p + 1;
    if (!*base || strcmp(base, "..") == 0) {
        log_warn("download", "Invalid filename: %s", filename);
        return -1;
    }

    char output_dir[512];
    store_output_dir(job_id, output_dir, sizeof(output_dir));

    char path[768];
#ifdef _WIN32
    snprintf(path, sizeof(path), "%s\\%s", output_dir, base);
#else
    snprintf(path, sizeof(path), "%s/%s", output_dir, base);
#endif

    struct stat st;
    if (stat(path, &st) != 0) {
        log_warn("download", "File not found: %s", path);
        return -1;
    }

    /* Send file using Mongoose built-in helper */
    struct mg_http_serve_opts opts;
    memset(&opts, 0, sizeof(opts));
    mg_http_serve_file(c, hm, path, &opts);

    log_info("download", "Serving %s (%ld bytes) for job %s",
             base, (long)st.st_size, job_id);
    return 0;
}
