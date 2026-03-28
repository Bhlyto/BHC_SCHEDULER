#ifndef EVENTS_H
#define EVENTS_H

#define EVENTS_BUF_SIZE  64
#define EVENTS_JSON_MAX  512

void events_init(void);
void events_push(const char *json);
int  events_drain(char out[][EVENTS_JSON_MAX], int max);

/* Persistent event logging — stores to DB for reporting */
void events_push_persistent(const char *category, const char *event_type,
                            const char *detail, const char *user_id);

#endif /* EVENTS_H */
