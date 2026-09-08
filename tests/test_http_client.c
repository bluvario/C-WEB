#define _POSIX_C_SOURCE 200809L

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "http.h"
#include "http_client.h"
#include "log.h"
#include "net.h"
#include "request.h"
#include "response.h"
#include "router.h"
#include "server.h"
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

// echoes path, query, request body and whether the caller sent X-Custom
static void echo_handler(Http_Request *req, Http_Response *res,
                         Str_Map *params, void *user_data)
{
    (void)params;
    (void)user_data;
    http_response_set_header(res, "X-Echo", "on");
    Strbuf body;
    strbuf_init(&body);
    strbuf_append_cstr(&body, "echo:");
    strbuf_append(&body, req->path.data, req->path.count);
    strbuf_append_char(&body, '|');
    strbuf_append(&body, req->query.data, req->query.count);
    strbuf_append_char(&body, '|');
    strbuf_append(&body, req->body.data, req->body.count);
    strbuf_append_char(&body, '|');
    strbuf_append_cstr(&body, http_request_get_header(req, "X-Custom") != NULL ? "custom" : "none");
    http_response_add_body(res, (String_View){body.items, body.count});
    strbuf_free(&body);
}

// emits three streamed parts; the client must reassemble them
static int g_part = 0;

static size_t chunk_pump(void *buf, size_t cap, void *user_data)
{
    (void)user_data;
    static const char *const parts[] = {"one-", "two-", "three"};
    if (g_part >= 3) {
        return 0;
    }
    size_t n = strlen(parts[g_part]);
    g_part++;
    if (n > cap) {
        n = cap;
    }
    memcpy(buf, parts[g_part - 1], n);
    return n;
}

static void chunk_handler(Http_Request *req, Http_Response *res,
                          Str_Map *params, void *user_data)
{
    (void)req;
    (void)params;
    (void)user_data;
    http_response_set_stream(res, chunk_pump, NULL);
}

int main(void)
{
    int fails = 0;

    if (net_init() != 0) {
        return 1;
    }
    Socket_Handle srv = net_listen(0);
    if (srv == -1) {
        fprintf(stderr, "listen failed: %s\n", net_error_string());
        return 1;
    }
    int port = net_bound_port(srv);

    Http_Router router;
    router_init(&router);
    router_add(&router, HTTP_GET, "/echo", echo_handler, NULL);
    router_add(&router, HTTP_POST, "/echo", echo_handler, NULL);
    router_add(&router, HTTP_GET, "/chunks", chunk_handler, NULL);

    pid_t child = fork();
    if (child == 0) {
        log_set_level(LOG_WARN);
        http_serve(srv, router_dispatch, &router);
        _exit(0);
    }
    net_close(srv);

    char url[256];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d", port);

    // GET with query + custom header
    char get_url[320];
    snprintf(get_url, sizeof(get_url), "%s/echo?x=1", url);
    Http_Client_Result r;
    fails += check("GET succeeds",
                   http_client_request(get_url, HTTP_GET, "X-Custom: yes\r\n",
                                       (String_View){0}, &r) == 0 && r.error == NULL);
    fails += check("GET status is 200", r.status == HTTP_200_OK);
    fails += check("GET body assembled",
                   r.body.count == strlen("echo:/echo|x=1||custom") &&
                   memcmp(r.body.items, "echo:/echo|x=1||custom", r.body.count) == 0);
    fails += check("GET headers kept",
                   strstr(r.headers.items, "X-Echo: on\r\n") != NULL);
    http_client_result_free(&r);

    // POST body round-trips through the request parser
    String_View payload = sv_from_cstr("hello-payload");
    fails += check("POST succeeds",
                   http_client_post(url, payload, &r) == 0); // path "/" -> 404, but the call itself works
    fails += check("POST to / is a 404", r.status == HTTP_404_NOT_FOUND);
    http_client_result_free(&r);

    snprintf(get_url, sizeof(get_url), "%s/echo", url);
    fails += check("POST with body echoes it",
                   http_client_post(get_url, payload, &r) == 0 && r.status == HTTP_200_OK);
    fails += check("POST body text",
                   r.body.count == strlen("echo:/echo||hello-payload|none") &&
                   memcmp(r.body.items, "echo:/echo||hello-payload|none", r.body.count) == 0);
    http_client_result_free(&r);

    // a route that isn't registered still lands without transport error
    snprintf(get_url, sizeof(get_url), "%s/missing", url);
    fails += check("404 surfaces as status, not failure",
                   http_client_get(get_url, &r) == 0 && r.status == HTTP_404_NOT_FOUND);
    http_client_result_free(&r);

    // chunked streaming reassembled end to end
    g_part = 0;
    snprintf(get_url, sizeof(get_url), "%s/chunks", url);
    fails += check("chunked GET succeeds", http_client_get(get_url, &r) == 0);
    fails += check("chunked body reassembled",
                   r.body.count == strlen("one-two-three") &&
                   memcmp(r.body.items, "one-two-three", r.body.count) == 0);
    http_client_result_free(&r);

    // refusal to do https is explicit, not a hang
    fails += check("https is refused",
                   http_client_get("https://example.com/", &r) == -1 &&
                   r.error != NULL && strstr(r.error, "https") != NULL);
    http_client_result_free(&r);

    // a URL with no host at all is a parse error
    fails += check("bare scheme URL is a parse error",
                   http_client_get("http://", &r) == -1 && r.error != NULL);
    http_client_result_free(&r);

    // connect to a dead port: transient error with a reason
    Socket_Handle dead = net_listen(0);
    int dead_port = net_bound_port(dead);
    net_close(dead);
    snprintf(get_url, sizeof(get_url), "http://127.0.0.1:%d/", dead_port);
    fails += check("connect refused reported", http_client_get(get_url, &r) == -1);
    fails += check("connect error message present",
                   r.error != NULL && strlen(r.error) > 0);
    http_client_result_free(&r);

    router_free(&router);
    kill(child, SIGTERM);
    waitpid(child, NULL, 0);
    net_cleanup();

    if (fails == 0) {
        printf("http client ok\n");
    }
    return fails != 0;
}