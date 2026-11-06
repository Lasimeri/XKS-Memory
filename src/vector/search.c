/* search.c: parallel top-k. Worker w scans the contiguous slice [N*w/n, N*(w+1)/n) into
 * its own min-heap; the caller merges the n heaps. Results are independent of n except for
 * the order of exact score ties. */
#include "search.h"

#include "kernels.h"

#include <stdlib.h>
#include <string.h>

/* min-heap of the best K: h[0] is the weakest kept */
static void push(xksm_hit *h, size_t *n, size_t K, float s, uint32_t i)
{
    if (*n < K) {
        size_t c = (*n)++;
        h[c] = (xksm_hit){s, i};
        while (c && h[(c - 1) / 2].score > h[c].score) {
            xksm_hit t = h[c];
            h[c] = h[(c - 1) / 2];
            h[(c - 1) / 2] = t;
            c = (c - 1) / 2;
        }
        return;
    }
    if (s <= h[0].score)
        return;
    h[0] = (xksm_hit){s, i};
    size_t c = 0;
    for (;;) {
        size_t l = 2 * c + 1, r = l + 1, m = c;
        if (l < K && h[l].score < h[m].score)
            m = l;
        if (r < K && h[r].score < h[m].score)
            m = r;
        if (m == c)
            break;
        xksm_hit t = h[c];
        h[c] = h[m];
        h[m] = t;
        c = m;
    }
}

static int by_score_desc(const void *a, const void *b)
{
    const xksm_hit *x = a, *y = b;
    if (x->score != y->score)
        return x->score < y->score ? 1 : -1;
    return x->index > y->index ? 1 : x->index < y->index ? -1 : 0; /* stable for ties */
}

typedef struct job {
    const float *vec;
    const int8_t *code;
    const float *scale;
    const float *q;
    const int8_t *qcode;
    size_t N, D, K;
    xksm_hit *heaps; /* n x K */
    size_t *counts;  /* n */
} job;

static void scan_f32(void *p, unsigned w, unsigned n)
{
    job *j = p;
    size_t lo = j->N * w / n, hi = j->N * (w + 1) / n, c = 0;
    xksm_hit *h = j->heaps + (size_t)w * j->K;
    for (size_t i = lo; i < hi; ++i)
        push(h, &c, j->K, xksm__dot_f32(j->q, j->vec + i * j->D, j->D), (uint32_t)i);
    j->counts[w] = c;
}

static void scan_lattice(void *p, unsigned w, unsigned n)
{
    job *j = p;
    size_t lo = j->N * w / n, hi = j->N * (w + 1) / n, c = 0;
    xksm_hit *h = j->heaps + (size_t)w * j->K;
    for (size_t i = lo; i < hi; ++i)
        push(h, &c, j->K, j->scale[i] * (float)xksm__dot_i8(j->qcode, j->code + i * j->D, j->D),
             (uint32_t)i);
    j->counts[w] = c;
}

static size_t run(xksm_pool *pool, job *j, xksm_task task, xksm_hit *out)
{
    unsigned n = pool ? xksm__pool_size(pool) : 1;
    if (j->K == 0 || j->N == 0)
        return 0;
    j->heaps = malloc(sizeof(xksm_hit) * j->K * n);
    j->counts = calloc(n, sizeof(size_t));
    if (!j->heaps || !j->counts) {
        free(j->heaps);
        free(j->counts);
        return 0;
    }
    if (pool)
        xksm__pool_run(pool, task, j);
    else
        task(j, 0, 1);
    size_t m = 0;
    for (unsigned w = 0; w < n; ++w)
        for (size_t c = 0; c < j->counts[w]; ++c) {
            const xksm_hit *x = &j->heaps[(size_t)w * j->K + c];
            push(out, &m, j->K, x->score, x->index);
        }
    qsort(out, m, sizeof *out, by_score_desc);
    free(j->heaps);
    free(j->counts);
    return m;
}

size_t xksm__f32_topk(xksm_pool *pool, const float *vec, size_t N, size_t D, const float *q,
                      size_t k, xksm_hit *out)
{
    job j = {.vec = vec, .q = q, .N = N, .D = D, .K = k};
    return run(pool, &j, scan_f32, out);
}

size_t xksm__lattice_topk(xksm_pool *pool, const int8_t *code, const float *scale, size_t N,
                          size_t D, const int8_t *qcode, size_t k, xksm_hit *out)
{
    job j = {.code = code, .scale = scale, .qcode = qcode, .N = N, .D = D, .K = k};
    return run(pool, &j, scan_lattice, out);
}

size_t xksm__f32_rerank(const float *vec, size_t D, const float *q, const xksm_hit *cand,
                        size_t nc, size_t k, xksm_hit *out)
{
    size_t m = 0;
    for (size_t c = 0; c < nc; ++c)
        push(out, &m, k, xksm__dot_f32(q, vec + (size_t)cand[c].index * D, D), cand[c].index);
    qsort(out, m, sizeof *out, by_score_desc);
    return m;
}
