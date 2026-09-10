#ifndef CWEB_AUTH_H
#define CWEB_AUTH_H

#include "middleware.h"
#include "response.h"
#include "sv.h"

// decodes the credentials in an "Authorization: Basic <base64>" header value.
// the base64 payload is "user:pass", split here into the caller's buffers
// (truncated to fit, always NUL-terminated). returns 0 for well-formed
// credentials, -1 when the scheme, encoding or shape is wrong. an empty
// password is legal, an empty user or a missing colon is not.
int http_basic_auth_parse(String_View authorization,
                          char *user, size_t user_size,
                          char *pass, size_t pass_size);

// answers a request with 401 and a WWW-Authenticate challenge for the realm,
// so clients pop a credentials dialog. realm is echoed back, escaped for the
// header's quoted-string grammar.
void http_response_require_basic_auth(Http_Response *res, const char *realm);

// options for the Basic-auth middleware; pass a pointer as user_data.
typedef struct {
    // the one identity that unlocks the chain. user must be non-empty; pass
    // may be NULL (an empty password is legal per http_basic_auth_parse).
    const char *user;
    const char *pass;
    // realm echoed in the WWW-Authenticate challenge so the browser labels
    // its stored credentials; NULL defaults to "cweb".
    const char *realm;
} Http_BasicAuth_Options;

// Basic-auth guard middleware: verifies the Authorization header before
// letting the chain run, so one call guards an entire app or route group.
//   * missing, malformed, or wrong credentials -> 401 with a WWW-Authenticate
//     challenge, and the rest of the chain is skipped;
//   * matching credentials -> the authenticated username is copied into
//     req->auth_user (owned by the request object, like request_id), so
//     handlers can greet or log who is talking, then next() runs;
//   * opts NULL, or an empty opts->user -> disabled, everything passes; handy
//     for a dev build that mounts the same server unguarded.
//
// the comparison is done on SHA-256 digests of the whole "user:pass" wire
// payload vs the expected identity, compared in constant time, so a timing
// side channel cannot reveal how much of a guess was right.
void http_basic_auth_middleware(Http_Request *req, Http_Response *res,
                                void *user_data,
                                Http_Handler_Fn next, void *next_data);

#endif