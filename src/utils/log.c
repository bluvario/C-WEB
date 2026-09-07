#include "log.h"

#include <stdarg.h>
#include <time.h>

static Log_Level g_level = LOG_INFO;
static FILE *g_out = NULL; // NULL means stderr

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