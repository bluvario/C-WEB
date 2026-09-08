#define _POSIX_C_SOURCE 200809L

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <time.h>

#include "log.h"
#include "net.h"
#include "request.h"
#include "response.h"
#include "server.h"

// stalls long enough that the parent can signal us mid-request: if shutdown
// is graceful the in-flight response still arrives before the connection dies
static void slow_handler(Http_Request *req, Http_Response *res, void *user_data)
{
    (void)user_data;
    struct timespec ts = {0, 200000000}; // 200 ms
    nanosleep(&ts, NULL);
    http_response_set_status(res, HTTP_200_OK);
    http_response_add_body_cstr(res, "drained");
    (void)req;
}

static long read_available(Socket_Handle fd, char *buf, size_t cap)
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
        int rc = http_serve(srv, slow_handler, NULL);
        // graceful shutdown means http_serve returns 0
        _exit(rc == 0 ? 0 : 3);
    }

    Socket_Handle client = net_connect("127.0.0.1", port);
    if (client == -1) {
        fprintf(stderr, "connect failed: %s\n", net_error_string());
        return 1;
    }
    const char *request = "GET /slow HTTP/1.1\r\nHost: localhost\r\n"
                          "Connection: close\r\n\r\n";
    if (net_send_all(client, request, strlen(request)) != (long)strlen(request)) {
        fprintf(stderr, "request send failed\n");
        return 1;
    }

    // let the request land, then ask for the door in the middle of it
    struct timespec wait = {0, 50000000}; // 50 ms
    nanosleep(&wait, NULL);
    kill(child, SIGTERM);

    char buf[4096];
    long got = read_available(client, buf, sizeof(buf));
    int drained = got > 0 && strstr(buf, "HTTP/1.1 200 OK") != NULL &&
                  strstr(buf, "drained") != NULL;

    net_close(client);

    int status = 0;
    waitpid(child, &status, 0);
    int exited = WIFEXITED(status);
    int code = exited ? WEXITSTATUS(status) : -1;

    net_close(srv);
    net_cleanup();

    int ok = drained && exited && code == 0;
    if (!ok) {
        fprintf(stderr, "graceful shutdown failed: drained=%d exited=%d code=%d\n"
                        "response:\n%s\n",
                drained, exited, code, buf);
        return 1;
    }
    printf("shutdown ok\n");
    return 0;
}