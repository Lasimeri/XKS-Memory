/* sha256.c: SHA-256 (FIPS 180-4) with two block kernels:
 *   - portable scalar C, always compiled;
 *   - x86 SHA-NI (sha256rnds2 / sha256msg1 / sha256msg2), selected at run time by CPUID.
 * Both are checked against FIPS vectors and each other by tests/test_sha256.c. */
#include "sha256.h"

#include <string.h>

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static inline uint32_t rotr(uint32_t x, unsigned n) { return (x >> n) | (x << (32u - n)); }

static inline uint32_t load_be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | (uint32_t)p[3];
}

void xksm__sha256_blocks_scalar(uint32_t st[8], const uint8_t *p, size_t nblocks)
{
    for (; nblocks--; p += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = load_be32(p + 4 * i);
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = st[0], b = st[1], c = st[2], d = st[3];
        uint32_t e = st[4], f = st[5], g = st[6], h = st[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
            uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            h = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }
        st[0] += a; st[1] += b; st[2] += c; st[3] += d;
        st[4] += e; st[5] += f; st[6] += g; st[7] += h;
    }
}

#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
#include <immintrin.h>
#define XKSM_HAVE_SHANI 1

/* Sixteen 4-round groups. Message words live in a 4-register ring x[i & 3];
 * group i >= 4 expands x[i-4..i-1] with msg1 (sigma0 + W[t-16]), alignr (W[t-7])
 * and msg2 (sigma1). rnds2 takes state as the ABEF / CDGH register pair. */
__attribute__((target("sha,sse4.1,ssse3")))
void xksm__sha256_blocks_shani(uint32_t st[8], const uint8_t *p, size_t nblocks)
{
    const __m128i bswap = _mm_set_epi64x(0x0c0d0e0f08090a0bLL, 0x0405060700010203LL);
    __m128i t = _mm_shuffle_epi32(_mm_loadu_si128((const __m128i *)&st[0]), 0xB1); /* CDAB */
    __m128i s1 = _mm_shuffle_epi32(_mm_loadu_si128((const __m128i *)&st[4]), 0x1B); /* EFGH */
    __m128i s0 = _mm_alignr_epi8(t, s1, 8);                                         /* ABEF */
    s1 = _mm_blend_epi16(s1, t, 0xF0);                                               /* CDGH */

    for (; nblocks--; p += 64) {
        const __m128i abef = s0, cdgh = s1;
        __m128i x[4];
#pragma GCC unroll 16
        for (int i = 0; i < 16; ++i) {
            __m128i w;
            if (i < 4) {
                w = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i *)(p + 16 * i)), bswap);
            } else {
                w = _mm_sha256msg1_epu32(x[i & 3], x[(i + 1) & 3]);
                w = _mm_add_epi32(w, _mm_alignr_epi8(x[(i + 3) & 3], x[(i + 2) & 3], 4));
                w = _mm_sha256msg2_epu32(w, x[(i + 3) & 3]);
            }
            x[i & 3] = w;
            __m128i m = _mm_add_epi32(w, _mm_loadu_si128((const __m128i *)&K[4 * i]));
            s1 = _mm_sha256rnds2_epu32(s1, s0, m);
            s0 = _mm_sha256rnds2_epu32(s0, s1, _mm_shuffle_epi32(m, 0x0E));
        }
        s0 = _mm_add_epi32(s0, abef);
        s1 = _mm_add_epi32(s1, cdgh);
    }

    t = _mm_shuffle_epi32(s0, 0x1B);  /* FEBA */
    s1 = _mm_shuffle_epi32(s1, 0xB1); /* DCHG */
    _mm_storeu_si128((__m128i *)&st[0], _mm_blend_epi16(t, s1, 0xF0)); /* DCBA */
    _mm_storeu_si128((__m128i *)&st[4], _mm_alignr_epi8(s1, t, 8));    /* HGFE */
}
#endif

typedef void (*blocks_fn)(uint32_t[8], const uint8_t *, size_t);

static blocks_fn pick(void)
{
#ifdef XKSM_HAVE_SHANI
    __builtin_cpu_init();
    if (__builtin_cpu_supports("sha") && __builtin_cpu_supports("sse4.1"))
        return xksm__sha256_blocks_shani;
#endif
    return xksm__sha256_blocks_scalar;
}

static blocks_fn g_blocks;

const char *xksm__sha256_kernel(void)
{
    if (!g_blocks)
        g_blocks = pick();
    return g_blocks == xksm__sha256_blocks_scalar ? "scalar" : "sha-ni";
}

void xksm__sha256_init(xksm_sha256 *c)
{
    static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    if (!g_blocks)
        g_blocks = pick();
    memcpy(c->h, iv, sizeof iv);
    c->len = 0;
    c->fill = 0;
}

void xksm__sha256_update(xksm_sha256 *c, const void *data, size_t n)
{
    const uint8_t *p = data;
    c->len += n;
    if (c->fill) {
        size_t take = 64 - c->fill < n ? 64 - c->fill : n;
        memcpy(c->buf + c->fill, p, take);
        c->fill += take;
        p += take;
        n -= take;
        if (c->fill < 64)
            return;
        g_blocks(c->h, c->buf, 1);
        c->fill = 0;
    }
    if (n >= 64) {
        g_blocks(c->h, p, n / 64);
        p += n & ~(size_t)63;
        n &= 63;
    }
    memcpy(c->buf, p, n);
    c->fill = n;
}

void xksm__sha256_final(xksm_sha256 *c, uint8_t out[32])
{
    uint64_t bits = (uint64_t)c->len * 8u;
    uint8_t pad[72] = {0x80};
    size_t padlen = (c->fill < 56 ? 56 : 120) - c->fill;
    for (int i = 0; i < 8; ++i)
        pad[padlen + (size_t)i] = (uint8_t)(bits >> (56 - 8 * i));
    uint64_t keep = c->len;
    xksm__sha256_update(c, pad, padlen + 8);
    c->len = keep;
    for (int i = 0; i < 8; ++i) {
        out[4 * i + 0] = (uint8_t)(c->h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(c->h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(c->h[i] >> 8);
        out[4 * i + 3] = (uint8_t)c->h[i];
    }
}

void xksm__sha256(const void *data, size_t n, uint8_t out[32])
{
    xksm_sha256 c;
    xksm__sha256_init(&c);
    xksm__sha256_update(&c, data, n);
    xksm__sha256_final(&c, out);
}
