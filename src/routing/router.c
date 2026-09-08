#include "router.h"

#include <string.h>

#include "da.h"
#include "http.h"
#include "params.h"
#include "request.h"
#include "response.h"
#include "route.h"
#include "strmap.h"
#include "xmem.h"

void router_init(Http_Router *r)
{
    r->items = NULL;
    r->count = 0;
    r->capacity = 0;
}

void router_free(Http_Router *r)
{
    for (size_t i = 0; i < r->count; i++) {
        xfree(r->items[i].pattern);
    }
    xfree(r->items);
    router_init(r);
}

int router_add(Http_Router *r, Http_Method method, const char *pattern,
               Http_Route_Handler handler, void *user_data)
{
    Http_Route route;
    route.method = method;
    route.pattern = xmalloc(strlen(pattern) + 1);
    strcpy(route.pattern, pattern);
    route.handler = handler;
    route.user_data = user_data;

    da_append(r, route);
    return 0;
}

// is name already one of the comma-separated tokens in allow[0..len-1]?
static bool allow_has(const char *allow, size_t len, const char *name)
{
    size_t nlen = strlen(name);
    size_t i = 0;
    while (i < len) {
        while (i < len && allow[i] == ' ') {
            i++;
        }
        size_t start = i;
        while (i < len && allow[i] != ',') {
            i++;
        }
        if (i - start == nlen && strncmp(allow + start, name, nlen) == 0) {
            return true;
        }
        if (i < len) {
            i++; // hop the comma
        }
    }
    return false;
}

void router_dispatch(Http_Request *req, Http_Response *res, void *user_data)
{
    Http_Router *r = user_data;

    // merges query and form-body values, added before captures so a <name> in
    // the pattern has the final word
    Str_Map params;
    strmap_init(&params);
    if (request_merge_params(req, &params) != 0) {
        http_response_set_status(res, HTTP_400_BAD_REQUEST);
        http_response_set_header(res, "Content-Type", "text/plain; charset=utf-8");
        http_response_add_body_cstr(res, "400 malformed request data");
        strmap_free(&params);
        return;
    }

    // the path may exist under other methods: collect them for the Allow
    // header (405) instead of answering 404. a HEAD request is served by the
    // GET handler with the body torn off afterwards.
    bool as_head = req->method == HTTP_HEAD;
    Http_Method want = as_head ? HTTP_GET : req->method;
    bool path_found = false;
    char allow[128];
    size_t allow_len = 0;

    for (size_t i = 0; i < r->count; i++) {
        Http_Route *route = &r->items[i];
        bool method_matches = route->method == want;

        if (route_path_matches(route->pattern, req->path, method_matches ? &params : NULL)) {
            if (method_matches) {
                route->handler(req, res, &params, route->user_data);
                if (as_head) {
                    res->suppress_body = true; // spill the body, keep length
                }
                strmap_free(&params);
                return; // first match wins
            }
            path_found = true;
            const char *name = http_method_name(route->method);
            if (name == NULL) {
                continue;
            }
            if (!allow_has(allow, allow_len, name)) {
                size_t nlen = strlen(name);
                if (allow_len > 0) {
                    allow[allow_len++] = ',';
                    allow[allow_len++] = ' ';
                }
                memcpy(allow + allow_len, name, nlen);
                allow_len += nlen;
                allow[allow_len] = '\0';
            }
        }
    }

    if (path_found) {
        if (req->method == HTTP_OPTIONS) {
            // OPTIONS asks what a resource allows, not for the resource
            http_response_set_status(res, HTTP_200_OK);
            if (allow_len > 0) {
                http_response_set_header(res, "Allow", allow);
            }
        } else {
            http_response_set_status(res, HTTP_405_METHOD_NOT_ALLOWED);
            http_response_set_header(res, "Content-Type", "text/plain; charset=utf-8");
            if (allow_len > 0) {
                http_response_set_header(res, "Allow", allow);
            }
            http_response_add_body_cstr(res, "405 method not allowed");
        }
    } else {
        http_response_set_status(res, HTTP_404_NOT_FOUND);
        http_response_set_header(res, "Content-Type", "text/plain; charset=utf-8");
        http_response_add_body_cstr(res, "404 not found");
    }
    strmap_free(&params);
}