/* kv.c: store key-value state (table kv) and source lookups. */
#include "internal.h"

#include <string.h>

xksm_status xksm_kv_put(xksm *x, const char *key, const void *value, size_t len)
{
    if (!x || !key || (!value && len))
        return XKSM_EINVAL;
    sqlite3_stmt *st = NULL;
    int ok = sqlite3_prepare_v2(x->db, "INSERT OR REPLACE INTO kv(key, value) VALUES (?1, ?2)", -1,
                                &st, NULL) == SQLITE_OK &&
             sqlite3_bind_text(st, 1, key, -1, SQLITE_STATIC) == SQLITE_OK &&
             sqlite3_bind_blob64(st, 2, len ? value : "", len, SQLITE_STATIC) == SQLITE_OK &&
             sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok ? XKSM_OK : xksm__fail(x, XKSM_EIO, "kv put %s: %s", key, sqlite3_errmsg(x->db));
}

xksm_status xksm_kv_get(xksm *x, const char *key, void *value, size_t cap, size_t *len)
{
    if (!x || !key || (!value && cap))
        return XKSM_EINVAL;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(x->db, "SELECT value FROM kv WHERE key = ?1", -1, &st, NULL) != SQLITE_OK ||
        sqlite3_bind_text(st, 1, key, -1, SQLITE_STATIC) != SQLITE_OK) {
        sqlite3_finalize(st);
        return xksm__fail(x, XKSM_EIO, "kv get %s: %s", key, sqlite3_errmsg(x->db));
    }
    int step = sqlite3_step(st);
    xksm_status rc = XKSM_ENOTFOUND;
    if (step == SQLITE_ROW) {
        size_t n = (size_t)sqlite3_column_bytes(st, 0);
        const void *b = sqlite3_column_blob(st, 0);
        if (cap)
            memcpy(value, b ? b : "", n < cap ? n : cap);
        if (len)
            *len = n;
        rc = XKSM_OK;
    } else if (step != SQLITE_DONE) {
        rc = xksm__fail(x, XKSM_EIO, "kv get %s: %s", key, sqlite3_errmsg(x->db));
    }
    sqlite3_finalize(st);
    return rc;
}

xksm_status xksm_kv_add(xksm *x, const char *key, int64_t delta, int64_t *out)
{
    if (!x || !key)
        return XKSM_EINVAL;
    sqlite3_stmt *st = NULL;
    int ok = sqlite3_prepare_v2(x->db,
                                "INSERT INTO kv(key, value) VALUES (?1, ?2) ON CONFLICT(key) DO "
                                "UPDATE SET value = CAST(value AS INTEGER) + ?2 RETURNING value",
                                -1, &st, NULL) == SQLITE_OK &&
             sqlite3_bind_text(st, 1, key, -1, SQLITE_STATIC) == SQLITE_OK &&
             sqlite3_bind_int64(st, 2, delta) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW;
    if (ok && out)
        *out = sqlite3_column_int64(st, 0);
    ok = ok && sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok ? XKSM_OK : xksm__fail(x, XKSM_EIO, "kv add %s: %s", key, sqlite3_errmsg(x->db));
}

xksm_status xksm_kv_delete(xksm *x, const char *key)
{
    if (!x || !key)
        return XKSM_EINVAL;
    sqlite3_stmt *st = NULL;
    int ok = sqlite3_prepare_v2(x->db, "DELETE FROM kv WHERE key = ?1", -1, &st, NULL) == SQLITE_OK &&
             sqlite3_bind_text(st, 1, key, -1, SQLITE_STATIC) == SQLITE_OK &&
             sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok ? XKSM_OK : xksm__fail(x, XKSM_EIO, "kv delete %s: %s", key, sqlite3_errmsg(x->db));
}

xksm_status xksm_source_info(xksm *x, xksm_id source, char *uri, size_t cap, int64_t *imported_us)
{
    if (!x || (!uri && cap))
        return XKSM_EINVAL;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(x->db, "SELECT uri, imported_us FROM source WHERE id = ?1", -1, &st,
                           NULL) != SQLITE_OK ||
        sqlite3_bind_int64(st, 1, source) != SQLITE_OK) {
        sqlite3_finalize(st);
        return xksm__fail(x, XKSM_EIO, "source %lld: %s", (long long)source, sqlite3_errmsg(x->db));
    }
    xksm_status rc = XKSM_ENOTFOUND;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const char *u = (const char *)sqlite3_column_text(st, 0);
        if (cap) {
            size_t n = u ? strlen(u) : 0;
            if (n >= cap)
                n = cap - 1;
            memcpy(uri, u ? u : "", n);
            uri[n] = 0;
        }
        if (imported_us)
            *imported_us = sqlite3_column_int64(st, 1);
        rc = XKSM_OK;
    }
    sqlite3_finalize(st);
    return rc;
}
