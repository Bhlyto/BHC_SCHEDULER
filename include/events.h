#ifndef EVENTS_H
#define EVENTS_H

#define EVENTS_BUF_SIZE  64
#define EVENTS_JSON_MAX  512

typedef struct {
    char json[EVENTS_JSON_MAX];
    char user_id[128];
} EventMessage;

void events_init(void);
void events_push(const char *json);
void events_push_user(const char *json, const char *user_id);
int  events_drain(EventMessage *out, int max);

/* Persistent event logging — stores to DB for reporting */
void events_push_persistent(const char *category, const char *event_type,
                            const char *detail, const char *user_id);

#endif /* EVENTS_H */
