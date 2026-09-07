#include <stdio.h>
#include <string.h>

#include "log.h"

int main(void)
{
    FILE *out = tmpfile();
    if (!out) {
        fprintf(stderr, "tmpfile failed\n");
        return 1;
    }
    log_set_output(out);
    log_set_level(LOG_INFO);

    log_debug("this must not appear");
    log_info("greetings number %d", 7);
    log_warn("careful now");

    fflush(out);
    long size = ftell(out);
    rewind(out);
    char buf[512];
    if (size <= 0 || size >= (long)sizeof(buf)) {
        fclose(out);
        fprintf(stderr, "captured log has a weird size %ld\n", size);
        return 1;
    }
    if (fread(buf, 1, (size_t)size, out) != (size_t)size) {
        fclose(out);
        fprintf(stderr, "could not read back the log\n");
        return 1;
    }
    buf[size] = '\0';
    fclose(out);
    log_set_output(stderr);

    if (strstr(buf, "greetings number 7") == NULL) {
        fprintf(stderr, "info log missing: %s\n", buf);
        return 1;
    }
    if (strstr(buf, "careful now") == NULL) {
        fprintf(stderr, "warn log missing: %s\n", buf);
        return 1;
    }
    if (strstr(buf, "this must not appear") != NULL) {
        fprintf(stderr, "debug log leaked past LOG_INFO level: %s\n", buf);
        return 1;
    }

    printf("log ok\n");
    return 0;
}