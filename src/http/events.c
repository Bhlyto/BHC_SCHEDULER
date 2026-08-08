#include "events.h"
#include "db.h"
#include "cJSON.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * events.c
 * Lock-protected ring buffer.  events_push() is safe to call from any
 * thread; events_drain() must only be called from the HTTP poll thread.
 */

#ifdef _WIN32
#  include <windows.h>
static SRWLOCK s_cs = SRWLOCK_INIT;
#  define EVT_LOCK()    AcquireSRWLockExclusive(&s_cs)
#  define EVT_UNLOCK()  ReleaseSRWLockExclusive(&s_cs)
#else
#  include <pthread.h>
static pthread_mutex_t s_mu = PTHREAD_MUTEX_INITIALIZER;
#  define EVT_LOCK()    pthread_mutex_lock(&s_mu)
#  define EVT_UNLOCK()  pthread_mutex_unlock(&s_mu)
#endif

static EventMessage s_buf[EVENTS_BUF_SIZE];
static int  s_head  = 0;
static int  s_tail  = 0;
static int  s_count = 0;

void events_init(void)
{
}

void events_push(const char *json)
{
    events_push_user(json, "");
}

void events_push_user(const char *json, const char *user_id)
{
    if (!json) return;
    EVT_LOCK();
    strncpy(s_buf[s_head].json, json, EVENTS_JSON_MAX - 1);
    s_buf[s_head].json[EVENTS_JSON_MAX - 1] = '\0';
    strncpy(s_buf[s_head].user_id, user_id ? user_id : "",
            sizeof(s_buf[s_head].user_id) - 1);
    s_buf[s_head].user_id[sizeof(s_buf[s_head].user_id) - 1] = '\0';
    s_head = (s_head + 1) % EVENTS_BUF_SIZE;
    if (s_count < EVENTS_BUF_SIZE) {
        s_count++;
    } else {
        s_tail = (s_tail + 1) % EVENTS_BUF_SIZE;
    }
    EVT_UNLOCK();
}

int events_drain(EventMessage *out, int max)
{
    EVT_LOCK();
    int n = (s_count < max) ? s_count : max;
    for (int i = 0; i < n; i++) {
        out[i] = s_buf[s_tail];
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
    cJSON *event = cJSON_CreateObject();
    if (!event) return;
    cJSON_AddStringToObject(event, "category", category ? category : "");
    cJSON_AddStringToObject(event, "type", event_type ? event_type : "");
    cJSON_AddStringToObject(event, "detail", detail ? detail : "");
    cJSON_AddStringToObject(event, "user", user_id ? user_id : "");
    char *json = cJSON_PrintUnformatted(event);
    if (json) {
        events_push_user(json, user_id);
        free(json);
    }
    cJSON_Delete(event);
}
