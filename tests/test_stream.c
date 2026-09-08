#define _POSIX_C_SOURCE 200809L

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "log.h"
#include "net.h"
#include "request.h"
#include "response.h"
#include "server.h"

// hands the body out in three uneven pieces so the framing has to vary
static int g_step;

static size_t stream_chunks(void *buf, size_t cap, void *user_data)
{
    (void)user_data;
    static const char *const parts[] = {"HELLO ", "WORLD", "!"};
    if (g_step >= (int)(sizeof(parts) / sizeof(parts[0]))) {
        return 0;
    }
    const char *p = parts[g_step++];
    size_t n = strlen(p);
    if (n > cap) {
        n = cap;
    }
    memcpy(buf, p, n);
    return n;
}

static void stream_handler(Http_Request *req, Http_Response *res, void *user_data)
{
    (void)user_data;
    http_response_set_status(res, HTTP_200_OK);
    http_response_set_header(res, "Content-Type", "text/plain; charset=utf-8");
    http_response_set_stream(res, stream_chunks, NULL);
    (void)req;
}

// drains the socket until the peer closes, then returns what came over
static size_t read_all(Socket_Handle fd, char *buf, size_t cap)
{
    size_t got = 0;
    long n;
    while (got < cap - 1 && (n = net_recv(fd, buf + got, cap - got - 1)) > 0) {
        got += (size_t)n;
    }
    buf[got] = '\0';
    return got;
}

int main(void)
{
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
        http_serve(srv, stream_handler, NULL);
        _exit(0);
    }

    // GET: three framed chunks and the balance frame before the close
    Socket_Handle client = net_connect("127.0.0.1", port);
    if (client == -1) {
        fprintf(stderr, "connect failed: %s\n", net_error_string());
        return 1;
    }
    const char *get = "GET /stream HTTP/1.1\r\nHost: localhost\r\n"
                      "Connection: close\r\n\r\n";
    if (net_send_all(client, get, strlen(get)) != (long)strlen(get)) {
        fprintf(stderr, "GET send failed\n");
        return 1;
    }
    char body[8192];
    read_all(client, body, sizeof(body));

    int ok = strstr(body, "HTTP/1.1 200 OK") != NULL &&
             strstr(body, "Transfer-Encoding: chunked") != NULL &&
             strstr(body, "Content-Length:") == NULL &&
             strstr(body, "6\r\nHELLO \r\n") != NULL &&
             strstr(body, "5\r\nWORLD\r\n") != NULL &&
             strstr(body, "1\r\n!\r\n") != NULL &&
             strstr(body, "0\r\n\r\n") != NULL;
    if (!ok) {
        fprintf(stderr, "unexpected streamed body:\n%s\n", body);
    }
    net_close(client);

    // HEAD: same metadata, but no chunk frames may hit the wire
    Socket_Handle head = net_connect("127.0.0.1", port);
    if (head == -1) {
        fprintf(stderr, "head connect failed\n");
        return 1;
    }
    const char *hreq = "HEAD /stream HTTP/1.1\r\nHost: localhost\r\n"
                       "Connection: close\r\n\r\n";
    if (net_send_all(head, hreq, strlen(hreq)) != (long)strlen(hreq)) {
        fprintf(stderr, "HEAD send failed\n");
        return 1;
    }
    char headbuf[4096];
    read_all(head, headbuf, sizeof(headbuf));
    int head_ok = strstr(headbuf, "HTTP/1.1 200 OK") != NULL &&
                  strstr(headbuf, "Transfer-Encoding: chunked") != NULL &&
                  strstr(headbuf, "WORLD") == NULL &&
                  strstr(headbuf, "0\r\n\r\n") == NULL;
    if (!head_ok) {
        fprintf(stderr, "unexpected HEAD response:\n%s\n", headbuf);
    }
    net_close(head);

    kill(child, SIGTERM);
    waitpid(child, NULL, 0);
    net_close(srv);
    net_cleanup();

    if (!ok || !head_ok) {
        return 1;
    }
    printf("stream ok\n");
    return 0;
}