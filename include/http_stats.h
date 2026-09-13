#ifndef CWEB_HTTP_STATS_H
#define CWEB_HTTP_STATS_H

#include <stdatomic.h>
#include <stddef.h>

#include "middleware.h"

// per-process request telemetry, collected by http_stats_middleware (or by
// hand-rolled handlers with http_stats_record) and read at any point, most
// usefully by a --healthz endpoint and in the server's shutdown banner. all
// counters are atomics, so worker threads increment without a shared lock and
// readers see a consistent-enough watermark for a probe. counters are never
// reset by the library; owners zero them with http_stats_init once at boot.
typedef struct {
    _Atomic unsigned long long requests;      // calls that entered the chain
    _Atomic unsigned long long bytes_written; // response body bytes (post-gzip)
    _Atomic unsigned long long status_2xx;    // per-status-class buckets
    _Atomic unsigned long long status_3xx;
    _Atomic unsigned long long status_4xx;
    _Atomic unsigned long long status_5xx;
} Http_Stats;

// zeroes every counter. also the correct way to reset the totals at a sync
// point (e.g. when a monitoring run wants a fresh window).
void http_stats_init(Http_Stats *s);

// counts one finished request into s. call it in code that bypasses the
// middleware (a hand-written server or a test driver) to stay consistent with
// the middleware's numbers; it increments requests, bytes_written and the
// status bucket for the response's final status.
void http_stats_record(Http_Stats *s, Http_Request *req, Http_Response *res);

// drop-in middleware: runs the rest of the chain inside, then records the
// finished request into the Http_Stats* passed as user_data. place it
// outermost (add it first) so it measures the final response - compressed,
// redirected, rejected and all. a NULL user_data leaves it inert.
void http_stats_middleware(Http_Request *req, Http_Response *res,
                           void *user_data,
                           Http_Handler_Fn next, void *next_data);

// renders the counters as one plain-text line ("requests N; bytes N; 2xx N
// 3xx N 4xx N 5xx N; uptime Ns"), uptime_ms passed by the caller. returns the
// number of characters written (the snprintf contract).
size_t http_stats_write(Http_Stats *s, char *out, size_t out_sz,
                        unsigned long long uptime_ms);

#endif