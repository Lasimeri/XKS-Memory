/* xksm.c: handle lifecycle. */
#include "internal.h"
#include "pool.h"
#include "../semantic/port.h"

#include <stdarg.h>
#include <stdlib.h>
#include <time.h>

#define XKSM_STR2(x) #x
#define XKSM_STR(x) XKSM_STR2(x)

const char *xksm_version(void)
{
    return XKSM_STR(XKSM_VERSION_MAJOR) "." XKSM_STR(XKSM_VERSION_MINOR) "." XKSM_STR(
        XKSM_VERSION_PATCH);
}

xksm_status xksm__fail(xksm *x, xksm_status s, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(x->err, sizeof x->err, fmt, ap);
    va_end(ap);
    return s;
}

xksm_status xksm_open(const char *path, xksm **out)
{
    if (!out)
        return XKSM_EINVAL;
    *out = NULL;
    if (!path)
        return XKSM_EINVAL;

    xksm *x = calloc(1, sizeof *x);
    if (!x)
        return XKSM_ENOMEM;
    *out = x;
    x->threads = XKSM_THREADS_DEFAULT;
    x->threads_embed = XKSM_EMBED_THREADS_DEFAULT;
    { /* environment overrides, e.g. for the CLI and the agent hooks */
        const char *e = getenv("XKSM_THREADS");
        if (e && *e)
            xksm_set_threads(x, atoi(e));
        e = getenv("XKSM_EMBED_THREADS");
        if (e && *e)
            xksm_set_embed_threads(x, atoi(e));
    }

    int rc = sqlite3_open_v2(path, &x->db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL);
    if (rc != SQLITE_OK)
        return xksm__fail(x, XKSM_EIO, "open %s: %s", path,
                          x->db ? sqlite3_errmsg(x->db) : sqlite3_errstr(rc));

    char *err = NULL;
    if (sqlite3_exec(x->db,
                     "PRAGMA journal_mode = WAL;"
                     "PRAGMA synchronous = NORMAL;"
                     "PRAGMA foreign_keys = ON;"
                     "PRAGMA secure_delete = ON;"
                     "PRAGMA busy_timeout = 10000;",
                     NULL, NULL, &err) != SQLITE_OK) {
        xksm_status s = xksm__fail(x, XKSM_EIO, "pragmas: %s", err ? err : "unknown");
        sqlite3_free(err);
        return s;
    }
    xksm_status s = xksm__migrate(x);
    return s == XKSM_OK ? xksm__cache_init(x) : s;
}

void xksm_close(xksm *x)
{
    if (!x)
        return;
    xksm__semantic_free(x);
    xksm__port_close(x->port);
    xksm__vindex_free(x);
    xksm__pool_destroy(x->pool);
    xksm__pool_destroy(x->pool_embed);
    xksm__cache_free(x);
    xksm__memory_finalize(x);
    sqlite3_close(x->db_ro);
    sqlite3_close(x->db);
    free(x);
}

int xksm_schema_version(const xksm *x)
{
    return x ? x->schema : -1;
}

const char *xksm_errmsg(const xksm *x)
{
    return x ? x->err : "null handle";
}

int64_t xksm__now_us(void)
{
    struct timespec ts;
    if (timespec_get(&ts, TIME_UTC) != TIME_UTC)
        return 0;
    return (int64_t)ts.tv_sec * 1000000 + (int64_t)(ts.tv_nsec / 1000);
}

struct xksm_pool *xksm__pool(xksm *x)
{
    if (!x->pool && x->threads > 1)
        x->pool = xksm__pool_create(x->threads);
    return x->pool;
}

int xksm_set_threads(xksm *x, int threads)
{
    if (!x)
        return 0;
    unsigned t = threads < 1                  ? 1u
                 : threads > XKSM_THREADS_MAX ? (unsigned)XKSM_THREADS_MAX
                                              : (unsigned)threads;
    if (t != x->threads) {
        xksm__pool_destroy(x->pool); /* rebuilt at the new size on next use */
        x->pool = NULL;
        x->threads = t;
    }
    return (int)t;
}

int xksm_get_threads(const xksm *x)
{
    return x ? (int)x->threads : 0;
}

struct xksm_pool *xksm__pool_embed(xksm *x)
{
    if (!x->pool_embed && x->threads_embed > 1)
        x->pool_embed = xksm__pool_create(x->threads_embed);
    return x->pool_embed;
}

int xksm_set_embed_threads(xksm *x, int threads)
{
    if (!x)
        return 0;
    unsigned t = threads < 1                  ? 1u
                 : threads > XKSM_THREADS_MAX ? (unsigned)XKSM_THREADS_MAX
                                              : (unsigned)threads;
    if (t != x->threads_embed) {
        xksm__pool_destroy(x->pool_embed); /* rebuilt at the new size on next use */
        x->pool_embed = NULL;
        x->threads_embed = t;
    }
    return (int)t;
}

int xksm_get_embed_threads(const xksm *x)
{
    return x ? (int)x->threads_embed : 0;
}
