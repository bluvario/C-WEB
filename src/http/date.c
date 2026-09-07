#define _POSIX_C_SOURCE 200809L

#include "date.h"

#include <stdio.h>

static const char *const wdays[] =
    {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
static const char *const months[] =
    {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
     "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

char *http_date_rfc7231(time_t t, char *buf, size_t bufsize)
{
    struct tm tm;
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    snprintf(buf, bufsize, "%s, %02d %s %d %02d:%02d:%02d GMT",
             wdays[tm.tm_wday], tm.tm_mday, months[tm.tm_mon],
             tm.tm_year + 1900, tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

char *http_date_now(char *buf, size_t bufsize)
{
    return http_date_rfc7231(time(NULL), buf, bufsize);
}