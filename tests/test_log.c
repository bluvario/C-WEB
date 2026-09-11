#include <stdio.h>
#include <string.h>

#include "log.h"
#include "thread.h"

#define LOG_THREADS 8
#define LOG_LINES 1500

typedef struct {
    int id;
} Log_Job;

static void log_worker(void *arg)
{
    Log_Job *j = arg;
    for (int i = 0; i < LOG_LINES; i++) {
        log_info("worker %d line %d", j->id, i);
    }
}

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

    // the access-log sink only writes when configured, and then whole lines
    log_set_clf(NULL);
    log_clf_line("never written", 13);
    FILE *clf = tmpfile();
    if (!clf) {
        fprintf(stderr, "tmpfile failed for the access log\n");
        return 1;
    }
    log_set_clf(clf);
    const char *sample = "127.0.0.1 - - [01/Jan/1970:00:00:00 +0000] "
                         "\"GET /notes HTTP/1.1\" 200 42";
    log_clf_line(sample, strlen(sample));
    log_set_clf(NULL);
    rewind(clf);
    char line[160];
    if (fgets(line, sizeof line, clf) == NULL) {
        fclose(clf);
        fprintf(stderr, "could not read the access log back\n");
        return 1;
    }
    fclose(clf);
    if (strstr(line, "[01/Jan/1970:00:00:00 +0000]") == NULL ||
        strstr(line, "\"GET /notes HTTP/1.1\" 200 42") == NULL ||
        line[strlen(line) - 1] != '\n') {
        fprintf(stderr, "access log line malformed: %s\n", line);
        return 1;
    }

    // concurrent workers must produce whole, ordered lines: the stream lock
    // held by log_log means a line can never be a stitch of two requests
    {
        FILE *out = tmpfile();
        if (!out) {
            fprintf(stderr, "tmpfile failed for the threaded log\n");
            return 1;
        }
        log_set_output(out);

        Thread threads[LOG_THREADS];
        Log_Job jobs[LOG_THREADS];
        for (int i = 0; i < LOG_THREADS; i++) {
            jobs[i].id = i;
            if (thread_init(&threads[i], log_worker, &jobs[i]) != 0) {
                fprintf(stderr, "could not spawn log worker %d\n", i);
                return 1;
            }
        }
        for (int i = 0; i < LOG_THREADS; i++) {
            thread_join(&threads[i]);
        }

        fflush(out);
        rewind(out);
        int lines = 0;
        int last_seen[LOG_THREADS];
        int seen[LOG_THREADS];
        for (int i = 0; i < LOG_THREADS; i++) {
            last_seen[i] = -1;
            seen[i] = 0;
        }
        char buf[512];
        while (fgets(buf, sizeof buf, out) != NULL) {
            lines++;
            char *body = strstr(buf, "[INFO] ");
            if (body == NULL) {
                fprintf(stderr, "malformed log line: %s", buf);
                return 1;
            }
            int id = -1, n = -1;
            if (sscanf(body, "[INFO] worker %d line %d", &id, &n) != 2 ||
                id < 0 || id >= LOG_THREADS || n < 0 || n >= LOG_LINES) {
                fprintf(stderr, "log line stitched or malformed: %s", buf);
                return 1;
            }
            if (n <= last_seen[id]) {
                fprintf(stderr, "worker %d lines out of order: %s", id, buf);
                return 1;
            }
            last_seen[id] = n;
            seen[id]++;
        }
        fclose(out);
        log_set_output(stderr);

        if (lines != LOG_THREADS * LOG_LINES) {
            fprintf(stderr, "expected %d log lines, saw %d\n",
                    LOG_THREADS * LOG_LINES, lines);
            return 1;
        }
        for (int i = 0; i < LOG_THREADS; i++) {
            if (seen[i] != LOG_LINES) {
                fprintf(stderr, "worker %d produced %d of %d lines\n",
                        i, seen[i], LOG_LINES);
                return 1;
            }
        }
    }

    printf("log ok\n");
    return 0;
}