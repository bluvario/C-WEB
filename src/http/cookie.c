#include "cookie.h"

#include <stdio.h>

#include "strbuf.h"
#include "sv.h"
#include "xmem.h"

void http_response_set_cookie(Http_Response *res, const char *name, const char *value,
                              const Cookie_Attrs *attrs)
{
    Strbuf h;
    strbuf_init(&h);
    strbuf_append_cstr(&h, name);
    strbuf_append_char(&h, '=');
    strbuf_append_cstr(&h, value);

    if (attrs != NULL) {
        if (attrs->path != NULL) {
            strbuf_append_cstr(&h, "; Path=");
            strbuf_append_cstr(&h, attrs->path);
        }
        if (attrs->max_age >= 0) {
            char buf[32];
            snprintf(buf, sizeof(buf), "; Max-Age=%d", attrs->max_age);
            strbuf_append_cstr(&h, buf);
        }
        if (attrs->http_only) {
            strbuf_append_cstr(&h, "; HttpOnly");
        }
        // SameSite=None is refused by browsers without Secure, so one cannot
        // ask for it without taking the other
        bool secure = attrs->secure || attrs->same_site == COOKIE_SAMESITE_NONE;
        if (secure) {
            strbuf_append_cstr(&h, "; Secure");
        }
        switch (attrs->same_site) {
        case COOKIE_SAMESITE_NONE:
            strbuf_append_cstr(&h, "; SameSite=None");
            break;
        case COOKIE_SAMESITE_LAX:
            strbuf_append_cstr(&h, "; SameSite=Lax");
            break;
        case COOKIE_SAMESITE_STRICT:
            strbuf_append_cstr(&h, "; SameSite=Strict");
            break;
        case COOKIE_SAMESITE_DEFAULT:
            break;
        }
    }

    strbuf_null_terminate(&h);
    http_response_set_header(res, "Set-Cookie", h.items);
    strbuf_free(&h);
}

void http_cookie_parse(Str_Map *jar, String_View header)
{
    String_View rest = header;
    while (rest.count > 0) {
        String_View pair = sv_chop_by_delim(&rest, ';');
        pair = sv_trim(pair);
        if (pair.count == 0) {
            continue;
        }
        String_View name = sv_chop_by_delim(&pair, '=');
        if (name.count == 0) {
            continue;
        }
        String_View value = sv_trim(pair);
        // cookie values may arrive quoted, RFC 6265 sends them bare but older
        // clients wrap them up
        if (value.count >= 2 && value.data[0] == '"' && value.data[value.count - 1] == '"') {
            value.data++;
            value.count -= 2;
        }
        strmap_set(jar, sv_trim(name), value);
    }
}