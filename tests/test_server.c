#define _POSIX_C_SOURCE 200809L

#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "log.h"
#include "net.h"
#include "server.h"
#include "strbuf.h"
#include "sv.h"

static void handler_fn(Http_Request *req, Http_Response *res, void *user_data)
{
    (void)user_data;
    http_response_set_status(res, HTTP_200_OK);
    http_response_set_header(res, "Content-Type", "text/plain; charset=utf-8");

    Strbuf body;
    strbuf_init(&body);
    strbuf_append_cstr(&body, "Hello, ");
    strbuf_append(&body, req->path.data, req->path.count);
    strbuf_append_cstr(&body, "!");
    http_response_add_body(res, (String_View){body.items, body.count});
    strbuf_free(&body);
}

// POSTs fill 'x' bytes against a running server; returns a heap copy of the
// whole response (or NULL when the connection fails)
static char *post_body(int port, const char *path, size_t len)
{
    Socket_Handle s = net_connect("127.0.0.1", port);
    if (s == -1) {
        return NULL;
    }
    char head[256];
    int hl = snprintf(head, sizeof head,
                      "POST %s HTTP/1.1\r\n"
                      "Host: 127.0.0.1\r\n"
                      "Content-Length: %zu\r\n"
                      "Connection: close\r\n"
                      "\r\n",
                      path, len);
    char *req = malloc((size_t)hl + len);
    memcpy(req, head, (size_t)hl);
    memset(req + hl, 'x', len);
    long sent = net_send_all(s, req, (long)((size_t)hl + len));
    free(req);
    if (sent != (long)((size_t)hl + len)) {
        net_close(s);
        return NULL;
    }
    size_t cap = 8192, got = 0;
    char *buf = malloc(cap);
    long n;
    while (got < cap - 1 && (n = net_recv(s, buf + got, cap - got - 1)) > 0) {
        got += (size_t)n;
    }
    buf[got] = '\0';
    net_close(s);
    return buf;
}

// sends a partial request, then waits past the server's --io-timeout to see
// whether it answers 408
static char *stall_partial(int port)
{
    Socket_Handle s = net_connect("127.0.0.1", port);
    if (s == -1) {
        return NULL;
    }
    const char *partial = "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\n";
    net_send_all(s, partial, strlen(partial));
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 400000000};
    nanosleep(&ts, NULL);
    net_set_timeout(s, 2000);
    size_t cap = 8192, got = 0;
    char *buf = malloc(cap);
    long n;
    while (got < cap - 1 && (n = net_recv(s, buf + got, cap - got - 1)) > 0) {
        got += (size_t)n;
    }
    buf[got] = '\0';
    net_close(s);
    return buf;
}

// the fd-exhaustion child holds the process against its RLIMIT_NOFILE with a
// wall of dummy descriptors; this detached thread frees them a second later so
// the server's accept loop gets a chance to recover and answer again
static int s_close_dummies[64];
static int s_close_ndummies;

static void *close_dummies_after_a_second(void *arg)
{
    (void)arg;
    struct timespec ts = {.tv_sec = 1, .tv_nsec = 0};
    nanosleep(&ts, NULL);
    for (int i = 0; i < s_close_ndummies; i++) {
        close(s_close_dummies[i]);
    }
    return NULL;
}

int main(void)
{
    if (net_init() != 0) {
        fprintf(stderr, "net_init failed\n");
        return 1;
    }

    Socket_Handle srv = net_listen(0);
    if (srv == -1) {
        fprintf(stderr, "listen failed: %s\n", net_error_string());
        return 1;
    }
    int port = net_bound_port(srv);

    pid_t child = fork();
    if (child == 0) {
        log_set_level(LOG_WARN);
        http_serve(srv, handler_fn, NULL);
        _exit(0); // unreachable, silence the compiler
    }

    Socket_Handle client = net_connect("127.0.0.1", port);
    if (client == -1) {
        fprintf(stderr, "connect failed: %s\n", net_error_string());
        return 1;
    }

    const char *request =
        "GET /hello?x=1 HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Connection: keep-alive\r\n"
        "\r\n"
        "GET /again HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Connection: close\r\n"
        "\r\n";
    if (net_send_all(client, request, strlen(request)) != (long)strlen(request)) {
        fprintf(stderr, "request send failed\n");
        return 1;
    }

    char buf[8192];
    size_t got = 0;
    long n;
    while (got < sizeof(buf) - 1 && (n = net_recv(client, buf + got, sizeof(buf) - got - 1)) > 0) {
        got += (size_t)n;
    }
    buf[got] = '\0';

    // both pipelined requests must be served over the same connection: the
    // first advertises keep-alive, the second closes it
    int ok = strstr(buf, "HTTP/1.1 200 OK") != NULL &&
             strstr(buf, "Hello, /hello!") != NULL &&
             strstr(buf, "Content-Length: 14\r\n") != NULL &&
             strstr(buf, "Connection: keep-alive\r\n") != NULL &&
             strstr(buf, "Hello, /again!") != NULL &&
             strstr(buf, "Content-Length: 14\r\n") != NULL &&
             strstr(buf, "Connection: close\r\n") != NULL &&
             strstr(buf, "Hello, /hello!") < strstr(buf, "Hello, /again!");
    if (!ok) {
        fprintf(stderr, "unexpected response:\n%s\n", buf);
    }

    net_close(client);
    kill(child, SIGTERM);
    waitpid(child, NULL, 0);
    net_close(srv);

    // the config knobs are honored by the accept loop: a tighter body cap
    // answers 413 mid-body and a shorter stall budget answers 408
    Socket_Handle srv2 = net_listen(0);
    if (srv2 == -1) {
        fprintf(stderr, "second listen failed: %s\n", net_error_string());
        net_cleanup();
        return 1;
    }
    int port2 = net_bound_port(srv2);
    Http_Server_Config cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.max_body = 2048;
    cfg.io_timeout_ms = 150;
    cfg.workers = 2;
    pid_t child2 = fork();
    if (child2 == 0) {
        log_set_level(LOG_WARN);
        http_serve_config(srv2, handler_fn, NULL, &cfg);
        _exit(0);
    }

    char *resp = post_body(port2, "/", 800);
    int ok2 = resp != NULL && strstr(resp, "HTTP/1.1 200 OK") != NULL &&
              strstr(resp, "Hello, /!") != NULL;
    free(resp);

    resp = post_body(port2, "/", 4096);
    ok2 = ok2 && resp != NULL && strstr(resp, "HTTP/1.1 413") != NULL;
    free(resp);

    resp = post_body(port2, "/", 800);
    ok2 = ok2 && resp != NULL && strstr(resp, "HTTP/1.1 200 OK") != NULL;
    free(resp);

    resp = stall_partial(port2);
    ok2 = ok2 && resp != NULL && strstr(resp, "HTTP/1.1 408") != NULL;
    if (!ok2) {
        fprintf(stderr, "config knobs were not honored:\n%s\n", resp ? resp : "(stall got nothing)");
    }
    free(resp);

    kill(child2, SIGTERM);
    waitpid(child2, NULL, 0);
    net_close(srv2);

    // --- fd exhaustion (EMFILE) back-off ---
    // a child walled against its own RLIMIT_NOFILE forces net_accept to fail
    // with EMFILE; the accept loop must back off instead of spinning, then
    // serve again the moment a descriptor is freed
    Socket_Handle srv3 = net_listen(0);
    if (srv3 == -1) {
        fprintf(stderr, "third listen failed: %s\n", net_error_string());
        net_cleanup();
        return 1;
    }
    int port3 = net_bound_port(srv3);
    pid_t child3 = fork();
    if (child3 == 0) {
        log_set_level(LOG_WARN);
        int listen_fd = (int)(intptr_t)srv3;
        struct rlimit rl;
        rl.rlim_cur = (rlim_t)(listen_fd + 4);
        rl.rlim_max = rl.rlim_cur;
        if (setrlimit(RLIMIT_NOFILE, &rl) != 0) {
            _exit(1);
        }
        int ndummies = 0;
        while (ndummies < 64) {
            s_close_dummies[ndummies] = open("/dev/null", O_RDONLY);
            if (s_close_dummies[ndummies] < 0) {
                break;
            }
            ndummies++;
        }
        s_close_ndummies = ndummies;
        // prove the wall is real: a fresh socket must come back as EMFILE
        int probe = socket(AF_INET, SOCK_STREAM, 0);
        if (probe >= 0) {
            close(probe);
            _exit(1);
        }
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_t tid;
        pthread_create(&tid, &attr, close_dummies_after_a_second, NULL);
        pthread_attr_destroy(&attr);
        http_serve(srv3, handler_fn, NULL);
        _exit(0);
    }

    // give the child time to hit the wall, then probe once: the kernel
    // queues (or drops) the connection while every fd is taken
    struct timespec nap = {.tv_sec = 0, .tv_nsec = 400000000};
    nanosleep(&nap, NULL);
    Socket_Handle in_storm = net_connect("127.0.0.1", port3);
    if (in_storm != -1) {
        net_close(in_storm);
    }
    // let the dummy-closer free the fds, then the loop must serve again
    nap.tv_sec = 1;
    nap.tv_nsec = 300000000;
    nanosleep(&nap, NULL);
    Socket_Handle cs3 = net_connect("127.0.0.1", port3);
    int ok3 = 0;
    if (cs3 != -1) {
        const char *req3 =
            "GET /emfile HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
        if (net_send_all(cs3, req3, strlen(req3)) == (long)strlen(req3)) {
            char buf3[8192];
            size_t got3 = 0;
            long n3;
            while (got3 < sizeof buf3 - 1 &&
                   (n3 = net_recv(cs3, buf3 + got3, sizeof buf3 - got3 - 1)) > 0) {
                got3 += (size_t)n3;
            }
            buf3[got3] = '\0';
            ok3 = strstr(buf3, "HTTP/1.1 200 OK") != NULL &&
                  strstr(buf3, "Hello, /emfile!") != NULL;
        }
        net_close(cs3);
    }
    kill(child3, SIGTERM);
    waitpid(child3, NULL, 0);
    net_close(srv3);
    if (!ok3) {
        fprintf(stderr, "server did not recover from the EMFILE storm\n");
        net_cleanup();
        return 1;
    }

    net_cleanup();

    if (!ok) {
        return 1;
    }
    if (!ok2) {
        return 1;
    }
    printf("server ok\n");
    return 0;
}