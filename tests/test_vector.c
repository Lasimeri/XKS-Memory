/* test_vector.c: vector kernels against references for every length 0..1100 (all tail
 * paths), int8 saturation worst case, square-lattice encoding bounds. */
#include "test.h"

#include "../src/vector/kernels.h"

#include <math.h>
#include <stdlib.h>

static uint64_t rng = 0x9e3779b97f4a7c15u;
static double uni(void) /* [0, 1) */
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return (double)(rng >> 11) * (1.0 / 9007199254740992.0);
}

int main(void)
{
    printf("kernel: %s\n", xksm__vector_kernel());
    enum { MAXN = 1100 };
    static float a[MAXN], b[MAXN], r[MAXN];
    static int8_t qa[MAXN], qb[MAXN];
    int f32_bad = 0, i8_bad = 0, enc_bad = 0;
    double worst_rel = 0.0;

    for (size_t n = 0; n <= MAXN; ++n) {
        for (size_t i = 0; i < n; ++i) {
            a[i] = (float)(uni() * 2.0 - 1.0);
            b[i] = (float)(uni() * 2.0 - 1.0);
        }
        /* f32: compare with a double-precision reference, relative to sum |a_i b_i| */
        double ref = 0.0, mag = 0.0;
        for (size_t i = 0; i < n; ++i) {
            ref += (double)a[i] * (double)b[i];
            mag += fabs((double)a[i] * (double)b[i]);
        }
        double got = (double)xksm__dot_f32(a, b, n);
        double rel = mag > 0 ? fabs(got - ref) / mag : fabs(got);
        if (rel > worst_rel)
            worst_rel = rel;
        f32_bad += rel > 1e-5;

        /* lattice encode: bounds and reconstruction error <= scale / 2 */
        float sa = xksm__lattice_encode(a, n, qa);
        float sb = xksm__lattice_encode(b, n, qb);
        int maxc = 0;
        for (size_t i = 0; i < n; ++i) {
            int c = qa[i] < 0 ? -qa[i] : qa[i];
            maxc = c > maxc ? c : maxc;
            enc_bad += qa[i] == -128 || fabsf(a[i] - sa * (float)qa[i]) > sa * 0.5f * 1.0001f;
        }
        enc_bad += n > 0 && maxc != 127;
        (void)sb;
        /* int8: exact against the scalar reference */
        i8_bad += xksm__dot_i8(qa, qb, n) != xksm__dot_i8_scalar(qa, qb, n);
    }
    printf("f32 worst relative error %.3g over lengths 0..%d\n", worst_rel, MAXN);
    CHECK(f32_bad == 0);
    CHECK(i8_bad == 0);
    CHECK(enc_bad == 0);

    /* saturation worst case: every pair sum is 2 * 127 * 127 */
    for (size_t i = 0; i < MAXN; ++i) {
        qa[i] = (int8_t)(i & 1 ? -127 : 127);
        qb[i] = qa[i];
    }
    CHECK(xksm__dot_i8(qa, qb, MAXN) == 127 * 127 * MAXN);
    for (size_t i = 0; i < MAXN; ++i)
        qb[i] = (int8_t)-qa[i];
    CHECK(xksm__dot_i8(qa, qb, MAXN) == -127 * 127 * MAXN);

    /* zero vector, normalization */
    for (size_t i = 0; i < 64; ++i)
        r[i] = 0.0f;
    CHECK(xksm__lattice_encode(r, 64, qa) == 0.0f && qa[0] == 0 && qa[63] == 0);
    for (size_t i = 0; i < 768; ++i)
        r[i] = (float)(uni() - 0.5);
    xksm__normalize(r, 768);
    CHECK(fabs((double)xksm__dot_f32_scalar(r, r, 768) - 1.0) < 1e-5);
    return test_report("vector");
}
