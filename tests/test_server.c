#define _POSIX_C_SOURCE 200809L

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
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
        "\r\n";
    if (net_send_all(client, request, strlen(request)) != (long)strlen(request)) {
        fprintf(stderr, "request send failed\n");
        return 1;
    }

    char buf[4096];
    size_t got = 0;
    long n;
    while (got < sizeof(buf) - 1 && (n = net_recv(client, buf + got, sizeof(buf) - got - 1)) > 0) {
        got += (size_t)n;
    }
    buf[got] = '\0';

    int ok = strstr(buf, "HTTP/1.1 200 OK") != NULL &&
             strstr(buf, "Hello, /hello!") != NULL &&
             strstr(buf, "Content-Length: 14\r\n") != NULL;
    if (!ok) {
        fprintf(stderr, "unexpected response:\n%s\n", buf);
    }

    net_close(client);
    kill(child, SIGTERM);
    waitpid(child, NULL, 0);
    net_close(srv);
    net_cleanup();

    if (!ok) {
        return 1;
    }
    printf("server ok\n");
    return 0;
}