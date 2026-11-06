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
