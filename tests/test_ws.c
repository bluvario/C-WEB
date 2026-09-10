#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "net.h"
#include "request.h"
#include "response.h"
#include "server.h"
#include "strbuf.h"
#include "sv.h"
#include "ws.h"
#include "xmem.h"

static int fails = 0;
static void check(const char *what, int cond)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        fails++;
    }
}

static void hexdump_into(char *out, size_t cap, const uint8_t *b, size_t n)
{
    size_t at = 0;
    for (size_t i = 0; i < n && at + 2 < cap; i++) {
        sprintf(out + at, "%02x", b[i]);
        at += 2;
    }
}

// RFC 6455 section 1.3 example request
static const char *RFC_KEYS =
    "GET /chat HTTP/1.1\r\n"
    "Host: server.example.com\r\n"
    "Upgrade: websocket\r\n"
    "Connection: Upgrade\r\n"
    "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
    "Origin: http://example.com\r\n"
    "Sec-WebSocket-Version: 13\r\n"
    "\r\n";

static char *accept_header(Http_Response *res)
{
    return http_response_get_header(res, "Sec-WebSocket-Accept");
}

// builds a masked client frame exactly as a browser would send it
static size_t client_frame(uint8_t *out, uint8_t opcode, const uint8_t *payload,
                           size_t len, const uint8_t mask[4])
{
    size_t at = 0;
    out[at++] = (uint8_t)(0x80 | opcode); // FIN, no RSV
    if (len <= 125) {
        out[at++] = (uint8_t)(0x80 | len);
    } else if (len <= 0xFFFF) {
        out[at++] = (uint8_t)(0x80 | 126);
        out[at++] = (uint8_t)(len >> 8);
        out[at++] = (uint8_t)len;
    } else {
        out[at++] = (uint8_t)(0x80 | 127);
        for (int i = 0; i < 8; i++) {
            out[at++] = (uint8_t)((uint64_t)len >> (56 - 8 * i));
        }
    }
    out[at++] = mask[0];
    out[at++] = mask[1];
    out[at++] = mask[2];
    out[at++] = mask[3];
    for (size_t i = 0; i < len; i++) {
        out[at++] = (uint8_t)(payload[i] ^ mask[i & 3]);
    }
    return at;
}

// reads from client until it holds head_end + tail_len bytes after it
static void read_until(char *buf, size_t cap, Socket_Handle client,
                       const char *head_end, size_t tail_len)
{
    size_t got = 0;
    size_t he = strlen(head_end);
    for (;;) {
        for (size_t i = 0; i + he + tail_len <= got; i++) {
            if (memcmp(buf + i, head_end, he) == 0) {
                return;
            }
        }
        long n = net_recv(client, buf + got, cap - got - 1);
        if (n <= 0) {
            return;
        }
        got += (size_t)n;
    }
}

// echo the first text frame and hang up
static void echo_on_open(Http_Ws_Conn *conn, void *user_data)
{
    int *did_echo = user_data;
    uint8_t buf[2048];
    Http_Ws_Frame f;
    int r = http_ws_read_frame(conn, &f, buf, sizeof buf);
    if (r == 1 && f.opcode == WS_OP_TEXT) {
        *did_echo = 1;
        http_ws_send_text(conn, (const char *)buf);
    }
    http_ws_send_close(conn, 1000);
}

static void upgrade_handler(Http_Request *req, Http_Response *res,
                            void *user_data)
{
    http_ws_upgrade(req, res, echo_on_open, user_data);
}

int main(void)
{
    if (net_init() != 0) {
        fprintf(stderr, "net_init failed: %s\n", net_error_string());
        return 1;
    }

    // SHA-1 vectors from RFC 3174 (the handshake derives the accept key with it)
    {
        uint8_t d[20];
        char hex[64];
        http_ws_sha1((const uint8_t *)"abc", 3, d);
        hexdump_into(hex, sizeof hex, d, 20);
        check("sha1(\"abc\")", strcmp(hex, "a9993e364706816aba3e25717850c26c9cd0d89d") == 0);
        http_ws_sha1((const uint8_t *)"", 0, d);
        hexdump_into(hex, sizeof hex, d, 20);
        check("sha1(\"\")", strcmp(hex, "da39a3ee5e6b4b0d3255bfef95601890afd80709") == 0);
        const char *msg = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
        http_ws_sha1((const uint8_t *)msg, strlen(msg), d);
        hexdump_into(hex, sizeof hex, d, 20);
        check("sha1(two-block)", strcmp(hex, "84983e441c3bd26ebaae4aa1f95129e5e54670f1") == 0);
    }

    // a valid handshake turns into a 101 with the RFC-published accept value
    {
        Http_Request req;
        http_request_parse(&req, sv_from_cstr(RFC_KEYS));
        Http_Response res;
        http_response_init(&res);
        int rc = http_ws_upgrade(&req, &res, echo_on_open, NULL);
        check("handshake accepted", rc == 0);
        check("status is 101", res.status == HTTP_101_SWITCHING_PROTOCOLS);
        char *acc = accept_header(&res);
        check("accept matches RFC 6455", acc != NULL &&
                                         strcmp(acc, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") == 0);
        free(acc);

        Strbuf wire;
        strbuf_init(&wire);
        http_response_serialize(&res, &wire);
        strbuf_null_terminate(&wire);
        check("wire says switching protocols",
              strstr(wire.items, "101 Switching Protocols") != NULL);
        check("wire keeps Connection: Upgrade",
              strstr(wire.items, "Connection: Upgrade\r\n") != NULL);
        check("wire advertises Upgrade: websocket",
              strstr(wire.items, "Upgrade: websocket\r\n") != NULL);
        check("no Content-Length on a 101",
              strstr(wire.items, "Content-Length:") == NULL);
        check("no default Connection: close on a 101",
              strstr(wire.items, "Connection: close") == NULL);
        http_request_free(&req);
        http_response_free(&res);
        strbuf_free(&wire);
    }

    // malformed handshakes are refused
    {
        static const char *bad[] = {
            "POST /chat HTTP/1.1\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n",
            "GET / HTTP/1.0\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n",
            "GET / HTTP/1.1\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n",
            "GET / HTTP/1.1\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
            "Sec-WebSocket-Version: 8\r\n\r\n",
            "GET / HTTP/1.1\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n",
            "GET / HTTP/1.1\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Key: dGhlIHNhbXBsZQ==\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n",
        };
        for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
            Http_Request req;
            http_request_parse(&req, sv_from_cstr(bad[i]));
            Http_Response res;
            http_response_init(&res);
            check("malformed handshake refused", http_ws_upgrade(&req, &res, NULL, NULL) == -1);
            http_request_free(&req);
            http_response_free(&res);
        }
    }

    // frame decoding over a real loopback socket: masked client frames, both
    // small lengths and the 126/127 extended-length encodings
    {
        Socket_Handle srv = net_listen(0);
        int port = net_bound_port(srv);
        Socket_Handle client = net_connect("127.0.0.1", port);
        Socket_Handle peer = net_accept(srv);

        Http_Ws_Conn conn;
        memset(&conn, 0, sizeof conn);
        conn.client = peer;
        rb_init(&conn.rb, 90000);

        uint8_t payload[90000];
        Http_Ws_Frame frame;
        const uint8_t mk[4] = {0xA7, 0x3C, 0x9E, 0x55};

        // small masked text frame "hi"
        uint8_t f[8];
        size_t fl = client_frame(f, WS_OP_TEXT, (const uint8_t *)"hi", 2, mk);
        net_send_all(client, f, fl);
        check("small text frame read", http_ws_read_frame(&conn, &frame, payload, sizeof payload) == 1);
        check("opcode TEXT", frame.opcode == WS_OP_TEXT);
        check("fin set", frame.fin);
        check("payload length", frame.payload_len == 2);
        check("payload 'hi'", payload[0] == 'h' && payload[1] == 'i');

        // 300 payload bytes forces the 16-bit extended length (126)
        uint8_t big[300];
        for (int i = 0; i < 300; i++) big[i] = (uint8_t)i;
        uint8_t wf[2 + 2 + 4 + 300];
        fl = client_frame(wf, WS_OP_BIN, big, 300, mk);
        net_send_all(client, wf, fl);
        check("extended 16-bit frame read", http_ws_read_frame(&conn, &frame, payload, sizeof payload) == 1);
        check("16-bit payload length", frame.payload_len == 300);
        check("16-bit payload bytes intact", (int)memcmp(payload, big, 300) == 0);

        // > 65535 payload bytes forces the 64-bit extended length (127)
        size_t huge = 70000;
        uint8_t *hb = xmalloc(huge);
        for (size_t i = 0; i < huge; i++) hb[i] = (uint8_t)(i * 7);
        uint8_t *hf = xmalloc(2 + 8 + 4 + huge);
        fl = client_frame(hf, WS_OP_BIN, hb, huge, mk);
        net_send_all(client, hf, fl);
        check("extended 64-bit frame read", http_ws_read_frame(&conn, &frame, payload, sizeof payload) == 1);
        check("64-bit payload length", frame.payload_len == huge);
        check("64-bit payload bytes intact", (int)memcmp(payload, hb, huge) == 0);
        xfree(hb);
        xfree(hf);

        // a masked close frame is decoded and surfaces as the end of a session
        uint8_t cf[10];
        const uint8_t mk2[4] = {0, 0, 0, 0};
        fl = client_frame(cf, WS_OP_CLOSE, (const uint8_t *)"\x03\xe8", 2, mk2);
        net_send_all(client, cf, fl);
        check("close frame read", http_ws_read_frame(&conn, &frame, payload, sizeof payload) == 0);
        check("close opcode", frame.opcode == WS_OP_CLOSE);

        rb_free(&conn.rb);
        net_close(peer);
        net_close(client);
        net_close(srv);
    }

    // frame sending over a loopback socket: the wire carries an unmasked
    // server frame with the correct header bytes
    {
        Socket_Handle srv = net_listen(0);
        int port = net_bound_port(srv);
        Socket_Handle client = net_connect("127.0.0.1", port);
        Socket_Handle peer = net_accept(srv);

        Http_Ws_Conn conn;
        memset(&conn, 0, sizeof conn);
        conn.client = peer;
        rb_init(&conn.rb, 4096);

        check("short text send", http_ws_send_text(&conn, "yo") == 0);
        uint8_t raw[512] = {0};
        long n = net_recv(client, raw, sizeof raw);
        check("text frame on the wire", n == 4 && raw[0] == 0x81 && raw[1] == 0x02 &&
                                          raw[2] == 'y' && raw[3] == 'o');

        check("close send", http_ws_send_close(&conn, 1000) == 0);
        n = net_recv(client, raw, sizeof raw);
        check("close frame on the wire", n == 4 && raw[0] == 0x88 && raw[1] == 0x02 &&
                                          raw[2] == 0x03 && raw[3] == 0xe8);

        // a 300-byte message uses the 16-bit extended length (unmasked here)
        uint8_t big[300];
        for (int i = 0; i < 300; i++) big[i] = (uint8_t)i;
        check("extended send", http_ws_send(&conn, WS_OP_BIN, big, 300, true) == 0);
        size_t got = 0;
        while (got < 4 + 300) {
            long k = net_recv(client, raw + got, sizeof raw - got);
            if (k <= 0) {
                break;
            }
            got += (size_t)k;
        }
        check("extended header on the wire in one write",
              got == 4 + 300 && raw[0] == 0x82 && raw[1] == 126 && raw[2] == 0x01 && raw[3] == 0x2c);
        check("extended payload arrived", raw[4] == 0x00 && raw[303] == 0x2b);

        rb_free(&conn.rb);
        net_close(peer);
        net_close(client);
        net_close(srv);
    }

    // full pipeline through the real connection driver: the client sends the
    // handshake and the first masked text frame pipelined in one write; the
    // 101 head goes out, the leftover bytes seed the WebSocket stream, the
    // echo comes back, and the connection is closed without draining as a
    // keep-alive request
    {
        Socket_Handle srv = net_listen(0);
        int port = net_bound_port(srv);
        Socket_Handle client = net_connect("127.0.0.1", port);
        Socket_Handle peer = net_accept(srv);

        // handshake then a pipelined masked "hi" frame, as one client write
        char reqbuf[1024];
        strcpy(reqbuf, "GET /echo HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\n"
                       "Connection: Upgrade\r\n"
                       "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                       "Sec-WebSocket-Version: 13\r\n\r\n");
        size_t rl = strlen(reqbuf);
        const uint8_t mk[4] = {0x11, 0x22, 0x33, 0x44};
        uint8_t frame[8];
        size_t fl = client_frame(frame, WS_OP_TEXT, (const uint8_t *)"hi", 2, mk);
        net_send_all(client, reqbuf, rl);
        net_send_all(client, frame, fl);

        int did_echo = 0;
        Http_Server_Config cfg;
        memset(&cfg, 0, sizeof cfg);
        cfg.io_timeout_ms = 5000;
        cfg.workers = 0;
        http_serve_connection_config(peer, upgrade_handler, &did_echo, &cfg);

        check("echo callback ran", did_echo == 1);
        char wire[4096];
        read_until(wire, sizeof wire, client, "\r\n\r\n", 8);
        char *bodyp = strstr(wire, "\r\n\r\n");
        check("101 head on the wire", bodyp != NULL &&
                                        strstr(wire, "101 Switching Protocols") != NULL);
        check("Sec-WebSocket-Accept on the wire", bodyp != NULL &&
                                          strstr(wire, "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != NULL);
        if (bodyp != NULL) {
            const uint8_t *tail = (const uint8_t *)(bodyp + 4);
            check("echo text frame followed the head",
                  tail[0] == 0x81 && tail[1] == 0x02 && tail[2] == 'h' && tail[3] == 'i');
            check("close frame followed the echo",
                  tail[4] == 0x88 && tail[5] == 0x02 && tail[6] == 0x03 && tail[7] == 0xe8);
        }

        net_close(peer);
        net_close(client);
        net_close(srv);
    }

    if (fails == 0) {
        printf("ws ok\n");
    }
    return fails != 0;
}