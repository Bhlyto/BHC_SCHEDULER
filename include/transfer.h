#ifndef TRANSFER_H
#define TRANSFER_H

#include "mongoose.h"

int  store_init_job_dirs(const char *job_id);
int  store_cleanup_job(const char *job_id);
void store_input_dir(const char *job_id,  char *buf, int len);
void store_output_dir(const char *job_id, char *buf, int len);
void store_stdout_path(const char *job_id, char *buf, int len);
void store_stderr_path(const char *job_id, char *buf, int len);

long upload_handle(const char *job_id, const char *filename,
                   const char *data, long data_len);
int  download_handle(struct mg_connection *c, struct mg_http_message *hm,
                     const char *job_id, const char *filename);

#endif /* TRANSFER_H */
