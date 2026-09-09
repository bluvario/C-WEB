#ifndef CWEB_LOG_H
#define CWEB_LOG_H

#include <stdio.h>

typedef enum {
    LOG_DEBUG = 0,
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR,
} Log_Level;

void log_set_level(Log_Level level);
// default output is stderr; point it elsewhere and it stays there
void log_set_output(FILE *out);

void log_log(Log_Level level, const char *fmt, ...);

// a second, dedicated sink for Common Log Format access lines (--log FILE).
// NULL (the default) makes log_clf_line a no-op. lines are written whole
// under the stream lock, so worker threads cannot interleave partial lines.
void log_set_clf(FILE *fh);
void log_clf_line(const char *data, size_t len);

#define log_debug(...) log_log(LOG_DEBUG, __VA_ARGS__)
#define log_info(...) log_log(LOG_INFO, __VA_ARGS__)
#define log_warn(...) log_log(LOG_WARN, __VA_ARGS__)
#define log_error(...) log_log(LOG_ERROR, __VA_ARGS__)

#endif