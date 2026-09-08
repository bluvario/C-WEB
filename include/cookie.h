#ifndef CWEB_COOKIE_H
#define CWEB_COOKIE_H

#include <stdbool.h>

#include "response.h"
#include "strmap.h"
#include "sv.h"

// attributes that may ride along on a Set-Cookie: NULL/negative/bool field
// just omits that attribute from the wire
typedef struct {
    const char *path;      // "Path=..." when set
    int max_age;           // "Max-Age=N" when >= 0
    bool http_only;        // HttpOnly flag
    bool secure;           // Secure flag
} Cookie_Attrs;

// appends a Set-Cookie header. name and value are emitted verbatim, the
// caller is responsible for any percent- or character escaping.
void http_response_set_cookie(Http_Response *res, const char *name, const char *value,
                              const Cookie_Attrs *attrs);

// parses a "Cookie: a=1; b=2" request header into *jar. quoted values lose
// their quotes, pairs without a name are skipped, and a duplicate name is
// overwritten (clients may send the same name twice, last one wins).
void http_cookie_parse(Str_Map *jar, String_View header);

#endif