/* store.c: persistent vectors (memory_vec) and the RAM index the scan runs on. The index
 * is derived: loaded from SQLite on first search per model, dropped on any change. */
#include "internal.h"
#include "kernels.h"
#include "search.h"

#include <stdlib.h>
#include <string.h>

struct xksm_vindex {
    char *model;
    size_t dim, n;
    xksm_id *ids;
    int8_t *code;
    float *scale;
    float *vec;
};

void xksm__vindex_free(xksm *x)
{
    struct xksm_vindex *v = x->vindex;
    if (!v)
        return;
    free(v->model);
    free(v->ids);
    free(v->code);
    free(v->scale);
    free(v->vec);
    free(v);
    x->vindex = NULL;
}

static xksm_status model_dim(xksm *x, const char *model, size_t *dim)
{
    sqlite3_stmt *st = NULL;
    *dim = 0;
    if (sqlite3_prepare_v2(x->db, "SELECT dim FROM memory_vec WHERE model = ?1 LIMIT 1", -1,
                           &st, NULL) != SQLITE_OK ||
        sqlite3_bind_text(st, 1, model, -1, SQLITE_STATIC) != SQLITE_OK) {
        sqlite3_finalize(st);
        return xksm__fail(x, XKSM_EIO, "vector dim: %s", sqlite3_errmsg(x->db));
    }
    if (sqlite3_step(st) == SQLITE_ROW)
        *dim = (size_t)sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return XKSM_OK;
}

xksm_status xksm_vector_put(xksm *x, xksm_id memory, const char *model, const float *v,
                            size_t dim)
{
    if (!x || !model || !model[0] || !v || dim == 0 || dim > 65536)
        return x ? xksm__fail(x, XKSM_EINVAL, "vector put: bad argument") : XKSM_EINVAL;
    size_t have;
    xksm_status rc = model_dim(x, model, &have);
    if (rc != XKSM_OK)
        return rc;
    if (have && have != dim)
        return xksm__fail(x, XKSM_EINVAL, "model %s has dim %zu, got %zu", model, have, dim);
    float *u = malloc(dim * sizeof *u);
    int8_t *code = malloc(dim);
    if (!u || !code) {
        free(u);
        free(code);
        return XKSM_ENOMEM;
    }
    memcpy(u, v, dim * sizeof *u);
    xksm__normalize(u, dim);
    float scale = xksm__lattice_encode(u, dim, code);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(x->db,
                           "INSERT OR REPLACE INTO memory_vec(memory_id, model, dim, vec, code, "
                           "scale) VALUES (?1, ?2, ?3, ?4, ?5, ?6)",
                           -1, &st, NULL) != SQLITE_OK ||
        sqlite3_bind_int64(st, 1, memory) != SQLITE_OK ||
        sqlite3_bind_text(st, 2, model, -1, SQLITE_STATIC) != SQLITE_OK ||
        sqlite3_bind_int64(st, 3, (sqlite3_int64)dim) != SQLITE_OK ||
        sqlite3_bind_blob64(st, 4, u, dim * sizeof *u, SQLITE_STATIC) != SQLITE_OK ||
        sqlite3_bind_blob64(st, 5, code, dim, SQLITE_STATIC) != SQLITE_OK ||
        sqlite3_bind_double(st, 6, (double)scale) != SQLITE_OK) {
        rc = xksm__fail(x, XKSM_EIO, "vector put: %s", sqlite3_errmsg(x->db));
    } else {
        int s = sqlite3_step(st);
        if (s == SQLITE_CONSTRAINT || sqlite3_extended_errcode(x->db) == SQLITE_CONSTRAINT_FOREIGNKEY)
            rc = xksm__fail(x, XKSM_ENOTFOUND, "memory %lld not found", (long long)memory);
        else if (s != SQLITE_DONE)
            rc = xksm__fail(x, XKSM_EIO, "vector put: %s", sqlite3_errmsg(x->db));
    }
    sqlite3_finalize(st);
    free(u);
    free(code);
    if (rc == XKSM_OK)
        xksm__vindex_free(x); /* derived index is stale */
    return rc;
}

static xksm_status load(xksm *x, const char *model)
{
    if (x->vindex && !strcmp(x->vindex->model, model))
        return XKSM_OK;
    xksm__vindex_free(x);
    struct xksm_vindex *v = calloc(1, sizeof *v);
    if (!v)
        return XKSM_ENOMEM;
    v->model = malloc(strlen(model) + 1);
    if (!v->model) {
        free(v);
        return XKSM_ENOMEM;
    }
    strcpy(v->model, model);
    x->vindex = v;
    sqlite3_stmt *st = NULL;
    size_t cap = 0;
    xksm_status rc = model_dim(x, model, &v->dim);
    if (rc != XKSM_OK || v->dim == 0)
        return rc;
    if (sqlite3_prepare_v2(x->db,
                           "SELECT memory_id, vec, code, scale FROM memory_vec WHERE model = ?1 "
                           "ORDER BY memory_id",
                           -1, &st, NULL) != SQLITE_OK ||
        sqlite3_bind_text(st, 1, model, -1, SQLITE_STATIC) != SQLITE_OK) {
        rc = xksm__fail(x, XKSM_EIO, "vector load: %s", sqlite3_errmsg(x->db));
        goto out;
    }
    int s;
    while ((s = sqlite3_step(st)) == SQLITE_ROW) {
        if (v->n == cap) {
            size_t nc = cap ? 2 * cap : 1024;
            xksm_id *ids = realloc(v->ids, nc * sizeof *ids);
            if (ids)
                v->ids = ids;
            float *sc = realloc(v->scale, nc * sizeof *sc);
            if (sc)
                v->scale = sc;
            int8_t *cd = realloc(v->code, nc * v->dim);
            if (cd)
                v->code = cd;
            float *vv = realloc(v->vec, nc * v->dim * sizeof *vv);
            if (vv)
                v->vec = vv;
            if (!ids || !sc || !cd || !vv) {
                rc = XKSM_ENOMEM;
                goto out;
            }
            cap = nc;
        }
        if ((size_t)sqlite3_column_bytes(st, 1) != v->dim * sizeof(float) ||
            (size_t)sqlite3_column_bytes(st, 2) != v->dim)
            continue; /* CHECK constraints make this unreachable; never trust blobs blindly */
        v->ids[v->n] = sqlite3_column_int64(st, 0);
        memcpy(v->vec + v->n * v->dim, sqlite3_column_blob(st, 1), v->dim * sizeof(float));
        memcpy(v->code + v->n * v->dim, sqlite3_column_blob(st, 2), v->dim);
        v->scale[v->n] = (float)sqlite3_column_double(st, 3);
        v->n++;
    }
    if (s != SQLITE_DONE)
        rc = xksm__fail(x, XKSM_EIO, "vector load: %s", sqlite3_errmsg(x->db));
out:
    sqlite3_finalize(st);
    if (rc != XKSM_OK)
        xksm__vindex_free(x);
    return rc;
}

xksm_status xksm_vector_search(xksm *x, const char *model, const float *q, size_t dim,
                               const xksm_vector_opts *opts, xksm_search_result *out)
{
    if (!x || !model || !q || !out)
        return XKSM_EINVAL;
    memset(out, 0, sizeof *out);
    xksm_vector_opts o = {0};
    if (opts)
        o = *opts;
    size_t limit = o.limit ? o.limit : 10, R = o.rerank ? o.rerank : 100;
    if (limit > 10000)
        limit = 10000;
    if (R < limit)
        R = limit;
    xksm_status rc = load(x, model);
    if (rc != XKSM_OK)
        return rc;
    struct xksm_vindex *v = x->vindex;
    if (v->n == 0)
        return XKSM_OK; /* no vectors for this model */
    if (dim != v->dim)
        return xksm__fail(x, XKSM_EINVAL, "query dim %zu, model %s has %zu", dim, model, v->dim);

    float *qu = malloc(dim * sizeof *qu);
    int8_t *qc = malloc(dim);
    xksm_hit *cand = NULL, *exact = NULL;
    sqlite3_stmt *meta = NULL;
    out->hits = calloc(limit, sizeof *out->hits);
    if (!qu || !qc || !out->hits) {
        rc = XKSM_ENOMEM;
        goto done;
    }
    memcpy(qu, q, dim * sizeof *qu);
    xksm__normalize(qu, dim);
    (void)xksm__lattice_encode(qu, dim, qc);
    if (sqlite3_prepare_v2(x->db, "SELECT state, namespace, category FROM memory WHERE id = ?1",
                           -1, &meta, NULL) != SQLITE_OK) {
        rc = xksm__fail(x, XKSM_EIO, "vector search: %s", sqlite3_errmsg(x->db));
        goto done;
    }
    xksm_pool *pool = xksm__pool(x);
    for (;; R *= 4) {
        if (R > v->n)
            R = v->n;
        free(cand);
        free(exact);
        cand = malloc(R * sizeof *cand);
        exact = malloc(R * sizeof *exact);
        if (!cand || !exact) {
            rc = XKSM_ENOMEM;
            goto done;
        }
        size_t nc = xksm__lattice_topk(pool, v->code, v->scale, v->n, dim, qc, R, cand);
        size_t ne = xksm__f32_rerank(v->vec, dim, qu, cand, nc, nc, exact);
        out->count = 0;
        for (size_t i = 0; i < ne && out->count < limit; ++i) {
            xksm_id id = v->ids[exact[i].index];
            sqlite3_bind_int64(meta, 1, id);
            if (sqlite3_step(meta) == SQLITE_ROW) {
                const char *ns = (const char *)sqlite3_column_text(meta, 1);
                int cat = sqlite3_column_int(meta, 2);
                if (sqlite3_column_int(meta, 0) == XKSM_STATE_ACTIVE &&
                    (!o.ns || (ns && !strcmp(ns, o.ns))) &&
                    (!o.categories || (cat >= 0 && cat < 32 && (o.categories >> cat) & 1u)))
                    out->hits[out->count++] = (xksm_search_hit){id, exact[i].score, NULL};
            }
            sqlite3_reset(meta);
        }
        if (out->count == limit || R == v->n)
            break; /* enough, or every vector was considered */
    }
done:
    sqlite3_finalize(meta);
    free(qu);
    free(qc);
    free(cand);
    free(exact);
    if (rc != XKSM_OK)
        xksm_search_free(out);
    return rc;
}
