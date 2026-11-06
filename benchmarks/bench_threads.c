/* bench_threads.c: thread scaling of exact f32 top-k and lattice top-R + exact rerank,
 * through the library's parallel search and worker pool. Same clustered placeholder data
 * as bench_vector. usage: bench_threads [N=100000] [dim=768] [queries=100] */
#define _POSIX_C_SOURCE 200809L /* clock_gettime under strict -std=c17 */
#include "../src/core/pool.h"
#include "../src/vector/kernels.h"
#include "../src/vector/search.h"

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
static float gauss(void)
{
    return (float)(sqrt(-2.0 * log(uni())) * cos(6.283185307179586 * uni()));
}

static int cmp_d(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

int main(int argc, char **argv)
{
    size_t N = argc > 1 ? strtoul(argv[1], NULL, 10) : 100000;
    size_t D = argc > 2 ? strtoul(argv[2], NULL, 10) : 768;
    size_t Q = argc > 3 ? strtoul(argv[3], NULL, 10) : 100;
    const size_t K = 10, R = 100, C = 1000;
    if (!N || !D || !Q)
        return 2;
    float *v = malloc(N * D * sizeof(float)), *cen = malloc(C * D * sizeof(float));
    float *q = malloc(Q * D * sizeof(float)), *vs = malloc(N * sizeof(float));
    int8_t *code = malloc(N * D), *qc = malloc(Q * D);
    xksm_hit *truth = malloc(Q * K * sizeof(xksm_hit));
    double *t = malloc(Q * sizeof(double));
    if (!v || !cen || !q || !vs || !code || !qc || !truth || !t)
        return 1;
    for (size_t i = 0; i < C * D; ++i)
        cen[i] = gauss();
    for (size_t c = 0; c < C; ++c)
        xksm__normalize(cen + c * D, D);
    for (size_t i = 0; i < N + Q; ++i) {
        float *dst = i < N ? v + i * D : q + (i - N) * D;
        const float *c = cen + (size_t)(uni() * (double)C) * D;
        for (size_t j = 0; j < D; ++j)
            dst[j] = c[j] + 0.5f * gauss() / sqrtf((float)D);
        xksm__normalize(dst, D);
        if (i < N)
            vs[i] = xksm__lattice_encode(dst, D, code + i * D);
        else
            (void)xksm__lattice_encode(dst, D, qc + (i - N) * D);
    }
    for (size_t qi = 0; qi < Q; ++qi) /* ground truth: exact, single thread */
        (void)xksm__f32_topk(NULL, v, N, D, q + qi * D, K, truth + qi * K);

    printf("N %zu, dim %zu, %zu queries, k %zu, rerank %zu, kernel %s\n", N, D, Q, K, R,
           xksm__vector_kernel());
    static const unsigned threads[] = {1, 2, 4, 8, 16, 32};
    for (size_t ti = 0; ti < sizeof threads / sizeof *threads; ++ti) {
        xksm_pool *p = xksm__pool_create(threads[ti]);
        if (!p)
            return 1;
        for (int mode = 0; mode < 2; ++mode) {
            size_t found = 0;
            for (size_t qi = 0; qi < Q; ++qi) {
                xksm_hit cand[100], out[10];
                size_t n;
                double a = now_s();
                if (mode == 0) {
                    n = xksm__f32_topk(p, v, N, D, q + qi * D, K, out);
                } else {
                    size_t nc = xksm__lattice_topk(p, code, vs, N, D, qc + qi * D, R, cand);
                    n = xksm__f32_rerank(v, D, q + qi * D, cand, nc, K, out);
                }
                t[qi] = now_s() - a;
                for (size_t k = 0; k < n; ++k)
                    for (size_t g = 0; g < K; ++g)
                        found += out[k].index == truth[qi * K + g].index;
            }
            qsort(t, Q, sizeof(double), cmp_d);
            double med = t[Q / 2];
            size_t bytes = mode == 0 ? N * D * 4 : N * D + N * 4;
            printf("threads %3u  %-16s median %7.3f ms/query  %7.1f q/s  %6.2f GB/s  recall@10 %.4f\n",
                   threads[ti], mode == 0 ? "exact f32" : "lattice+rerank", med * 1e3, 1.0 / med,
                   (double)bytes / med / 1e9, (double)found / (double)(Q * K));
        }
        xksm__pool_destroy(p);
    }
    return 0;
}
