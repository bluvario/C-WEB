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
    FILE *out = g_out ? g_out : stderr;

    // localtime is not reentrant (it returns a pointer to a shared struct
    // tm), and the server's workers now log concurrently, so format into our
    // own buffer on the stack
    time_t now = time(NULL);
    struct tm tm;
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    char stamp[32];
    if (strftime(stamp, sizeof(stamp), "%H:%M:%S", &tm) == 0) {
        stamp[0] = '\0';
    }

    // the whole line belongs to one lock so worker threads cannot stitch a
    // prefix from one request onto a message from another
#ifndef _WIN32
    flockfile(out);
#endif
    fprintf(out, "%s [%s] ", stamp, g_level_names[level]);
    va_list args;
    va_start(args, fmt);
    vfprintf(out, fmt, args);
    va_end(args);
    fprintf(out, "\n");
#ifndef _WIN32
    funlockfile(out);
#endif
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