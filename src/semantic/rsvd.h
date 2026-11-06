/* rsvd.h: private randomized truncated SVD of a CSR matrix. */
#ifndef XKSM_RSVD_H
#define XKSM_RSVD_H

#include "sparse.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Rank-k approximation A ~ U diag(sigma) V^T with oversampling p and q power iterations.
 * at = A^T. u: rows x k, sigma: k (descending), v: cols x k (may be NULL). 1 on success. */
int xksm__rsvd(xksm_pool *pool, const xksm_csr *a, const xksm_csr *at, size_t k, size_t p,
               int q, uint64_t seed, float *u, float *sigma, float *v);

#ifdef __cplusplus
}
#endif
#endif
