#ifndef LOG_H
#define LOG_H

/* Set global minimum log level: "debug" | "info" | "warn" | "error" */
void log_set_level(const char *level_str);

void log_debug(const char *tag, const char *fmt, ...);
void log_info (const char *tag, const char *fmt, ...);
void log_warn (const char *tag, const char *fmt, ...);
void log_error(const char *tag, const char *fmt, ...);

#endif /* LOG_H */
