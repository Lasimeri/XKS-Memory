/* backend.c: the CPU reference vector backend. */
#include "backend.h"

static size_t cpu_topk_dot(void *ctx, const float *mat, size_t rows, size_t dim, const float *q,
                           size_t k, xksm_hit *out)
{
    return xksm__f32_topk((xksm_pool *)ctx, mat, rows, dim, q, k, out);
}

xksm_vbackend xksm__vbackend_cpu(xksm_pool *pool)
{
    xksm_vbackend b = {"cpu", cpu_topk_dot, pool};
    return b;
}
