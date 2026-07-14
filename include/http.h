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

/* Register an SSE subscriber with user identity for filtered events. */
void httpd_sse_add_user(struct mg_connection *c, const char *user_id, const char *role);

/* ── Auth ────────────────────────────────────── */

/* Extract X-API-Key header, SHA-256 hash it, look up in DB.
   Returns 1 if valid, 0 if missing/invalid. */
int auth_check(struct mg_connection *c, struct mg_http_message *hm);

/* Same as auth_check but also resolves the role ("admin" or "user").
   out_role must be >= 16 bytes. Returns 1 if valid. */
int auth_check_role(struct mg_connection *c, struct mg_http_message *hm,
                    char *out_role);

/* Same as auth_check_role but also resolves the user_id tied to the key.
   out_user_id must be >= 128 bytes. Returns 1 if valid. */
int auth_check_role_user(struct mg_connection *c, struct mg_http_message *hm,
                         char *out_role, char *out_user_id);

/* Hash a raw API key to its SHA-256 hex representation (65-byte buffer). */
void auth_hash_key(const char *raw_key, char *out_hex_65);

/* Hash a password with random salt → "hexsalt$hexhash" (97 chars + NUL). */
#define AUTH_PASSWORD_HASH_LEN 160
int auth_hash_password(const char *password, char *out_buf);

/* Verify a password against a stored hash (salted or legacy). Returns 1 on match. */
int  auth_verify_password(const char *password, const char *stored_hash);
int  auth_password_needs_rehash(const char *stored_hash);

void http_build_headers(char *out, int out_len, const char *content_type);

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
