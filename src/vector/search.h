/* search.h: private parallel top-k scans over flat vector arrays. */
#ifndef XKSM_VECTOR_SEARCH_H
#define XKSM_VECTOR_SEARCH_H

#include "../core/pool.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct xksm_hit {
    float score;
    uint32_t index;
} xksm_hit;

/* Exact: best k of N row-major f32 vectors (dim D) by dot product with q.
 * Writes min(k, N) hits to out, best first; returns the count. pool may be NULL. */
size_t xksm__f32_topk(xksm_pool *pool, const float *vec, size_t N, size_t D, const float *q,
                      size_t k, xksm_hit *out);

/* Square lattice: best k by scale[i] * <code_i, qcode> (int8 codes, dim D). */
size_t xksm__lattice_topk(xksm_pool *pool, const int8_t *code, const float *scale, size_t N,
                          size_t D, const int8_t *qcode, size_t k, xksm_hit *out);

/* Exact rescoring of nc candidates (indices into vec), best k first; returns the count. */
size_t xksm__f32_rerank(const float *vec, size_t D, const float *q, const xksm_hit *cand,
                        size_t nc, size_t k, xksm_hit *out);

#ifdef __cplusplus
}
#endif
#endif
