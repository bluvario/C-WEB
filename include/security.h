#ifndef CWEB_SECURITY_H
#define CWEB_SECURITY_H

#include "middleware.h"
#include "response.h"

// stamps res with a conservative baseline of hardening headers:
// X-Content-Type-Options: nosniff, Referrer-Policy: no-referrer,
// X-Frame-Options: DENY and a Content-Security-Policy. pass an explicit csp to
// override the default directive, NULL for the built-in one. purely additive,
// handlers that already set a header keep their value.
void http_response_security_headers(Http_Response *res, const char *csp);

// middleware form of the helper: stamps the same hardening headers, then
// passes control down the chain, so every response that leaves the server
// carries them. user_data is a (const char *) csp override, NULL for the
// baseline default. handlers run after this layer and may override.
void http_security_middleware(Http_Request *req, Http_Response *res,
                              void *user_data,
                              Http_Handler_Fn next, void *next_data);

#endif