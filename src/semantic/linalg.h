/* linalg.h: private dense linear algebra for the semantic model (row-major). */
#ifndef XKSM_LINALG_H
#define XKSM_LINALG_H

#include "../core/pool.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Fill m x n with N(0, 1) samples from a fixed seed (deterministic builds). */
void xksm__gauss_fill(float *a, size_t m, size_t n, uint64_t seed);

/* Orthonormalize the columns of the tall m x l matrix Y in place (CholQR applied twice,
 * Gram matrix and Cholesky in double). Returns 0 if Y is numerically rank deficient. */
int xksm__cholqr2(float *y, size_t m, size_t l);

/* Symmetric eigen-decomposition of the n x n double matrix A (destroyed) by cyclic Jacobi:
 * eigenvalues to w (descending), eigenvectors as columns of V (n x n). Returns sweeps used. */
int xksm__jacobi_eig(double *a, size_t n, double *w, double *v);

/* Upper Cholesky factor R of the l x l SPD matrix g (G = R^T R) and its inverse R^-1 into
 * rinv (upper triangular). 0 if g is not numerically positive definite. */
int xksm__chol_upper_inv(const double *g, size_t l, double *rinv);

/* Fast paths (worker pool + AVX2/FMA when the CPU has it; plain C otherwise):
 * G (l x l, double) = Y^T Y with f32 partials over 256-row chunks summed in double;
 * Z (m x n) = Y (m x l) M (l x n), upper = 1 when M is upper triangular;
 * CholQR2 as twice { Gram; R^-1; Y R^-1 } (tmp: m x l scratch). */
int xksm__gram_fast(xksm_pool *pool, const float *y, size_t m, size_t l, double *g);
void xksm__gemm_fast(xksm_pool *pool, const float *y, size_t m, size_t l, const float *mm,
                     size_t n, int upper, float *z);
int xksm__cholqr2_fast(xksm_pool *pool, float *y, size_t m, size_t l, float *tmp);
const char *xksm__linalg_kernel(void);
void xksm__linalg_force_scalar(int on); /* tests: 1 = plain C paths, 0 = CPUID again */

/* C (m x n) = A^T (k x m, row-major) * B (k x n): Gram-type products, double accumulate. */
void xksm__gemm_tn(const float *a, const float *b, double *c, size_t k, size_t m, size_t n);

#ifdef __cplusplus
}
#endif
#endif
