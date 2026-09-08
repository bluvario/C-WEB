#ifndef CWEB_HTTP_H
#define CWEB_HTTP_H

#include "sv.h"

typedef enum {
    HTTP_100_CONTINUE = 100,
    HTTP_101_SWITCHING_PROTOCOLS = 101,
    HTTP_200_OK = 200,
    HTTP_201_CREATED = 201,
    HTTP_202_ACCEPTED = 202,
    HTTP_204_NO_CONTENT = 204,
    HTTP_206_PARTIAL_CONTENT = 206,
    HTTP_301_MOVED_PERMANENTLY = 301,
    HTTP_302_FOUND = 302,
    HTTP_303_SEE_OTHER = 303,
    HTTP_304_NOT_MODIFIED = 304,
    HTTP_307_TEMPORARY_REDIRECT = 307,
    HTTP_308_PERMANENT_REDIRECT = 308,
    HTTP_400_BAD_REQUEST = 400,
    HTTP_401_UNAUTHORIZED = 401,
    HTTP_403_FORBIDDEN = 403,
    HTTP_404_NOT_FOUND = 404,
    HTTP_405_METHOD_NOT_ALLOWED = 405,
    HTTP_408_REQUEST_TIMEOUT = 408,
    HTTP_413_PAYLOAD_TOO_LARGE = 413,
    HTTP_415_UNSUPPORTED_MEDIA_TYPE = 415,
    HTTP_416_RANGE_NOT_SATISFIABLE = 416,
    HTTP_429_TOO_MANY_REQUESTS = 429,
    HTTP_500_INTERNAL_SERVER_ERROR = 500,
    HTTP_501_NOT_IMPLEMENTED = 501,
    HTTP_502_BAD_GATEWAY = 502,
    HTTP_503_SERVICE_UNAVAILABLE = 503,
} Http_Status;

typedef enum {
    HTTP_GET,
    HTTP_POST,
    HTTP_PUT,
    HTTP_DELETE,
    HTTP_PATCH,
    HTTP_HEAD,
    HTTP_OPTIONS,
    HTTP_UNKNOWN_METHOD,
} Http_Method;

// reason phrase for a status code, "Unknown" when not in the table. the codes
// above are the ones we actually use, the rest are a TODO.
const char *http_status_reason(Http_Status status);

// request-line method tokens are case-sensitive per RFC 9110
Http_Method http_method_from_sv(String_View name);
// "GET" ... for HTTP_UNKNOWN_METHOD returns NULL
const char *http_method_name(Http_Method method);

#endif