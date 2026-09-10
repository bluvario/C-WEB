#define _POSIX_C_SOURCE 200809L

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "log.h"
#include "net.h"
#include "request.h"
#include "response.h"
#include "server.h"

static int check(const char *what, int cond)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        return 1;
    }
    return 0;
}

static void unix_handler(Http_Request *req, Http_Response *res, void *user_data)
{
    (void)req;
    (void)user_data;
    http_response_set_status(res, HTTP_200_OK);
    http_response_set_header(res, "Content-Type", "text/plain; charset=utf-8");
    http_response_add_body_cstr(res, "hello over the pipe");
}

int main(void)
{
    int fails = 0;
    if (net_init() != 0) {
        fprintf(stderr, "net_init failed: %s\n", net_error_string());
        return 1;
    }

    char path[sizeof((struct sockaddr_un){0}).sun_path];
    snprintf(path, sizeof path, "/tmp/cweb-unix-%d.sock", (int)getpid());

    // a stale file must not block a restart: net_listen_unix clears it first
    {
        FILE *stale = fopen(path, "w");
        if (stale != NULL) {
            fputs("leftover", stale);
            fclose(stale);
        }
    }
    Socket_Handle srv = net_listen_unix(path);
    fails += check("unix listener bound over a stale file", srv != -1);
    {
        FILE *gone = fopen(path, "r");
        fails += check("socket file became a socket, not the stale file",
                       gone == NULL);
        if (gone != NULL) {
            fclose(gone);
        }
    }

    // raw connect/accept roundtrip over the unix socket
    Socket_Handle client = net_connect_unix(path);
    fails += check("unix client connected", client != -1);
    Socket_Handle peer = net_accept(srv);
    fails += check("unix server accepted", peer != -1);
    if (net_send_all(peer, "ping", 4) != 4) {
        fprintf(stderr, "unix server send failed: %s\n", net_error_string());
        return 1;
    }
    char buf[16] = {0};
    long n = net_recv(client, buf, sizeof(buf));
    fails += check("ping crossed the socket", n == 4 && memcmp(buf, "ping", 4) == 0);
    net_close(client);
    n = net_recv(peer, buf, sizeof(buf));
    fails += check("close seen as EOF over unix", n == 0);
    net_close(peer);

    if (fails != 0) {
        net_close(srv);
        net_unix_unlink(path);
        net_cleanup();
        return fails != 0;
    }

    // full HTTP round-trip: serve the framework over the unix socket
    pid_t child = fork();
    if (child == 0) {
        log_set_level(LOG_WARN);
        http_serve(srv, unix_handler, NULL);
        _exit(0);
    }

    Socket_Handle hc = net_connect_unix(path);
    fails += check("http client connected", hc != -1);
    const char *req = "GET / HTTP/1.1\r\nHost: localhost\r\n"
                      "Connection: close\r\n\r\n";
    if (net_send_all(hc, req, strlen(req)) != (long)strlen(req)) {
        fprintf(stderr, "http request send failed\n");
        return 1;
    }
    char body[4096];
    long got = 0;
    long r;
    while (got < (long)sizeof(body) - 1 && (r = net_recv(hc, body + got,
                                                         (size_t)(sizeof(body) - 1 - got))) > 0) {
        got += r;
    }
    body[got] = '\0';
    net_close(hc);

    fails += check("http 200 over unix", strstr(body, "HTTP/1.1 200 OK") != NULL);
    fails += check("body crossed the pipe",
                   strstr(body, "hello over the pipe") != NULL);

    kill(child, SIGTERM);
    waitpid(child, NULL, 0);
    net_close(srv);
    net_unix_unlink(path);
    fails += check("socket file removed on cleanup", access(path, F_OK) != 0);
    net_cleanup();

    if (fails != 0) {
        fprintf(stderr, "unexpected http response:\n%s\n", body);
        return 1;
    }
    printf("unix ok\n");
    return 0;
}