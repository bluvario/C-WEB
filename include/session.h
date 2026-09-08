#ifndef CWEB_SESSION_H
#define CWEB_SESSION_H

#include <stdbool.h>
#include <time.h>

#include "cookie.h"
#include "request.h"
#include "response.h"
#include "strmap.h"

// a server-side session: an opaque random token on the client maps to this
// key/value bag stored on the server, so clients can neither read nor forge
// the data.
typedef struct {
    Str_Map data;   // owned key/value copies
    time_t expires; // epoch seconds, 0 = never expires
} Http_Session;

typedef struct {
    Http_Session *sessions; // parallel arrays, one per token
    char **tokens;
    size_t count;
    size_t capacity;
    time_t default_ttl; // seconds http_session_create uses when ttl < 0, 0 = forever
} Http_Session_Store;

// not thread-safe: give the store a lock, or hold one store per worker thread.
void http_session_store_init(Http_Session_Store *s, time_t default_ttl);
void http_session_store_free(Http_Session_Store *s);

// creates a session with a fresh opaque token (hex of OS random bytes) and
// adopts it into the store. ttl < 0 uses the store default. returns the token
// borrowed from the store: it stays valid until the session is destroyed or
// the store is freed, and it must not be xfreed by the caller. NULL on failure.
char *http_session_create(Http_Session_Store *s, time_t ttl);

// resolves a token to its session; expired sessions are dropped on contact.
// NULL when the token is unknown or stale.
Http_Session *http_session_open(Http_Session_Store *s, const char *token);

// keys and values are stored as owned copies; setting an existing key
// replaces the value.
void http_session_set(Http_Session *sesh, const char *key, const char *value);
// NULL when the key was never set
const char *http_session_value(Http_Session *sesh, const char *key);

// removes a token entirely; unknown tokens are ignored
void http_session_destroy(Http_Session_Store *s, const char *token);

// sweeps and destroys every expired session; call it periodically from a
// timer or a maintenance thread so long-gone sessions cannot pile up
void http_session_reap(Http_Session_Store *s);

// --- cookie glue ---------------------------------------------------------

// picks cookie_name out of the request Cookie header and opens the session
// that token names. NULL when the cookie is missing or the token is unknown.
// the Session handle belongs to the store, do not free it. this is the "load
// the session" step a handler or middleware does first.
Http_Session *http_session_from_cookie(Http_Session_Store *s, Http_Request *req,
                                       const char *cookie_name);

// the mirror write: appends a Set-Cookie carrying the session token. when
// attrs is NULL the cookie is a browser-lifetime session cookie scoped to
// "/" and marked HttpOnly; pass your own attrs to customize (Path, Max-Age,
// Secure...).
void http_session_issue_cookie(Http_Response *res, const char *cookie_name,
                               const char *token, const Cookie_Attrs *attrs);

#endif