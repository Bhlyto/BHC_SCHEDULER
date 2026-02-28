#ifndef EVENTS_H
#define EVENTS_H

/*
 * events.h
 * Thread-safe ring buffer for SSE job events.
 *
 * events_push()  — called from any thread (scheduler, executor).
 * events_drain() — called from the HTTP thread only (inside mg_mgr_poll loop).
 */

#define EVENTS_BUF_SIZE  64    /* max queued events before oldest is dropped */
#define EVENTS_JSON_MAX  512   /* max length of one JSON event string        */

void events_init(void);

/* Push a JSON event string (will be sent as `data: <json>\n\n`). */
void events_push(const char *json);

/* Copy up to max pending events into out[][]. Returns count copied. */
int  events_drain(char out[][EVENTS_JSON_MAX], int max);

#endif /* EVENTS_H */
