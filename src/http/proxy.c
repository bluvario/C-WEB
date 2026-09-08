#include "proxy.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "http_client.h"
#include "http.h"
#include "strbuf.h"
#include "sv.h"
#include "xmem.h"

typedef struct {
    char *upstream_base;
} Proxy_Mount;

static bool header_is(const String_View name, const char *want)
{
    size_t n = strlen(want);
    if (name.count != n) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (toupper((unsigned char)name.data[i]) !=
            toupper((unsigned char)want[i])) {
            return false;
        }
    }
    return true;
}

static bool req_may_forward(const String_View name)
{
    // hop-by-hop headers describe one connection and must be consumed by a
    // proxy, not relayed (RFC 9110 7.6.1). Host never travels either: the
    // client derives it from the upstream base.
    static const char *skip[] = {
        "connection", "keep-alive", "proxy-connection", "transfer-encoding",
        "te", "trailer", "upgrade", "expect", "content-length", "host",
    };
    for (size_t i = 0; i < sizeof skip / sizeof *skip; i++) {
        if (header_is(name, skip[i])) {
            return false;
        }
    }
    return true;
}

static bool res_may_forward(const String_View name)
{
    static const char *skip[] = {
        "connection", "keep-alive", "proxy-connection", "proxy-authenticate",
        "proxy-authorization", "transfer-encoding", "te", "trailer", "upgrade",
        "content-length",
    };
    for (size_t i = 0; i < sizeof skip / sizeof *skip; i++) {
        if (header_is(name, skip[i])) {
            return false;
        }
    }
    return true;
}

void http_reverse_proxy(Http_Request *req, Http_Response *res,
                        Str_Map *params, void *user_data)
{
    Proxy_Mount *m = user_data;
    (void)params;

    // the target keeps its path and query, only the authority changes;
    // origin-form was already guaranteed by the parser
    Strbuf target;
    strbuf_init(&target);
    strbuf_append(&target, req->target.data, req->target.count);
    strbuf_null_terminate(&target);

    // reassemble the surviving request headers as raw lines for the client
    Strbuf fwd;
    strbuf_init(&fwd);
    for (size_t i = 0; i < req->headers.count; i++) {
        Http_Header h = req->headers.items[i];
        if (!req_may_forward(h.key) || h.value.count == 0) {
            continue;
        }
        strbuf_append(&fwd, h.key.data, h.key.count);
        strbuf_append_cstr(&fwd, ": ");
        strbuf_append(&fwd, h.value.data, h.value.count);
        strbuf_append_cstr(&fwd, "\r\n");
    }
    strbuf_null_terminate(&fwd);

    // a proxy relays 3xx back to its caller, it does not chase them
    Http_Client *up = http_client_open(m->upstream_base);
    if (up == NULL) {
        http_response_set_status(res, HTTP_502_BAD_GATEWAY);
        http_response_set_header(res, "Content-Type", "text/plain; charset=utf-8");
        http_response_add_body_cstr(res, "502 bad gateway");
        strbuf_free(&target);
        strbuf_free(&fwd);
        return;
    }
    http_client_set_redirects(up, 0);

    // HEAD is served through the GET handler framework-wide, so it reaches
    // upstream as GET too; the server tears the body off afterwards and the
    // Content-Length still advertises the real entity
    Http_Method method = req->method == HTTP_HEAD ? HTTP_GET : req->method;

    Http_Client_Result rj;
    memset(&rj, 0, sizeof rj);
    int rc = http_client_req(up, target.items, method, fwd.items, req->body, &rj);
    if (rc != 0) {
        http_response_set_status(res, HTTP_502_BAD_GATEWAY);
        http_response_set_header(res, "Content-Type", "text/plain; charset=utf-8");
        http_response_add_body_cstr(res, "502 bad gateway");
        http_client_result_free(&rj);
        http_client_close(up);
        strbuf_free(&target);
        strbuf_free(&fwd);
        return;
    }

    http_response_set_status(res, rj.status);
    // relay the surviving upstream header lines verbatim, dropping the status
    // line and everything that would fight the proxy's own framing
    String_View head = (String_View){rj.headers.items, rj.headers.count};
    bool first = true;
    while (head.count > 0) {
        // the '\n' came off in the chop, the line's '\r' is still attached
        String_View line = sv_chop_by_delim(&head, '\n');
        if (first) {
            first = false; // the status line
            continue;
        }
        size_t c = 0;
        while (c < line.count && line.data[c] != ':') {
            c++;
        }
        String_View key = sv_trim((String_View){line.data, c});
        if (!res_may_forward(key)) {
            continue;
        }
        strbuf_append(&res->headers, line.data, line.count);
        strbuf_append_cstr(&res->headers, "\n");
    }

    if (rj.body.count > 0) {
        http_response_add_body(res, (String_View){rj.body.items, rj.body.count});
    }
    http_client_result_free(&rj);
    http_client_close(up);
    strbuf_free(&target);
    strbuf_free(&fwd);
}

int http_proxy_mount(Http_Router *r, const char *url_prefix, const char *upstream_base)
{
    Proxy_Mount *m = xcalloc(1, sizeof *m);
    m->upstream_base = xmalloc(strlen(upstream_base) + 1);
    strcpy(m->upstream_base, upstream_base);

    char pattern[1024];
    int n = snprintf(pattern, sizeof pattern, "%s/*", url_prefix);
    if (n <= 0 || (size_t)n >= sizeof pattern) {
        xfree(m->upstream_base);
        xfree(m);
        return -1;
    }

    // every method a server can receive must route into the same proxy
    Http_Method methods[] = {HTTP_GET, HTTP_POST, HTTP_PUT,
                             HTTP_DELETE, HTTP_PATCH, HTTP_OPTIONS};
    for (size_t i = 0; i < sizeof methods / sizeof *methods; i++) {
        if (router_add(r, methods[i], pattern, http_reverse_proxy, m) != 0) {
            return -1;
        }
    }
    return 0;
}

void http_proxy_unmount(Http_Router *r, const char *url_prefix)
{
    char pattern[1024];
    int n = snprintf(pattern, sizeof pattern, "%s/*", url_prefix);
    if (n <= 0 || (size_t)n >= sizeof pattern) {
        return;
    }
    // all six routes of one mount share a single Proxy_Mount, so free each
    // distinct pointer exactly once
    void *freed = NULL;
    for (size_t i = 0; i < r->count; i++) {
        Http_Route *route = &r->items[i];
        if (strcmp(route->pattern, pattern) == 0 && route->user_data != freed) {
            Proxy_Mount *m = route->user_data;
            xfree(m->upstream_base);
            xfree(m);
            freed = route->user_data;
        }
    }
}