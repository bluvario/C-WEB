#include "escape.h"

#include "strbuf.h"
#include "sv.h"

void html_escape_into(Strbuf *sb, String_View src)
{
    for (size_t i = 0; i < src.count; i++) {
        char c = src.data[i];
        switch (c) {
        case '&':
            strbuf_append_cstr(sb, "&amp;");
            break;
        case '<':
            strbuf_append_cstr(sb, "&lt;");
            break;
        case '>':
            strbuf_append_cstr(sb, "&gt;");
            break;
        case '"':
            strbuf_append_cstr(sb, "&quot;");
            break;
        case '\'':
            strbuf_append_cstr(sb, "&#39;");
            break;
        default:
            strbuf_append_char(sb, c);
            break;
        }
    }
}