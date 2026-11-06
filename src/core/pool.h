/* pool.h: private persistent worker pool (POSIX threads). The calling thread is worker 0,
 * so a pool of n runs n - 1 extra threads; n = 1 spawns nothing. */
#ifndef XKSM_POOL_H
#define XKSM_POOL_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct xksm_pool xksm_pool;

/* One parallel step: every worker w in [0, n) calls task(arg, w, n) once. */
typedef void (*xksm_task)(void *arg, unsigned worker, unsigned nworkers);

xksm_pool *xksm__pool_create(unsigned n); /* NULL on failure */
void xksm__pool_run(xksm_pool *p, xksm_task task, void *arg); /* blocks until all finish */
unsigned xksm__pool_size(const xksm_pool *p);
void xksm__pool_destroy(xksm_pool *p);

#ifdef __cplusplus
}
#endif
#endif
