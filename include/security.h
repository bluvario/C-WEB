#ifndef CWEB_SECURITY_H
#define CWEB_SECURITY_H

#include "middleware.h"
#include "response.h"

// controls the CSP pair stamped by the security helpers. pass one of these as
// the middleware user_data (or NULL for the built-in defaults).
typedef struct {
    const char *csp;             // Content-Security-Policy value; NULL = built-in directive
    const char *csp_report_only; // Content-Security-Policy-Report-Only; absent when NULL
} Http_Security_Options;

// stamps res with a conservative baseline of hardening headers:
// X-Content-Type-Options: nosniff, Referrer-Policy: no-referrer,
// X-Frame-Options: DENY plus the Content-Security-Policy chosen by opts (the
// built-in directive when opts->csp is NULL, none extra when opts is NULL).
// purely additive, handlers that already set a header keep their value.
void http_response_security_headers_opts(Http_Response *res,
                                         const Http_Security_Options *opts);

// single-CSP form of the helper: sets Content-Security-Policy to csp (or the
// built-in directive when NULL) alongside the other hardening headers.
void http_response_security_headers(Http_Response *res, const char *csp);

// middleware form of the helper: stamps the same hardening headers, then
// passes control down the chain, so every response that leaves the server
// carries them. user_data is an Http_Security_Options const*, or NULL for the
// baseline default. handlers run after this layer and may override.
void http_security_middleware(Http_Request *req, Http_Response *res,
                              void *user_data,
                              Http_Handler_Fn next, void *next_data);

#endif