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

// the login example, compiled with the cweb CLI and exercised over the wire:
// bad login stays on the form, good login issues a session cookie and lands
// on the home page, logout clears it.

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

// one request per connection; *cookie and *body may be NULL. returns a heap
// copy of the whole response.
static char *request(int port, const char *method, const char *path,
                     const char *cookie, const char *body)
{
    Socket_Handle s = net_connect("127.0.0.1", port);
    if (s == -1) {
        return NULL;
    }
    char req[2048];
    int n = snprintf(req, sizeof req, "%s %s HTTP/1.1\r\n"
                     "Host: 127.0.0.1\r\n"
                     "Connection: close\r\n",
                     method, path);
    if (cookie != NULL) {
        n += snprintf(req + n, sizeof req - (size_t)n, "Cookie: %s\r\n", cookie);
    }
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

// pulls "cweb_session=<token>" out of a response's Set-Cookie header
static void grab_session_cookie(const char *resp, char *out, size_t out_size)
{
    const char *mark = strstr(resp, "Set-Cookie: cweb_session=");
    if (mark == NULL) {
        out[0] = '\0';
        return;
    }
    mark += strlen("Set-Cookie: cweb_session=");
    size_t n = 0;
    while (mark[n] != '\0' && mark[n] != ';' && mark[n] != '\r' &&
           n + 1 < out_size) {
        out[n] = mark[n];
        n++;
    }
    out[n] = '\0';
}

int main(void)
{
    if (net_init() != 0) {
        return 1;
    }

    char tmp[] = "/tmp/cweb_login_test_XXXXXX";
    if (mkdtemp(tmp) == NULL) {
        perror("mkdtemp");
        return 1;
    }

    // compile the example with the CLI, mounting its static dir
    char cmd[1024];
    snprintf(cmd, sizeof cmd, "%s build examples/login/views %s . examples/login/static >/dev/null", TOOL, tmp);
    if (system(cmd) != 0) {
        fprintf(stderr, "cweb build failed for the login example\n");
        return 1;
    }

    // reserve a port, then hand it to the freshly linked server
    Socket_Handle probe = net_listen(0);
    int port = net_bound_port(probe);
    net_close(probe);

    char server_path[512], port_arg[16];
    snprintf(server_path, sizeof server_path, "%s/server", tmp);
    snprintf(port_arg, sizeof port_arg, "%d", port);
    pid_t child = fork();
    if (child == 0) {
        execl(server_path, "server", "--port", port_arg, NULL);
        _exit(127);
    }
    if (wait_for_port(port) != 0) {
        fprintf(stderr, "server did not come up on port %d\n", port);
        return 1;
    }

    int ok = 1;
    char *resp;

    resp = request(port, "GET", "/login", NULL, NULL);
    ok = ok && resp != NULL && strstr(resp, "HTTP/1.1 200 OK") != NULL &&
         strstr(resp, "<form") != NULL && strstr(resp, "admin / secret") != NULL &&
         strstr(resp, "/style.css") != NULL;
    if (!ok) {
        fprintf(stderr, "login form fetch failed:\n%s\n", resp ? resp : "(connect error)");
    }
    free(resp);

    resp = request(port, "GET", "/style.css", NULL, NULL);
    ok = ok && resp != NULL && strstr(resp, "HTTP/1.1 200 OK") != NULL &&
         strstr(resp, "text/css") != NULL && strstr(resp, "system-ui") != NULL;
    if (!ok) {
        fprintf(stderr, "stylesheet should be served from the static mount:\n%s\n", resp ? resp : "(connect error)");
    }
    free(resp);

    resp = request(port, "POST", "/login", NULL, "username=admin&password=nope");
    ok = ok && resp != NULL && strstr(resp, "HTTP/1.1 200 OK") != NULL &&
         strstr(resp, "wrong") != NULL;
    if (!ok) {
        fprintf(stderr, "bad login should restay on the form:\n%s\n", resp ? resp : "(connect error)");
    }
    free(resp);

    resp = request(port, "POST", "/login", NULL, "username=admin&password=secret");
    ok = ok && resp != NULL && strstr(resp, "HTTP/1.1 303") != NULL &&
         strstr(resp, "Location: /") != NULL &&
         strstr(resp, "Set-Cookie: cweb_session=") != NULL &&
         strstr(resp, "Content-Type: text/html") != NULL &&
         strstr(resp, "text/plain") == NULL &&
         strstr(resp, "redirecting to") == NULL;
    if (!ok) {
        fprintf(stderr, "good login should redirect with a session cookie:\n%s\n", resp ? resp : "(connect error)");
        return 1;
    }
    char cookie[128];
    grab_session_cookie(resp, cookie, sizeof cookie);
    free(resp);
    if (cookie[0] == '\0') {
        fprintf(stderr, "no session cookie to reuse\n");
        return 1;
    }

    char cookie_header[160];
    snprintf(cookie_header, sizeof cookie_header, "cweb_session=%s", cookie);

    resp = request(port, "GET", "/", cookie_header, NULL);
    ok = ok && resp != NULL && strstr(resp, "HTTP/1.1 200 OK") != NULL &&
         strstr(resp, "Hi admin") != NULL && strstr(resp, "Log out") != NULL;
    if (!ok) {
        fprintf(stderr, "home page with session should greet the user:\n%s\n", resp ? resp : "(connect error)");
    }
    free(resp);

    resp = request(port, "POST", "/logout", cookie_header, "");
    ok = ok && resp != NULL && strstr(resp, "HTTP/1.1 303") != NULL &&
         strstr(resp, "Location: /") != NULL;
    if (!ok) {
        fprintf(stderr, "logout should redirect home:\n%s\n", resp ? resp : "(connect error)");
    }
    free(resp);

    resp = request(port, "GET", "/", NULL, NULL);
    ok = ok && resp != NULL && strstr(resp, "HTTP/1.1 200 OK") != NULL &&
         strstr(resp, "not logged in") != NULL && strstr(resp, "/login") != NULL &&
         strstr(resp, "Hi admin") == NULL;
    if (!ok) {
        fprintf(stderr, "after logout the home page should be anonymous again:\n%s\n", resp ? resp : "(connect error)");
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
    printf("login example ok\n");
    return 0;
}