#ifndef TRANSFER_H
#define TRANSFER_H

#include "mongoose.h"

int  store_init_job_dirs(const char *job_id);
int  store_cleanup_job(const char *job_id);
int  store_reset_job_outputs(const char *job_id);
void store_input_dir(const char *job_id,  char *buf, int len);
void store_output_dir(const char *job_id, char *buf, int len);
void store_stdout_path(const char *job_id, char *buf, int len);
void store_stderr_path(const char *job_id, char *buf, int len);

/* Copy all files from parent_job output dir into child_job input dir */
int  store_forward_outputs(const char *parent_job_id, const char *child_job_id);

/* Filesystem artifact backend. Metadata is persisted separately from bytes so
   another storage backend can later provide URIs without changing scheduling. */
int artifact_register_file(const char *job_id, const char *type,
                           const char *path);
int artifact_collect_job(const char *job_id);

long upload_handle(const char *job_id, const char *filename,
                   const char *data, long data_len);
int  download_handle(struct mg_connection *c, struct mg_http_message *hm,
                     const char *job_id, const char *filename);

#endif /* TRANSFER_H */
