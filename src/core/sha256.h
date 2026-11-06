/* sha256.h: private SHA-256 used for content checksums and source dedup. */
#ifndef XKSM_SHA256_H
#define XKSM_SHA256_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct xksm_sha256 {
    uint32_t h[8];
    uint64_t len;  /* total bytes fed */
    size_t fill;   /* bytes waiting in buf */
    uint8_t buf[64];
} xksm_sha256;

void xksm__sha256_init(xksm_sha256 *c);
void xksm__sha256_update(xksm_sha256 *c, const void *data, size_t n);
void xksm__sha256_final(xksm_sha256 *c, uint8_t out[32]);
void xksm__sha256(const void *data, size_t n, uint8_t out[32]);

/* Name of the block kernel in use: "sha-ni" or "scalar". */
const char *xksm__sha256_kernel(void);

/* Kernels exposed for tests and benchmarks. */
void xksm__sha256_blocks_scalar(uint32_t st[8], const uint8_t *p, size_t nblocks);
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
void xksm__sha256_blocks_shani(uint32_t st[8], const uint8_t *p, size_t nblocks);
#ifdef __cplusplus
}
#endif

#endif

#ifdef __cplusplus
}
#endif

#endif
