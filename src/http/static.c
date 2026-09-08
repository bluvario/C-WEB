#include "static.h"

#include <string.h>

#include "file.h"
#include "http.h"
#include "mime.h"
#include "path.h"
#include "request.h"
#include "response.h"
#include "strbuf.h"
#include "sv.h"
#include "url.h"
#include "xmem.h"

static void reject(Http_Response *res, Http_Status status)
{
    http_response_set_status(res, status);
    http_response_set_header(res, "Content-Type", "text/plain; charset=utf-8");
    http_response_add_body_cstr(res, status == HTTP_404_NOT_FOUND ? "404 not found"
                                                                  : "400 bad path");
}

void http_serve_static(Http_Request *req, Http_Response *res, void *user_data)
{
    const char *root = user_data;
    Strbuf decoded;
    strbuf_init(&decoded);

    // normalize strips the trailing slash, so remember the directory intent
    // here before decoding
    int dir_request = req->path.count > 0 && req->path.data[req->path.count - 1] == '/';

    // the URL path may arrive percent-encoded and the raw bytes are a
    // traversal hazard of their own, decode first then normalize
    if (url_decode_into(&decoded, req->path) != 0) {
        reject(res, HTTP_400_BAD_REQUEST);
        strbuf_free(&decoded);
        return;
    }

    Strbuf norm;
    strbuf_init(&norm);
    if (uri_path_normalize((String_View){decoded.items, decoded.count}, &norm) != 0) {
        reject(res, HTTP_400_BAD_REQUEST);
        strbuf_free(&decoded);
        strbuf_free(&norm);
        return;
    }
    strbuf_free(&decoded);

    String_View mime = mime_for_path((String_View){norm.items, norm.count});

    Strbuf path;
    strbuf_init(&path);
    strbuf_append_cstr(&path, root);
    if (path.count == 0 || path.items[path.count - 1] != '/') {
        strbuf_append_char(&path, '/');
    }
    // norm always starts with '/', so skip it when splicing under root
    strbuf_append(&path, norm.items + (norm.count > 0 && norm.items[0] == '/' ? 1 : 0),
                  norm.count - (norm.count > 0 && norm.items[0] == '/' ? 1 : 0));
    if (dir_request || path.count == 0 || path.items[path.count - 1] == '/') {
        if (path.count == 0 || path.items[path.count - 1] != '/') {
            strbuf_append_char(&path, '/');
        }
        strbuf_append_cstr(&path, "index.html");
    }
    strbuf_free(&norm);
    if (strbuf_null_terminate(&path) != 0) {
        strbuf_free(&path);
        reject(res, HTTP_500_INTERNAL_SERVER_ERROR);
        return;
    }

    char *data;
    size_t len;
    if (file_read_all(path.items, &data, &len) != 0) {
        reject(res, HTTP_404_NOT_FOUND);
        strbuf_free(&path);
        return;
    }
    strbuf_free(&path);

    char mime_z[64];
    if (mime.count >= sizeof(mime_z)) {
        mime.count = sizeof(mime_z) - 1;
    }
    memcpy(mime_z, mime.data, mime.count);
    mime_z[mime.count] = '\0';

    http_response_set_status(res, HTTP_200_OK);
    http_response_set_header(res, "Content-Type", mime_z);
    http_response_add_body(res, (String_View){data, len});
    xfree(data);
}