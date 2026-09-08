#ifndef CWEB_PROXY_H
#define CWEB_PROXY_H

#include "request.h"
#include "response.h"
#include "router.h"

// forwards the request upstream verbatim (method, path, query, forwarded
// headers, body) and relays the upstream reply back: status line, surviving
// header lines and re-buffered body. hop-by-hop headers are consumed on both
// sides so each connection's framing stays owned by its single endpoint, and
// redirects are passed through rather than chased. responses are buffered
// whole, so a streaming upstream is not a fit yet.
void http_reverse_proxy(Http_Request *req, Http_Response *res,
                        Str_Map *params, void *user_data);

// mounts the reverse proxy on a router: everything under url_prefix (leading
// slash, no trailing slash, e.g. "/api") is sent to upstream_base
// ("http://host:port", no trailing slash, no path). the request target is
// passed through untouched, so "/api/users?page=2" reaches upstream as
// "/api/users?page=2". the mount lives as long as the router does.
int http_proxy_mount(Http_Router *r, const char *url_prefix, const char *upstream_base);

// releases the resources a mount owns; call it once per prefix you mounted
// (the router itself still frees its own copies of the patterns).
void http_proxy_unmount(Http_Router *r, const char *url_prefix);

#endif