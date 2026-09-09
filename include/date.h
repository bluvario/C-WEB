#ifndef CWEB_DATE_H
#define CWEB_DATE_H

#include <stddef.h>
#include <time.h>

#include "sv.h"

// formats t as an IMF-fixdate HTTP-date: "Sun, 06 Nov 1994 08:49:37 GMT".
// buf needs at least 30 bytes. returns buf.
char *http_date_rfc7231(time_t t, char *buf, size_t bufsize);

// the current moment as an HTTP-date
char *http_date_now(char *buf, size_t bufsize);

// formats t in Common Log Format local time: "10/Oct/2000:13:55:36 -0700"
// (no brackets; the access logger adds them). buf needs at least 27 bytes.
// returns buf.
char *http_date_clf(time_t t, char *buf, size_t bufsize);

// parses an IMF-fixdate HTTP-date into seconds since epoch. returns -1 when
// the text is not a valid date of exactly that shape.
time_t http_date_parse(String_View text);

// millisecond ticks on a monotonic clock, for durations and timers. never
// jumps backwards across NTP adjustments. windows uses GetTickCount64.
unsigned long long time_mono_ms(void);

#endif