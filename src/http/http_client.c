#include "http_client.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "http.h"
#include "net.h"
#include "strbuf.h"
#include "uri.h"
#include "xmem.h"

// a hostile or misbehaving peer must not hand us an unbounded body or head
static const unsigned long long CLIENT_MAX_BODY = 32ull * 1024 * 1024;
static const size_t CLIENT_HEAD_CAP = 64 * 1024;
// how long a request waits for data by default (milliseconds)
static const unsigned long CLIENT_TIMEOUT_MS = 10000;

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static void set_err(char **err, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char msg[512];
    int n = vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (n < 0) {
        n = 0;
    }
    *err = xmalloc((size_t)n + 1);
    memcpy(*err, msg, (size_t)n + 1);
}

static void fail(Http_Client_Result *out, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char msg[512];
    int n = vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (n < 0) {
        n = 0;
    }
    out->error = xmalloc((size_t)n + 1);
    memcpy(out->error, msg, (size_t)n + 1);
}

static bool idempotent(Http_Method m)
{
    return m == HTTP_GET || m == HTTP_HEAD || m == HTTP_OPTIONS;
}

// ---- response head helpers over the raw head text ----

static bool header_is(String_View line, const char *name)
{
    size_t n = strlen(name);
    if (line.count < n + 2 || line.data[n] != ':') {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (tolower((unsigned char)line.data[i]) != tolower((unsigned char)name[i])) {
            return false;
        }
    }
    return true;
}

// lowercase-compare a comma/space separated value against one token
static bool value_has_token(String_View value, const char *token)
{
    size_t n = strlen(token);
    String_View rest = value;
    while (rest.count > 0) {
        String_View part = sv_trim(sv_chop_by_delim(&rest, ','));
        if (part.count == n) {
            bool same = true;
            for (size_t i = 0; i < n; i++) {
                if (tolower((unsigned char)part.data[i]) != tolower((unsigned char)token[i])) {
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

static bool head_has_token(String_View head, const char *name, const char *token)
{
    String_View rest = head;
    while (rest.count > 0) {
        String_View line = sv_trim_right(sv_chop_by_delim(&rest, '\n'));
        if (line.count == 0) {
            break;
        }
        if (header_is(line, name)) {
            String_View value = sv_trim((String_View){line.data + strlen(name) + 1,
                                                      line.count - strlen(name) - 1});
            if (value_has_token(value, token)) {
                return true;
            }
        }
    }
    return false;
}

// first value of a header, trimmed, lowercased name lookup. false when absent.
static bool head_get_value(String_View head, const char *name, String_View *value)
{
    size_t n = strlen(name);
    String_View rest = head;
    while (rest.count > 0) {
        String_View line = sv_trim_right(sv_chop_by_delim(&rest, '\n'));
        if (line.count == 0) {
            break;
        }
        if (header_is(line, name)) {
            *value = sv_trim((String_View){line.data + n + 1, line.count - n - 1});
            return true;
        }
    }
    return false;
}

static long long head_content_length(String_View head)
{
    String_View value;
    if (head_get_value(head, "content-length", &value)) {
        long long n;
        if (sv_to_i64(value, &n) && n >= 0) {
            return n;
        }
        return -1; // present but garbage, do not guess
    }
    return -1;
}

static int parse_status_line(String_View head, Http_Status *status)
{
    // first line: "HTTP/1.1 200 OK"
    String_View line = sv_trim_right(sv_chop_by_delim(&head, '\n'));
    const char *p = memchr(line.data, ' ', line.count);
    if (p == NULL || line.count - (size_t)(p - line.data) < 4) {
        return -1;
    }
    for (int i = 1; i <= 3; i++) {
        if (!isdigit((unsigned char)p[i])) {
            return -1;
        }
    }
    int code = (p[1] - '0') * 100 + (p[2] - '0') * 10 + (p[3] - '0');
    if (code < 100 || code > 599) {
        return -1;
    }
    *status = (Http_Status)code;
    return 0;
}

// ---- incremental chunked decoder ----
//
// the previous decoder had to see the whole body before starting, which is
// fine on a Connection: close response but impossible on a keep-alive one
// (the connection never ends). this one is resumable: it consumes as much of
// the buffer as it can and asks for more, keeping only the framing scrap it
// has not finished reading.

typedef struct {
    int phase;      // 0 size line, 1 payload, 2 terminator CRLF, 3 trailers
    unsigned long long pending; // payload bytes still owed
    size_t crlf_seen;      // bytes of the post-payload "\r\n" collected
    char line[266];        // partial size or trailer line
    size_t line_len;
} Chunk_Reader;

static void chunk_bad(char **err, const char *why)
{
    if (err != NULL) {
        *err = xmalloc(strlen(why) + 1);
        strcpy(*err, why);
    }
}

// consumes from buf[*pos, end). returns 1 when the whole framing is done, 0
// when more bytes are needed, -1 on malformed input (err names the problem).
static int chunk_feed(Chunk_Reader *cr, const char *buf, size_t *pos, size_t end,
                      Strbuf *body, char **err)
{
    while (*pos < end) {
        switch (cr->phase) {
        case 0: // size line up to and including '\n'
            while (*pos < end && buf[*pos] != '\n') {
                if (cr->line_len >= sizeof(cr->line)) {
                    chunk_bad(err, "chunk size line too long");
                    return -1;
                }
                cr->line[cr->line_len++] = buf[(*pos)++];
            }
            if (*pos >= end) {
                return 0; // newline has not arrived yet
            }
            (*pos)++;
            {
                unsigned long long n = 0;
                int digits = 0;
                for (size_t i = 0; i < cr->line_len; i++) {
                    // the size line still carries its trailing '\r' and may
                    // have a ";ext" suffix; both end the digits here
                    if (cr->line[i] == '\r' || cr->line[i] == ';') {
                        break;
                    }
                    int d = hex_digit(cr->line[i]);
                    if (d < 0) {
                        chunk_bad(err, "malformed chunk size");
                        return -1;
                    }
                    n = n * 16 + (unsigned)d;
                    digits++;
                    if (n > CLIENT_MAX_BODY) {
                        chunk_bad(err, "chunk claims more than the cap");
                        return -1;
                    }
                }
                if (digits == 0) {
                    chunk_bad(err, "missing chunk size");
                    return -1;
                }
                if (n == 0) {
                    cr->phase = 3; // terminal chunk, drain trailers
                } else {
                    cr->phase = 1;
                    cr->pending = n;
                }
                cr->line_len = 0;
            }
            break;
        case 1: // payload
        {
            size_t take = end - *pos;
            if ((unsigned long long)take > cr->pending) {
                take = (size_t)cr->pending;
            }
            if ((unsigned long long)(body->count + take) > CLIENT_MAX_BODY) {
                chunk_bad(err, "body too large");
                return -1;
            }
            strbuf_append(body, buf + *pos, take);
            *pos += take;
            cr->pending -= take;
            if (cr->pending == 0) {
                cr->phase = 2;
                cr->crlf_seen = 0;
            }
            break;
        }
        case 2: // the "\r\n" that closes each payload
            while (cr->crlf_seen < 2 && *pos < end) {
                char want = cr->crlf_seen == 0 ? '\r' : '\n';
                if (buf[(*pos)++] != want) {
                    chunk_bad(err, "chunk payload not CRLF terminated");
                    return -1;
                }
                cr->crlf_seen++;
            }
            if (cr->crlf_seen == 2) {
                cr->phase = 0;
            }
            break;
        case 3: // trailer lines up to the blank line that ends the framing
            while (*pos < end && buf[*pos] != '\n') {
                if (cr->line_len >= sizeof(cr->line)) {
                    chunk_bad(err, "trailer line too long");
                    return -1;
                }
                cr->line[cr->line_len++] = buf[(*pos)++];
            }
            if (*pos >= end) {
                return 0;
            }
            (*pos)++;
            // blank ("\r\n" or bare "\n") means done; anything else is a
            // trailer header we can ignore, so keep scanning.
            if (cr->line_len == 0 ||
                (cr->line_len == 1 && cr->line[0] == '\r')) {
                return 1;
            }
            cr->line_len = 0;
            break;
        }
    }
    return 0;
}

// ---- the persistent client ----

struct Http_Client {
    Socket_Handle sock; // -1 when closed
    char host[256];
    int port;
    unsigned long timeout_ms;
    size_t opens; // TCP connections opened since birth, for diagnostics
    int redirects; // 3xx hops to chase before giving up
    Strbuf rbuf;  // bytes already read waiting to be consumed
    size_t rstart; // how many of rbuf.count belong to a finished request
};

Http_Client *http_client_open(const char *base_url)
{
    Http_Client *c = xcalloc(1, sizeof *c);
    c->sock = -1;
    c->timeout_ms = CLIENT_TIMEOUT_MS;
    c->redirects = 5;
    strbuf_init(&c->rbuf);
    if (base_url != NULL) {
        Uri u;
        if (uri_parse(sv_from_cstr(base_url), &u) != 0 ||
            !uri_scheme_is(&u, "http") || u.host.count == 0 ||
            u.host.count >= sizeof(c->host)) {
            http_client_close(c);
            return NULL;
        }
        int port = uri_default_port(u.scheme);
        if (u.port.count > 0) {
            long long pn;
            if (!sv_to_i64(u.port, &pn) || pn <= 0 || pn > 65535) {
                http_client_close(c);
                return NULL;
            }
            port = (int)pn;
        }
        memcpy(c->host, u.host.data, u.host.count);
        c->host[u.host.count] = '\0';
        c->port = port;
    }
    return c;
}

void http_client_close(Http_Client *c)
{
    if (c == NULL) {
        return;
    }
    if (c->sock != -1) {
        net_close(c->sock);
    }
    strbuf_free(&c->rbuf);
    xfree(c);
}

int http_client_set_timeout(Http_Client *c, unsigned long ms)
{
    if (c == NULL) {
        return -1;
    }
    c->timeout_ms = ms;
    return 0;
}

void http_client_set_redirects(Http_Client *c, int max_redirects)
{
    if (c != NULL) {
        c->redirects = max_redirects < 0 ? 0 : max_redirects;
    }
}

bool http_client_keepalive_active(const Http_Client *c)
{
    return c != NULL && c->sock != -1;
}

size_t http_client_connection_opens(const Http_Client *c)
{
    return c == NULL ? 0 : c->opens;
}

// pulls one socket read into the read-ahead buffer. 1 = bytes arrived, 0 =
// orderly close, -1 = error/timout (err names it).
static int refill(Http_Client *c, char **err)
{
    char chunk[8192];
    long n = net_recv(c->sock, chunk, sizeof chunk);
    if (n == NET_READ_TIMEOUT) {
        set_err(err, "timed out waiting for the response");
        return -1;
    }
    if (n < 0) {
        set_err(err, "read failed: %s", net_error_string());
        return -1;
    }
    if (n == 0) {
        return 0;
    }
    strbuf_append(&c->rbuf, chunk, (size_t)n);
    return 1;
}

static size_t find_head_sep(const char *data, size_t from, size_t count)
{
    for (size_t i = from; i + 3 < count; i++) {
        if (data[i] == '\r' && data[i + 1] == '\n' &&
            data[i + 2] == '\r' && data[i + 3] == '\n') {
            return i;
        }
    }
    return SIZE_MAX;
}

static bool too_big(const Strbuf *b)
{
    return (unsigned long long)b->count > CLIENT_MAX_BODY + CLIENT_HEAD_CAP;
}

// consumes one complete response from the wire into out. *conn_dead is set
// when the framing makes the connection unreusable for the next request, and
// *touched when any response byte arrived (a retry may only resend when
// nothing came back at all).
static int read_response(Http_Client *c, Http_Method method,
                         Http_Client_Result *out, bool *conn_dead, bool *touched)
{
    *conn_dead = false;
    *touched = false;

    // collect the head
    size_t sep;
    for (;;) {
        sep = find_head_sep(c->rbuf.items, c->rstart, c->rbuf.count);
        if (sep != SIZE_MAX) {
            break;
        }
        if (c->rbuf.count - c->rstart > CLIENT_HEAD_CAP) {
            fail(out, "response head larger than %zu bytes", CLIENT_HEAD_CAP);
            return -1;
        }
        char *err = NULL;
        int r = refill(c, &err);
        if (r == 1) {
            *touched = true;
            continue;
        }
        if (r == 0 && c->rbuf.count - c->rstart == 0) {
            // the connection was dropped before it answered anything: a dead
            // keep-alive socket. retry may resend, so report it as untouched.
            fail(out, "server closed the connection without a response");
        } else if (err != NULL) {
            fail(out, "%s", err);
        } else {
            fail(out, "server closed mid-head");
        }
        xfree(err);
        return -1;
    }

    // the head properly ends with the last header line's CRLF (sep points at the
// first CR of the blank-line terminator, whose first "\r\n" closes the last
// field), so the stored text keeps every line's terminator
    size_t head_end = sep + 2;
    String_View head = (String_View){c->rbuf.items + c->rstart, head_end - c->rstart};
    if (parse_status_line(head, &out->status) != 0) {
        fail(out, "unparseable status line");
        return -1;
    }
    strbuf_append(&out->headers, head.data, head.count);

    size_t body_base = sep + 4;
    size_t body_consumed = 0;

    // a HEAD response declares lengths that it will never deliver, so there is
    // no body to read regardless of Content-Length or framing. the same goes
    // for 204 and 304, whose head already omits any length: reading to EOF
    // there would sit waiting on a keep-alive socket. the framing rules here
    // mirror http_response_serialize_head.
    bool has_body = method != HTTP_HEAD &&
                    out->status != HTTP_204_NO_CONTENT &&
                    out->status != HTTP_304_NOT_MODIFIED;

    if (has_body && head_has_token(head, "transfer-encoding", "chunked")) {
        Chunk_Reader cr;
        memset(&cr, 0, sizeof cr);
        size_t pos = body_base;
        for (;;) {
            char *err = NULL;
            int r = chunk_feed(&cr, c->rbuf.items, &pos, c->rbuf.count, &out->body, &err);
            if (r == 1) {
                break;
            }
            if (r < 0) {
                fail(out, "%s", err != NULL ? err : "malformed chunked body");
                xfree(err);
                return -1;
            }
            xfree(err);
            if (too_big(&c->rbuf)) {
                fail(out, "response body exceeds the %llu cap",
                     CLIENT_MAX_BODY);
                return -1;
            }
            err = NULL;
            int rr = refill(c, &err);
            if (rr <= 0) {
                fail(out, err != NULL ? "%s" : "connection dropped mid-body", err != NULL ? err : "?");
                xfree(err);
                return -1;
            }
            xfree(err);
        }
        body_consumed = pos - body_base;
    } else if (has_body) {
        long long cl = head_content_length(head);
        if (cl >= 0) {
            // wait out exactly the declared bytes so the socket stays in sync
            if ((unsigned long long)cl > CLIENT_MAX_BODY) {
                fail(out, "response body exceeds the %llu cap", CLIENT_MAX_BODY);
                return -1;
            }
            while (c->rbuf.count - body_base < (size_t)cl) {
                char *err = NULL;
                int r = refill(c, &err);
                if (r <= 0) {
                    fail(out, err != NULL ? "%s" : "connection dropped mid-body",
                         err != NULL ? err : "?");
                    xfree(err);
                    return -1;
                }
                xfree(err);
                if (too_big(&c->rbuf)) {
                    fail(out, "response body exceeds the %llu cap", CLIENT_MAX_BODY);
                    return -1;
                }
            }
            strbuf_append(&out->body, c->rbuf.items + body_base, (size_t)cl);
            body_consumed = (size_t)cl;
        } else {
            // no length and no chunking: the body runs to the end of the
            // connection, which means the connection dies with this response
            for (;;) {
                char *err = NULL;
                int r = refill(c, &err);
                if (r == 0) {
                    break;
                }
                if (r < 0) {
                    fail(out, err != NULL ? "%s" : "read failed", err != NULL ? err : "?");
                    xfree(err);
                    return -1;
                }
                xfree(err);
                if (too_big(&c->rbuf)) {
                    fail(out, "response body exceeds the %llu cap", CLIENT_MAX_BODY);
                    return -1;
                }
            }
            size_t n = c->rbuf.count - body_base;
            strbuf_append(&out->body, c->rbuf.items + body_base, n);
            body_consumed = n;
            *conn_dead = true;
        }
    }

    if (head_has_token(head, "connection", "close")) {
        *conn_dead = true;
    }

    // discard the consumed prefix, keep any pipelined bytes that arrived early
    size_t total = (body_base + body_consumed) - c->rstart;
    size_t keep = c->rbuf.count - (c->rstart + total);
    if (keep > 0) {
        memmove(c->rbuf.items, c->rbuf.items + c->rstart + total, keep);
    }
    c->rbuf.count = keep;
    c->rstart = 0;
    return 0;
}

static bool looks_absolute(const char *s)
{
    const char *p = strstr(s, "://");
    return p != NULL && p > s;
}

// an absolute URL replaces the client's base and its path/query become the
// request target. a root-relative target only needs the request line.
static int resolve_target(Http_Client *c, const char *raw,
                          Strbuf *target, char **err)
{
    if (looks_absolute(raw)) {
        Uri u;
        if (uri_parse(sv_from_cstr(raw), &u) != 0 || u.host.count == 0) {
            set_err(err, "bad URL");
            return -1;
        }
        if (!uri_scheme_is(&u, "http")) {
            set_err(err, uri_scheme_is(&u, "https")
                             ? "https is not supported yet"
                             : "URL needs an http:// scheme");
            return -1;
        }
        int port = uri_default_port(u.scheme);
        if (u.port.count > 0) {
            long long pn;
            if (!sv_to_i64(u.port, &pn) || pn <= 0 || pn > 65535) {
                set_err(err, "bad port in URL");
                return -1;
            }
            port = (int)pn;
        }
        if (u.host.count >= sizeof(c->host)) {
            set_err(err, "host name too long");
            return -1;
        }
        // new authority means the old keep-alive socket is meaningless
        bool same = strcmp(c->host, "") == 0 ||
                    (strlen(c->host) == u.host.count &&
                     memcmp(c->host, u.host.data, u.host.count) == 0 &&
                     c->port == port);
        if (!same) {
            if (c->sock != -1) {
                net_close(c->sock);
                c->sock = -1;
            }
        }
        memcpy(c->host, u.host.data, u.host.count);
        c->host[u.host.count] = '\0';
        c->port = port;
        strbuf_append(target, u.path.data, u.path.count);
        if (u.query.count > 0) {
            strbuf_append_char(target, '?');
            strbuf_append(target, u.query.data, u.query.count);
        }
        return 0;
    }
    if (raw[0] != '/') {
        set_err(err, "target must be absolute or start with '/'");
        return -1;
    }
    if (c->host[0] == '\0') {
        set_err(err, "no base URL: pass an absolute target or set one with http_client_open");
        return -1;
    }
    strbuf_append_cstr(target, raw);
    return 0;
}

static int do_request(Http_Client *c, const char *raw, Http_Method method,
                      const char *extra_headers, String_View body,
                      Http_Client_Result *out)
{
    if (c == NULL) {
        return -1;
    }
    const char *method_name = http_method_name(method);
    if (method_name == NULL) {
        fail(out, "unsupported method");
        return -1;
    }

    Strbuf target;
    strbuf_init(&target);
    char *reason = NULL;
    if (resolve_target(c, raw, &target, &reason) != 0) {
        fail(out, "%s", reason != NULL ? reason : "bad target");
        xfree(reason);
        strbuf_free(&target);
        return -1;
    }

    bool first = true;
    for (;;) {
        if (first) {
            first = false;
            memset(out, 0, sizeof *out);
        } else {
            // a retry replaces the failed attempt's partial result
            strbuf_free(&out->headers);
            strbuf_free(&out->body);
            xfree(out->error);
            memset(out, 0, sizeof *out);
        }
        bool reused = c->sock != -1;
        if (!reused) {
            c->sock = net_connect_host(c->host, c->port);
            c->opens++;
            if (c->sock == -1) {
                fail(out, "connect to %s:%d failed: %s", c->host, c->port,
                     net_error_string());
                strbuf_free(&target);
                return -1;
            }
        }
        // arm the deadline every attempt: after an idle keep-alive socket the
        // deadline was dropped, and a fresh socket never had one
        net_set_timeout(c->sock, c->timeout_ms);

        Strbuf head;
        strbuf_init(&head);
        strbuf_append_cstr(&head, method_name);
        strbuf_append_char(&head, ' ');
        strbuf_append(&head, target.items, target.count);
        strbuf_append_cstr(&head, " HTTP/1.1\r\nHost: ");
        strbuf_append_cstr(&head, c->host);
        if (c->port != 80 && c->port != 443) {
            char p[16];
            snprintf(p, sizeof(p), ":%d", c->port);
            strbuf_append_cstr(&head, p);
        }
        strbuf_append_cstr(&head, "\r\nConnection: keep-alive\r\n");
        if (extra_headers != NULL) {
            strbuf_append_cstr(&head, extra_headers);
        }
        if (body.count > 0) {
            char cl[64];
            snprintf(cl, sizeof(cl), "Content-Length: %zu\r\n", body.count);
            strbuf_append_cstr(&head, cl);
        }
        strbuf_append_cstr(&head, "\r\n");

        bool send_ok = net_send_all(c->sock, head.items, head.count) == (long)head.count &&
                       (body.count == 0 ||
                        net_send_all(c->sock, body.data, body.count) == (long)body.count);
        strbuf_free(&head);
        if (!send_ok) {
            net_close(c->sock);
            c->sock = -1;
            if (reused && idempotent(method)) {
                continue; // the socket had gone stale, resend on a fresh one
            }
            fail(out, "send failed: %s", net_error_string());
            strbuf_free(&target);
            return -1;
        }

        bool conn_dead = false;
        bool touched = false;
        int r = read_response(c, method, out, &conn_dead, &touched);
        if (r != 0) {
            net_close(c->sock);
            c->sock = -1;
            if (reused && idempotent(method) && !touched) {
                continue; // nothing came back: safe to resend once
            }
            strbuf_free(&target);
            return -1;
        }

        if (conn_dead) {
            net_close(c->sock);
            c->sock = -1;
        } else {
            // the socket stays open for reuse; drop the deadline meanwhile
            net_set_timeout(c->sock, 0);
        }
        strbuf_free(&target);
        return 0;
    }
}

static bool redirect_code(Http_Status s)
{
    return s == HTTP_301_MOVED_PERMANENTLY || s == HTTP_302_FOUND ||
           s == HTTP_303_SEE_OTHER || s == HTTP_307_TEMPORARY_REDIRECT ||
           s == HTTP_308_PERMANENT_REDIRECT;
}

// turns a Location value into something resolve_target accepts: absolute URLs
// and root-relative paths pass through, a bare relative path gets "/" prefixed
// so it resolves against the client's base host
static char *resolve_location(String_View location)
{
    char *out = xcalloc(location.count + 2, 1);
    bool absolute = false;
    for (size_t i = 0; i + 2 < location.count; i++) {
        if (location.data[i] == ':' && location.data[i + 1] == '/' &&
            location.data[i + 2] == '/') {
            absolute = true;
            break;
        }
    }
    if (absolute || (location.count > 0 && location.data[0] == '/')) {
        memcpy(out, location.data, location.count);
    } else {
        out[0] = '/';
        memcpy(out + 1, location.data, location.count);
    }
    return out;
}

int http_client_req(Http_Client *c, const char *url_or_path, Http_Method method,
                    const char *extra_headers, String_View body,
                    Http_Client_Result *out)
{
    if (url_or_path == NULL || out == NULL) {
        return -1;
    }

    int hops = 0;
    char *owned = NULL;
    for (;;) {
        int rc = do_request(c, url_or_path, method, extra_headers, body, out);
        if (rc != 0) {
            xfree(owned);
            return rc;
        }
        if (!redirect_code(out->status) || hops >= c->redirects) {
            break;
        }
        String_View location;
        if (!head_get_value((String_View){out->headers.items, out->headers.count},
                            "location", &location) || location.count == 0) {
            break; // a redirect with no Location goes nowhere
        }

        xfree(owned);
        owned = resolve_location(location);
        url_or_path = owned; // do_request keeps it through this call only
        hops++;
        // 303 always lands on a plain GET; 301/302 also downgrade a POST, the
        // way browsers handle form submissions. 307/308 must keep the method
        // and bytes so the destination can decide.
        if (out->status == HTTP_303_SEE_OTHER ||
            (out->status != HTTP_307_TEMPORARY_REDIRECT &&
             out->status != HTTP_308_PERMANENT_REDIRECT && method == HTTP_POST)) {
            method = HTTP_GET;
            body = (String_View){0};
        }
        // the redirect response was read; the next hop replaces it wholesale
        http_client_result_free(out);
    }
    xfree(owned);
    out->redirects = hops;
    return 0;
}

int http_client_req_get(Http_Client *c, const char *url_or_path, Http_Client_Result *out)
{
    return http_client_req(c, url_or_path, HTTP_GET, NULL, (String_View){0}, out);
}

int http_client_req_post(Http_Client *c, const char *url_or_path, String_View body,
                         Http_Client_Result *out)
{
    return http_client_req(c, url_or_path, HTTP_POST, NULL, body, out);
}

int http_client_request(const char *url, Http_Method method,
                        const char *extra_headers, String_View body,
                        Http_Client_Result *out)
{
    Http_Client *c = http_client_open(NULL);
    if (c == NULL) {
        return -1;
    }
    int rc = http_client_req(c, url, method, extra_headers, body, out);
    http_client_close(c);
    return rc;
}

int http_client_get(const char *url, Http_Client_Result *out)
{
    return http_client_request(url, HTTP_GET, NULL, (String_View){0}, out);
}

int http_client_post(const char *url, String_View body, Http_Client_Result *out)
{
    return http_client_request(url, HTTP_POST, NULL, body, out);
}

void http_client_result_free(Http_Client_Result *out)
{
    strbuf_free(&out->headers);
    strbuf_free(&out->body);
    xfree(out->error);
    memset(out, 0, sizeof *out);
}