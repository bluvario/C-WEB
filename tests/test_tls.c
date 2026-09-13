#define _POSIX_C_SOURCE 200809L

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "gzip.h"
#include "hpack.h"
#include "log.h"
#include "net.h"
#include "server.h"
#include "strbuf.h"
#include "tls.h"

// a self-signed dev cert + key (CN=localhost, SANs localhost/127.0.0.1,
// valid ~50 years) so the TLS tests run hermetically with SSL_VERIFY_NONE.
// test material only: the server never sees a trust chain requirement.
static const char TEST_CERT_PEM[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIDVzCCAj+gAwIBAgIUYTIOeaI5LWGRGnL7ryJ0IkpZM40wDQYJKoZIhvcNAQEL\n"
    "BQAwLDESMBAGA1UEAwwJbG9jYWxob3N0MRYwFAYDVQQKDA1DLVdFQiBUZXN0IENB\n"
    "MCAXDTI2MDkxMjEzMzQ0N1oYDzIwNzYwODMwMTMzNDQ3WjAsMRIwEAYDVQQDDAls\n"
    "b2NhbGhvc3QxFjAUBgNVBAoMDUMtV0VCIFRlc3QgQ0EwggEiMA0GCSqGSIb3DQEB\n"
    "AQUAA4IBDwAwggEKAoIBAQCM75mtkldSVZYQFFhX4iWDJVxlSpbwAbvgqC0l8ZdV\n"
    "fBTYwtMuWPqItVSxueh7r5QZLPVkg3PMv8pGwv1H+fHmFc2sAKAwLWKNVo+koyQf\n"
    "Tcy7ESJ5n2a+6EbrXC7HJLBKeRfqnzH4MIOIdCOcypyFBPct01BhdGtAdNzPUgOA\n"
    "fEdqKc9ZrO6mkdt9aZvmV/lF8fVLsfDyWES8ssVXSms2W2M5EcMTlbB07a2VRuWa\n"
    "zq/GJvKGnyHD6T6sE1FYksWj04pzRSv8qZcoUPgcqQGRwrzenbQyy3Ai8ktbTemE\n"
    "cdMqnv+SrsiCvOE6Y9ygzSuMCSCXynbGkD5UsNSir/xHAgMBAAGjbzBtMB0GA1Ud\n"
    "DgQWBBTFh+bjavTljwqXFLaGMabflEO5hDAfBgNVHSMEGDAWgBTFh+bjavTljwqX\n"
    "FLaGMabflEO5hDAPBgNVHRMBAf8EBTADAQH/MBoGA1UdEQQTMBGCCWxvY2FsaG9z\n"
    "dIcEfwAAATANBgkqhkiG9w0BAQsFAAOCAQEAfC0B9CCaFGTfaAHWS3PPis4bc/Mr\n"
    "I95OtilO5nXr5xWizqjSw/pAddwlUOR9nqFcShJ1+geRf+UQISzl5+Y30QUUFT57\n"
    "B4cxO03cufsEI15Vp04R9XE7wcUoFxLdOf8fqD3TtKLAzGCijszBpmNseO5dGc6a\n"
    "170WGJv2ect9b+P/Qeei8INGvsqVNchcPUyukoc1omWpVqlHeWFW2lOAzJ7hNrd5\n"
    "ayIs7STzhQ5SNLqEBGSocOtnKr9lsLrDHPYVlWL0h8VGY2vNsG7WE4ddWnr+gIfp\n"
    "MP9Uv76nQvVhDzEOVAwXIQpwltkXCkhzXpbpTg2PkS1oIkP0uDzKrOGzhw==\n"
    "-----END CERTIFICATE-----\n";

static const char TEST_KEY_PEM[] =
    "-----BEGIN PRIVATE KEY-----\n"
    "MIIEvAIBADANBgkqhkiG9w0BAQEFAASCBKYwggSiAgEAAoIBAQCM75mtkldSVZYQ\n"
    "FFhX4iWDJVxlSpbwAbvgqC0l8ZdVfBTYwtMuWPqItVSxueh7r5QZLPVkg3PMv8pG\n"
    "wv1H+fHmFc2sAKAwLWKNVo+koyQfTcy7ESJ5n2a+6EbrXC7HJLBKeRfqnzH4MIOI\n"
    "dCOcypyFBPct01BhdGtAdNzPUgOAfEdqKc9ZrO6mkdt9aZvmV/lF8fVLsfDyWES8\n"
    "ssVXSms2W2M5EcMTlbB07a2VRuWazq/GJvKGnyHD6T6sE1FYksWj04pzRSv8qZco\n"
    "UPgcqQGRwrzenbQyy3Ai8ktbTemEcdMqnv+SrsiCvOE6Y9ygzSuMCSCXynbGkD5U\n"
    "sNSir/xHAgMBAAECggEAG6mlyN9Z+2OoJjTQGEPfqAurxIfs82PrvA9WeUWfthw6\n"
    "PUeUStsVJcMtwgqi+q/+Sii8a8OvdYdJjTUVoMyK8eXirjmbN4mY9/P/Esogu3I6\n"
    "Vpdo5GVotd0CmozH87ecUKnPC3K+zJ+9GYnuOYa9TRCp8ZkyBEblDExD0P+gh1XS\n"
    "3NelcN8JjvJSeZFzYlHS5KVRpZ60k3fOrSIxPAZ44x9K3Opus3kTqNufrp1twZl9\n"
    "sJC+BqbsSe5dW+VlVJex1uDbr+XxlJ89h7dxM/351lOU2+/gWWwPPA4+X5VZ9wSl\n"
    "yQdC+4O/qXVSYeaMDNjoTHlJjE76cG3FE/SeAJZoKQKBgQDBTGmforpsl4fYeqvk\n"
    "vBiSTbOZ0V742hkej/xc3NPtxO8WY6owX1BqNW5Ytkn842C2YETT3ZKYaIi+fdHi\n"
    "qdOYLAY0oAMOFsW3JflyLVXNfQjj+y0RwNdTf4Nsofl/twVwdDKM5Ez4NbyGHcyC\n"
    "phFFiLyaQXiV0j4ab+1LYyRArwKBgQC6pvrR4Tpc7F0Psy/uCZNZ/BcbMCAJsaXi\n"
    "9ZQkCJf3N5EErWrDz/zPrXTLApvnIkEzVuOXNFbHiArUdRbPLJT/i1nliYLyEvhX\n"
    "KK6nDUhmg8RIIFAzIaJDyCvYDO41mBNmpkhi+8iSBAD8aDUeqnFx5rqbswT5rHF5\n"
    "gR/MnCXz6QKBgGDguTdGyYRPVchLgwc6tl3tD4ySALVcKabFp0erbZQHVYS1IsDn\n"
    "pFf2u30+r0pNc1U0XoqIzEYSbiZ6zMx7LQ9hUSLi52USSLmIEMMGsbAxcFlCs83o\n"
    "BoNUrzus2m0F/3Xi5srySRlFZV0aZjs8m/9rjosJMTR1kKaTXiygXbUFAoGASPvl\n"
    "849YHOGnee4c/bzsyzDRcCYfQOsf7GEObhCWK2qk3+lXZ/254xL2KjN49qXkTMja\n"
    "8VLb4+WEJpMqOhQ6prm8iw58D1/vj1UvBc8h0kQPygwoj/XE1zA8RJy5wGcYnvjC\n"
    "MUI2qTG7mPa+XqFS/rCzoxIbDe2p6VLeNkmQZZECgYAGOwIKcgZR9ZZmyDBllpMf\n"
    "tggzqUTQSdigLV2lIxCf4Z18mpX6tFgUMN05wucjsGhxM+w2oVYaAf3UNTnMTyEz\n"
    "rqSnlJjrukar49E50f6d5xmos7sHXbaoKRu7uGR9Zb8QW5ruOATpGBRobXQ7IRCn\n"
    "aCNYRsdkIfbvl0tcLKIoSQ==\n"
    "-----END PRIVATE KEY-----\n";

// echoes the request body back verbatim, GET answers a fixed string of the
// same length so byte counting stays honest across encrypted boundaries
static void echo_body_fn(Http_Request *req, Http_Response *res, void *user_data)
{
    (void)user_data;
    http_response_set_status(res, HTTP_200_OK);
    http_response_set_header(res, "Content-Type", "application/octet-stream");
    if (req->method == HTTP_POST) {
        http_response_add_body(res, req->body);
    } else {
        http_response_add_body_cstr(res, "hello tls");
    }
}

static void client_ctx_init(SSL_CTX *ctx)
{
    // the cert is self-signed test material: skip verification intentionally
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
}

// opens a TLS connection to 127.0.0.1:port, writes `req` (req_len bytes,
// binary-safe: the gzip body carries NULs so strlen is out), reads until the
// server closes, and returns a heap copy of everything received (response
// head + body, so substring checks are enough)
static char *tls_roundtrip(int port, const char *req, size_t req_len)
{
    Socket_Handle s = net_connect("127.0.0.1", port);
    if (s == -1) {
        return NULL;
    }
    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    client_ctx_init(ctx);
    SSL *ssl = SSL_new(ctx);
    SSL_set_fd(ssl, (int)s);
    int cr = SSL_connect(ssl);
    if (cr != 1) {
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        net_close(s);
        return NULL;
    }
    size_t cap = req_len + 16384;
    char *buf = malloc(cap);
    size_t got = 0;
    if (SSL_write(ssl, req, (int)req_len) > 0) {
        long n;
        while (got < cap - 1 && (n = SSL_read(ssl, buf + got, cap - got - 1)) > 0) {
            got += (size_t)n;
        }
    }
    buf[got] = '\0';
    SSL_free(ssl);
    SSL_CTX_free(ctx);
    net_close(s);
    return buf;
}

// how many times needle appears in haystack
static int substr_count(const char *haystack, const char *needle)
{
    int count = 0;
    const char *at = haystack;
    size_t n = strlen(needle);
    while ((at = strstr(at, needle)) != NULL) {
        count++;
        at += n;
    }
    return count;
}

// ---- minimal HTTP/2-over-TLS client -----------------------------------
// over TLS there is no 24-octet client preface (RFC 7540 section 3.5): the
// connection starts with the SETTINGS frame. this client negotiates ALPN on
// the handshake, sends its SETTINGS, hpack-encodes one GET and reads back
// the response headers + body, ACKing the server's settings along the way.

enum {
    TH2_FRAME_HDR = 9,
    TH2_DATA = 0x0,
    TH2_HEADERS = 0x1,
    TH2_SETTINGS = 0x4,
    TH2_F_ACK = 0x1,
    TH2_F_END_STREAM = 0x1,
    TH2_F_END_HEADERS = 0x4,
};

typedef struct {
    Socket_Handle fd;
    SSL *ssl;
    Hpack hp;
    Strbuf in;
    uint8_t staging[8192];
} Th2;

// advertises a single ALPN protocol on the client side
static int th2_set_alpn(SSL *ssl, const char *proto)
{
    unsigned char wire[32];
    size_t n = strlen(proto);
    if (n == 0 || n > 31) {
        return -1;
    }
    wire[0] = (unsigned char)n;
    memcpy(wire + 1, proto, n);
    return SSL_set_alpn_protos(ssl, wire, (unsigned int)n + 1);
}

static int th2_open(Th2 *c, int port, const char *alpn)
{
    memset(c, 0, sizeof *c);
    c->fd = net_connect("127.0.0.1", port);
    if (c->fd == -1) {
        return -1;
    }
    net_set_timeout(c->fd, 15000);
    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    client_ctx_init(ctx);
    c->ssl = SSL_new(ctx);
    if (c->ssl == NULL) {
        SSL_CTX_free(ctx);
        net_close(c->fd);
        return -1;
    }
    if (alpn != NULL && th2_set_alpn(c->ssl, alpn) != 0) {
        SSL_free(c->ssl);
        SSL_CTX_free(ctx);
        net_close(c->fd);
        return -1;
    }
    SSL_set_fd(c->ssl, (int)c->fd);
    if (SSL_connect(c->ssl) != 1) {
        SSL_free(c->ssl);
        SSL_CTX_free(ctx);
        net_close(c->fd);
        return -1;
    }
    SSL_CTX_free(ctx); // SSL_new keeps a reference, so this is safe here
    hpack_init(&c->hp);
    strbuf_init(&c->in);
    return 0;
}

static void th2_close(Th2 *c)
{
    SSL_free(c->ssl);
    hpack_free(&c->hp);
    strbuf_free(&c->in);
    net_close(c->fd);
}

static int th2_write_frame(Th2 *c, uint8_t type, uint8_t flags, uint32_t sid,
                           const void *payload, size_t len)
{
    uint8_t h[TH2_FRAME_HDR];
    h[0] = (uint8_t)(len >> 16);
    h[1] = (uint8_t)(len >> 8);
    h[2] = (uint8_t)len;
    h[3] = type;
    h[4] = flags;
    h[5] = (uint8_t)((sid >> 24) & 0x7f);
    h[6] = (uint8_t)(sid >> 16);
    h[7] = (uint8_t)(sid >> 8);
    h[8] = (uint8_t)sid;
    if (SSL_write(c->ssl, h, sizeof h) != (int)sizeof h) {
        return -1;
    }
    if (len > 0 && SSL_write(c->ssl, payload, (int)len) != (int)len) {
        return -1;
    }
    return 0;
}

// 1 on bytes, 0 on orderly close, -1 on error
static int th2_read_into(Th2 *c)
{
    char tmp[8192];
    int n = SSL_read(c->ssl, tmp, sizeof tmp);
    if (n > 0) {
        strbuf_append(&c->in, tmp, (size_t)n);
        return 1;
    }
    return n == 0 ? 0 : -1;
}

static int th2_ensure(Th2 *c, size_t need)
{
    while (c->in.count < need) {
        int r = th2_read_into(c);
        if (r <= 0) {
            return r;
        }
    }
    return 1;
}

// like hc_next_frame: the server's SETTINGS are ACKed and skipped, every
// other frame is handed to the caller (payload valid until the next frame)
static int th2_next_frame(Th2 *c, uint8_t *type, uint8_t *flags, uint32_t *sid,
                          const uint8_t **payload, size_t *len)
{
    for (;;) {
        if (th2_ensure(c, TH2_FRAME_HDR) <= 0) {
            return -1;
        }
        const uint8_t *h = (const uint8_t *)c->in.items;
        size_t flen = ((size_t)h[0] << 16) | ((size_t)h[1] << 8) | h[2];
        if (th2_ensure(c, TH2_FRAME_HDR + flen) <= 0) {
            return -1;
        }
        memcpy(c->staging, c->in.items + TH2_FRAME_HDR, flen);
        memmove(c->in.items, c->in.items + TH2_FRAME_HDR + flen,
                c->in.count - TH2_FRAME_HDR - flen);
        c->in.count -= TH2_FRAME_HDR + flen;
        uint8_t t = h[3];
        uint8_t f = h[4];
        uint32_t s = ((uint32_t)(h[5] & 0x7f) << 24) | ((uint32_t)h[6] << 16) |
                     ((uint32_t)h[7] << 8) | h[8];
        if (t == TH2_SETTINGS && !(f & TH2_F_ACK)) {
            th2_write_frame(c, TH2_SETTINGS, TH2_F_ACK, 0, NULL, 0);
            continue;
        }
        *type = t;
        *flags = f;
        *sid = s;
        *payload = c->staging;
        *len = flen;
        return 1;
    }
}

// reads one complete response on sid; *status is the :status value, the body
// accumulates across DATA frames. 0 on success, -1 on framing trouble.
static int th2_read_response(Th2 *c, uint32_t sid, int *status, Strbuf *body)
{
    Strbuf hb;
    size_t body_cap = 0;
    strbuf_init(&hb);
    for (;;) {
        uint8_t type, flags;
        uint32_t f_sid;
        const uint8_t *pl;
        size_t plen;
        int r = th2_next_frame(c, &type, &flags, &f_sid, &pl, &plen);
        if (r <= 0) {
            strbuf_free(&hb);
            return -1;
        }
        if (f_sid != sid) {
            continue;
        }
        if (type == TH2_HEADERS) {
            strbuf_append(&hb, (const char *)pl, plen);
            if (!(flags & TH2_F_END_HEADERS)) {
                continue;
            }
            Hpack_Decoded dec = { 0 };
            if (hpack_decode_block(&c->hp, (const uint8_t *)hb.items,
                                   hb.count, &dec) < 0) {
                strbuf_free(&hb);
                return -1;
            }
            for (size_t i = 0; i < dec.count; i++) {
                if (sv_equal(dec.items[i].name, sv_from_cstr(":status")) &&
                    *status == 0) {
                    char tmp[16];
                    size_t n = dec.items[i].value.count < sizeof tmp - 1
                                   ? dec.items[i].value.count
                                   : sizeof tmp - 1;
                    memcpy(tmp, dec.items[i].value.data, n);
                    tmp[n] = '\0';
                    *status = atoi(tmp);
                }
            }
            hpack_decoded_free(&dec);
            if (flags & TH2_F_END_STREAM) {
                strbuf_free(&hb);
                return 0;
            }
            continue;
        }
        if (type == TH2_DATA) {
            if (body_cap + plen > 256u * 1024u) {
                strbuf_free(&hb);
                return -1;
            }
            body_cap += plen;
            strbuf_append(body, (const char *)pl, plen);
            if (flags & TH2_F_END_STREAM) {
                strbuf_free(&hb);
                return 0;
            }
            continue;
        }
    }
}

// h2 transport + ALPN via the same TLS listener: an h2 client is served
// HTTP/2 over TLS, an http/1.1 client still gets HTTP/1.1
static int test_h2_alpn(int port)
{
    Th2 c;
    if (th2_open(&c, port, "h2") != 0) {
        fprintf(stderr, "TH2: h2 alpn connect failed\n");
        return 0;
    }
    const unsigned char *neg = NULL;
    unsigned int neglen = 0;
    SSL_get0_alpn_selected(c.ssl, &neg, &neglen);
    int ok = neglen == 2 && neg[0] == 'h' && neg[1] == '2';
    if (!ok) {
        fprintf(stderr, "TH2: ALPN did not negotiate h2\n");
        th2_close(&c);
        return 0;
    }

    // client preface over TLS is just the settings frame
    ok = th2_write_frame(&c, TH2_SETTINGS, 0, 0, NULL, 0) == 0;
    Strbuf blk;
    strbuf_init(&blk);
    hpack_encode_field(&blk, sv_from_cstr(":method"), sv_from_cstr("GET"));
    hpack_encode_field(&blk, sv_from_cstr(":scheme"), sv_from_cstr("https"));
    hpack_encode_field(&blk, sv_from_cstr(":path"), sv_from_cstr("/h2"));
    hpack_encode_field(&blk, sv_from_cstr(":authority"),
                       sv_from_cstr("localhost"));
    ok = ok &&
         th2_write_frame(&c, TH2_HEADERS, TH2_F_END_HEADERS | TH2_F_END_STREAM,
                         1, blk.items, blk.count) == 0;
    strbuf_free(&blk);

    int status = 0;
    Strbuf body;
    strbuf_init(&body);
    int r = th2_read_response(&c, 1, &status, &body);
    ok = ok && r == 0 && status == 200 &&
         body.count == strlen("hello tls") &&
         memcmp(body.items, "hello tls", body.count) == 0;
    if (!ok) {
        fprintf(stderr, "TH2: response status=%d body=%.*s read=%d\n", status,
                (int)body.count, body.count ? body.items : "", r);
    }
    strbuf_free(&body);
    th2_close(&c);

    if (!ok) {
        return 0;
    }

    // a client that offers only http/1.1 must still get HTTP/1.1 served
    if (th2_open(&c, port, "http/1.1") != 0) {
        fprintf(stderr, "TH2: http/1.1 alpn connect failed\n");
        return 0;
    }
    SSL_get0_alpn_selected(c.ssl, &neg, &neglen);
    ok = neglen == 8 && memcmp(neg, "http/1.1", 8) == 0;
    char buf[4096];
    size_t got = 0;
    if (ok) {
        const char *req = "GET /plain HTTP/1.1\r\nHost: tls.test\r\n"
                          "Connection: close\r\n\r\n";
        if (SSL_write(c.ssl, req, (int)strlen(req)) > 0) {
            long n;
            while (got < sizeof buf - 1 &&
                   (n = SSL_read(c.ssl, buf + got,
                                 (int)(sizeof buf - 1 - got))) > 0) {
                got += (size_t)n;
            }
        }
        buf[got] = '\0';
        ok = strstr(buf, "HTTP/1.1 200 OK") != NULL &&
             strstr(buf, "hello tls") != NULL;
    }
    if (!ok) {
        fprintf(stderr, "TH2: http/1.1 alpn request failed: %s\n",
                ok && got == 0 ? "" : buf);
    }
    th2_close(&c);
    return ok;
}

int main(void)
{
    if (!tls_available()) {
        fprintf(stderr, "TLS transport not compiled in\n");
        return 1;
    }

    char cert_path[128];
    char key_path[128];
    snprintf(cert_path, sizeof cert_path, "/tmp/cweb-tls-%d-cert.pem", (int)getpid());
    snprintf(key_path, sizeof key_path, "/tmp/cweb-tls-%d-key.pem", (int)getpid());
    FILE *f = fopen(cert_path, "w");
    if (f == NULL || fwrite(TEST_CERT_PEM, 1, sizeof TEST_CERT_PEM - 1, f) != sizeof TEST_CERT_PEM - 1) {
        fprintf(stderr, "cannot write test cert\n");
        return 1;
    }
    fclose(f);
    f = fopen(key_path, "w");
    if (f == NULL || fwrite(TEST_KEY_PEM, 1, sizeof TEST_KEY_PEM - 1, f) != sizeof TEST_KEY_PEM - 1) {
        fprintf(stderr, "cannot write test key\n");
        return 1;
    }
    fclose(f);

    Socket_Handle srv = net_listen(0);
    if (srv == -1) {
        fprintf(stderr, "listen failed: %s\n", net_error_string());
        return 1;
    }
    int port = net_bound_port(srv);
    Http_Server_Config cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.tls_cert = cert_path;
    cfg.tls_key = key_path;
    cfg.io_timeout_ms = 2000;
    pid_t child = fork();
    if (child == 0) {
        log_set_level(LOG_WARN);
        http_serve_config(srv, echo_body_fn, NULL, &cfg);
        _exit(0);
    }

    int ok = 1;

    // 1: a plain GET over the encrypted listener answers 200
    char *resp = tls_roundtrip(port,
                               "GET /hello HTTP/1.1\r\n"
                               "Host: tls.test\r\n"
                               "Connection: close\r\n"
                               "\r\n",
                               strlen("GET /hello HTTP/1.1\r\n"
                                      "Host: tls.test\r\n"
                                      "Connection: close\r\n"
                                      "\r\n"));
    ok = resp != NULL && strstr(resp, "HTTP/1.1 200 OK") != NULL &&
         strstr(resp, "hello tls") != NULL;
    free(resp);

    // 2: two keep-alive GETs on one TLS connection both answer
    const char *piped =
        "GET /one HTTP/1.1\r\nHost: tls.test\r\n\r\n"
        "GET /two HTTP/1.1\r\nHost: tls.test\r\n"
        "Connection: close\r\n\r\n";
    resp = tls_roundtrip(port, piped, strlen(piped));
    ok = resp != NULL && substr_count(resp, "HTTP/1.1 200 OK") == 2;
    free(resp);

    // 3: a gzip-compressed POST is inflated server-side and echoed decoded
    Strbuf gz;
    strbuf_init(&gz);
    const char *plain = "hello gzip over tls";
    if (http_gzip_compress(plain, strlen(plain), &gz) == 0) {
        char head[256];
        int hl = snprintf(head, sizeof head,
                          "POST /gz HTTP/1.1\r\n"
                          "Host: tls.test\r\n"
                          "Content-Encoding: gzip\r\n"
                          "Content-Length: %zu\r\n"
                          "Connection: close\r\n"
                          "\r\n", gz.count);
        size_t req_len = (size_t)hl + gz.count;
        char *req = malloc(req_len + 1);
        memcpy(req, head, (size_t)hl);
        memcpy(req + hl, gz.items, gz.count);
        req[req_len] = '\0';
        resp = tls_roundtrip(port, req, req_len);
        free(req);
        ok = ok && resp != NULL &&
             strstr(resp, "HTTP/1.1 200 OK") != NULL &&
             strstr(resp, plain) != NULL;
        free(resp);
    } else {
        ok = 0;
    }
    strbuf_free(&gz);

    // 4: a plaintext client against the TLS listener is turned away, and the
    // server survives to serve the next TLS client
    Socket_Handle raw = net_connect("127.0.0.1", port);
    if (raw != -1) {
        net_send_all(raw, "GET / HTTP/1.1\r\nHost: x\r\n\r\n", 26);
        char tmp[64];
        long n = net_recv(raw, tmp, sizeof tmp);
        net_close(raw);
        if (n > 0) {
            // the HTTP reply to a speak-TLS-or-go-away listener is itself
            // the handshake failure; a working TLS client proves it
            fprintf(stderr, "plaintext client got bytes back from TLS listener\n");
            ok = 0;
        }
    }
    resp = tls_roundtrip(port,
                         "GET /after HTTP/1.1\r\n"
                         "Host: tls.test\r\n"
                         "Connection: close\r\n"
                         "\r\n",
                         strlen("GET /after HTTP/1.1\r\n"
                                "Host: tls.test\r\n"
                                "Connection: close\r\n"
                                "\r\n"));
    ok = ok && resp != NULL && strstr(resp, "HTTP/1.1 200 OK") != NULL;
    free(resp);

    // 5: ALPN on the same listener: "h2" is served as HTTP/2 over TLS and
    // "http/1.1" is still served as HTTP/1.1
    ok = ok && test_h2_alpn(port);

    kill(child, SIGTERM);
    waitpid(child, NULL, 0);
    net_close(srv);
    unlink(cert_path);
    unlink(key_path);

    if (!ok) {
        char errbuf[1024];
        ERR_error_string_n(ERR_get_error(), errbuf, sizeof errbuf);
        fprintf(stderr, "TLS test failed: %s\n", errbuf);
        return 1;
    }

    printf("tls ok\n");
    return 0;
}