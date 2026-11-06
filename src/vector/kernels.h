/* kernels.h: private vector kernels (CPU). Square-lattice (Z^n) int8 codes plus f32. */
#ifndef XKSM_VECTOR_KERNELS_H
#define XKSM_VECTOR_KERNELS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* f32 dot product: plain C reference and the run-time selected best kernel */
float xksm__dot_f32_scalar(const float *a, const float *b, size_t n);
float xksm__dot_f32(const float *a, const float *b, size_t n);

/* int8 lattice dot product (codes in [-127, 127]) */
int32_t xksm__dot_i8_scalar(const int8_t *a, const int8_t *b, size_t n);
int32_t xksm__dot_i8(const int8_t *a, const int8_t *b, size_t n);

/* Square-lattice quantization: scale = max|x| / 127, code = round(x / scale) in
 * [-127, 127]. Returns the scale; x ~= scale * code. */
float xksm__lattice_encode(const float *x, size_t n, int8_t *code);

/* Normalize x to unit length in place (no-op for the zero vector). */
void xksm__normalize(float *x, size_t n);

/* Name of the selected kernels, e.g. "avx2+fma" or "scalar". */
const char *xksm__vector_kernel(void);

#ifdef __cplusplus
}
#endif
#endif
