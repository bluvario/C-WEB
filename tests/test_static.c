#define _XOPEN_SOURCE 700

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "request.h"
#include "response.h"
#include "static.h"
#include "strbuf.h"
#include "sv.h"
#include "date.h"

static void write_fixture(const char *path, const char *content)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "fixture write failed: %s\n", path);
        _exit(1);
    }
    fwrite(content, 1, strlen(content), f);
    fclose(f);
}

int main(void)
{
    const char *root = "/tmp/cweb_static_test_XXXXXX";
    char rootbuf[64];
    strcpy(rootbuf, root);
    if (mkdtemp(rootbuf) == NULL) {
        fprintf(stderr, "mkdtemp failed\n");
        return 1;
    }

    Strbuf hello, subdir, indexf, pngf;
    strbuf_init(&hello);
    strbuf_init(&subdir);
    strbuf_init(&indexf);
    strbuf_init(&pngf);
    strbuf_append_cstr(&hello, rootbuf);
    strbuf_append_cstr(&hello, "/hello.txt");
    strbuf_append_cstr(&subdir, rootbuf);
    strbuf_append_cstr(&subdir, "/sub");
    strbuf_null_terminate(&subdir);
    strbuf_append_cstr(&indexf, subdir.items);
    strbuf_append_cstr(&indexf, "/index.html");
    strbuf_append_cstr(&pngf, rootbuf);
    strbuf_append_cstr(&pngf, "/pic.png");
    strbuf_null_terminate(&hello);
    strbuf_null_terminate(&indexf);
    strbuf_null_terminate(&pngf);

    mkdir(subdir.items, 0755);
    write_fixture(hello.items, "hello static");
    write_fixture(indexf.items, "<h1>Hi</h1>");
    write_fixture(pngf.items, "not really a png but fine");

    struct stat st;
    if (stat(indexf.items, &st) != 0) {
        fprintf(stderr, "index fixture missing: %s\n", indexf.items);
        return 1;
    }

    Http_Request req;
    Http_Response res;
    Strbuf wire;

    http_request_parse(&req, sv_from_cstr("GET /hello.txt HTTP/1.1\r\nHost: x\r\n\r\n"));
    http_response_init(&res);
    http_serve_static(&req, &res, rootbuf);
    strbuf_init(&wire);
    http_response_serialize(&res, &wire);
    strbuf_null_terminate(&wire);
    const char *expect = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n";
    if (strncmp(wire.items, expect, strlen(expect)) != 0) {
        fprintf(stderr, "text file status or type wrong:\n%s\n", wire.items);
        return 1;
    }
    if (strstr(wire.items, "Content-Length: 12\r\n") == NULL ||
        res.body.count != 12 ||
        memcmp(res.body.items, "hello static", 12) != 0) {
        fprintf(stderr, "text file body wrong:\n%s\n", wire.items);
        return 1;
    }
    if (strstr(wire.items, "Last-Modified: ") == NULL) {
        fprintf(stderr, "no Last-Modified header\n");
        return 1;
    }
    // lift the ETag out of this response to feed the If-None-Match probes
    const char *etag_at = strstr(wire.items, "\r\nETag: ");
    if (etag_at == NULL) {
        fprintf(stderr, "no ETag header\n");
        return 1;
    }
    char etag[128];
    etag_at += 8;
    const char *eol = strstr(etag_at, "\r\n");
    if (eol == NULL || eol - etag_at >= (long)sizeof(etag)) {
        fprintf(stderr, "ETag malformed\n");
        return 1;
    }
    memcpy(etag, etag_at, (size_t)(eol - etag_at));
    etag[eol - etag_at] = '\0';
    strbuf_free(&wire);
    http_response_free(&res);
    http_request_free(&req);

    // directory serves index.html
    http_request_parse(&req, sv_from_cstr("GET /sub/ HTTP/1.1\r\nHost: x\r\n\r\n"));
    http_response_init(&res);
    http_serve_static(&req, &res, rootbuf);
    strbuf_init(&wire);
    http_response_serialize(&res, &wire);
    strbuf_null_terminate(&wire);
    if (strstr(wire.items, "HTTP/1.1 200 OK") == NULL || strstr(wire.items, "<h1>Hi</h1>") == NULL) {
        fprintf(stderr, "index.html not served:\n%s\n", wire.items);
        return 1;
    }
    strbuf_free(&wire);
    http_response_free(&res);
    http_request_free(&req);

    // png gets image/png
    http_request_parse(&req, sv_from_cstr("GET /pic.png HTTP/1.1\r\nHost: x\r\n\r\n"));
    http_response_init(&res);
    http_serve_static(&req, &res, rootbuf);
    strbuf_init(&wire);
    http_response_serialize(&res, &wire);
    strbuf_null_terminate(&wire);
    if (strstr(wire.items, "Content-Type: image/png") == NULL) {
        fprintf(stderr, "png mime wrong:\n%s\n", wire.items);
        return 1;
    }
    // the 200 path advertises its Last-Modified time
    if (strstr(wire.items, "Last-Modified: ") == NULL) {
        fprintf(stderr, "no Last-Modified header\n");
        return 1;
    }
    strbuf_free(&wire);
    http_response_free(&res);
    http_request_free(&req);

    // a copy the client already holds comes back as 304 with no body
    http_request_parse(&req, sv_from_cstr(
        "GET /hello.txt HTTP/1.1\r\nHost: x\r\nIf-Modified-Since: Sat, 01 Jan 2100 00:00:00 GMT\r\n\r\n"));
    http_response_init(&res);
    http_serve_static(&req, &res, rootbuf);
    if (res.status != HTTP_304_NOT_MODIFIED) {
        fprintf(stderr, "stale client should get 304, got %d\n", (int)res.status);
        return 1;
    }
    strbuf_init(&wire);
    http_response_serialize(&res, &wire);
    strbuf_null_terminate(&wire);
    if (strstr(wire.items, "HTTP/1.1 304 Not Modified") == NULL ||
        res.body.count != 0) {
        fprintf(stderr, "304 carried a body or a bad status line:\n%s\n", wire.items);
        return 1;
    }
    strbuf_free(&wire);
    http_response_free(&res);
    http_request_free(&req);

    // an unparseable If-Modified-Since must not break the 200
    http_request_parse(&req, sv_from_cstr(
        "GET /hello.txt HTTP/1.1\r\nHost: x\r\nIf-Modified-Since: some day\r\n\r\n"));
    http_response_init(&res);
    http_serve_static(&req, &res, rootbuf);
    if (res.status != HTTP_200_OK) {
        fprintf(stderr, "garbage If-Modified-Since should fall through to 200, got %d\n", (int)res.status);
        return 1;
    }
    http_response_free(&res);
    http_request_free(&req);

    // the exact ETag proves the copy is current
    Strbuf probe;
    strbuf_init(&probe);
    strbuf_append_cstr(&probe, "GET /hello.txt HTTP/1.1\r\nHost: x\r\nIf-None-Match: ");
    strbuf_append_cstr(&probe, etag);
    strbuf_append_cstr(&probe, "\r\n\r\n");
    strbuf_null_terminate(&probe);
    http_request_parse(&req, (String_View){probe.items, probe.count});
    http_response_init(&res);
    http_serve_static(&req, &res, rootbuf);
    if (res.status != HTTP_304_NOT_MODIFIED) {
        fprintf(stderr, "matching If-None-Match should be 304, got %d\n", (int)res.status);
        return 1;
    }
    http_response_free(&res);
    http_request_free(&req);
    strbuf_free(&probe);

    // "*" also stands for any existing resource
    http_request_parse(&req, sv_from_cstr(
        "GET /hello.txt HTTP/1.1\r\nHost: x\r\nIf-None-Match: *\r\n\r\n"));
    http_response_init(&res);
    http_serve_static(&req, &res, rootbuf);
    if (res.status != HTTP_304_NOT_MODIFIED) {
        fprintf(stderr, "If-None-Match * should be 304, got %d\n", (int)res.status);
        return 1;
    }
    http_response_free(&res);
    http_request_free(&req);

    // a mismatched ETag ignores a stale If-Modified-Since entirely
    http_request_parse(&req, sv_from_cstr(
        "GET /hello.txt HTTP/1.1\r\nHost: x\r\n"
        "If-None-Match: \"totally-different\"\r\n"
        "If-Modified-Since: Sat, 01 Jan 2100 00:00:00 GMT\r\n\r\n"));
    http_response_init(&res);
    http_serve_static(&req, &res, rootbuf);
    if (res.status != HTTP_200_OK) {
        fprintf(stderr, "If-None-Match mismatch must trump stale If-Modified-Since, got %d\n", (int)res.status);
        return 1;
    }
    http_response_free(&res);
    http_request_free(&req);

    // Range: bytes=0-4 slices the first five bytes
    http_request_parse(&req, sv_from_cstr(
        "GET /hello.txt HTTP/1.1\r\nHost: x\r\nRange: bytes=0-4\r\n\r\n"));
    http_response_init(&res);
    http_serve_static(&req, &res, rootbuf);
    if (res.status != HTTP_206_PARTIAL_CONTENT) {
        fprintf(stderr, "range should be 206, got %d\n", (int)res.status);
        return 1;
    }
    strbuf_init(&wire);
    http_response_serialize(&res, &wire);
    strbuf_null_terminate(&wire);
    if (res.body.count != 5 || memcmp(res.body.items, "hello", 5) != 0 ||
        strstr(wire.items, "Content-Range: bytes 0-4/12\r\n") == NULL) {
        fprintf(stderr, "range body or Content-Range wrong:\n%s\n", wire.items);
        return 1;
    }
    strbuf_free(&wire);
    http_response_free(&res);
    http_request_free(&req);

    // Range: bytes=6- means "from the final word onward"
    http_request_parse(&req, sv_from_cstr(
        "GET /hello.txt HTTP/1.1\r\nHost: x\r\nRange: bytes=6-\r\n\r\n"));
    http_response_init(&res);
    http_serve_static(&req, &res, rootbuf);
    if (res.status != HTTP_206_PARTIAL_CONTENT ||
        res.body.count != 6 || memcmp(res.body.items, "static", 6) != 0) {
        fprintf(stderr, "open-ended range wrong, got %d\n", (int)res.status);
        return 1;
    }
    http_response_free(&res);
    http_request_free(&req);

    // Range: bytes=-6 takes the final six bytes
    http_request_parse(&req, sv_from_cstr(
        "GET /hello.txt HTTP/1.1\r\nHost: x\r\nRange: bytes=-6\r\n\r\n"));
    http_response_init(&res);
    http_serve_static(&req, &res, rootbuf);
    if (res.status != HTTP_206_PARTIAL_CONTENT ||
        res.body.count != 6 || memcmp(res.body.items, "static", 6) != 0) {
        fprintf(stderr, "suffix range wrong, got %d\n", (int)res.status);
        return 1;
    }
    http_response_free(&res);
    http_request_free(&req);

    // past-the-end ranges are 416 with the resource's real span
    http_request_parse(&req, sv_from_cstr(
        "GET /hello.txt HTTP/1.1\r\nHost: x\r\nRange: bytes=100-\r\n\r\n"));
    http_response_init(&res);
    http_serve_static(&req, &res, rootbuf);
    if (res.status != HTTP_416_RANGE_NOT_SATISFIABLE) {
        fprintf(stderr, "out of range should be 416, got %d\n", (int)res.status);
        return 1;
    }
    strbuf_init(&wire);
    http_response_serialize(&res, &wire);
    strbuf_null_terminate(&wire);
    if (strstr(wire.items, "Content-Range: bytes */12\r\n") == NULL) {
        fprintf(stderr, "416 missing span header:\n%s\n", wire.items);
        return 1;
    }
    strbuf_free(&wire);
    http_response_free(&res);
    http_request_free(&req);

    // a multi-range list is answered in full because one 200 covers them all
    http_request_parse(&req, sv_from_cstr(
        "GET /hello.txt HTTP/1.1\r\nHost: x\r\nRange: bytes=0-2,4-5\r\n\r\n"));
    http_response_init(&res);
    http_serve_static(&req, &res, rootbuf);
    if (res.status != HTTP_200_OK || res.body.count != 12) {
        fprintf(stderr, "multi-range should fall back to full 200, got %d\n", (int)res.status);
        return 1;
    }
    http_response_free(&res);
    http_request_free(&req);

    // traversal is refused
    http_request_parse(&req, sv_from_cstr("GET /../etc/passwd HTTP/1.1\r\nHost: x\r\n\r\n"));
    http_response_init(&res);
    http_serve_static(&req, &res, rootbuf);
    if (res.status != HTTP_400_BAD_REQUEST) {
        fprintf(stderr, "traversal should be 400, got %d\n", (int)res.status);
        return 1;
    }
    http_response_free(&res);
    http_request_free(&req);

    // missing file is 404
    http_request_parse(&req, sv_from_cstr("GET /nope.txt HTTP/1.1\r\nHost: x\r\n\r\n"));
    http_response_init(&res);
    http_serve_static(&req, &res, rootbuf);
    if (res.status != HTTP_404_NOT_FOUND) {
        fprintf(stderr, "missing file should be 404, got %d\n", (int)res.status);
        return 1;
    }
    http_response_free(&res);
    http_request_free(&req);

    // Cache-Control stays off until http_static_set_cache() says otherwise
    http_request_parse(&req, sv_from_cstr("GET /hello.txt HTTP/1.1\r\nHost: x\r\n\r\n"));
    http_response_init(&res);
    http_serve_static(&req, &res, rootbuf);
    strbuf_init(&wire);
    http_response_serialize(&res, &wire);
    strbuf_null_terminate(&wire);
    if (strstr(wire.items, "Cache-Control:") != NULL) {
        fprintf(stderr, "static files should be uncached by default:\n%s\n", wire.items);
        return 1;
    }
    strbuf_free(&wire);
    http_response_free(&res);
    http_request_free(&req);

    // a configured max-age stamps every successful answer, 304 included
    http_static_set_cache(300);
    http_request_parse(&req, sv_from_cstr("GET /hello.txt HTTP/1.1\r\nHost: x\r\n\r\n"));
    http_response_init(&res);
    http_serve_static(&req, &res, rootbuf);
    strbuf_init(&wire);
    http_response_serialize(&res, &wire);
    strbuf_null_terminate(&wire);
    if (strstr(wire.items, "Cache-Control: public, max-age=300\r\n") == NULL) {
        fprintf(stderr, "200 should carry max-age after the toggle:\n%s\n", wire.items);
        return 1;
    }
    strbuf_free(&wire);
    http_response_free(&res);
    http_request_free(&req);

    // a validating 304 from a cached client keeps the freshness directive
    http_request_parse(&req, sv_from_cstr(
        "GET /hello.txt HTTP/1.1\r\nHost: x\r\nIf-None-Match: *\r\n\r\n"));
    http_response_init(&res);
    http_serve_static(&req, &res, rootbuf);
    strbuf_init(&wire);
    http_response_serialize(&res, &wire);
    strbuf_null_terminate(&wire);
    if (res.status != HTTP_304_NOT_MODIFIED ||
        strstr(wire.items, "Cache-Control: public, max-age=300\r\n") == NULL) {
        fprintf(stderr, "304 should keep the max-age directive:\n%s\n", wire.items);
        return 1;
    }
    strbuf_free(&wire);
    http_response_free(&res);
    http_request_free(&req);

    rmdir(subdir.items);
    unlink(hello.items);
    unlink(indexf.items);
    unlink(pngf.items);
    rmdir(rootbuf);
    strbuf_free(&hello);
    strbuf_free(&subdir);
    strbuf_free(&indexf);
    strbuf_free(&pngf);

    printf("static ok\n");
    return 0;
}