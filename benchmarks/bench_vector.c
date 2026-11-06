/* bench_vector.c: exact f32 scan vs square-lattice (Z^n, int8) scan, single thread.
 * Data: fixed-seed clustered placeholders (C centers; vector = normalize(center + s * g)).
 * usage: bench_vector [N=100000] [dim=768] [queries=100] [k=10] [rerank=100] [clusters=1000]
 * Modes: A f32 scalar, B f32 AVX2/FMA (ground truth), C lattice top-k,
 *        D lattice top-rerank then exact f32 rerank to top-k. */
#define _POSIX_C_SOURCE 200809L /* clock_gettime under strict -std=c17 */
#include "../src/vector/kernels.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

static uint64_t rng = 0x9e3779b97f4a7c15u;
static double uni(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return ((double)(rng >> 11) + 0.5) * (1.0 / 9007199254740992.0);
}
static float gauss(void) /* Box-Muller */
{
    return (float)(sqrt(-2.0 * log(uni())) * cos(6.283185307179586 * uni()));
}

typedef struct hit {
    float s;
    uint32_t i;
} hit;

/* min-heap of the best K: h[0] is the weakest kept */
static void heap_push(hit *h, size_t *n, size_t K, float s, uint32_t i)
{
    if (*n < K) {
        size_t c = (*n)++;
        h[c] = (hit){s, i};
        while (c && h[(c - 1) / 2].s > h[c].s) {
            hit t = h[c]; h[c] = h[(c - 1) / 2]; h[(c - 1) / 2] = t;
            c = (c - 1) / 2;
        }
        return;
    }
    if (s <= h[0].s)
        return;
    h[0] = (hit){s, i};
    size_t c = 0;
    for (;;) {
        size_t l = 2 * c + 1, r = l + 1, m = c;
        if (l < K && h[l].s < h[m].s) m = l;
        if (r < K && h[r].s < h[m].s) m = r;
        if (m == c) break;
        hit t = h[c]; h[c] = h[m]; h[m] = t;
        c = m;
    }
}

static int cmp_d(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static int has(const hit *h, size_t n, uint32_t i)
{
    for (size_t k = 0; k < n; ++k)
        if (h[k].i == i)
            return 1;
    return 0;
}

int main(int argc, char **argv)
{
    size_t N = argc > 1 ? strtoul(argv[1], NULL, 10) : 100000;
    size_t D = argc > 2 ? strtoul(argv[2], NULL, 10) : 768;
    size_t Q = argc > 3 ? strtoul(argv[3], NULL, 10) : 100;
    size_t K = argc > 4 ? strtoul(argv[4], NULL, 10) : 10;
    size_t R = argc > 5 ? strtoul(argv[5], NULL, 10) : 100;
    size_t C = argc > 6 ? strtoul(argv[6], NULL, 10) : 1000;
    if (!N || !D || !Q || !K || R < K || R > 1024 || !C || N > UINT32_MAX)
        return 2;

    float *v = malloc(N * D * sizeof(float)), *cen = malloc(C * D * sizeof(float));
    float *q = malloc(Q * D * sizeof(float)), *vs = malloc(N * sizeof(float));
    int8_t *code = malloc(N * D), *qc = malloc(D);
    hit *truth = malloc(Q * K * sizeof(hit)), *h = malloc(R * sizeof(hit));
    double *t = malloc(Q * sizeof(double));
    if (!v || !cen || !q || !vs || !code || !qc || !truth || !h || !t)
        return 1;
    for (size_t i = 0; i < C * D; ++i)
        cen[i] = gauss();
    for (size_t c = 0; c < C; ++c)
        xksm__normalize(cen + c * D, D);
    const float spread = 0.5f; /* noise vector norm about 0.5 against a unit center */
    for (size_t i = 0; i < N; ++i) {
        const float *c = cen + (size_t)(uni() * (double)C) * D;
        for (size_t j = 0; j < D; ++j)
            v[i * D + j] = c[j] + spread * gauss() / sqrtf((float)D);
        xksm__normalize(v + i * D, D);
        vs[i] = xksm__lattice_encode(v + i * D, D, code + i * D);
    }
    for (size_t i = 0; i < Q; ++i) {
        const float *c = cen + (size_t)(uni() * (double)C) * D;
        for (size_t j = 0; j < D; ++j)
            q[i * D + j] = c[j] + spread * gauss() / sqrtf((float)D);
        xksm__normalize(q + i * D, D);
    }

    printf("N %zu, dim %zu, %zu queries, k %zu, rerank %zu, %zu clusters, kernel %s\n", N, D, Q,
           K, R, C, xksm__vector_kernel());
    printf("f32 data %.1f MB, lattice codes %.1f MB (+ %.1f MB scales)\n",
           (double)(N * D * 4) / 1e6, (double)(N * D) / 1e6, (double)(N * 4) / 1e6);

    const char *name[4] = {"A f32 scalar", "B f32 avx2+fma", "C lattice i8", "D lattice+rerank"};
    static const int order[4] = {1, 0, 2, 3}; /* ground truth (B) first */
    for (int oi = 0; oi < 4; ++oi) {
        int mode = order[oi];
        size_t found = 0;
        for (size_t qi = 0; qi < Q; ++qi) {
            const float *qq = q + qi * D;
            size_t n = 0;
            double a = now_s();
            if (mode == 0) {
                for (size_t i = 0; i < N; ++i)
                    heap_push(h, &n, K, xksm__dot_f32_scalar(qq, v + i * D, D), (uint32_t)i);
            } else if (mode == 1) {
                for (size_t i = 0; i < N; ++i)
                    heap_push(h, &n, K, xksm__dot_f32(qq, v + i * D, D), (uint32_t)i);
            } else {
                (void)xksm__lattice_encode(qq, D, qc); /* query scale ranks nothing: dropped */
                size_t keep = mode == 2 ? K : R;
                for (size_t i = 0; i < N; ++i)
                    heap_push(h, &n, keep, vs[i] * (float)xksm__dot_i8(qc, code + i * D, D),
                              (uint32_t)i);
                if (mode == 3) { /* exact rerank of the R candidates */
                    hit cand[1024];
                    size_t nc = n < 1024 ? n : 1024;
                    memcpy(cand, h, nc * sizeof(hit));
                    n = 0;
                    for (size_t c = 0; c < nc; ++c)
                        heap_push(h, &n, K, xksm__dot_f32(qq, v + (size_t)cand[c].i * D, D),
                                  cand[c].i);
                }
            }
            t[qi] = now_s() - a;
            if (mode == 1)
                memcpy(truth + qi * K, h, K * sizeof(hit));
            else
                for (size_t k = 0; k < n && k < K; ++k)
                    found += (size_t)has(truth + qi * K, K, h[k].i);
        }
        qsort(t, Q, sizeof(double), cmp_d);
        double med = t[Q / 2];
        size_t bytes = mode < 2 ? N * D * 4 : N * D + N * 4;
        printf("%-17s median %8.3f ms/query  %7.1f q/s  %6.2f GB/s scanned", name[mode],
               med * 1e3, 1.0 / med, (double)bytes / med / 1e9);
        if (mode != 1)
            printf("  recall@%zu %.4f", K, (double)found / (double)(Q * K));
        printf("\n");
    }
    return 0;
}
