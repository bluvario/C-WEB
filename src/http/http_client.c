#include "http_client.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "http.h"
#include "net.h"
#include "strbuf.h"
#include "uri.h"
#include "xmem.h"

// a hostile or misbehaving peer must not hand us an unbounded body
static const size_t CLIENT_MAX_BODY = 32 * 1024 * 1024;
// how long to wait for the first byte and between later ones
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

static int parse_status(const char *line, Http_Status *status)
{
    // "HTTP/1.1 200 OK" -> the code as an enum-worthy number
    const char *p = strchr(line, ' ');
    if (p == NULL) {
        return -1;
    }
    p++;
    char *endp = NULL;
    long code = strtol(p, &endp, 10);
    if (endp == p || code < 100 || code > 599) {
        return -1;
    }
    *status = (Http_Status)code;
    return 0;
}

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

static bool body_is_chunked(String_View head_lines)
{
    String_View rest = head_lines;
    while (rest.count > 0) {
        String_View line = sv_trim_right(sv_chop_by_delim(&rest, '\n'));
        if (line.count == 0) {
            break; // the blank line after the headers
        }
        if (header_is(line, "transfer-encoding")) {
            const char *colon = memchr(line.data, ':', line.count);
            String_View value = sv_trim((String_View){colon + 1, line.count - ((size_t)(colon - line.data) + 1)});
            if (sv_equal(value, sv_from_cstr("chunked"))) {
                return true;
            }
        }
    }
    return false;
}

// consumes one chunk: hex size [;ext] line, payload, trailing CRLF. returns
// the payload length, or 0 for the terminal chunk, or -1 on malformed input.
static long chunk_head(String_View *data)
{
    String_View rest = *data;
    String_View line = sv_chop_by_delim(&rest, '\n');
    if (line.count == 0) {
        return -1;
    }
    line = sv_trim_right(line);

    long long size = 0;
    int digits = 0;
    for (size_t i = 0; i < line.count; i++) {
        char c = line.data[i];
        if (c == ';') {
            break; // chunk extension, ignore
        }
        int d = hex_digit(c);
        if (d < 0) {
            return -1;
        }
        size = size * 16 + d;
        digits++;
        if ((unsigned long long)size > (unsigned long long)CLIENT_MAX_BODY) {
            return -1;
        }
    }
    if (digits == 0) {
        return -1;
    }
    *data = rest;
    return (long)size;
}

static int decode_chunked(String_View bytes, Strbuf *body, char **err)
{
    for (;;) {
        long size = chunk_head(&bytes);
        if (size < 0) {
            *err = xmalloc(strlen("malformed chunk framing") + 1);
            strcpy(*err, "malformed chunk framing");
            return -1;
        }
        if (size == 0) {
            // drain trailers (header lines up to the following blank line)
            String_View rest = bytes;
            while (rest.count > 0) {
                String_View line = sv_chop_by_delim(&rest, '\n');
                if (sv_trim_right(line).count == 0) {
                    break;
                }
            }
            return 0;
        }
        // the size line ended in "\n" only if it had "\r", our chop took it
        if (bytes.count < (size_t)size + 2) {
            *err = xmalloc(strlen("truncated chunked body") + 1);
            strcpy(*err, "truncated chunked body");
            return -1;
        }
        strbuf_append(body, bytes.data, (size_t)size);
        bytes.data += size + 2; // payload plus CRLF
        bytes.count -= size + 2;
        if (body->count > CLIENT_MAX_BODY) {
            *err = xmalloc(strlen("body too large") + 1);
            strcpy(*err, "body too large");
            return -1;
        }
    }
}

int http_client_request(const char *url, Http_Method method,
                        const char *extra_headers, String_View body,
                        Http_Client_Result *out)
{
    memset(out, 0, sizeof *out);

    Uri uri;
    if (uri_parse(sv_from_cstr(url), &uri) != 0) {
        fail(out, "bad URL");
        return -1;
    }
    if (!uri_scheme_is(&uri, "http")) {
        if (uri_scheme_is(&uri, "https")) {
            fail(out, "https is not supported yet");
        } else {
            fail(out, "URL needs an http:// scheme");
        }
        return -1;
    }
    if (uri.host.count == 0) {
        fail(out, "URL has no host");
        return -1;
    }

    int port = 0;
    if (uri.port.count > 0) {
        long long pn;
        if (!sv_to_i64(uri.port, &pn) || pn <= 0 || pn > 65535) {
            fail(out, "bad port in URL");
            return -1;
        }
        port = (int)pn;
    } else {
        port = uri_default_port(uri.scheme);
    }
    char hostz[256];
    String_View host = uri.host;
    if (host.count >= sizeof(hostz)) {
        fail(out, "host name too long");
        return -1;
    }
    memcpy(hostz, host.data, host.count);
    hostz[host.count] = '\0';

    // request target: path plus query
    Strbuf target;
    strbuf_init(&target);
    strbuf_append(&target, uri.path.data, uri.path.count);
    if (uri.query.count > 0) {
        strbuf_append_char(&target, '?');
        strbuf_append(&target, uri.query.data, uri.query.count);
    }

    const char *method_name = http_method_name(method);
    if (method_name == NULL) {
        strbuf_free(&target);
        fail(out, "unsupported method");
        return -1;
    }

    Strbuf head;
    strbuf_init(&head);
    strbuf_append_cstr(&head, method_name);
    strbuf_append_char(&head, ' ');
    strbuf_append(&head, target.items, target.count);
    strbuf_append_cstr(&head, " HTTP/1.1\r\n");
    strbuf_append_cstr(&head, "Host: ");
    strbuf_append_cstr(&head, hostz);
    if (uri.port.count > 0) {
        strbuf_append_char(&head, ':');
        strbuf_append(&head, uri.port.data, uri.port.count);
    }
    strbuf_append_cstr(&head, "\r\nConnection: close\r\n");
    if (extra_headers != NULL) {
        strbuf_append_cstr(&head, extra_headers);
    }
    if (body.count > 0) {
        char cl[64];
        snprintf(cl, sizeof(cl), "Content-Length: %zu\r\n", body.count);
        strbuf_append_cstr(&head, cl);
    }
    strbuf_append_cstr(&head, "\r\n");
    strbuf_free(&target);

    Socket_Handle fd = net_connect_host(hostz, port);
    if (fd == -1) {
        fail(out, "connect to %s:%d failed: %s", hostz, port, net_error_string());
        strbuf_free(&head);
        return -1;
    }
    net_set_timeout(fd, CLIENT_TIMEOUT_MS);

    if (net_send_all(fd, head.items, head.count) != (long)head.count ||
        (body.count > 0 && net_send_all(fd, body.data, body.count) != (long)body.count)) {
        strbuf_free(&head);
        fail(out, "send failed: %s", net_error_string());
        net_close(fd);
        return -1;
    }
    strbuf_free(&head);

    Strbuf wire;
    strbuf_init(&wire);
    char chunk[16384];
    for (;;) {
        long n = net_recv(fd, chunk, sizeof(chunk));
        if (n == NET_READ_TIMEOUT) {
            strbuf_free(&wire);
            fail(out, "timed out waiting for the response");
            net_close(fd);
            return -1;
        }
        if (n < 0) {
            strbuf_free(&wire);
            fail(out, "read failed: %s", net_error_string());
            net_close(fd);
            return -1;
        }
        if (n == 0) {
            break; // Connection: close, the whole response is here
        }
        if (wire.count + (size_t)n > CLIENT_MAX_BODY) {
            strbuf_free(&wire);
            fail(out, "response exceeds %zu bytes", CLIENT_MAX_BODY);
            net_close(fd);
            return -1;
        }
        strbuf_append(&wire, chunk, (size_t)n);
    }
    net_close(fd);

    // split head from body at the first blank line
    char *head_end = NULL;
    for (size_t i = 0; i + 3 < wire.count; i++) {
        if (wire.items[i] == '\r' && wire.items[i + 1] == '\n' &&
            wire.items[i + 2] == '\r' && wire.items[i + 3] == '\n') {
            head_end = &wire.items[i];
            break;
        }
    }
    if (head_end == NULL) {
        strbuf_free(&wire);
        fail(out, "response had no header terminator");
        return -1;
    }

    String_View head_view = (String_View){wire.items, (size_t)(head_end - wire.items)};
    String_View body_view = (String_View){head_end + 4, wire.count - head_view.count - 4};

    char status_line[512];
    size_t line_len = 0;
    while (line_len < head_view.count &&
           head_view.data[line_len] != '\r' && head_view.data[line_len] != '\n') {
        line_len++;
    }
    if (line_len >= sizeof(status_line)) {
        strbuf_free(&wire);
        fail(out, "unparseable status line");
        return -1;
    }
    memcpy(status_line, head_view.data, line_len);
    status_line[line_len] = '\0';
    if (parse_status(status_line, &out->status) != 0) {
        strbuf_free(&wire);
        fail(out, "unparseable status line");
        return -1;
    }

    strbuf_append(&out->headers, wire.items, head_view.count);

    if (body_is_chunked(head_view)) {
        if (decode_chunked(body_view, &out->body, &out->error) != 0) {
            strbuf_free(&wire);
            return -1;
        }
    } else {
        strbuf_append(&out->body, body_view.data, body_view.count);
    }

    strbuf_free(&wire);
    return 0;
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