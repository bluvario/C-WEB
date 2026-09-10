#ifndef CWEB_CORS_H
#define CWEB_CORS_H

#include "middleware.h"
#include "sv.h"

// CORS middleware options. origin is the only required field: "*" opens
// the endpoint to any origin without credentials; a concrete URL like
// "https://example.com" only matches that origin. methods and headers are
// comma-separated strings passed verbatim to the preflight response;
// when NULL the middleware uses sensible defaults ("GET, POST, PUT,
// DELETE, PATCH, OPTIONS" and "Content-Type, Authorization").
typedef struct {
    const char *origin;         // allowed origin, "*" = any (required)
    const char *methods;        // Access-Control-Allow-Methods; NULL = default
    const char *headers;        // Access-Control-Allow-Headers; NULL = default
    int credentials;            // set Access-Control-Allow-Credentials: true
    long max_age;               // Access-Control-Max-Age seconds; 0 = none
} Http_Cors_Options;

// middleware: on preflight OPTIONS, answers 204 with the CORS headers and
// skips the rest of the chain.  on every other request carrying an Origin
// header, stamps Access-Control-Allow-Origin (and Vary: Origin) then lets
// the chain continue.  when the request has no Origin the middleware is a
// no-op passthrough.  user_data is an Http_Cors_Options* (or NULL for the
// permissive default: origin *, all methods, no credentials).
void http_cors_middleware(Http_Request *req, Http_Response *res,
                          void *user_data,
                          Http_Handler_Fn next, void *next_data);

#endif
