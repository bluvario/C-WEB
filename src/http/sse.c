#include "sse.h"

#include <stdio.h>
#include <string.h>

#include "http.h"
#include "response.h"
#include "strbuf.h"
#include "sv.h"

void http_response_start_sse(Http_Response *res, Http_Sse_Produce produce,
                             void *user_data)
{
    http_response_set_status(res, HTTP_200_OK);
    http_response_set_header(res, "Content-Type", "text/event-stream");
    // an event stream must never be cached as one blob
    http_response_set_header(res, "Cache-Control", "no-cache");
    // proxies love buffering streaming answers; ask them not to
    http_response_set_header(res, "X-Accel-Buffering", "no");
    http_response_set_stream(res, (Http_Stream_Fn)produce, user_data);
}

size_t sse_frame(Strbuf *out, String_View data, const char *event,
                 const char *id, int retry_ms)
{
    size_t before = out->count;

    if (id != NULL) {
        strbuf_append_cstr(out, "id: ");
        strbuf_append_cstr(out, id);
        strbuf_append_char(out, '\n');
    }
    if (event != NULL) {
        strbuf_append_cstr(out, "event: ");
        strbuf_append_cstr(out, event);
        strbuf_append_char(out, '\n');
    }
    // every piece of data out on its own "data: " line, like the spec wants
    String_View rest = data;
    do {
        String_View line = sv_chop_by_delim(&rest, '\n');
        strbuf_append_cstr(out, "data: ");
        strbuf_append(out, line.data, line.count);
        strbuf_append_char(out, '\n');
    } while (rest.count > 0);
    if (retry_ms > 0) {
        char r[32];
        int n = snprintf(r, sizeof(r), "retry: %d\n", retry_ms);
        if (n > 0) {
            strbuf_append(out, r, (size_t)n);
        }
    }
    // the blank line that separates events
    strbuf_append_char(out, '\n');

    return out->count - before;
}