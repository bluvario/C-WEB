#ifndef CWEB_STATIC_H
#define CWEB_STATIC_H

#include "request.h"
#include "response.h"

// serves files from a root directory (user_data is a NUL-terminated path).
// percent-decodes the URL path, normalizes it and refuses anything that
// climbs above the root with a 400. directories fall back to index.html and
// unknown extensions get application/octet-stream. 404 when the file is gone.
// drops straight into http_serve/router as a handler.
void http_serve_static(Http_Request *req, Http_Response *res, void *user_data);

#endif