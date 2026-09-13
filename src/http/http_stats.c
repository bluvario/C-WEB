#include "http_stats.h"

#include <stdio.h>

#include "request.h"
#include "response.h"

void http_stats_init(Http_Stats *s)
{
    atomic_store_explicit(&s->requests, 0, memory_order_relaxed);
    atomic_store_explicit(&s->bytes_written, 0, memory_order_relaxed);
    atomic_store_explicit(&s->status_2xx, 0, memory_order_relaxed);
    atomic_store_explicit(&s->status_3xx, 0, memory_order_relaxed);
    atomic_store_explicit(&s->status_4xx, 0, memory_order_relaxed);
    atomic_store_explicit(&s->status_5xx, 0, memory_order_relaxed);
}

void http_stats_record(Http_Stats *s, Http_Request *req, Http_Response *res)
{
    (void)req;
    atomic_fetch_add_explicit(&s->requests, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&s->bytes_written,
                              (unsigned long long)res->body.count,
                              memory_order_relaxed);
    int code = res->status;
    _Atomic unsigned long long *bucket = NULL;
    if (code >= 200 && code < 300) {
        bucket = &s->status_2xx;
    } else if (code >= 300 && code < 400) {
        bucket = &s->status_3xx;
    } else if (code >= 400 && code < 500) {
        bucket = &s->status_4xx;
    } else if (code >= 500 && code < 600) {
        bucket = &s->status_5xx;
    }
    if (bucket != NULL) {
        atomic_fetch_add_explicit(bucket, 1, memory_order_relaxed);
    }
}

void http_stats_middleware(Http_Request *req, Http_Response *res,
                           void *user_data,
                           Http_Handler_Fn next, void *next_data)
{
    Http_Stats *s = user_data;
    if (s == NULL) {
        next(req, res, next_data);
        return;
    }
    next(req, res, next_data);
    // the chain has fully run by the time control returns here, so status and
    // body are the final ones the server will send (gzip already rewrote the
    // body in place for compressible responses)
    http_stats_record(s, req, res);
}

size_t http_stats_write(Http_Stats *s, char *out, size_t out_sz,
                        unsigned long long uptime_ms)
{
    return (size_t)snprintf(out, out_sz,
                            "requests %llu; bytes %llu; "
                            "2xx %llu 3xx %llu 4xx %llu 5xx %llu; uptime %llus\n",
                            atomic_load_explicit(&s->requests, memory_order_relaxed),
                            atomic_load_explicit(&s->bytes_written, memory_order_relaxed),
                            atomic_load_explicit(&s->status_2xx, memory_order_relaxed),
                            atomic_load_explicit(&s->status_3xx, memory_order_relaxed),
                            atomic_load_explicit(&s->status_4xx, memory_order_relaxed),
                            atomic_load_explicit(&s->status_5xx, memory_order_relaxed),
                            uptime_ms / 1000);
}