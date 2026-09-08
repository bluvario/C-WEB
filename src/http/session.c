#define _POSIX_C_SOURCE 200809L

#include "session.h"

#ifdef _WIN32
#include <time.h>
#endif

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "sv.h"
#include "xmem.h"

#define TOKEN_BYTES 20
#define TOKEN_HEX (TOKEN_BYTES * 2)

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
}

void http_session_store_free(Http_Session_Store *s)
{
    for (size_t i = 0; i < s->count; i++) {
        strmap_free(&s->sessions[i].data);
        xfree(s->tokens[i]);
    }
    xfree(s->sessions);
    xfree(s->tokens);
    http_session_store_init(s, 0);
}

// swaps slot i out from under the last element so the arrays stay dense
static void remove_at(Http_Session_Store *s, size_t i)
{
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

static size_t find_token(const Http_Session_Store *s, const char *token)
{
    for (size_t i = 0; i < s->count; i++) {
        if (strcmp(s->tokens[i], token) == 0) {
            return i;
        }
    }
    return SIZE_MAX;
}

Http_Session *http_session_open(Http_Session_Store *s, const char *token)
{
    size_t i = find_token(s, token);
    if (i == SIZE_MAX) {
        return NULL;
    }
    if (s->sessions[i].expires != 0 && s->sessions[i].expires <= time(NULL)) {
        http_session_destroy(s, token);
        return NULL;
    }
    return &s->sessions[i];
}

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