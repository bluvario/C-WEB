#define _POSIX_C_SOURCE 200809L

#include "ws.h"

#include <string.h>

#include "base64.h"
#include "log.h"
#include "strbuf.h"
#include "sv.h"
#include "xmem.h"

// server-side WebSocket (RFC 6455). the handshake derives a Sec-WebSocket-
// Accept value from the client key with SHA-1, so a compact SHA-1 lives here
// too; the rest of the framework only ever needs SHA-256 (hmac.h).

static uint32_t ws_rol(uint32_t x, int bits)
{
    return (x << bits) | (x >> (32 - bits));
}

static void ws_sha1_block(uint32_t h[5], const uint8_t block[64])
{
    uint32_t w[80];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[4 * i] << 24) | ((uint32_t)block[4 * i + 1] << 16) |
               ((uint32_t)block[4 * i + 2] << 8) | (uint32_t)block[4 * i + 3];
    }
    for (int i = 16; i < 80; i++) {
        w[i] = ws_rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5A827999u;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1u;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDCu;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6u;
        }
        uint32_t t = ws_rol(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = ws_rol(b, 30);
        b = a;
        a = t;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
}

// one-shot FIPS 180-1 SHA-1: writes the 20-byte big-endian digest into out
void http_ws_sha1(const uint8_t *data, size_t len, uint8_t out[20])
{
    uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    uint64_t bitlen = (uint64_t)len * 8;
    uint8_t block[64];
    size_t at = 0;
    for (;;) {
        size_t need = len - at;
        bool last = need < 64;
        if (!last) {
            memcpy(block, data + at, 64);
            at += 64;
        } else {
            // final block: message tail, a 0x80 byte, then the 64-bit length
            memset(block, 0, sizeof block);
            memcpy(block, data + at, need);
            block[need] = 0x80;
            if (need >= 56) {
                // the length field no longer fits: pad out this block and
                // carry the length over to a fresh one
                ws_sha1_block(h, block);
                memset(block, 0, sizeof block);
            }
            for (int i = 0; i < 8; i++) {
                block[63 - i] = (uint8_t)(bitlen >> (8 * i));
            }
            at = len;
        }
        ws_sha1_block(h, block);
        if (last) {
            break;
        }
    }
    for (int i = 0; i < 5; i++) {
        out[4 * i] = (uint8_t)(h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(h[i] >> 8);
        out[4 * i + 3] = (uint8_t)h[i];
    }
}

const char WS_GUID[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

// does a comma/space separated header value contain this token, any case?
static bool ws_header_has_token(Http_Request *req, const char *name,
                                const char *token)
{
    const String_View *value = http_request_get_header(req, name);
    if (value == NULL) {
        return false;
    }
    size_t n = strlen(token);
    String_View rest = *value;
    while (rest.count > 0) {
        String_View part = sv_trim(sv_chop_by_delim(&rest, ','));
        if (part.count == n) {
            bool same = true;
            for (size_t i = 0; i < n; i++) {
                char a = (char)part.data[i];
                char b = token[i];
                if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
                if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
                if (a != b) {
                    same = false;
                    break;
                }
            }
            if (same) {
                return true;
            }
        }
    }
    return false;
}

// the framing user_data http_ws_upgrade stores in the response: the app's
// on_open and its context, handed back to the server-side trampoline
typedef struct {
    Http_Ws_On_Open on_open;
    void *user_data;
} Ws_Upgrade;

// the response's upgrade_fn: build a connection (seeding its read buffer with
// whatever the client pipelined past the handshake), run the app, clean up
static void ws_trampoline(Socket_Handle client, String_View leftover,
                          void *opaque)
{
    Ws_Upgrade *u = opaque;
    Http_Ws_Conn conn;
    memset(&conn, 0, sizeof conn);
    conn.client = client;
    size_t cap = leftover.count + 4096;
    if (cap < 4096 * 16) {
        cap = 4096 * 16; // 64 KiB default staging
    }
    rb_init(&conn.rb, cap);
    if (leftover.count > 0) {
        memcpy(conn.rb.data, leftover.data, leftover.count);
        conn.rb.count = leftover.count;
    }
    if (u->on_open != NULL) {
        u->on_open(&conn, u->user_data);
    }
    rb_free(&conn.rb);
    xfree(u);
}

int http_ws_upgrade(Http_Request *req, Http_Response *res,
                    Http_Ws_On_Open on_open, void *user_data)
{
    // RFC 6455 section 4.2.1: GET, HTTP/1.1 or better, an Upgrade header that
    // names websocket, a Connection header that names Upgrade, a
    // Sec-WebSocket-Key that base64-decodes to 16 bytes, version 13
    if (req->method != HTTP_GET) {
        return -1;
    }
    if (!sv_equal(req->version, sv_from_cstr("HTTP/1.1"))) {
        return -1;
    }
    if (!ws_header_has_token(req, "upgrade", "websocket")) {
        return -1;
    }
    if (!ws_header_has_token(req, "connection", "upgrade")) {
        return -1;
    }
    const String_View *key = http_request_get_header(req, "sec-websocket-key");
    if (key == NULL) {
        return -1;
    }
    const String_View *vers = http_request_get_header(req, "sec-websocket-version");
    if (vers == NULL || !sv_equal(sv_trim(*vers), sv_from_cstr("13"))) {
        return -1;
    }

    Strbuf key_bytes;
    strbuf_init(&key_bytes);
    if (base64_decode_into(&key_bytes, *key) != 0 || key_bytes.count != 16) {
        strbuf_free(&key_bytes);
        return -1;
    }
    strbuf_free(&key_bytes);

    // accept = base64(sha1(key || GUID)) as the RFC directs
    char joined[196];
    size_t key_len = key->count < sizeof joined - sizeof WS_GUID - 1
                         ? key->count
                         : sizeof joined - sizeof WS_GUID - 1;
    memcpy(joined, key->data, key_len);
    memcpy(joined + key_len, WS_GUID, sizeof WS_GUID - 1);

    uint8_t digest[20];
    http_ws_sha1((const uint8_t *)joined, key_len + sizeof WS_GUID - 1, digest);

    Strbuf accept;
    strbuf_init(&accept);
    base64_encode_into(&accept, (String_View){(const char *)digest, 20});
    strbuf_append_char(&accept, '\0');

    Ws_Upgrade *u = xmalloc(sizeof *u);
    u->on_open = on_open;
    u->user_data = user_data;

    http_response_set_status(res, HTTP_101_SWITCHING_PROTOCOLS);
    http_response_set_header(res, "Upgrade", "websocket");
    http_response_set_header(res, "Connection", "Upgrade");
    http_response_set_header(res, "Sec-WebSocket-Accept", accept.items);
    res->upgrade_fn = ws_trampoline;
    res->upgrade_user = u;
    strbuf_free(&accept);
    return 0;
}

// tops the connection's read buffer up to at least need bytes, blocking on the
// socket as necessary. returns 0 or -1 (peer went away, read timed out, or
// buffer exhausted with need still unmet).
static int ws_ensure(Http_Ws_Conn *conn, size_t need)
{
    while (rb_view(&conn->rb).count < need) {
        String_View tail = rb_write_head(&conn->rb);
        if (tail.count == 0) {
            return -1;
        }
        long n = net_recv(conn->client, (void *)tail.data, tail.count);
        if (n == NET_READ_TIMEOUT || n <= 0) {
            return -1;
        }
        rb_commit(&conn->rb, (size_t)n);
    }
    return 0;
}

int http_ws_read_frame(Http_Ws_Conn *conn, Http_Ws_Frame *frame,
                       uint8_t *payload, size_t payload_cap)
{
    if (ws_ensure(conn, 2) != 0) {
        return -1;
    }
    String_View v = rb_view(&conn->rb);
    uint8_t b0 = (uint8_t)v.data[0];
    uint8_t b1 = (uint8_t)v.data[1];
    if (b0 & 0x70) {
        return -1; // RSV1-3 must be clear for the server's sesquipedalian frames
    }
    bool fin = (b0 & 0x80) != 0;
    uint8_t opcode = b0 & 0x0f;
    bool control = opcode >= 0x8;
    if (control ? (opcode != WS_OP_CLOSE && opcode != WS_OP_PING && opcode != WS_OP_PONG)
                : (opcode != WS_OP_CONT && opcode != WS_OP_TEXT && opcode != WS_OP_BIN)) {
        return -1; // reserved opcode
    }
    // RFC 6455 5.1/5.5: control frames are not fragmented and hold <= 125
    // payload bytes; every frame from a client must be masked
    if (control && (!fin || (b1 & 0x7f) > 125)) {
        return -1;
    }
    if (!(b1 & 0x80)) {
        return -1; // client frames must carry a masking key
    }

    size_t hdr = 2;
    uint64_t len = b1 & 0x7f;
    if (len == 126) {
        if (ws_ensure(conn, 4) != 0) {
            return -1;
        }
        v = rb_view(&conn->rb);
        len = ((uint64_t)(uint8_t)v.data[2] << 8) | (uint8_t)v.data[3];
        hdr = 4;
    } else if (len == 127) {
        if (ws_ensure(conn, 10) != 0) {
            return -1;
        }
        v = rb_view(&conn->rb);
        if (v.data[2] & 0x80) {
            return -1; // RFC 6455 5.2: the high bit of a 64-bit length must be 0
        }
        len = 0;
        for (int i = 0; i < 8; i++) {
            len = (len << 8) | (uint8_t)v.data[2 + i];
        }
        hdr = 10;
    }
    if (len > payload_cap) {
        return -1; // caller decides how (close with 1009, or grow the buffer)
    }

    if (ws_ensure(conn, hdr + 4 + (size_t)len) != 0) {
        return -1;
    }
    v = rb_view(&conn->rb);

    uint8_t mask[4];
    mask[0] = (uint8_t)v.data[hdr];
    mask[1] = (uint8_t)v.data[hdr + 1];
    mask[2] = (uint8_t)v.data[hdr + 2];
    mask[3] = (uint8_t)v.data[hdr + 3];

    const uint8_t *src = (const uint8_t *)v.data + hdr + 4;
    for (size_t i = 0; i < (size_t)len; i++) {
        payload[i] = (uint8_t)(src[i] ^ mask[i & 3]);
    }
    rb_discard(&conn->rb, hdr + 4 + (size_t)len);

    frame->fin = fin;
    frame->opcode = opcode;
    frame->payload_len = len;
    return opcode == WS_OP_CLOSE ? 0 : 1;
}

int http_ws_send(Http_Ws_Conn *conn, uint8_t opcode,
                 const uint8_t *payload, size_t len, bool fin)
{
    uint8_t hdr[14];
    size_t h = 0;
    hdr[h++] = (uint8_t)((fin ? 0x80u : 0x00u) | (opcode & 0x0f));
    if (len <= 125) {
        hdr[h++] = (uint8_t)len;
    } else if (len <= 0xFFFF) {
        hdr[h++] = 126;
        hdr[h++] = (uint8_t)(len >> 8);
        hdr[h++] = (uint8_t)len;
    } else {
        hdr[h++] = 127;
        for (int i = 0; i < 8; i++) {
            hdr[h++] = (uint8_t)((uint64_t)len >> (56 - 8 * i));
        }
    }
    if (net_send_all(conn->client, hdr, h) < 0) {
        return -1;
    }
    if (len > 0 && net_send_all(conn->client, payload, len) < 0) {
        return -1;
    }
    return 0;
}

int http_ws_send_text(Http_Ws_Conn *conn, const char *text)
{
    return http_ws_send(conn, WS_OP_TEXT, (const uint8_t *)text, strlen(text), true);
}

int http_ws_send_close(Http_Ws_Conn *conn, uint16_t code)
{
    uint8_t payload[2] = {(uint8_t)(code >> 8), (uint8_t)code};
    return http_ws_send(conn, WS_OP_CLOSE, payload, sizeof payload, true);
}

int http_ws_send_pong(Http_Ws_Conn *conn, const uint8_t *payload, size_t len)
{
    return http_ws_send(conn, WS_OP_PONG, payload, len, true);
}

int http_ws_loop(Http_Ws_Conn *conn, Http_Ws_On_Message on_message,
                 void *user_data, uint8_t *buf, size_t buf_cap)
{
    for (;;) {
        Http_Ws_Frame frame;
        int r = http_ws_read_frame(conn, &frame, buf, buf_cap);
        if (r == 0) {
            // the peer asked to close; echo 1000 and end the conversation
            http_ws_send_close(conn, 1000);
            return 0;
        }
        if (r < 0) {
            return -1;
        }
        if (frame.opcode == WS_OP_PING) {
            http_ws_send_pong(conn, buf, (size_t)frame.payload_len);
            continue;
        }
        if (frame.opcode == WS_OP_PONG) {
            continue; // answered only to keep the link alive, nothing to do
        }
        if (on_message != NULL && on_message(conn, &frame, buf, user_data) != 0) {
            return 0;
        }
    }
}