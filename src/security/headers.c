#include "security.h"

void http_response_security_headers(Http_Response *res, const char *csp)
{
    http_response_set_header(res, "X-Content-Type-Options", "nosniff");
    http_response_set_header(res, "Referrer-Policy", "no-referrer");
    http_response_set_header(res, "X-Frame-Options", "DENY");
    http_response_set_header(res, "Content-Security-Policy",
                             csp ? csp : "default-src 'self'; frame-ancestors 'none'; object-src 'none'");
}

void http_security_middleware(Http_Request *req, Http_Response *res,
                              void *user_data,
                              Http_Handler_Fn next, void *next_data)
{
    (void)req;
    http_response_security_headers(res, user_data);
    next(req, res, next_data);
}