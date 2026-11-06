/* kernels.c: vector kernels. Scalar references always compiled; AVX2/FMA versions are
 * selected at run time by CPUID and verified against the references by tests. */
#include "kernels.h"

#include <math.h>
#include <stdatomic.h>

float xksm__dot_f32_scalar(const float *a, const float *b, size_t n)
{
    float s = 0.0f;
    for (size_t i = 0; i < n; ++i)
        s += a[i] * b[i];
    return s;
}

int32_t xksm__dot_i8_scalar(const int8_t *a, const int8_t *b, size_t n)
{
    int32_t s = 0;
    for (size_t i = 0; i < n; ++i)
        s += (int32_t)a[i] * (int32_t)b[i];
    return s;
}

float xksm__lattice_encode(const float *x, size_t n, int8_t *code)
{
    float m = 0.0f;
    for (size_t i = 0; i < n; ++i)
        m = fmaxf(m, fabsf(x[i]));
    if (m == 0.0f) {
        for (size_t i = 0; i < n; ++i)
            code[i] = 0;
        return 0.0f;
    }
    float scale = m / 127.0f, inv = 127.0f / m;
    for (size_t i = 0; i < n; ++i) {
        long q = lrintf(x[i] * inv);
        code[i] = (int8_t)(q > 127 ? 127 : q < -127 ? -127 : q);
    }
    return scale;
}

void xksm__normalize(float *x, size_t n)
{
    double s = 0.0;
    for (size_t i = 0; i < n; ++i)
        s += (double)x[i] * (double)x[i];
    if (s == 0.0)
        return;
    float inv = (float)(1.0 / sqrt(s));
    for (size_t i = 0; i < n; ++i)
        x[i] *= inv;
}

#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
#include <immintrin.h>
#define XKSM_HAVE_AVX2 1

__attribute__((target("avx2,fma"))) static float dot_f32_avx2(const float *a, const float *b,
                                                               size_t n)
{
    __m256 s0 = _mm256_setzero_ps(), s1 = s0, s2 = s0, s3 = s0;
    size_t i = 0;
    for (; i + 32 <= n; i += 32) { /* 4 independent chains hide the 4-cycle FMA latency */
        s0 = _mm256_fmadd_ps(_mm256_loadu_ps(a + i), _mm256_loadu_ps(b + i), s0);
        s1 = _mm256_fmadd_ps(_mm256_loadu_ps(a + i + 8), _mm256_loadu_ps(b + i + 8), s1);
        s2 = _mm256_fmadd_ps(_mm256_loadu_ps(a + i + 16), _mm256_loadu_ps(b + i + 16), s2);
        s3 = _mm256_fmadd_ps(_mm256_loadu_ps(a + i + 24), _mm256_loadu_ps(b + i + 24), s3);
    }
    for (; i + 8 <= n; i += 8)
        s0 = _mm256_fmadd_ps(_mm256_loadu_ps(a + i), _mm256_loadu_ps(b + i), s0);
    __m256 s = _mm256_add_ps(_mm256_add_ps(s0, s1), _mm256_add_ps(s2, s3));
    __m128 h = _mm_add_ps(_mm256_castps256_ps128(s), _mm256_extractf128_ps(s, 1));
    h = _mm_add_ps(h, _mm_movehl_ps(h, h));
    h = _mm_add_ss(h, _mm_movehdup_ps(h));
    float r = _mm_cvtss_f32(h);
    for (; i < n; ++i)
        r += a[i] * b[i];
    return r;
}

/* |a| * (b * sign(a)) turns signed x signed into the unsigned x signed form vpmaddubsw
 * takes; pair sums peak at 2 * 127 * 127 = 32258 < 32767, so no saturation. */
__attribute__((target("avx2"))) static int32_t dot_i8_avx2(const int8_t *a, const int8_t *b,
                                                           size_t n)
{
    const __m256i ones = _mm256_set1_epi16(1);
    __m256i acc0 = _mm256_setzero_si256(), acc1 = acc0;
    size_t i = 0;
    for (; i + 64 <= n; i += 64) {
        __m256i a0 = _mm256_loadu_si256((const __m256i *)(a + i));
        __m256i b0 = _mm256_loadu_si256((const __m256i *)(b + i));
        __m256i a1 = _mm256_loadu_si256((const __m256i *)(a + i + 32));
        __m256i b1 = _mm256_loadu_si256((const __m256i *)(b + i + 32));
        __m256i p0 = _mm256_maddubs_epi16(_mm256_sign_epi8(a0, a0), _mm256_sign_epi8(b0, a0));
        __m256i p1 = _mm256_maddubs_epi16(_mm256_sign_epi8(a1, a1), _mm256_sign_epi8(b1, a1));
        acc0 = _mm256_add_epi32(acc0, _mm256_madd_epi16(p0, ones));
        acc1 = _mm256_add_epi32(acc1, _mm256_madd_epi16(p1, ones));
    }
    for (; i + 32 <= n; i += 32) {
        __m256i a0 = _mm256_loadu_si256((const __m256i *)(a + i));
        __m256i b0 = _mm256_loadu_si256((const __m256i *)(b + i));
        __m256i p0 = _mm256_maddubs_epi16(_mm256_sign_epi8(a0, a0), _mm256_sign_epi8(b0, a0));
        acc0 = _mm256_add_epi32(acc0, _mm256_madd_epi16(p0, ones));
    }
    __m256i acc = _mm256_add_epi32(acc0, acc1);
    __m128i h = _mm_add_epi32(_mm256_castsi256_si128(acc), _mm256_extracti128_si256(acc, 1));
    h = _mm_add_epi32(h, _mm_shuffle_epi32(h, 0x4E));
    h = _mm_add_epi32(h, _mm_shuffle_epi32(h, 0xB1));
    int32_t r = _mm_cvtsi128_si32(h);
    for (; i < n; ++i)
        r += (int32_t)a[i] * (int32_t)b[i];
    return r;
}
#endif

/* Lazily resolved, possibly first from several pool workers at once: atomic, so the
 * concurrent first use is defined behaviour (every racer stores the same value). Relaxed
 * order suffices: the value carries no other data. On x86 the load is a plain mov. */
static _Atomic int g_avx2 = -1;

static int have_avx2(void)
{
    int v = atomic_load_explicit(&g_avx2, memory_order_relaxed);
    if (v < 0) {
#ifdef XKSM_HAVE_AVX2
        __builtin_cpu_init();
        v = __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
#else
        v = 0;
#endif
        atomic_store_explicit(&g_avx2, v, memory_order_relaxed);
    }
    return v;
}

float xksm__dot_f32(const float *a, const float *b, size_t n)
{
#ifdef XKSM_HAVE_AVX2
    if (have_avx2())
        return dot_f32_avx2(a, b, n);
#endif
    return xksm__dot_f32_scalar(a, b, n);
}

int32_t xksm__dot_i8(const int8_t *a, const int8_t *b, size_t n)
{
#ifdef XKSM_HAVE_AVX2
    if (have_avx2())
        return dot_i8_avx2(a, b, n);
#endif
    return xksm__dot_i8_scalar(a, b, n);
}

const char *xksm__vector_kernel(void)
{
    return have_avx2() ? "avx2+fma" : "scalar";
}
