#ifndef TRANSFER_H
#define TRANSFER_H

/*
 * transfer.h
 * File transfer layer: input upload / output download.
 */

/* Create work_dir/<job_id>/input/ and work_dir/<job_id>/output/.
   Returns 0 on success, -1 on error. */
int  store_init_job_dirs(const char *job_id);

/* Delete the entire work_dir/<job_id>/ tree (called after TTL expires). */
int  store_cleanup_job(const char *job_id);

/* Full path to the input directory for a job (caller-supplied buf, len). */
void store_input_dir(const char *job_id,  char *buf, int len);

/* Full path to the output directory for a job (caller-supplied buf, len). */
void store_output_dir(const char *job_id, char *buf, int len);

/* Full paths to the captured stdout/stderr log files. */
void store_stdout_path(const char *job_id, char *buf, int len);
void store_stderr_path(const char *job_id, char *buf, int len);

/* ── Upload ──────────────────────────────────── */

/* Write raw body from an HTTP message into input_dir/<filename>.
   Returns bytes written, or -1 on error. */
long upload_handle(const char *job_id, const char *filename,
                   const char *data, long data_len);

/* ── Download ────────────────────────────────── */

/* Open job's output/<filename> and stream it via the Mongoose connection.
   Returns 0 on success, -1 if file not found. */
int  download_handle(struct mg_connection *c,
                     struct mg_http_message *hm,
                     const char *job_id, const char *filename);

#endif /* TRANSFER_H */
