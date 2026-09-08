#ifndef CWEB_STATIC_H
#define CWEB_STATIC_H

#include "request.h"
#include "response.h"
#include "router.h"

// serves files from a root directory (user_data is a NUL-terminated path).
// percent-decodes the URL path, normalizes it and refuses anything that
// climbs above the root with a 400. directories fall back to index.html and
// unknown extensions get application/octet-stream. 404 when the file is gone.
// drops straight into http_serve/router as a handler.
void http_serve_static(Http_Request *req, Http_Response *res, void *user_data);

// mounts that handler on a router: requests under url_prefix (leading slash,
// no trailing slash, e.g. "/assets") are served from the disk root with the
// prefix stripped, so "/assets/css/app.css" reads root + "/css/app.css". the
// mount lives as long as the router does.
int http_static_mount(Http_Router *r, const char *url_prefix, const char *fs_root);

#endif