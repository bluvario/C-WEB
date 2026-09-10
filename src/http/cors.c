#include "cors.h"

#include <string.h>

#include "http.h"
#include "sv.h"

#define CORS_DEFAULT_METHODS "GET, POST, PUT, DELETE, PATCH, OPTIONS"
#define CORS_DEFAULT_HEADERS "Content-Type, Authorization"

static int origin_matches(const char *allowed, String_View incoming)
{
    if (allowed == NULL) {
        return 0;
    }
    // "*" matches everything
    if (strcmp(allowed, "*") == 0) {
        return 1;
    }
    return sv_equal(incoming, sv_from_cstr(allowed));
}

static void stamp_preflight_headers(Http_Response *res,
                                    const Http_Cors_Options *opts,
                                    const char *origin)
{
    http_response_set_header(res, "Access-Control-Allow-Origin", origin);
    http_response_set_header(res, "Vary", "Origin");
    http_response_set_header(res, "Access-Control-Allow-Methods",
                             opts->methods ? opts->methods : CORS_DEFAULT_METHODS);
    http_response_set_header(res, "Access-Control-Allow-Headers",
                             opts->headers ? opts->headers : CORS_DEFAULT_HEADERS);
    if (opts->max_age > 0) {
        char age[32];
        snprintf(age, sizeof age, "%ld", opts->max_age);
        http_response_set_header(res, "Access-Control-Max-Age", age);
    }
    if (opts->credentials) {
        http_response_set_header(res, "Access-Control-Allow-Credentials", "true");
    }
}

void http_cors_middleware(Http_Request *req, Http_Response *res,
                          void *user_data,
                          Http_Handler_Fn next, void *next_data)
{
    Http_Cors_Options defaults = {0};
    defaults.origin = "*";
    const Http_Cors_Options *opts = user_data != NULL ? user_data : &defaults;

    // a request with no Origin header is not a CORS request
    const String_View *origin_hdr = http_request_get_header(req, "Origin");
    if (origin_hdr == NULL || origin_hdr->count == 0) {
        next(req, res, next_data);
        return;
    }

    if (!origin_matches(opts->origin, *origin_hdr)) {
        next(req, res, next_data);
        return;
    }

    char origin_buf[512];
    size_t n = origin_hdr->count < sizeof origin_buf ? origin_hdr->count
                                                     : sizeof origin_buf - 1;
    memcpy(origin_buf, origin_hdr->data, n);
    origin_buf[n] = '\0';

    if (req->method == HTTP_OPTIONS) {
        // preflight: answer 204 with CORS headers, skip the chain
        http_response_set_status(res, HTTP_204_NO_CONTENT);
        stamp_preflight_headers(res, opts, origin_buf);
        return;
    }

    // actual request: stamp CORS headers then let the handler run
    http_response_set_header(res, "Access-Control-Allow-Origin", origin_buf);
    http_response_set_header(res, "Vary", "Origin");
    if (opts->credentials) {
        http_response_set_header(res, "Access-Control-Allow-Credentials", "true");
    }
    next(req, res, next_data);
}