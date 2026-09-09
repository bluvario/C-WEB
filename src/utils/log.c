#define _POSIX_C_SOURCE 200809L

#include "log.h"

#include <stdarg.h>
#include <time.h>

static Log_Level g_level = LOG_INFO;
static FILE *g_out = NULL; // NULL means stderr
static FILE *g_clf = NULL; // NULL means no access log

static const char *const g_level_names[] = {
    [LOG_DEBUG] = "DEBUG",
    [LOG_INFO] = "INFO",
    [LOG_WARN] = "WARN",
    [LOG_ERROR] = "ERROR",
};

void log_set_level(Log_Level level)
{
    g_level = level;
}

void log_set_output(FILE *out)
{
    g_out = out;
}

void log_log(Log_Level level, const char *fmt, ...)
{
    if (level < g_level) {
        return;
    }
    // TODO: this is a shared global, will need a mutex once the server
    // grows a thread pool.
    FILE *out = g_out ? g_out : stderr;

    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char stamp[32];
    if (tm && strftime(stamp, sizeof(stamp), "%H:%M:%S", tm) == 0) {
        stamp[0] = '\0';
    }

    fprintf(out, "%s [%s] ", stamp, g_level_names[level]);
    va_list args;
    va_start(args, fmt);
    vfprintf(out, fmt, args);
    va_end(args);
    fprintf(out, "\n");
}

void log_set_clf(FILE *fh)
{
    g_clf = fh;
}

void log_clf_line(const char *data, size_t len)
{
    if (g_clf == NULL) {
        return;
    }
#ifndef _WIN32
    flockfile(g_clf);
#endif
    fwrite(data, 1, len, g_clf);
    if (len == 0 || data[len - 1] != '\n') {
        fputc('\n', g_clf);
    }
    fflush(g_clf);
#ifndef _WIN32
    funlockfile(g_clf);
#endif
}