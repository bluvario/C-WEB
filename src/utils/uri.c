#include "uri.h"

#include <ctype.h>
#include <stdint.h>
#include <string.h>

// index of the next byte in sv, or SIZE_MAX when absent
static size_t sv_index(String_View sv, char c)
{
    for (size_t i = 0; i < sv.count; i++) {
        if (sv.data[i] == c) {
            return i;
        }
    }
    return SIZE_MAX;
}

static bool scheme_ok(String_View s)
{
    if (s.count == 0 || !isalpha((unsigned char)s.data[0])) {
        return false;
    }
    for (size_t i = 1; i < s.count; i++) {
        char c = s.data[i];
        if (!(isalnum((unsigned char)c) || c == '+' || c == '-' || c == '.')) {
            return false;
        }
    }
    return true;
}

// separates host[:port]. IPv6 literals are bracketed, so a ":" beyond the
// closing bracket belongs to the port. returns 0, or -1 on a nonsense port.
static int split_authority(String_View auth, String_View *host, String_View *port)
{
    host->count = 0;
    port->count = 0;
    if (auth.count == 0) {
        return -1;
    }
    if (auth.data[0] == '[') {
        size_t close = sv_index(auth, ']');
        if (close == SIZE_MAX) {
            return -1;
        }
        *host = (String_View){auth.data + 1, close - 1};
        String_View tail = (String_View){auth.data + close + 1, auth.count - close - 1};
        if (tail.count == 0) {
            return 0;
        }
        if (!sv_consume_prefix(&tail, ":")) {
            return -1; // junk between ']' and the port
        }
        *port = tail;
    } else {
        size_t colon = sv_index(auth, ':');
        if (colon == SIZE_MAX) {
            *host = auth;
            return 0;
        }
        *host = (String_View){auth.data, colon};
        *port = (String_View){auth.data + colon + 1, auth.count - colon - 1};
        if (host->count == 0) {
            return -1;
        }
    }
    // port must be all digits and at most a common-size port number
    if (port->count == 0 || port->count > 5) {
        return -1;
    }
    long long pn;
    if (!sv_to_i64(*port, &pn) || pn < 0 || pn > 65535) {
        return -1;
    }
    return 0;
}

int uri_parse(String_View text, Uri *out)
{
    memset(out, 0, sizeof *out);

    String_View rest = text;
    size_t hash = sv_index(rest, '#');
    if (hash != SIZE_MAX) {
        out->fragment = (String_View){rest.data + hash + 1, rest.count - hash - 1};
        rest.count = hash;
    }
    size_t q = sv_index(rest, '?');
    if (q != SIZE_MAX) {
        out->query = (String_View){rest.data + q + 1, rest.count - q - 1};
        rest.count = q;
    }

    // scheme present only as "scheme://"
    size_t colon = sv_index(rest, ':');
    if (colon != SIZE_MAX && colon + 2 < rest.count &&
        rest.data[colon + 1] == '/' && rest.data[colon + 2] == '/') {
        out->scheme = (String_View){rest.data, colon};
        if (!scheme_ok(out->scheme)) {
            return -1;
        }
        String_View authority = (String_View){rest.data + colon + 3, rest.count - colon - 3};
        size_t slash = sv_index(authority, '/');
        String_View auth;
        if (slash == SIZE_MAX) {
            auth = authority;
            out->path = sv_from_cstr("/");
        } else {
            auth = (String_View){authority.data, slash};
            out->path = (String_View){authority.data + slash, authority.count - slash};
            if (out->path.count == 0) {
                out->path = sv_from_cstr("/");
            }
        }
        if (split_authority(auth, &out->host, &out->port) != 0) {
            return -1;
        }
    } else {
        // no scheme: everything left is a (possibly empty) path
        if (rest.count == 0) {
            out->path = sv_from_cstr("/");
        } else {
            out->path = rest;
        }
    }
    return 0;
}

static bool scheme_eq(String_View scheme, const char *name)
{
    size_t n = strlen(name);
    if (scheme.count != n) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (tolower((unsigned char)scheme.data[i]) != tolower((unsigned char)name[i])) {
            return false;
        }
    }
    return true;
}

bool uri_scheme_is(const Uri *u, const char *name)
{
    return scheme_eq(u->scheme, name);
}

int uri_default_port(String_View scheme)
{
    if (scheme_eq(scheme, "http")) {
        return 80;
    }
    if (scheme_eq(scheme, "https")) {
        return 443;
    }
    return -1;
}