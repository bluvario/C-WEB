#define _POSIX_C_SOURCE 200809L

#include "session.h"

#ifdef _WIN32
#include <time.h>
#endif

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "db.h"
#include "strbuf.h"
#include "sv.h"
#include "xmem.h"

#define TOKEN_BYTES 20
#define TOKEN_HEX (TOKEN_BYTES * 2)
#define SESSION_DB_PREFIX "session-"

// fills buf with OS random bytes. /dev/urandom on unix; on Windows the C99
// toolchain has no free CSPRNG until you drag in BCrypt, so a time-seeded
// rand() stands in (fine for local dev, swap in a real one for production).
static int random_bytes(unsigned char *buf, size_t n)
{
#ifdef _WIN32
    static int seeded = 0;
    if (!seeded) {
        srand((unsigned int)time(NULL));
        seeded = 1;
    }
    for (size_t i = 0; i < n; i++) {
        buf[i] = (unsigned char)(rand() & 0xff);
    }
    return 0;
#else
    FILE *f = fopen("/dev/urandom", "rb");
    if (f == NULL) {
        return -1;
    }
    size_t got = fread(buf, 1, n, f);
    fclose(f);
    return got == n ? 0 : -1;
#endif
}

// a token is just the hex of random bytes: a clean cookie value with no ',',
// ';', '"' or whitespace to quote or escape
static char *new_token(void)
{
    unsigned char rnd[TOKEN_BYTES];
    if (random_bytes(rnd, sizeof rnd) != 0) {
        return NULL;
    }
    char *tok = xmalloc(TOKEN_HEX + 1);
    for (size_t i = 0; i < TOKEN_BYTES; i++) {
        snprintf(tok + 2 * i, 3, "%02x", rnd[i]);
    }
    tok[TOKEN_HEX] = '\0';
    return tok;
}

void http_session_store_init(Http_Session_Store *s, time_t default_ttl)
{
    s->sessions = NULL;
    s->tokens = NULL;
    s->count = 0;
    s->capacity = 0;
    s->default_ttl = default_ttl;
    s->db = NULL;
}

void http_session_store_free(Http_Session_Store *s)
{
    // deliberately leaves the db keys behind: a restart reloads them, which is
    // the whole point of backing a store with a persistent db
    for (size_t i = 0; i < s->count; i++) {
        strmap_free(&s->sessions[i].data);
        xfree(s->tokens[i]);
    }
    xfree(s->sessions);
    xfree(s->tokens);
    http_session_store_init(s, 0);
}

void http_session_store_set_db(Http_Session_Store *s, Cweb_Db *db)
{
    s->db = db;
}

static size_t find_token(const Http_Session_Store *s, const char *token)
{
    for (size_t i = 0; i < s->count; i++) {
        if (strcmp(s->tokens[i], token) == 0) {
            return i;
        }
    }
    return SIZE_MAX;
}

// swaps slot i out from under the last element so the arrays stay dense.
// when a backing db is present the session's key is removed from it so a
// subsequent open cannot resurrect a destroyed session.
static void remove_at(Http_Session_Store *s, size_t i)
{
    if (s->db != NULL) {
        char key[64];
        snprintf(key, sizeof key, "%s%s", SESSION_DB_PREFIX, s->tokens[i]);
        cweb_db_delete(s->db, sv_from_cstr(key));
    }
    strmap_free(&s->sessions[i].data);
    xfree(s->tokens[i]);
    s->count--;
    if (i != s->count) {
        s->tokens[i] = s->tokens[s->count];
        s->sessions[i] = s->sessions[s->count];
    }
}

char *http_session_create(Http_Session_Store *s, time_t ttl)
{
    char *tok = new_token();
    if (tok == NULL) {
        return NULL;
    }
    if (s->count == s->capacity) {
        size_t nc = s->capacity ? s->capacity * 2 : 8;
        s->sessions = xrealloc(s->sessions, nc * sizeof(*s->sessions));
        s->tokens = xrealloc(s->tokens, nc * sizeof(*s->tokens));
        s->capacity = nc;
    }
    s->tokens[s->count] = tok;
    strmap_init(&s->sessions[s->count].data);
    time_t t = ttl < 0 ? s->default_ttl : ttl;
    s->sessions[s->count].expires = t > 0 ? time(NULL) + t : 0;
    s->count++;
    return tok;
}

// resolves a token to its session; expired sessions are dropped on contact.
// (the db-aware version below also loads uncached, persisted sessions.)
Http_Session *http_session_open(Http_Session_Store *s, const char *token);

void http_session_set(Http_Session *sesh, const char *key, const char *value)
{
    strmap_set(&sesh->data, sv_from_cstr(key), sv_from_cstr(value));
}

const char *http_session_value(Http_Session *sesh, const char *key)
{
    return strmap_get_cstr(&sesh->data, key);
}

void http_session_destroy(Http_Session_Store *s, const char *token)
{
    size_t i = find_token(s, token);
    if (i != SIZE_MAX) {
        remove_at(s, i);
    }
}

void http_session_reap(Http_Session_Store *s)
{
    time_t now = time(NULL);
    size_t i = 0;
    while (i < s->count) {
        if (s->sessions[i].expires != 0 && s->sessions[i].expires <= now) {
            remove_at(s, i); // last slot moves into i, so check i again
        } else {
            i++;
        }
    }
}

Http_Session *http_session_from_cookie(Http_Session_Store *s, Http_Request *req,
                                       const char *cookie_name)
{
    const String_View *h = http_request_get_header(req, "cookie");
    if (h == NULL) {
        return NULL;
    }
    Str_Map jar;
    strmap_init(&jar);
    http_cookie_parse(&jar, *h);
    const char *token = strmap_get_cstr(&jar, cookie_name);
    Http_Session *sesh = (token != NULL) ? http_session_open(s, token) : NULL;
    strmap_free(&jar);
    return sesh;
}

void http_session_issue_cookie(Http_Response *res, const char *cookie_name,
                               const char *token, const Cookie_Attrs *attrs)
{
    Cookie_Attrs a = {0};
    if (attrs != NULL) {
        a = *attrs;
    } else {
        a.path = "/";
        a.http_only = true;
        a.max_age = -1; // negative omits Max-Age: a browser-lifetime cookie
    }
    http_response_set_cookie(res, cookie_name, token, &a);
}

// ---- db-backed session persistence ----------------------------------------

// the on-disk format for a single session record is:
//   <expires>\n       decimal, 0 = never
//   <key>\n<value>\n  zero or more newline-terminated pairs
//
// the "session-" prefix identifies the key; a missing or unparseable record is
// silently ignored so a fresh session forms in memory. sessions that were
// destroyed while the server ran have their records deleted immediately by
// remove_at, so no stale keys linger.

static void session_key(char *buf, size_t bufsz, const char *token)
{
    snprintf(buf, bufsz, "%s%s", SESSION_DB_PREFIX, token);
}

static void session_to_db(const Http_Session_Store *s, size_t i)
{
    if (s->db == NULL) {
        return;
    }
    Strbuf buf;
    strbuf_init(&buf);
    char num[32];
    snprintf(num, sizeof num, "%ld", (long)s->sessions[i].expires);
    strbuf_append_cstr(&buf, num);
    strbuf_append_char(&buf, '\n');
    for (size_t k = 0; k < s->sessions[i].data.capacity; k++) {
        if (s->sessions[i].data.entries[k].key == NULL) {
            continue;
        }
        strbuf_append_cstr(&buf, s->sessions[i].data.entries[k].key);
        strbuf_append_char(&buf, '\n');
        strbuf_append_cstr(&buf, s->sessions[i].data.entries[k].value);
        strbuf_append_char(&buf, '\n');
    }
    char key[64];
    session_key(key, sizeof key, s->tokens[i]);
    strbuf_null_terminate(&buf);
    cweb_db_put(s->db, sv_from_cstr(key),
                sv_from_cstr(buf.items));
    strbuf_free(&buf);
}

static void session_from_db(Cweb_Db *db, Http_Session *sesh,
                            const char *token)
{
    char key[64];
    session_key(key, sizeof key, token);
    String_View blob = cweb_db_get(db, sv_from_cstr(key));
    if (blob.data == NULL || blob.count == 0) {
        return;
    }
    String_View rest = blob;
    String_View line = sv_chop_by_delim(&rest, '\n');
    long long exp = 0;
    sv_to_i64(line, &exp);
    sesh->expires = (time_t)exp;
    while (rest.count > 0) {
        String_View k = sv_chop_by_delim(&rest, '\n');
        if (rest.count == 0) {
            break;
        }
        String_View v = sv_chop_by_delim(&rest, '\n');
        strmap_set(&sesh->data, k, v);
    }
}

// resolves a token to its session, loading an uncached (persisted) session
// into the store on a miss. expired sessions are dropped on contact.
Http_Session *http_session_open(Http_Session_Store *s, const char *token)
{
    size_t i = find_token(s, token);
    if (i == SIZE_MAX && s->db != NULL) {
        char key[64];
        session_key(key, sizeof key, token);
        if (cweb_db_get(s->db, sv_from_cstr(key)).data == NULL) {
            return NULL;
        }
        if (s->count == s->capacity) {
            size_t nc = s->capacity ? s->capacity * 2 : 8;
            s->sessions = xrealloc(s->sessions, nc * sizeof(*s->sessions));
            s->tokens = xrealloc(s->tokens, nc * sizeof(*s->tokens));
            s->capacity = nc;
        }
        s->tokens[s->count] = xmalloc(strlen(token) + 1);
        strcpy(s->tokens[s->count], token);
        strmap_init(&s->sessions[s->count].data);
        s->sessions[s->count].expires = 0;
        session_from_db(s->db, &s->sessions[s->count], token);
        i = s->count;
        s->count++;
    }
    if (i == SIZE_MAX) {
        return NULL;
    }
    if (s->sessions[i].expires != 0 && s->sessions[i].expires <= time(NULL)) {
        http_session_destroy(s, s->tokens[i]);
        return NULL;
    }
    return &s->sessions[i];
}

// serializes every live session into the backing db. sessions that were
// destroyed during this run already had their keys removed by remove_at, so
// the db ends up with exactly the sessions that are still live in memory.
void http_session_store_dump(Http_Session_Store *s)
{
    for (size_t i = 0; i < s->count; i++) {
        session_to_db(s, i);
    }
}
