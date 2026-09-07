#include "base64.h"

#include <stdint.h>

#include "strbuf.h"
#include "sv.h"

static const char *const B64 =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

void base64_encode_into(Strbuf *sb, String_View d)
{
    size_t i = 0;
    for (; i + 3 <= d.count; i += 3) {
        uint32_t v = ((uint32_t)(unsigned char)d.data[i] << 16) |
                     ((uint32_t)(unsigned char)d.data[i + 1] << 8) |
                     (uint32_t)(unsigned char)d.data[i + 2];
        char out[4] = {
            B64[(v >> 18) & 0x3F],
            B64[(v >> 12) & 0x3F],
            B64[(v >> 6) & 0x3F],
            B64[v & 0x3F],
        };
        strbuf_append(sb, out, 4);
    }

    size_t rem = d.count - i;
    if (rem == 1) {
        uint32_t v = (uint32_t)d.data[i] << 16;
        char out[4] = {
            B64[(v >> 18) & 0x3F],
            B64[(v >> 12) & 0x3F],
            '=',
            '=',
        };
        strbuf_append(sb, out, 4);
    } else if (rem == 2) {
        uint32_t v = ((uint32_t)(unsigned char)d.data[i] << 16) |
                     ((uint32_t)(unsigned char)d.data[i + 1] << 8);
        char out[4] = {
            B64[(v >> 18) & 0x3F],
            B64[(v >> 12) & 0x3F],
            B64[(v >> 6) & 0x3F],
            '=',
        };
        strbuf_append(sb, out, 4);
    }
}

int base64_decode_into(Strbuf *sb, String_View s)
{
    size_t end = s.count;
    while (end > 0 && s.data[end - 1] == '=') {
        end--; // padding only affects the length
    }
    if (end % 4 == 1) {
        return -1; // leftover single base64 char can never carry a byte
    }

    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < end; i++) {
        int v = b64val(s.data[i]);
        if (v < 0) {
            return -1;
        }
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (strbuf_append_char(sb, (char)((acc >> bits) & 0xFF)) != 0) {
                return -1;
            }
            acc &= (1u << bits) - 1; // drop the consumed high bits
        }
    }
    // the unused padding bits of the last group must be zero
    if (bits > 0 && (acc & ((1u << bits) - 1)) != 0) {
        return -1;
    }
    return 0;
}