#ifndef CWEB_HMAC_H
#define CWEB_HMAC_H

#include <stddef.h>

// SHA-256 (FIPS 180-4) and HMAC-SHA256 (RFC 2104, test vectors in RFC 4231),
// implemented from the published algorithms so the framework stays
// dependency-free. semantics match OpenSSL's SHA256/HMAC-SHA256.

// SHA-256 of data; writes the 32-byte big-endian digest into out.
void sha256(const unsigned char *data, size_t len, unsigned char out[32]);

// HMAC-SHA256 keyed by key; writes 32 bytes into mac. every key length is
// legal (short keys are padded, long keys are hashed down as RFC 2104 says).
void hmac_sha256(const unsigned char *key, size_t key_len,
                 const unsigned char *msg, size_t msg_len,
                 unsigned char mac[32]);

// helper for text callers: fills hex with 64 lowercase hex characters and a
// terminating NUL (hex_size must be at least 65).
void hmac_sha256_hex(const unsigned char *key, size_t key_len,
                     const unsigned char *msg, size_t msg_len,
                     char *hex, size_t hex_size);

// compares two byte ranges in constant time: never returns on the first
// mismatch, so a side channel cannot learn how much of a guess was right.
// returns 1 when the ranges are identical, 0 otherwise.
int secure_byte_equal(const unsigned char *a, const unsigned char *b, size_t n);

#endif