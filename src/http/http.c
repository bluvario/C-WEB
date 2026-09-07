#include "http.h"

#include "sv.h"

const char *http_status_reason(Http_Status status)
{
    switch (status) {
    case HTTP_100_CONTINUE: return "Continue";
    case HTTP_101_SWITCHING_PROTOCOLS: return "Switching Protocols";
    case HTTP_200_OK: return "OK";
    case HTTP_201_CREATED: return "Created";
    case HTTP_202_ACCEPTED: return "Accepted";
    case HTTP_204_NO_CONTENT: return "No Content";
    case HTTP_301_MOVED_PERMANENTLY: return "Moved Permanently";
    case HTTP_302_FOUND: return "Found";
    case HTTP_304_NOT_MODIFIED: return "Not Modified";
    case HTTP_307_TEMPORARY_REDIRECT: return "Temporary Redirect";
    case HTTP_308_PERMANENT_REDIRECT: return "Permanent Redirect";
    case HTTP_400_BAD_REQUEST: return "Bad Request";
    case HTTP_401_UNAUTHORIZED: return "Unauthorized";
    case HTTP_403_FORBIDDEN: return "Forbidden";
    case HTTP_404_NOT_FOUND: return "Not Found";
    case HTTP_405_METHOD_NOT_ALLOWED: return "Method Not Allowed";
    case HTTP_408_REQUEST_TIMEOUT: return "Request Timeout";
    case HTTP_413_PAYLOAD_TOO_LARGE: return "Payload Too Large";
    case HTTP_415_UNSUPPORTED_MEDIA_TYPE: return "Unsupported Media Type";
    case HTTP_429_TOO_MANY_REQUESTS: return "Too Many Requests";
    case HTTP_500_INTERNAL_SERVER_ERROR: return "Internal Server Error";
    case HTTP_501_NOT_IMPLEMENTED: return "Not Implemented";
    case HTTP_503_SERVICE_UNAVAILABLE: return "Service Unavailable";
    default: return "Unknown";
    }
}

Http_Method http_method_from_sv(String_View name)
{
    if (sv_equal(name, sv_from_cstr("GET"))) return HTTP_GET;
    if (sv_equal(name, sv_from_cstr("POST"))) return HTTP_POST;
    if (sv_equal(name, sv_from_cstr("PUT"))) return HTTP_PUT;
    if (sv_equal(name, sv_from_cstr("DELETE"))) return HTTP_DELETE;
    if (sv_equal(name, sv_from_cstr("PATCH"))) return HTTP_PATCH;
    if (sv_equal(name, sv_from_cstr("HEAD"))) return HTTP_HEAD;
    if (sv_equal(name, sv_from_cstr("OPTIONS"))) return HTTP_OPTIONS;
    return HTTP_UNKNOWN_METHOD;
}

const char *http_method_name(Http_Method method)
{
    switch (method) {
    case HTTP_GET: return "GET";
    case HTTP_POST: return "POST";
    case HTTP_PUT: return "PUT";
    case HTTP_DELETE: return "DELETE";
    case HTTP_PATCH: return "PATCH";
    case HTTP_HEAD: return "HEAD";
    case HTTP_OPTIONS: return "OPTIONS";
    case HTTP_UNKNOWN_METHOD: return NULL;
    }
    return NULL; // unreachable, silence compiler
}