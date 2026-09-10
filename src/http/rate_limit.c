#include "rate_limit.h"

#include <stdio.h>
#include <string.h>

#include "base64.h"
#include "date.h"
#include "strbuf.h"
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

// read-only bucket lookup; returns NULL if the key has no bucket yet.
static Http_RateLimit_Bucket *find_bucket(const Http_RateLimiter *rl,
                                          String_View key)
{
    char flat[key.count + 1];
    if (key.count > 0) {
        memcpy(flat, key.data, key.count);
    }
    flat[key.count] = '\0';
    for (size_t i = 0; i < rl->count; i++) {
        if (strcmp(rl->items[i].key, flat) == 0) {
            return &rl->items[i];
        }
    }
    return NULL;
}

void http_rate_limiter_status(const Http_RateLimiter *rl, String_View key,
                              Http_RateLimit_Status *out)
{
    out->limit = (int)rl->burst;
    out->remaining = 0;
    out->reset = 0;
    const Http_RateLimit_Bucket *b = find_bucket(rl, key);
    if (b == NULL) {
        out->remaining = out->limit;
        return;
    }
    out->remaining = b->tokens >= 0.0 ? (int)b->tokens : 0;
    if (rl->rate > 0.0 && b->tokens < rl->burst) {
        double secs = (rl->burst - b->tokens) / rl->rate;
        out->reset = (int)(secs + 0.999);
    }
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

// shared decode scratch for the claimed-username step below: the view it vends
// only has to outlive the middleware's immediate allow()/status() calls, so a
// per-thread buffer (like template.c's capture buffers) is safe to reuse
static _Thread_local Strbuf key_buf;

String_View http_rate_limit_user_key(Http_Request *req, void *user_data)
{
    (void)user_data;

    // a verified identity recorded by an outer middleware owns the budget:
    // one account = one bucket, wherever its requests come from
    if (req->auth_user.count > 0) {
        return req->auth_user;
    }

    // credentials still being checked? the claimed username throttles the
    // account before the password is verified, so a fresh caller IP cannot
    // reroll a bucket on each guess and brute-forcing one account is limited
    // no matter how many addresses the attacker spreads across
    const String_View *auth = http_request_get_header(req, "authorization");
    if (auth != NULL && auth->count >= 6 && auth->data[5] == ' ' &&
        (auth->data[0] == 'b' || auth->data[0] == 'B') &&
        (auth->data[1] == 'a' || auth->data[1] == 'A') &&
        (auth->data[2] == 's' || auth->data[2] == 'S') &&
        (auth->data[3] == 'i' || auth->data[3] == 'I') &&
        (auth->data[4] == 'c' || auth->data[4] == 'C')) {
        String_View payload = {auth->data + 6, auth->count - 6};
        if (key_buf.items == NULL) {
            strbuf_init(&key_buf);
        }
        key_buf.count = 0;
        if (base64_decode_into(&key_buf, payload) == 0) {
            // the username is everything before the first ':'; an empty user
            // or a missing colon yields no identity, so fall through to the
            // caller's address below
            for (size_t i = 0; i < key_buf.count; i++) {
                if (key_buf.items[i] == ':') {
                    if (i > 0) {
                        return (String_View){key_buf.items, i};
                    }
                    break;
                }
            }
        }
    }

    // nobody claimed an identity: bill the calling address
    return req->remote;
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

    int allowed = http_rate_limiter_allow(rl, key) == 0;

    Http_RateLimit_Status st;
    http_rate_limiter_status(rl, key, &st);
    char buf[32];
    snprintf(buf, sizeof buf, "%d", st.limit);
    http_response_set_header(res, "X-RateLimit-Limit", buf);
    snprintf(buf, sizeof buf, "%d", st.remaining);
    http_response_set_header(res, "X-RateLimit-Remaining", buf);
    snprintf(buf, sizeof buf, "%d", st.reset);
    http_response_set_header(res, "X-RateLimit-Reset", buf);

    if (allowed) {
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
    if (retry < 0.0001) {
        strcpy(buf, "1");
    } else {
        snprintf(buf, sizeof buf, "%lld", (long long)(retry + 0.999));
    }
    http_response_set_header(res, "Retry-After", buf);
}