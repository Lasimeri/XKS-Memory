/* sha512.h: private SHA-512 (FIPS 180-4) for content checksums and source dedup. */
#ifndef XKSM_SHA512_H
#define XKSM_SHA512_H

#include <stddef.h>
#include <stdint.h>

#define XKSM_SHA512_BYTES 64

#ifdef __cplusplus
extern "C" {
#endif

typedef struct xksm_sha512 {
    uint64_t h[8];
    uint64_t len;   /* total bytes fed; inputs of 2^61 bytes or more are not supported */
    size_t fill;    /* bytes waiting in buf */
    uint8_t buf[128];
} xksm_sha512;

void xksm__sha512_init(xksm_sha512 *c);
void xksm__sha512_update(xksm_sha512 *c, const void *data, size_t n);
void xksm__sha512_final(xksm_sha512 *c, uint8_t out[XKSM_SHA512_BYTES]);
void xksm__sha512(const void *data, size_t n, uint8_t out[XKSM_SHA512_BYTES]);

/* Block kernel (exposed for tests and benchmarks): nblocks x 128 bytes. */
void xksm__sha512_blocks(uint64_t st[8], const uint8_t *p, size_t nblocks);

#ifdef __cplusplus
}
#endif

#endif
