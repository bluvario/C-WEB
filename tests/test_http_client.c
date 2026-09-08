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

// strbuf payloads are not NUL-terminated, so strstr() would read past the
// allocation; search within explicit bounds instead
static int find_bytes(const char *hay, size_t hay_len, const char *needle)
{
    size_t n = strlen(needle);
    if (n == 0) {
        return 1;
    }
    if (hay_len < n) {
        return 0;
    }
    for (size_t i = 0; i + n <= hay_len; i++) {
        if (memcmp(hay + i, needle, n) == 0) {
            return 1;
        }
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
    g_part = 0; // each request replays the three parts
    http_response_set_stream(res, chunk_pump, NULL);
}

// streamed responses reuse the surrogate connection; this one demands the
// connection be shut right after, which must force the client to let go
static void close_handler(Http_Request *req, Http_Response *res,
                          Str_Map *params, void *user_data)
{
    echo_handler(req, res, params, user_data);
    http_response_set_header(res, "Connection", "close");
}

// bodyless statuses: the head carries no length and the connection stays
// open, so a client waiting for body bytes would hang
static void empty_handler(Http_Request *req, Http_Response *res,
                          Str_Map *params, void *user_data)
{
    (void)req;
    (void)params;
    (void)user_data;
    http_response_set_status(res, HTTP_204_NO_CONTENT);
}

static void stale_handler(Http_Request *req, Http_Response *res,
                          Str_Map *params, void *user_data)
{
    (void)req;
    (void)params;
    (void)user_data;
    http_response_set_status(res, HTTP_304_NOT_MODIFIED);
    http_response_set_header(res, "ETag", "\"deadbeef\"");
}

static void final_handler(Http_Request *req, Http_Response *res,
                          Str_Map *params, void *user_data)
{
    (void)req;
    (void)params;
    (void)user_data;
    http_response_add_body_cstr(res, "landed");
}

static void redir_handler(Http_Request *req, Http_Response *res,
                          Str_Map *params, void *user_data)
{
    (void)req;
    (void)params;
    (void)user_data;
    http_response_redirect(res, HTTP_302_FOUND, "/final");
}

// a Location without a leading slash must still resolve against the base host
static void relredir_handler(Http_Request *req, Http_Response *res,
                             Str_Map *params, void *user_data)
{
    (void)req;
    (void)params;
    (void)user_data;
    http_response_redirect(res, HTTP_302_FOUND, "final");
}

// 307 keeps the method, 303 must downgrade to GET
static void r307_handler(Http_Request *req, Http_Response *res,
                         Str_Map *params, void *user_data)
{
    (void)req;
    (void)params;
    (void)user_data;
    http_response_redirect(res, HTTP_307_TEMPORARY_REDIRECT, "/echo");
}

static void r303_handler(Http_Request *req, Http_Response *res,
                         Str_Map *params, void *user_data)
{
    (void)req;
    (void)params;
    (void)user_data;
    http_response_redirect(res, HTTP_303_SEE_OTHER, "/echo");
}

static void loop_handler(Http_Request *req, Http_Response *res,
                         Str_Map *params, void *user_data)
{
    (void)req;
    (void)params;
    (void)user_data;
    http_response_redirect(res, HTTP_302_FOUND, "/loop");
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
    router_add(&router, HTTP_GET, "/close", close_handler, NULL);
    router_add(&router, HTTP_GET, "/empty204", empty_handler, NULL);
    router_add(&router, HTTP_GET, "/stale304", stale_handler, NULL);
    router_add(&router, HTTP_GET, "/final", final_handler, NULL);
    router_add(&router, HTTP_GET, "/redir", redir_handler, NULL);
    router_add(&router, HTTP_GET, "/rel", relredir_handler, NULL);
    router_add(&router, HTTP_GET, "/loop", loop_handler, NULL);
    router_add(&router, HTTP_POST, "/r307", r307_handler, NULL);
    router_add(&router, HTTP_POST, "/r303", r303_handler, NULL);

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
                   find_bytes(r.headers.items, r.headers.count, "X-Echo: on\r\n"));
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

    // ---- persistent client: keep-alive reuse on one socket ----
    Http_Client *pc = http_client_open(url);
    fails += check("persistent client opened", pc != NULL);
    fails += check("no connection before first request", !http_client_keepalive_active(pc));

    fails += check("reuse req 1",
                   http_client_req(pc, "/echo?x=1", HTTP_GET, NULL,
                                   (String_View){0}, &r) == 0 && r.status == HTTP_200_OK);
    fails += check("reuse body 1",
                   r.body.count == strlen("echo:/echo|x=1||none") &&
                   memcmp(r.body.items, "echo:/echo|x=1||none", r.body.count) == 0);
    fails += check("server offered keep-alive",
                   find_bytes(r.headers.items, r.headers.count, "Connection: keep-alive\r\n"));
    fails += check("connection kept open", http_client_keepalive_active(pc));
    http_client_result_free(&r);

    fails += check("reuse req 2 on the same socket",
                   http_client_req_get(pc, "/echo", &r) == 0 && r.status == HTTP_200_OK);
    fails += check("still exactly one connection", http_client_connection_opens(pc) == 1);
    http_client_result_free(&r);

    // HEAD declares a Content-Length it never delivers; the client must not
    // wait for body bytes and must keep the socket in sync for the next call
    fails += check("HEAD succeeds",
                   http_client_req(pc, "/echo", HTTP_HEAD, NULL,
                                   (String_View){0}, &r) == 0 && r.status == HTTP_200_OK);
    fails += check("HEAD has no body", r.body.count == 0);
    char head_cl[64];
    snprintf(head_cl, sizeof(head_cl), "Content-Length: %zu\r\n",
             strlen("echo:/echo|||none"));
    fails += check("HEAD advertises the real length",
                   find_bytes(r.headers.items, r.headers.count, head_cl));
    fails += check("HEAD socket stays live", http_client_keepalive_active(pc));
    fails += check("HEAD did not reopen", http_client_connection_opens(pc) == 1);
    http_client_result_free(&r);

    fails += check("followup after HEAD on the same socket",
                   http_client_req_get(pc, "/echo", &r) == 0 && r.status == HTTP_200_OK);
    fails += check("still one connection after HEAD", http_client_connection_opens(pc) == 1);
    http_client_result_free(&r);

    // 204 and 304 send a head without any length and hold the connection
    // open; the client must not wait for body bytes that will never arrive
    fails += check("204 succeeds",
                   http_client_req_get(pc, "/empty204", &r) == 0 &&
                   r.status == HTTP_204_NO_CONTENT);
    fails += check("204 has no body", r.body.count == 0);
    fails += check("204 socket still live", http_client_keepalive_active(pc));
    fails += check("204 kept one connection", http_client_connection_opens(pc) == 1);
    http_client_result_free(&r);

    fails += check("304 succeeds",
                   http_client_req_get(pc, "/stale304", &r) == 0 &&
                   r.status == HTTP_304_NOT_MODIFIED);
    fails += check("304 has no body", r.body.count == 0);
    fails += check("304 headers kept",
                   find_bytes(r.headers.items, r.headers.count, "ETag: \"deadbeef\"\r\n"));
    fails += check("304 socket still live", http_client_keepalive_active(pc));
    fails += check("304 kept one connection", http_client_connection_opens(pc) == 1);
    http_client_result_free(&r);

    fails += check("followup after 304 on the same socket",
                   http_client_req_get(pc, "/echo", &r) == 0 && r.status == HTTP_200_OK);
    fails += check("still one connection after bodyless", http_client_connection_opens(pc) == 1);
    http_client_result_free(&r);

    // chunked responses must leave the socket in sync for the next request
    fails += check("reuse chunked req",
                   http_client_req_get(pc, "/chunks", &r) == 0 && r.status == HTTP_200_OK);
    fails += check("reuse chunked body",
                   r.body.count == strlen("one-two-three") &&
                   memcmp(r.body.items, "one-two-three", r.body.count) == 0);
    fails += check("chunked socket still live", http_client_keepalive_active(pc));
    fails += check("still one connection after chunked", http_client_connection_opens(pc) == 1);
    http_client_result_free(&r);

    fails += check("post-chunk followup on the same socket",
                   http_client_req_get(pc, "/echo", &r) == 0 && r.status == HTTP_200_OK);
    fails += check("still one connection after followup", http_client_connection_opens(pc) == 1);
    http_client_result_free(&r);

    // a response demanding Connection: close must drop the socket
    fails += check("close-request succeeds",
                   http_client_req_get(pc, "/close", &r) == 0 && r.status == HTTP_200_OK);
    fails += check("client dropped the socket", !http_client_keepalive_active(pc));
    http_client_result_free(&r);

    fails += check("reconnects automatically",
                   http_client_req_get(pc, "/echo", &r) == 0 && r.status == HTTP_200_OK);
    fails += check("two connections after reconnect", http_client_connection_opens(pc) == 2);
    http_client_result_free(&r);

    // with no base URL, relative targets are refused up front
    Http_Client *pc2 = http_client_open(NULL);
    fails += check("no-base client refuses relative targets",
                   http_client_req_get(pc2, "/echo", &r) == -1 &&
                   r.error != NULL && strstr(r.error, "base URL") != NULL);
    http_client_result_free(&r);
    fails += check("garbage base url refused", http_client_open("not a url") == NULL);
    http_client_close(pc2);

    // let the server's 5s idle timeout kill the keep-alive connection, then a
    // GET must silently reopen instead of erroring
    sleep(6);
    fails += check("dead-keepalive GET reconnects",
                   http_client_req_get(pc, "/echo", &r) == 0 && r.status == HTTP_200_OK);
    fails += check("reopened exactly once", http_client_connection_opens(pc) == 3);
    http_client_result_free(&r);

    // ---- redirects ----
    fails += check("302 followed to final",
                   http_client_req_get(pc, "/redir", &r) == 0 &&
                   r.status == HTTP_200_OK && r.redirects == 1);
    fails += check("redirect landing body",
                   r.body.count == strlen("landed") &&
                   memcmp(r.body.items, "landed", r.body.count) == 0);
    http_client_result_free(&r);

    fails += check("relative location resolves to base",
                   http_client_req_get(pc, "/rel", &r) == 0 &&
                   r.status == HTTP_200_OK);
    http_client_result_free(&r);

    fails += check("redirect loop capped at the default",
                   http_client_req_get(pc, "/loop", &r) == 0 &&
                   r.status == HTTP_302_FOUND && r.redirects == 5);
    http_client_result_free(&r);

    // 307 must carry the method and body through to the destination
    fails += check("307 preserves POST",
                   http_client_req_post(pc, "/r307", payload, &r) == 0 &&
                   r.status == HTTP_200_OK && r.redirects == 1);
    fails += check("307 body survived the hop",
                   find_bytes(r.body.items, r.body.count, "hello-payload"));
    http_client_result_free(&r);

    // 303 lands on GET and never carries the original body
    fails += check("303 downgrades POST to GET",
                   http_client_req_post(pc, "/r303", payload, &r) == 0 &&
                   r.status == HTTP_200_OK && r.redirects == 1);
    fails += check("303 body dropped",
                   !find_bytes(r.body.items, r.body.count, "hello-payload"));
    http_client_result_free(&r);

    // redirects are a client policy and can be switched off
    http_client_set_redirects(pc, 0);
    fails += check("redirects disabled returns the 302",
                   http_client_req_get(pc, "/redir", &r) == 0 &&
                   r.status == HTTP_302_FOUND && r.redirects == 0);
    http_client_result_free(&r);
    http_client_set_redirects(pc, 5);

    http_client_close(pc);

    router_free(&router);
    kill(child, SIGTERM);
    waitpid(child, NULL, 0);
    net_cleanup();

    if (fails == 0) {
        printf("http client ok\n");
    }
    return fails != 0;
}