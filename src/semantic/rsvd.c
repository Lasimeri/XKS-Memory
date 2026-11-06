/* rsvd.c: randomized truncated SVD (Halko, Martinsson, Tropp 2011, Algorithm 4.4 + 5.1):
 * Gaussian sketch, q power iterations with re-orthonormalization, small SVD via the
 * eigen-decomposition of B B^T. Large products: CSR x dense (xksm__spmm), dense Gram and
 * GEMM through the pool + AVX2/FMA fast paths in linalg.c. */
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
    float *z = malloc(sizeof(float) * n * l), *tmp = malloc(sizeof(float) * (m > n ? m : n) * l);
    double *g = malloc(sizeof(double) * l * l), *w = malloc(sizeof(double) * l);
    double *ub = malloc(sizeof(double) * l * l);
    float *ubf = malloc(sizeof(float) * l * k);
    int ok = om && y && z && tmp && g && w && ub && ubf;
    if (ok) {
        xksm__gauss_fill(om, n, l, seed);
        xksm__spmm(pool, a, om, y, l); /* Y = A Omega */
        ok = xksm__cholqr2_fast(pool, y, m, l, tmp);
        for (int it = 0; ok && it < q; ++it) {
            xksm__spmm(pool, at, y, z, l); /* Z = A^T Y */
            ok = xksm__cholqr2_fast(pool, z, n, l, tmp);
            if (ok) {
                xksm__spmm(pool, a, z, y, l); /* Y = A Z */
                ok = xksm__cholqr2_fast(pool, y, m, l, tmp);
            }
        }
    }
    if (ok) {
        xksm__spmm(pool, at, y, z, l);       /* Bt = A^T Q, n x l (B = Q^T A) */
        ok = xksm__gram_fast(pool, z, n, l, g); /* B B^T = Bt^T Bt, l x l */
    }
    if (ok) {
        xksm__jacobi_eig(g, l, w, ub);
        for (size_t i = 0; i < k; ++i)
            sigma[i] = (float)sqrt(w[i] > 0 ? w[i] : 0.0);
        for (size_t t = 0; t < l; ++t)
            for (size_t c = 0; c < k; ++c)
                ubf[t * k + c] = (float)ub[t * l + c];
        xksm__gemm_fast(pool, y, m, l, ubf, k, 0, u); /* U = Q U_B[:, :k] */
        if (v) { /* V = Bt U_B[:, :k] / sigma */
            xksm__gemm_fast(pool, z, n, l, ubf, k, 0, v);
            for (size_t r = 0; r < n; ++r)
                for (size_t c = 0; c < k; ++c)
                    v[r * k + c] = sigma[c] > 0 ? v[r * k + c] / sigma[c] : 0.0f;
        }
    }
    free(om);
    free(y);
    free(z);
    free(tmp);
    free(ubf);
    free(g);
    free(w);
    free(ub);
    return ok;
}
