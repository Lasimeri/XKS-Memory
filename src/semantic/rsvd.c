/* rsvd.c: randomized truncated SVD (Halko, Martinsson, Tropp 2011, Algorithm 4.4 + 5.1):
 * Gaussian sketch, q power iterations with re-orthonormalization, small SVD via the
 * eigen-decomposition of B B^T. All large products are CSR x dense (xksm__spmm). */
#include "rsvd.h"

#include "linalg.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

int xksm__rsvd(xksm_pool *pool, const xksm_csr *a, const xksm_csr *at, size_t k, size_t p,
               int q, uint64_t seed, float *u, float *sigma, float *v)
{
    size_t m = a->rows, n = a->cols, l = k + p;
    if (l > m || l > n || k == 0)
        return 0;
    float *om = malloc(sizeof(float) * n * l), *y = malloc(sizeof(float) * m * l);
    float *z = malloc(sizeof(float) * n * l);
    double *g = malloc(sizeof(double) * l * l), *w = malloc(sizeof(double) * l);
    double *ub = malloc(sizeof(double) * l * l);
    int ok = om && y && z && g && w && ub;
    if (ok) {
        xksm__gauss_fill(om, n, l, seed);
        xksm__spmm(pool, a, om, y, l); /* Y = A Omega */
        ok = xksm__cholqr2(y, m, l);
        for (int it = 0; ok && it < q; ++it) {
            xksm__spmm(pool, at, y, z, l); /* Z = A^T Y */
            ok = xksm__cholqr2(z, n, l);
            if (ok) {
                xksm__spmm(pool, a, z, y, l); /* Y = A Z */
                ok = xksm__cholqr2(y, m, l);
            }
        }
    }
    if (ok) {
        xksm__spmm(pool, at, y, z, l);       /* Bt = A^T Q, n x l (B = Q^T A) */
        xksm__gemm_tn(z, z, g, n, l, l);     /* B B^T = Bt^T Bt, l x l */
        xksm__jacobi_eig(g, l, w, ub);
        for (size_t i = 0; i < k; ++i)
            sigma[i] = (float)sqrt(w[i] > 0 ? w[i] : 0.0);
        for (size_t r = 0; r < m; ++r)       /* U = Q U_B[:, :k] */
            for (size_t c = 0; c < k; ++c) {
                double s = 0;
                for (size_t t = 0; t < l; ++t)
                    s += (double)y[r * l + t] * ub[t * l + c];
                u[r * k + c] = (float)s;
            }
        if (v)
            for (size_t r = 0; r < n; ++r)   /* V = Bt U_B[:, :k] / sigma */
                for (size_t c = 0; c < k; ++c) {
                    double s = 0;
                    for (size_t t = 0; t < l; ++t)
                        s += (double)z[r * l + t] * ub[t * l + c];
                    v[r * k + c] = sigma[c] > 0 ? (float)(s / sigma[c]) : 0.0f;
                }
    }
    free(om);
    free(y);
    free(z);
    free(g);
    free(w);
    free(ub);
    return ok;
}
