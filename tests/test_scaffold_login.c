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
#define REPO "."

// the --login scaffold, generated and compiled with the cweb CLI, exercised
// over the wire: signup auto-logs-in, brute-forcing one account to a dry
// bucket answers 429 with Retry-After instead of chewing the password hash,
// a pause lets the budget refill, and an account-creation flood is capped by
// the caller's address.

static void nap(void)
{
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 20000000};
    nanosleep(&ts, NULL);
}

static int wait_for_port(int port)
{
    for (int i = 0; i < 200; i++) {
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

// status code out of a response's start line; 0 when it does not look like
// "HTTP/1.x NNN"
static int status_of(const char *resp)
{
    if (resp == NULL) {
        return 0;
    }
    const char *sp = strstr(resp, "HTTP/1.");
    if (sp == NULL) {
        return 0;
    }
    while (*sp != '\0' && *sp != ' ') {
        sp++;
    }
    while (*sp == ' ') {
        sp++;
    }
    return atoi(sp);
}

int main(void)
{
    if (net_init() != 0) {
        return 1;
    }

    char tmp[] = "/tmp/cweb_scaffold_login_XXXXXX";
    if (mkdtemp(tmp) == NULL) {
        perror("mkdtemp");
        return 1;
    }

    // generate the scaffold, then compile it with the CLI
    char cmd[4096];
    snprintf(cmd, sizeof cmd,
             "%s new --login %s >/dev/null", TOOL, tmp);
    if (system(cmd) != 0) {
        fprintf(stderr, "cweb new --login failed\n");
        return 1;
    }
    char views_dir[512], build_dir[512], static_dir[512];
    snprintf(views_dir, sizeof views_dir, "%s/views", tmp);
    snprintf(build_dir, sizeof build_dir, "%s/build", tmp);
    snprintf(static_dir, sizeof static_dir, "%s/static", tmp);
    snprintf(cmd, sizeof cmd,
             "%s build %s %s %s %s >/dev/null", TOOL, views_dir, build_dir,
             REPO, static_dir);
    if (system(cmd) != 0) {
        fprintf(stderr, "cweb build failed for the scaffold\n");
        return 1;
    }

    // reserve a port, then hand it to the freshly linked server
    Socket_Handle probe = net_listen(0);
    int port = net_bound_port(probe);
    net_close(probe);

    char server_path[512], port_arg[16], db_path[512];
    snprintf(server_path, sizeof server_path, "%s/build/server", tmp);
    snprintf(port_arg, sizeof port_arg, "%d", port);
    snprintf(db_path, sizeof db_path, "%s/app.db", tmp);
    pid_t child = fork();
    if (child == 0) {
        execl(server_path, "server", "--port", port_arg, "--db", db_path, NULL);
        _exit(127);
    }
    if (wait_for_port(port) != 0) {
        fprintf(stderr, "server did not come up on port %d\n", port);
        return 1;
    }

    int ok = 1;
    char *resp;

    // the home page renders the scaffold shell and both auth links
    resp = request(port, "GET", "/", NULL, NULL);
    ok = ok && status_of(resp) == 200 &&
         strstr(resp, "/login") != NULL && strstr(resp, "/signup") != NULL;
    if (!ok) {
        fprintf(stderr, "scaffold home should show auth links:\n%s\n",
                resp ? resp : "(connect error)");
    }
    free(resp);

    // an empty signup (no db-backed account yet) creates the account, drops
    // a session cookie and lands on the private dashboard
    resp = request(port, "POST", "/signup", NULL,
                   "username=alice&password=correct-horse");
    ok = ok && resp != NULL && status_of(resp) == 303 &&
         strstr(resp, "Location: /dashboard") != NULL &&
         strstr(resp, "Set-Cookie: cweb_session=") != NULL;
    if (!ok) {
        fprintf(stderr, "signup should create alice, set the session and "
                        "redirect to the dashboard:\n%s\n",
                resp ? resp : "(connect error)");
    }
    free(resp);

    // brute-force alice: the claimed username owns the bucket, so a sustained
    // run of wrong guesses drains burst (3) and the password check (a real
    // PBKDF2 hash, the thing the guard protects) stops running; once denied,
    // every immediate retry stays denied with a 429 + Retry-After
    char what[128];
    int denied = 0;
    int denied_tail = 0;
    for (int i = 1; i <= 12; i++) {
        snprintf(what, sizeof what, "username=alice&password=guess-%d", i);
        resp = request(port, "POST", "/login", NULL, what);
        int st = status_of(resp);
        if (st == 429) {
            denied++;
            denied_tail++;
            ok = ok && strstr(resp, "Retry-After:") != NULL &&
                 strstr(resp, "too many attempts") != NULL;
        } else if (st == 200) {
            denied_tail = 0;
        }
        free(resp);
    }
    ok = ok && denied >= 3 && denied_tail >= 2;
    if (!ok) {
        fprintf(stderr, "rapid wrong logins should dry the account bucket into "
                        "a sustained 429 (got denied=%d tail=%d)\n",
                denied, denied_tail);
    }

    // with the format page idle the bucket refills (~1 token/sec), so the
    // real password is accepted once more
    struct timespec ts = {.tv_sec = 2, .tv_nsec = 0};
    nanosleep(&ts, NULL);
    resp = request(port, "POST", "/login", NULL,
                   "username=alice&password=correct-horse");
    ok = ok && resp != NULL && status_of(resp) == 303 &&
         strstr(resp, "Set-Cookie: cweb_session=") != NULL;
    if (!ok) {
        fprintf(stderr, "after a pause the real login should be allowed:\n%s\n",
                resp ? resp : "(connect error)");
    }
    free(resp);

    // account-creation floods draw on the caller's address bucket: distinct
    // names do not each get a fresh budget, so a burst of signups from one
    // address is capped the same way
    denied = 0;
    denied_tail = 0;
    for (int i = 1; i <= 12; i++) {
        snprintf(what, sizeof what,
                 "username=bot%d&password=correct-horse", i);
        resp = request(port, "POST", "/signup", NULL, what);
        int st = status_of(resp);
        if (st == 429) {
            denied++;
            denied_tail++;
        } else {
            denied_tail = 0;
        }
        free(resp);
    }
    ok = ok && denied >= 3 && denied_tail >= 2;
    if (!ok) {
        fprintf(stderr, "a signup flood from one address should be throttled "
                        "(got denied=%d tail=%d)\n",
                denied, denied_tail);
    }

    kill(child, SIGTERM);
    waitpid(child, NULL, 0);
    net_cleanup();

    char cleanup[4096];
    snprintf(cleanup, sizeof cleanup, "rm -rf %s", tmp);
    system(cleanup);

    if (!ok) {
        return 1;
    }
    printf("scaffold login ok\n");
    return 0;
}