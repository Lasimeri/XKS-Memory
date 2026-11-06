/* backend.c: the CPU reference vector backend. */
#include "backend.h"

static size_t cpu_topk_dot(void *ctx, const float *mat, size_t rows, size_t dim, const float *q,
                           size_t k, xksm_hit *out)
{
    return xksm__f32_topk((xksm_pool *)ctx, mat, rows, dim, q, k, out);
}

static size_t cpu_topk_i8(void *ctx, const int8_t *code, const float *scale, size_t rows,
                          size_t dim, const int8_t *q, size_t k, xksm_hit *out)
{
    return xksm__lattice_topk((xksm_pool *)ctx, code, scale, rows, dim, q, k, out);
}

xksm_vbackend xksm__vbackend_cpu(xksm_pool *pool)
{
    xksm_vbackend b = {"cpu", cpu_topk_dot, cpu_topk_i8, pool};
    return b;
}
