#include <stdio.h>
#include <string.h>

#include "hmac.h"

static int check(const char *what, int cond)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", what);
        return 1;
    }
    return 0;
}

static void digest_hex(const unsigned char *mac, size_t n, char out[65])
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = digits[mac[i] >> 4];
        out[2 * i + 1] = digits[mac[i] & 0x0f];
    }
    out[2 * n] = '\0';
}

static int has_known_vector(const unsigned char mac[32], const char *expect)
{
    char hex[65];
    digest_hex(mac, 32, hex);
    return strcmp(hex, expect) == 0;
}

int main(void)
{
    int fails = 0;
    unsigned char mac[32];
    char hex[65];

    // FIPS 180-4 examples
    sha256((const unsigned char *)"", 0, mac);
    fails += check("sha256(\"\")",
                   has_known_vector(mac,
                       "e3b0c44298fc1c149afbf4c8996fb924"
                       "27ae41e4649b934ca495991b7852b855"));
    sha256((const unsigned char *)"abc", 3, mac);
    fails += check("sha256(abc)",
                   has_known_vector(mac,
                       "ba7816bf8f01cfea414140de5dae2223"
                       "b00361a396177a9cb410ff61f20015ad"));
    sha256((const unsigned char *)"The quick brown fox jumps over the lazy dog", 43, mac);
    fails += check("sha256(fox dog)",
                   has_known_vector(mac,
                       "d7a8fbb307d7809469ca9abcb0082e4f"
                       "8d5651e46d3cdb762d02d0bf37c9e592"));

    // multi-block message so the 64-byte compression path is exercised
    static const char many[] =
        "the quick brown fox jumps over the lazy dog, the quick brown fox "
        "jumps over the lazy dog, the quick brown fox jumps over the lazy "
        "dog. cryptography is easy; doing it right is not.";
    sha256((const unsigned char *)many, strlen(many), mac);
    fails += check("sha256(multi-block)",
                   has_known_vector(mac,
                       "4ebafb1ebfeb9860d46451aa6f44bfef"
                       "47e9d54c8bef1ad332838dc659f47b2d"));

    // RFC 4231 HMAC-SHA256 test cases
    unsigned char key1[20], data3[50];
    memset(key1, 0x0b, sizeof(key1));
    hmac_sha256(key1, sizeof(key1), (const unsigned char *)"Hi There", 8, mac);
    fails += check("hmac R4231 case 1",
                   has_known_vector(mac,
                       "b0344c61d8db38535ca8afceaf0bf12b"
                       "881dc200c9833da726e9376c2e32cff7"));

    hmac_sha256((const unsigned char *)"Jefe", 4,
                (const unsigned char *)"what do ya want for nothing?", 28, mac);
    fails += check("hmac R4231 case 2",
                   has_known_vector(mac,
                       "5bdcc146bf60754e6a042426089575c7"
                       "5a003f089d2739839dec58b964ec3843"));

    memset(key1, 0xaa, sizeof(key1));
    memset(data3, 0xdd, sizeof(data3));
    hmac_sha256(key1, sizeof(key1), data3, sizeof(data3), mac);
    fails += check("hmac R4231 case 3",
                   has_known_vector(mac,
                       "773ea91e36800e46854db8ebd09181a7"
                       "2959098b3ef8c122d9635514ced565fe"));

    // long key: hashed down to 32 bytes per RFC 2104
    unsigned char longkey[128];
    memset(longkey, 0xaa, sizeof(longkey));
    hmac_sha256(longkey, sizeof(longkey),
                (const unsigned char *)"Test Using Larger Than Block-Size Key - "
                                       "Hash Key First",
                54, mac);
    fails += check("hmac long key",
                   has_known_vector(mac,
                       "f4c628398866742a99f3e2550d7f6ca1"
                       "35a8995a3940a190d75636a4fe27d788"));

    // hex form
    hmac_sha256_hex((const unsigned char *)"key", 3,
                    (const unsigned char *)"The quick brown fox jumps over the lazy dog",
                    43, hex, sizeof(hex));
    fails += check("hmac hex known",
                   strcmp(hex,
                       "f7bc83f430538424b13298e6aa6fb143"
                       "ef4d59a14946175997479dbc2d1a3cd8") == 0);
    fails += check("hex is 64 chars then NUL", strlen(hex) == 64);

    unsigned char a[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    unsigned char b[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    unsigned char c[8] = {1, 2, 3, 4, 5, 6, 7, 9};
    fails += check("secure_byte_equal equal", secure_byte_equal(a, b, 8) == 1);
    fails += check("secure_byte_equal differ", secure_byte_equal(a, c, 8) == 0);

    if (fails == 0) {
        printf("hmac ok\n");
    }
    return fails ? 1 : 0;
}