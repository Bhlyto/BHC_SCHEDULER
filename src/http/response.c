#include "http.h"
#include "mongoose.h"
#include <stdio.h>
#include <string.h>

/*
 * response.c
 * Helpers for sending JSON and error responses.
 */

void http_json_reply(struct mg_connection *c, int status_code,
                     const char *json_body)
{
    mg_http_reply(c, status_code,
                  "Content-Type: application/json\r\n"
                  "Access-Control-Allow-Origin: *\r\n",
                  "%s", json_body);
}

void http_error(struct mg_connection *c, int status_code, const char *msg)
{
    char body[256];
    snprintf(body, sizeof(body), "{\"error\":\"%s\"}", msg);
    http_json_reply(c, status_code, body);
}

int http_stream_file(struct mg_connection *c, const char *filepath)
{
    struct mg_http_serve_opts opts;
    memset(&opts, 0, sizeof(opts));
    /* We forward to mg_http_serve_file via the routes handler, so this is
       a convenience wrapper that sets no extra headers. */
    (void)c; (void)filepath; (void)opts;
    return 0; /* actual call happens in download.c via mg_http_serve_file */
}
