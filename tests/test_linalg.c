/* test_linalg.c: Jacobi eigensolver, CholQR2, CSR build / transpose / SpMM against dense
 * references, randomized SVD singular values against an exact dense eigendecomposition. */
#include "test.h"

#include "../src/core/pool.h"
#include "../src/semantic/linalg.h"
#include "../src/semantic/rsvd.h"
#include "../src/semantic/sparse.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

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
    /* Jacobi: residual and orthogonality on a random symmetric 40 x 40 */
    enum { N = 40 };
    static double A[N * N], A0[N * N], V[N * N], W[N];
    for (int i = 0; i < N; ++i)
        for (int j = 0; j <= i; ++j)
            A[i * N + j] = A[j * N + i] = uni() - 0.5;
    memcpy(A0, A, sizeof A);
    int sweeps = xksm__jacobi_eig(A, N, W, V);
    double res = 0, orth = 0;
    for (int i = 0; i < N; ++i)
        for (int c = 0; c < N; ++c) {
            double av = 0, vv = 0;
            for (int k = 0; k < N; ++k) {
                av += A0[i * N + k] * V[k * N + c];
                vv += V[k * N + i] * V[k * N + c];
            }
            res = fmax(res, fabs(av - V[i * N + c] * W[c]));
            orth = fmax(orth, fabs(vv - (i == c)));
        }
    printf("jacobi: %d sweeps, max |AV - VW| %.2e, max |V^T V - I| %.2e\n", sweeps, res, orth);
    CHECK(res < 1e-9 && orth < 1e-9);
    int sorted = 1;
    for (int i = 1; i < N; ++i)
        sorted &= W[i - 1] >= W[i];
    CHECK(sorted);

    /* CholQR2: columns orthonormal */
    enum { M = 3000, L = 48 };
    float *y = malloc(sizeof(float) * M * L);
    xksm__gauss_fill(y, M, L, 7);
    CHECK(xksm__cholqr2(y, M, L));
    double *g = malloc(sizeof(double) * L * L), qerr = 0;
    xksm__gemm_tn(y, y, g, M, L, L);
    for (int i = 0; i < L; ++i)
        for (int j = 0; j < L; ++j)
            qerr = fmax(qerr, fabs(g[i * L + j] - (i == j)));
    printf("cholqr2: max |Q^T Q - I| %.2e\n", qerr);
    CHECK(qerr < 1e-5);

    /* CSR: duplicates summed, transpose round trip, SpMM == dense, pool == serial */
    enum { R = 300, C = 220, NT = 9000, LL = 20 };
    xksm_triplet *t = malloc(sizeof *t * (NT + 1));
    static float dense[R][C];
    for (int i = 0; i < NT; ++i) {
        t[i] = (xksm_triplet){(uint32_t)(uni() * (double)R), (uint32_t)(uni() * (double)C),
                             (float)(uni() - 0.5)};
        dense[t[i].r][t[i].c] += t[i].v;
    }
    xksm_csr a, at, att;
    CHECK(xksm__csr_from_triplets(&a, R, C, t, NT));
    CHECK(xksm__csr_transpose(&a, &at) && xksm__csr_transpose(&at, &att));
    CHECK(att.nnz == a.nnz && !memcmp(att.col, a.col, a.nnz * sizeof *a.col) &&
          !memcmp(att.rowptr, a.rowptr, (R + 1) * sizeof *a.rowptr));
    float *x = malloc(sizeof(float) * C * LL), *ys = malloc(sizeof(float) * R * LL);
    float *yp = malloc(sizeof(float) * R * LL);
    xksm__gauss_fill(x, C, LL, 3);
    xksm__spmm(NULL, &a, x, ys, LL);
    xksm_pool *pool = xksm__pool_create(8);
    xksm__spmm(pool, &a, x, yp, LL);
    double serr = 0;
    for (int r = 0; r < R; ++r)
        for (int c = 0; c < LL; ++c) {
            double ref = 0;
            for (int k = 0; k < C; ++k)
                ref += (double)dense[r][k] * x[k * LL + c];
            serr = fmax(serr, fabs(ref - ys[r * LL + c]));
        }
    printf("spmm: max |dense - csr| %.2e, pool %s serial\n", serr,
           memcmp(ys, yp, sizeof(float) * R * LL) ? "!=" : "==");
    CHECK(serr < 1e-4 && !memcmp(ys, yp, sizeof(float) * R * LL));

    /* rSVD: top singular values vs exact (dense eig of A^T A) */
    static double ata[C * C], vv[C * C], ww[C];
    for (int i = 0; i < C; ++i)
        for (int j = 0; j < C; ++j) {
            double s = 0;
            for (int r = 0; r < R; ++r)
                s += (double)dense[r][i] * dense[r][j];
            ata[i * C + j] = s;
        }
    xksm__jacobi_eig(ata, C, ww, vv);
    enum { K = 10 };
    float *u = malloc(sizeof(float) * R * K), sig[K], *v = malloc(sizeof(float) * C * K);
    CHECK(xksm__rsvd(pool, &a, &at, K, 16, 2, 11, u, sig, v));
    double worst = 0;
    for (int i = 0; i < K; ++i)
        worst = fmax(worst, fabs(sig[i] - sqrt(ww[i])) / sqrt(ww[i]));
    printf("rsvd k=%d p=16 q=2: sigma_1 %.4f (exact %.4f), worst relative error %.2e\n", K,
           sig[0], sqrt(ww[0]), worst);
    /* A random sparse matrix has a flat spectrum, the hardest case for a q = 2 sketch
     * (Halko et al. 2011, section 4.5). Correctness shows as convergence: more power
     * iterations and oversampling must drive the error down sharply. */
    double prev = worst;
    int monotone = 1;
    static const int qs[] = {4, 8, 16};
    for (int s = 0; s < 3; ++s) {
        CHECK(xksm__rsvd(pool, &a, &at, K, 48, qs[s], 11, u, sig, v));
        double e = 0;
        for (int i = 0; i < K; ++i)
            e = fmax(e, fabs(sig[i] - sqrt(ww[i])) / sqrt(ww[i]));
        printf("rsvd k=%d p=48 q=%d: worst relative error %.2e\n", K, qs[s], e);
        monotone &= e <= prev * 1.0001;
        prev = e;
    }
    CHECK(worst < 0.10 && monotone && prev < 1e-3);
    xksm__pool_destroy(pool);
    xksm__csr_free(&a);
    xksm__csr_free(&at);
    xksm__csr_free(&att);
    free(y); free(g); free(t); free(x); free(ys); free(yp); free(u); free(v);
    return test_report("linalg");
}
