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

    for (size_t i = 0; i < r->count; i++) {
        Http_Route *route = &r->items[i];

        if (route_match((Route_Def){route->method, route->pattern},
                        req->method, req->path, &params)) {
            route->handler(req, res, &params, route->user_data);
            strmap_free(&params);
            return; // first match wins
        }
    }

    http_response_set_status(res, HTTP_404_NOT_FOUND);
    http_response_set_header(res, "Content-Type", "text/plain; charset=utf-8");
    http_response_add_body_cstr(res, "404 not found");
    strmap_free(&params);
}