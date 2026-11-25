#include "ed25519_lite.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * Self-contained C99 implementation of:
 * - FIPS 180-4 SHA-256 and SHA-512
 * - RFC 8032 Ed25519 (TweetNaCl-style GF(2^255 - 19) radix-2^16 arithmetic)
 * - Unpadded Base64URL and key fingerprint formatting
 *
 * Zero heap allocations; safe on ESP32-S31 and in host unit tests.
 */

static inline uint32_t load32_be(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static inline void store32_be(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static inline void store64_be(uint8_t* p, uint64_t v) {
    store32_be(p, (uint32_t)(v >> 32));
    store32_be(p + 4, (uint32_t)v);
}

static inline uint64_t load64_be(const uint8_t* p) {
    return ((uint64_t)load32_be(p) << 32) | (uint64_t)load32_be(p + 4);
}

static inline uint32_t rotr32(uint32_t x, int n) {
    return (x >> n) | (x << (32 - n));
}

static inline uint64_t rotr64(uint64_t x, int n) {
    return (x >> n) | (x << (64 - n));
}

/* ================================ SHA-256 ================================ */

static const uint32_t kSha256K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u,
};

typedef struct {
    uint32_t state[8];
    uint64_t total_len;
    uint8_t buf[64];
    size_t buf_len;
} sha256_ctx_t;

static void sha256_compress(sha256_ctx_t* ctx, const uint8_t block[64]) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = load32_be(block + i * 4);
    }
    for (int i = 16; i < 64; ++i) {
        uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = ctx->state[0];
    uint32_t b = ctx->state[1];
    uint32_t c = ctx->state[2];
    uint32_t d = ctx->state[3];
    uint32_t e = ctx->state[4];
    uint32_t f = ctx->state[5];
    uint32_t g = ctx->state[6];
    uint32_t h = ctx->state[7];

    for (int i = 0; i < 64; ++i) {
        uint32_t s1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t temp1 = h + s1 + ch + kSha256K[i] + w[i];
        uint32_t s0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = s0 + maj;

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

static void sha256_init(sha256_ctx_t* ctx) {
    ctx->state[0] = 0x6a09e667u;
    ctx->state[1] = 0xbb67ae85u;
    ctx->state[2] = 0x3c6ef372u;
    ctx->state[3] = 0xa54ff53au;
    ctx->state[4] = 0x510e527fu;
    ctx->state[5] = 0x9b05688cu;
    ctx->state[6] = 0x1f83d9abu;
    ctx->state[7] = 0x5be0cd19u;
    ctx->total_len = 0;
    ctx->buf_len = 0;
}

static void sha256_update(sha256_ctx_t* ctx, const uint8_t* data, size_t len) {
    if (len == 0) {
        return;
    }
    ctx->total_len += len;
    if (ctx->buf_len > 0) {
        size_t fill = 64 - ctx->buf_len;
        if (len < fill) {
            memcpy(ctx->buf + ctx->buf_len, data, len);
            ctx->buf_len += len;
            return;
        }
        memcpy(ctx->buf + ctx->buf_len, data, fill);
        sha256_compress(ctx, ctx->buf);
        ctx->buf_len = 0;
        data += fill;
        len -= fill;
    }
    while (len >= 64) {
        sha256_compress(ctx, data);
        data += 64;
        len -= 64;
    }
    if (len > 0) {
        memcpy(ctx->buf, data, len);
        ctx->buf_len = len;
    }
}

static void sha256_final(sha256_ctx_t* ctx, uint8_t out[32]) {
    uint64_t total_bits = ctx->total_len * 8u;
    ctx->buf[ctx->buf_len++] = 0x80u;
    if (ctx->buf_len > 56) {
        while (ctx->buf_len < 64) {
            ctx->buf[ctx->buf_len++] = 0x00u;
        }
        sha256_compress(ctx, ctx->buf);
        ctx->buf_len = 0;
    }
    while (ctx->buf_len < 56) {
        ctx->buf[ctx->buf_len++] = 0x00u;
    }
    store64_be(ctx->buf + 56, total_bits);
    sha256_compress(ctx, ctx->buf);
    for (int i = 0; i < 8; ++i) {
        store32_be(out + i * 4, ctx->state[i]);
    }
}

void ez_sha256(const uint8_t* data, size_t len, uint8_t out[32]) {
    sha256_ctx_t ctx;
    sha256_init(&ctx);
    if (data != NULL && len > 0) {
        sha256_update(&ctx, data, len);
    }
    sha256_final(&ctx, out);
}

/* ================================ SHA-512 ================================ */

static const uint64_t kSha512K[80] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
    0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
    0xd807aa98a3030242ULL, 0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
    0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
    0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL, 0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
    0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
    0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
    0xd192e819d6ef5218ULL, 0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
    0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
    0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
    0xca273eceea26619cULL, 0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
    0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
    0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
    0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL,
};

typedef struct {
    uint64_t state[8];
    uint64_t total_len;
    uint8_t buf[128];
    size_t buf_len;
} sha512_ctx_t;

static void sha512_compress(sha512_ctx_t* ctx, const uint8_t block[128]) {
    uint64_t w[80];
    for (int i = 0; i < 16; ++i) {
        w[i] = load64_be(block + i * 8);
    }
    for (int i = 16; i < 80; ++i) {
        uint64_t s0 = rotr64(w[i - 15], 1) ^ rotr64(w[i - 15], 8) ^ (w[i - 15] >> 7);
        uint64_t s1 = rotr64(w[i - 2], 19) ^ rotr64(w[i - 2], 61) ^ (w[i - 2] >> 6);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint64_t a = ctx->state[0];
    uint64_t b = ctx->state[1];
    uint64_t c = ctx->state[2];
    uint64_t d = ctx->state[3];
    uint64_t e = ctx->state[4];
    uint64_t f = ctx->state[5];
    uint64_t g = ctx->state[6];
    uint64_t h = ctx->state[7];

    for (int i = 0; i < 80; ++i) {
        uint64_t s1 = rotr64(e, 14) ^ rotr64(e, 18) ^ rotr64(e, 41);
        uint64_t ch = (e & f) ^ ((~e) & g);
        uint64_t temp1 = h + s1 + ch + kSha512K[i] + w[i];
        uint64_t s0 = rotr64(a, 28) ^ rotr64(a, 34) ^ rotr64(a, 39);
        uint64_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint64_t temp2 = s0 + maj;

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

static void sha512_init(sha512_ctx_t* ctx) {
    ctx->state[0] = 0x6a09e667f3bcc908ULL;
    ctx->state[1] = 0xbb67ae8584caa73bULL;
    ctx->state[2] = 0x3c6ef372fe94f82bULL;
    ctx->state[3] = 0xa54ff53a5f1d36f1ULL;
    ctx->state[4] = 0x510e527fade682d1ULL;
    ctx->state[5] = 0x9b05688c2b3e6c1fULL;
    ctx->state[6] = 0x1f83d9abfb41bd6bULL;
    ctx->state[7] = 0x5be0cd19137e2179ULL;
    ctx->total_len = 0;
    ctx->buf_len = 0;
}

static void sha512_update(sha512_ctx_t* ctx, const uint8_t* data, size_t len) {
    if (len == 0) {
        return;
    }
    ctx->total_len += len;
    if (ctx->buf_len > 0) {
        size_t fill = 128 - ctx->buf_len;
        if (len < fill) {
            memcpy(ctx->buf + ctx->buf_len, data, len);
            ctx->buf_len += len;
            return;
        }
        memcpy(ctx->buf + ctx->buf_len, data, fill);
        sha512_compress(ctx, ctx->buf);
        ctx->buf_len = 0;
        data += fill;
        len -= fill;
    }
    while (len >= 128) {
        sha512_compress(ctx, data);
        data += 128;
        len -= 128;
    }
    if (len > 0) {
        memcpy(ctx->buf, data, len);
        ctx->buf_len = len;
    }
}

static void sha512_final(sha512_ctx_t* ctx, uint8_t out[64]) {
    uint64_t hi_bits = ctx->total_len >> 61;
    uint64_t lo_bits = ctx->total_len << 3;
    ctx->buf[ctx->buf_len++] = 0x80u;
    if (ctx->buf_len > 112) {
        while (ctx->buf_len < 128) {
            ctx->buf[ctx->buf_len++] = 0x00u;
        }
        sha512_compress(ctx, ctx->buf);
        ctx->buf_len = 0;
    }
    while (ctx->buf_len < 112) {
        ctx->buf[ctx->buf_len++] = 0x00u;
    }
    store64_be(ctx->buf + 112, hi_bits);
    store64_be(ctx->buf + 120, lo_bits);
    sha512_compress(ctx, ctx->buf);
    for (int i = 0; i < 8; ++i) {
        store64_be(out + i * 8, ctx->state[i]);
    }
}

void ez_sha512(const uint8_t* data, size_t len, uint8_t out[64]) {
    sha512_ctx_t ctx;
    sha512_init(&ctx);
    if (data != NULL && len > 0) {
        sha512_update(&ctx, data, len);
    }
    sha512_final(&ctx, out);
}

/* ================================ Ed25519 ================================ */

typedef int64_t gf[16];

static const gf kGf0 = {0};
static const gf kGf1 = {1};
static const gf kD2 = {0xf159, 0x26b2, 0x9b94, 0xebd6, 0xb156, 0x8283, 0x149a, 0x00e0,
                        0xd130, 0xeef3, 0x80f2, 0x198e, 0xfce7, 0x56df, 0xd9dc, 0x2406};
static const gf kBaseX = {0xd51a, 0x8f25, 0x2d60, 0xc956, 0xa7b2, 0x9525, 0xc760, 0x692c,
                          0xdc5c, 0xfdd6, 0xe231, 0xc0a4, 0x53fe, 0xcd6e, 0x36d3, 0x2169};
static const gf kBaseY = {0x6658, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666,
                          0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666};

static const uint64_t kOrderL[32] = {
    0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7,
    0xa2, 0xde, 0xf9, 0xde, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10,
};

static void gf_set(gf o, const gf a) {
    for (int i = 0; i < 16; ++i) {
        o[i] = a[i];
    }
}

static void gf_carry(gf o) {
    for (int i = 0; i < 16; ++i) {
        o[i] += (1LL << 16);
        int64_t c = o[i] >> 16;
        o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
        o[i] -= c * 65536LL;
    }
}

static void gf_sel(gf p, gf q, int b) {
    int64_t c = ~(b - 1);
    for (int i = 0; i < 16; ++i) {
        int64_t t = c & (p[i] ^ q[i]);
        p[i] ^= t;
        q[i] ^= t;
    }
}

static void gf_add(gf o, const gf a, const gf b) {
    for (int i = 0; i < 16; ++i) {
        o[i] = a[i] + b[i];
    }
}

static void gf_sub(gf o, const gf a, const gf b) {
    for (int i = 0; i < 16; ++i) {
        o[i] = a[i] - b[i];
    }
}

static void gf_mul(gf o, const gf a, const gf b) {
    int64_t t[31];
    for (int i = 0; i < 31; ++i) {
        t[i] = 0;
    }
    for (int i = 0; i < 16; ++i) {
        for (int j = 0; j < 16; ++j) {
            t[i + j] += a[i] * b[j];
        }
    }
    for (int i = 0; i < 15; ++i) {
        t[i] += 38 * t[i + 16];
    }
    for (int i = 0; i < 16; ++i) {
        o[i] = t[i];
    }
    gf_carry(o);
    gf_carry(o);
}

static void gf_sqr(gf o, const gf a) {
    gf_mul(o, a, a);
}

static void gf_inv(gf o, const gf i) {
    gf c;
    gf_set(c, i);
    for (int a = 253; a >= 0; --a) {
        gf_sqr(c, c);
        if (a != 2 && a != 4) {
            gf_mul(c, c, i);
        }
    }
    gf_set(o, c);
}

static void gf_pack(uint8_t* o, const gf n) {
    gf m;
    gf t;
    gf_set(t, n);
    gf_carry(t);
    gf_carry(t);
    gf_carry(t);
    for (int j = 0; j < 2; ++j) {
        m[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; ++i) {
            m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
            m[i - 1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        int b = (int)((m[15] >> 16) & 1);
        m[14] &= 0xffff;
        gf_sel(t, m, 1 - b);
    }
    for (int i = 0; i < 16; ++i) {
        o[2 * i] = (uint8_t)(t[i] & 0xff);
        o[2 * i + 1] = (uint8_t)(t[i] >> 8);
    }
}

static int gf_parity(const gf a) {
    uint8_t d[32];
    gf_pack(d, a);
    return d[0] & 1;
}

static void ed_add(gf p[4], gf q[4]) {
    gf a, b, c, d, e, f, g, h, t;

    gf_sub(a, p[1], p[0]);
    gf_sub(t, q[1], q[0]);
    gf_mul(a, a, t);
    gf_add(b, p[0], p[1]);
    gf_add(t, q[0], q[1]);
    gf_mul(b, b, t);
    gf_mul(c, p[3], q[3]);
    gf_mul(c, c, kD2);
    gf_mul(d, p[2], q[2]);
    gf_add(d, d, d);
    gf_sub(e, b, a);
    gf_sub(f, d, c);
    gf_add(g, d, c);
    gf_add(h, b, a);

    gf_mul(p[0], e, f);
    gf_mul(p[1], h, g);
    gf_mul(p[2], g, f);
    gf_mul(p[3], e, h);
}

static void ed_cswap(gf p[4], gf q[4], uint8_t b) {
    for (int i = 0; i < 4; ++i) {
        gf_sel(p[i], q[i], (int)b);
    }
}

static void ed_pack(uint8_t r[32], gf p[4]) {
    gf tx, ty, zi;
    gf_inv(zi, p[2]);
    gf_mul(tx, p[0], zi);
    gf_mul(ty, p[1], zi);
    gf_pack(r, ty);
    r[31] ^= (uint8_t)(gf_parity(tx) << 7);
}

static void ed_scalarmult(gf p[4], gf q[4], const uint8_t* s) {
    gf_set(p[0], kGf0);
    gf_set(p[1], kGf1);
    gf_set(p[2], kGf1);
    gf_set(p[3], kGf0);
    for (int i = 255; i >= 0; --i) {
        uint8_t b = (uint8_t)((s[i / 8] >> (i & 7)) & 1);
        ed_cswap(p, q, b);
        ed_add(q, p);
        ed_add(p, p);
        ed_cswap(p, q, b);
    }
}

static void ed_scalarbase(gf p[4], const uint8_t* s) {
    gf q[4];
    gf_set(q[0], kBaseX);
    gf_set(q[1], kBaseY);
    gf_set(q[2], kGf1);
    gf_mul(q[3], kBaseX, kBaseY);
    ed_scalarmult(p, q, s);
}

static void ed_mod_l(uint8_t* r, int64_t x[64]) {
    for (int i = 63; i >= 32; --i) {
        int64_t carry = 0;
        int k;
        for (int j = i - 32; j < i - 12; ++j) {
            x[j] += carry - 16 * x[i] * (int64_t)kOrderL[j - (i - 32)];
            carry = (x[j] + 128) >> 8;
            x[j] -= carry * 256LL;
        }
        k = i - 12;
        x[k] += carry;
        x[i] = 0;
    }
    int64_t carry = 0;
    for (int j = 0; j < 32; ++j) {
        x[j] += carry - (x[31] >> 4) * (int64_t)kOrderL[j];
        carry = x[j] >> 8;
        x[j] &= 255;
    }
    for (int j = 0; j < 32; ++j) {
        x[j] -= carry * (int64_t)kOrderL[j];
    }
    for (int i = 0; i < 32; ++i) {
        x[i + 1] += x[i] >> 8;
        r[i] = (uint8_t)(x[i] & 255);
    }
}

static void ed_reduce(uint8_t r[64]) {
    int64_t x[64];
    for (int i = 0; i < 64; ++i) {
        x[i] = (int64_t)r[i];
    }
    memset(r, 0, 64);
    ed_mod_l(r, x);
}

static void ed_clamp(uint8_t az[32]) {
    az[0] &= 248u;
    az[31] &= 127u;
    az[31] |= 64u;
}

void ez_ed25519_public_key(const uint8_t seed[32], uint8_t out_pub[32]) {
    uint8_t az[64];
    gf p[4];
    ez_sha512(seed, 32, az);
    ed_clamp(az);
    ed_scalarbase(p, az);
    ed_pack(out_pub, p);
}

void ez_ed25519_sign(const uint8_t seed[32],
                     const uint8_t pub[32],
                     const uint8_t* msg,
                     size_t msg_len,
                     uint8_t out_sig[64]) {
    uint8_t az[64];
    uint8_t r[64];
    uint8_t hram[64];
    gf p[4];
    int64_t x[64];
    sha512_ctx_t ctx;

    ez_sha512(seed, 32, az);
    ed_clamp(az);

    /* r = H(az[32..64] || msg) mod L */
    sha512_init(&ctx);
    sha512_update(&ctx, az + 32, 32);
    if (msg != NULL && msg_len > 0) {
        sha512_update(&ctx, msg, msg_len);
    }
    sha512_final(&ctx, r);
    ed_reduce(r);

    /* R = [r]B */
    ed_scalarbase(p, r);
    ed_pack(out_sig, p);

    /* k = H(R || A || msg) mod L */
    sha512_init(&ctx);
    sha512_update(&ctx, out_sig, 32);
    sha512_update(&ctx, pub, 32);
    if (msg != NULL && msg_len > 0) {
        sha512_update(&ctx, msg, msg_len);
    }
    sha512_final(&ctx, hram);
    ed_reduce(hram);

    /* S = (r + k * a) mod L */
    for (int i = 0; i < 32; ++i) {
        x[i] = (int64_t)r[i];
    }
    for (int i = 32; i < 64; ++i) {
        x[i] = 0;
    }
    for (int i = 0; i < 32; ++i) {
        for (int j = 0; j < 32; ++j) {
            x[i + j] += (int64_t)hram[i] * (int64_t)az[j];
        }
    }
    ed_mod_l(out_sig + 32, x);
}

/* =========================== Base64URL & Fingerprint =========================== */

size_t ez_base64url_encode(const uint8_t* in, size_t in_len, char* out, size_t out_cap) {
    static const char kTable[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    if (out == NULL || out_cap == 0) {
        return 0;
    }
    size_t out_len = ((in_len / 3) * 4) + ((in_len % 3 == 0) ? 0 : (in_len % 3 + 1));
    if (out_len + 1 > out_cap) {
        return 0;
    }
    size_t j = 0;
    for (size_t i = 0; i < in_len; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        size_t rem = in_len - i;
        if (rem > 1) {
            v |= (uint32_t)in[i + 1] << 8;
        }
        if (rem > 2) {
            v |= (uint32_t)in[i + 2];
        }
        out[j++] = kTable[(v >> 18) & 0x3fu];
        out[j++] = kTable[(v >> 12) & 0x3fu];
        if (rem > 1) {
            out[j++] = kTable[(v >> 6) & 0x3fu];
        }
        if (rem > 2) {
            out[j++] = kTable[v & 0x3fu];
        }
    }
    out[j] = '\0';
    return j;
}

void ez_public_key_fingerprint(const uint8_t pub[32], char out[10]) {
    static const char kHex[] = "0123456789ABCDEF";
    uint8_t digest[32];
    ez_sha256(pub, 32, digest);
    out[0] = kHex[(digest[0] >> 4) & 0x0fu];
    out[1] = kHex[digest[0] & 0x0fu];
    out[2] = kHex[(digest[1] >> 4) & 0x0fu];
    out[3] = kHex[digest[1] & 0x0fu];
    out[4] = '-';
    out[5] = kHex[(digest[2] >> 4) & 0x0fu];
    out[6] = kHex[digest[2] & 0x0fu];
    out[7] = kHex[(digest[3] >> 4) & 0x0fu];
    out[8] = kHex[digest[3] & 0x0fu];
    out[9] = '\0';
}
