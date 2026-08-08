#include "http.h"
#include "config.h"
#include "mongoose.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void http_build_headers(char *out, int out_len, const char *content_type)
{
    int used = snprintf(out, (size_t)out_len,
        "Content-Type: %s\r\n"
        "X-Content-Type-Options: nosniff\r\n"
        "X-Frame-Options: DENY\r\n"
        "Referrer-Policy: no-referrer\r\n"
        "Content-Security-Policy: default-src 'self'; script-src 'self'; "
        "style-src 'self' 'unsafe-inline'; img-src 'self' data:; connect-src 'self'; "
        "object-src 'none'; base-uri 'none'; form-action 'self'; frame-ancestors 'none'\r\n",
        content_type ? content_type : "application/octet-stream");
    if (used < 0 || used >= out_len) return;

    if (g_config.require_https) {
        used += snprintf(out + used, (size_t)(out_len - used),
                         "Strict-Transport-Security: max-age=31536000; includeSubDomains\r\n");
        if (used < 0 || used >= out_len) return;
    }
    if (g_config.cors_allowed_origin[0] &&
        !strchr(g_config.cors_allowed_origin, '\r') &&
        !strchr(g_config.cors_allowed_origin, '\n')) {
        snprintf(out + used, (size_t)(out_len - used),
                 "Access-Control-Allow-Origin: %s\r\nVary: Origin\r\n",
                 g_config.cors_allowed_origin);
    }
}

void http_json_reply(struct mg_connection *c, int status_code,
                     const char *json_body)
{
    char headers[1536] = {0};
    http_build_headers(headers, sizeof(headers), "application/json; charset=utf-8");
    mg_http_reply(c, status_code, headers, "%s", json_body ? json_body : "null");
}

void http_error(struct mg_connection *c, int status_code, const char *msg)
{
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "error", msg);
    char *body = cJSON_PrintUnformatted(obj);
    http_json_reply(c, status_code, body);
    free(body);
    cJSON_Delete(obj);
}

int http_stream_file(struct mg_connection *c, const char *filepath)
{
    struct mg_http_serve_opts opts;
    memset(&opts, 0, sizeof(opts));
    (void)c; (void)filepath; (void)opts;
    return 0;
}
