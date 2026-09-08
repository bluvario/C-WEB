#define _POSIX_C_SOURCE 200809L

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "http.h"
#include "log.h"
#include "net.h"
#include "request.h"
#include "response.h"
#include "server.h"
#include "sse.h"
#include "strbuf.h"
#include "sv.h"

static int check(const char *what, int cond)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        return 1;
    }
    return 0;
}

// unit: one fully dressed frame renders to exactly the spectated bytes
static int test_frame(void)
{
    Strbuf out;
    strbuf_init(&out);
    String_View data = sv_from_cstr("hello\nworld");
    sse_frame(&out, data, "chat", "42", 5000);
    const char *want =
        "id: 42\n"
        "event: chat\n"
        "data: hello\n"
        "data: world\n"
        "retry: 5000\n"
        "\n";
    int ok = out.count == strlen(want) && memcmp(out.items, want, strlen(want)) == 0;
    strbuf_free(&out);
    return ok;
}

static int test_frame_bare(void)
{
    // empty data with no id/event/retry: one bare "data:" line plus the blank
    Strbuf out;
    strbuf_init(&out);
    sse_frame(&out, sv_from_cstr(""), NULL, NULL, 0);
    int ok = out.count == 8 && memcmp(out.items, "data: \n\n", 8) == 0;
    strbuf_free(&out);
    return ok;
}

// wire: two events then the stream ends
static int g_event = 0;

static size_t sse_pump(void *buf, size_t cap, void *user_data)
{
    (void)user_data;
    Strbuf frame;
    strbuf_init(&frame);
    if (g_event == 0) {
        sse_frame(&frame, sv_from_cstr("tick"), "ping", NULL, 0);
    } else if (g_event == 1) {
        sse_frame(&frame, sv_from_cstr("done"), "bye", "7", 0);
    }
    g_event++;
    size_t n = frame.count;
    if (n > cap) {
        n = cap;
    }
    memcpy(buf, frame.items, n);
    strbuf_free(&frame);
    return n;
}

static void sse_handler(Http_Request *req, Http_Response *res, void *user_data)
{
    (void)req;
    (void)user_data;
    http_response_start_sse(res, sse_pump, NULL);
}

static long read_all(Socket_Handle fd, char *buf, size_t cap)
{
    size_t got = 0;
    long n;
    while (got < cap - 1 && (n = net_recv(fd, buf + got, cap - got - 1)) > 0) {
        got += (size_t)n;
    }
    buf[got] = '\0';
    return (long)got;
}

int main(void)
{
    int fails = 0;
    fails += check("sse frame renders spec bytes", test_frame());
    fails += check("bare sse frame", test_frame_bare());

    if (net_init() != 0) {
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
        http_serve(srv, sse_handler, NULL);
        _exit(0);
    }

    Socket_Handle client = net_connect("127.0.0.1", port);
    if (client == -1) {
        fprintf(stderr, "connect failed: %s\n", net_error_string());
        return 1;
    }
    const char *request = "GET /events HTTP/1.1\r\nHost: localhost\r\n"
                          "Connection: close\r\n\r\n";
    if (net_send_all(client, request, strlen(request)) != (long)strlen(request)) {
        fprintf(stderr, "request send failed\n");
        return 1;
    }
    char body[8192];
    read_all(client, body, sizeof(body));

    int wire_ok = strstr(body, "HTTP/1.1 200 OK") != NULL &&
                  strstr(body, "Content-Type: text/event-stream\r\n") != NULL &&
                  strstr(body, "Cache-Control: no-cache\r\n") != NULL &&
                  strstr(body, "Transfer-Encoding: chunked\r\n") != NULL &&
                  strstr(body, "data: tick\nevent: ping\n\n") == NULL && // frame order put data before event
                  strstr(body, "event: ping\ndata: tick\n\n") != NULL &&
                  strstr(body, "id: 7\nevent: bye\ndata: done\n\n") != NULL &&
                  strstr(body, "0\r\n\r\n") != NULL;
    if (!wire_ok) {
        fprintf(stderr, "unexpected SSE stream:\n%s\n", body);
    }
    net_close(client);

    kill(child, SIGTERM);
    waitpid(child, NULL, 0);
    net_close(srv);
    net_cleanup();

    if (fails != 0 || !wire_ok) {
        return 1;
    }
    printf("sse ok\n");
    return 0;
}