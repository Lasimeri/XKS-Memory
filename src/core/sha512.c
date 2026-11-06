/* sha512.c: SHA-512, portable C. x86-64 CPUs before Arrow Lake / Lunar Lake have no
 * SHA-512 instructions, so this scalar form is the CPU baseline everywhere. */
#include "sha512.h"

#include <string.h>

static const uint64_t K[80] = {
    0x428a2f98d728ae22, 0x7137449123ef65cd, 0xb5c0fbcfec4d3b2f, 0xe9b5dba58189dbbc,
    0x3956c25bf348b538, 0x59f111f1b605d019, 0x923f82a4af194f9b, 0xab1c5ed5da6d8118,
    0xd807aa98a3030242, 0x12835b0145706fbe, 0x243185be4ee4b28c, 0x550c7dc3d5ffb4e2,
    0x72be5d74f27b896f, 0x80deb1fe3b1696b1, 0x9bdc06a725c71235, 0xc19bf174cf692694,
    0xe49b69c19ef14ad2, 0xefbe4786384f25e3, 0x0fc19dc68b8cd5b5, 0x240ca1cc77ac9c65,
    0x2de92c6f592b0275, 0x4a7484aa6ea6e483, 0x5cb0a9dcbd41fbd4, 0x76f988da831153b5,
    0x983e5152ee66dfab, 0xa831c66d2db43210, 0xb00327c898fb213f, 0xbf597fc7beef0ee4,
    0xc6e00bf33da88fc2, 0xd5a79147930aa725, 0x06ca6351e003826f, 0x142929670a0e6e70,
    0x27b70a8546d22ffc, 0x2e1b21385c26c926, 0x4d2c6dfc5ac42aed, 0x53380d139d95b3df,
    0x650a73548baf63de, 0x766a0abb3c77b2a8, 0x81c2c92e47edaee6, 0x92722c851482353b,
    0xa2bfe8a14cf10364, 0xa81a664bbc423001, 0xc24b8b70d0f89791, 0xc76c51a30654be30,
    0xd192e819d6ef5218, 0xd69906245565a910, 0xf40e35855771202a, 0x106aa07032bbd1b8,
    0x19a4c116b8d2d0c8, 0x1e376c085141ab53, 0x2748774cdf8eeb99, 0x34b0bcb5e19b48a8,
    0x391c0cb3c5c95a63, 0x4ed8aa4ae3418acb, 0x5b9cca4f7763e373, 0x682e6ff3d6b2b8a3,
    0x748f82ee5defb2fc, 0x78a5636f43172f60, 0x84c87814a1f0ab72, 0x8cc702081a6439ec,
    0x90befffa23631e28, 0xa4506cebde82bde9, 0xbef9a3f7b2c67915, 0xc67178f2e372532b,
    0xca273eceea26619c, 0xd186b8c721c0c207, 0xeada7dd6cde0eb1e, 0xf57d4f7fee6ed178,
    0x06f067aa72176fba, 0x0a637dc5a2c898a6, 0x113f9804bef90dae, 0x1b710b35131c471b,
    0x28db77f523047d84, 0x32caab7b40c72493, 0x3c9ebe0a15c9bebc, 0x431d67c49c100d4c,
    0x4cc5d4becb3e42b6, 0x597f299cfc657e2a, 0x5fcb6fab3ad6faec, 0x6c44198c4a475817,
};

static inline uint64_t rotr(uint64_t x, unsigned n) { return (x >> n) | (x << (64u - n)); }

static inline uint64_t load_be64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v = v << 8 | p[i];
    return v;
}

void xksm__sha512_blocks(uint64_t st[8], const uint8_t *p, size_t nblocks)
{
    for (; nblocks--; p += 128) {
        uint64_t w[80];
        for (int i = 0; i < 16; ++i)
            w[i] = load_be64(p + 8 * i);
        for (int i = 16; i < 80; ++i) {
            uint64_t s0 = rotr(w[i - 15], 1) ^ rotr(w[i - 15], 8) ^ (w[i - 15] >> 7);
            uint64_t s1 = rotr(w[i - 2], 19) ^ rotr(w[i - 2], 61) ^ (w[i - 2] >> 6);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint64_t a = st[0], b = st[1], c = st[2], d = st[3];
        uint64_t e = st[4], f = st[5], g = st[6], h = st[7];
        for (int i = 0; i < 80; ++i) {
            uint64_t t1 = h + (rotr(e, 14) ^ rotr(e, 18) ^ rotr(e, 41)) + ((e & f) ^ (~e & g)) +
                          K[i] + w[i];
            uint64_t t2 = (rotr(a, 28) ^ rotr(a, 34) ^ rotr(a, 39)) + ((a & b) ^ (a & c) ^ (b & c));
            h = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }
        st[0] += a; st[1] += b; st[2] += c; st[3] += d;
        st[4] += e; st[5] += f; st[6] += g; st[7] += h;
    }
}

void xksm__sha512_init(xksm_sha512 *c)
{
    static const uint64_t iv[8] = {0x6a09e667f3bcc908, 0xbb67ae8584caa73b, 0x3c6ef372fe94f82b,
                                   0xa54ff53a5f1d36f1, 0x510e527fade682d1, 0x9b05688c2b3e6c1f,
                                   0x1f83d9abfb41bd6b, 0x5be0cd19137e2179};
    memcpy(c->h, iv, sizeof iv);
    c->len = 0;
    c->fill = 0;
}

void xksm__sha512_update(xksm_sha512 *c, const void *data, size_t n)
{
    const uint8_t *p = data;
    c->len += n;
    if (c->fill) {
        size_t take = 128 - c->fill < n ? 128 - c->fill : n;
        memcpy(c->buf + c->fill, p, take);
        c->fill += take;
        p += take;
        n -= take;
        if (c->fill < 128)
            return;
        xksm__sha512_blocks(c->h, c->buf, 1);
        c->fill = 0;
    }
    if (n >= 128) {
        xksm__sha512_blocks(c->h, p, n / 128);
        p += n & ~(size_t)127;
        n &= 127;
    }
    memcpy(c->buf, p, n);
    c->fill = n;
}

void xksm__sha512_final(xksm_sha512 *c, uint8_t out[XKSM_SHA512_BYTES])
{
    /* 0x80, zeros to 112 mod 128, then a 128-bit big-endian bit count (high 64 bits 0). */
    uint64_t bits = c->len * 8u;
    uint8_t pad[144] = {0x80};
    size_t padlen = (c->fill < 112 ? 112 : 240) - c->fill;
    for (int i = 0; i < 8; ++i)
        pad[padlen + 8 + (size_t)i] = (uint8_t)(bits >> (56 - 8 * i));
    xksm__sha512_update(c, pad, padlen + 16);
    for (int i = 0; i < 8; ++i)
        for (int j = 0; j < 8; ++j)
            out[8 * i + j] = (uint8_t)(c->h[i] >> (56 - 8 * j));
}

void xksm__sha512(const void *data, size_t n, uint8_t out[XKSM_SHA512_BYTES])
{
    xksm_sha512 c;
    xksm__sha512_init(&c);
    xksm__sha512_update(&c, data, n);
    xksm__sha512_final(&c, out);
}
