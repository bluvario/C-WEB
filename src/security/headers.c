#include "security.h"

static const char *default_csp(void)
{
    return "default-src 'self'; frame-ancestors 'none'; object-src 'none'";
}

void http_response_security_headers_opts(Http_Response *res,
                                         const Http_Security_Options *opts)
{
    http_response_set_header(res, "X-Content-Type-Options", "nosniff");
    http_response_set_header(res, "Referrer-Policy", "no-referrer");
    http_response_set_header(res, "X-Frame-Options", "DENY");
    const char *csp = opts ? opts->csp : NULL;
    const char *report_only = opts ? opts->csp_report_only : NULL;
    http_response_set_header(res, "Content-Security-Policy",
                             csp ? csp : default_csp());
    if (report_only != NULL) {
        http_response_set_header(res, "Content-Security-Policy-Report-Only",
                                 report_only);
    }
}

void http_response_security_headers(Http_Response *res, const char *csp)
{
    Http_Security_Options opts = {0};
    opts.csp = csp;
    http_response_security_headers_opts(res, &opts);
}

void http_security_middleware(Http_Request *req, Http_Response *res,
                              void *user_data,
                              Http_Handler_Fn next, void *next_data)
{
    (void)req;
    http_response_security_headers_opts(res, user_data);
    next(req, res, next_data);
}