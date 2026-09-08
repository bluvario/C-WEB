#include "rate_limit.h"

#include <stdio.h>
#include <string.h>

#include "date.h"
#include "xmem.h"

void http_rate_limiter_init(Http_RateLimiter *rl, double rate, double burst)
{
    rl->items = NULL;
    rl->count = 0;
    rl->capacity = 0;
    rl->rate = rate;
    rl->burst = burst;
    rl->clock_ms = NULL;
    rl->key_fn = NULL;
    rl->key_user = NULL;
}

void http_rate_limiter_free(Http_RateLimiter *rl)
{
    for (size_t i = 0; i < rl->count; i++) {
        xfree(rl->items[i].key);
    }
    xfree(rl->items);
    http_rate_limiter_init(rl, 0, 0);
}

void http_rate_limiter_clear(Http_RateLimiter *rl)
{
    for (size_t i = 0; i < rl->count; i++) {
        xfree(rl->items[i].key);
    }
    rl->count = 0;
}

// one bucket per key (owned copy of the key string). the table stays small —
// a handful of routes or callers — so linear search is fine and keeps a hash
// table out of the way.
static Http_RateLimit_Bucket *bucket_for(Http_RateLimiter *rl, String_View key,
                                         unsigned long long now)
{
    char *flat = xmalloc(key.count + 1);
    if (key.count > 0) {
        memcpy(flat, key.data, key.count);
    }
    flat[key.count] = '\0';

    for (size_t i = 0; i < rl->count; i++) {
        if (strcmp(rl->items[i].key, flat) == 0) {
            xfree(flat);
            return &rl->items[i];
        }
    }

    if (rl->count == rl->capacity) {
        size_t nc = rl->capacity ? rl->capacity * 2 : 8;
        rl->items = xrealloc(rl->items, nc * sizeof(*rl->items));
        rl->capacity = nc;
    }
    Http_RateLimit_Bucket *b = &rl->items[rl->count];
    b->key = flat;
    b->tokens = rl->burst; // a new bucket starts full
    b->last_ms = now;      // and counts its age from this instant
    rl->count++;
    return b;
}

int http_rate_limiter_allow(Http_RateLimiter *rl, String_View key)
{
    unsigned long long now = rl->clock_ms != NULL ? rl->clock_ms()
                                                  : time_mono_ms();
    Http_RateLimit_Bucket *b = bucket_for(rl, key, now);
    // clamp the delta: a clock that ever steps backward must not mint tokens
    // out of an unsigned underflow
    unsigned long long delta = now > b->last_ms ? now - b->last_ms : 0;
    double dt = (double)delta / 1000.0;
    b->tokens += dt * rl->rate;
    if (b->tokens > rl->burst) {
        b->tokens = rl->burst;
    }
    b->last_ms = now;

    if (b->tokens >= 1.0) {
        b->tokens -= 1.0;
        return 0;
    }
    return -1;
}

void http_rate_limit_middleware(Http_Request *req, Http_Response *res,
                                void *user_data,
                                Http_Handler_Fn next, void *next_data)
{
    Http_RateLimiter *rl = user_data;
    String_View key = rl->key_fn != NULL ? rl->key_fn(req, rl->key_user)
                                         : req->path;
    if (key.count == 0) {
        key = sv_from_cstr("/");
    }

    if (http_rate_limiter_allow(rl, key) == 0) {
        next(req, res, next_data);
        return;
    }

    res->status = HTTP_429_TOO_MANY_REQUESTS;
    // the bucket was just refilled by allow() before it said no; roughly how
    // long until it holds a whole fresh token. recompute the refill so the
    // hint reflects the same instant allow() measured.
    unsigned long long now = rl->clock_ms != NULL ? rl->clock_ms()
                                                  : time_mono_ms();
    Http_RateLimit_Bucket *b = bucket_for(rl, key, now);
    double retry = (1.0 - b->tokens) / rl->rate;
    char buf[32];
    if (retry < 0.0001) {
        strcpy(buf, "1");
    } else {
        snprintf(buf, sizeof buf, "%lld", (long long)(retry + 0.999));
    }
    http_response_set_header(res, "Retry-After", buf);
}