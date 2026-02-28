#ifndef HTTP_H
#define HTTP_H

#include "mongoose.h"

/* ── Server lifecycle ────────────────────────── */

/* Start the HTTP listener on the given port.
   Runs in its own thread. Returns 0 on success. */
int  httpd_start(int port);

/* Signal the HTTP thread to stop. */
void httpd_stop(void);

/* Register a mongoose connection as an SSE subscriber.
   Must be called from the HTTP thread (inside a route handler). */
void httpd_sse_add(struct mg_connection *c);

/* ── Auth ────────────────────────────────────── */

/* Extract X-API-Key header, SHA-256 hash it, look up in DB.
   Returns 1 if valid, 0 if missing/invalid. */
int auth_check(struct mg_connection *c, struct mg_http_message *hm);

/* ── Response helpers ────────────────────────── */

/* Send a JSON body with given HTTP status code (e.g. 200, 201, 400, 401). */
void http_json_reply(struct mg_connection *c, int status_code,
                     const char *json_body);

/* Send a plain-text error response. */
void http_error(struct mg_connection *c, int status_code, const char *msg);

/* Stream a file from disk using chunked transfer encoding. */
int  http_stream_file(struct mg_connection *c, const char *filepath);

/* ── Route dispatcher ────────────────────────── */

/* Main Mongoose event callback — registered by httpd_start(). */
void routes_handler(struct mg_connection *c, int ev, void *ev_data);

#endif /* HTTP_H */
