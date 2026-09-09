#define _POSIX_C_SOURCE 200809L

#include "csrf.h"

#ifdef _WIN32
#include <time.h>
#endif

#include <stdio.h>
#include <string.h>

#include "escape.h"
#include "strbuf.h"
#include "sv.h"
#include "xmem.h"

#define CSRF_KEY "csrf-token"
#define CSRF_FIELD "csrf_token"
#define TOKEN_BYTES 20
#define TOKEN_HEX (TOKEN_BYTES * 2)

// fills buf with OS random bytes. /dev/urandom on unix; the Windows C99
// fallback is a time-seeded rand(), fine for local dev.
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

const char *http_csrf_token(Http_Session *sesh)
{
    if (sesh == NULL) {
        return NULL;
    }
    const char *have = http_session_value(sesh, CSRF_KEY);
    if (have != NULL && have[0] != '\0') {
        return have;
    }
    unsigned char rnd[TOKEN_BYTES];
    if (random_bytes(rnd, sizeof rnd) != 0) {
        return NULL;
    }
    char tok[TOKEN_HEX + 1];
    for (size_t i = 0; i < TOKEN_BYTES; i++) {
        snprintf(tok + 2 * i, 3, "%02x", rnd[i]);
    }
    http_session_set(sesh, CSRF_KEY, tok);
    return http_session_value(sesh, CSRF_KEY);
}

void http_csrf_field(Http_Response *res, Http_Session *sesh)
{
    const char *tok = http_csrf_token(sesh);
    if (tok == NULL) {
        return;
    }
    char input[128];
    snprintf(input, sizeof input,
             "<input type=\"hidden\" name=\"" CSRF_FIELD "\" value=\"%s\">",
             tok);
    http_response_add_body_cstr(res, input);
}

int http_csrf_verify(Str_Map *params, Http_Session *sesh)
{
    const char *sent = params != NULL ? strmap_get_cstr(params, CSRF_FIELD) : NULL;
    // never generate a token here: a form cannot be genuine if the session
    // never served one
    const char *token = sesh != NULL ? http_session_value(sesh, CSRF_KEY) : NULL;
    return (sent != NULL && token != NULL && strcmp(sent, token) == 0) ? 0 : -1;
}

void http_csrf_reject(Http_Response *res, const char *retry_url)
{
    http_response_set_status(res, HTTP_403_FORBIDDEN);
    http_response_set_header(res, "Content-Type", "text/html; charset=utf-8");
    http_response_add_body_cstr(res, "<p>this form was not submitted by this site "
                                     "(the CSRF token did not match). ");
    Strbuf esc;
    strbuf_init(&esc);
    html_escape_into(&esc, sv_from_cstr(retry_url));
    http_response_add_body_cstr(res, "<a href=\"");
    http_response_add_body(res, (String_View){esc.items, esc.count});
    http_response_add_body_cstr(res, "\">try again</a></p>");
    strbuf_free(&esc);
}