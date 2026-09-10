#include "request.h"

#include <ctype.h>
#include <string.h>

#include "da.h"
#include "sv.h"
#include "xmem.h"

// index of the blank line ending the header block ("\r\n\r\n" or "\n\n"),
// or SIZE_MAX when the request is not complete yet
static size_t find_header_end(String_View raw)
{
    for (size_t i = 0; i + 2 < raw.count; i++) {
        if (raw.data[i] == '\r' && raw.data[i + 1] == '\n' &&
            raw.data[i + 2] == '\r' && raw.data[i + 3] == '\n') {
            return i + 4;
        }
    }
    for (size_t i = 0; i + 1 < raw.count; i++) {
        if (raw.data[i] == '\n' && raw.data[i + 1] == '\n') {
            return i + 2;
        }
    }
    return (size_t)-1;
}

static int parse_request_line(Http_Request *req, String_View line)
{
    String_View method = sv_chop_by_delim(&line, ' ');
    String_View target = sv_chop_by_delim(&line, ' ');
    String_View version = sv_chop_by_delim(&line, ' ');

    if (method.count == 0 || target.count == 0 || version.count == 0 ||
        !sv_starts_with(version, "HTTP/") || line.count != 0) {
        return -1;
    }

    req->method = http_method_from_sv(method);
    req->target = target;
    req->version = version;

    size_t q = 0;
    while (q < target.count && target.data[q] != '?') {
        q++;
    }
    req->path = (String_View){target.data, q};
    req->query = (String_View){target.data + q, q < target.count ? target.count - q - 1 : 0};
    if (q < target.count) {
        req->query.data++; // skip the '?'
    }
    return 0;
}

static int parse_header_line(Http_Request *req, String_View line)
{
    size_t c = 0;
    while (c < line.count && line.data[c] != ':') {
        c++;
    }
    if (c >= line.count) {
        return 0; // line without a colon, treat as junk and move on
    }

    String_View key = sv_trim((String_View){line.data, c});
    String_View value = sv_trim((String_View){line.data + c + 1, line.count - c - 1});
    if (key.count == 0) {
        return 0;
    }
    da_append(&req->headers, ((Http_Header){key, value}));
    return 0;
}

Request_Parse_Result http_request_parse_adv(Http_Request *req, String_View raw, size_t *consumed)
{
    *consumed = 0;
    req->method = HTTP_UNKNOWN_METHOD;
    req->target = req->path = req->query = req->version = req->body = (String_View){0};
    req->remote = (String_View){0};
    req->route_timeout_ms = 0;
    req->headers.items = NULL;
    req->headers.count = 0;
    req->headers.capacity = 0;

    size_t end = find_header_end(raw);
    if (end == (size_t)-1) {
        return REQ_INCOMPLETE;
    }

    String_View head = sv_trim((String_View){raw.data, end});
    size_t lineno = 0;
    while (head.count > 0) {
        String_View line = sv_chop_by_delim(&head, '\n');
        if (line.count > 0 && line.data[line.count - 1] == '\r') {
            line.count--;
        }
        if (lineno == 0) {
            if (parse_request_line(req, line) != 0) {
                http_request_free(req);
                return REQ_ERROR;
            }
        } else {
            parse_header_line(req, line);
        }
        lineno++;
    }

    req->body = (String_View){raw.data + end, raw.count - end};
    const String_View *cl = http_request_get_header(req, "content-length");
    if (cl) {
        long long len;
        if (!sv_to_i64(*cl, &len) || len < 0) {
            http_request_free(req);
            return REQ_ERROR;
        }
        if ((size_t)len < req->body.count) {
            req->body.count = (size_t)len;
        } else if ((size_t)len > req->body.count) {
            http_request_free(req);
            return REQ_INCOMPLETE;
        }
        *consumed = end + (size_t)len;
    } else {
        *consumed = end;
    }
    return REQ_OK;
}

Request_Parse_Result http_request_parse(Http_Request *req, String_View raw)
{
    size_t consumed;
    return http_request_parse_adv(req, raw, &consumed);
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int http_request_decode_chunked(Http_Request *req, char *raw, size_t raw_count,
                                size_t *consumed)
{
    (void)raw_count;
    // chunking and Content-Length together is a request smuggling vector, so
    // contradict each other openly; refuse rather than guess (RFC 9112 7.1)
    if (http_request_get_header(req, "content-length") != NULL) {
        return -1;
    }

    char *data = (char *)req->body.data; // alias, so we can overwrite framing
    size_t len = req->body.count;
    size_t r = 0; // cursor reading the chunked stream
    size_t w = 0; // cursor writing decoded bytes (never overtakes r)
    for (;;) {
        // a stray CRLF between chunks is allowed; otherwise the chunk line
        // begins here
        while (r < len && (data[r] == '\r' || data[r] == '\n')) {
            r++;
        }
        if (r >= len) {
            return 1; // chunk size line never arrived
        }
        size_t size = 0;
        bool saw_digit = false;
        while (r < len) {
            int v = hex_value(data[r]);
            if (v >= 0) {
                size = size * 16 + (size_t)v;
                saw_digit = true;
                r++;
            } else if (data[r] == ';' || data[r] == ' ') {
                // chunk extensions (";ext=value") and padding are ignored
                while (r < len && data[r] != '\r' && data[r] != '\n') {
                    r++;
                }
                break;
            } else {
                break;
            }
        }
        if (!saw_digit || r + 1 >= len || data[r] != '\r' || data[r + 1] != '\n') {
            return -1;
        }
        r += 2;
        if (size == 0) {
            // the trailer block, at least an empty line; anything after the
            // zero chunk is trailers, and they disappear with the response
            if (r + 1 <= len && data[r] == '\r' && data[r + 1] == '\n') {
                r += 2;
            } else if (r < len) {
                while (r < len && data[r] != '\r' && data[r] != '\n') {
                    r++;
                }
                if (r + 1 < len && data[r] == '\r' && data[r + 1] == '\n') {
                    r += 2;
                } else if (r < len && data[r] == '\n') {
                    r++;
                } else {
                    return 1; // trailer line without its terminator yet
                }
            }
            break;
        }
        if (r + size > len) {
            return 1; // the chunk payload is still arriving
        }
        // slide the payload over the framing bytes we no longer need
        memmove(data + w, data + r, size);
        w += size;
        r += size;
        if (r + 1 >= len || data[r] != '\r' || data[r + 1] != '\n') {
            return -1;
        }
        r += 2;
    }
    req->body = (String_View){(const char *)data, w};
    *consumed = (size_t)(data - raw) + r;
    return 0;
}

void http_request_free(Http_Request *req)
{
    xfree(req->headers.items);
    req->headers.items = NULL;
    req->headers.count = 0;
    req->headers.capacity = 0;
}

const String_View *http_request_get_header(Http_Request *req, const char *name)
{
    size_t n = strlen(name);
    for (size_t i = 0; i < req->headers.count; i++) {
        if (req->headers.items[i].key.count == n) {
            bool same = true;
            for (size_t j = 0; j < n; j++) {
                if (toupper((unsigned char)req->headers.items[i].key.data[j]) !=
                    toupper((unsigned char)name[j])) {
                    same = false;
                    break;
                }
            }
            if (same) {
                return &req->headers.items[i].value;
            }
        }
    }
    return NULL;
}