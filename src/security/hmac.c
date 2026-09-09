#include "hmac.h"

#include <stdint.h>
#include <string.h>

// ---------------------------------------------------------------------------
// SHA-256
// ---------------------------------------------------------------------------

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define SHR(x, n) ((x) >> (n))
#define CH(x, y, z) (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define SIG0(x) (ROTR(x, 2) ^ ROTR(x, 13) ^ ROTR(x, 22))
#define SIG1(x) (ROTR(x, 6) ^ ROTR(x, 11) ^ ROTR(x, 25))
#define sig0(x) (ROTR(x, 7) ^ ROTR(x, 18) ^ SHR(x, 3))
#define sig1(x) (ROTR(x, 17) ^ ROTR(x, 19) ^ SHR(x, 10))

static const uint32_t K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu,
    0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u,
    0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u,
    0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u,
    0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u,
    0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u,
    0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu,
    0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

typedef struct {
    uint32_t h[8];
    uint64_t bytes;        // message bytes processed so far
    unsigned char block[64];
    size_t used;
} Sha256_Ctx;

static void sha256_init(Sha256_Ctx *ctx)
{
    ctx->h[0] = 0x6a09e667u;
    ctx->h[1] = 0xbb67ae85u;
    ctx->h[2] = 0x3c6ef372u;
    ctx->h[3] = 0xa54ff53au;
    ctx->h[4] = 0x510e527fu;
    ctx->h[5] = 0x9b05688cu;
    ctx->h[6] = 0x1f83d9abu;
    ctx->h[7] = 0x5be0cd19u;
    ctx->bytes = 0;
    ctx->used = 0;
}

static uint32_t load_be32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | ((uint32_t)p[3]);
}

static void store_be32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

static void sha256_compress(Sha256_Ctx *ctx, const unsigned char *block)
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = load_be32(block + 4 * i);
    }
    for (int i = 16; i < 64; i++) {
        w[i] = sig1(w[i - 2]) + w[i - 7] + sig0(w[i - 15]) + w[i - 16];
    }

    uint32_t a = ctx->h[0], b = ctx->h[1], c = ctx->h[2], d = ctx->h[3];
    uint32_t e = ctx->h[4], f = ctx->h[5], g = ctx->h[6], h = ctx->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + SIG1(e) + CH(e, f, g) + K[i] + w[i];
        uint32_t t2 = SIG0(a) + MAJ(a, b, c);
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    ctx->h[0] += a;
    ctx->h[1] += b;
    ctx->h[2] += c;
    ctx->h[3] += d;
    ctx->h[4] += e;
    ctx->h[5] += f;
    ctx->h[6] += g;
    ctx->h[7] += h;
}

static void sha256_update(Sha256_Ctx *ctx, const unsigned char *data, size_t len)
{
    ctx->bytes += len;
    while (len > 0) {
        size_t take = len;
        if (take > sizeof(ctx->block) - ctx->used) {
            take = sizeof(ctx->block) - ctx->used;
        }
        memcpy(ctx->block + ctx->used, data, take);
        ctx->used += take;
        data += take;
        len -= take;
        if (ctx->used == sizeof(ctx->block)) {
            sha256_compress(ctx, ctx->block);
            ctx->used = 0;
        }
    }
}

static void sha256_final(Sha256_Ctx *ctx, unsigned char out[32])
{
    uint64_t bits = ctx->bytes * 8ULL;

    ctx->block[ctx->used++] = 0x80;
    if (ctx->used > 56) {
        while (ctx->used < sizeof(ctx->block)) {
            ctx->block[ctx->used++] = 0;
        }
        sha256_compress(ctx, ctx->block);
        ctx->used = 0;
    }
    while (ctx->used < 56) {
        ctx->block[ctx->used++] = 0;
    }
    for (int i = 0; i < 8; i++) {
        ctx->block[56 + i] = (unsigned char)(bits >> (56 - 8 * i));
    }
    sha256_compress(ctx, ctx->block);

    for (int i = 0; i < 8; i++) {
        store_be32(out + 4 * i, ctx->h[i]);
    }
}

void sha256(const unsigned char *data, size_t len, unsigned char out[32])
{
    Sha256_Ctx ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, data, len);
    sha256_final(&ctx, out);
}

// ---------------------------------------------------------------------------
// HMAC-SHA256
// ---------------------------------------------------------------------------

void hmac_sha256(const unsigned char *key, size_t key_len,
                 const unsigned char *msg, size_t msg_len,
                 unsigned char mac[32])
{
    unsigned char k[64];
    memset(k, 0, sizeof(k));
    if (key_len <= sizeof(k)) {
        memcpy(k, key, key_len);
    } else {
        sha256(key, key_len, k); // digests into the first 32 bytes, rest stays 0
    }

    unsigned char ipad[64], opad[64];
    for (int i = 0; i < 64; i++) {
        ipad[i] = k[i] ^ 0x36;
        opad[i] = k[i] ^ 0x5c;
    }

    Sha256_Ctx ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, ipad, sizeof(ipad));
    sha256_update(&ctx, msg, msg_len);
    unsigned char inner[32];
    sha256_final(&ctx, inner);

    sha256_init(&ctx);
    sha256_update(&ctx, opad, sizeof(opad));
    sha256_update(&ctx, inner, sizeof(inner));
    sha256_final(&ctx, mac);
}

void hmac_sha256_hex(const unsigned char *key, size_t key_len,
                     const unsigned char *msg, size_t msg_len,
                     char *hex, size_t hex_size)
{
    unsigned char mac[32];
    hmac_sha256(key, key_len, msg, msg_len, mac);

    static const char digits[] = "0123456789abcdef";
    if (hex_size >= 65) {
        for (int i = 0; i < 32; i++) {
            hex[2 * i] = digits[mac[i] >> 4];
            hex[2 * i + 1] = digits[mac[i] & 0x0f];
        }
        hex[64] = '\0';
    }
}

int secure_byte_equal(const unsigned char *a, const unsigned char *b, size_t n)
{
    unsigned char diff = 0;
    for (size_t i = 0; i < n; i++) {
        diff |= a[i] ^ b[i];
    }
    return diff == 0;
}