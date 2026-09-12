#ifndef CWEB_REQUEST_H
#define CWEB_REQUEST_H

#include "http.h"
#include "sv.h"

typedef struct {
    String_View key;
    String_View value;
} Http_Header;

typedef struct {
    Http_Header *items;
    size_t count;
    size_t capacity;
} Http_Header_Array;

// all views borrow from the request buffer that was parsed, the owner keeps
// that buffer alive until the response is done. only the header array is
// allocated here.
typedef struct {
    Http_Method method;
    String_View target;  // raw request target, query string included
    String_View path;    // target without the query string
    String_View query;   // query string without the leading '?', empty if none
    String_View version; // "HTTP/1.1" etc.
    // everything after the header block, clamped to Content-Length when the
    // header is present, or contiguous decoded bytes after a chunked body has
    // been unfolded by http_request_decode_chunked. the view borrows from the
    // raw request bytes. when the body exceeds the server's in-RAM cap and a
    // --body-dir is configured the server spills the body to a temp file and
    // mmap's it back; req->body then borrows from the mapping (freed on
    // request cleanup).
    String_View body;

    // once the header block has been parsed — even while the declared body
    // has not fully arrived — these two fields tell the server where the body
    // starts in the staging buffer and how many bytes were promised by
    // Content-Length. the server uses them to route oversized bodies straight
    // to disk instead of returning a 413.
    long long content_length; // Content-Length or -1
    size_t    body_offset;   // byte index into the staging buffer (header block length)

    // when a body has been spilled to a temp file the server mmap's it back
    // so handlers read req->body exactly as before. these track that mapping;
    // http_request_free tears it down.
    void  *body_mmap;       // PROT_READ mapping of the spilled body, NULL when in-RAM
    size_t body_mmap_len;   // bytes mapped
    char  *body_file_path;  // heap-allocated temp path to unlink on free

    // heap storage for a Content-Encoding: gzip request body after the server
    // inflates it; req->body then borrows from here. owned by the request and
    // released by http_request_free; NULL when the body was not decompressed.
    void *body_heap;

    // caller's IP address as text, filled in by the server for the lifetime
    // of the connection; empty in hand-constructed requests. handy for
    // middleware that keys on the client, like per-IP rate limiting.
    String_View remote;

    // effective client address after trusted-proxy resolution, set by
    // http_client_ip_middleware. borrows from the request buffer (like remote,
    // and like remote it is the direct peer when no trusted proxy is in play);
    // empty when the middleware is not in the chain. consumers that want "who
    // is really calling" — access logs, per-address rate buckets — read this
    // instead of remote.
    String_View client_ip;

    // unique identifier for this request, set by the request-id middleware
    // (http_request_id_middleware) when it runs. data points at heap storage
    // owned by the request object and released by http_request_free, so the
    // view stays valid for the whole request/response cycle. empty when the
    // middleware is not in the chain.
    String_View request_id;

    // authenticated username, set by http_basic_auth_middleware when it runs
    // and the request carried valid credentials. same ownership and lifetime
    // as request_id: heap storage released by http_request_free, empty when
    // the guard (or the chain it guards) did not run.
    String_View auth_user;

    Http_Header_Array headers;

    // filled in by the router when a route carries a timeout override.
    // the server uses this to reset SO_RCVTIMEO after each response so the
    // next request on a keep-alive connection uses the matched route's budget.
    // zero means "use the server default".
    unsigned long route_timeout_ms;
} Http_Request;

typedef enum {
    REQ_OK,        // parsed, ready to dispatch
    REQ_INCOMPLETE, // need more bytes
    REQ_ERROR,     // malformed beyond recovery
} Request_Parse_Result;

Request_Parse_Result http_request_parse(Http_Request *req, String_View raw);
// like http_request_parse, but also hands back how many bytes of *raw belong
// to this one request (headers plus declared body), so the socket layer can
// keep any pipelined remainder buffered.
Request_Parse_Result http_request_parse_adv(Http_Request *req, String_View raw, size_t *consumed);
// Re-parses only the header lines out of an already-parsed request's header
// block (request line included; it is skipped). The request parser releases a
// request's headers whenever it reports an incomplete body, so the server's
// spill-to-disk path — which keeps an oversized request alive until its body
// lands on disk — calls this to restore them before dispatch. req->headers
// must be empty on entry.
void http_request_parse_header_block(Http_Request *req, String_View head);
// Unfolds a Transfer-Encoding: chunked body in place inside raw (the buffer
// the request bytes live in and that req->body borrows from), leaving req->body
// pointing at contiguous decoded bytes. On success sets *consumed to the full
// length the request occupied in raw and returns 0. Returns 1 when the body is
// incomplete (caller should read more and re-parse) and -1 on malformed
// framing or when Content-Length is present alongside chunking (a request
// smuggling vector).
int http_request_decode_chunked(Http_Request *req, char *raw, size_t raw_count,
                                size_t *consumed);
void http_request_free(Http_Request *req);

// case-insensitive header lookup (HTTP names are case-insensitive),
// returns NULL when the header is absent
const String_View *http_request_get_header(Http_Request *req, const char *name);

#endif