/* test_ocl.c: OpenCL randomized SVD against the CPU path on the same matrix and sketch:
 * singular values and well-separated singular vectors must agree. Skips (passes) when no
 * OpenCL GPU is present. Built only with XKSM_HAVE_OPENCL. */
#include "test.h"

#include "../backends/opencl/ocl_rsvd.h"
#include "../src/core/pool.h"
#include "../src/semantic/rsvd.h"

#include <math.h>
#include <stdlib.h>

static uint64_t rng = 0x9e3779b97f4a7c15u;
static double uni(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return (double)(rng >> 11) * (1.0 / 9007199254740992.0);
}

int main(void)
{
    /* sparse matrix with a decaying spectrum: entries scaled by row and column weights */
    enum { R = 4000, C = 3000, NT = 400000, K = 32, P = 16 };
    xksm_triplet *t = malloc(sizeof *t * NT);
    for (int i = 0; i < NT; ++i) {
        uint32_t r = (uint32_t)(uni() * (double)R), c = (uint32_t)(uni() * (double)C);
        double wr = 1.0 / (1.0 + r / 40.0), wc = 1.0 / (1.0 + c / 30.0);
        t[i] = (xksm_triplet){r, c, (float)((uni() - 0.3) * wr * wc)};
    }
    xksm_csr a, at;
    CHECK(xksm__csr_from_triplets(&a, R, C, t, NT) && xksm__csr_transpose(&a, &at));
    float *uc = malloc(sizeof(float) * R * K), *vc = malloc(sizeof(float) * C * K), sc[K];
    float *ug = malloc(sizeof(float) * R * K), *vg = malloc(sizeof(float) * C * K), sg[K];
    xksm_pool *pool = xksm__pool_create(8);
    CHECK(xksm__rsvd(pool, &a, &at, K, P, 4, 99, uc, sc, vc));
    char dev[128] = {0};
    if (!xksm__ocl_rsvd(&a, &at, K, P, 4, 99, ug, sg, vg, dev, sizeof dev)) {
        printf("no usable OpenCL GPU: skipped\n");
        xksm__csr_free(&a);
        xksm__csr_free(&at);
        free(t); free(uc); free(vc); free(ug); free(vg);
        xksm__pool_destroy(pool);
        return test_report("ocl");
    }
    printf("device: %s\n", dev);
    double sigerr = 0;
    int checked = 0, vecbad = 0;
    for (int i = 0; i < K; ++i) {
        sigerr = fmax(sigerr, fabs(sg[i] - sc[i]) / sc[i]);
        /* vectors are only defined when sigma_i is separated from its neighbours */
        double gap = fmin(i ? sc[i - 1] - sc[i] : INFINITY, i + 1 < K ? sc[i] - sc[i + 1] : INFINITY);
        if (gap / sc[i] < 0.02)
            continue;
        double du = 0, dv = 0;
        for (int r = 0; r < R; ++r)
            du += (double)uc[r * K + i] * ug[r * K + i];
        for (int c = 0; c < C; ++c)
            dv += (double)vc[c * K + i] * vg[c * K + i];
        ++checked;
        vecbad += fabs(du) < 0.999 || fabs(dv) < 0.999;
    }
    printf("sigma max relative difference GPU vs CPU %.2e; %d well-separated vectors, %d differ\n",
           sigerr, checked, vecbad);
    CHECK(sigerr < 1e-3);
    CHECK(checked >= 8 && vecbad == 0);
    xksm__pool_destroy(pool);
    xksm__csr_free(&a);
    xksm__csr_free(&at);
    free(t); free(uc); free(vc); free(ug); free(vg);
    return test_report("ocl");
}
