#ifndef CWEB_DATE_H
#define CWEB_DATE_H

#include <stddef.h>
#include <time.h>

// formats t as an IMF-fixdate HTTP-date: "Sun, 06 Nov 1994 08:49:37 GMT".
// buf needs at least 30 bytes. returns buf.
char *http_date_rfc7231(time_t t, char *buf, size_t bufsize);

// the current moment as an HTTP-date
char *http_date_now(char *buf, size_t bufsize);

#endif