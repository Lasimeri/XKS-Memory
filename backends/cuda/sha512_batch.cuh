/* sha512_batch.cuh: device SHA-512, one thread per independent message.
 * Batch layout contract: message i starts at data + off[i], off[i] % 8 == 0, and the
 * buffer has at least 8 readable bytes past the end of every message (alignment slack).
 * Bytes past len are masked, so slack content does not matter. */
#ifndef XKSM_SHA512_BATCH_CUH
#define XKSM_SHA512_BATCH_CUH

#include <stdint.h>

__constant__ uint64_t xksm_k512[80] = {
    0x428a2f98d728ae22ull, 0x7137449123ef65cdull, 0xb5c0fbcfec4d3b2full, 0xe9b5dba58189dbbcull,
    0x3956c25bf348b538ull, 0x59f111f1b605d019ull, 0x923f82a4af194f9bull, 0xab1c5ed5da6d8118ull,
    0xd807aa98a3030242ull, 0x12835b0145706fbeull, 0x243185be4ee4b28cull, 0x550c7dc3d5ffb4e2ull,
    0x72be5d74f27b896full, 0x80deb1fe3b1696b1ull, 0x9bdc06a725c71235ull, 0xc19bf174cf692694ull,
    0xe49b69c19ef14ad2ull, 0xefbe4786384f25e3ull, 0x0fc19dc68b8cd5b5ull, 0x240ca1cc77ac9c65ull,
    0x2de92c6f592b0275ull, 0x4a7484aa6ea6e483ull, 0x5cb0a9dcbd41fbd4ull, 0x76f988da831153b5ull,
    0x983e5152ee66dfabull, 0xa831c66d2db43210ull, 0xb00327c898fb213full, 0xbf597fc7beef0ee4ull,
    0xc6e00bf33da88fc2ull, 0xd5a79147930aa725ull, 0x06ca6351e003826full, 0x142929670a0e6e70ull,
    0x27b70a8546d22ffcull, 0x2e1b21385c26c926ull, 0x4d2c6dfc5ac42aedull, 0x53380d139d95b3dfull,
    0x650a73548baf63deull, 0x766a0abb3c77b2a8ull, 0x81c2c92e47edaee6ull, 0x92722c851482353bull,
    0xa2bfe8a14cf10364ull, 0xa81a664bbc423001ull, 0xc24b8b70d0f89791ull, 0xc76c51a30654be30ull,
    0xd192e819d6ef5218ull, 0xd69906245565a910ull, 0xf40e35855771202aull, 0x106aa07032bbd1b8ull,
    0x19a4c116b8d2d0c8ull, 0x1e376c085141ab53ull, 0x2748774cdf8eeb99ull, 0x34b0bcb5e19b48a8ull,
    0x391c0cb3c5c95a63ull, 0x4ed8aa4ae3418acbull, 0x5b9cca4f7763e373ull, 0x682e6ff3d6b2b8a3ull,
    0x748f82ee5defb2fcull, 0x78a5636f43172f60ull, 0x84c87814a1f0ab72ull, 0x8cc702081a6439ecull,
    0x90befffa23631e28ull, 0xa4506cebde82bde9ull, 0xbef9a3f7b2c67915ull, 0xc67178f2e372532bull,
    0xca273eceea26619cull, 0xd186b8c721c0c207ull, 0xeada7dd6cde0eb1eull, 0xf57d4f7fee6ed178ull,
    0x06f067aa72176fbaull, 0x0a637dc5a2c898a6ull, 0x113f9804bef90daeull, 0x1b710b35131c471bull,
    0x28db77f523047d84ull, 0x32caab7b40c72493ull, 0x3c9ebe0a15c9bebcull, 0x431d67c49c100d4cull,
    0x4cc5d4becb3e42b6ull, 0x597f299cfc657e2aull, 0x5fcb6fab3ad6faecull, 0x6c44198c4a475817ull,
};

__device__ __forceinline__ uint64_t xk_rotr(uint64_t x, unsigned n)
{
    return (x >> n) | (x << (64u - n));
}

/* Big-endian 64-bit load from an 8-byte-aligned little-endian word: two PRMT byte swaps. */
__device__ __forceinline__ uint64_t xk_be64(uint64_t v)
{
    uint32_t lo = (uint32_t)v, hi = (uint32_t)(v >> 32);
    return (uint64_t)__byte_perm(lo, 0, 0x0123) << 32 | __byte_perm(hi, 0, 0x0123);
}

__device__ __forceinline__ void xk_compress(uint64_t h[8], uint64_t w[16])
{
    uint64_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
#pragma unroll
    for (int i = 0; i < 80; ++i) {
        uint64_t wi;
        if (i < 16) {
            wi = w[i];
        } else {
            uint64_t w15 = w[(i - 15) & 15], w2 = w[(i - 2) & 15];
            uint64_t s0 = xk_rotr(w15, 1) ^ xk_rotr(w15, 8) ^ (w15 >> 7);
            uint64_t s1 = xk_rotr(w2, 19) ^ xk_rotr(w2, 61) ^ (w2 >> 6);
            wi = w[i & 15] = w[i & 15] + s0 + w[(i - 7) & 15] + s1;
        }
        uint64_t t1 = hh + (xk_rotr(e, 14) ^ xk_rotr(e, 18) ^ xk_rotr(e, 41)) +
                      ((e & f) ^ (~e & g)) + xksm_k512[i] + wi;
        uint64_t t2 = (xk_rotr(a, 28) ^ xk_rotr(a, 34) ^ xk_rotr(a, 39)) +
                      ((a & b) ^ (a & c) ^ (b & c));
        hh = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

__global__ void xksm_sha512_batch_kernel(const uint8_t *__restrict__ data,
                                         const uint64_t *__restrict__ off,
                                         const uint32_t *__restrict__ len, uint32_t n,
                                         uint64_t *__restrict__ out /* n x 8 words, BE */)
{
    uint32_t id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id >= n)
        return;
    const uint64_t *p = (const uint64_t *)(data + off[id]);
    const uint32_t L = len[id];
    uint64_t h[8] = {0x6a09e667f3bcc908ull, 0xbb67ae8584caa73bull, 0x3c6ef372fe94f82bull,
                     0xa54ff53a5f1d36f1ull, 0x510e527fade682d1ull, 0x9b05688c2b3e6c1full,
                     0x1f83d9abfb41bd6bull, 0x5be0cd19137e2179ull};
    uint64_t w[16];

    uint32_t full = L / 128;
    for (uint32_t b = 0; b < full; ++b, p += 16) {
#pragma unroll
        for (int i = 0; i < 16; ++i)
            w[i] = xk_be64(__ldg(p + i));
        xk_compress(h, w);
    }

    /* tail: r = L % 128 message bytes, 0x80, zeros, 128-bit bit length */
    uint32_t r = L & 127;
#pragma unroll
    for (int i = 0; i < 16; ++i) {
        int valid = (int)r - 8 * i; /* message bytes in word i */
        uint64_t v = 0;
        if (valid > 0) {
            v = xk_be64(__ldg(p + i));
            if (valid < 8)
                v &= ~0ull << (64 - 8 * valid);
        }
        if (valid >= 0 && valid < 8)
            v |= 0x80ull << (56 - 8 * valid);
        w[i] = v;
    }
    if (r >= 112) {
        xk_compress(h, w);
#pragma unroll
        for (int i = 0; i < 16; ++i)
            w[i] = 0;
    }
    w[15] = (uint64_t)L * 8u; /* w[14] = high 64 bits of the length = 0 */
    xk_compress(h, w);

#pragma unroll
    for (int i = 0; i < 8; ++i)
        out[(size_t)id * 8 + i] = h[i];
}

#endif
