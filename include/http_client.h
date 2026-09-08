#ifndef CWEB_HTTP_CLIENT_H
#define CWEB_HTTP_CLIENT_H

#include "http.h"
#include "strbuf.h"
#include "sv.h"

// one-shot synchronous HTTP/1.1 client for speaking to other servers: proxy
// work, health checks, webhook calls. each request opens its own connection
// and closes it afterwards; keep-alive reuse is not here yet.

typedef struct {
    Http_Status status; // valid when error == NULL
    Strbuf headers;     // status line plus headers, raw "Name: value\r\n"
    Strbuf body;        // reassembled payload, chunked decoding already done
    char *error;        // owned reason, NULL when the exchange succeeded
} Http_Client_Result;

// sends one request to the absolute url (scheme http or https; https is
// refused for now, TLS has no home here yet). extra_headers may be NULL or a
// NUL-terminated block of raw "Name: value\r\n" lines to append; body bytes
// travel as-is with a computed Content-Length. returns 0 when a response
// arrived -- whatever its status -- and -1 when the call itself failed, in
// which case error explains why.
int http_client_request(const char *url, Http_Method method,
                        const char *extra_headers, String_View body,
                        Http_Client_Result *out);
int http_client_get(const char *url, Http_Client_Result *out);
int http_client_post(const char *url, String_View body, Http_Client_Result *out);

void http_client_result_free(Http_Client_Result *out);

#endif