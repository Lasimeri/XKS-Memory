/* internal.h: shared private declarations of libxksm. Not installed. */
#ifndef XKSM_INTERNAL_H
#define XKSM_INTERNAL_H

#include "xksm.h"

#include <sqlite3.h>
#include <stdio.h>

#define XKSM_NSTMT 8

struct xksm {
    sqlite3 *db;
    int schema;
    char err[512];
    sqlite3_stmt *st[XKSM_NSTMT]; /* cached statements, memory/memory.c */
    struct xksm_cache *cache;     /* Tier-1 record cache, cache/cache.c */
    struct xksm_pool *pool;       /* worker pool, created on first parallel use */
    unsigned threads;             /* pool size, 1..XKSM_THREADS_MAX, default 8 */
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

/* Bring the open database to XKSM_SCHEMA_VERSION (storage/schema.c). */
xksm_status xksm__migrate(xksm *x);

#endif
