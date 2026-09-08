#include "static.h"

#include <stdio.h>
#include <string.h>

#include "date.h"
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

// does the If-None-Match header list our ETag (or "*" for any existing
// resource)? weak "W/" prefixes are tolerated, strong comparison otherwise.
static bool etag_list_matches(String_View header, const char *ours)
{
    // strip the quoting from our own validator, the list entries have theirs
    // peeled off as they come
    size_t olen = strlen(ours);
    if (olen >= 2 && ours[0] == '"' && ours[olen - 1] == '"') {
        ours++;
        olen -= 2;
    }
    String_View rest = header;
    while (rest.count > 0) {
        String_View tag = sv_chop_by_delim(&rest, ',');
        tag = sv_trim(tag);
        if (tag.count >= 2 && tag.data[0] == 'W' && tag.data[1] == '/') {
            tag.data += 2;
            tag.count -= 2;
            tag = sv_trim(tag);
        }
        // "*" matches any entity that exists
        if (tag.count == 1 && tag.data[0] == '*') {
            return true;
        }
        if (tag.count >= 2 && tag.data[0] == '"' && tag.data[tag.count - 1] == '"') {
            tag.data++;
            tag.count -= 2;
            if (tag.count == olen && memcmp(tag.data, ours, olen) == 0) {
                return true;
            }
        }
    }
    return false;
}

// single-range bytes=<start>-<end> only. returns 0 when no range applies (the
// whole file is served), 1 when only [*base, *base+*n) should be served, and
// -1 when the header asks for bytes the file cannot cover (caller answers
// 416). multi-range lists get a plain 200, which is always legal.
static int parse_range(String_View header, size_t fsize, size_t *base, size_t *n)
{
    header = sv_trim(header);
    if (!sv_consume_prefix(&header, "bytes=")) {
        return 0;
    }
    // any comma means a list; one 200 with the full body covers them all
    if (sv_count_char(header, ',') > 0) {
        return 0;
    }
    String_View start = sv_chop_by_delim(&header, '-');
    start = sv_trim(start);
    header = sv_trim(header);

    if (start.count == 0 && header.count == 0) {
        return -1; // "bytes=" with nothing at all
    }

    if (start.count > 0 && header.count == 0) {
        // "N-": from N to the end
        long long from;
        if (!sv_to_i64(start, &from) || from < 0 || (size_t)from >= fsize) {
            return -1;
        }
        *base = (size_t)from;
        *n = fsize - *base;
        return 1;
    }

    if (start.count == 0 && header.count > 0) {
        // "-N": the final N bytes
        long long last;
        if (!sv_to_i64(header, &last) || last <= 0) {
            return -1;
        }
        if (fsize == 0) {
            return -1;
        }
        *n = (size_t)last < fsize ? (size_t)last : fsize;
        *base = fsize - *n;
        return 1;
    }

    // "N-M": a closed window, clamped to the file end
    long long from, to;
    if (!sv_to_i64(start, &from) || !sv_to_i64(header, &to) ||
        from < 0 || to < from || (size_t)from >= fsize) {
        return -1;
    }
    *base = (size_t)from;
    size_t want = (size_t)(to - from) + 1;
    *n = want < fsize - *base ? want : fsize - *base;
    return 1;
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

    char mime_z[64];
    if (mime.count >= sizeof(mime_z)) {
        mime.count = sizeof(mime_z) - 1;
    }
    memcpy(mime_z, mime.data, mime.count);
    mime_z[mime.count] = '\0';

    http_response_set_status(res, HTTP_200_OK);
    http_response_set_header(res, "Content-Type", mime_z);

    // validators: Last-Modified plus a strong ETag from mtime and size. an
    // If-None-Match answer beats If-Modified-Since when both are present.
    time_t mtime;
    size_t fsize;
    if (file_stat(path.items, &mtime, &fsize) != 0) {
        reject(res, HTTP_404_NOT_FOUND);
        strbuf_free(&path);
        return;
    }
    char lm[64];
    http_date_rfc7231(mtime, lm, sizeof(lm));
    http_response_set_header(res, "Last-Modified", lm);

    char etag[64];
    snprintf(etag, sizeof(etag), "\"%08llx-%zx\"",
             (unsigned long long)mtime, fsize);
    http_response_set_header(res, "ETag", etag);

    bool not_modified = false;
    const String_View *inm = http_request_get_header(req, "if-none-match");
    if (inm != NULL) {
        not_modified = etag_list_matches(*inm, etag);
    } else {
        const String_View *ims = http_request_get_header(req, "if-modified-since");
        if (ims != NULL) {
            time_t since = http_date_parse(*ims);
            if (since >= 0 && mtime <= since) {
                not_modified = true;
            }
        }
    }
    if (not_modified) {
        http_response_set_status(res, HTTP_304_NOT_MODIFIED);
        strbuf_free(&path);
        return;
    }

    // ranges only make sense for fetches; other verbs get the whole entity
    size_t base = 0;
    size_t rlen = fsize;
    bool partial = false;
    if (req->method == HTTP_GET || req->method == HTTP_HEAD) {
        const String_View *range = http_request_get_header(req, "range");
        if (range != NULL) {
            int rr = parse_range(*range, fsize, &base, &rlen);
            if (rr < 0) {
                // tell the client what the resource will actually hand out
                char bad[64];
                snprintf(bad, sizeof(bad), "bytes */%zu", fsize);
                http_response_set_status(res, HTTP_416_RANGE_NOT_SATISFIABLE);
                http_response_set_header(res, "Content-Range", bad);
                strbuf_free(&path);
                return;
            }
            partial = rr > 0;
        }
    }

    char *data;
    size_t len;
    if (partial ? file_read_range(path.items, base, rlen, &data, &len) != 0
                : file_read_all(path.items, &data, &len) != 0) {
        reject(res, HTTP_404_NOT_FOUND);
        strbuf_free(&path);
        return;
    }
    strbuf_free(&path);

    if (partial) {
        char cr[64];
        snprintf(cr, sizeof(cr), "bytes %zu-%zu/%zu", base, base + len - 1, fsize);
        http_response_set_header(res, "Content-Range", cr);
        http_response_set_status(res, HTTP_206_PARTIAL_CONTENT);
    }

    http_response_add_body(res, (String_View){data, len});
    xfree(data);
}