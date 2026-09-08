#ifndef CWEB_SECURITY_H
#define CWEB_SECURITY_H

#include "response.h"

// stamps res with a conservative baseline of hardening headers:
// X-Content-Type-Options: nosniff, Referrer-Policy: no-referrer,
// X-Frame-Options: DENY and a Content-Security-Policy. pass an explicit csp to
// override the default directive, NULL for the built-in one. purely additive,
// handlers that already set a header keep their value.
void http_response_security_headers(Http_Response *res, const char *csp);

#endif