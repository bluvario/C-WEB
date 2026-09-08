#ifndef CWEB_URI_H
#define CWEB_URI_H

#include "sv.h"

// RFC 3986-ish URI decomposition: scheme://authority/path?query#fragment.
// authority is host[:port], with [brackets] around IPv6 literals. every
// component is a view into the input; nothing is copied, decoded or encoded.
typedef struct {
    String_View scheme;   // "http", "https", ...; empty when absent
    String_View host;     // without brackets or port
    String_View port;     // as written, empty when absent
    String_View path;     // becomes "/" when the source omits it
    String_View query;    // without '?'
    String_View fragment; // without '#'
} Uri;

// decomposes a URI. 0 on success, -1 when the shape is nonsense: a scheme
// marker with no host, a non-numeric port, a bracketed host with no closing
// brace. relative forms ("/path?q") are fine, scheme comparisons happen
// case-insensitively and percent-encoding is left untouched.
int uri_parse(String_View text, Uri *out);

// case-insensitive: "http" -> 80, "https" -> 443, anything else -> -1
int uri_default_port(String_View scheme);

// case-insensitive scheme check, e.g. uri_scheme_is(&u, "http")
bool uri_scheme_is(const Uri *u, const char *name);

#endif