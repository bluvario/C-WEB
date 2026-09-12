#define _POSIX_C_SOURCE 200809L

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include "h2.h"
#include "hpack.h"
#include "log.h"
#include "net.h"
#include "server.h"
#include "strbuf.h"
#include "sv.h"

static int fails = 0;
static void check(const char *what, int cond)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        fails++;
    }
}

static const char *contains(const char *hay, size_t hay_len, const char *needle)
{
    size_t n = strlen(needle);
    if (hay_len < n) {
        return NULL;
    }
    for (size_t i = 0; i + n <= hay_len; i++) {
        if (memcmp(hay + i, needle, n) == 0) {
            return hay + i;
        }
    }
    return NULL;
}

// ---- framing constants (RFC 7540) -------------------------------------
enum {
    T_DATA = 0x0, T_HEADERS = 0x1, T_RST = 0x3, T_SETTINGS = 0x4,
    T_CONT = 0x9,
};
enum {
    F_END_STREAM = 0x1, F_ACK = 0x1, F_END_HEADERS = 0x4,
};
#define FRAME_HDR 9u

// ---- test handlers ----------------------------------------------------

typedef struct {
    size_t idx;
    const char *const *parts;
    size_t nparts;
} Ticker;

static const char *tick_parts[] = { "alpha-", "beta-", "gamma" };
static Ticker stream_tick = { 0, tick_parts, 3 };

static size_t ticker_fn(void *buf, size_t cap, void *user_data)
{
    Ticker *t = user_data;
    if (t->idx >= t->nparts) {
        return 0;
    }
    const char *part = t->parts[t->idx++];
    size_t n = strlen(part);
    if (n > cap) {
        n = cap;
    }
    memcpy(buf, part, n);
    return n;
}

static void handler_fn(Http_Request *req, Http_Response *res, void *user_data)
{
    (void)user_data;
    http_response_set_status(res, HTTP_200_OK);
    if (req->path.count >= 4 && memcmp(req->path.data, "/err", 4) == 0) {
        http_response_set_status(res, HTTP_404_NOT_FOUND);
        http_response_add_body_cstr(res, "nope");
        return;
    }
    if (req->method == HTTP_POST) {
        http_response_set_header(res, "Content-Type",
                                 "application/octet-stream");
        http_response_add_body(res, req->body);
        return;
    }
    if (sv_equal(req->path, sv_from_cstr("/stream"))) {
        http_response_set_header(res, "Content-Type", "text/event-stream");
        stream_tick.idx = 0;
        http_response_set_stream(res, ticker_fn, &stream_tick);
        return;
    }
    http_response_set_header(res, "Content-Type", "text/plain; charset=utf-8");
    Strbuf body;
    strbuf_init(&body);
    strbuf_append_cstr(&body, "Hello, ");
    strbuf_append(&body, req->path.data, req->path.count);
    strbuf_append_cstr(&body, "!");
    http_response_add_body(res, (String_View){ body.items, body.count });
    strbuf_free(&body);
}

// spawn a forked server on an ephemeral port; returns the child pid
static pid_t spawn_server(int *port_out, const Http_Server_Config *cfg)
{
    Socket_Handle srv = net_listen(0);
    if (srv == -1) {
        fprintf(stderr, "listen failed: %s\n", net_error_string());
        return -1;
    }
    *port_out = net_bound_port(srv);
    pid_t child = fork();
    if (child == 0) {
        signal(SIGPIPE, SIG_IGN);
        log_set_level(LOG_WARN);
        if (cfg != NULL) {
            http_serve_config(srv, handler_fn, NULL, cfg);
        } else {
            http_serve(srv, handler_fn, NULL);
        }
        _exit(0);
    }
    net_close(srv);
    return child;
}

static void stop_server(pid_t pid)
{
    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);
}

// ---- minimal h2c client ----------------------------------------------

typedef struct {
    Socket_Handle fd;
    Hpack hp;
    Strbuf in;
    uint8_t staging[16384];
} H2Client;

static int hc_open(H2Client *c, int port)
{
    memset(c, 0, sizeof *c);
    c->fd = net_connect("127.0.0.1", port);
    if (c->fd == -1) {
        return -1;
    }
    net_set_timeout(c->fd, 15000);
    hpack_init(&c->hp);
    strbuf_init(&c->in);
    return 0;
}

static void hc_close(H2Client *c)
{
    net_close(c->fd);
    hpack_free(&c->hp);
    strbuf_free(&c->in);
}

static int hc_write_frame(H2Client *c, uint8_t type, uint8_t flags,
                          uint32_t sid, const void *payload, size_t len)
{
    uint8_t h[FRAME_HDR];
    h[0] = (uint8_t)(len >> 16);
    h[1] = (uint8_t)(len >> 8);
    h[2] = (uint8_t)len;
    h[3] = type;
    h[4] = flags;
    h[5] = (uint8_t)((sid >> 24) & 0x7f);
    h[6] = (uint8_t)(sid >> 16);
    h[7] = (uint8_t)(sid >> 8);
    h[8] = (uint8_t)sid;
    if (net_send_all(c->fd, h, sizeof h) != (long)sizeof h) {
        return -1;
    }
    if (len > 0 && net_send_all(c->fd, payload, len) != (long)len) {
        return -1;
    }
    return 0;
}

// pulls the next chunk of socket bytes into the buffer.
// returns 1 on bytes, 0 on orderly EOF, -1 on error/timeout
static int hc_read_into(H2Client *c)
{
    char tmp[8192];
    long n = net_recv(c->fd, tmp, sizeof tmp);
    if (n == NET_READ_TIMEOUT) {
        return -1;
    }
    if (n == 0) {
        return 0;
    }
    if (n < 0) {
        return -1;
    }
    strbuf_append(&c->in, tmp, (size_t)n);
    return 1;
}

static int hc_ensure(H2Client *c, size_t need)
{
    while (c->in.count < need) {
        int r = hc_read_into(c);
        if (r <= 0) {
            return r;
        }
    }
    return 1;
}

static int hc_search(H2Client *c, const char *needle, size_t *pos)
{
    size_t n = strlen(needle);
    if (c->in.count < n) {
        return -1;
    }
    for (size_t i = 0; i + n <= c->in.count; i++) {
        if (memcmp(c->in.items + i, needle, n) == 0) {
            *pos = i;
            return 0;
        }
    }
    return -1;
}

// consumes bytes of the read buffer up to and including needle
static int hc_consume_through(H2Client *c, const char *needle)
{
    size_t pos;
    for (;;) {
        if (hc_search(c, needle, &pos) == 0) {
            size_t end = pos + strlen(needle);
            memmove(c->in.items, c->in.items + end, c->in.count - end);
            c->in.count -= end;
            return 0;
        }
        int r = hc_read_into(c);
        if (r <= 0) {
            return -1;
        }
    }
}

static uint32_t hc_rd32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

// returns 1 with the frame filled (payload valid until the next frame read),
// 0 on EOF, -1 on error. SETTINGS and PING frames are handled internally:
// the server's settings are ACKed and all settings/pings are skipped.
static int hc_next_frame(H2Client *c, uint8_t *type, uint8_t *flags,
                         uint32_t *sid, const uint8_t **payload, size_t *len)
{
    for (;;) {
        int r = hc_ensure(c, FRAME_HDR);
        if (r <= 0) {
            return r;
        }
        const uint8_t *h = (const uint8_t *)c->in.items;
        size_t flen = ((size_t)h[0] << 16) | ((size_t)h[1] << 8) | h[2];
        uint8_t t = h[3];
        uint8_t f = h[4];
        uint32_t s = ((uint32_t)(h[5] & 0x7f) << 24) | ((uint32_t)h[6] << 16) |
                     ((uint32_t)h[7] << 8) | h[8];
        if (hc_ensure(c, FRAME_HDR + flen) <= 0) {
            return -1;
        }
        // copy the payload out before the buffer is compacted below, since
        // the caller only reads it after this function returns
        memcpy(c->staging, c->in.items + FRAME_HDR, flen);
        memmove(c->in.items, c->in.items + FRAME_HDR + flen,
                c->in.count - FRAME_HDR - flen);
        c->in.count -= FRAME_HDR + flen;
        if (t == T_SETTINGS) {
            if (!(f & F_ACK)) {
                hc_write_frame(c, T_SETTINGS, F_ACK, 0, NULL, 0);
            }
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

// builds a request header block; extra_name/value (may be NULL) appends a
// regular header field, useful for malformed-name tests
static void hc_make_request(Strbuf *blk, const char *method, const char *path,
                            const char *authority, const char *extra_name,
                            const char *extra_value)
{
    strbuf_init(blk);
    hpack_encode_field(blk, sv_from_cstr(":method"), sv_from_cstr(method));
    hpack_encode_field(blk, sv_from_cstr(":scheme"), sv_from_cstr("http"));
    hpack_encode_field(blk, sv_from_cstr(":path"), sv_from_cstr(path));
    hpack_encode_field(blk, sv_from_cstr(":authority"),
                       sv_from_cstr(authority));
    if (extra_name != NULL) {
        hpack_encode_field(blk, sv_from_cstr(extra_name),
                           sv_from_cstr(extra_value));
    }
}

static int hc_send_request(H2Client *c, uint32_t sid, const Strbuf *blk,
                           bool end_stream)
{
    uint8_t flags = F_END_HEADERS | (end_stream ? F_END_STREAM : 0);
    return hc_write_frame(c, T_HEADERS, flags, sid, blk->items, blk->count);
}

static int hc_send_preface(H2Client *c)
{
    if (net_send_all(c->fd, H2_MAGIC, H2_MAGIC_LEN) != (long)H2_MAGIC_LEN) {
        return -1;
    }
    return hc_write_frame(c, T_SETTINGS, 0, 0, NULL, 0);
}

// reads one complete response on sid. on success fills *status, appends the
// entity body to body and the decoded headers ("name: value\n") to headers,
// returning 0. when the stream is reset, *rst_code is filled and -1 returns.
static int hc_read_response(H2Client *c, uint32_t sid, int *status,
                            Strbuf *body, Strbuf *headers, uint32_t *rst_code)
{
    Strbuf hb; // accumulated header-block fragments
    size_t body_cap = 0;
    strbuf_init(&hb);
    if (headers != NULL) {
        strbuf_init(headers);
    }
    for (;;) {
        uint8_t type, flags;
        uint32_t f_sid;
        const uint8_t *pl;
        size_t plen;
        int r = hc_next_frame(c, &type, &flags, &f_sid, &pl, &plen);
        if (r <= 0) {
            strbuf_free(&hb);
            return -1;
        }
        if (f_sid != sid) {
            continue;
        }
        if (type == T_HEADERS || type == T_CONT) {
            strbuf_append(&hb, (const char *)pl, plen);
            if (!(flags & F_END_HEADERS)) {
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
                    size_t n = dec.items[i].value.count <
                                       sizeof tmp - 1
                                   ? dec.items[i].value.count
                                   : sizeof tmp - 1;
                    memcpy(tmp, dec.items[i].value.data, n);
                    tmp[n] = '\0';
                    *status = atoi(tmp);
                }
                if (headers != NULL) {
                    strbuf_append(headers, dec.items[i].name.data,
                                  dec.items[i].name.count);
                    strbuf_append_cstr(headers, ": ");
                    strbuf_append(headers, dec.items[i].value.data,
                                  dec.items[i].value.count);
                    strbuf_append_char(headers, '\n');
                }
            }
            hpack_decoded_free(&dec);
            if (flags & F_END_STREAM) {
                strbuf_free(&hb);
                return 0;
            }
            continue;
        }
        if (type == T_DATA) {
            if (body_cap + plen > (256u * 1024u)) {
                strbuf_free(&hb);
                return -1;
            }
            body_cap += plen;
            strbuf_append(body, (const char *)pl, plen);
            if (flags & F_END_STREAM) {
                strbuf_free(&hb);
                return 0;
            }
            continue;
        }
        if (type == T_RST) {
            if (rst_code != NULL) {
                *rst_code = hc_rd32(pl);
            }
            strbuf_free(&hb);
            return -1;
        }
    }
}

// ---- tests -------------------------------------------------------------

static void test_prior_knowledge_get(void)
{
    int port;
    pid_t pid = spawn_server(&port, NULL);
    if (pid == -1) {
        return;
    }
    H2Client c;
    if (hc_open(&c, port) != 0) {
        stop_server(pid);
        check("prior-knowledge connect", 0);
        return;
    }
    check("prior-knowledge connect", 1);
    check("magic+settings send", hc_send_preface(&c) == 0);
    Strbuf blk;
    hc_make_request(&blk, "GET", "/hello", "127.0.0.1", NULL, NULL);
    check("request send", hc_send_request(&c, 1, &blk, true) == 0);
    strbuf_free(&blk);

    int status = 0;
    Strbuf body, headers;
    strbuf_init(&body);
    int r = hc_read_response(&c, 1, &status, &body, &headers, NULL);
    check("prior-knowledge GET status 200",
          r == 0 && status == 200);
    check("prior-knowledge GET body",
          body.count == strlen("Hello, /hello!") &&
          memcmp(body.items, "Hello, /hello!", body.count) == 0);
    check("response carries content-type",
          headers.count > 0 &&
          contains(headers.items, headers.count,
                   "content-type: text/plain") != NULL);
    strbuf_free(&headers);
    strbuf_free(&body);
    hc_close(&c);
    stop_server(pid);
}

static void test_multi_stream(void)
{
    int port;
    pid_t pid = spawn_server(&port, NULL);
    if (pid == -1) {
        return;
    }
    H2Client c;
    if (hc_open(&c, port) != 0) {
        stop_server(pid);
        check("multi-stream connect", 0);
        return;
    }
    check("multi-stream connect", 1);
    check("magic+settings send", hc_send_preface(&c) == 0);
    Strbuf one, two;
    hc_make_request(&one, "GET", "/one", "127.0.0.1", NULL, NULL);
    hc_make_request(&two, "GET", "/two", "127.0.0.1", NULL, NULL);
    check("stream 1 request", hc_send_request(&c, 1, &one, true) == 0);
    check("stream 3 request", hc_send_request(&c, 3, &two, true) == 0);
    strbuf_free(&one);
    strbuf_free(&two);

    int s1 = 0, s3 = 0;
    Strbuf b1, b3;
    strbuf_init(&b1);
    strbuf_init(&b3);
    int r1 = hc_read_response(&c, 1, &s1, &b1, NULL, NULL);
    int r3 = hc_read_response(&c, 3, &s3, &b3, NULL, NULL);
    check("both streams answered",
          r1 == 0 && r3 == 0 && s1 == 200 && s3 == 200);
    check("stream 1 body",
          b1.count == strlen("Hello, /one!") &&
          memcmp(b1.items, "Hello, /one!", b1.count) == 0);
    check("stream 3 body",
          b3.count == strlen("Hello, /two!") &&
          memcmp(b3.items, "Hello, /two!", b3.count) == 0);
    strbuf_free(&b1);
    strbuf_free(&b3);
    hc_close(&c);
    stop_server(pid);
}

static void test_post_echo(void)
{
    int port;
    pid_t pid = spawn_server(&port, NULL);
    if (pid == -1) {
        return;
    }
    H2Client c;
    if (hc_open(&c, port) != 0) {
        stop_server(pid);
        check("post-echo connect", 0);
        return;
    }
    check("post-echo connect", 1);
    check("magic+settings send", hc_send_preface(&c) == 0);
    Strbuf blk;
    hc_make_request(&blk, "POST", "/echo", "127.0.0.1", NULL, NULL);
    check("headers send", hc_send_request(&c, 1, &blk, false) == 0);
    strbuf_free(&blk);
    check("data send",
          hc_write_frame(&c, T_DATA, F_END_STREAM, 1, "payload-xyz",
                         strlen("payload-xyz")) == 0);

    int status = 0;
    Strbuf body;
    strbuf_init(&body);
    int r = hc_read_response(&c, 1, &status, &body, NULL, NULL);
    check("post echo status 200", r == 0 && status == 200);
    check("post body echoed",
          body.count == strlen("payload-xyz") &&
          memcmp(body.items, "payload-xyz", body.count) == 0);
    strbuf_free(&body);
    hc_close(&c);
    stop_server(pid);
}

static void test_streaming(void)
{
    int port;
    pid_t pid = spawn_server(&port, NULL);
    if (pid == -1) {
        return;
    }
    H2Client c;
    if (hc_open(&c, port) != 0) {
        stop_server(pid);
        check("streaming connect", 0);
        return;
    }
    check("streaming connect", 1);
    check("magic+settings send", hc_send_preface(&c) == 0);
    Strbuf blk;
    hc_make_request(&blk, "GET", "/stream", "127.0.0.1", NULL, NULL);
    check("request send", hc_send_request(&c, 1, &blk, true) == 0);
    strbuf_free(&blk);

    int status = 0;
    Strbuf body;
    strbuf_init(&body);
    int r = hc_read_response(&c, 1, &status, &body, NULL, NULL);
    const char want[] = "alpha-beta-gamma";
    check("stream status 200", r == 0 && status == 200);
    check("streamed body assembled",
          body.count == strlen(want) &&
          memcmp(body.items, want, body.count) == 0);
    strbuf_free(&body);
    hc_close(&c);
    stop_server(pid);
}

static void test_not_found(void)
{
    int port;
    pid_t pid = spawn_server(&port, NULL);
    if (pid == -1) {
        return;
    }
    H2Client c;
    if (hc_open(&c, port) != 0) {
        stop_server(pid);
        check("not-found connect", 0);
        return;
    }
    check("not-found connect", 1);
    check("magic+settings send", hc_send_preface(&c) == 0);
    Strbuf blk;
    hc_make_request(&blk, "GET", "/err/any", "127.0.0.1", NULL, NULL);
    check("request send", hc_send_request(&c, 1, &blk, true) == 0);
    strbuf_free(&blk);

    int status = 0;
    Strbuf body;
    strbuf_init(&body);
    int r = hc_read_response(&c, 1, &status, &body, NULL, NULL);
    check("404 status", r == 0 && status == 404);
    check("404 body", body.count == strlen("nope") &&
                          memcmp(body.items, "nope", body.count) == 0);
    strbuf_free(&body);
    hc_close(&c);
    stop_server(pid);
}

static void test_body_too_large(void)
{
    Http_Server_Config cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.max_body = 1024;
    int port;
    pid_t pid = spawn_server(&port, &cfg);
    if (pid == -1) {
        return;
    }
    H2Client c;
    if (hc_open(&c, port) != 0) {
        stop_server(pid);
        check("413 connect", 0);
        return;
    }
    check("413 connect", 1);
    check("magic+settings send", hc_send_preface(&c) == 0);
    Strbuf blk;
    hc_make_request(&blk, "POST", "/big", "127.0.0.1", NULL, NULL);
    check("headers send", hc_send_request(&c, 1, &blk, false) == 0);
    strbuf_free(&blk);
    char big[4096];
    memset(big, 'x', sizeof big);
    check("data send",
          hc_write_frame(&c, T_DATA, F_END_STREAM, 1, big, sizeof big) == 0);

    int status = 0;
    Strbuf body;
    strbuf_init(&body);
    int r = hc_read_response(&c, 1, &status, &body, NULL, NULL);
    check("413 status", r == 0 && status == 413);
    check("413 body present", body.count > 0);
    strbuf_free(&body);
    hc_close(&c);
    stop_server(pid);
}

static void test_malformed_request_rst(void)
{
    int port;
    pid_t pid = spawn_server(&port, NULL);
    if (pid == -1) {
        return;
    }
    H2Client c;
    if (hc_open(&c, port) != 0) {
        stop_server(pid);
        check("malformed connect", 0);
        return;
    }
    check("malformed connect", 1);
    check("magic+settings send", hc_send_preface(&c) == 0);
    Strbuf blk;
    hc_make_request(&blk, "GET", "/x", "127.0.0.1", "X-Evil", "1");
    check("request send", hc_send_request(&c, 1, &blk, true) == 0);
    strbuf_free(&blk);

    int status = 0;
    Strbuf body;
    strbuf_init(&body);
    uint32_t rst = 0;
    int r = hc_read_response(&c, 1, &status, &body, NULL, &rst);
    check("malformed request reset", r == -1 && rst == 0x1);
    strbuf_free(&body);
    hc_close(&c);
    stop_server(pid);
}

static void test_h2c_upgrade(void)
{
    int port;
    pid_t pid = spawn_server(&port, NULL);
    if (pid == -1) {
        return;
    }
    H2Client c;
    if (hc_open(&c, port) != 0) {
        stop_server(pid);
        check("upgrade connect", 0);
        return;
    }
    check("upgrade connect", 1);
    const char *req =
        "GET /up HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Connection: Upgrade, HTTP2-Settings\r\n"
        "Upgrade: h2c\r\n"
        "HTTP2-Settings: AAMAAABk\r\n"
        "Connection: keep-alive\r\n"
        "\r\n";
    check("upgrade request sent",
          net_send_all(c.fd, req, strlen(req)) == (long)strlen(req));

    // the server answers the upgrade with a 101 + its settings preface
    check("101 received", hc_consume_through(&c, "\r\n\r\n") == 0);

    // client preface for an upgraded connection is just the settings frame
    check("client settings frame sent",
          hc_write_frame(&c, T_SETTINGS, 0, 0, NULL, 0) == 0);

    int status = 0;
    Strbuf body;
    strbuf_init(&body);
    int r = hc_read_response(&c, 1, &status, &body, NULL, NULL);
    check("upgrade stream 1 status 200", r == 0 && status == 200);
    check("upgrade stream 1 body",
          body.count == strlen("Hello, /up!") &&
          memcmp(body.items, "Hello, /up!", body.count) == 0);
    strbuf_free(&body);
    hc_close(&c);
    stop_server(pid);
}

int main(void)
{
    if (net_init() != 0) {
        fprintf(stderr, "net_init failed\n");
        return 1;
    }

    test_prior_knowledge_get();
    test_multi_stream();
    test_post_echo();
    test_streaming();
    test_not_found();
    test_body_too_large();
    test_malformed_request_rst();
    test_h2c_upgrade();

    net_cleanup();
    if (fails == 0) {
        printf("h2 ok\n");
    }
    return fails != 0;
}