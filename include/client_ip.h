#ifndef CWEB_CLIENT_IP_H
#define CWEB_CLIENT_IP_H

#include "ip.h"
#include "middleware.h"
#include "sv.h"

// resolves the effective client address of a request from an X-Forwarded-For
// chain, but only when the direct peer (req->remote) is a trusted reverse
// proxy, so a mere client cannot spoof its way into a log or a rate bucket.
// the strategy, walking the header right-to-left:
//   * peer not inside any trusted network -> the peer *is* the client and the
//     header is ignored outright;
//   * peer trusted, no X-Forwarded-For    -> the peer is the client;
//   * peer trusted, chain present         -> the rightmost entry that is not
//     itself a trusted proxy is the client; an empty or all-trusted chain
//     falls back to the peer.
// xff is the raw header value (may be empty), peer is the TCP peer address.
// *out borrows from xff or peer (no allocation) and stays valid as long as
// the request buffer lives. returns 0; out is always set.
int http_client_ip_resolve(String_View xff, String_View peer,
                           const Http_Cidr *trusted, size_t trusted_count,
                           String_View *out);

// options for http_client_ip_middleware; pass a pointer as user_data.
typedef struct {
    // caller-owned list of networks allowed to forward requests. NULL or an
    // empty list disables header trust and the middleware is then just a
    // passthrough that reports the peer itself.
    const Http_Cidr *trusted;
    size_t trusted_count;
} Http_ClientIp_Options;

// middleware: fills req->client_ip with the effective client address (see
// http_client_ip_resolve) and then runs the rest of the chain. the view
// borrows from the request buffer, so there is nothing to free. runs before
// anything that consumes an address — access logs, per-address rate buckets —
// and is harmless (client_ip == peer) when no trusted proxy is configured.
void http_client_ip_middleware(Http_Request *req, Http_Response *res,
                               void *user_data,
                               Http_Handler_Fn next, void *next_data);

#endif