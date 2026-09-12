#include "static.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "date.h"
#include "file.h"
#include "http.h"
#include "mime.h"
#include "path.h"
#include "request.h"
#include "response.h"
#include "router.h"
#include "strbuf.h"
#include "sv.h"
#include "url.h"
#include "xmem.h"

// Cache-Control max-age for static answers, 0 = leave them uncached
static unsigned long g_static_cache_max_age = 0;

void http_static_set_cache(unsigned long max_age)
{
    g_static_cache_max_age = max_age;
}

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

// a coalesced byte range: [base, base+n)
typedef struct {
    size_t base;
    size_t n;
} Byte_Range;

#define MAX_RANGES 64

// parses one "N-M", "N-", or "-N" byte-range-spec, clamping the window to the
// file end. returns 0 with *out filled when the spec asks for bytes the file
// has, -1 when it is unsatisfiable.
static int range_spec(String_View spec, size_t fsize, Byte_Range *out)
{
    spec = sv_trim(spec);
    String_View start = sv_chop_by_delim(&spec, '-');
    start = sv_trim(start);
    spec = sv_trim(spec);

    if (start.count == 0 && spec.count == 0) {
        return -1; // "bytes=-" with nothing at all
    }

    if (start.count > 0 && spec.count == 0) {
        // "N-": from N to the end
        long long from;
        if (!sv_to_i64(start, &from) || from < 0 || (size_t)from >= fsize) {
            return -1;
        }
        out->base = (size_t)from;
        out->n = fsize - out->base;
        return 0;
    }

    if (start.count == 0 && spec.count > 0) {
        // "-N": the final N bytes
        long long last;
        if (!sv_to_i64(spec, &last) || last <= 0 || fsize == 0) {
            return -1;
        }
        out->n = (size_t)last < fsize ? (size_t)last : fsize;
        out->base = fsize - out->n;
        return 0;
    }

    // "N-M": a closed window, clamped to the file end
    long long from, to;
    if (!sv_to_i64(start, &from) || !sv_to_i64(spec, &to) ||
        from < 0 || to < from || (size_t)from >= fsize) {
        return -1;
    }
    out->base = (size_t)from;
    size_t want = (size_t)(to - from) + 1;
    out->n = want < fsize - out->base ? want : fsize - out->base;
    return 0;
}

static int range_cmp(const void *a, const void *b)
{
    const Byte_Range *x = a;
    const Byte_Range *y = b;
    return (int)(x->base > y->base) - (int)(x->base < y->base);
}

// parses a "bytes=" range list (RFC 7233), coalescing overlapping and
// adjacent windows so no byte is served twice (RFC 9110 14.2). fills out up
// to cap entries and returns how many, 0 when no range applies (the whole
// file is served), and -1 when every spec is unsatisfiable (caller answers
// 416). a mix of satisfiable and not only keeps the satisfiable ones.
static int parse_ranges(String_View header, size_t fsize,
                        Byte_Range *out, size_t cap)
{
    header = sv_trim(header);
    if (!sv_consume_prefix(&header, "bytes=")) {
        return 0;
    }
    if (sv_count_char(header, ',') == 0) {
        int rr = range_spec(header, fsize, out);
        return rr < 0 ? -1 : 1;
    }

    Byte_Range raw[MAX_RANGES];
    size_t nraw = 0;
    String_View rest = header;
    while (rest.count > 0) {
        String_View spec = sv_chop_by_delim(&rest, ',');
        Byte_Range r;
        if (nraw < MAX_RANGES && range_spec(spec, fsize, &r) == 0) {
            raw[nraw++] = r;
        }
    }
    if (nraw == 0) {
        return -1;
    }
    qsort(raw, nraw, sizeof raw[0], range_cmp);
    size_t nout = 0;
    for (size_t i = 0; i < nraw && nout < cap; i++) {
        if (nout > 0 && raw[i].base <= out[nout - 1].base + out[nout - 1].n) {
            // overlaps or is adjacent to the coalesced window: extend it
            size_t end = raw[i].base + raw[i].n;
            size_t prev_end = out[nout - 1].base + out[nout - 1].n;
            if (end > prev_end) {
                out[nout - 1].n += end - prev_end;
            }
        } else {
            out[nout++] = raw[i];
        }
    }
    return (int)nout;
}

// does the If-Range validator (RFC 7233 3.2) still match the current copy?
// strong entity-tag comparison only -- a weak W/ tag never matches -- or else
// an HTTP-date that is not earlier than Last-Modified.
static bool if_range_matches(String_View v, const char *etag, time_t mtime)
{
    v = sv_trim(v);
    if (v.count >= 2 && v.data[0] == '"' && v.data[v.count - 1] == '"') {
        size_t n = strlen(etag);
        return n == v.count && memcmp(etag, v.data, n) == 0;
    }
    time_t since = http_date_parse(v);
    return since >= 0 && mtime <= since;
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

    // "public" lets proxies hold copies too; the directive rides on every
    // status this handler sends back, including a validating 304
    if (g_static_cache_max_age > 0) {
        char cc[64];
        snprintf(cc, sizeof(cc), "public, max-age=%lu", g_static_cache_max_age);
        http_response_set_header(res, "Cache-Control", cc);
    }

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

    // ranges only make sense for fetches; other verbs get the whole entity.
    // If-Range gates the whole request: when the validator it names no longer
    // matches the current copy, the Range header is ignored entirely and the
    // full body is served instead.
    Byte_Range ranges[MAX_RANGES];
    int nrange = 0;
    if (req->method == HTTP_GET || req->method == HTTP_HEAD) {
        const String_View *range = http_request_get_header(req, "range");
        if (range != NULL) {
            const String_View *ir = http_request_get_header(req, "if-range");
            if (ir == NULL || if_range_matches(*ir, etag, mtime)) {
                int rr = parse_ranges(*range, fsize, ranges, MAX_RANGES);
                if (rr < 0) {
                    // tell the client what the resource will actually hand out
                    char bad[64];
                    snprintf(bad, sizeof(bad), "bytes */%zu", fsize);
                    http_response_set_status(res, HTTP_416_RANGE_NOT_SATISFIABLE);
                    http_response_set_header(res, "Content-Range", bad);
                    strbuf_free(&path);
                    return;
                }
                nrange = rr;
            }
        }
    }

    if (nrange == 0) {
        char *data;
        size_t len;
        if (file_read_all(path.items, &data, &len) != 0) {
            reject(res, HTTP_404_NOT_FOUND);
            strbuf_free(&path);
            return;
        }
        http_response_add_body(res, (String_View){data, len});
        xfree(data);
        strbuf_free(&path);
        return;
    }

    // a single range is answered with one Content-Range; several coalesced
    // ranges become a multipart/byteranges body with one part per window
    Strbuf body;
    strbuf_init(&body);
    bool ok = true;
    if (nrange == 1) {
        char *data;
        size_t len;
        if (file_read_range(path.items, ranges[0].base, ranges[0].n,
                            &data, &len) != 0) {
            ok = false;
        } else {
            char cr[64];
            snprintf(cr, sizeof(cr), "bytes %zu-%zu/%zu", ranges[0].base,
                     ranges[0].base + len - 1, fsize);
            http_response_set_header(res, "Content-Range", cr);
            strbuf_append(&body, data, len);
            xfree(data);
        }
    } else {
        // a boundary unlikely to collide with the file bytes: mix the file
        // stats into a short prefix so two mounts of the same size differ
        char boundary[40];
        snprintf(boundary, sizeof(boundary), "----cweb%08llx-%zu",
                 (unsigned long long)mtime, fsize);
        char ctype[96];
        snprintf(ctype, sizeof(ctype), "multipart/byteranges; boundary=%s", boundary);
        http_response_set_header(res, "Content-Type", ctype);
        for (int i = 0; i < nrange && ok; i++) {
            char *data;
            size_t len;
            if (file_read_range(path.items, ranges[i].base, ranges[i].n,
                                &data, &len) != 0) {
                ok = false;
                break;
            }
            strbuf_append_cstr(&body, "\r\n--");
            strbuf_append_cstr(&body, boundary);
            strbuf_append_cstr(&body, "\r\nContent-Type: ");
            strbuf_append(&body, mime_z, strlen(mime_z));
            char cr[64];
            snprintf(cr, sizeof(cr), "\r\nContent-Range: bytes %zu-%zu/%zu\r\n\r\n",
                     ranges[i].base, ranges[i].base + len - 1, fsize);
            strbuf_append_cstr(&body, cr);
            strbuf_append(&body, data, len);
            xfree(data);
        }
        if (ok) {
            strbuf_append_cstr(&body, "\r\n--");
            strbuf_append_cstr(&body, boundary);
            strbuf_append_cstr(&body, "--\r\n");
        }
    }
    strbuf_free(&path);

    if (!ok) {
        strbuf_free(&body);
        reject(res, HTTP_404_NOT_FOUND);
        return;
    }
    http_response_set_status(res, HTTP_206_PARTIAL_CONTENT);
    http_response_add_body(res, (String_View){body.items, body.count});
    strbuf_free(&body);
}

// --- prefix mounts -------------------------------------------------------

// everything under url_prefix is peeled off the request path so the file
// handler only ever sees the part meant for the disk root
typedef struct {
    char *url_prefix;
    char *fs_root;
} Static_Mount;

static void http_static_mount_handler(Http_Request *req, Http_Response *res,
                                      Str_Map *params, void *user_data)
{
    Static_Mount *m = user_data;
    (void)params;

    String_View original = req->path;
    Strbuf sub;
    strbuf_init(&sub);
    size_t plen = strlen(m->url_prefix);
    if (plen < req->path.count) {
        strbuf_append(&sub, req->path.data + plen, req->path.count - plen);
    }
    if (sub.count == 0) {
        strbuf_append_cstr(&sub, "/"); // the mount root itself
    }
    req->path = (String_View){sub.items, sub.count};
    http_serve_static(req, res, m->fs_root);
    strbuf_free(&sub);
    req->path = original;
}

int http_static_mount(Http_Router *r, const char *url_prefix, const char *fs_root)
{
    // the mount outlives the calls, so its strings are owned copies and only
    // freed when the process (and the router with it) goes away
    Static_Mount *mount = xmalloc(sizeof *mount);
    mount->url_prefix = xmalloc(strlen(url_prefix) + 1);
    strcpy(mount->url_prefix, url_prefix);
    mount->fs_root = xmalloc(strlen(fs_root) + 1);
    strcpy(mount->fs_root, fs_root);

    char pattern[1024];
    int n = snprintf(pattern, sizeof(pattern), "%s/*", url_prefix);
    if (n <= 0 || (size_t)n >= sizeof(pattern)) {
        xfree(mount->url_prefix);
        xfree(mount->fs_root);
        xfree(mount);
        return -1;
    }
    return router_add(r, HTTP_GET, pattern, http_static_mount_handler, mount);
}

int http_static_unmount(Http_Router *r, const char *url_prefix)
{
    char pattern[1024];
    int n = snprintf(pattern, sizeof(pattern), "%s/*", url_prefix);
    if (n <= 0 || (size_t)n >= sizeof(pattern)) {
        return -1;
    }
    void *ud = NULL;
    if (router_remove(r, HTTP_GET, pattern, &ud) != 0) {
        return -1;
    }
    Static_Mount *mount = ud;
    if (mount != NULL) {
        xfree(mount->url_prefix);
        xfree(mount->fs_root);
        xfree(mount);
    }
    return 0;
}