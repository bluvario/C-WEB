#define _POSIX_C_SOURCE 200809L

#include "date.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "sv.h"

static const char *const wdays[] =
    {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
static const char *const months[] =
    {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
     "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

static int month_index(const char *mon)
{
    for (int i = 0; i < 12; i++) {
        if (strncmp(mon, months[i], 3) == 0) {
            return i;
        }
    }
    return -1;
}

// days since epoch for the civil date, from Howard Hinnant's civil-from-days
// algorithm: exact, no timezone and no clock system calls involved
static long days_from_civil(int y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long)doe - 719468;
}

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

char *http_date_clf(time_t t, char *buf, size_t bufsize)
{
    struct tm tm;
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    // localtime gives the wall clock; mktime of those parts back into seconds
    // yields the local mean offset, which is what CLF wants signed as %z
    long off = (long)(mktime(&tm) - t);
    int oh = (int)(off / 3600);
    int om = (int)((off % 3600) / 60);
    if (oh < -12 || oh > 14) {
        oh = 0; // clock skew should not print garbage offsets
        om = 0;
    }
    snprintf(buf, bufsize, "%02d/%s/%04d:%02d:%02d:%02d %c%02d%02d",
             tm.tm_mday, months[tm.tm_mon], tm.tm_year + 1900,
             tm.tm_hour, tm.tm_min, tm.tm_sec,
             off < 0 ? '-' : '+', oh < 0 ? -oh : oh, om < 0 ? -om : om);
    return buf;
}

time_t http_date_parse(String_View text)
{
    // IMF-fixdate is the only form we emit: fixed 29 characters, so a format
    // string parse has no surprises waiting at the edges
    if (text.count != 29) {
        return -1;
    }
    // "Sun, 06 Nov 1994 08:49:37 GMT"
    if (text.data[3] != ',' || text.data[4] != ' ' ||
        text.data[7] != ' ' || text.data[11] != ' ' ||
        text.data[16] != ' ' || text.data[19] != ':' ||
        text.data[22] != ':' || text.data[25] != ' ' ||
        memcmp(text.data + 26, "GMT", 3) != 0) {
        return -1;
    }
    char mon[4];
    mon[0] = text.data[8];
    mon[1] = text.data[9];
    mon[2] = text.data[10];
    mon[3] = '\0';
    int month = month_index(mon);
    if (month < 0) {
        return -1;
    }

    int d = (text.data[5] - '0') * 10 + (text.data[6] - '0');
    int y = (text.data[12] - '0') * 1000 + (text.data[13] - '0') * 100 +
            (text.data[14] - '0') * 10 + (text.data[15] - '0');
    int h = (text.data[17] - '0') * 10 + (text.data[18] - '0');
    int mi = (text.data[20] - '0') * 10 + (text.data[21] - '0');
    int s = (text.data[23] - '0') * 10 + (text.data[24] - '0');

    if (d < 1 || d > 31 || y < 1970 || h > 23 || mi > 59 || s > 60) {
        return -1;
    }

    long secs = days_from_civil(y, (unsigned)month + 1, (unsigned)d) * 86400L;
    secs += (long)h * 3600 + (long)mi * 60 + s;
    return (time_t)secs;
}

unsigned long long time_mono_ms(void)
{
#ifdef _WIN32
    return (unsigned long long)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000 +
           (unsigned long long)ts.tv_nsec / 1000000;
#endif
}