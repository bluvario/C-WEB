#ifndef CWEB_HTTP_CLIENT_H
#define CWEB_HTTP_CLIENT_H

#include "http.h"
#include "strbuf.h"
#include "sv.h"

// http client, two ways to use it:
//
//   one-shot:  http_client_get/post issue a single request on their own
//              throwaway connection.
//
//   persistent: http_client_open holds one connection open across http_client_req
//              calls and transparently reconnects when the server drops it, so a
//              loop of requests does not pay a TCP handshake each time. body
//              framing (Content-Length, chunked, close-delimited) is read back
//              exactly, which is what keeps the socket in sync for reuse.

typedef struct {
    Http_Status status; // valid when error == NULL
    Strbuf headers;     // status line plus headers, raw "Name: value\r\n"
    Strbuf body;        // reassembled payload, chunked decoding already done
    char *error;        // owned reason, NULL when the exchange succeeded
} Http_Client_Result;

typedef struct Http_Client Http_Client;

// base_url pins the default host for relative request targets and may be NULL
// when every call passes an absolute URL. returns NULL on a garbage base_url.
Http_Client *http_client_open(const char *base_url);
void http_client_close(Http_Client *c);

// how long a request may wait for data; 0 keeps the kernel default. 10000ms.
int http_client_set_timeout(Http_Client *c, unsigned long ms);

// diagnostics for tests and load watchers
bool http_client_keepalive_active(const Http_Client *c);
size_t http_client_connection_opens(const Http_Client *c);

// runs one request on the client. target is either an absolute scheme:// URL
// (which becomes the client's base for later relative calls) or a root-relative
// "/path?query" string, in which case base_url must have been set. extra_headers
// may be NULL or NUL-terminated raw "Name: value\r\n" lines; body bytes travel
// as-is with a computed Content-Length. returns 0 when a response arrived --
// whatever its status -- and -1 on failure, with error explaining why. a reused
// connection that turns out to be dead is retried once, but only for idempotent
// methods (GET, HEAD, OPTIONS), never for POST.
int http_client_req(Http_Client *c, const char *url_or_path, Http_Method method,
                    const char *extra_headers, String_View body,
                    Http_Client_Result *out);
int http_client_req_get(Http_Client *c, const char *url_or_path, Http_Client_Result *out);
int http_client_req_post(Http_Client *c, const char *url_or_path, String_View body,
                         Http_Client_Result *out);

// one-shot calls: issue a single request and close the connection again
int http_client_request(const char *url, Http_Method method,
                        const char *extra_headers, String_View body,
                        Http_Client_Result *out);
int http_client_get(const char *url, Http_Client_Result *out);
int http_client_post(const char *url, String_View body, Http_Client_Result *out);

void http_client_result_free(Http_Client_Result *out);

#endif