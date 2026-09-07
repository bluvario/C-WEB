#include <stdio.h>
#include <string.h>

#include "date.h"

int main(void)
{
    char buf[32];

    http_date_rfc7231(0, buf, sizeof(buf));
    if (strcmp(buf, "Thu, 01 Jan 1970 00:00:00 GMT") != 0) {
        fprintf(stderr, "epoch date wrong: %s\n", buf);
        return 1;
    }

    // one day later is a Friday
    http_date_rfc7231(86400, buf, sizeof(buf));
    if (strcmp(buf, "Fri, 02 Jan 1970 00:00:00 GMT") != 0) {
        fprintf(stderr, "day-after date wrong: %s\n", buf);
        return 1;
    }

    // a real date, the current moment
    http_date_now(buf, sizeof(buf));
    if (strlen(buf) != 29 || strstr(buf, " GMT") == NULL || buf[3] != ',') {
        fprintf(stderr, "now date malformed: %s\n", buf);
        return 1;
    }
    // year lives at offset 12, assume the system clock is not from 1919
    int year = 0;
    for (int i = 0; i < 4; i++) {
        if (buf[12 + i] < '0' || buf[12 + i] > '9') {
            fprintf(stderr, "now date has no year: %s\n", buf);
            return 1;
        }
        year = year * 10 + (buf[12 + i] - '0');
    }
    if (year < 2020) {
        fprintf(stderr, "now date year looks wrong: %d (%s)\n", year, buf);
        return 1;
    }

    printf("date ok\n");
    return 0;
}