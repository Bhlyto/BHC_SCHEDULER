#include "transfer.h"
#include "log.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

/*
 * upload.c
 * Write raw request body to work_dir/<job_id>/input/<filename>.
 */

int transfer_valid_filename(const char *filename)
{
    if (!filename) return 0;
    size_t length = strlen(filename);
    if (length == 0 || length > 255 || filename[0] == '.') return 0;
    for (const unsigned char *p = (const unsigned char *)filename; *p; p++) {
        if (!isalnum(*p) && !strchr(" _-.()+@,=", *p)) return 0;
    }
    return 1;
}

long upload_handle(const char *job_id, const char *filename,
                   const char *data, long data_len)
{
    char path[768];
    char input_dir[512];

    store_input_dir(job_id, input_dir, sizeof(input_dir));

    if (!transfer_valid_filename(filename)) {
        log_warn("upload", "Invalid filename: %s", filename);
        return -1;
    }

#ifdef _WIN32
    snprintf(path, sizeof(path), "%s\\%s", input_dir, filename);
#else
    snprintf(path, sizeof(path), "%s/%s", input_dir, filename);
#endif

    /* Ensure input dir exists */
    store_init_job_dirs(job_id);

    FILE *f = fopen(path, "wb");
    if (!f) {
        log_error("upload", "Cannot open %s for write", path);
        return -1;
    }
    long written = (long)fwrite(data, 1, (size_t)data_len, f);
    int close_failed = fclose(f) != 0;
    if (written != data_len || close_failed) {
        log_error("upload", "Incomplete write to %s", path);
        return -1;
    }

    log_info("upload", "Saved %ld bytes to %s (job %s)", written, path, job_id);
    return written;
}
