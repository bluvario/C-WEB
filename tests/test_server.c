#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
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

#include "gzip.h"
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

// echoes the request body back verbatim so the spill test can prove the
// full payload survived the mmap round-trip
static void echo_body_fn(Http_Request *req, Http_Response *res, void *user_data)
{
    (void)user_data;
    http_response_set_status(res, HTTP_200_OK);
    http_response_set_header(res, "Content-Type", "application/octet-stream");
    http_response_add_body(res, req->body);
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

// sends a POST with a deterministic non-homogeneous body so byte-for-byte
// comparison is possible; each body byte is (base + i) & 0xff so the tail
// is distinguishable from the head; the response buffer is heap-sized so
// even a large spill-echoed body fits
static char *post_pattern(int port, const char *path, size_t len,
                          unsigned char base, char **body_out)
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
    char *body = req + hl;
    for (size_t i = 0; i < len; i++) {
        body[i] = (char)((base + (unsigned char)i) & 0xff);
    }
    if (body_out != NULL) {
        *body_out = malloc(len);
        memcpy(*body_out, body, len);
    }
    long sent = net_send_all(s, req, (long)((size_t)hl + len));
    free(req);
    if (sent != (long)((size_t)hl + len)) {
        net_close(s);
        if (body_out != NULL && *body_out != NULL) {
            free(*body_out);
            *body_out = NULL;
        }
        return NULL;
    }
    // heap-size the response buffer so even a large echoed body fits
    size_t cap = (size_t)hl + len + 1024;
    char *buf = malloc(cap);
    size_t got = 0;
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

// two-phase request: headers with Expect: 100-continue go out first and the
// body only after the server's interim reply, the way a real uploading
// client behaves. hands back whether the interim 100 actually arrived before
// the body; returns a heap copy of everything the server sent. a server that
// rejected the request early (413/417) closes without the body being
// transmitted here at all, which is exactly what the test asserts.
static char *post_expect(int port, const char *path, size_t len,
                         const char *expect_value, bool *got_interim)
{
    Socket_Handle s = net_connect("127.0.0.1", port);
    if (s == -1) {
        return NULL;
    }
    net_set_timeout(s, 3000);
    char head[256];
    int hl = snprintf(head, sizeof head,
                      "POST %s HTTP/1.1\r\n"
                      "Host: 127.0.0.1\r\n"
                      "Content-Length: %zu\r\n"
                      "Expect: %s\r\n"
                      "Connection: close\r\n"
                      "\r\n",
                      path, len, expect_value);
    if (net_send_all(s, head, hl) != hl) {
        net_close(s);
        return NULL;
    }

    size_t cap = (size_t)hl + len + 2048;
    char *buf = malloc(cap);
    size_t got = 0;
    long n = net_recv(s, buf + got, cap - got - 1);
    if (n <= 0) {
        net_close(s);
        free(buf);
        return NULL;
    }
    got += (size_t)n;
    *got_interim = n >= 21 && strncmp(buf, "HTTP/1.1 100 Continue", 21) == 0;
    if (*got_interim) {
        // the server green-lit the upload: stream the body, then read the
        // final answer
        char *body = malloc(len);
        for (size_t i = 0; i < len; i++) {
            body[i] = (char)((0xA1 + i) & 0xff);
        }
        if (net_send_all(s, body, (long)len) != (long)len) {
            free(body);
            net_close(s);
            free(buf);
            return NULL;
        }
        free(body);
    }
    while (got < cap - 1 && (n = net_recv(s, buf + got, cap - got - 1)) > 0) {
        got += (size_t)n;
    }
    buf[got] = '\0';
    net_close(s);
    return buf;
}

// POSTs a body with an extra raw header line ("" for none) so the test can
// claim Content-Encoding without a form layer; returns the whole response
static char *post_with_header(int port, const char *path, const char *extra,
                              const char *body, size_t len)
{
    Socket_Handle s = net_connect("127.0.0.1", port);
    if (s == -1) {
        return NULL;
    }
    char head[320];
    int hl = snprintf(head, sizeof head,
                      "POST %s HTTP/1.1\r\n"
                      "Host: 127.0.0.1\r\n"
                      "Content-Length: %zu\r\n"
                      "%s"
                      "Connection: close\r\n"
                      "\r\n",
                      path, len, extra);
    char *req = malloc((size_t)hl + len);
    memcpy(req, head, (size_t)hl);
    memcpy(req + hl, body, len);
    long sent = net_send_all(s, req, hl + (long)len);
    free(req);
    if (sent != hl + (long)len) {
        net_close(s);
        return NULL;
    }
    size_t cap = (size_t)hl + len + 4096;
    char *buf = malloc(cap);
    size_t got = 0;
    long n;
    while (got < cap - 1 && (n = net_recv(s, buf + got, cap - got - 1)) > 0) {
        got += (size_t)n;
    }
    buf[got] = '\0';
    net_close(s);
    return buf;
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

    // --- oversized body spill to disk via --body-dir ---
    // a temp dir for the spill file, a server with max_body=2048 and
    // body_dir set: a 4096-byte body must reach the handler (200 OK)
    // instead of earning a 413, and the temp file must be gone after
    // the response is sent
    char spill_tmpdir[] = "/tmp/cweb-spill-test-XXXXXX";
    if (mkdtemp(spill_tmpdir) == NULL) {
        fprintf(stderr, "mkdtemp failed\n");
        net_cleanup();
        return 1;
    }

    Socket_Handle srv4 = net_listen(0);
    if (srv4 == -1) {
        fprintf(stderr, "fourth listen failed: %s\n", net_error_string());
        rmdir(spill_tmpdir);
        net_cleanup();
        return 1;
    }
    int port4 = net_bound_port(srv4);
    Http_Server_Config cfg4;
    memset(&cfg4, 0, sizeof cfg4);
    cfg4.max_body = 64 * 1024;
    cfg4.body_dir = spill_tmpdir;
    cfg4.io_timeout_ms = 500;
    pid_t child4 = fork();
    if (child4 == 0) {
        log_set_level(LOG_WARN);
        http_serve_config(srv4, echo_body_fn, NULL, &cfg4);
        _exit(0);
    }

    // 800 bytes (below max_body) must still work the in-RAM path
    resp = post_body(port4, "/small", 800);
    int ok4 = resp != NULL && strstr(resp, "HTTP/1.1 200 OK") != NULL &&
              strstr(resp, "small!") == NULL;  // echo handler echoes body, not path
    free(resp);

    // 256 KiB body (above max_body) must spill to disk and come back
    // byte-for-byte identical; a non-homogeneous pattern (including null
    // bytes) so tail corruption, truncation, or offsets are caught.
    // the body length comes from the Content-Length header, never strlen:
    // the pattern deliberately contains 0x00 bytes
    char *expected_body = NULL;
    char *resp256 = post_pattern(port4, "/big", 256 * 1024, 0xA1,
                                 &expected_body);
    ok4 = ok4 && resp256 != NULL &&
          strstr(resp256, "HTTP/1.1 200 OK") != NULL;
    if (ok4 && expected_body != NULL) {
        char *cl = strstr(resp256, "Content-Length:");
        long echo_len = cl ? strtol(cl + strlen("Content-Length:"), NULL, 10) : -1;
        const char *body_start = strstr(resp256, "\r\n\r\n");
        ok4 = echo_len == 256 * 1024 && body_start != NULL &&
              memcmp(body_start + 4, expected_body, 256 * 1024) == 0;
    }
    free(expected_body);
    free(resp256);

    // the temp spill file must have been unlinked by http_request_free
    {
        DIR *d = opendir(spill_tmpdir);
        if (d) {
            struct dirent *ent;
            while ((ent = readdir(d)) != NULL) {
                if (strncmp(ent->d_name, "cweb-body-", 10) == 0) {
                    ok4 = 0;
                    break;
                }
            }
            closedir(d);
        }
    }

    kill(child4, SIGTERM);
    waitpid(child4, NULL, 0);
    net_close(srv4);

    // clean up the temp dir
    rmdir(spill_tmpdir);

    if (!ok4) {
        fprintf(stderr, "spill-to-disk test failed\n");
        net_cleanup();
        return 1;
    }

    // --- Expect: 100-continue ---
    // a tight 4 KiB cap with no spill dir: a waiting client must be answered
    // with the interim 100 before its body is transmitted, an oversized
    // upload must be rejected 413 before a single body byte is sent, an
    // unsupported expectation must earn a 417, and a plain request with no
    // Expect header must never see a 100 at all
    Socket_Handle srv5 = net_listen(0);
    if (srv5 == -1) {
        fprintf(stderr, "fifth listen failed: %s\n", net_error_string());
        net_cleanup();
        return 1;
    }
    int port5 = net_bound_port(srv5);
    Http_Server_Config cfg5;
    memset(&cfg5, 0, sizeof cfg5);
    cfg5.max_body = 4096;
    cfg5.io_timeout_ms = 1500;
    cfg5.workers = 2;
    pid_t child5 = fork();
    if (child5 == 0) {
        log_set_level(LOG_WARN);
        http_serve_config(srv5, echo_body_fn, NULL, &cfg5);
        _exit(0);
    }

    bool interim = false;
    resp = post_expect(port5, "/expect", 16, "100-continue", &interim);
    int ok5 = resp != NULL && interim &&
              strstr(resp, "HTTP/1.1 100 Continue") != NULL &&
              strstr(resp, "HTTP/1.1 200 OK") != NULL;
    free(resp);

    // a 64 KiB upload -- far past the 4 KiB cap -- is rejected before the
    // body would ever be sent; the client never uploads anything
    resp = post_expect(port5, "/huge", 65536, "100-continue", &interim);
    ok5 = ok5 && resp != NULL && !interim &&
          strstr(resp, "HTTP/1.1 413") != NULL &&
          strstr(resp, "100 Continue") == NULL &&
          strlen(resp) < 1024;
    free(resp);

    resp = post_expect(port5, "/other", 16, "something-else", &interim);
    ok5 = ok5 && resp != NULL && !interim &&
          strstr(resp, "HTTP/1.1 417") != NULL;
    free(resp);

    // no Expect header means no interim response, just the ordinary answer
    resp = post_body(port5, "/", 800);
    ok5 = ok5 && resp != NULL && strstr(resp, "HTTP/1.1 200 OK") != NULL &&
          strstr(resp, "100 Continue") == NULL;
    free(resp);

    kill(child5, SIGTERM);
    waitpid(child5, NULL, 0);
    net_close(srv5);
    if (!ok5) {
        fprintf(stderr, "Expect: 100-continue test failed\n");
        net_cleanup();
        return 1;
    }

    // the same handshake must survive the spill route: a 256 KiB upload past
    // the 64 KiB cap gets the interim 100, then streams to disk and comes
    // back byte-for-byte identical
    char spill_tmpdir2[] = "/tmp/cweb-spill-e2-XXXXXX";
    if (mkdtemp(spill_tmpdir2) == NULL) {
        fprintf(stderr, "mkdtemp failed\n");
        net_cleanup();
        return 1;
    }
    Socket_Handle srv6 = net_listen(0);
    if (srv6 == -1) {
        fprintf(stderr, "sixth listen failed: %s\n", net_error_string());
        rmdir(spill_tmpdir2);
        net_cleanup();
        return 1;
    }
    int port6 = net_bound_port(srv6);
    Http_Server_Config cfg6;
    memset(&cfg6, 0, sizeof cfg6);
    cfg6.max_body = 64 * 1024;
    cfg6.body_dir = spill_tmpdir2;
    cfg6.io_timeout_ms = 3000;
    pid_t child6 = fork();
    if (child6 == 0) {
        log_set_level(LOG_WARN);
        http_serve_config(srv6, echo_body_fn, NULL, &cfg6);
        _exit(0);
    }

    resp = post_expect(port6, "/big", 256 * 1024, "100-continue", &interim);
    int ok6 = resp != NULL && interim &&
              strstr(resp, "HTTP/1.1 100 Continue") != NULL &&
              strstr(resp, "HTTP/1.1 200 OK") != NULL;
    if (ok6 && resp != NULL) {
        long echo_len = -1;
        char *cl = strstr(resp, "Content-Length:");
        if (cl != NULL) {
            echo_len = strtol(cl + strlen("Content-Length:"), NULL, 10);
        }
        // resp carries the interim 100 followed by the real response, so
        // the body starts after the *second* header/body separator
        const char *sep1 = strstr(resp, "\r\n\r\n");
        const char *sep2 = sep1 != NULL ? strstr(sep1 + 4, "\r\n\r\n") : NULL;
        const char *body_start = sep2 != NULL ? sep2 + 4 : NULL;
        if (echo_len != 256 * 1024 || body_start == NULL) {
            ok6 = 0;
        } else {
            for (size_t i = 0; i < 256 * 1024; i++) {
                if ((unsigned char)body_start[i] !=
                    (unsigned char)((0xA1 + i) & 0xff)) {
                    ok6 = 0;
                    break;
                }
            }
        }
    }
    free(resp);

    kill(child6, SIGTERM);
    waitpid(child6, NULL, 0);
    net_close(srv6);
    rmdir(spill_tmpdir2);
    if (!ok6) {
        fprintf(stderr, "Expect: 100-continue over the spill route failed\n");
        net_cleanup();
        return 1;
    }

    // --- Content-Encoding: gzip request bodies ---
    // the server inflates a gzipped upload before the handler sees it, and
    // the echo handler must return the decompressed bytes verbatim. the
    // decompression ceiling is 4096, so a compressed bomb (7000 bytes of
    // zeros) gets a 413 instead of ballooning a worker, and garbage with
    // the header earns a 400.
    Socket_Handle srv7 = net_listen(0);
    if (srv7 == -1) {
        fprintf(stderr, "seventh listen failed: %s\n", net_error_string());
        net_cleanup();
        return 1;
    }
    int port7 = net_bound_port(srv7);
    Http_Server_Config cfg7;
    memset(&cfg7, 0, sizeof cfg7);
    cfg7.max_body = 64 * 1024;
    cfg7.max_inflated = 4096;
    cfg7.io_timeout_ms = 2000;
    pid_t child7 = fork();
    if (child7 == 0) {
        log_set_level(LOG_WARN);
        http_serve_config(srv7, echo_body_fn, NULL, &cfg7);
        _exit(0);
    }

    int ok7 = 1;
    Strbuf gz;
    strbuf_init(&gz);
    const char *plain = "hello gzip upload";
    if (http_gzip_compress(plain, strlen(plain), &gz) == 0) {
        resp = post_with_header(port7, "/gz", "Content-Encoding: gzip\r\n",
                                gz.items, gz.count);
        ok7 = resp != NULL && strstr(resp, "HTTP/1.1 200 OK") != NULL &&
              strstr(resp, plain) != NULL &&
              strstr(resp, "100 Continue") == NULL;
        free(resp);
    } else {
        ok7 = 0;
    }
    strbuf_free(&gz);

    static char bomb[7000];
    memset(bomb, 0, sizeof bomb);
    strbuf_init(&gz);
    if (http_gzip_compress(bomb, sizeof bomb, &gz) == 0) {
        resp = post_with_header(port7, "/bomb", "Content-Encoding: gzip\r\n",
                                gz.items, gz.count);
        ok7 = ok7 && resp != NULL && strstr(resp, "HTTP/1.1 413") != NULL &&
              strstr(resp, "200 OK") == NULL;
        free(resp);
    } else {
        ok7 = 0;
    }
    strbuf_free(&gz);

    const char *trash = "this is definitely not gzip";
    resp = post_with_header(port7, "/trash", "Content-Encoding: gzip\r\n",
                            trash, strlen(trash));
    ok7 = ok7 && resp != NULL && strstr(resp, "HTTP/1.1 400") != NULL;
    free(resp);

    // control: no Content-Encoding header, the body must pass through raw
    const char *raw = "plain body";
    resp = post_with_header(port7, "/raw", "", raw, strlen(raw));
    ok7 = ok7 && resp != NULL && strstr(resp, "HTTP/1.1 200 OK") != NULL &&
          strstr(resp, raw) != NULL;
    free(resp);

    kill(child7, SIGTERM);
    waitpid(child7, NULL, 0);
    net_close(srv7);
    if (!ok7) {
        fprintf(stderr, "Content-Encoding: gzip request test failed\n");
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