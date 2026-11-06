/* session.c: registry of agent sessions per harness, and forgetting a whole session. */
#include "internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

xksm_status xksm_session_touch(xksm *x, const char *harness, const char *id, const char *cwd,
                               int64_t event_us)
{
    if (!x || !harness || !*harness || !id || !*id)
        return XKSM_EINVAL;
    sqlite3_stmt *st = NULL;
    int ok = sqlite3_prepare_v2(x->db,
                                "INSERT INTO session(harness, id, first_us, last_us, turns, cwd) "
                                "VALUES (?1, ?2, ?3, ?3, 1, ?4) ON CONFLICT(harness, id) DO UPDATE "
                                "SET first_us = min(first_us, ?3), last_us = max(last_us, ?3), "
                                "turns = turns + 1, cwd = coalesce(?4, cwd)",
                                -1, &st, NULL) == SQLITE_OK &&
             sqlite3_bind_text(st, 1, harness, -1, SQLITE_STATIC) == SQLITE_OK &&
             sqlite3_bind_text(st, 2, id, -1, SQLITE_STATIC) == SQLITE_OK &&
             sqlite3_bind_int64(st, 3, event_us) == SQLITE_OK &&
             (cwd && *cwd ? sqlite3_bind_text(st, 4, cwd, -1, SQLITE_STATIC)
                          : sqlite3_bind_null(st, 4)) == SQLITE_OK &&
             sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok ? XKSM_OK : xksm__fail(x, XKSM_EIO, "session touch: %s", sqlite3_errmsg(x->db));
}

static char *dup_col(sqlite3_stmt *st, int c)
{
    const char *s = (const char *)sqlite3_column_text(st, c);
    if (!s)
        return NULL;
    size_t n = strlen(s);
    char *d = malloc(n + 1);
    if (d)
        memcpy(d, s, n + 1);
    return d;
}

void xksm_session_list_free(xksm_session_info *l, size_t n)
{
    for (size_t i = 0; l && i < n; ++i) {
        free(l[i].harness);
        free(l[i].id);
        free(l[i].cwd);
    }
    free(l);
}

xksm_status xksm_session_list(xksm *x, const char *harness, size_t limit, xksm_session_info **out,
                              size_t *count)
{
    if (!x || !out || !count)
        return XKSM_EINVAL;
    *out = NULL;
    *count = 0;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(x->db,
                           "SELECT harness, id, cwd, first_us, last_us, turns FROM session "
                           "WHERE ?1 IS NULL OR harness = ?1 ORDER BY last_us DESC, harness, id "
                           "LIMIT ?2",
                           -1, &st, NULL) != SQLITE_OK ||
        (harness ? sqlite3_bind_text(st, 1, harness, -1, SQLITE_STATIC) : sqlite3_bind_null(st, 1)) !=
            SQLITE_OK ||
        sqlite3_bind_int64(st, 2, limit ? (sqlite3_int64)limit : -1) != SQLITE_OK) {
        sqlite3_finalize(st);
        return xksm__fail(x, XKSM_EIO, "session list: %s", sqlite3_errmsg(x->db));
    }
    xksm_session_info *v = NULL;
    size_t n = 0, cap = 0;
    xksm_status rc = XKSM_OK;
    while (sqlite3_step(st) == SQLITE_ROW) {
        if (n == cap) {
            cap = cap ? 2 * cap : 32;
            xksm_session_info *nv = realloc(v, sizeof *nv * cap);
            if (!nv) {
                rc = XKSM_ENOMEM;
                break;
            }
            v = nv;
        }
        xksm_session_info *s = &v[n++];
        s->harness = dup_col(st, 0);
        s->id = dup_col(st, 1);
        s->cwd = dup_col(st, 2);
        s->first_us = sqlite3_column_int64(st, 3);
        s->last_us = sqlite3_column_int64(st, 4);
        s->turns = sqlite3_column_int64(st, 5);
        if (!s->harness || !s->id) {
            rc = XKSM_ENOMEM;
            break;
        }
    }
    sqlite3_finalize(st);
    if (rc != XKSM_OK) {
        xksm_session_list_free(v, n);
        return rc;
    }
    *out = v;
    *count = n;
    return XKSM_OK;
}

xksm_status xksm_session_forget(xksm *x, const char *harness, const char *id, size_t *deleted)
{
    if (deleted)
        *deleted = 0;
    if (!x || !harness || !*harness || !id || !*id)
        return XKSM_EINVAL;
    char prefix[1024];
    int pl = snprintf(prefix, sizeof prefix, "xksm-turn://%s/%s/", harness, id);
    if (pl < 0 || (size_t)pl >= sizeof prefix)
        return XKSM_EINVAL;
    /* exact prefix compare (no LIKE: ids may contain % or _) */
    sqlite3_stmt *st = NULL;
    xksm_id *ids = NULL;
    size_t n = 0, cap = 0;
    xksm_status rc = XKSM_OK;
    if (sqlite3_prepare_v2(x->db,
                           "SELECT m.id FROM memory m JOIN source s ON s.id = m.source_id "
                           "WHERE substr(s.uri, 1, ?2) = ?1",
                           -1, &st, NULL) != SQLITE_OK ||
        sqlite3_bind_text(st, 1, prefix, pl, SQLITE_STATIC) != SQLITE_OK ||
        sqlite3_bind_int(st, 2, pl) != SQLITE_OK) {
        sqlite3_finalize(st);
        return xksm__fail(x, XKSM_EIO, "session forget: %s", sqlite3_errmsg(x->db));
    }
    while (sqlite3_step(st) == SQLITE_ROW) {
        if (n == cap) {
            cap = cap ? 2 * cap : 256;
            xksm_id *ni = realloc(ids, sizeof *ni * cap);
            if (!ni) {
                rc = XKSM_ENOMEM;
                break;
            }
            ids = ni;
        }
        ids[n++] = sqlite3_column_int64(st, 0);
    }
    sqlite3_finalize(st);
    size_t done = 0;
    for (size_t i = 0; rc == XKSM_OK && i < n; ++i) {
        rc = xksm_memory_delete(x, ids[i]);
        if (rc == XKSM_ENOTFOUND)
            rc = XKSM_OK;
        else if (rc == XKSM_OK)
            ++done;
    }
    free(ids);
    if (rc == XKSM_OK) { /* the readable files of the session (mirror.c) */
        sqlite3_stmt *ss = NULL;
        xksm_id *src = NULL;
        size_t ns = 0, sc = 0;
        if (sqlite3_prepare_v2(x->db, "SELECT id FROM source WHERE substr(uri, 1, ?2) = ?1", -1, &ss, NULL) ==
                SQLITE_OK &&
            sqlite3_bind_text(ss, 1, prefix, pl, SQLITE_STATIC) == SQLITE_OK &&
            sqlite3_bind_int(ss, 2, pl) == SQLITE_OK)
            while (sqlite3_step(ss) == SQLITE_ROW) {
                if (ns == sc) {
                    sc = sc ? 2 * sc : 256;
                    xksm_id *nsrc = realloc(src, sizeof *nsrc * sc);
                    if (!nsrc)
                        break;
                    src = nsrc;
                }
                src[ns++] = sqlite3_column_int64(ss, 0);
            }
        sqlite3_finalize(ss);
        xksm__mirror_forget(x, harness, id, src, ns);
        free(src);
    }
    if (rc == XKSM_OK) {
        st = NULL;
        int ok = sqlite3_prepare_v2(x->db, "DELETE FROM source WHERE substr(uri, 1, ?2) = ?1", -1,
                                    &st, NULL) == SQLITE_OK &&
                 sqlite3_bind_text(st, 1, prefix, pl, SQLITE_STATIC) == SQLITE_OK &&
                 sqlite3_bind_int(st, 2, pl) == SQLITE_OK && sqlite3_step(st) == SQLITE_DONE;
        sqlite3_finalize(st);
        st = NULL;
        ok = ok && sqlite3_prepare_v2(x->db, "DELETE FROM session WHERE harness = ?1 AND id = ?2", -1,
                                      &st, NULL) == SQLITE_OK &&
             sqlite3_bind_text(st, 1, harness, -1, SQLITE_STATIC) == SQLITE_OK &&
             sqlite3_bind_text(st, 2, id, -1, SQLITE_STATIC) == SQLITE_OK &&
             sqlite3_step(st) == SQLITE_DONE;
        sqlite3_finalize(st);
        if (!ok)
            rc = xksm__fail(x, XKSM_EIO, "session forget: %s", sqlite3_errmsg(x->db));
    }
    if (deleted)
        *deleted = done;
    return rc;
}

xksm_status xksm_session_memories(xksm *x, const char *harness, const char *id, xksm_id **ids,
                                  size_t *count)
{
    if (!x || !harness || !id || !ids || !count)
        return XKSM_EINVAL;
    *ids = NULL;
    *count = 0;
    char prefix[1024];
    int pl = snprintf(prefix, sizeof prefix, "xksm-turn://%s/%s/", harness, id);
    if (pl < 0 || (size_t)pl >= sizeof prefix)
        return XKSM_EINVAL;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(x->db,
                           "SELECT m.id FROM memory m JOIN source s ON s.id = m.source_id "
                           "WHERE substr(s.uri, 1, ?2) = ?1 AND m.state = 1 "
                           "ORDER BY coalesce(m.event_us, m.created_us), m.id",
                           -1, &st, NULL) != SQLITE_OK ||
        sqlite3_bind_text(st, 1, prefix, pl, SQLITE_STATIC) != SQLITE_OK ||
        sqlite3_bind_int(st, 2, pl) != SQLITE_OK) {
        sqlite3_finalize(st);
        return xksm__fail(x, XKSM_EIO, "session memories: %s", sqlite3_errmsg(x->db));
    }
    xksm_id *v = NULL;
    size_t n = 0, cap = 0;
    xksm_status rc = XKSM_OK;
    while (sqlite3_step(st) == SQLITE_ROW) {
        if (n == cap) {
            cap = cap ? 2 * cap : 64;
            xksm_id *nv = realloc(v, sizeof *nv * cap);
            if (!nv) {
                rc = XKSM_ENOMEM;
                break;
            }
            v = nv;
        }
        v[n++] = sqlite3_column_int64(st, 0);
    }
    sqlite3_finalize(st);
    if (rc != XKSM_OK) {
        free(v);
        return rc;
    }
    *ids = v;
    *count = n;
    return XKSM_OK;
}

void xksm_ns_counts_free(xksm_ns_count *l, size_t n)
{
    for (size_t i = 0; l && i < n; ++i)
        free(l[i].ns);
    free(l);
}

xksm_status xksm_ns_counts(xksm *x, const char *model, xksm_ns_count **out, size_t *count)
{
    if (!x || !out || !count)
        return XKSM_EINVAL;
    *out = NULL;
    *count = 0;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(x->db,
                           "SELECT m.namespace, count(*), count(v.memory_id) FROM memory m "
                           "LEFT JOIN memory_vec v ON v.memory_id = m.id AND v.model = ?1 "
                           "WHERE m.state = 1 GROUP BY m.namespace ORDER BY m.namespace",
                           -1, &st, NULL) != SQLITE_OK ||
        sqlite3_bind_text(st, 1, model ? model : XKSM_SEMANTIC_MODEL, -1, SQLITE_STATIC) != SQLITE_OK) {
        sqlite3_finalize(st);
        return xksm__fail(x, XKSM_EIO, "namespace counts: %s", sqlite3_errmsg(x->db));
    }
    xksm_ns_count *v = NULL;
    size_t n = 0, cap = 0;
    xksm_status rc = XKSM_OK;
    while (sqlite3_step(st) == SQLITE_ROW) {
        if (n == cap) {
            cap = cap ? 2 * cap : 16;
            xksm_ns_count *nv = realloc(v, sizeof *nv * cap);
            if (!nv) {
                rc = XKSM_ENOMEM;
                break;
            }
            v = nv;
        }
        const char *ns = (const char *)sqlite3_column_text(st, 0);
        size_t l = ns ? strlen(ns) : 0;
        v[n].ns = malloc(l + 1);
        if (!v[n].ns) {
            rc = XKSM_ENOMEM;
            break;
        }
        memcpy(v[n].ns, ns ? ns : "", l + 1);
        v[n].memories = sqlite3_column_int64(st, 1);
        v[n].embedded = sqlite3_column_int64(st, 2);
        ++n;
    }
    sqlite3_finalize(st);
    if (rc != XKSM_OK) {
        xksm_ns_counts_free(v, n);
        return rc;
    }
    *out = v;
    *count = n;
    return XKSM_OK;
}

xksm_status xksm_memory_ids(xksm *x, const char *ns, size_t offset, size_t limit, xksm_id **ids,
                            size_t *count)
{
    if (!x || !ids || !count)
        return XKSM_EINVAL;
    *ids = NULL;
    *count = 0;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(x->db,
                           "SELECT id FROM memory WHERE state = 1 AND (?1 IS NULL OR namespace = ?1) "
                           "ORDER BY coalesce(event_us, created_us) DESC, id DESC LIMIT ?2 OFFSET ?3",
                           -1, &st, NULL) != SQLITE_OK ||
        (ns ? sqlite3_bind_text(st, 1, ns, -1, SQLITE_STATIC) : sqlite3_bind_null(st, 1)) != SQLITE_OK ||
        sqlite3_bind_int64(st, 2, limit ? (sqlite3_int64)limit : -1) != SQLITE_OK ||
        sqlite3_bind_int64(st, 3, (sqlite3_int64)offset) != SQLITE_OK) {
        sqlite3_finalize(st);
        return xksm__fail(x, XKSM_EIO, "memory list: %s", sqlite3_errmsg(x->db));
    }
    xksm_id *v = NULL;
    size_t n = 0, cap = 0;
    xksm_status rc = XKSM_OK;
    while (sqlite3_step(st) == SQLITE_ROW) {
        if (n == cap) {
            cap = cap ? 2 * cap : 256;
            xksm_id *nv = realloc(v, sizeof *nv * cap);
            if (!nv) {
                rc = XKSM_ENOMEM;
                break;
            }
            v = nv;
        }
        v[n++] = sqlite3_column_int64(st, 0);
    }
    sqlite3_finalize(st);
    if (rc != XKSM_OK) {
        free(v);
        return rc;
    }
    *ids = v;
    *count = n;
    return XKSM_OK;
}

int64_t xksm_memory_count(xksm *x, const char *ns)
{
    if (!x)
        return -1;
    sqlite3_stmt *st = NULL;
    int64_t n = -1;
    if (sqlite3_prepare_v2(x->db,
                           "SELECT count(*) FROM memory WHERE state = 1 AND (?1 IS NULL OR namespace = ?1)",
                           -1, &st, NULL) == SQLITE_OK &&
        (ns ? sqlite3_bind_text(st, 1, ns, -1, SQLITE_STATIC) : sqlite3_bind_null(st, 1)) == SQLITE_OK &&
        sqlite3_step(st) == SQLITE_ROW)
        n = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return n;
}
