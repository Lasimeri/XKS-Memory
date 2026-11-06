/* backend.h: private vector-backend interface. The CPU implementation is the reference;
 * accelerators (Knights Corner MVEX, OpenCL GPU) implement the same calls and are verified
 * against it before use. */
#ifndef XKSM_VECTOR_BACKEND_H
#define XKSM_VECTOR_BACKEND_H

#include "search.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct xksm_vbackend {
    const char *name;
    /* best k rows of the row-major f32 matrix (rows x dim) by dot product with q, best
     * first; returns the count written to out. ctx is backend state (the CPU: a pool). */
    size_t (*topk_dot)(void *ctx, const float *mat, size_t rows, size_t dim, const float *q,
                       size_t k, xksm_hit *out);
    /* same over square-lattice int8 codes (row r = code[r * dim ..], scale[r]) and an int8
     * query code: approximate scores, for candidate selection before an exact rerank */
    size_t (*topk_i8)(void *ctx, const int8_t *code, const float *scale, size_t rows, size_t dim,
                      const int8_t *q, size_t k, xksm_hit *out);
    void *ctx;
} xksm_vbackend;

/* The CPU reference backend over a worker pool (pool may be NULL: single thread). */
xksm_vbackend xksm__vbackend_cpu(xksm_pool *pool);

#ifdef __cplusplus
}
#endif
#endif
