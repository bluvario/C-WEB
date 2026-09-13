#include "h2.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "da.h"
#include "date.h"
#include "gzip.h"
#include "hpack.h"
#include "log.h"
#include "net.h"
#include "request.h"
#include "response.h"
#include "strbuf.h"
#include "sv.h"
#include "tls.h"
#include "xmem.h"

#define H2_INITIAL_WINDOW 65535u
#define H2_MAX_FRAME_SIZE 16384u
#define H2_FRAME_HDR 9u
#define H2_BUFFER_CAP (64u * 1024u)
#define H2_HEADER_BLOCK_CAP (256u * 1024u)
#define H2_MAX_HEADER_LIST (256u * 1024u)
#define H2_ERROR_BODY "cweb error page (it hurts us too)\r\n"

typedef enum {
    H2_DATA = 0x0,
    H2_HEADERS,
    H2_PRIORITY,
    H2_RST,
    H2_SETTINGS,
    H2_PUSH_PROMISE,
    H2_PING,
    H2_GOAWAY,
    H2_WINDOW,
    H2_CONT
} H2_Frame_Type;

typedef enum {
    H2F_END_STREAM = 0x1,
    H2F_ACK = 0x1,
    H2F_END_HEADERS = 0x4,
    H2F_PADDED = 0x8,
    H2F_PRIORITY = 0x20
} H2_Frame_Flag;

typedef enum {
    HE_NO_ERROR = 0x0,
    HE_PROTOCOL = 0x1,
    HE_INTERNAL = 0x2,
    HE_FLOW = 0x3,
    HE_SETTINGS_TIMEOUT = 0x4,
    HE_STREAM_CLOSED = 0x5,
    HE_FRAME_SIZE = 0x6,
    HE_REFUSED = 0x7,
    HE_CANCEL = 0x8,
    HE_COMPRESSION = 0x9,
    HE_CONNECT = 0xa,
    HE_CALM = 0xb,
    HE_SECURITY = 0xc,
    HE_HTTP11 = 0xd
} H2_Error;

typedef struct {
    uint32_t len;
    uint8_t type;
    uint8_t flags;
    uint32_t stream_id;
    const uint8_t *payload;
} H2_Frame;

typedef struct {
    uint32_t id;
    bool open;
    bool remote_ended;
    bool dispatched;
    bool response_ready;
    bool headers_sent;
    bool local_ended;
    bool reset;
    bool body_too_big;
    Http_Request req;
    Http_Response res;
    Strbuf body;
    Strbuf storage;
    size_t req_bytes;
    size_t queue_i;
    size_t queue_off;
    long long body_off;
    long long body_sent;
    bool fn_eof;
    char fn_buf[16384];
    long long recv_window;
    long long send_window;
} H2_Stream;

typedef struct {
    Socket_Handle fd;
    void *ssl;  // OpenSSL session (tls.h) when this is HTTP/2 over TLS
    Http_Handler_Fn handler;
    void *user_data;
    const Http_Server_Config *cfg;
    String_View remote;
    size_t max_body;
    unsigned long timeout_ms;

    Hpack hpack;
    Read_Buffer rb;
    H2_Stream *streams;
    size_t streams_count;
    size_t streams_cap;
    uint32_t last_stream_id;
    bool got_client_settings;
    bool client_goaway;
    uint32_t client_goaway_last;
    long long conn_recv_window;
    long long conn_send_window;
    uint32_t client_init_window;
    uint32_t error;

    uint32_t hb_stream;
    Strbuf hb;
    bool hb_end_stream;
    bool hb_abort;
} H2_Conn;

static size_t h2_find_stream(const H2_Conn *c, uint32_t id)
{
    for (size_t i = 0; i < c->streams_count; i++) {
        if (c->streams[i].open && c->streams[i].id == id) {
            return i;
        }
    }
    return (size_t)-1;
}

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] << 8 | p[1]);
}

static uint32_t rd32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static bool sv_ieq_str(String_View a, const char *b)
{
    size_t n = strlen(b);
    if (a.count != n) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (tolower((unsigned char)a.data[i]) !=
            tolower((unsigned char)b[i])) {
            return false;
        }
    }
    return true;
}

static bool tok_in(String_View sv, const char *token)
{
    size_t n = strlen(token);
    String_View rest = sv;
    while (rest.count > 0) {
        String_View part = sv_chop_by_delim(&rest, ',');
        part = sv_trim(part);
        if (part.count == n) {
            bool same = true;
            for (size_t i = 0; i < n; i++) {
                if (tolower((unsigned char)part.data[i]) !=
                    tolower((unsigned char)token[i])) {
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

static bool header_has_token(const Http_Request *req, const char *name,
                             const char *token)
{
    const String_View *v = http_request_get_header((Http_Request *)req, name);
    if (v == NULL) {
        return false;
    }
    return tok_in(*v, token);
}

static bool conn_spec_name(String_View name)
{
    static const char *const bad[] = {
        "connection", "keep-alive", "proxy-connection",
        "transfer-encoding", "upgrade",
    };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        if (sv_ieq_str(name, bad[i])) {
            return true;
        }
    }
    return false;
}

static bool token_char(char c)
{
    unsigned char u = (unsigned char)c;
    if ((u >= 'a' && u <= 'z') || (u >= '0' && u <= '9')) {
        return true;
    }
    switch (c) {
    case '!': case '#': case '$': case '%': case '&': case '\'':
    case '*': case '+': case '.': case '^': case '_': case '`':
    case '|': case '~': case '-':
        return true;
    default:
        return false;
    }
}

static bool name_valid(String_View name)
{
    if (name.count == 0) {
        return false;
    }
    for (size_t i = 0; i < name.count; i++) {
        unsigned char c = (unsigned char)name.data[i];
        if (!token_char((char)c) || (c >= 'A' && c <= 'Z')) {
            return false;
        }
    }
    return true;
}

static int h2_write(const H2_Conn *c, const void *buf, size_t len)
{
    long n = c->ssl != NULL ? tls_send_all(c->ssl, buf, len)
                            : net_send_all(c->fd, buf, len);
    return n == (long)len ? 0 : -1;
}

static void h2_conn_error(H2_Conn *c, uint32_t err);

static int h2_send_frame(H2_Conn *c, H2_Frame_Type type, uint8_t flags,
                         uint32_t stream_id, const void *payload, size_t len)
{
    if (len > H2_MAX_FRAME_SIZE) {
        h2_conn_error(c, HE_FRAME_SIZE);
        return -1;
    }
    uint8_t h[9];
    h[0] = (uint8_t)(len >> 16);
    h[1] = (uint8_t)(len >> 8);
    h[2] = (uint8_t)len;
    h[3] = (uint8_t)type;
    h[4] = flags;
    h[5] = (uint8_t)((stream_id >> 24) & 0x7f);
    h[6] = (uint8_t)(stream_id >> 16);
    h[7] = (uint8_t)(stream_id >> 8);
    h[8] = (uint8_t)stream_id;
    if (h2_write(c, h, sizeof h) != 0) {
        return -1;
    }
    if (len > 0 && h2_write(c, payload, len) != 0) {
        return -1;
    }
    return 0;
}

static int h2_send_goaway(H2_Conn *c, uint32_t last_stream, uint32_t err)
{
    uint8_t p[8];
    p[0] = (uint8_t)((last_stream >> 24) & 0x7f);
    p[1] = (uint8_t)(last_stream >> 16);
    p[2] = (uint8_t)(last_stream >> 8);
    p[3] = (uint8_t)last_stream;
    p[4] = (uint8_t)(err >> 24);
    p[5] = (uint8_t)(err >> 16);
    p[6] = (uint8_t)(err >> 8);
    p[7] = (uint8_t)err;
    return h2_send_frame(c, H2_GOAWAY, 0, 0, p, sizeof p);
}

static int h2_send_stream_error(H2_Conn *c, uint32_t stream_id, uint32_t err)
{
    uint8_t p[4];
    p[0] = (uint8_t)(err >> 24);
    p[1] = (uint8_t)(err >> 16);
    p[2] = (uint8_t)(err >> 8);
    p[3] = (uint8_t)err;
    return h2_send_frame(c, H2_RST, 0, stream_id, p, sizeof p);
}

static int h2_send_settings(H2_Conn *c)
{
    uint8_t p[6] = { 0x00, 0x03, 0x00, 0x00, 0x00, H2_MAX_CONCURRENT_STREAMS };
    return h2_send_frame(c, H2_SETTINGS, 0, 0, p, sizeof p);
}

static int h2_settings_ack(H2_Conn *c)
{
    return h2_send_frame(c, H2_SETTINGS, H2F_ACK, 0, NULL, 0);
}

static void h2_conn_error(H2_Conn *c, uint32_t err)
{
    if (c->error == HE_NO_ERROR) {
        c->error = err;
    }
}

static int h2_apply_settings(H2_Conn *c, const uint8_t *payload, uint32_t len)
{
    if (len % 6 != 0) {
        h2_conn_error(c, HE_FRAME_SIZE);
        return -1;
    }
    for (uint32_t off = 0; off < len && !c->error; off += 6) {
        uint16_t id = rd16(payload + off);
        uint32_t val = rd32(payload + off + 2);
        switch (id) {
        case 0x1:
            break;
        case 0x2:
            if (val > 1) {
                h2_conn_error(c, HE_PROTOCOL);
            }
            break;
        case 0x3:
            break;
        case 0x4:
            if (val > 0x7fffffffu) {
                h2_conn_error(c, HE_FLOW);
                break;
            }
            {
                long long delta = (long long)val -
                                  (long long)c->client_init_window;
                c->client_init_window = val;
                for (size_t i = 0; i < c->streams_count; i++) {
                    H2_Stream *s = &c->streams[i];
                    if (!s->open) {
                        continue;
                    }
                    s->send_window += delta;
                    if (s->send_window > 0x7fffffffLL) {
                        h2_conn_error(c, HE_FLOW);
                        break;
                    }
                }
            }
            break;
        case 0x5:
            if (val < H2_MAX_FRAME_SIZE || val > 0xffffffu) {
                h2_conn_error(c, HE_PROTOCOL);
            }
            break;
        case 0x6:
            break;
        default:
            break;
        }
    }
    return c->error ? -1 : 0;
}

static int h2_fill(H2_Conn *c)
{
    String_View w = rb_write_head(&c->rb);
    if (w.count == 0) {
        return -1;
    }
    long n = c->ssl != NULL ? tls_recv(c->ssl, (void *)w.data, w.count)
                            : net_recv(c->fd, (void *)w.data, w.count);
    if (n == NET_READ_TIMEOUT) {
        log_warn("HTTP/2 client idle, dropping connection");
        return -1;
    }
    if (n <= 0) {
        return -1;
    }
    rb_commit(&c->rb, (size_t)n);
    return 0;
}

static int h2_frame_peek(H2_Conn *c, H2_Frame *f)
{
    if (c->rb.count < H2_FRAME_HDR) {
        return 0;
    }
    const uint8_t *h = (const uint8_t *)c->rb.data;
    uint32_t len = ((uint32_t)h[0] << 16) | ((uint32_t)h[1] << 8) | h[2];
    if (len > H2_MAX_FRAME_SIZE) {
        h2_conn_error(c, HE_FRAME_SIZE);
        return -1;
    }
    if (c->rb.count < H2_FRAME_HDR + len) {
        return 0;
    }
    f->len = len;
    f->type = h[3];
    f->flags = h[4];
    f->stream_id = ((uint32_t)(h[5] & 0x7f) << 24) | ((uint32_t)h[6] << 16) |
                   ((uint32_t)h[7] << 8) | h[8];
    f->payload = h + H2_FRAME_HDR;
    return 1;
}

static void h2_send_data(H2_Conn *c, H2_Stream *s, const void *data,
                         size_t len, bool end_stream)
{
    uint8_t flags = end_stream ? H2F_END_STREAM : 0;
    if (h2_send_frame(c, H2_DATA, flags, s->id, data, len) != 0) {
        h2_conn_error(c, HE_INTERNAL);
        return;
    }
    if (len > 0) {
        c->conn_send_window -= (long long)len;
        s->send_window -= (long long)len;
        s->body_sent += (long long)len;
    }
}

static void h2_mark_response_done(H2_Conn *c, H2_Stream *s);

static int h2_next_data(H2_Stream *s, long long room, const uint8_t **src,
                        size_t *len, bool *eof)
{
    *src = NULL;
    *len = 0;
    *eof = false;
    bool streaming = s->res.stream_fn != NULL ||
                     s->res.stream_chunk_count > 0;
    if (streaming) {
        for (;;) {
            if (s->queue_i < s->res.stream_chunk_count) {
                Http_Stream_Chunk *ch = &s->res.stream_chunks[s->queue_i];
                size_t rem = ch->len - s->queue_off;
                if (rem == 0) {
                    s->queue_i++;
                    s->queue_off = 0;
                    continue;
                }
                if (room <= 0) {
                    return 0;
                }
                size_t cap = room > H2_MAX_FRAME_SIZE ? H2_MAX_FRAME_SIZE
                                                       : (size_t)room;
                size_t take = rem < cap ? rem : cap;
                *src = (const uint8_t *)ch->data + s->queue_off;
                *len = take;
                s->queue_off += take;
                if (s->queue_off == ch->len) {
                    s->queue_i++;
                    s->queue_off = 0;
                }
                if (s->queue_i >= s->res.stream_chunk_count &&
                    s->res.stream_fn == NULL) {
                    *eof = true;
                }
                return 1;
            }
            if (s->res.stream_fn == NULL) {
                *eof = true;
                return 1;
            }
            if (s->fn_eof) {
                *eof = true;
                return 1;
            }
            if (room <= 0) {
                return 0;
            }
            {
                size_t cap = room > H2_MAX_FRAME_SIZE ? H2_MAX_FRAME_SIZE
                                                       : (size_t)room;
                size_t got = s->res.stream_fn(s->fn_buf, cap,
                                              s->res.stream_user);
                if (got == 0 || got > sizeof s->fn_buf) {
                    s->fn_eof = true;
                    continue;
                }
                *src = (const uint8_t *)s->fn_buf;
                *len = got;
                return 1;
            }
        }
    }
    size_t rem = s->res.body.count - (size_t)s->body_off;
    if (rem == 0) {
        *eof = true;
        return 1;
    }
    if (room <= 0) {
        return 0;
    }
    {
        size_t cap = room > H2_MAX_FRAME_SIZE ? H2_MAX_FRAME_SIZE
                                               : (size_t)room;
        size_t take = rem < cap ? rem : cap;
        *src = (const uint8_t *)s->res.body.items + (size_t)s->body_off;
        *len = take;
        s->body_off += (long long)take;
        if ((size_t)s->body_off >= s->res.body.count) {
            *eof = true;
        }
        return 1;
    }
}

static void h2_log_request(H2_Stream *s, unsigned long long elapsed_ms)
{
    const char *method = http_method_name(s->req.method);
    if (method == NULL) {
        method = "???";
    }
    log_info("%s %.*s -> %d in %llu ms", method, (int)s->req.path.count,
             s->req.path.data, (int)s->res.status, elapsed_ms);
}

static void h2_clf_append_escaped(Strbuf *line, String_View s)
{
    for (size_t i = 0; i < s.count; i++) {
        unsigned char c = (unsigned char)s.data[i];
        if (c < 0x20 || c == 0x7f || c == '"' || c == '\\') {
            char esc[8];
            snprintf(esc, sizeof esc, "\\x%02X", c);
            strbuf_append_cstr(line, esc);
        } else {
            strbuf_append_char(line, (char)c);
        }
    }
}

static void h2_log_access(H2_Stream *s, long long bytes)
{
    char date[40];
    http_date_clf(time(NULL), date, sizeof date);
    String_View who = s->req.client_ip.count > 0 ? s->req.client_ip
                                                 : s->req.remote;
    if (who.count == 0) {
        who = sv_from_cstr("0.0.0.0");
    }
    Strbuf line;
    strbuf_init(&line);
    strbuf_append(&line, who.data, who.count);
    strbuf_append_cstr(&line, " - - [");
    strbuf_append_cstr(&line, date);
    strbuf_append_cstr(&line, "] \"");
    const char *method = http_method_name(s->req.method);
    strbuf_append_cstr(&line, method != NULL ? method : "???");
    strbuf_append_char(&line, ' ');
    h2_clf_append_escaped(&line, s->req.path);
    strbuf_append_char(&line, ' ');
    h2_clf_append_escaped(&line, s->req.version);
    strbuf_append_cstr(&line, "\" ");
    snprintf(date, sizeof date, "%d", (int)s->res.status);
    strbuf_append_cstr(&line, date);
    strbuf_append_char(&line, ' ');
    if (bytes > 0) {
        snprintf(date, sizeof date, "%lld", bytes);
        strbuf_append_cstr(&line, date);
    } else {
        strbuf_append_char(&line, '-');
    }
    log_clf_line(line.items, line.count);
    strbuf_free(&line);
}

static void h2_mark_response_done(H2_Conn *c, H2_Stream *s)
{
    (void)c;
    if (!s->response_ready) {
        return;
    }
    long long bytes = s->req.method == HTTP_HEAD ? 0 : s->body_sent;
    h2_log_access(s, bytes);
    s->response_ready = false;
}

static void h2_dispatch(H2_Conn *c, H2_Stream *s)
{
    s->dispatched = true;
    http_response_init(&s->res);
    unsigned long long t0 = time_mono_ms();
    c->handler(&s->req, &s->res, c->user_data);
    unsigned long long elapsed = time_mono_ms() - t0;
    h2_log_request(s, elapsed);
    s->response_ready = true;
    if (s->req.route_timeout_ms > 0) {
        net_set_timeout(c->fd, s->req.route_timeout_ms);
    } else {
        net_set_timeout(c->fd, c->timeout_ms);
    }
}

static void h2_direct_error(H2_Stream *s, Http_Status status)
{
    s->dispatched = true;
    http_response_init(&s->res);
    http_response_set_status(&s->res, status);
    http_response_set_header(&s->res, "Content-Type",
                             "text/plain; charset=utf-8");
    http_response_add_body_cstr(&s->res, H2_ERROR_BODY);
    s->response_ready = true;
}

static void h2_dispatch_ready(H2_Conn *c)
{
    if (c->error != HE_NO_ERROR) {
        return;
    }
    for (size_t i = 0; i < c->streams_count && !c->error; i++) {
        H2_Stream *s = &c->streams[i];
        if (!s->open || s->dispatched || s->reset || !s->remote_ended) {
            continue;
        }
        bool too_big = s->body_too_big;
        if (!too_big && s->req.content_length >= 0 &&
            (size_t)s->req.content_length > c->max_body) {
            too_big = true;
        }
        if (too_big) {
            h2_direct_error(s, HTTP_413_PAYLOAD_TOO_LARGE);
            continue;
        }
        if (header_has_token(&s->req, "content-encoding", "gzip") ||
            header_has_token(&s->req, "content-encoding", "x-gzip")) {
            size_t cap = c->cfg != NULL && c->cfg->max_inflated > 0
                             ? c->cfg->max_inflated
                             : HTTP_GZIP_MAX_INFLATED_DEFAULT;
            unsigned char *infl = NULL;
            size_t infl_len = 0;
            int drc = http_gzip_decompress(s->body.items, s->body.count,
                                           cap, &infl, &infl_len);
            if (drc == -1) {
                h2_direct_error(s, HTTP_400_BAD_REQUEST);
                continue;
            }
            if (drc == -2) {
                h2_direct_error(s, HTTP_413_PAYLOAD_TOO_LARGE);
                continue;
            }
            strbuf_free(&s->body);
            strbuf_init(&s->body);
            s->req.body_heap = infl;
            s->req.body = (String_View){ (const char *)infl, infl_len };
        } else {
            s->req.body = (String_View){ s->body.count > 0 ? s->body.items
                                                           : NULL,
                                         s->body.count };
        }
        h2_dispatch(c, s);
    }
}

typedef struct {
    const char *name;
    size_t name_len;
    const char *value;
    size_t value_len;
} H2_Field;

static int h2_request_from_block(H2_Conn *c, const char *blk, size_t n,
                                 H2_Stream *s)
{
    Hpack_Decoded dec = { 0 };
    if (hpack_decode_block(&c->hpack, (const uint8_t *)blk, n, &dec) < 0) {
        hpack_decoded_free(&dec);
        h2_conn_error(c, HE_COMPRESSION);
        return -1;
    }

    String_View method = { 0 }, scheme = { 0 }, path = { 0 };
    String_View authority = { 0 };
    bool pseudo_done = false;
    bool malformed = false;
    size_t list_bytes = 0;
    long long clv = -1;
    bool has_cl = false;
    H2_Field *fields = NULL;
    size_t fields_count = 0;
    size_t fields_cap = 0;

    for (size_t i = 0; i < dec.count && !malformed; i++) {
        String_View name = dec.items[i].name;
        String_View value = dec.items[i].value;
        list_bytes += name.count + value.count + 32u;
        if (list_bytes > H2_MAX_HEADER_LIST) {
            malformed = true;
            break;
        }
        bool pseudo = name.count > 0 && name.data[0] == ':';
        if (pseudo) {
            if (pseudo_done) {
                malformed = true;
                break;
            }
            if (name.count < 2) {
                malformed = true;
                break;
            }
            for (size_t k = 1; k < name.count; k++) {
                unsigned char cc = (unsigned char)name.data[k];
                if (!token_char((char)cc) || (cc >= 'A' && cc <= 'Z')) {
                    malformed = true;
                    break;
                }
            }
            if (malformed) {
                break;
            }
            if (sv_ieq_str(name, ":method") && method.count == 0) {
                method = value;
            } else if (sv_ieq_str(name, ":scheme") && scheme.count == 0) {
                scheme = value;
            } else if (sv_ieq_str(name, ":path") && path.count == 0) {
                path = value;
            } else if (sv_ieq_str(name, ":authority") &&
                       authority.count == 0) {
                authority = value;
            } else {
                malformed = true;
                break;
            }
            continue;
        }
        pseudo_done = true;
        if (!name_valid(name)) {
            malformed = true;
            break;
        }
        if (conn_spec_name(name)) {
            malformed = true;
            break;
        }
        if (sv_ieq_str(name, "te")) {
            if (!sv_equal(sv_trim(value), sv_from_cstr("trailers"))) {
                malformed = true;
                break;
            }
            continue;
        }
        if (sv_ieq_str(name, "content-length")) {
            if (has_cl) {
                malformed = true;
                break;
            }
            long long parsed;
            if (!sv_to_i64(value, &parsed) || parsed < 0) {
                malformed = true;
                break;
            }
            has_cl = true;
            clv = parsed;
        }
        if (fields_count == fields_cap) {
            fields_cap = fields_cap ? fields_cap * 2 : 16;
            fields = xrealloc(fields, fields_cap * sizeof *fields);
        }
        fields[fields_count].name = name.data;
        fields[fields_count].name_len = name.count;
        fields[fields_count].value = value.data;
        fields[fields_count].value_len = value.count;
        fields_count++;
    }
    if (malformed) {
        hpack_decoded_free(&dec);
        xfree(fields);
        return -2;
    }

    bool is_connect = sv_ieq_str(method, "CONNECT");
    if (is_connect) {
        if (authority.count == 0 || scheme.count != 0 || path.count != 0) {
            hpack_decoded_free(&dec);
            xfree(fields);
            return -2;
        }
    } else {
        if (method.count == 0 || scheme.count == 0 || path.count == 0) {
            hpack_decoded_free(&dec);
            xfree(fields);
            return -2;
        }
        if (path.count > 0 && path.data[0] == '*') {
            if (!sv_ieq_str(method, "OPTIONS")) {
                hpack_decoded_free(&dec);
                xfree(fields);
                return -2;
            }
        } else if (path.count == 0 &&
                   (sv_ieq_str(scheme, "http") ||
                    sv_ieq_str(scheme, "https"))) {
            hpack_decoded_free(&dec);
            xfree(fields);
            return -2;
        }
    }

    memset(&s->req, 0, sizeof s->req);
    strbuf_init(&s->storage);
    strbuf_init(&s->body);
    s->recv_window = H2_INITIAL_WINDOW;
    s->send_window = c->client_init_window;
    s->req.method = http_method_from_sv(method);
    s->req.version = sv_from_cstr("HTTP/2.0");
    s->req.remote = c->remote;
    s->req.content_length = clv;

    // copy the request URI into storage first so target/path/query survive
    // the decoded block's release at the end of this function
    size_t path_off = s->storage.count;
    if (strbuf_append(&s->storage, path.data, path.count) < 0) {
        hpack_decoded_free(&dec);
        xfree(fields);
        h2_conn_error(c, HE_INTERNAL);
        return -1;
    }

    bool has_host = false;
    size_t cookie_first = (size_t)-1;
    size_t cookie_count = 0;
    for (size_t i = 0; i < fields_count; i++) {
        String_View fname = (String_View){ fields[i].name,
                                           fields[i].name_len };
        if (sv_ieq_str(fname, "host")) {
            has_host = true;
        }
        if (sv_ieq_str(fname, "cookie")) {
            if (cookie_count == 0) {
                cookie_first = i;
            }
            cookie_count++;
        }
    }
    bool merge_cookies = cookie_count > 1;

    // stage every string in storage first and only resolve the offsets into
    // String_Views afterwards, so a strbuf realloc cannot invalidate a view
    // that was already handed out
    typedef struct {
        size_t name_off;
        size_t name_len;
        size_t value_off;
        size_t value_len;
    } Slot;
    Slot *slots = xmalloc((fields_count + 1) * sizeof *slots);
    size_t slots_count = 0;
    for (size_t i = 0; i < fields_count; i++) {
        if (merge_cookies &&
            sv_ieq_str((String_View){ fields[i].name, fields[i].name_len },
                       "cookie") &&
            i != cookie_first) {
            continue;
        }
        Slot *sl = &slots[slots_count++];
        sl->name_off = s->storage.count;
        sl->name_len = fields[i].name_len;
        if (merge_cookies && i == cookie_first) {
            Strbuf merged;
            strbuf_init(&merged);
            strbuf_append(&merged, fields[i].value, fields[i].value_len);
            for (size_t k = 0; k < fields_count; k++) {
                if (k == i ||
                    fields[k].name_len != fields[i].name_len) {
                    continue;
                }
                if (!sv_ieq_str((String_View){ fields[k].name,
                                               fields[k].name_len },
                                "cookie")) {
                    continue;
                }
                strbuf_append_cstr(&merged, "; ");
                strbuf_append(&merged, fields[k].value,
                              fields[k].value_len);
            }
            if (strbuf_append(&s->storage, merged.items, merged.count) < 0) {
                strbuf_free(&merged);
                hpack_decoded_free(&dec);
                xfree(fields);
                xfree(slots);
                h2_conn_error(c, HE_INTERNAL);
                return -1;
            }
            strbuf_free(&merged);
            sl->value_off = sl->name_off;
            sl->value_len = s->storage.count - sl->name_off;
        } else {
            if (strbuf_append(&s->storage, fields[i].name,
                              fields[i].name_len) < 0 ||
                strbuf_append(&s->storage, fields[i].value,
                              fields[i].value_len) < 0) {
                hpack_decoded_free(&dec);
                xfree(fields);
                xfree(slots);
                h2_conn_error(c, HE_INTERNAL);
                return -1;
            }
            sl->value_off = sl->name_off + fields[i].name_len;
            sl->value_len = fields[i].value_len;
        }
    }
    xfree(fields);

    size_t host_off = 0;
    size_t host_len = 0;
    if (authority.count > 0 && !has_host) {
        host_off = s->storage.count;
        if (strbuf_append(&s->storage, "host", 4) < 0 ||
            strbuf_append(&s->storage, authority.data, authority.count) < 0) {
            hpack_decoded_free(&dec);
            xfree(slots);
            h2_conn_error(c, HE_INTERNAL);
            return -1;
        }
        host_len = authority.count;
    }

    // storage is final now; resolve offset bookkeeping into stable views
    char *mem = s->storage.items;
    size_t q = 0;
    while (q < path.count && path.data[q] != '?') {
        q++;
    }
    s->req.target = (String_View){ mem + path_off, path.count };
    s->req.path = (String_View){ mem + path_off, q };
    s->req.query = (String_View){
        mem + path_off + (q < path.count ? q + 1 : q),
        q < path.count ? path.count - q - 1 : 0
    };
    for (size_t i = 0; i < slots_count; i++) {
        da_append(&s->req.headers, ((Http_Header){
            (String_View){ mem + slots[i].name_off, slots[i].name_len },
            (String_View){ mem + slots[i].value_off, slots[i].value_len },
        }));
    }
    if (host_len > 0) {
        da_append(&s->req.headers, ((Http_Header){
            (String_View){ mem + host_off, 4 },
            (String_View){ mem + host_off + 4, host_len },
        }));
    }
    xfree(slots);
    // every view borrowed above (path, authority, fields) pointed into
    // dec.bytes; the request now owns its copies in s->storage, so the
    // decoded block is safe to release here at the very end
    hpack_decoded_free(&dec);
    return 0;
}

static int h2_send_header_block(H2_Conn *c, uint32_t stream_id,
                                const char *blk, size_t n, bool end_stream);

static void h2_pump(H2_Conn *c)
{
    if (c->error != HE_NO_ERROR) {
        return;
    }
    for (size_t i = 0; i < c->streams_count; i++) {
        H2_Stream *s = &c->streams[i];
        if (!s->open || s->reset || s->local_ended || !s->response_ready) {
            continue;
        }
        if (!s->headers_sent) {
            Http_Status st = s->res.status;
            bool status_no_body = st < 200 || st == 204 || st == 304;
            bool want_body = !status_no_body && !s->res.suppress_body &&
                             s->req.method != HTTP_HEAD;
            bool streaming = s->res.stream_fn != NULL ||
                             s->res.stream_chunk_count > 0;
            bool have_body = s->res.body.count > 0;
            bool last = !(want_body && (streaming || have_body));
            Strbuf blk;
            strbuf_init(&blk);
            if (hpack_encode_status(&blk, (uint16_t)st) < 0) {
                strbuf_free(&blk);
                h2_conn_error(c, HE_INTERNAL);
                break;
            }
            size_t at = 0;
            bool have_date = false;
            while (at < s->res.headers.count) {
                size_t line_end = at;
                while (line_end < s->res.headers.count &&
                       s->res.headers.items[line_end] != '\n') {
                    line_end++;
                }
                size_t line_len = line_end - at;
                if (line_len >= 2 &&
                    s->res.headers.items[line_end - 1] == '\r') {
                    line_len--;
                }
                const char *line = s->res.headers.items + at;
                size_t nl = 0;
                while (nl < line_len && line[nl] != ':') {
                    nl++;
                }
                if (nl > 0 && nl < line_len) {
                    if (sv_ieq_str((String_View){ line, nl }, "date")) {
                        have_date = true;
                    }
                    if (!conn_spec_name((String_View){ line, nl }) &&
                        !sv_ieq_str((String_View){ line, nl },
                                    "content-length")) {
                        String_View value = sv_trim(
                            (String_View){ line + nl + 1, line_len - nl - 1 });
                        char lower[256];
                        size_t ln = nl < sizeof lower ? nl : sizeof lower;
                        for (size_t k = 0; k < ln; k++) {
                            lower[k] = (char)tolower((unsigned char)line[k]);
                        }
                        if (hpack_encode_field(&blk,
                                               (String_View){ lower, ln },
                                               value) < 0) {
                            strbuf_free(&blk);
                            h2_conn_error(c, HE_INTERNAL);
                            break;
                        }
                    }
                }
                at = line_end < s->res.headers.count ? line_end + 1
                                                     : s->res.headers.count;
            }
            if (c->error != HE_NO_ERROR) {
                strbuf_free(&blk);
                break;
            }
            if (!have_date) {
                char d[64];
                http_date_rfc7231(time(NULL), d, sizeof d);
                if (hpack_encode_field(&blk, sv_from_cstr("date"),
                                       sv_from_cstr(d)) < 0) {
                    strbuf_free(&blk);
                    h2_conn_error(c, HE_INTERNAL);
                    break;
                }
            }
            if (h2_send_header_block(c, s->id, blk.items, blk.count,
                                     last) != 0) {
                strbuf_free(&blk);
                h2_conn_error(c, HE_INTERNAL);
                break;
            }
            strbuf_free(&blk);
s->headers_sent = true;
            s->body_off = 0;
            if (last) {
                s->local_ended = true;
                h2_mark_response_done(c, s);
                continue;
            }
        }
        while (!s->local_ended && c->error == HE_NO_ERROR) {
            long long room = c->conn_send_window;
            if (s->send_window < room) {
                room = s->send_window;
            }
            const uint8_t *src = NULL;
            size_t len = 0;
            bool eof = false;
            int r = h2_next_data(s, room, &src, &len, &eof);
            if (r == 0) {
                break;
            }
            if (len > 0 && room <= 0) {
                break;
            }
            h2_send_data(c, s, src, len, eof);
            if (eof) {
                s->local_ended = true;
                h2_mark_response_done(c, s);
                break;
            }
        }
    }
}

static void h2_free_stream(H2_Conn *c, H2_Stream *s)
{
    http_request_free(&s->req);
    http_response_free(&s->res);
    strbuf_free(&s->body);
    strbuf_free(&s->storage);
    size_t idx = (size_t)(s - c->streams);
    if (idx + 1 < c->streams_count) {
        c->streams[idx] = c->streams[c->streams_count - 1];
    }
    c->streams_count--;
}

static void h2_sweep(H2_Conn *c)
{
    for (size_t i = c->streams_count; i-- > 0;) {
        H2_Stream *s = &c->streams[i];
        if (s->reset ||
            (s->local_ended && s->remote_ended)) {
            h2_free_stream(c, s);
        }
    }
}

static bool h2_client_goaway_done(const H2_Conn *c)
{
    if (!c->client_goaway) {
        return false;
    }
    for (size_t i = 0; i < c->streams_count; i++) {
        if (c->streams[i].open) {
            return false;
        }
    }
    return true;
}

static void h2_on_data(H2_Conn *c, const H2_Frame *f)
{
    if (f->stream_id == 0) {
        h2_conn_error(c, HE_PROTOCOL);
        return;
    }
    const uint8_t *p = f->payload;
    uint32_t plen = f->len;
    uint32_t pad = 0;
    if (f->flags & H2F_PADDED) {
        if (f->len < 1) {
            h2_conn_error(c, HE_PROTOCOL);
            return;
        }
        pad = *p;
        p++;
        plen--;
        if (pad >= plen) {
            h2_conn_error(c, HE_PROTOCOL);
            return;
        }
    }
    if ((uint64_t)f->len > (uint64_t)c->conn_recv_window) {
        h2_conn_error(c, HE_FLOW);
        return;
    }
    c->conn_recv_window -= (long long)f->len;
    {
        uint8_t inc[4];
        inc[0] = (uint8_t)(f->len >> 24);
        inc[1] = (uint8_t)(f->len >> 16);
        inc[2] = (uint8_t)(f->len >> 8);
        inc[3] = (uint8_t)f->len;
        h2_send_frame(c, H2_WINDOW, 0, 0, inc, sizeof inc);
    }
    size_t si = h2_find_stream(c, f->stream_id);
    if (si == (size_t)-1) {
        if (f->stream_id > c->last_stream_id) {
            h2_conn_error(c, HE_PROTOCOL);
        } else {
            h2_send_stream_error(c, f->stream_id, HE_STREAM_CLOSED);
        }
        return;
    }
    H2_Stream *s = &c->streams[si];
    if (s->reset) {
        return;
    }
    if ((long long)f->len > s->recv_window) {
        s->recv_window -= (long long)f->len;
        h2_send_stream_error(c, s->id, HE_FLOW);
        s->reset = true;
        return;
    }
    s->recv_window -= (long long)f->len;
    {
        uint8_t inc[4];
        inc[0] = (uint8_t)(f->len >> 24);
        inc[1] = (uint8_t)(f->len >> 16);
        inc[2] = (uint8_t)(f->len >> 8);
        inc[3] = (uint8_t)f->len;
        h2_send_frame(c, H2_WINDOW, 0, s->id, inc, sizeof inc);
    }
    if (s->remote_ended) {
        h2_send_stream_error(c, s->id, HE_STREAM_CLOSED);
        s->reset = true;
        return;
    }
    uint32_t data_len = plen - pad;
    s->req_bytes += data_len;
    if (!s->body_too_big) {
        if (s->body.count + data_len > c->max_body) {
            s->body_too_big = true;
        } else {
            strbuf_append(&s->body, (const char *)p, data_len);
        }
    }
    if (f->flags & H2F_END_STREAM) {
        s->remote_ended = true;
        if (s->req.content_length >= 0 &&
            (long long)s->req_bytes != s->req.content_length) {
            h2_send_stream_error(c, s->id, HE_PROTOCOL);
            s->reset = true;
        }
    }
}

static void h2_on_priority(H2_Conn *c, const H2_Frame *f)
{
    if (f->stream_id == 0) {
        h2_conn_error(c, HE_PROTOCOL);
        return;
    }
    if (f->len != 5) {
        if (h2_find_stream(c, f->stream_id) != (size_t)-1) {
            h2_send_stream_error(c, f->stream_id, HE_FRAME_SIZE);
        } else {
            h2_conn_error(c, HE_PROTOCOL);
        }
    }
}

static void h2_on_rst(H2_Conn *c, const H2_Frame *f)
{
    if (f->len != 4) {
        h2_conn_error(c, HE_FRAME_SIZE);
        return;
    }
    if (f->stream_id == 0) {
        h2_conn_error(c, HE_PROTOCOL);
        return;
    }
    size_t si = h2_find_stream(c, f->stream_id);
    if (si == (size_t)-1) {
        if (f->stream_id > c->last_stream_id) {
            h2_conn_error(c, HE_PROTOCOL);
        }
        return;
    }
    c->streams[si].reset = true;
}

static void h2_on_settings(H2_Conn *c, const H2_Frame *f)
{
    if (f->stream_id != 0) {
        h2_conn_error(c, HE_PROTOCOL);
        return;
    }
    if (f->flags & H2F_ACK) {
        if (f->len != 0) {
            h2_conn_error(c, HE_FRAME_SIZE);
        }
        return;
    }
    c->got_client_settings = true;
    if (h2_apply_settings(c, f->payload, f->len) < 0) {
        return;
    }
    h2_settings_ack(c);
}

static void h2_on_ping(H2_Conn *c, const H2_Frame *f)
{
    if (f->len != 8) {
        h2_conn_error(c, HE_FRAME_SIZE);
        return;
    }
    if (f->stream_id != 0) {
        h2_conn_error(c, HE_PROTOCOL);
        return;
    }
    if (!(f->flags & H2F_ACK)) {
        h2_send_frame(c, H2_PING, H2F_ACK, 0, f->payload, f->len);
    }
}

static void h2_on_goaway(H2_Conn *c, const H2_Frame *f)
{
    if (f->len < 8) {
        h2_conn_error(c, HE_FRAME_SIZE);
        return;
    }
    if (f->stream_id != 0) {
        h2_conn_error(c, HE_PROTOCOL);
        return;
    }
    c->client_goaway = true;
    c->client_goaway_last = rd32(f->payload) & 0x7fffffffu;
}

static void h2_on_window(H2_Conn *c, const H2_Frame *f)
{
    if (f->len != 4) {
        h2_conn_error(c, HE_FRAME_SIZE);
        return;
    }
    uint32_t inc = rd32(f->payload) & 0x7fffffffu;
    if (inc == 0) {
        if (f->stream_id == 0) {
            h2_conn_error(c, HE_PROTOCOL);
        } else {
            h2_send_stream_error(c, f->stream_id, HE_PROTOCOL);
        }
        return;
    }
    if (f->stream_id == 0) {
        c->conn_send_window += (long long)inc;
        if (c->conn_send_window > 0x7fffffffLL) {
            h2_conn_error(c, HE_FLOW);
        }
        return;
    }
    size_t si = h2_find_stream(c, f->stream_id);
    if (si == (size_t)-1) {
        if (f->stream_id > c->last_stream_id) {
            h2_conn_error(c, HE_PROTOCOL);
        }
        return;
    }
    H2_Stream *s = &c->streams[si];
    if (s->reset) {
        return;
    }
    s->send_window += (long long)inc;
    if (s->send_window > 0x7fffffffLL) {
        h2_send_stream_error(c, s->id, HE_FLOW);
        s->reset = true;
    }
}

static int h2_send_header_block(H2_Conn *c, uint32_t stream_id,
                                const char *blk, size_t n, bool end_stream)
{
    size_t off = 0;
    do {
        size_t take = n - off;
        if (take > H2_MAX_FRAME_SIZE) {
            take = H2_MAX_FRAME_SIZE;
        }
        bool first = off == 0;
        bool last_frag = off + take == n;
        H2_Frame_Type type = first ? H2_HEADERS : H2_CONT;
        uint8_t flags = 0;
        if (last_frag) {
            flags |= H2F_END_HEADERS;
        }
        if (first && end_stream) {
            flags |= H2F_END_STREAM;
        }
        if (h2_send_frame(c, type, flags, stream_id, blk + off, take) != 0) {
            return -1;
        }
        off += take;
    } while (off < n);
    if (n == 0) {
        uint8_t flags = H2F_END_HEADERS;
        if (end_stream) {
            flags |= H2F_END_STREAM;
        }
        if (h2_send_frame(c, H2_HEADERS, flags, stream_id, NULL, 0) != 0) {
            return -1;
        }
    }
    return 0;
}

static void h2_on_header_block_finish(H2_Conn *c, uint32_t id, bool es,
                                      const char *blk, size_t n);
static void h2_on_cont(H2_Conn *c, const H2_Frame *f)
{
    if (c->hb_stream == 0) {
        h2_conn_error(c, HE_PROTOCOL);
        return;
    }
    if (f->stream_id != c->hb_stream) {
        h2_conn_error(c, HE_PROTOCOL);
        return;
    }
    if (!c->hb_abort && c->hb.count + f->len > H2_HEADER_BLOCK_CAP) {
        c->hb_abort = true;
        h2_send_stream_error(c, f->stream_id, HE_CANCEL);
    }
    if (!c->hb_abort) {
        strbuf_append(&c->hb, (const char *)f->payload, f->len);
    }
    if (f->flags & H2F_END_HEADERS) {
        uint32_t id = c->hb_stream;
        bool es = c->hb_end_stream;
        bool abort = c->hb_abort;
        String_View data = c->hb.count > 0
                               ? (String_View){ c->hb.items, c->hb.count }
                               : (String_View){ NULL, 0 };
        c->hb_stream = 0;
        c->hb_end_stream = false;
        c->hb_abort = false;
        c->hb.count = 0;
        if (!abort) {
            h2_on_header_block_finish(c, id, es, data.data, data.count);
        }
    }
}

static void h2_on_headers_start(H2_Conn *c, const H2_Frame *f)
{
    if (f->stream_id == 0) {
        h2_conn_error(c, HE_PROTOCOL);
        return;
    }
    c->hb_stream = f->stream_id;
    c->hb_end_stream = (f->flags & H2F_END_STREAM) != 0;
    c->hb_abort = false;
    c->hb.count = 0;
    const uint8_t *p = f->payload;
    uint32_t plen = f->len;
    uint32_t pad = 0;
    if (f->flags & H2F_PADDED) {
        if (f->len < 1) {
            h2_conn_error(c, HE_PROTOCOL);
            return;
        }
        pad = *p;
        p++;
        plen--;
    }
    if (f->flags & H2F_PRIORITY) {
        if (plen < 5) {
            h2_conn_error(c, HE_PROTOCOL);
            return;
        }
        p += 5;
        plen -= 5;
    }
    if (pad > plen) {
        h2_conn_error(c, HE_PROTOCOL);
        return;
    }
    uint32_t frag = plen - pad;
    if (frag > H2_HEADER_BLOCK_CAP) {
        h2_conn_error(c, HE_CALM);
        return;
    }
    strbuf_append(&c->hb, (const char *)p, frag);
    if (f->flags & H2F_END_HEADERS) {
        uint32_t id = c->hb_stream;
        bool es = c->hb_end_stream;
        const char *blk = c->hb.items;
        size_t bn = c->hb.count;
        c->hb_stream = 0;
        c->hb_end_stream = false;
        c->hb_abort = false;
        c->hb.count = 0;
        h2_on_header_block_finish(c, id, es, blk, bn);
    }
}

static void h2_on_header_block_finish(H2_Conn *c, uint32_t id, bool es,
                                      const char *blk, size_t n)
{
    if ((id & 1u) == 0) {
        h2_conn_error(c, HE_PROTOCOL);
        return;
    }
    if (c->client_goaway && id > c->client_goaway_last) {
        h2_send_stream_error(c, id, HE_REFUSED);
        return;
    }
    if (id <= c->last_stream_id) {
        h2_conn_error(c, HE_PROTOCOL);
        return;
    }
    if (h2_find_stream(c, id) != (size_t)-1) {
        h2_conn_error(c, HE_PROTOCOL);
        return;
    }
    if (c->streams_count == c->streams_cap) {
        c->streams_cap = c->streams_cap ? c->streams_cap * 2 : 16;
        c->streams = xrealloc(c->streams, c->streams_cap * sizeof *c->streams);
    }
    H2_Stream *s = &c->streams[c->streams_count];
    memset(s, 0, sizeof *s);
    s->id = id;
    s->open = true;
    s->remote_ended = es;
    int rc = h2_request_from_block(c, blk, n, s);
    if (rc == -1) {
        memset(s, 0, sizeof *s);
        return;
    }
    if (rc == -2) {
        memset(s, 0, sizeof *s);
        h2_send_stream_error(c, id, HE_PROTOCOL);
        return;
    }
    c->streams_count++;
    c->last_stream_id = id;
    if (c->streams_count > H2_MAX_CONCURRENT_STREAMS) {
        h2_send_stream_error(c, id, HE_REFUSED);
        H2_Stream *new_s = &c->streams[c->streams_count - 1];
        h2_free_stream(c, new_s);
    }
}

static void h2_process_frame(H2_Conn *c, const H2_Frame *f)
{
    if (!c->got_client_settings) {
        if (f->type != H2_SETTINGS || (f->flags & H2F_ACK)) {
            h2_conn_error(c, HE_PROTOCOL);
            return;
        }
    }
    if (c->hb_stream != 0) {
        if (f->type != H2_CONT || f->stream_id != c->hb_stream) {
            h2_conn_error(c, HE_PROTOCOL);
            return;
        }
    }
    switch (f->type) {
    case H2_DATA:
        h2_on_data(c, f);
        break;
    case H2_HEADERS:
        h2_on_headers_start(c, f);
        break;
    case H2_CONT:
        h2_on_cont(c, f);
        break;
    case H2_PRIORITY:
        h2_on_priority(c, f);
        break;
    case H2_RST:
        h2_on_rst(c, f);
        break;
    case H2_SETTINGS:
        h2_on_settings(c, f);
        break;
    case H2_PING:
        h2_on_ping(c, f);
        break;
    case H2_GOAWAY:
        h2_on_goaway(c, f);
        break;
    case H2_WINDOW:
        h2_on_window(c, f);
        break;
    case H2_PUSH_PROMISE:
        h2_conn_error(c, HE_PROTOCOL);
        break;
    default:
        break;
    }
}

static void h2_add_upgrade_stream(H2_Conn *c, Http_Request *src)
{
    if (c->streams_count == c->streams_cap) {
        c->streams_cap = c->streams_cap ? c->streams_cap * 2 : 8;
        c->streams = xrealloc(c->streams, c->streams_cap * sizeof *c->streams);
    }
    H2_Stream *s = &c->streams[c->streams_count];
    memset(s, 0, sizeof *s);
    s->id = 1;
    s->open = true;
    s->remote_ended = true;
    s->recv_window = H2_INITIAL_WINDOW;
    s->send_window = c->client_init_window;
    strbuf_init(&s->storage);
    strbuf_init(&s->body);
    s->req.method = src->method;
    s->req.version = sv_from_cstr("HTTP/2.0");
    s->req.remote = c->remote;

    size_t path_off = s->storage.count;
    strbuf_append(&s->storage, src->path.data, src->path.count);
    size_t q = 0;
    while (q < src->path.count && src->path.data[q] != '?') {
        q++;
    }
    s->req.target = (String_View){ s->storage.items + path_off,
                                   src->path.count };
    s->req.path = (String_View){ s->storage.items + path_off, q };
    s->req.query = (String_View){ s->storage.items + path_off + q,
                                  q < src->path.count
                                      ? src->path.count - q - 1
                                      : 0 };
    if (q < src->path.count) {
        s->req.query.data++;
    }

    bool has_host = false;
    for (size_t i = 0; i < src->headers.count; i++) {
        if (sv_ieq_str(src->headers.items[i].key, "host")) {
            has_host = true;
        }
    }
    for (size_t i = 0; i < src->headers.count; i++) {
        Http_Header h = src->headers.items[i];
        if (conn_spec_name(h.key) ||
            sv_ieq_str(h.key, "http2-settings")) {
            continue;
        }
        size_t name_off = s->storage.count;
        strbuf_append(&s->storage, h.key.data, h.key.count);
        strbuf_append(&s->storage, h.value.data, h.value.count);
        da_append(&s->req.headers, ((Http_Header){
            (String_View){ s->storage.items + name_off, h.key.count },
            (String_View){ s->storage.items + name_off + h.key.count,
                           h.value.count },
        }));
    }
    if (!has_host) {
        const String_View *authority = http_request_get_header(src,
                                                               ":authority");
        if (authority == NULL) {
            authority = http_request_get_header(src, "host");
        }
        if (authority != NULL) {
            size_t name_off = s->storage.count;
            strbuf_append(&s->storage, "host", 4);
            strbuf_append(&s->storage, authority->data, authority->count);
            da_append(&s->req.headers, ((Http_Header){
                (String_View){ s->storage.items + name_off, 4 },
                (String_View){ s->storage.items + name_off + 4,
                               authority->count },
            }));
        }
    }

    if (src->body.count > 0) {
        strbuf_append(&s->body, src->body.data, src->body.count);
    }
    s->req_bytes = src->body.count;
    s->req.content_length = src->content_length >= 0
                                ? src->content_length
                                : (long long)src->body.count;
    s->req.body_heap = NULL;
    s->req.headers.count = s->req.headers.count;
    c->streams_count++;
    c->last_stream_id = s->id;
}

typedef struct {
    size_t name_off;
    size_t name_len;
    size_t value_off;
    size_t value_len;
} H2_Slot;

static long h2_b64url_decode(const char *s, size_t n, uint8_t *out,
                             size_t cap)
{
    static int8_t tab[256];
    static bool built = false;
    if (!built) {
        memset(tab, -1, sizeof tab);
        for (int i = 0; i < 26; i++) {
            tab[(int)('A' + i)] = (int8_t)i;
            tab[(int)('a' + i)] = (int8_t)(26 + i);
        }
        for (int i = 0; i < 10; i++) {
            tab[(int)('0' + i)] = (int8_t)(52 + i);
        }
        tab[(int)'-'] = 62;
        tab[(int)'_'] = 63;
        built = true;
    }
    size_t out_n = 0;
    uint32_t acc = 0;
    int nbits = 0;
    for (size_t i = 0; i < n; i++) {
        char ch = s[i];
        if (ch == '=') {
            break;
        }
        int v = ch < 0 ? -1 : tab[(unsigned char)ch];
        if (v < 0) {
            return -1;
        }
        acc = (acc << 6) | (uint32_t)v;
        nbits += 6;
        if (nbits >= 8) {
            nbits -= 8;
            uint8_t byte = (uint8_t)(acc >> nbits);
            if (out_n >= cap) {
                return -1;
            }
            out[out_n++] = byte;
        }
    }
    return (long)out_n;
}

void h2_serve(Socket_Handle fd, void *ssl, Http_Handler_Fn handler,
              void *user_data, const Http_Server_Config *cfg, Read_Buffer *rb,
              size_t consumed, Http_Request *upgrade_req, String_View remote,
              unsigned long default_timeout_ms)
{
    H2_Conn c;
    memset(&c, 0, sizeof c);
    c.fd = fd;
    c.ssl = ssl;
    c.handler = handler;
    c.user_data = user_data;
    c.cfg = cfg;
    c.remote = remote;
    c.max_body = cfg != NULL && cfg->max_body > 0 ? cfg->max_body
                                                  : H2_BUFFER_CAP;
    c.timeout_ms = default_timeout_ms;
    c.conn_recv_window = H2_INITIAL_WINDOW;
    c.conn_send_window = H2_INITIAL_WINDOW;
    c.client_init_window = H2_INITIAL_WINDOW;
    hpack_init(&c.hpack);
    strbuf_init(&c.hb);
    rb_init(&c.rb, H2_BUFFER_CAP);

    if (rb != NULL && rb->count > consumed) {
        size_t left = rb->count - consumed;
        memcpy(c.rb.data, rb->data + consumed, left);
        c.rb.count = left;
    }
    net_set_timeout(fd, default_timeout_ms);

    if (upgrade_req != NULL) {
        static const char upgrade101[] =
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Connection: Upgrade\r\n"
            "Upgrade: h2c\r\n\r\n";
        if (h2_write(&c, upgrade101, sizeof upgrade101 - 1) != 0) {
            goto out;
        }
        const String_View *hs =
            http_request_get_header(upgrade_req, "http2-settings");
        if (hs != NULL) {
            uint8_t *payload = xmalloc(hs->count ? hs->count : 1);
            long dl = h2_b64url_decode(hs->data, hs->count, payload,
                                       hs->count);
            if (dl < 0 || (uint32_t)dl % 6 != 0) {
                xfree(payload);
                static const char bad400[] =
                    "HTTP/1.1 400 Bad Request\r\n"
                    "Content-Length: 0\r\nConnection: close\r\n\r\n";
                h2_write(&c, bad400, sizeof bad400 - 1);
                goto out;
            }
            h2_apply_settings(&c, payload, (uint32_t)dl);
            xfree(payload);
            if (c.error != HE_NO_ERROR) {
                goto out;
            }
        }
        h2_add_upgrade_stream(&c, upgrade_req);
    }

    h2_send_settings(&c);
    if (c.error != HE_NO_ERROR) {
        goto out;
    }

    for (;;) {
        // over TLS, ALPN already identifies the stream as h2, yet nghttp2
        // clients (curl) still emit the 24-octet "PRI * HTTP/2.0" preface
        // before their first SETTINGS (RFC 7540 section 3.5 scopes the
        // preface to h2c, but several clients send it unconditionally). skip
        // it so frame modulo-parsing sees SETTINGS as the first frame. the
        // h2c paths handed this buffer over with the preface already eaten,
        // so the match can only fire on the TLS path.
        if (c.rb.count >= H2_MAGIC_LEN &&
            memcmp(c.rb.data, H2_MAGIC, H2_MAGIC_LEN) == 0) {
            rb_discard(&c.rb, H2_MAGIC_LEN);
        }
        h2_dispatch_ready(&c);
        h2_pump(&c);
        h2_sweep(&c);
        if (c.error != HE_NO_ERROR) {
            break;
        }
        if (h2_client_goaway_done(&c)) {
            break;
        }
        for (;;) {
            H2_Frame f;
            int r = h2_frame_peek(&c, &f);
            if (r < 0) {
                break;
            }
            if (r == 0) {
                break;
            }
            h2_process_frame(&c, &f);
            rb_discard(&c.rb, H2_FRAME_HDR + f.len);
            if (c.error != HE_NO_ERROR) {
                break;
            }
            h2_dispatch_ready(&c);
            h2_pump(&c);
            h2_sweep(&c);
        }
        if (c.error != HE_NO_ERROR) {
            break;
        }
        if (h2_client_goaway_done(&c)) {
            break;
        }
        if (h2_fill(&c) != 0) {
            break;
        }
    }

out:
    h2_send_goaway(&c, c.last_stream_id,
                   c.error != HE_NO_ERROR ? c.error : HE_NO_ERROR);
    while (c.streams_count > 0) {
        h2_free_stream(&c, &c.streams[0]);
    }
    xfree(c.streams);
    hpack_free(&c.hpack);
    strbuf_free(&c.hb);
    rb_free(&c.rb);
}

bool h2_is_upgrade_request(const Http_Request *req)
{
    if (!header_has_token(req, "connection", "upgrade")) {
        return false;
    }
    if (!header_has_token(req, "connection", "http2-settings")) {
        return false;
    }
    const String_View *up =
        http_request_get_header((Http_Request *)req, "upgrade");
    if (up == NULL || !tok_in(*up, "h2c")) {
        return false;
    }
    size_t settings_fields = 0;
    for (size_t i = 0; i < req->headers.count; i++) {
        if (sv_ieq_str(req->headers.items[i].key, "http2-settings")) {
            settings_fields++;
        }
    }
    return settings_fields == 1;
}