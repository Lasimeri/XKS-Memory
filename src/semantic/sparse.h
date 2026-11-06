/* sparse.h: private CSR matrices for the semantic model. */
#ifndef XKSM_SPARSE_H
#define XKSM_SPARSE_H

#include "../core/pool.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct xksm_csr {
    uint32_t rows, cols;
    uint64_t nnz;
    uint64_t *rowptr; /* rows + 1 */
    uint32_t *col;    /* nnz, sorted within each row */
    float *val;       /* nnz */
} xksm_csr;

typedef struct xksm_triplet {
    uint32_t r, c;
    float v;
} xksm_triplet;

/* Build from n triplets (any order; duplicates are summed; t is sorted in place). */
int xksm__csr_from_triplets(xksm_csr *a, uint32_t rows, uint32_t cols, xksm_triplet *t,
                            uint64_t n);
int xksm__csr_transpose(const xksm_csr *a, xksm_csr *at);
void xksm__csr_free(xksm_csr *a);

/* Y (a->rows x l) = A * X (a->cols x l), row-major dense; rows split over the pool. */
void xksm__spmm(xksm_pool *pool, const xksm_csr *a, const float *x, float *y, size_t l);

#ifdef __cplusplus
}
#endif
#endif
