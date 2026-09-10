#ifndef CWEB_SESSION_H
#define CWEB_SESSION_H

#include <stdbool.h>
#include <time.h>

#include "cookie.h"
#include "db.h"
#include "request.h"
#include "response.h"
#include "strmap.h"
#include "thread.h"

// a server-side session: an opaque random token on the client maps to this
// key/value bag stored on the server, so clients can neither read nor forge
// the data.
typedef struct Http_Session_Store Http_Session_Store; // forward decl, defined below

typedef struct {
    Str_Map data;           // owned key/value copies
    time_t expires;         // epoch seconds, 0 = never expires
    Http_Session_Store *store; // owner: set() mirrors changes through it
} Http_Session;

struct Http_Session_Store {
    Mutex mu;           // serializes every operation below and the arrays
    Http_Session **sessions; // parallel arrays; each session is its own heap
    char **tokens;          // object so handles survive slot-table growth
    size_t count;
    size_t capacity;
    time_t default_ttl; // seconds http_session_create uses when ttl < 0, 0 = forever
    Cweb_Db *db;        // optional backing store; NULL = in-memory only
};

// thread-safe: every store call is serialized by the store's own mutex, and a
// handle stays valid while other threads create or destroy sessions (sessions
// are heap objects and only the slot table moves on growth). the caller still
// owns its own thread discipline: two threads using the SAME handle at once
// race their key writes just like shared variables, and destroying a session
// while another thread holds its handle is a use-after-free by design.
void http_session_store_init(Http_Session_Store *s, time_t default_ttl);
void http_session_store_free(Http_Session_Store *s);

// attaches a persistent backing store. every session is mirrored under a
// "session-<token>" key as it changes: creating a session and every
// http_session_set write its record to the db immediately, sessions that are
// not in memory are loaded back from it on demand, and destroying or reaping
// a session removes its key. freeing the store does NOT clear its keys: that
// is how logins survive a restart, even an abrupt one that never ran a dump.
// pass NULL to detach.
void http_session_store_set_db(Http_Session_Store *s, Cweb_Db *db);

// flushes every live session's record to the backing store. the store already
// mirrors every create/set as it happens, so this is just a final clean-shutdown
// belt-and-braces pass. no-op when the store has no backing db.
void http_session_store_dump(Http_Session_Store *s);

// creates a session with a fresh opaque token (hex of OS random bytes) and
// adopts it into the store. ttl < 0 uses the store default. returns the token
// borrowed from the store: it stays valid until the session is destroyed or
// the store is freed, and it must not be xfreed by the caller. NULL on failure.
char *http_session_create(Http_Session_Store *s, time_t ttl);

// resolves a token to its session; expired sessions are dropped on contact.
// NULL when the token is unknown or stale.
Http_Session *http_session_open(Http_Session_Store *s, const char *token);

// keys and values are stored as owned copies; setting an existing key
// replaces the value. a session with a backing db mirrors the change to its
// record and syncs it immediately, so the store survives a crash.
void http_session_set(Http_Session *sesh, const char *key, const char *value);
// NULL when the key was never set
const char *http_session_value(Http_Session *sesh, const char *key);

// removes a single key from a session. a backing db mirrors the removal the
// same way set() mirrors additions, so a crash cannot resurrect a key that
// was already deleted. returns 1 when the key existed, 0 otherwise.
int http_session_del(Http_Session *sesh, const char *key);

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