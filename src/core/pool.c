/* pool.c: persistent worker pool (POSIX threads: ThreadSanitizer intercepts them; glibc C11
 * thrd_create/mtx_lock bypass its interceptors). Workers sleep on a generation counter; a dispatch is
 * one broadcast, completion is one counter signalled by the last finisher. */
#define _POSIX_C_SOURCE 200809L /* pthreads under strict -std=c17 */
#include "pool.h"

#include <stdint.h>
#include <stdlib.h>
#include <pthread.h>

struct worker {
    struct xksm_pool *pool;
    unsigned id;
};

struct xksm_pool {
    pthread_mutex_t m;
    pthread_cond_t go, done;
    pthread_t *thr;
    struct worker *w;
    unsigned n, started;
    uint64_t gen;
    unsigned pending;
    xksm_task task;
    void *arg;
    int quit;
};

static void *worker_main(void *p)
{
    struct worker *w = p;
    struct xksm_pool *pl = w->pool;
    uint64_t seen = 0;
    pthread_mutex_lock(&pl->m);
    for (;;) {
        while (pl->gen == seen && !pl->quit)
            pthread_cond_wait(&pl->go, &pl->m);
        if (pl->quit)
            break;
        seen = pl->gen;
        xksm_task t = pl->task;
        void *a = pl->arg;
        unsigned n = pl->n;
        pthread_mutex_unlock(&pl->m);
        t(a, w->id, n);
        pthread_mutex_lock(&pl->m);
        if (--pl->pending == 0)
            pthread_cond_signal(&pl->done);
    }
    pthread_mutex_unlock(&pl->m);
    return 0;
}

xksm_pool *xksm__pool_create(unsigned n)
{
    if (n == 0)
        return NULL;
    struct xksm_pool *p = calloc(1, sizeof *p);
    if (!p)
        return NULL;
    p->n = n;
    if (pthread_mutex_init(&p->m, NULL) != 0) {
        free(p);
        return NULL;
    }
    if (pthread_cond_init(&p->go, NULL) != 0 || pthread_cond_init(&p->done, NULL) != 0) {
        pthread_mutex_destroy(&p->m);
        free(p);
        return NULL;
    }
    if (n > 1) {
        p->thr = calloc(n - 1, sizeof *p->thr);
        p->w = calloc(n - 1, sizeof *p->w);
        if (!p->thr || !p->w) {
            xksm__pool_destroy(p);
            return NULL;
        }
        for (unsigned i = 0; i < n - 1; ++i) {
            p->w[i] = (struct worker){p, i + 1};
            if (pthread_create(&p->thr[i], NULL, worker_main, &p->w[i]) != 0) {
                xksm__pool_destroy(p);
                return NULL;
            }
            p->started++;
        }
    }
    return p;
}

void xksm__pool_run(xksm_pool *p, xksm_task task, void *arg)
{
    if (p->n == 1) {
        task(arg, 0, 1);
        return;
    }
    pthread_mutex_lock(&p->m);
    p->task = task;
    p->arg = arg;
    p->pending = p->n - 1;
    p->gen++;
    pthread_cond_broadcast(&p->go);
    pthread_mutex_unlock(&p->m);
    task(arg, 0, p->n); /* the caller is worker 0 */
    pthread_mutex_lock(&p->m);
    while (p->pending)
        pthread_cond_wait(&p->done, &p->m);
    pthread_mutex_unlock(&p->m);
}

unsigned xksm__pool_size(const xksm_pool *p)
{
    return p ? p->n : 0;
}

void xksm__pool_destroy(xksm_pool *p)
{
    if (!p)
        return;
    pthread_mutex_lock(&p->m);
    p->quit = 1;
    pthread_cond_broadcast(&p->go);
    pthread_mutex_unlock(&p->m);
    for (unsigned i = 0; i < p->started; ++i)
        pthread_join(p->thr[i], NULL);
    free(p->thr);
    free(p->w);
    pthread_cond_destroy(&p->go);
    pthread_cond_destroy(&p->done);
    pthread_mutex_destroy(&p->m);
    free(p);
}
