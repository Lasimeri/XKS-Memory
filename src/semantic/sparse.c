/* sparse.c: CSR construction, transpose and sparse x dense multiply. */
#include "sparse.h"

#include <stdlib.h>
#include <string.h>

static int by_rc(const void *a, const void *b)
{
    const xksm_triplet *x = a, *y = b;
    if (x->r != y->r)
        return x->r < y->r ? -1 : 1;
    return x->c < y->c ? -1 : x->c > y->c;
}

void xksm__csr_free(xksm_csr *a)
{
    if (!a)
        return;
    free(a->rowptr);
    free(a->col);
    free(a->val);
    memset(a, 0, sizeof *a);
}

/* stable counting sort of triplets by (r, c): by column, then by row; O(n + rows + cols).
 * Returns 0 when the scratch buffers cannot be allocated (the caller falls back to qsort). */
static int sort_rc(xksm_triplet *t, uint64_t n, uint32_t rows, uint32_t cols)
{
    size_t nb = (size_t)(rows > cols ? rows : cols) + 1;
    xksm_triplet *tmp = malloc(sizeof *tmp * (size_t)(n ? n : 1));
    uint64_t *cnt = malloc(sizeof *cnt * nb);
    if (!tmp || !cnt) {
        free(tmp);
        free(cnt);
        return 0;
    }
    memset(cnt, 0, sizeof *cnt * nb);
    for (uint64_t i = 0; i < n; ++i) /* pass 1: by column, t -> tmp */
        cnt[t[i].c + 1]++;
    for (size_t b = 1; b < nb; ++b)
        cnt[b] += cnt[b - 1];
    for (uint64_t i = 0; i < n; ++i)
        tmp[cnt[t[i].c]++] = t[i];
    memset(cnt, 0, sizeof *cnt * nb);
    for (uint64_t i = 0; i < n; ++i) /* pass 2: by row, tmp -> t (stable: columns stay sorted) */
        cnt[tmp[i].r + 1]++;
    for (size_t b = 1; b < nb; ++b)
        cnt[b] += cnt[b - 1];
    for (uint64_t i = 0; i < n; ++i)
        t[cnt[tmp[i].r]++] = tmp[i];
    free(tmp);
    free(cnt);
    return 1;
}

int xksm__csr_from_triplets(xksm_csr *a, uint32_t rows, uint32_t cols, xksm_triplet *t,
                            uint64_t n)
{
    memset(a, 0, sizeof *a);
    if (!sort_rc(t, n, rows, cols))
        qsort(t, (size_t)n, sizeof *t, by_rc);
    uint64_t u = 0; /* merge duplicates in place */
    for (uint64_t i = 0; i < n; ++i) {
        if (u && t[u - 1].r == t[i].r && t[u - 1].c == t[i].c)
            t[u - 1].v += t[i].v;
        else
            t[u++] = t[i];
    }
    a->rows = rows;
    a->cols = cols;
    a->nnz = u;
    a->rowptr = calloc((size_t)rows + 1, sizeof *a->rowptr);
    a->col = malloc(sizeof *a->col * (size_t)(u ? u : 1));
    a->val = malloc(sizeof *a->val * (size_t)(u ? u : 1));
    if (!a->rowptr || !a->col || !a->val) {
        xksm__csr_free(a);
        return 0;
    }
    for (uint64_t i = 0; i < u; ++i) {
        a->rowptr[t[i].r + 1]++;
        a->col[i] = t[i].c;
        a->val[i] = t[i].v;
    }
    for (uint32_t r = 0; r < rows; ++r)
        a->rowptr[r + 1] += a->rowptr[r];
    return 1;
}

int xksm__csr_transpose(const xksm_csr *a, xksm_csr *at)
{
    memset(at, 0, sizeof *at);
    at->rows = a->cols;
    at->cols = a->rows;
    at->nnz = a->nnz;
    at->rowptr = calloc((size_t)at->rows + 1, sizeof *at->rowptr);
    at->col = malloc(sizeof *at->col * (size_t)(a->nnz ? a->nnz : 1));
    at->val = malloc(sizeof *at->val * (size_t)(a->nnz ? a->nnz : 1));
    uint64_t *fill = calloc((size_t)at->rows + 1, sizeof *fill);
    if (!at->rowptr || !at->col || !at->val || !fill) {
        free(fill);
        xksm__csr_free(at);
        return 0;
    }
    for (uint64_t i = 0; i < a->nnz; ++i)
        at->rowptr[a->col[i] + 1]++;
    for (uint32_t r = 0; r < at->rows; ++r)
        at->rowptr[r + 1] += at->rowptr[r];
    for (uint32_t r = 0; r < a->rows; ++r) /* rows visited in order: columns stay sorted */
        for (uint64_t i = a->rowptr[r]; i < a->rowptr[r + 1]; ++i) {
            uint32_t c = a->col[i];
            uint64_t dst = at->rowptr[c] + fill[c]++;
            at->col[dst] = r;
            at->val[dst] = a->val[i];
        }
    free(fill);
    return 1;
}

typedef struct spmm_job {
    const xksm_csr *a;
    const float *x;
    float *y;
    size_t l;
} spmm_job;

static void spmm_slice(void *p, unsigned w, unsigned n)
{
    spmm_job *j = p;
    uint32_t lo = (uint32_t)((uint64_t)j->a->rows * w / n);
    uint32_t hi = (uint32_t)((uint64_t)j->a->rows * (w + 1) / n);
    size_t l = j->l;
    for (uint32_t r = lo; r < hi; ++r) {
        float *yr = j->y + (size_t)r * l;
        memset(yr, 0, sizeof(float) * l);
        for (uint64_t i = j->a->rowptr[r]; i < j->a->rowptr[r + 1]; ++i) {
            float v = j->a->val[i];
            const float *xr = j->x + (size_t)j->a->col[i] * l;
            for (size_t c = 0; c < l; ++c) /* contiguous: the compiler vectorizes this */
                yr[c] += v * xr[c];
        }
    }
}

void xksm__spmm(xksm_pool *pool, const xksm_csr *a, const float *x, float *y, size_t l)
{
    spmm_job j = {a, x, y, l};
    if (pool)
        xksm__pool_run(pool, spmm_slice, &j);
    else
        spmm_slice(&j, 0, 1);
}
