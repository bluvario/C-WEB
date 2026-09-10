#ifndef CWEB_RATE_LIMIT_H
#define CWEB_RATE_LIMIT_H

#include "middleware.h"
#include "sv.h"

// token-bucket rate limiter, keyed per caller. each key's bucket refills at
// `rate` tokens per second up to a `burst` ceiling, so a bucket holds burst
// instant arrivals and slower traffic afterwards flows at rate/second. the
// request's key is any String_View; see key_fn below for the middleware.

typedef String_View (*Http_RateLimit_Key_Fn)(Http_Request *req, void *user_data);

typedef struct {
    char *key;          // owned
    double tokens;      // current bucket contents
    unsigned long long last_ms; // when the bucket was last touched
} Http_RateLimit_Bucket;

typedef struct {
    Http_RateLimit_Bucket *items;
    size_t count;
    size_t capacity;
    double rate;  // tokens added per second
    double burst; // maximum tokens a bucket can hold
    // injectable clock so tests can fast-forward time; NULL uses the real
    // monotonic millisecond clock
    unsigned long long (*clock_ms)(void);
    // how the middleware names a caller; NULL rate-limits per request path
    Http_RateLimit_Key_Fn key_fn;
    void *key_user;
} Http_RateLimiter;

// bucket snapshot returned by http_rate_limiter_status()
typedef struct {
    int limit;      // burst ceiling (max tokens)
    int remaining;  // tokens left after the most recent allow()
    int reset;      // seconds until the bucket refills to full
} Http_RateLimit_Status;

// built-in identity-aware key, useful when some requests carry credentials:
//
//   1. req->auth_user (a verified identity already recorded by an outer
//      middleware such as http_basic_auth_middleware) owns the bucket, so
//      every request from a logged-in account draws from one budget no matter
//      which address it arrives from;
//   2. otherwise a "Basic" Authorization header's *claimed* username owns the
//      bucket even before the password is verified, which throttles guessing
//      one account across every caller IP instead of letting a fresh address
//      reroll the budget on each attempt;
//   3. otherwise the caller's req->remote address.
//
// step 2 only needs the header, so it also throttles failed logins: put this
// key function on a limiter that runs *outside* the auth guard and a
// brute-forcer burns the account's bucket on every wrong try. as with any
// key_fn the returned view only has to stay valid until the middleware's
// allow()/status() calls return, which it does here (step 2 decodes into
// thread-local scratch storage).
String_View http_rate_limit_user_key(Http_Request *req, void *user_data);

void http_rate_limiter_init(Http_RateLimiter *rl, double rate, double burst);
void http_rate_limiter_free(Http_RateLimiter *rl);
// drops every bucket so a drained limiter starts over
void http_rate_limiter_clear(Http_RateLimiter *rl);

// consumes one token for key. 0 = allowed through, -1 = rate exhausted.
// buckets are created on first sight and pruned only by clear().
int http_rate_limiter_allow(Http_RateLimiter *rl, String_View key);

// fills out with the bucket state for the given key after a prior allow()
// call. if no bucket exists yet, out is zeroed.
void http_rate_limiter_status(const Http_RateLimiter *rl, String_View key,
                              Http_RateLimit_Status *out);

// middleware: pulls the request's key (key_fn, defaulting to the request
// path) and passes through while a token remains; when the bucket is dry it
// answers 429 with a Retry-After hint and skips the rest of the chain.
void http_rate_limit_middleware(Http_Request *req, Http_Response *res,
                                void *user_data,
                                Http_Handler_Fn next, void *next_data);

#endif