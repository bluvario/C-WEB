#include <stdio.h>
#include <string.h>

#include "sv.h"
#include "uri.h"

static int check(const char *what, int cond)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        return 1;
    }
    return 0;
}

static int uri_eq(const Uri *u, const char *scheme, const char *host,
                  const char *port, const char *path, const char *query,
                  const char *fragment)
{
    return (scheme == NULL || sv_equal(u->scheme, sv_from_cstr(scheme))) &&
           sv_equal(u->host, sv_from_cstr(host)) &&
           (port == NULL || sv_equal(u->port, sv_from_cstr(port))) &&
           sv_equal(u->path, sv_from_cstr(path)) &&
           (query == NULL || sv_equal(u->query, sv_from_cstr(query))) &&
           (fragment == NULL || sv_equal(u->fragment, sv_from_cstr(fragment)));
}

static int parse_ok(const char *text)
{
    Uri u;
    return uri_parse(sv_from_cstr(text), &u) == 0;
}

static int parse_rejects(const char *text)
{
    Uri u;
    return uri_parse(sv_from_cstr(text), &u) != 0;
}

int main(void)
{
    int fails = 0;
    Uri u;

    u = (Uri){0};
    fails += check("scheme/authority/path/query/fragment",
                   uri_parse(sv_from_cstr("http://example.com:8080/a/b?x=1#top"), &u) == 0 &&
                   uri_eq(&u, "http", "example.com", "8080", "/a/b", "x=1", "top"));

    u = (Uri){0};
    fails += check("host without port or path",
                   uri_parse(sv_from_cstr("https://example.com"), &u) == 0 &&
                   uri_eq(&u, "https", "example.com", NULL, "/", NULL, NULL));

    u = (Uri){0};
    fails += check("bare path gets a scheme-less default",
                   uri_parse(sv_from_cstr("/only/path?q"), &u) == 0 &&
                   uri_eq(&u, NULL, "", NULL, "/only/path", "q", NULL));

    u = (Uri){0};
    fails += check("default port omission",
                   uri_parse(sv_from_cstr("http://localhost"), &u) == 0 &&
                   u.port.count == 0 && uri_scheme_is(&u, "http"));

    u = (Uri){0};
    fails += check("upper case scheme parsed",
                   uri_parse(sv_from_cstr("HTTP://host/x"), &u) == 0 &&
                   uri_scheme_is(&u, "http"));

    u = (Uri){0};
    fails += check("ipv6 literal host",
                   uri_parse(sv_from_cstr("https://[::1]:8443/x"), &u) == 0 &&
                   uri_eq(&u, "https", "::1", "8443", "/x", NULL, NULL));

    u = (Uri){0};
    fails += check("ipv6 literal without port",
                   uri_parse(sv_from_cstr("http://[2001:db8::1]/"), &u) == 0 &&
                   sv_equal(u.host, sv_from_cstr("2001:db8::1")));

    u = (Uri){0};
    fails += check("query survives without path",
                   uri_parse(sv_from_cstr("?debug=1"), &u) == 0 &&
                   sv_equal(u.path, sv_from_cstr("/")) &&
                   sv_equal(u.query, sv_from_cstr("debug=1")));

    // scheme helpers
    fails += check("default http port", uri_default_port(sv_from_cstr("http")) == 80);
    fails += check("default https port", uri_default_port(sv_from_cstr("https")) == 443);
    fails += check("unknown scheme port", uri_default_port(sv_from_cstr("ftp")) == -1);

    // malformed URIs must fail
    fails += check("empty host rejected", parse_rejects("http://"));
    fails += check("empty authority rejected", parse_rejects("http://:80/x"));
    fails += check("alpha port rejected", parse_rejects("http://h:abc/"));
    fails += check("oversized port rejected", parse_rejects("http://h:70000/"));
    u = (Uri){0};
    fails += check("unbracketed ipv6 rejected", parse_rejects("http://[::1/x"));
    fails += check("garbage in brackets rejected", parse_rejects("http://[::1]x/x"));

    // a colon without "://" is not a scheme marker; the text stays a path
    u = (Uri){0};
    fails += check("colon path stays relative",
                   uri_parse(sv_from_cstr("a:b/c"), &u) == 0 &&
                   sv_equal(u.path, sv_from_cstr("a:b/c")) &&
                   u.scheme.count == 0);

    fails += check("empty input is a valid root path", parse_ok(""));
    u = (Uri){0};
    fails += check("empty input decomposes to /",
                   uri_parse(sv_from_cstr(""), &u) == 0 &&
                   sv_equal(u.path, sv_from_cstr("/")));

    if (fails == 0) {
        printf("uri ok\n");
    }
    return fails != 0;
}