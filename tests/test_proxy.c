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
#include "proxy.h"
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

static size_t count_bytes(const char *hay, size_t hay_len, const char *needle)
{
    size_t n = strlen(needle);
    size_t hits = 0;
    if (hay_len < n) {
        return 0;
    }
    for (size_t i = 0; i + n <= hay_len; i++) {
        if (memcmp(hay + i, needle, n) == 0) {
            hits++;
        }
    }
    return hits;
}

// the upstream records everything it saw so the assertions can prove the
// proxy forwarded method, path, query, body and event headers whole
static void capture_handler(Http_Request *req, Http_Response *res,
                            Str_Map *params, void *user_data)
{
    (void)params;
    (void)user_data;
    http_response_set_header(res, "X-Upstream", "yes");
    Strbuf body;
    strbuf_init(&body);
    strbuf_append_cstr(&body, "method=");
    strbuf_append_cstr(&body, http_method_name(req->method));
    strbuf_append_cstr(&body, "&path=");
    strbuf_append(&body, req->path.data, req->path.count);
    strbuf_append_cstr(&body, "&query=");
    strbuf_append(&body, req->query.data, req->query.count);
    strbuf_append_cstr(&body, "&custom=");
    const String_View *custom = http_request_get_header(req, "X-Custom");
    if (custom != NULL) {
        strbuf_append(&body, custom->data, custom->count);
    } else {
        strbuf_append_cstr(&body, "none");
    }
    strbuf_append_cstr(&body, "&body=");
    strbuf_append(&body, req->body.data, req->body.count);
    http_response_set_status(res, HTTP_200_OK);
    http_response_add_body(res, (String_View){body.items, body.count});
    strbuf_free(&body);
}

static void miss_handler(Http_Request *req, Http_Response *res,
                         Str_Map *params, void *user_data)
{
    (void)req;
    (void)params;
    (void)user_data;
    http_response_set_status(res, HTTP_404_NOT_FOUND);
    http_response_add_body_cstr(res, "upstream miss");
}

int main(void)
{
    int fails = 0;

    if (net_init() != 0) {
        return 1;
    }

    // --- upstream origin server ---
    // upstream routes match the proxied path as-is because the proxy passes
    // the target through unchanged (prefix included)
    Http_Router up_router;
    router_init(&up_router);
    router_add(&up_router, HTTP_GET, "/up/capture", capture_handler, NULL);
    router_add(&up_router, HTTP_POST, "/up/capture", capture_handler, NULL);
    router_add(&up_router, HTTP_PUT, "/up/capture", capture_handler, NULL);
    router_add(&up_router, HTTP_GET, "/up/miss", miss_handler, NULL);

    Socket_Handle up_srv = net_listen(0);
    if (up_srv == -1) {
        fprintf(stderr, "upstream listen failed: %s\n", net_error_string());
        return 1;
    }
    int up_port = net_bound_port(up_srv);

    pid_t up_child = fork();
    if (up_child == 0) {
        log_set_level(LOG_WARN);
        http_serve(up_srv, router_dispatch, &up_router);
        _exit(0);
    }
    net_close(up_srv);

    // --- the proxy itself, mounted in front of the origin ---
    Http_Router pr_router;
    router_init(&pr_router);
    char upstream[256];
    snprintf(upstream, sizeof(upstream), "http://127.0.0.1:%d", up_port);
    if (http_proxy_mount(&pr_router, "/up", upstream) != 0) {
        fprintf(stderr, "proxy mount failed\n");
        return 1;
    }

    Socket_Handle pr_srv = net_listen(0);
    if (pr_srv == -1) {
        fprintf(stderr, "proxy listen failed: %s\n", net_error_string());
        return 1;
    }
    int pr_port = net_bound_port(pr_srv);

    pid_t pr_child = fork();
    if (pr_child == 0) {
        log_set_level(LOG_WARN);
        http_serve(pr_srv, router_dispatch, &pr_router);
        _exit(0);
    }
    net_close(pr_srv);

    char url[256];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d", pr_port);
    char target[320];

    Http_Client_Result r;

    // the request line travels byte for byte: /up/capture?q=42 reaches the
    // origin as the same path and query
    snprintf(target, sizeof(target), "%s/up/capture?q=42", url);
    fails += check("proxied GET succeeds",
                   http_client_get(target, &r) == 0 && r.error == NULL);
    fails += check("proxied GET status", r.status == HTTP_200_OK);
    fails += check("proxied GET path+query forwarded",
                   r.body.count == strlen("method=GET&path=/up/capture&query=q=42&custom=none&body=") &&
                   memcmp(r.body.items, "method=GET&path=/up/capture&query=q=42&custom=none&body=", r.body.count) == 0);
    fails += check("origin headers relayed",
                   find_bytes(r.headers.items, r.headers.count, "X-Upstream: yes\r\n"));
    http_client_result_free(&r);

    // hop-by-hop headers are consumed at the proxy: the origin's own
    // Connection and Content-Length must not surface next to the proxy's
    snprintf(target, sizeof(target), "%s/up/capture", url);
    fails += check("GET again for framing", http_client_get(target, &r) == 0);
    fails += check("exactly one Connection line",
                   count_bytes(r.headers.items, r.headers.count, "Connection:") == 1);
    fails += check("exactly one Content-Length line",
                   count_bytes(r.headers.items, r.headers.count, "Content-Length:") == 1);
    http_client_result_free(&r);

    // the request body and an end-to-end header must ride through
    String_View payload = sv_from_cstr("ping");
    snprintf(target, sizeof(target), "%s/up/capture", url);
    fails += check("proxied POST succeeds",
                   http_client_request(target, HTTP_POST, "X-Custom: abc\r\n",
                                       payload, &r) == 0 && r.status == HTTP_200_OK);
    fails += check("POST body and header forwarded",
                   r.body.count == strlen("method=POST&path=/up/capture&query=&custom=abc&body=ping") &&
                   memcmp(r.body.items, "method=POST&path=/up/capture&query=&custom=abc&body=ping", r.body.count) == 0);
    http_client_result_free(&r);

    // PUT routes into the same mount
    snprintf(target, sizeof(target), "%s/up/capture", url);
    fails += check("proxied PUT succeeded",
                   http_client_request(target, HTTP_PUT, NULL,
                                       (String_View){0}, &r) == 0 && r.status == HTTP_200_OK);
    fails += check("PUT method forwarded",
                   r.body.count == strlen("method=PUT&path=/up/capture&query=&custom=none&body=") &&
                   memcmp(r.body.items, "method=PUT&path=/up/capture&query=&custom=none&body=", r.body.count) == 0);
    http_client_result_free(&r);

    // an upstream failure surfaces as the real status and body, not a
    // framework-generated reply
    snprintf(target, sizeof(target), "%s/up/miss", url);
    fails += check("upstream 404 passes through",
                   http_client_get(target, &r) == 0 && r.status == HTTP_404_NOT_FOUND);
    fails += check("upstream 404 body passes through",
                   r.body.count == strlen("upstream miss") &&
                   memcmp(r.body.items, "upstream miss", r.body.count) == 0);
    http_client_result_free(&r);

    // a proxied URL that is not under the mount is the proxy's own 404
    snprintf(target, sizeof(target), "%s/elsewhere", url);
    fails += check("outside the mount is a plain 404",
                   http_client_get(target, &r) == 0 && r.status == HTTP_404_NOT_FOUND);
    http_client_result_free(&r);

    // kill the origin: the proxy must own the failure with a 502
    kill(up_child, SIGTERM);
    waitpid(up_child, NULL, 0);
    snprintf(target, sizeof(target), "%s/up/capture", url);
    fails += check("dead origin yields 502",
                   http_client_get(target, &r) == 0 && r.status == HTTP_502_BAD_GATEWAY);
    fails += check("502 carries an explanation",
                   r.body.count == strlen("502 bad gateway") &&
                   memcmp(r.body.items, "502 bad gateway", r.body.count) == 0);
    http_client_result_free(&r);

    kill(pr_child, SIGTERM);
    waitpid(pr_child, NULL, 0);
    http_proxy_unmount(&pr_router, "/up");
    router_free(&pr_router);
    router_free(&up_router);

    if (fails == 0) {
        printf("proxy ok\n");
        return 0;
    }
    fprintf(stderr, "%d FAILURES\n", fails);
    return 1;
}