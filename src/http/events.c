#include "events.h"
#include "db.h"
#include <string.h>
#include <stdio.h>

/*
 * events.c
 * Lock-protected ring buffer.  events_push() is safe to call from any
 * thread; events_drain() must only be called from the HTTP poll thread.
 */

#ifdef _WIN32
#  include <windows.h>
static CRITICAL_SECTION s_cs;
static int              s_cs_init = 0;
#  define EVT_LOCK()    EnterCriticalSection(&s_cs)
#  define EVT_UNLOCK()  LeaveCriticalSection(&s_cs)
#else
#  include <pthread.h>
static pthread_mutex_t s_mu = PTHREAD_MUTEX_INITIALIZER;
#  define EVT_LOCK()    pthread_mutex_lock(&s_mu)
#  define EVT_UNLOCK()  pthread_mutex_unlock(&s_mu)
#endif

static char s_buf[EVENTS_BUF_SIZE][EVENTS_JSON_MAX];
static int  s_head  = 0;
static int  s_tail  = 0;
static int  s_count = 0;

void events_init(void)
{
#ifdef _WIN32
    if (!s_cs_init) {
        InitializeCriticalSection(&s_cs);
        s_cs_init = 1;
    }
#endif
}

void events_push(const char *json)
{
    EVT_LOCK();
    strncpy(s_buf[s_head], json, EVENTS_JSON_MAX - 1);
    s_buf[s_head][EVENTS_JSON_MAX - 1] = '\0';
    s_head = (s_head + 1) % EVENTS_BUF_SIZE;
    if (s_count < EVENTS_BUF_SIZE) {
        s_count++;
    } else {
        s_tail = (s_tail + 1) % EVENTS_BUF_SIZE;
    }
    EVT_UNLOCK();
}

int events_drain(char out[][EVENTS_JSON_MAX], int max)
{
    EVT_LOCK();
    int n = (s_count < max) ? s_count : max;
    for (int i = 0; i < n; i++) {
        strncpy(out[i], s_buf[s_tail], EVENTS_JSON_MAX);
        s_tail  = (s_tail + 1) % EVENTS_BUF_SIZE;
    }
    s_count -= n;
    EVT_UNLOCK();
    return n;
}

void events_push_persistent(const char *category, const char *event_type,
                            const char *detail, const char *user_id)
{
    /* Persist to DB */
    db_insert_event(category, event_type, detail, user_id, "", "");

    /* Also push to ring buffer for live SSE */
    char json[EVENTS_JSON_MAX];
    snprintf(json, sizeof(json),
        "{\"category\":\"%s\",\"type\":\"%s\",\"detail\":\"%s\",\"user\":\"%s\"}",
        category ? category : "",
        event_type ? event_type : "",
        detail ? detail : "",
        user_id ? user_id : "");
    events_push(json);
}
