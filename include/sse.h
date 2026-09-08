#ifndef CWEB_SSE_H
#define CWEB_SSE_H

#include "response.h"
#include "strbuf.h"
#include "sv.h"

// Server-Sent Events turn one HTTP response into a stream of text events.
// build the events with sse_frame() into a Strbuf you own, hand it out, done.

// the produce callback runs in the response's streaming slot: fill buf with
// frame bytes and return how many, or 0 to end the stream, which closes the
// connection. the server does the chunked framing, never build HTTP here.
typedef size_t (*Http_Sse_Produce)(void *buf, size_t cap, void *user_data);

// turns the response into an SSE endpoint: the right headers, no caching, and
// a streamed body sourced from produce. any buffered body is dropped, a
// response is streamed or assembled, not both.
void http_response_start_sse(Http_Response *res, Http_Sse_Produce produce,
                             void *user_data);

// renders one event to the frame buffer per the SSE spec: an optional id line,
// an optional event type, the data (embedded newlines split across "data: "
// lines, an empty payload still yields one bare line), an optional retry hint,
// then the terminating blank line. returns bytes appended (>0 while out grew).
size_t sse_frame(Strbuf *out, String_View data, const char *event,
                 const char *id, int retry_ms);

#endif