#include "auth.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "base64.h"
#include "hmac.h"
#include "strbuf.h"
#include "sv.h"
#include "xmem.h"

int http_basic_auth_parse(String_View header,
                          char *user, size_t user_size,
                          char *pass, size_t pass_size)
{
    header = sv_trim(header);
    // scheme names are case-insensitive, "Basic " is the usual spelling
    if (header.count < 6 ||
        tolower((unsigned char)header.data[0]) != 'b' ||
        tolower((unsigned char)header.data[1]) != 'a' ||
        tolower((unsigned char)header.data[2]) != 's' ||
        tolower((unsigned char)header.data[3]) != 'i' ||
        tolower((unsigned char)header.data[4]) != 'c' ||
        header.data[5] != ' ') {
        return -1;
    }
    header.data += 6;
    header.count -= 6;
    if (header.count == 0) {
        return -1;
    }

    Strbuf decoded;
    strbuf_init(&decoded);
    if (base64_decode_into(&decoded, header) != 0) {
        strbuf_free(&decoded);
        return -1;
    }

    // the wire form is "user:pass" with exactly one colon separating them
    size_t colon = 0;
    while (colon < decoded.count && decoded.items[colon] != ':') {
        colon++;
    }
    if (colon == 0 || colon >= decoded.count) {
        strbuf_free(&decoded);
        return -1;
    }
    size_t user_len = colon;
    size_t pass_len = decoded.count - colon - 1;

    size_t uc = user_len < user_size - 1 ? user_len : user_size - 1;
    memcpy(user, decoded.items, uc);
    user[uc] = '\0';

    size_t pc = pass_len < pass_size - 1 ? pass_len : pass_size - 1;
    memcpy(pass, decoded.items + colon + 1, pc);
    pass[pc] = '\0';

    strbuf_free(&decoded);
    return 0;
}

void http_response_require_basic_auth(Http_Response *res, const char *realm)
{
    // escape the realm for the quoted-string grammar of WWW-Authenticate
    char quoted[256];
    size_t n = 0;
    for (size_t i = 0; realm[i] != '\0' && n + 1 < sizeof(quoted); i++) {
        if (realm[i] == '"' || realm[i] == '\\') {
            quoted[n++] = '\\';
        }
        quoted[n++] = realm[i];
    }
    quoted[n] = '\0';

    char challenge[sizeof(quoted) + 32];
    snprintf(challenge, sizeof(challenge), "Basic realm=\"%s\"", quoted);

    http_response_set_status(res, HTTP_401_UNAUTHORIZED);
    http_response_set_header(res, "Content-Type", "text/plain; charset=utf-8");
    http_response_set_header(res, "WWW-Authenticate", challenge);
    http_response_add_body_cstr(res, "authentication required\n");
}

// decodes one Authorization header and compares the whole "user:pass" wire
// payload against the expected identity. the digests are SHA-256 so the
// comparison is constant-length regardless of how long the guess was, and
// secure_byte_equal never bails early, keeping the timing flat. on a match,
// the username is copied (truncated to fit, NUL-terminated) into user_out.
static int check_creds(String_View header, const char *want_user,
                       const char *want_pass, size_t pass_len,
                       char *user_out, size_t user_cap)
{
    header = sv_trim(header);
    if (header.count < 6 ||
        tolower((unsigned char)header.data[0]) != 'b' ||
        tolower((unsigned char)header.data[1]) != 'a' ||
        tolower((unsigned char)header.data[2]) != 's' ||
        tolower((unsigned char)header.data[3]) != 'i' ||
        tolower((unsigned char)header.data[4]) != 'c' ||
        header.data[5] != ' ') {
        return -1;
    }
    String_View payload = {header.data + 6, header.count - 6};
    if (payload.count == 0) {
        return -1;
    }

    // decode once; the payload is exactly "user:pass"
    Strbuf decoded;
    strbuf_init(&decoded);
    if (base64_decode_into(&decoded, payload) != 0) {
        strbuf_free(&decoded);
        return -1;
    }
    size_t colon = 0;
    while (colon < decoded.count && decoded.items[colon] != ':') {
        colon++;
    }
    // an empty user or a missing colon is malformed; an empty password is fine
    if (colon == 0 || colon >= decoded.count) {
        strbuf_free(&decoded);
        return -1;
    }

    const char *pass = want_pass != NULL ? want_pass : "";
    size_t want_user_len = strlen(want_user);
    char expected[1024];
    if (want_user_len + 1 + pass_len + 1 > sizeof expected) {
        strbuf_free(&decoded);
        return -1;
    }
    memcpy(expected, want_user, want_user_len);
    expected[want_user_len] = ':';
    memcpy(expected + want_user_len + 1, pass, pass_len);
    expected[want_user_len + 1 + pass_len] = '\0';

    unsigned char want_digest[32];
    unsigned char got_digest[32];
    sha256((const unsigned char *)expected,
           want_user_len + 1 + pass_len, want_digest);
    sha256((const unsigned char *)decoded.items, decoded.count, got_digest);

    int ok = secure_byte_equal(want_digest, got_digest, sizeof want_digest);
    if (ok && user_out != NULL && user_cap > 0) {
        size_t n = colon < user_cap - 1 ? colon : user_cap - 1;
        memcpy(user_out, decoded.items, n);
        user_out[n] = '\0';
    }
    strbuf_free(&decoded);
    return ok ? 0 : -1;
}

void http_basic_auth_middleware(Http_Request *req, Http_Response *res,
                                void *user_data,
                                Http_Handler_Fn next, void *next_data)
{
    Http_BasicAuth_Options *opts = user_data;

    // a guard with no identity is not a guard: NULL opts (or an empty user)
    // lets everything through, so one code path serves both a locked app and
    // a dev build that mounts it unguarded
    if (opts == NULL || opts->user == NULL || opts->user[0] == '\0') {
        next(req, res, next_data);
        return;
    }

    char userbuf[128];
    size_t pass_len = opts->pass != NULL ? strlen(opts->pass) : 0;
    const String_View *auth = http_request_get_header(req, "authorization");
    if (auth == NULL ||
        check_creds(*auth, opts->user, opts->pass, pass_len,
                    userbuf, sizeof userbuf) != 0) {
        http_response_require_basic_auth(
            res, opts->realm != NULL ? opts->realm : "cweb");
        return;
    }

    // stash the authenticated username where handlers can read it; the copy is
    // owned by the request object and released by http_request_free
    size_t ulen = strlen(userbuf);
    char *owned = xmalloc(ulen + 1);
    memcpy(owned, userbuf, ulen + 1);
    req->auth_user.data = owned;
    req->auth_user.count = ulen;

    next(req, res, next_data);
}