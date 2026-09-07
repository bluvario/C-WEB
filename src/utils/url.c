#include "url.h"

#include "strbuf.h"
#include "sv.h"

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int url_decode_into(Strbuf *sb, String_View src)
{
    for (size_t i = 0; i < src.count; i++) {
        char c = src.data[i];
        if (c == '%') {
            if (i + 2 >= src.count) {
                return -1;
            }
            int hi = hexval(src.data[i + 1]);
            int lo = hexval(src.data[i + 2]);
            if (hi < 0 || lo < 0) {
                return -1;
            }
            if (strbuf_append_char(sb, (char)(hi * 16 + lo)) != 0) {
                return -1;
            }
            i += 2;
        } else if (c == '+') {
            if (strbuf_append_char(sb, ' ') != 0) {
                return -1;
            }
        } else {
            if (strbuf_append_char(sb, c) != 0) {
                return -1;
            }
        }
    }
    return 0;
}

// RFC 3986 unreserved: ALPHA / DIGIT / "-" / "." / "_" / "~"
static bool unreserved(char c)
{
    return (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') ||
           c == '-' || c == '.' || c == '_' || c == '~';
}

void url_encode_into(Strbuf *sb, String_View src)
{
    static const char *const HEX = "0123456789ABCDEF";
    for (size_t i = 0; i < src.count; i++) {
        unsigned char c = (unsigned char)src.data[i];
        if (unreserved((char)c)) {
            strbuf_append_char(sb, (char)c);
        } else {
            strbuf_append_char(sb, '%');
            strbuf_append_char(sb, HEX[(c >> 4) & 0xF]);
            strbuf_append_char(sb, HEX[c & 0xF]);
        }
    }
}