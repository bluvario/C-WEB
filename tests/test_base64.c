#include <stdio.h>
#include <string.h>

#include "base64.h"
#include "strbuf.h"
#include "sv.h"

static int check(const char *what, int cond)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        return 1;
    }
    return 0;
}

static int enc_ok(Strbuf *sb, const char *in, const char *want)
{
    strbuf_free(sb);
    strbuf_init(sb);
    base64_encode_into(sb, sv_from_cstr(in));
    if (strbuf_null_terminate(sb) != 0) return 0;
    return strcmp(sb->items, want) == 0;
}

static int dec_ok(Strbuf *sb, const char *in, const char *want)
{
    strbuf_free(sb);
    strbuf_init(sb);
    if (base64_decode_into(sb, sv_from_cstr(in)) != 0) return 0;
    if (strbuf_null_terminate(sb) != 0) return 0;
    return strcmp(sb->items, want) == 0;
}

int main(void)
{
    int fails = 0;
    Strbuf sb;
    strbuf_init(&sb);

    // RFC 4648 test vectors
    fails += check("enc m", enc_ok(&sb, "m", "bQ=="));
    fails += check("enc ma", enc_ok(&sb, "ma", "bWE="));
    fails += check("enc man", enc_ok(&sb, "man", "bWFu"));
    fails += check("enc hello", enc_ok(&sb, "hello", "aGVsbG8="));

    fails += check("dec bQ==", dec_ok(&sb, "bQ==", "m"));
    fails += check("dec bWE=", dec_ok(&sb, "bWE=", "ma"));
    fails += check("dec unpadded", dec_ok(&sb, "bWFu", "man"));

    // binary roundtrip, including NUL bytes
    unsigned char blob[256];
    for (int i = 0; i < 256; i++) {
        blob[i] = (unsigned char)i;
    }
    strbuf_free(&sb);
    strbuf_init(&sb);
    base64_encode_into(&sb, (String_View){.data = (const char *)blob, .count = 256});
    Strbuf back;
    strbuf_init(&back);
    fails += check("binary decodes", base64_decode_into(&back, (String_View){.data = sb.items, .count = sb.count}) == 0);
    fails += check("binary roundtrips", back.count == 256 && memcmp(back.items, blob, 256) == 0);

    fails += check("garbage rejected", base64_decode_into(&sb, sv_from_cstr("a!sb")) == -1);
    fails += check("lone char rejected", base64_decode_into(&sb, sv_from_cstr("a===")) == -1);
    fails += check("nonzero padding rejected", base64_decode_into(&sb, sv_from_cstr("ab==")) == -1);

    strbuf_free(&sb);
    strbuf_free(&back);

    if (fails == 0) {
        printf("base64 ok\n");
    }
    return fails != 0;
}