/* rsvd.cl: OpenCL C kernels for the randomized SVD (f32). Row-major dense matrices.
 * Host side: backends/opencl/ocl_rsvd.c. LS = work-group size of spmm (multiple of 32). */

#ifndef LS
#define LS 160
#endif

/* Y (rows x l) = A (CSR) * X (cols x l). One work-group per row; lane c owns column c.
 * The row's (col, val) entries are staged through local memory in tiles of LS. */
__kernel __attribute__((reqd_work_group_size(LS, 1, 1)))
void spmm_csr(__global const uint *rowptr, __global const uint *col,
              __global const float *val, __global const float *X,
              __global float *Y, const uint l)
{
    const uint row = get_group_id(0), c = get_local_id(0);
    __local uint lc[LS];
    __local float lv[LS];
    const uint beg = rowptr[row], end = rowptr[row + 1];
    float acc = 0.0f;
    for (uint base = beg; base < end; base += LS) {
        const uint i = base + c;
        if (i < end) {
            lc[c] = col[i];
            lv[c] = val[i];
        }
        barrier(CLK_LOCAL_MEM_FENCE);
        const uint n = min((uint)LS, end - base);
        if (c < l)
            for (uint t = 0; t < n; ++t)
                acc = fma(lv[t], X[(size_t)lc[t] * l + c], acc);
        barrier(CLK_LOCAL_MEM_FENCE);
    }
    if (c < l)
        Y[(size_t)row * l + c] = acc;
}

/* Partial Gram matrices: P[g] (l x l) = sum over rows r in chunk g of Y[r]^T Y[r].
 * Work-item (i, j, g). Chunks are short, so f32 partial sums stay accurate; the host
 * adds the partials in double. */
__kernel void gram_partial(__global const float *Y, __global float *P, const uint m,
                           const uint l, const uint chunk)
{
    const uint i = get_global_id(0), j = get_global_id(1), g = get_global_id(2);
    if (i >= l || j >= l)
        return;
    const uint r0 = g * chunk, r1 = min(m, r0 + chunk);
    float acc = 0.0f;
    for (uint r = r0; r < r1; ++r)
        acc = fma(Y[(size_t)r * l + i], Y[(size_t)r * l + j], acc);
    P[((size_t)g * l + i) * l + j] = acc;
}

/* Z (m x n) = Y (m x l) * M (l x n), M small. Work-item (j, r). */
__kernel void gemm_small(__global const float *Y, __global const float *M, __global float *Z,
                         const uint m, const uint l, const uint n)
{
    const uint j = get_global_id(0), r = get_global_id(1);
    if (j >= n || r >= m)
        return;
    const __global float *yr = Y + (size_t)r * l;
    float acc = 0.0f;
    for (uint t = 0; t < l; ++t)
        acc = fma(yr[t], M[(size_t)t * n + j], acc);
    Z[(size_t)r * n + j] = acc;
}
