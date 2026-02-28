#include "http.h"
#include "config.h"
#include "log.h"
#include "mongoose.h"
#include "events.h"
#include <stdio.h>
#include <string.h>

/*
 * httpd.c
 * Starts the Mongoose HTTP server in a background thread.
 * Also manages SSE subscriber connections and broadcasts job events.
 */

static struct mg_mgr s_mgr;
static int           s_running = 0;

/* ── SSE subscriber list ─────────────────────────────────── */
#define SSE_MAX_CONNS 32
static struct mg_connection *s_sse[SSE_MAX_CONNS];
static int s_sse_count = 0;

void httpd_sse_add(struct mg_connection *c)
{
    if (s_sse_count < SSE_MAX_CONNS)
        s_sse[s_sse_count++] = c;
}

static void sse_broadcast(const char *json)
{
    for (int i = 0; i < s_sse_count; ) {
        if (s_sse[i]->is_closing || s_sse[i]->is_draining) {
            s_sse[i] = s_sse[--s_sse_count];  /* compact the list */
        } else {
            mg_printf(s_sse[i], "data: %s\n\n", json);
            i++;
        }
    }
}

static void sse_heartbeat(void)
{
    for (int i = 0; i < s_sse_count; ) {
        if (s_sse[i]->is_closing || s_sse[i]->is_draining) {
            s_sse[i] = s_sse[--s_sse_count];
        } else {
            mg_printf(s_sse[i], ": keepalive\n\n");
            i++;
        }
    }
}

#ifdef _WIN32
#  include <windows.h>
static DWORD WINAPI http_thread(LPVOID arg)
#else
#  include <pthread.h>
static void *http_thread(void *arg)
#endif
{
    (void)arg;
    char addr[64];
    snprintf(addr, sizeof(addr), "http://0.0.0.0:%d", g_config.listen_port);

    mg_mgr_init(&s_mgr);
    struct mg_connection *conn = mg_http_listen(&s_mgr, addr, routes_handler, NULL);
    if (!conn) {
        log_error("httpd", "Failed to bind %s", addr);
#ifdef _WIN32
        return 1;
#else
        return NULL;
#endif
    }

    log_info("httpd", "Listening on %s", addr);
    events_init();
    uint64_t hb_ts = mg_millis();
    while (s_running) {
        mg_mgr_poll(&s_mgr, 100);

        /* Drain and broadcast any job status events queued by other threads */
        char evts[EVENTS_BUF_SIZE][EVENTS_JSON_MAX];
        int n = events_drain(evts, EVENTS_BUF_SIZE);
        for (int i = 0; i < n; i++) sse_broadcast(evts[i]);

        /* Send a SSE keepalive comment every 15 s (survives proxies/firewalls) */
        if (mg_millis() - hb_ts > 15000) { sse_heartbeat(); hb_ts = mg_millis(); }
    }

    mg_mgr_free(&s_mgr);
    log_info("httpd", "HTTP server stopped");
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

int httpd_start(int port)
{
    (void)port; /* uses g_config.listen_port */
    s_running = 1;
#ifdef _WIN32
    HANDLE th = CreateThread(NULL, 0, http_thread, NULL, 0, NULL);
    if (!th) { log_error("httpd", "CreateThread failed"); return -1; }
    CloseHandle(th);
#else
    pthread_t th;
    if (pthread_create(&th, NULL, http_thread, NULL) != 0) {
        log_error("httpd", "pthread_create failed");
        return -1;
    }
    pthread_detach(th);
#endif
    return 0;
}

void httpd_stop(void)
{
    s_running = 0;
}
