/* test_search.c: parallel top-k equals a brute-force full sort, identical for every pool
 * size 1..128; thread setting clamps to 1..128 with default 8. argv[1] = scratch store. */
#include "test.h"

#include "../src/core/pool.h"
#include "../src/vector/kernels.h"
#include "../src/vector/search.h"

#include <stdlib.h>
#include <string.h>

static uint64_t rng = 0x9e3779b97f4a7c15u;
static float uni(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return (float)((double)(rng >> 11) * (1.0 / 9007199254740992.0)) - 0.5f;
}

static int cmp_desc(const void *a, const void *b)
{
    const xksm_hit *x = a, *y = b;
    if (x->score != y->score)
        return x->score < y->score ? 1 : -1;
    return x->index > y->index ? 1 : x->index < y->index ? -1 : 0;
}

static int same(const xksm_hit *a, const xksm_hit *b, size_t n)
{
    for (size_t i = 0; i < n; ++i)
        if (a[i].index != b[i].index || a[i].score != b[i].score)
            return 0;
    return 1;
}

int main(int argc, char **argv)
{
    if (argc != 2)
        return 2;
    enum { N = 5003, D = 96, K = 10 };
    float *v = malloc(sizeof(float) * N * D), *sc = malloc(sizeof(float) * N), q[D];
    int8_t *code = malloc((size_t)N * D), qc[D];
    xksm_hit *all = malloc(sizeof(xksm_hit) * N), ref[K], refl[K], got[K + 8];
    for (size_t i = 0; i < (size_t)N * D; ++i)
        v[i] = uni();
    for (size_t i = 0; i < N; ++i)
        sc[i] = xksm__lattice_encode(v + i * D, D, code + i * D);
    for (size_t i = 0; i < D; ++i)
        q[i] = uni();
    (void)xksm__lattice_encode(q, D, qc);

    /* brute force: every score, full sort */
    for (size_t i = 0; i < N; ++i)
        all[i] = (xksm_hit){xksm__dot_f32(q, v + i * D, D), (uint32_t)i};
    qsort(all, N, sizeof *all, cmp_desc);
    CHECK(xksm__f32_topk(NULL, v, N, D, q, K, ref) == K);
    CHECK(same(ref, all, K));
    for (size_t i = 0; i < N; ++i)
        all[i] = (xksm_hit){sc[i] * (float)xksm__dot_i8(qc, code + i * D, D), (uint32_t)i};
    qsort(all, N, sizeof *all, cmp_desc);
    CHECK(xksm__lattice_topk(NULL, code, sc, N, D, qc, K, refl) == K);
    CHECK(same(refl, all, K));

    /* every pool size gives the same answer */
    static const unsigned sizes[] = {1, 2, 3, 7, 8, 16, 64, 128};
    int mismatch = 0;
    for (size_t s = 0; s < sizeof sizes / sizeof *sizes; ++s) {
        xksm_pool *p = xksm__pool_create(sizes[s]);
        CHECK(p != NULL && xksm__pool_size(p) == sizes[s]);
        for (int rep = 0; rep < 3; ++rep) { /* reuse the pool: repeated dispatch */
            mismatch += xksm__f32_topk(p, v, N, D, q, K, got) != K || !same(got, ref, K);
            mismatch += xksm__lattice_topk(p, code, sc, N, D, qc, K, got) != K ||
                        !same(got, refl, K);
        }
        /* fewer rows than workers: most slices empty */
        size_t m = xksm__f32_topk(p, v, 5, D, q, K, got);
        xksm_hit small[5];
        size_t ms = xksm__f32_topk(NULL, v, 5, D, q, K, small);
        mismatch += m != 5 || ms != 5 || !same(got, small, 5);
        xksm__pool_destroy(p);
    }
    printf("pool sizes 1..128: %d mismatches\n", mismatch);
    CHECK(mismatch == 0);

    /* rerank: exact rescoring of lattice candidates */
    xksm_hit cand[100], rr[K];
    size_t nc = xksm__lattice_topk(NULL, code, sc, N, D, qc, 100, cand);
    CHECK(nc == 100 && xksm__f32_rerank(v, D, q, cand, nc, K, rr) == K);
    CHECK(rr[0].score >= rr[K - 1].score);

    /* handle setting */
    xksm *x = NULL;
    remove(argv[1]);
    CHECK(xksm_open(argv[1], &x) == XKSM_OK);
    CHECK(xksm_get_threads(x) == 8);
    CHECK(xksm_set_threads(x, 0) == 1 && xksm_set_threads(x, 1000) == 128);
    CHECK(xksm_set_threads(x, 16) == 16 && xksm_get_threads(x) == 16);
    xksm_close(x);
    remove(argv[1]);

    free(v);
    free(sc);
    free(code);
    free(all);
    return test_report("search");
}
