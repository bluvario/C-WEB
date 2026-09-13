#include "password.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "base64.h"
#include "hmac.h"
#include "strbuf.h"
#include "sv.h"
#include "xmem.h"

int cweb_secure_random_bytes(unsigned char *out, size_t n)
{
#ifndef _WIN32
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) {
        return -1;
    }
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, out + got, n - got);
        if (r < 0) {
            close(fd);
            return -1;
        }
        if (r == 0) {
            close(fd);
            return -1;
        }
        got += (size_t)r;
    }
    close(fd);
    return 0;
#else
    (void)out;
    (void)n;
    return -1;
#endif
}

// PBKDF2-HMAC-SHA256 (RFC 2898) on top of the framework's own hmac_sha256, so
// hashes work identically whether or not OpenSSL is in the build. out_len
// must be <= (2^32 - 1) * 32; practical callers never approach that.
static void pbkdf2_hmac_sha256(const unsigned char *pw, size_t pw_len,
                               const unsigned char *salt, size_t salt_len,
                               unsigned iterations,
                               unsigned char *out, size_t out_len)
{
    unsigned char u[32];
    unsigned char block[32];

    // the per-block first message is salt || INT(block_index, 32-bit BE)
    size_t first_len = salt_len + 4;
    unsigned char *first = xmalloc(first_len);
    memcpy(first, salt, salt_len);

    unsigned char *cursor = out;
    size_t remaining = out_len;
    for (unsigned block_index = 1; remaining > 0; block_index++) {
        first[salt_len + 0] = (unsigned char)((block_index >> 24) & 0xff);
        first[salt_len + 1] = (unsigned char)((block_index >> 16) & 0xff);
        first[salt_len + 2] = (unsigned char)((block_index >> 8) & 0xff);
        first[salt_len + 3] = (unsigned char)(block_index & 0xff);

        hmac_sha256(pw, pw_len, first, first_len, u);   // u = U1
        size_t chunk = remaining < 32 ? remaining : 32;
        for (size_t k = 0; k < chunk; k++) {
            cursor[k] = u[k];                           // T = U1
        }
        for (unsigned round = 1; round < iterations; round++) {
            hmac_sha256(pw, pw_len, u, 32, block);      // block = U_{round+1}
            for (size_t k = 0; k < 32; k++) {
                u[k] = block[k];
            }
            for (size_t k = 0; k < chunk; k++) {
                cursor[k] ^= u[k];                      // T ^= U_{round+1}
            }
        }
        cursor += chunk;
        remaining -= chunk;
    }
    xfree(first);
}

// splits an encoded record "pbkdf2$<iters>$<salt>$<hash>" into its three
// fields; returns 0 with the whole record's prefix consumed and each *part
// pointing into encoded, -1 on a malformed shape.
static int parse_record(String_View encoded,
                        String_View *iters, String_View *salt, String_View *hash)
{
    String_View scheme = sv_chop_by_delim(&encoded, '$');
    *iters = sv_chop_by_delim(&encoded, '$');
    *salt = sv_chop_by_delim(&encoded, '$');
    *hash = encoded;
    return sv_equal(scheme, sv_from_cstr("pbkdf2")) ? 0 : -1;
}

int cweb_password_hash(const char *password, unsigned iterations,
                       char *out, size_t out_size)
{
    if (iterations == 0) {
        iterations = CWEB_PBKDF2_ITERATIONS;
    }

    unsigned char salt[16];
    if (cweb_secure_random_bytes(salt, sizeof salt) != 0) {
        return -1;
    }

    unsigned char dk[32];
    const unsigned char *pw = (const unsigned char *)password;
    size_t pw_len = strlen(password);
    pbkdf2_hmac_sha256(pw, pw_len, salt, sizeof salt, iterations, dk, sizeof dk);

    Strbuf record;
    strbuf_init(&record);
    char iters_buf[24];
    snprintf(iters_buf, sizeof iters_buf, "%u", iterations);
    strbuf_append_cstr(&record, "pbkdf2$");
    strbuf_append_cstr(&record, iters_buf);
    strbuf_append_char(&record, '$');
    base64_encode_into(&record,
                       (String_View){(const char *)salt, sizeof salt});
    strbuf_append_char(&record, '$');
    base64_encode_into(&record,
                       (String_View){(const char *)dk, sizeof dk});
    strbuf_null_terminate(&record);

    int rc;
    if (record.count + 1 > out_size) {
        rc = -1;
    } else {
        memcpy(out, record.items, record.count + 1);
        rc = 0;
    }
    strbuf_free(&record);
    return rc;
}

int cweb_password_verify(const char *password, const char *encoded)
{
    if (encoded == NULL || password == NULL) {
        return 0;
    }

    String_View sv = sv_from_cstr(encoded);
    String_View iters_sv, salt_sv, hash_sv;
    if (parse_record(sv, &iters_sv, &salt_sv, &hash_sv) != 0) {
        return 0;
    }

    long long iterations = 0;
    if (!sv_to_i64(iters_sv, &iterations) ||
        iterations < 1 || iterations > 100000000LL) {
        return 0;
    }

    // the payload the scheme produces is a 32-byte SHA-256 digest; hashes we
    // create tag a 16-byte salt (24 b64 chars), but verify() stays honest and
    // accepts any non-empty salt a record carries — the parameters travel in
    // the string
    Strbuf salt_decoded, hash_decoded;
    strbuf_init(&salt_decoded);
    strbuf_init(&hash_decoded);
    if (base64_decode_into(&salt_decoded, salt_sv) != 0 ||
        base64_decode_into(&hash_decoded, hash_sv) != 0 ||
        salt_decoded.count == 0 || hash_decoded.count != 32) {
        strbuf_free(&salt_decoded);
        strbuf_free(&hash_decoded);
        return 0;
    }

    unsigned char rederived[32];
    const unsigned char *pw = (const unsigned char *)password;
    pbkdf2_hmac_sha256(pw, strlen(password),
                       (const unsigned char *)salt_decoded.items,
                       salt_decoded.count, (unsigned)iterations,
                       rederived, sizeof rederived);

    int ok = secure_byte_equal(rederived,
                               (const unsigned char *)hash_decoded.items,
                               sizeof rederived);
    strbuf_free(&salt_decoded);
    strbuf_free(&hash_decoded);
    return ok;
}