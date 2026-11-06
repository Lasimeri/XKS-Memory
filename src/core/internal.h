/* internal.h: shared private declarations of libxksm. Not installed. */
#ifndef XKSM_INTERNAL_H
#define XKSM_INTERNAL_H

#include "xksm.h"

#include <sqlite3.h>
#include <stdio.h>

#define XKSM_NSTMT 8

struct xksm {
    sqlite3 *db;
    sqlite3 *db_ro;               /* read-only second connection: lexical branch of recall */
    int schema;
    char err[512];
    sqlite3_stmt *st[XKSM_NSTMT]; /* cached statements, memory/memory.c */
    struct xksm_cache *cache;     /* Tier-1 record cache, cache/cache.c */
    struct xksm_pool *pool;       /* worker pool, created on first parallel use */
    unsigned threads;             /* recall pool size, 1..XKSM_THREADS_MAX, default 4 */
    struct xksm_pool *pool_embed; /* embedding pool (table build), created on first use */
    unsigned threads_embed;       /* its size, default 4 */
    struct xksm_vindex *vindex;   /* RAM vector index (derived), vector/store.c */
    struct xksm_semantic *sem;    /* loaded semantic model, semantic/semantic.c */
    struct xksm_port *port;       /* semantic port (static word vectors), semantic/port.c */
};

/* Record a formatted error on the handle and return status s. */
xksm_status xksm__fail(xksm *x, xksm_status s, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/* Current wall-clock time, unix microseconds UTC (C11 timespec_get). */
int64_t xksm__now_us(void);

/* 1 if s[0..n) is valid UTF-8 per RFC 3629 (memory/memory.c). */
int xksm__utf8_valid(const uint8_t *s, size_t n);

/* One memory inside an open transaction (memory/memory.c): XKSM_OK, XKSM_EXISTS,
 * XKSM_EINVAL (nothing written), or XKSM_ENOMEM / XKSM_EIO (caller rolls back). */
xksm_status xksm__memory_add_in_txn(xksm *x, const xksm_memory_in *in, xksm_id *id_out,
                                    int64_t now);

/* Finalize the cached statements (memory/memory.c); called by xksm_close. */
void xksm__memory_finalize(xksm *x);

/* Tier-1 record cache (cache/cache.c). Capacity = ceil(records / 4). */
xksm_status xksm__cache_init(xksm *x);
void xksm__cache_free(xksm *x);
void xksm__cache_records_delta(xksm *x, int64_t delta);
int xksm__cache_get(xksm *x, xksm_id id, xksm_memory *out, xksm_status *rc); /* 1 = hit */
void xksm__cache_put(xksm *x, const xksm_memory *m);
void xksm__cache_evict(xksm *x, xksm_id id);

/* The handle's worker pool, created with x->threads workers on first use; NULL if
 * creation failed (callers then run single-threaded). core/xksm.c */
struct xksm_pool *xksm__pool(xksm *x);

/* The embedding pool (x->threads_embed workers), same contract. core/xksm.c */
struct xksm_pool *xksm__pool_embed(xksm *x);

/* Drop the loaded semantic model (semantic/semantic.c); it reloads on next use. */
void xksm__semantic_free(xksm *x);

/* Drop the derived RAM vector index (vector/store.c); it reloads on next search. */
void xksm__vindex_free(xksm *x);

/* A destructive vector change (replace, delete, rebuild): bump the store's vector epoch so
 * the on-disk index snapshot is rewritten on next load; drops the RAM index. */
void xksm__vector_epoch_bump(xksm *x);

/* Recall with its two branches concurrent (retrieval/hybrid.c): the lexical list runs in a
 * thread on x->db_ro (a read-only connection), vec_side fills the vector list on this thread
 * (vector work on the recall pool); both are fused by RRF exactly as xksm_hybrid_search.
 * In-memory stores run the branches one after the other. */
typedef xksm_status (*xksm__vector_side)(xksm *x, void *ctx, const xksm_vector_opts *vo,
                                         xksm_search_result *out);
xksm_status xksm__hybrid_concurrent(xksm *x, const char *text, const xksm_hybrid_opts *opts,
                                    xksm__vector_side vec_side, void *ctx, xksm_hybrid_result *out);

/* Bring the open database to XKSM_SCHEMA_VERSION (storage/schema.c). */
xksm_status xksm__migrate(xksm *x);

#endif
