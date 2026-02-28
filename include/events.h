#ifndef EVENTS_H
#define EVENTS_H

#define EVENTS_BUF_SIZE  64
#define EVENTS_JSON_MAX  512

void events_init(void);
void events_push(const char *json);
int  events_drain(char out[][EVENTS_JSON_MAX], int max);

#endif /* EVENTS_H */
