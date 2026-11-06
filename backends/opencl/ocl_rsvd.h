/* ocl_rsvd.h: randomized SVD on an OpenCL GPU (optional backend, XKSM_OPENCL). */
#ifndef XKSM_OCL_RSVD_H
#define XKSM_OCL_RSVD_H

#include "../../src/semantic/sparse.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Same contract as xksm__rsvd (src/semantic/rsvd.h), computed on the first OpenCL GPU.
 * Returns 1 on success; 0 if no usable device or any OpenCL call failed (the caller then
 * runs the CPU path). device receives the device name. */
int xksm__ocl_rsvd(const xksm_csr *a, const xksm_csr *at, size_t k, size_t p, int q,
                   uint64_t seed, float *u, float *sigma, float *v, char *device,
                   size_t device_len);

#ifdef __cplusplus
}
#endif
#endif
