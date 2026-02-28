#include "config.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>

/*
 * log.c
 * Structured logger with configurable level.
 */

typedef enum { LOG_DEBUG=0, LOG_INFO, LOG_WARN, LOG_ERROR } LogLevel;

static LogLevel s_level = LOG_INFO;

void log_set_level(const char *level_str)
{
    if      (strcmp(level_str, "debug") == 0) s_level = LOG_DEBUG;
    else if (strcmp(level_str, "warn")  == 0) s_level = LOG_WARN;
    else if (strcmp(level_str, "error") == 0) s_level = LOG_ERROR;
    else                                       s_level = LOG_INFO;
}

static void log_write(LogLevel level, const char *tag, const char *fmt, va_list ap)
{
    if (level < s_level) return;

    static const char *names[] = {"DEBUG","INFO","WARN","ERROR"};
    char ts[32];
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", t);

    fprintf(stderr, "[%s] [%s] [%s] ", ts, names[level], tag);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    fflush(stderr);
}

void log_debug(const char *tag, const char *fmt, ...) { va_list a; va_start(a,fmt); log_write(LOG_DEBUG,tag,fmt,a); va_end(a); }
void log_info (const char *tag, const char *fmt, ...) { va_list a; va_start(a,fmt); log_write(LOG_INFO, tag,fmt,a); va_end(a); }
void log_warn (const char *tag, const char *fmt, ...) { va_list a; va_start(a,fmt); log_write(LOG_WARN, tag,fmt,a); va_end(a); }
void log_error(const char *tag, const char *fmt, ...) { va_list a; va_start(a,fmt); log_write(LOG_ERROR,tag,fmt,a); va_end(a); }
