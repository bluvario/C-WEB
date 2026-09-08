#ifndef CWEB_AUTH_H
#define CWEB_AUTH_H

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

#endif