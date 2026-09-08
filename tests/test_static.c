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