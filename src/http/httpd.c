#include "http.h"
#include "config.h"
#include "log.h"
#include "mongoose.h"
#include <stdio.h>
#include <string.h>

/*
 * httpd.c
 * Starts the Mongoose HTTP server in a background thread.
 */

static struct mg_mgr s_mgr;
static int           s_running = 0;

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
    while (s_running)
        mg_mgr_poll(&s_mgr, 100);   /* 100 ms poll timeout */

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
