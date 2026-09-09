#define _POSIX_C_SOURCE 200809L

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "net.h"

#define TOOL "./build/cweb"

// the notes example, compiled with the CLI and exercised over the wire: the
// layout frames every page, the --db store persists pinned notes across
// requests and a server restart, note text is HTML-escaped, --secure stamps
// hardening headers, and without --db the page says so.

static void nap(void)
{
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 20000000};
    nanosleep(&ts, NULL);
}

static int wait_for_port(int port)
{
    for (int i = 0; i < 100; i++) {
        Socket_Handle s = net_connect("127.0.0.1", port);
        if (s != -1) {
            net_close(s);
            return 0;
        }
        nap();
    }
    return -1;
}

static char *request(int port, const char *method, const char *path,
                     const char *body)
{
    Socket_Handle s = net_connect("127.0.0.1", port);
    if (s == -1) {
        return NULL;
    }
    char req[4096];
    int n = snprintf(req, sizeof req, "%s %s HTTP/1.1\r\n"
                     "Host: 127.0.0.1\r\n"
                     "Connection: close\r\n",
                     method, path);
    if (body != NULL) {
        n += snprintf(req + n, sizeof req - (size_t)n,
                      "Content-Type: application/x-www-form-urlencoded\r\n"
                      "Content-Length: %zu\r\n",
                      strlen(body));
    }
    n += snprintf(req + n, sizeof req - (size_t)n, "\r\n");
    if (body != NULL) {
        memcpy(req + n, body, strlen(body));
        n += (int)strlen(body);
    }
    if (net_send_all(s, req, (long)n) != n) {
        net_close(s);
        return NULL;
    }
    size_t cap = 8192, got = 0;
    char *buf = malloc(cap);
    long r;
    while (got < cap - 1 && (r = net_recv(s, buf + got, cap - got - 1)) > 0) {
        got += (size_t)r;
    }
    buf[got] = '\0';
    net_close(s);
    return buf;
}

int main(void)
{
    if (net_init() != 0) {
        return 1;
    }

    char tmp[] = "/tmp/cweb_notes_test_XXXXXX";
    if (mkdtemp(tmp) == NULL) {
        perror("mkdtemp");
        return 1;
    }

    // compile the example with the CLI, mounting its static dir
    char cmd[1024];
    snprintf(cmd, sizeof cmd,
             "%s build examples/notes/views %s . examples/notes/static >/dev/null",
             TOOL, tmp);
    if (system(cmd) != 0) {
        fprintf(stderr, "cweb build failed for the notes example\n");
        return 1;
    }

    Socket_Handle probe = net_listen(0);
    int port = net_bound_port(probe);
    net_close(probe);

    char db_path[512], server_path[512], port_arg[16];
    snprintf(db_path, sizeof db_path, "%s/notes.db", tmp);
    snprintf(server_path, sizeof server_path, "%s/server", tmp);
    snprintf(port_arg, sizeof port_arg, "%d", port);

    pid_t child = fork();
    if (child == 0) {
        execl(server_path, "server", "--port", port_arg, "--db", db_path,
              "--secure", NULL);
        _exit(127);
    }
    if (wait_for_port(port) != 0) {
        fprintf(stderr, "server did not come up on port %d\n", port);
        return 1;
    }

    int ok = 1;
    char *resp;

    // the layout wraps the empty board: shell, form, footer, stylesheet link
    resp = request(port, "GET", "/", NULL);
    ok = ok && resp != NULL && strstr(resp, "HTTP/1.1 200 OK") != NULL &&
         strstr(resp, "<header><h1>cweb notes</h1>") != NULL &&
         strstr(resp, "you have 0 notes") != NULL &&
         strstr(resp, "<form method=\"post\" action=\"/\">") != NULL &&
         strstr(resp, "Powered by C-WEB</footer>") != NULL &&
         strstr(resp, "/style.css") != NULL &&
         strstr(resp, "X-Content-Type-Options: nosniff") != NULL;
    if (!ok) {
        fprintf(stderr, "board should render inside the layout:\n%s\n", resp ? resp : "(connect error)");
    }
    free(resp);

    resp = request(port, "GET", "/style.css", NULL);
    ok = ok && resp != NULL && strstr(resp, "HTTP/1.1 200 OK") != NULL &&
         strstr(resp, "text/css") != NULL && strstr(resp, "system-ui") != NULL;
    if (!ok) {
        fprintf(stderr, "stylesheet should come from the static mount:\n%s\n", resp ? resp : "(connect error)");
    }
    free(resp);

    // the 404 page is wrapped by the layout too
    resp = request(port, "GET", "/nowhere", NULL);
    ok = ok && resp != NULL && strstr(resp, "HTTP/1.1 404") != NULL &&
         strstr(resp, "no such note") != NULL &&
         strstr(resp, "Powered by C-WEB</footer>") != NULL;
    if (!ok) {
        fprintf(stderr, "unknown paths should render the wrapped 404 page:\n%s\n", resp ? resp : "(connect error)");
    }
    free(resp);

    // pin a note: redirect home, then the board shows it, counted
    resp = request(port, "POST", "/", "note=first");
    ok = ok && resp != NULL && strstr(resp, "HTTP/1.1 303") != NULL &&
         strstr(resp, "Location: /") != NULL;
    if (!ok) {
        fprintf(stderr, "pinning a note should redirect home:\n%s\n", resp ? resp : "(connect error)");
    }
    free(resp);

    resp = request(port, "GET", "/", NULL);
    ok = ok && resp != NULL && strstr(resp, "HTTP/1.1 200 OK") != NULL &&
         strstr(resp, "you have 1 notes") != NULL &&
         strstr(resp, "<p class=\"note\">first</p>") != NULL;
    if (!ok) {
        fprintf(stderr, "pinned note should appear on the board:\n%s\n", resp ? resp : "(connect error)");
    }
    free(resp);

    // note text is escaped, so an injected script stays inert text
    resp = request(port, "POST", "/", "note=%3Cscript%3Ealert(1)%3C/script%3E");
    ok = ok && resp != NULL && strstr(resp, "HTTP/1.1 303") != NULL;
    if (!ok) {
        fprintf(stderr, "pinning a script should redirect home:\n%s\n", resp ? resp : "(connect error)");
    }
    free(resp);

    resp = request(port, "GET", "/", NULL);
    ok = ok && resp != NULL && strstr(resp, "you have 2 notes") != NULL &&
         strstr(resp, "&lt;script&gt;alert(1)&lt;/script&gt;") != NULL &&
         strstr(resp, "<script>alert(1)</script>") == NULL;
    if (!ok) {
        fprintf(stderr, "note text must be HTML-escaped:\n%s\n", resp ? resp : "(connect error)");
    }
    free(resp);

    // restart against the same db file: notes survive, in order
    kill(child, SIGTERM);
    waitpid(child, NULL, 0);
    child = fork();
    if (child == 0) {
        execl(server_path, "server", "--port", port_arg, "--db", db_path,
              "--secure", NULL);
        _exit(127);
    }
    if (wait_for_port(port) != 0) {
        fprintf(stderr, "server did not come back up on port %d\n", port);
        return 1;
    }

    resp = request(port, "GET", "/", NULL);
    ok = ok && resp != NULL && strstr(resp, "HTTP/1.1 200 OK") != NULL &&
         strstr(resp, "you have 2 notes") != NULL &&
         strstr(resp, "<p class=\"note\">first</p>") != NULL &&
         strstr(resp, "<p class=\"note\">&lt;script&gt;alert(1)&lt;/script&gt;</p>") != NULL;
    if (!ok) {
        fprintf(stderr, "notes should survive a server restart:\n%s\n", resp ? resp : "(connect error)");
    }
    free(resp);

    kill(child, SIGTERM);
    waitpid(child, NULL, 0);

    // without --db the accessor is NULL and the page says so
    child = fork();
    if (child == 0) {
        execl(server_path, "server", "--port", port_arg, NULL);
        _exit(127);
    }
    if (wait_for_port(port) != 0) {
        fprintf(stderr, "db-less server did not come up on port %d\n", port);
        return 1;
    }
    resp = request(port, "GET", "/", NULL);
    ok = ok && resp != NULL && strstr(resp, "HTTP/1.1 200 OK") != NULL &&
         strstr(resp, "--db notes.db</code>") != NULL &&
         strstr(resp, "you have ") == NULL;
    if (!ok) {
        fprintf(stderr, "without --db the page should explain how to enable it:\n%s\n", resp ? resp : "(connect error)");
    }
    free(resp);
    kill(child, SIGTERM);
    waitpid(child, NULL, 0);

    net_cleanup();

    char cleanup[512];
    snprintf(cleanup, sizeof cleanup, "rm -rf %s", tmp);
    system(cleanup);

    if (!ok) {
        return 1;
    }
    printf("notes example ok\n");
    return 0;
}