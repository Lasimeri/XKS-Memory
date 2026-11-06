/* bench_semantic.c: semantic model build time, CPU vs OpenCL GPU, interleaved repeats.
 * usage: bench_semantic STORE [repeats=3] */
#include "xksm.h"

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    if (argc < 2)
        return 2;
    int reps = argc > 2 ? atoi(argv[2]) : 3;
    xksm *x = NULL;
    if (xksm_open(argv[1], &x) != XKSM_OK)
        return 1;
    for (int r = 0; r < reps; ++r)
        for (int mode = 0; mode < 2; ++mode) {
            xksm_semantic_opts o = {0};
            o.gpu = mode ? 1 : -1;
            xksm_semantic_report rep;
            if (xksm_semantic_build(x, &o, &rep) != XKSM_OK) {
                fprintf(stderr, "%s build: %s\n", mode ? "gpu" : "cpu", xksm_errmsg(x));
                return 1;
            }
            printf("run %d %-3s svd %7.3f s  total %7.3f s  (vocab %zu, nnz %llu)%s%s\n", r + 1,
                   mode ? "gpu" : "cpu", rep.t_svd, rep.t_total, rep.vocab,
                   (unsigned long long)rep.nnz, rep.gpu_used ? "  on " : "",
                   rep.gpu_used ? rep.device : "");
        }
    xksm_close(x);
    return 0;
}
