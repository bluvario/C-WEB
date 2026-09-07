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

    Http_Header_Array headers;
} Http_Request;

typedef enum {
    REQ_OK,        // parsed, ready to dispatch
    REQ_INCOMPLETE, // need more bytes
    REQ_ERROR,     // malformed beyond recovery
} Request_Parse_Result;

Request_Parse_Result http_request_parse(Http_Request *req, String_View raw);
void http_request_free(Http_Request *req);

// case-insensitive header lookup (HTTP names are case-insensitive),
// returns NULL when the header is absent
const String_View *http_request_get_header(Http_Request *req, const char *name);

#endif