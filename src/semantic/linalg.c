/* linalg.c: dense helpers for randomized SVD. Sizes here are small (l = k + p, about 144)
 * except the tall m dimension, so double precision is affordable where it matters. */
#include "linalg.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static uint64_t splitmix(uint64_t *s)
{
    uint64_t z = (*s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

void xksm__gauss_fill(float *a, size_t m, size_t n, uint64_t seed)
{
    uint64_t s = seed;
    size_t total = m * n;
    for (size_t i = 0; i < total; i += 2) { /* Box-Muller, two samples per pair */
        double u1 = ((double)(splitmix(&s) >> 11) + 0.5) * (1.0 / 9007199254740992.0);
        double u2 = (double)(splitmix(&s) >> 11) * (1.0 / 9007199254740992.0);
        double r = sqrt(-2.0 * log(u1)), t = 6.283185307179586 * u2;
        a[i] = (float)(r * cos(t));
        if (i + 1 < total)
            a[i + 1] = (float)(r * sin(t));
    }
}

void xksm__gemm_tn(const float *a, const float *b, double *c, size_t k, size_t m, size_t n)
{
    memset(c, 0, sizeof(double) * m * n);
    for (size_t r = 0; r < k; ++r) {
        const float *ar = a + r * m, *br = b + r * n;
        for (size_t i = 0; i < m; ++i) {
            double ai = ar[i];
            double *ci = c + i * n;
            for (size_t j = 0; j < n; ++j)
                ci[j] += ai * br[j];
        }
    }
}

/* One CholQR pass: G = Y^T Y, G = R^T R, Y = Y R^-1. */
static int cholqr(float *y, size_t m, size_t l)
{
    double *g = malloc(sizeof(double) * l * l);
    if (!g)
        return 0;
    xksm__gemm_tn(y, y, g, m, l, l);
    for (size_t j = 0; j < l; ++j) { /* Cholesky: upper R stored in g */
        for (size_t k = 0; k < j; ++k)
            g[j * l + j] -= g[k * l + j] * g[k * l + j];
        if (!(g[j * l + j] > 1e-300)) {
            free(g);
            return 0;
        }
        g[j * l + j] = sqrt(g[j * l + j]);
        for (size_t i = j + 1; i < l; ++i) {
            for (size_t k = 0; k < j; ++k)
                g[j * l + i] -= g[k * l + j] * g[k * l + i];
            g[j * l + i] /= g[j * l + j];
        }
    }
    double *row = malloc(sizeof(double) * l);
    if (!row) {
        free(g);
        return 0;
    }
    for (size_t r = 0; r < m; ++r) { /* solve x R = y_r for each row (forward substitution) */
        float *yr = y + r * l;
        for (size_t j = 0; j < l; ++j) {
            double s = yr[j];
            for (size_t k = 0; k < j; ++k)
                s -= row[k] * g[k * l + j];
            row[j] = s / g[j * l + j];
        }
        for (size_t j = 0; j < l; ++j)
            yr[j] = (float)row[j];
    }
    free(row);
    free(g);
    return 1;
}

int xksm__cholqr2(float *y, size_t m, size_t l)
{
    return cholqr(y, m, l) && cholqr(y, m, l);
}

int xksm__jacobi_eig(double *a, size_t n, double *w, double *v)
{
    for (size_t i = 0; i < n; ++i)
        for (size_t j = 0; j < n; ++j)
            v[i * n + j] = i == j;
    int sweep;
    for (sweep = 1; sweep <= 100; ++sweep) {
        double off = 0.0, diag = 0.0;
        for (size_t i = 0; i < n; ++i)
            for (size_t j = 0; j < n; ++j)
                if (i == j)
                    diag += a[i * n + j] * a[i * n + j];
                else
                    off += a[i * n + j] * a[i * n + j];
        if (off <= 1e-24 * (diag > 0 ? diag : 1.0))
            break;
        for (size_t p = 0; p < n; ++p)
            for (size_t q = p + 1; q < n; ++q) {
                double apq = a[p * n + q];
                if (fabs(apq) < 1e-300)
                    continue;
                double app = a[p * n + p], aqq = a[q * n + q];
                double theta = (aqq - app) / (2.0 * apq);
                double t = (theta >= 0 ? 1.0 : -1.0) / (fabs(theta) + sqrt(theta * theta + 1.0));
                double c = 1.0 / sqrt(t * t + 1.0), s = t * c;
                for (size_t k = 0; k < n; ++k) { /* rotate rows/columns p, q */
                    double akp = a[k * n + p], akq = a[k * n + q];
                    a[k * n + p] = c * akp - s * akq;
                    a[k * n + q] = s * akp + c * akq;
                }
                for (size_t k = 0; k < n; ++k) {
                    double apk = a[p * n + k], aqk = a[q * n + k];
                    a[p * n + k] = c * apk - s * aqk;
                    a[q * n + k] = s * apk + c * aqk;
                }
                for (size_t k = 0; k < n; ++k) {
                    double vkp = v[k * n + p], vkq = v[k * n + q];
                    v[k * n + p] = c * vkp - s * vkq;
                    v[k * n + q] = s * vkp + c * vkq;
                }
            }
    }
    /* sort descending, permuting eigenvector columns */
    for (size_t i = 0; i < n; ++i)
        w[i] = a[i * n + i];
    for (size_t i = 0; i < n; ++i) {
        size_t best = i;
        for (size_t j = i + 1; j < n; ++j)
            if (w[j] > w[best])
                best = j;
        if (best != i) {
            double t = w[i];
            w[i] = w[best];
            w[best] = t;
            for (size_t k = 0; k < n; ++k) {
                double tv = v[k * n + i];
                v[k * n + i] = v[k * n + best];
                v[k * n + best] = tv;
            }
        }
    }
    return sweep;
}

int xksm__chol_upper_inv(const double *gin, size_t l, double *rinv)
{
    double *g = malloc(sizeof(double) * l * l);
    if (!g)
        return 0;
    memcpy(g, gin, sizeof(double) * l * l);
    for (size_t j = 0; j < l; ++j) { /* upper Cholesky factor R in g (as in cholqr) */
        for (size_t k = 0; k < j; ++k)
            g[j * l + j] -= g[k * l + j] * g[k * l + j];
        if (!(g[j * l + j] > 1e-300)) {
            free(g);
            return 0;
        }
        g[j * l + j] = sqrt(g[j * l + j]);
        for (size_t i = j + 1; i < l; ++i) {
            for (size_t k = 0; k < j; ++k)
                g[j * l + i] -= g[k * l + j] * g[k * l + i];
            g[j * l + i] /= g[j * l + j];
        }
    }
    memset(rinv, 0, sizeof(double) * l * l); /* R X = I by back substitution, X upper */
    for (size_t c = 0; c < l; ++c)
        for (size_t ii = c + 1; ii-- > 0;) {
            double s = ii == c ? 1.0 : 0.0;
            for (size_t k = ii + 1; k <= c; ++k)
                s -= g[ii * l + k] * rinv[k * l + c];
            rinv[ii * l + c] = s / g[ii * l + ii];
        }
    free(g);
    return 1;
}

/* ---- fast paths: worker pool + AVX2/FMA (run-time dispatch), plain C fallback -------
 * Same formulation as the OpenCL backend: Gram matrices accumulated in f32 over 256-row
 * chunks and summed in double; Y R^-1 as a GEMM instead of a row-serial solve. */
#include <stdatomic.h>
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
#include <immintrin.h>
#define XKSM_LA_AVX2 1
#endif

static _Atomic int g_la_avx2 = -1;
static int la_avx2(void)
{
    int v = atomic_load_explicit(&g_la_avx2, memory_order_relaxed);
    if (v < 0) {
#ifdef XKSM_LA_AVX2
        __builtin_cpu_init();
        v = __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
#else
        v = 0;
#endif
        atomic_store_explicit(&g_la_avx2, v, memory_order_relaxed);
    }
    return v;
}

/* p[j0..n) += a * x[j0..n) */
static void axpy_c(float *p, const float *x, float a, size_t j0, size_t n)
{
    for (size_t j = j0; j < n; ++j)
        p[j] += a * x[j];
}
#ifdef XKSM_LA_AVX2
__attribute__((target("avx2,fma"))) static void axpy_avx2(float *p, const float *x, float a,
                                                          size_t j0, size_t n)
{
    __m256 va = _mm256_set1_ps(a);
    size_t j = j0;
    for (; j + 32 <= n; j += 32) {
        _mm256_storeu_ps(p + j, _mm256_fmadd_ps(va, _mm256_loadu_ps(x + j), _mm256_loadu_ps(p + j)));
        _mm256_storeu_ps(p + j + 8,
                         _mm256_fmadd_ps(va, _mm256_loadu_ps(x + j + 8), _mm256_loadu_ps(p + j + 8)));
        _mm256_storeu_ps(p + j + 16,
                         _mm256_fmadd_ps(va, _mm256_loadu_ps(x + j + 16), _mm256_loadu_ps(p + j + 16)));
        _mm256_storeu_ps(p + j + 24,
                         _mm256_fmadd_ps(va, _mm256_loadu_ps(x + j + 24), _mm256_loadu_ps(p + j + 24)));
    }
    for (; j + 8 <= n; j += 8)
        _mm256_storeu_ps(p + j, _mm256_fmadd_ps(va, _mm256_loadu_ps(x + j), _mm256_loadu_ps(p + j)));
    for (; j < n; ++j)
        p[j] += a * x[j];
}
#endif

typedef void (*axpy_fn)(float *, const float *, float, size_t, size_t);
static axpy_fn pick_axpy(void)
{
#ifdef XKSM_LA_AVX2
    if (la_avx2())
        return axpy_avx2;
#endif
    return axpy_c;
}

typedef struct gram_job {
    const float *y;
    size_t m, l;
    double *part; /* nworkers x l x l */
} gram_job;

static void gram_slice(void *pv, unsigned w, unsigned nw)
{
    gram_job *j = pv;
    size_t l = j->l, lo = j->m * w / nw, hi = j->m * (w + 1) / nw;
    double *g = j->part + (size_t)w * l * l;
    memset(g, 0, sizeof(double) * l * l);
    float *p = calloc(l * l, sizeof *p);
    if (!p)
        return;
    axpy_fn axpy = pick_axpy();
    for (size_t c0 = lo; c0 < hi; c0 += 256) { /* f32 over 256 rows, then into double */
        size_t c1 = c0 + 256 < hi ? c0 + 256 : hi;
        for (size_t r = c0; r < c1; ++r) {
            const float *yr = j->y + r * l;
            for (size_t i = 0; i < l; ++i)
                if (yr[i] != 0.0f)
                    axpy(p + i * l, yr, yr[i], i & ~(size_t)7, l); /* upper triangle */
        }
        for (size_t i = 0; i < l; ++i)
            for (size_t k = i; k < l; ++k) {
                g[i * l + k] += p[i * l + k];
                p[i * l + k] = 0.0f;
            }
        for (size_t i = 0; i < l; ++i) /* also clear the few lower cells the 8-aligned start touched */
            for (size_t k = i & ~(size_t)7; k < i; ++k)
                p[i * l + k] = 0.0f;
    }
    free(p);
}

int xksm__gram_fast(xksm_pool *pool, const float *y, size_t m, size_t l, double *g)
{
    unsigned nw = pool ? xksm__pool_size(pool) : 1;
    gram_job j = {y, m, l, malloc(sizeof(double) * nw * l * l)};
    if (!j.part)
        return 0;
    if (pool)
        xksm__pool_run(pool, gram_slice, &j);
    else
        gram_slice(&j, 0, 1);
    memset(g, 0, sizeof(double) * l * l);
    for (unsigned w = 0; w < nw; ++w) /* fixed order: deterministic result */
        for (size_t i = 0; i < l; ++i)
            for (size_t k = i; k < l; ++k)
                g[i * l + k] += j.part[(size_t)w * l * l + i * l + k];
    for (size_t i = 0; i < l; ++i)
        for (size_t k = 0; k < i; ++k)
            g[i * l + k] = g[k * l + i];
    free(j.part);
    return 1;
}

typedef struct gemm_job {
    const float *y, *mm;
    float *z;
    size_t m, l, n;
    int upper;
} gemm_job;

static void gemm_slice(void *pv, unsigned w, unsigned nw)
{
    gemm_job *j = pv;
    size_t lo = j->m * w / nw, hi = j->m * (w + 1) / nw;
    axpy_fn axpy = pick_axpy();
    for (size_t r = lo; r < hi; ++r) {
        const float *yr = j->y + r * j->l;
        float *zr = j->z + r * j->n;
        memset(zr, 0, sizeof(float) * j->n);
        for (size_t t = 0; t < j->l; ++t)
            if (yr[t] != 0.0f)
                axpy(zr, j->mm + t * j->n, yr[t], j->upper ? (t & ~(size_t)7) : 0, j->n);
    }
}

void xksm__gemm_fast(xksm_pool *pool, const float *y, size_t m, size_t l, const float *mm,
                     size_t n, int upper, float *z)
{
    gemm_job j = {y, mm, z, m, l, n, upper};
    if (pool)
        xksm__pool_run(pool, gemm_slice, &j);
    else
        gemm_slice(&j, 0, 1);
}

int xksm__cholqr2_fast(xksm_pool *pool, float *y, size_t m, size_t l, float *tmp)
{
    double *g = malloc(sizeof(double) * l * l), *rinv = malloc(sizeof(double) * l * l);
    float *rf = malloc(sizeof(float) * l * l);
    int ok = g && rinv && rf;
    for (int pass = 0; ok && pass < 2; ++pass) {
        ok = xksm__gram_fast(pool, y, m, l, g) && xksm__chol_upper_inv(g, l, rinv);
        if (!ok)
            break;
        for (size_t i = 0; i < l * l; ++i)
            rf[i] = (float)rinv[i];
        xksm__gemm_fast(pool, y, m, l, rf, l, 1, tmp); /* Y R^-1 (R^-1 upper triangular) */
        memcpy(y, tmp, sizeof(float) * m * l);
    }
    free(g);
    free(rinv);
    free(rf);
    return ok;
}

const char *xksm__linalg_kernel(void)
{
    return la_avx2() ? "avx2+fma" : "scalar";
}

void xksm__linalg_force_scalar(int on)
{
    atomic_store_explicit(&g_la_avx2, on ? 0 : -1, memory_order_relaxed);
}
