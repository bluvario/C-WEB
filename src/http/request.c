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

Request_Parse_Result http_request_parse(Http_Request *req, String_View raw)
{
    req->method = HTTP_UNKNOWN_METHOD;
    req->target = req->path = req->query = req->version = (String_View){0};
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
    return REQ_OK;
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