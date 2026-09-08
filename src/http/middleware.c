#define _POSIX_C_SOURCE 199309L

#include "middleware.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "date.h"
#include "xmem.h"

// each middleware becomes one node in a linked trampoline: the wrapper carries
// the middleware callback, its user data, and the function that represents the
// rest of the chain (the next node, or the final handler at the end)
typedef struct {
    Http_Middleware_Fn fn;
    void *mw_user;
    Http_Handler_Fn next;
    void *next_data;
} Node;

static void trampoline(Http_Request *req, Http_Response *res, void *data)
{
    Node *n = data;
    if (n->fn == NULL) {
        // empty chain: just run the handler the node wraps
        n->next(req, res, n->next_data);
        return;
    }
    n->fn(req, res, n->mw_user, n->next, n->next_data);
}

void http_middleware_init(Http_Middleware_Chain *c)
{
    c->items = NULL;
    c->count = 0;
    c->capacity = 0;
}

void http_middleware_free(Http_Middleware_Chain *c)
{
    // the user_data pointers belong to the caller, only the array is ours
    xfree(c->items);
    http_middleware_init(c);
}

int http_middleware_add(Http_Middleware_Chain *c, Http_Middleware_Fn fn,
                        void *user_data)
{
    if (c->count == c->capacity) {
        size_t nc = c->capacity ? c->capacity * 2 : 4;
        c->items = xrealloc(c->items, nc * sizeof(*c->items));
        c->capacity = nc;
    }
    c->items[c->count].fn = fn;
    c->items[c->count].user_data = user_data;
    c->count++;
    return 0;
}

Http_Handler_Fn http_middleware_build(Http_Middleware_Chain *c,
                                      Http_Handler_Fn final_handler,
                                      void *final_user_data,
                                      void **handler_data)
{
    // the caller always dispatches through the trampoline with the node array
    // it gets back, whether the chain is empty or not; that keeps the calling
    // convention one shape instead of two
    Node *nodes = xcalloc(c->count > 0 ? c->count : 1, sizeof(*nodes));
    if (c->count == 0) {
        nodes[0].fn = NULL;
        nodes[0].next = final_handler;
        nodes[0].next_data = final_user_data;
        *handler_data = nodes;
        return trampoline;
    }
    // nodes[i].next points at nodes[i+1] through the shared trampoline; a
    // single function serves every layer, only its user_data differs
    for (size_t i = 0; i < c->count; i++) {
        nodes[i].fn = c->items[i].fn;
        nodes[i].mw_user = c->items[i].user_data;
        if (i + 1 < c->count) {
            nodes[i].next = trampoline;
            nodes[i].next_data = &nodes[i + 1];
        } else {
            nodes[i].next = final_handler;
            nodes[i].next_data = final_user_data;
        }
    }
    *handler_data = nodes;
    return trampoline;
}

void http_middleware_data_free(void *handler_data)
{
    xfree(handler_data);
}

// wall-clock milliseconds, monotonic so NTP jumps do not skew request timing
static void clf_timestamp(char *buf, size_t n)
{
    time_t now = time(NULL);
    struct tm tm;
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    strftime(buf, n, "[%d/%b/%Y:%H:%M:%S %z]", &tm);
    if (buf[0] == '\0') {
        strcpy(buf, "[?/?/????:??:??:?? +0000]");
    }
}

void http_access_log_middleware(Http_Request *req, Http_Response *res,
                                void *user_data,
                                Http_Handler_Fn next, void *next_data)
{
    unsigned long long t0 = time_mono_ms();
    next(req, res, next_data);
    unsigned long long elapsed = time_mono_ms() - t0;

    Http_AccessLog_Opts *opts = user_data;
    FILE *f = (opts != NULL && opts->file != NULL) ? opts->file : stderr;

    const char *method = http_method_name(req->method);
    if (method == NULL) {
        method = "-";
    }
    char ts[64];
    clf_timestamp(ts, sizeof ts);
    fprintf(f, "- - %s \"%s %.*s %.*s\" %d %zu %llu\n",
            ts,
            method,
            (int)req->path.count, req->path.data,
            (int)req->version.count, req->version.data,
            (int)res->status,
            res->body.count, elapsed);
    fflush(f);
}