#include <stdio.h>
#include <string.h>

#include "base64.h"
#include "password.h"
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

// decodes a hex string (lowercase ASCII) into out
static void hex_decode(const char *hex, unsigned char *out)
{
    size_t n = strlen(hex) / 2;
    for (size_t i = 0; i < n; i++) {
        unsigned int byte;
        sscanf(hex + 2 * i, "%2x", &byte);
        out[i] = (unsigned char)byte;
    }
}

int main(void)
{
    int fails = 0;

    // self-consistency: a hash verifies against its own password
    char rec[CWEB_PASSWORD_HASH_MAX];
    fails += check("hash ok", cweb_password_hash("correct horse", 1000,
                                                 rec, sizeof rec) == 0);
    fails += check("record shape", strncmp(rec, "pbkdf2$1000$", 12) == 0);
    fails += check("short record", strlen(rec) < sizeof rec);
    fails += check("verify own", cweb_password_verify("correct horse", rec) == 1);
    fails += check("verify wrong pw", cweb_password_verify("wrong pw", rec) == 0);
    fails += check("verify empty pw", cweb_password_verify("", rec) == 0);
    fails += check("empty password roundtrip",
                   cweb_password_hash("", 1000, rec, sizeof rec) == 0 &&
                   cweb_password_verify("", rec) == 1);

    // a published vector: RFC 7914 section 11, PBKDF2-HMAC-SHA-256 with
    // password "passwd", salt "salt", c=1, dkLen=32 (verified against the
    // local OpenSSL implementation). "salt" encodes to c2FsdA==.
    unsigned char want_digest[32];
    hex_decode("55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc",
               want_digest);
    Strbuf record;
    strbuf_init(&record);
    strbuf_append_cstr(&record, "pbkdf2$1$c2FsdA==$");
    base64_encode_into(&record,
                       (String_View){(const char *)want_digest, sizeof want_digest});
    strbuf_null_terminate(&record);
    fails += check("RFC 7914 vector",
                   cweb_password_verify("passwd", record.items) == 1);
    fails += check("vector wrong pw",
                   cweb_password_verify("Password", record.items) == 0);
    strbuf_free(&record);

    // tampered records must never pass
    char mut[sizeof rec];
    memcpy(mut, rec, sizeof rec);

    char *iters = strchr(mut, '$');
    if (iters != NULL) {
        iters[1]++; // pbkdf2$1001$... now
        fails += check("tampered iterations",
                       cweb_password_verify("correct horse", mut) == 0);
    } else {
        fails += check("tampered iterations (shape)", 0);
    }

    memcpy(mut, rec, sizeof rec);
    size_t n = strlen(mut);
    mut[n - 1] = (char)(mut[n - 1] == 'A' ? 'B' : 'A'); // flip a digest char
    fails += check("tampered digest", cweb_password_verify("correct horse", mut) == 0);

    // malformed and foreign records are rejected, not trusted
    fails += check("garbage", cweb_password_verify("x", "pbkdf2$1$a$b") == 0);
    fails += check("bad scheme", cweb_password_verify("x", "sha1$1$aa$bb") == 0);
    fails += check("missing fields", cweb_password_verify("x", "pbkdf2$1$aa") == 0);
    fails += check("bad b64", cweb_password_verify("x", "pbkdf2$1$!%!@$!!!") == 0);
    fails += check("zero iterations", cweb_password_verify("x", "pbkdf2$0$c2FsdA==$c2FsdA==") == 0);

    // the default work factor path is the same code, just expensive
    fails += check("hash default iters",
                   cweb_password_hash("default", 0, rec, sizeof rec) == 0);
    fails += check("verify default iters", cweb_password_verify("default", rec) == 1);

    // tiny destination must be refused, not overflowed
    char tiny[12];
    fails += check("hash too small", cweb_password_hash("x", 100, tiny, sizeof tiny) == -1);

    fails += check("random bytes", cweb_secure_random_bytes((unsigned char *)mut, 8) == 0);

    if (fails == 0) {
        printf("password ok\n");
    }
    return fails != 0;
}