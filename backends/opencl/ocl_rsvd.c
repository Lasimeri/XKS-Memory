/* ocl_rsvd.c: randomized SVD with every large operation on an OpenCL GPU.
 * Same algorithm and the same Gaussian sketch as src/semantic/rsvd.c. Y, Z and Bt stay in
 * device memory; per orthonormalization only the partial Gram matrices come back (f32
 * partials summed in double on the host) and R^-1 (l x l) goes out. */
#define CL_TARGET_OPENCL_VERSION 120
#include "ocl_rsvd.h"

#include "../../src/semantic/linalg.h"
#include "rsvd_cl.h" /* k_rsvd_cl[]: kernel source, generated from rsvd.cl by CMake */

#include <CL/cl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct ocl {
    cl_context ctx;
    cl_command_queue q;
    cl_program prog;
    cl_kernel spmm, gram, gemm;
    size_t ls;
} ocl;

typedef struct dcsr {
    cl_mem rowptr, col, val;
    uint32_t rows;
} dcsr;

static void ocl_release(ocl *o)
{
    if (o->spmm) clReleaseKernel(o->spmm);
    if (o->gram) clReleaseKernel(o->gram);
    if (o->gemm) clReleaseKernel(o->gemm);
    if (o->prog) clReleaseProgram(o->prog);
    if (o->q) clReleaseCommandQueue(o->q);
    if (o->ctx) clReleaseContext(o->ctx);
}

static int ocl_init(ocl *o, size_t l, char *device, size_t device_len)
{
    memset(o, 0, sizeof *o);
    cl_platform_id plats[8];
    cl_uint np = 0;
    if (clGetPlatformIDs(8, plats, &np) != CL_SUCCESS || np == 0)
        return 0;
    cl_device_id dev = NULL;
    for (cl_uint i = 0; i < np && !dev; ++i) {
        cl_uint nd = 0;
        if (clGetDeviceIDs(plats[i], CL_DEVICE_TYPE_GPU, 1, &dev, &nd) != CL_SUCCESS || !nd)
            dev = NULL;
    }
    if (!dev)
        return 0;
    if (device && device_len)
        clGetDeviceInfo(dev, CL_DEVICE_NAME, device_len, device, NULL);
    size_t maxwg = 0;
    clGetDeviceInfo(dev, CL_DEVICE_MAX_WORK_GROUP_SIZE, sizeof maxwg, &maxwg, NULL);
    o->ls = (l + 31) / 32 * 32;
    if (o->ls > maxwg)
        return 0;
    cl_int err;
    o->ctx = clCreateContext(NULL, 1, &dev, NULL, NULL, &err);
    if (err != CL_SUCCESS)
        return 0;
    o->q = clCreateCommandQueue(o->ctx, dev, 0, &err);
    if (err != CL_SUCCESS)
        return 0;
    const char *src = (const char *)k_rsvd_cl;
    o->prog = clCreateProgramWithSource(o->ctx, 1, &src, NULL, &err);
    if (err != CL_SUCCESS)
        return 0;
    char opts[64];
    snprintf(opts, sizeof opts, "-cl-std=CL1.2 -DLS=%zu", o->ls);
    if (clBuildProgram(o->prog, 1, &dev, opts, NULL, NULL) != CL_SUCCESS) {
        char log[4096] = {0};
        clGetProgramBuildInfo(o->prog, dev, CL_PROGRAM_BUILD_LOG, sizeof log - 1, log, NULL);
        fprintf(stderr, "xksm opencl build failed:\n%s\n", log);
        return 0;
    }
    o->spmm = clCreateKernel(o->prog, "spmm_csr", &err);
    if (err == CL_SUCCESS)
        o->gram = clCreateKernel(o->prog, "gram_partial", &err);
    if (err == CL_SUCCESS)
        o->gemm = clCreateKernel(o->prog, "gemm_small", &err);
    return err == CL_SUCCESS;
}

static cl_mem upload(ocl *o, const void *p, size_t n, int *ok)
{
    cl_int err;
    cl_mem m = clCreateBuffer(o->ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, n ? n : 4,
                              (void *)p, &err);
    if (err != CL_SUCCESS)
        *ok = 0;
    return m;
}

static cl_mem scratch(ocl *o, size_t n, int *ok)
{
    cl_int err;
    cl_mem m = clCreateBuffer(o->ctx, CL_MEM_READ_WRITE, n, NULL, &err);
    if (err != CL_SUCCESS)
        *ok = 0;
    return m;
}

static int dcsr_upload(ocl *o, const xksm_csr *a, dcsr *d)
{
    int ok = a->nnz < UINT32_MAX;
    uint32_t *rp = malloc(sizeof *rp * ((size_t)a->rows + 1));
    if (!rp)
        return 0;
    for (uint32_t r = 0; r <= a->rows; ++r)
        rp[r] = (uint32_t)a->rowptr[r];
    d->rows = a->rows;
    d->rowptr = upload(o, rp, sizeof *rp * ((size_t)a->rows + 1), &ok);
    d->col = upload(o, a->col, sizeof *a->col * (size_t)a->nnz, &ok);
    d->val = upload(o, a->val, sizeof *a->val * (size_t)a->nnz, &ok);
    free(rp);
    return ok;
}

static void dcsr_release(dcsr *d)
{
    if (d->rowptr) clReleaseMemObject(d->rowptr);
    if (d->col) clReleaseMemObject(d->col);
    if (d->val) clReleaseMemObject(d->val);
}

static int spmm(ocl *o, const dcsr *a, cl_mem x, cl_mem y, cl_uint l)
{
    size_t g = (size_t)a->rows * o->ls, lsz = o->ls;
    return clSetKernelArg(o->spmm, 0, sizeof(cl_mem), &a->rowptr) == CL_SUCCESS &&
           clSetKernelArg(o->spmm, 1, sizeof(cl_mem), &a->col) == CL_SUCCESS &&
           clSetKernelArg(o->spmm, 2, sizeof(cl_mem), &a->val) == CL_SUCCESS &&
           clSetKernelArg(o->spmm, 3, sizeof(cl_mem), &x) == CL_SUCCESS &&
           clSetKernelArg(o->spmm, 4, sizeof(cl_mem), &y) == CL_SUCCESS &&
           clSetKernelArg(o->spmm, 5, sizeof(cl_uint), &l) == CL_SUCCESS &&
           clEnqueueNDRangeKernel(o->q, o->spmm, 1, NULL, &g, &lsz, 0, NULL, NULL) == CL_SUCCESS;
}

enum { CHUNK = 256 };

/* G (l x l, double) = Y^T Y for device Y (m x l) */
static int gram(ocl *o, cl_mem y, cl_uint m, cl_uint l, cl_mem part, float *hpart, double *g)
{
    cl_uint chunk = CHUNK, nch = (m + CHUNK - 1) / CHUNK;
    size_t gsz[3] = {(l + 15) / 16 * 16, (l + 15) / 16 * 16, nch}, lsz[3] = {16, 16, 1};
    if (clSetKernelArg(o->gram, 0, sizeof(cl_mem), &y) != CL_SUCCESS ||
        clSetKernelArg(o->gram, 1, sizeof(cl_mem), &part) != CL_SUCCESS ||
        clSetKernelArg(o->gram, 2, sizeof(cl_uint), &m) != CL_SUCCESS ||
        clSetKernelArg(o->gram, 3, sizeof(cl_uint), &l) != CL_SUCCESS ||
        clSetKernelArg(o->gram, 4, sizeof(cl_uint), &chunk) != CL_SUCCESS ||
        clEnqueueNDRangeKernel(o->q, o->gram, 3, NULL, gsz, lsz, 0, NULL, NULL) != CL_SUCCESS ||
        clEnqueueReadBuffer(o->q, part, CL_TRUE, 0, sizeof(float) * nch * l * l, hpart, 0, NULL,
                            NULL) != CL_SUCCESS)
        return 0;
    memset(g, 0, sizeof(double) * l * l);
    for (cl_uint c = 0; c < nch; ++c) /* partials summed in double */
        for (size_t i = 0; i < (size_t)l * l; ++i)
            g[i] += hpart[(size_t)c * l * l + i];
    return 1;
}

static int gemm(ocl *o, cl_mem y, cl_mem mm, cl_mem z, cl_uint m, cl_uint l, cl_uint n)
{
    size_t gsz[2] = {(n + 31) / 32 * 32, (m + 7) / 8 * 8}, lsz[2] = {32, 8};
    return clSetKernelArg(o->gemm, 0, sizeof(cl_mem), &y) == CL_SUCCESS &&
           clSetKernelArg(o->gemm, 1, sizeof(cl_mem), &mm) == CL_SUCCESS &&
           clSetKernelArg(o->gemm, 2, sizeof(cl_mem), &z) == CL_SUCCESS &&
           clSetKernelArg(o->gemm, 3, sizeof(cl_uint), &m) == CL_SUCCESS &&
           clSetKernelArg(o->gemm, 4, sizeof(cl_uint), &l) == CL_SUCCESS &&
           clSetKernelArg(o->gemm, 5, sizeof(cl_uint), &n) == CL_SUCCESS &&
           clEnqueueNDRangeKernel(o->q, o->gemm, 2, NULL, gsz, lsz, 0, NULL, NULL) == CL_SUCCESS;
}

/* CholQR2 on device Y (m x l): twice { G = Y^T Y; R^-1 on host; Y = Y R^-1 } */
static int orth(ocl *o, cl_mem *y, cl_mem *tmp, cl_uint m, cl_uint l, cl_mem part,
                float *hpart, double *g, double *rinv, float *rinvf, cl_mem dm)
{
    for (int pass = 0; pass < 2; ++pass) {
        if (!gram(o, *y, m, l, part, hpart, g) || !xksm__chol_upper_inv(g, l, rinv))
            return 0;
        for (size_t i = 0; i < (size_t)l * l; ++i)
            rinvf[i] = (float)rinv[i];
        if (clEnqueueWriteBuffer(o->q, dm, CL_TRUE, 0, sizeof(float) * l * l, rinvf, 0, NULL,
                                 NULL) != CL_SUCCESS ||
            !gemm(o, *y, dm, *tmp, m, l, l))
            return 0;
        cl_mem s = *y;
        *y = *tmp;
        *tmp = s;
    }
    return 1;
}

int xksm__ocl_rsvd(const xksm_csr *a, const xksm_csr *at, size_t k, size_t p, int q,
                   uint64_t seed, float *u, float *sigma, float *v, char *device,
                   size_t device_len)
{
    cl_uint m = a->rows, n = a->cols, l = (cl_uint)(k + p);
    if (l > m || l > n || k == 0)
        return 0;
    ocl o;
    if (!ocl_init(&o, l, device, device_len)) {
        ocl_release(&o);
        return 0;
    }
    int ok = 1;
    dcsr dA = {0}, dAt = {0};
    ok = dcsr_upload(&o, a, &dA) && dcsr_upload(&o, at, &dAt);
    cl_uint big = m > n ? m : n, nch = (big + CHUNK - 1) / CHUNK;
    float *om = malloc(sizeof(float) * n * l), *hpart = malloc(sizeof(float) * nch * l * l);
    float *rinvf = malloc(sizeof(float) * l * l), *mk = malloc(sizeof(float) * l * k);
    double *g = malloc(sizeof(double) * l * l), *rinv = malloc(sizeof(double) * l * l);
    double *w = malloc(sizeof(double) * l), *ub = malloc(sizeof(double) * l * l);
    ok = ok && om && hpart && rinvf && mk && g && rinv && w && ub;
    cl_mem dX = NULL, dY = NULL, dT = NULL, dZ = NULL, dT2 = NULL, dP = NULL, dM = NULL,
           dO = NULL;
    if (ok) {
        xksm__gauss_fill(om, n, l, seed); /* the same sketch as the CPU path */
        dX = upload(&o, om, sizeof(float) * n * l, &ok);
        dY = scratch(&o, sizeof(float) * m * l, &ok);
        dT = scratch(&o, sizeof(float) * m * l, &ok);
        dZ = scratch(&o, sizeof(float) * n * l, &ok);
        dT2 = scratch(&o, sizeof(float) * n * l, &ok);
        dP = scratch(&o, sizeof(float) * nch * l * l, &ok);
        dM = scratch(&o, sizeof(float) * l * l, &ok);
        dO = scratch(&o, sizeof(float) * big * k, &ok);
    }
    ok = ok && spmm(&o, &dA, dX, dY, l) && orth(&o, &dY, &dT, m, l, dP, hpart, g, rinv, rinvf, dM);
    for (int it = 0; ok && it < q; ++it)
        ok = spmm(&o, &dAt, dY, dZ, l) && orth(&o, &dZ, &dT2, n, l, dP, hpart, g, rinv, rinvf, dM) &&
             spmm(&o, &dA, dZ, dY, l) && orth(&o, &dY, &dT, m, l, dP, hpart, g, rinv, rinvf, dM);
    /* Bt = A^T Q (n x l); B B^T = Bt^T Bt; Jacobi on the host */
    ok = ok && spmm(&o, &dAt, dY, dZ, l) && gram(&o, dZ, n, l, dP, hpart, g);
    if (ok) {
        xksm__jacobi_eig(g, l, w, ub);
        for (size_t i = 0; i < k; ++i)
            sigma[i] = (float)sqrt(w[i] > 0 ? w[i] : 0.0);
        for (size_t t = 0; t < l; ++t) /* U = Q U_B[:, :k] */
            for (size_t c = 0; c < k; ++c)
                mk[t * k + c] = (float)ub[t * l + c];
        ok = clEnqueueWriteBuffer(o.q, dM, CL_TRUE, 0, sizeof(float) * l * k, mk, 0, NULL, NULL) ==
                 CL_SUCCESS &&
             gemm(&o, dY, dM, dO, m, l, (cl_uint)k) &&
             clEnqueueReadBuffer(o.q, dO, CL_TRUE, 0, sizeof(float) * m * k, u, 0, NULL, NULL) ==
                 CL_SUCCESS;
    }
    if (ok && v) { /* V = Bt U_B[:, :k] / sigma */
        for (size_t t = 0; t < l; ++t)
            for (size_t c = 0; c < k; ++c)
                mk[t * k + c] = sigma[c] > 0 ? (float)(ub[t * l + c] / sigma[c]) : 0.0f;
        ok = clEnqueueWriteBuffer(o.q, dM, CL_TRUE, 0, sizeof(float) * l * k, mk, 0, NULL, NULL) ==
                 CL_SUCCESS &&
             gemm(&o, dZ, dM, dO, n, l, (cl_uint)k) &&
             clEnqueueReadBuffer(o.q, dO, CL_TRUE, 0, sizeof(float) * n * k, v, 0, NULL, NULL) ==
                 CL_SUCCESS;
    }
    cl_mem all[] = {dX, dY, dT, dZ, dT2, dP, dM, dO};
    for (size_t i = 0; i < sizeof all / sizeof *all; ++i)
        if (all[i])
            clReleaseMemObject(all[i]);
    dcsr_release(&dA);
    dcsr_release(&dAt);
    ocl_release(&o);
    free(om); free(hpart); free(rinvf); free(mk); free(g); free(rinv); free(w); free(ub);
    return ok;
}
