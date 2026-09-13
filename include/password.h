#ifndef CWEB_PASSWORD_H
#define CWEB_PASSWORD_H

#include <stddef.h>

// stored-password hashing and verification. the framework ships no bcrypt or
// argon2, and OpenSSL is optional, so C-WEB implements PBKDF2-HMAC-SHA256 on
// top of its own hmac_sha256 — the same published algorithms the framework's
// other digests use — and stays dependency-free. strings have the form
//
//     pbkdf2$<iterations>$<salt_b64>$<hash_b64>
//
// where salt is 16 random bytes and the hash is the 32-byte PBKDF2 output, so
// the cost factor, salt and digest travel together in one field. verify()
// re-derives the digest from the stored salt/iterations and compares in
// constant time; it never reaches for the stored hash to "check" against, it
// recomputes what the stored parameters would produce — so a tampered record
// is a rejected login, not trust in the attacker's numbers.

// the default work factor when hash() is called with iterations == 0.
// 200000 is a deliberate balance: fast enough that a form POST stays snappy,
// slow enough that guessing a short password stays expensive.
#define CWEB_PBKDF2_ITERATIONS 200000u

// how much room hash() needs: scheme + up to 10 digits of iterations + two
// base64 payloads of 24 and 44 chars + separators + NUL.
#define CWEB_PASSWORD_HASH_MAX 128

// fills out (must hold CWEB_PASSWORD_HASH_MAX bytes) with an encoded hash for
// password. iterations == 0 selects CWEB_PBKDF2_ITERATIONS. returns 0 on
// success, -1 on failure (salt source error, or out too small).
int cweb_password_hash(const char *password, unsigned iterations,
                       char *out, size_t out_size);

// returns 1 when password matches the encoded hash (constant-time compare),
// 0 when it does not or the record is malformed/unknown. never relies on the
// stored scheme beyond "pbkdf2".
int cweb_password_verify(const char *password, const char *encoded);

// OS-quality randomness, used for salts self-contained (and safe to call
// elsewhere, e.g. to mint tokens). reads /dev/urandom. returns 0 on success,
// -1 when the source can be opened or read (POSIX only).
int cweb_secure_random_bytes(unsigned char *out, size_t n);

#endif