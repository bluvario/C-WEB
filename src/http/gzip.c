#include "gzip.h"

#include <stdio.h>
#include <string.h>

#include <zlib.h>

#include "request.h"
#include "response.h"
#include "sv.h"
#include "xmem.h"

#define HTTP_GZIP_MIN_DEFAULT (size_t)1024

static bool client_wants_gzip(Http_Request *req)
{
    const String_View *ae = http_request_get_header(req, "accept-encoding");
    if (ae == NULL) {
        return false;
    }
    String_View rest = *ae;
    while (rest.count > 0) {
        String_View token = sv_trim(sv_chop_by_delim(&rest, ','));
        if (token.count >= 4 && sv_starts_with(token, "gzip")) {
            // a q=0 clause refuses gzip; q=0.5 still allows it
            for (size_t i = 0; i + 3 <= token.count; i++) {
                if (token.data[i] == 'q' && token.data[i + 1] == '=' &&
                    token.data[i + 2] == '0' &&
                    (i + 3 >= token.count || token.data[i + 3] < '1' ||
                     token.data[i + 3] > '9')) {
                    return false;
                }
            }
            return true;
        }
    }
    return false;
}

static bool type_is_compressible(const char *ctype, const Http_Gzip_Opts *opts)
{
    const char *space = (opts != NULL && opts->types != NULL)
                            ? opts->types
                            : HTTP_GZIP_DEFAULT_TYPES;
    if (ctype == NULL || space == NULL) {
        return false;
    }
    const char *q = space;
    while (*q != '\0') {
        while (*q == ' ') {
            q++;
        }
        const char *start = q;
        while (*q != '\0' && *q != ' ') {
            q++;
        }
        size_t len = (size_t)(q - start);
        if (len > 0 && strncmp(ctype, start, len) == 0) {
            return true;
        }
    }
    return false;
}

int http_gzip_compress(const char *src, size_t len, Strbuf *out)
{
    // raw DEFLATE into a freshly sized buffer, then wrapped in a gzip header.
    z_stream z;
    memset(&z, 0, sizeof z);
    if (deflateInit2(&z, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -15,
                     8, Z_DEFAULT_STRATEGY) != Z_OK) {
        return -1;
    }
    // worst case: the DEFLATE stream is at most a hair larger than the input
    size_t cap = compressBound(len);
    unsigned char *tmp = xmalloc(cap + 64);
    z.next_in = (Bytef *)(uintptr_t)src;
    z.avail_in = (uInt)len;
    z.next_out = tmp + 64;
    z.avail_out = (uInt)cap;
    int rc = deflate(&z, Z_FINISH);
    int ok = (rc == Z_STREAM_END);
    size_t zlen = ok ? (size_t)z.total_out : 0;
    ok = ok && deflateEnd(&z) == Z_OK;
    if (!ok) {
        xfree(tmp);
        return -1;
    }

    strbuf_free(out);
    strbuf_init(out);
    // gzip header: magic, method 8, no flags, mtime 0, XFL 2 (best), unix OS
    static const unsigned char head[10] = {
        0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0xff};
    strbuf_append(out, (const char *)head, sizeof head);
    strbuf_append(out, (const char *)(tmp + 64), zlen);
    // trailer: crc32 of the original input then its length mod 2^32
    unsigned long crc = crc32(crc32(0L, Z_NULL, 0),
                              (const Bytef *)src, (uInt)len);
    unsigned char tail[8];
    tail[0] = (unsigned char)(crc & 0xff);
    tail[1] = (unsigned char)((crc >> 8) & 0xff);
    tail[2] = (unsigned char)((crc >> 16) & 0xff);
    tail[3] = (unsigned char)((crc >> 24) & 0xff);
    tail[4] = (unsigned char)(len & 0xff);
    tail[5] = (unsigned char)((len >> 8) & 0xff);
    tail[6] = (unsigned char)((len >> 16) & 0xff);
    tail[7] = (unsigned char)((len >> 24) & 0xff);
    strbuf_append(out, (const char *)tail, sizeof tail);

    xfree(tmp);
    return 0;
}

int http_gzip_decompress(const char *src, size_t len, size_t cap,
                         unsigned char **out, size_t *out_len)
{
    if (cap == 0) {
        return -2; // nothing allowed through a zero ceiling
    }
    z_stream z;
    memset(&z, 0, sizeof z);
    // windowBits 15 + 16 selects the RFC 1952 gzip wrapper (magic, header,
    // crc32 trailer), which the response compressor emits
    if (inflateInit2(&z, 15 + 16) != Z_OK) {
        return -1;
    }

    // grow the output in fixed steps, halting once the ceiling is crossed
    size_t step = cap < 65536 ? cap : 65536;
    unsigned char *buf = xmalloc(step);
    z.next_out = buf;
    z.avail_out = (uInt)step;
    size_t in_off = 0;
    int rc = 0;
    for (;;) {
        if (z.avail_in == 0 && in_off < len) {
            size_t take = len - in_off;
            if (take > (size_t)(1u << 30)) {
                take = 1u << 30; // keep the uInt input counters in range
            }
            z.next_in = (Bytef *)(uintptr_t)(src + in_off);
            z.avail_in = (uInt)take;
            in_off += take;
        }
        int ir = inflate(&z, Z_NO_FLUSH);
        if (ir == Z_STREAM_END) {
            break;
        }
        if (ir != Z_OK && ir != Z_BUF_ERROR) {
            rc = -1; // Z_DATA_ERROR (bad bytes/crc), Z_MEM_ERROR, Z_NEED_DICT
            break;
        }
        if (z.avail_out == 0) {
            if (z.total_out >= cap) {
                rc = -2;
                break; // compressed bomb: the stream would pass the ceiling
            }
            size_t add = cap - z.total_out < step ? cap - z.total_out : step;
            buf = xrealloc(buf, z.total_out + add);
            z.next_out = buf + z.total_out;
            z.avail_out = (uInt)add;
            continue;
        }
        if (z.avail_in == 0 || ir == Z_BUF_ERROR) {
            rc = -1; // input ran out before the stream ended: truncated
            break;
        }
    }
    inflateEnd(&z);
    if (rc != 0) {
        xfree(buf);
        return rc;
    }
    if (z.total_out == 0) {
        xfree(buf);
        *out = NULL;
        *out_len = 0;
    } else {
        *out = xrealloc(buf, z.total_out);
        *out_len = z.total_out;
    }
    return 0;
}

void http_gzip_middleware(Http_Request *req, Http_Response *res,
                          void *user_data,
                          Http_Handler_Fn next, void *next_data)
{
    next(req, res, next_data);

    Http_Gzip_Opts *opts = user_data;

    // streamed bodies can't be buffered; assembled ones can be inspected
    if (res->stream_fn != NULL || res->suppress_body || res->body.count == 0) {
        return;
    }
    // only a happy 200 with an entity is worth this
    if (res->status != HTTP_200_OK) {
        return;
    }
    if (!client_wants_gzip(req)) {
        return;
    }
    if (!http_response_has_header(res, "Content-Encoding")) {
        char *ctype = http_response_get_header(res, "Content-Type");
        bool compressible = type_is_compressible(ctype, opts);
        xfree(ctype);
        if (!compressible) {
            return;
        }
        size_t min = (opts != NULL && opts->min_bytes) ? opts->min_bytes
                                                       : HTTP_GZIP_MIN_DEFAULT;
        if (res->body.count < min) {
            return;
        }
        Strbuf gz;
        strbuf_init(&gz);
        if (http_gzip_compress(res->body.items, res->body.count, &gz) == 0) {
            strbuf_free(&res->body);
            res->body = gz; // ownership moves; server recomputes Content-Length
            http_response_set_header(res, "Content-Encoding", "gzip");
            // Vary may already list Origin etc., so append the new selector
            http_response_append_header(res, "Vary", "Accept-Encoding");
        } else {
            strbuf_free(&gz);
        }
    }
}