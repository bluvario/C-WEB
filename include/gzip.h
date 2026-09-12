#ifndef CWEB_GZIP_H
#define CWEB_GZIP_H

#include <stddef.h>

#include "middleware.h"
#include "strbuf.h"

// gzip compression for on-the-wire responses. a middleware that, after the
// handler has filled an in-memory body, compresses it with zlib, stamps
// Content-Encoding: gzip and Vary: Accept-Encoding, and lets the server
// recompute Content-Length. nothing is gzipped when the client did not send
// Accept-Encoding: gzip, the body is already encoded, the answer is a
// redirect/error with no entity, the body is streamed (can't buffer it), or
// it is too small to be worth the overhead.

// options fed to http_gzip_middleware as user_data (may be statically
// zero-initialized; all fields optional).
typedef struct {
    size_t min_bytes;    // skip bodies shorter than this; 0 = default (1024)
    const char *types;   // space-separated substrings to match a Content-Type;
                         // NULL = the default compressible set below
} Http_Gzip_Opts;

// the default set of Content-Type substrings considered compressible.
#define HTTP_GZIP_DEFAULT_TYPES \
    "text/html text/css text/plain text/xml text/javascript " \
    "application/javascript application/json application/xml application/xhtml+xml"

// compresses src (raw bytes) into *out with gzip framing; returns 0 on
// success. *out must be initialized by the caller and is reset here.
int http_gzip_compress(const char *src, size_t len, Strbuf *out);

// the ceiling a Content-Encoding: gzip request body may expand to when the
// serving config leaves max_inflated zero (the "128 MiB" default).
#define HTTP_GZIP_MAX_INFLATED_DEFAULT (size_t)(128u * 1024 * 1024)

// decompresses src, which must be a gzip stream (RFC 1952 framing), into a
// freshly allocated buffer of exactly *out_len bytes. cap bounds how large
// the output is allowed to get, so a compressed bomb cannot exhaust a
// worker's RAM: returns 0 on success (*out owned by the caller), -1 when
// src is not valid gzip (or is truncated), -2 when the payload would
// exceed cap.
int http_gzip_decompress(const char *src, size_t len, size_t cap,
                         unsigned char **out, size_t *out_len);

// the middleware entry point; pass Http_Gzip_Opts* as user_data.
void http_gzip_middleware(Http_Request *req, Http_Response *res,
                          void *user_data,
                          Http_Handler_Fn next, void *next_data);

#endif