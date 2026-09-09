#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "gzip.h"
#include "request.h"
#include "response.h"
#include "strbuf.h"
#include "sv.h"
#include "xmem.h"
#include <zlib.h>

static int fails = 0;
static void check(const char *what, int cond)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        fails++;
    }
}

// decompress a gzip blob so the round trip can be asserted
static int gunzip(const char *src, size_t len, Strbuf *out)
{
    z_stream z;
    memset(&z, 0, sizeof z);
    if (inflateInit2(&z, 15 + 32) != Z_OK) { // 15+32 = gzip autodetect
        return -1;
    }
    z.next_in = (Bytef *)(uintptr_t)src;
    z.avail_in = (uInt)len;
    char tmp[512];
    int rc;
    do {
        z.next_out = (Bytef *)tmp;
        z.avail_out = sizeof tmp;
        rc = inflate(&z, Z_NO_FLUSH);
        if (rc != Z_OK && rc != Z_STREAM_END) {
            inflateEnd(&z);
            return -1;
        }
        strbuf_append(out, tmp, sizeof tmp - z.avail_out);
    } while (rc != Z_STREAM_END);
    inflateEnd(&z);
    return 0;
}

// a trivial handler that stuffs a big marker body into the response
static void fat_handler(Http_Request *req, Http_Response *res, void *user_data)
{
    (void)req;
    http_response_set_header(res, "Content-Type", "text/html");
    for (int i = 0; i < 100; i++) {
        http_response_add_body_cstr(res,
            "<p>lorem ipsum dolor sit amet, consectetur adipiscing elit</p>\n");
    }
    (void)user_data;
}

// a handler that announces a non-compressible type: an image should never be
// gzipped even when the client accepts it
static void binary_handler(Http_Request *req, Http_Response *res, void *user_data)
{
    (void)req;
    http_response_set_header(res, "Content-Type", "image/png");
    for (int i = 0; i < 100; i++) {
        http_response_add_body_cstr(res,
            "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\n");
    }
    (void)user_data;
}

int main(void)
{
    // unit: compress a known body and read it back
    {
        const char *text = "the quick brown fox jumps over the lazy dog. "
                           "the quick brown fox jumps over the lazy dog.";
        Strbuf gz;
        strbuf_init(&gz);
        check("compress succeeds", http_gzip_compress(text, strlen(text), &gz) == 0);
        check("gzip magic", gz.count >= 18 && (unsigned char)gz.items[0] == 0x1f &&
                            (unsigned char)gz.items[1] == 0x8b);
        Strbuf back;
        strbuf_init(&back);
        int gzip_ok = gunzip(gz.items, gz.count, &back) == 0;
        strbuf_null_terminate(&back);
        check("round trips through gunzip",
              gzip_ok && back.count == strlen(text) &&
              strcmp(back.items, text) == 0);
        strbuf_free(&back);
        strbuf_free(&gz);
    }

    // middleware: compresses a compressible content-type when asked
    {
        Http_Request req;
        http_request_parse(&req, sv_from_cstr(
            "GET / HTTP/1.1\r\nHost: x\r\nAccept-Encoding: gzip\r\n\r\n"));
        Http_Response res;
        http_response_init(&res);
        Http_Gzip_Opts opts = {.min_bytes = 0};
        http_gzip_middleware(&req, &res, &opts, fat_handler, NULL);
        check("response got gzip encoding header",
              http_response_has_header(&res, "Content-Encoding") &&
              strstr(res.headers.items, "Content-Encoding: gzip") != NULL);
        check("response grew a Vary header",
              strstr(res.headers.items, "Vary: Accept-Encoding") != NULL);
        check("body actually shrank",
              res.body.count < 100 * strlen(
                  "<p>lorem ipsum dolor sit amet, consectetur adipiscing elit</p>\n"));
        // the compressed bytes must decompress back to the source
        Strbuf back;
        strbuf_init(&back);
        check("compressed middleware body is valid gzip",
              gunzip(res.body.items, res.body.count, &back) == 0 &&
              back.count == 100 * strlen(
                  "<p>lorem ipsum dolor sit amet, consectetur adipiscing elit</p>\n"));
        strbuf_free(&back);
        http_response_free(&res);
        http_request_free(&req);
    }

    // middleware: does not gzip when the client refuses q=0
    {
        Http_Request req;
        http_request_parse(&req, sv_from_cstr(
            "GET / HTTP/1.1\r\nHost: x\r\nAccept-Encoding: gzip;q=0\r\n\r\n"));
        Http_Response res;
        http_response_init(&res);
        Http_Gzip_Opts opts = {.min_bytes = 0};
        http_gzip_middleware(&req, &res, &opts, fat_handler, NULL);
        check("gzip;q=0 is not encoded",
              !http_response_has_header(&res, "Content-Encoding"));
        http_response_free(&res);
        http_request_free(&req);
    }

    // middleware: no Accept-Encoding means no compression
    {
        Http_Request req;
        http_request_parse(&req, sv_from_cstr("GET / HTTP/1.1\r\nHost: x\r\n\r\n"));
        Http_Response res;
        http_response_init(&res);
        http_gzip_middleware(&req, &res, NULL, fat_handler, NULL);
        check("no accept-encoding means unchanged",
              !http_response_has_header(&res, "Content-Encoding"));
        http_response_free(&res);
        http_request_free(&req);
    }

    // middleware: non-compressible content-type is left alone
    {
        Http_Request req;
        http_request_parse(&req, sv_from_cstr(
            "GET / HTTP/1.1\r\nHost: x\r\nAccept-Encoding: gzip\r\n\r\n"));
        Http_Response res;
        http_response_init(&res);
        http_gzip_middleware(&req, &res, NULL, binary_handler, NULL);
        check("non-compressible content-type is fluid",
              !http_response_has_header(&res, "Content-Encoding"));
        http_response_free(&res);
        http_request_free(&req);
    }

    // a real dispatch chain: the middleware wired around router dispatch
    {
        Http_Middleware_Chain chain;
        http_middleware_init(&chain);
        http_middleware_add(&chain, http_gzip_middleware, NULL);
        void *chain_data = NULL;
        Http_Handler_Fn dispatch =
            http_middleware_build(&chain, fat_handler, NULL, &chain_data);
        Http_Request req;
        http_request_parse(&req, sv_from_cstr(
            "GET / HTTP/1.1\r\nHost: x\r\nAccept-Encoding: gzip\r\n\r\n"));
        Http_Response res;
        http_response_init(&res);
        dispatch(&req, &res, chain_data);
        check("built chain compresses",
              http_response_has_header(&res, "Content-Encoding"));
        Strbuf back;
        strbuf_init(&back);
        check("built chain body is valid gzip",
              gunzip(res.body.items, res.body.count, &back) == 0);
        strbuf_free(&back);
        http_response_free(&res);
        http_request_free(&req);
        http_middleware_data_free(chain_data);
        http_middleware_free(&chain);
    }

    if (fails == 0) {
        printf("gzip ok\n");
    }
    return fails != 0;
}