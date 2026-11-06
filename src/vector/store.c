/* store.c: persistent vectors (memory_vec) and the index the scan runs on.
 * The index is derived data in two segments:
 *   0: a memory-mapped snapshot file next to the store (<store>.vidx-<model hash>), written
 *      after a full load, valid while the store's vector epoch is unchanged;
 *   1: a RAM tail of the rows appended after the snapshot (memory_id > snapshot max id),
 *      read from SQLite on load: the per-turn case, a handful of rows.
 * Destructive changes (replacing a vector, inserting below the current max id, deleting a
 * memory, rebuilding a model) bump the epoch (kv key vector_epoch), so the next load
 * rewrites the snapshot from SQLite and a deleted memory's vector does not survive there.
 * Hits are always re-checked against the memory table (state, namespace, category). */
#define _POSIX_C_SOURCE 200809L
#include "internal.h"
#include "kernels.h"
#include "search.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct vseg {
    size_t n;
    xksm_id *ids;
    int8_t *code;
    float *scale;
    float *vec;
    int owned; /* 1 = malloc'd arrays, 0 = pointers into the mapping */
} vseg;

struct xksm_vindex {
    char *model;
    size_t dim;
    vseg seg[2];
    void *map;
    size_t map_len;
};

enum { SNAP_HEADER = 64 };

static void seg_free(vseg *s)
{
    if (s->owned) {
        free(s->ids);
        free(s->code);
        free(s->scale);
        free(s->vec);
    }
    memset(s, 0, sizeof *s);
}

void xksm__vindex_free(xksm *x)
{
    struct xksm_vindex *v = x->vindex;
    if (!v)
        return;
    free(v->model);
    seg_free(&v->seg[0]);
    seg_free(&v->seg[1]);
    if (v->map)
        munmap(v->map, v->map_len);
    free(v);
    x->vindex = NULL;
}

/* remove every index snapshot of this store (any model): stale or holding deleted data */
static void snap_unlink_all(xksm *x)
{
    const char *f = sqlite3_db_filename(x->db, "main");
    if (!f || !*f)
        return;
    char dir[4096], pre[1024];
    const char *slash = strrchr(f, (int)0x2F);
    if (slash) {
        snprintf(dir, sizeof dir, "%.*s", (int)(slash - f), f);
        snprintf(pre, sizeof pre, "%s.vidx-", slash + 1);
    } else {
        snprintf(dir, sizeof dir, ".");
        snprintf(pre, sizeof pre, "%s.vidx-", f);
    }
    DIR *d = opendir(dir[0] ? dir : "/");
    struct dirent *e;
    size_t pl = strlen(pre);
    char tg[1100]; /* the port-target snapshot of semantic.c */
    snprintf(tg, sizeof tg, "%.*s.ptgt", (int)(pl - 6), pre);
    while (d && (e = readdir(d)))
        if (!strncmp(e->d_name, pre, pl) || !strcmp(e->d_name, tg)) {
            char p[8192];
            snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
            unlink(p);
        }
    if (d)
        closedir(d);
}

void xksm__vector_epoch_bump(xksm *x)
{
    (void)xksm_kv_add(x, "vector_epoch", 1, NULL);
    snap_unlink_all(x);
    xksm__vindex_free(x);
}

static int64_t epoch_now(xksm *x)
{
    int64_t e = 0;
    return xksm_kv_add(x, "vector_epoch", 0, &e) == XKSM_OK ? e : -1;
}

static uint64_t fnv64(const char *s)
{
    uint64_t h = 1469598103934665603ull;
    for (; *s; ++s)
        h = (h ^ (uint8_t)*s) * 1099511628211ull;
    return h;
}

/* snapshot path for a model; 0 if the store has no file (in-memory or temporary) */
static int snap_path(xksm *x, const char *model, char *out, size_t cap)
{
    const char *f = sqlite3_db_filename(x->db, "main");
    if (!f || !*f || getenv("XKSM_NO_VIDX"))
        return 0;
    return snprintf(out, cap, "%s.vidx-%016llx", f, (unsigned long long)fnv64(model)) < (int)cap;
}

static size_t up64(size_t n) { return (n + 63) / 64 * 64; }

/* layout: header | ids n*8 | scale n*4 (pad 64) | code n*dim (pad 64) | vec n*dim*4 */
static size_t snap_size(size_t n, size_t dim, size_t off[4])
{
    off[0] = SNAP_HEADER;
    off[1] = up64(off[0] + n * sizeof(xksm_id));
    off[2] = up64(off[1] + n * sizeof(float));
    off[3] = up64(off[2] + n * dim);
    return off[3] + n * dim * sizeof(float);
}

static void snap_write(xksm *x, const struct xksm_vindex *v, int64_t epoch)
{
    char path[4096], tmp[4200];
    const vseg *s = &v->seg[0];
    if (!snap_path(x, v->model, path, sizeof path) || epoch < 0 || !s->n)
        return;
    snprintf(tmp, sizeof tmp, "%s.tmp%ld", path, (long)getpid());
    FILE *f = fopen(tmp, "wb");
    if (!f)
        return;
    size_t off[4], total = snap_size(s->n, v->dim, off);
    uint8_t h[SNAP_HEADER] = {0};
    uint32_t dim32 = (uint32_t)v->dim;
    uint64_t n64 = s->n, mh = fnv64(v->model);
    int64_t maxid = s->ids[s->n - 1];
    memcpy(h, "XKSMVIX1", 8);
    memcpy(h + 8, &dim32, 4);
    memcpy(h + 16, &n64, 8);
    memcpy(h + 24, &epoch, 8);
    memcpy(h + 32, &maxid, 8);
    memcpy(h + 40, &mh, 8);
    /* each section at its offset; the gaps are zero padding */
    int ok = fwrite(h, 1, SNAP_HEADER, f) == SNAP_HEADER &&
             fwrite(s->ids, sizeof *s->ids, s->n, f) == s->n &&
             fseeko(f, (off_t)off[1], SEEK_SET) == 0 &&
             fwrite(s->scale, sizeof *s->scale, s->n, f) == s->n &&
             fseeko(f, (off_t)off[2], SEEK_SET) == 0 &&
             fwrite(s->code, 1, s->n * v->dim, f) == s->n * v->dim &&
             fseeko(f, (off_t)off[3], SEEK_SET) == 0 &&
             fwrite(s->vec, sizeof(float) * v->dim, s->n, f) == s->n;
    ok = ok && ftello(f) == (off_t)total; /* the file is exactly the layout size */
    ok = fclose(f) == 0 && ok;
    if (ok)
        ok = rename(tmp, path) == 0;
    if (!ok)
        unlink(tmp);
}

/* map a snapshot valid for epoch; 1 on success (segment 0 points into the mapping) */
static int snap_map(xksm *x, struct xksm_vindex *v, int64_t epoch, int64_t *maxid)
{
    char path[4096];
    if (!snap_path(x, v->model, path, sizeof path))
        return 0;
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    struct stat st;
    void *m = MAP_FAILED;
    if (fstat(fd, &st) == 0 && st.st_size >= SNAP_HEADER)
        m = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (m == MAP_FAILED)
        return 0;
    const uint8_t *h = m;
    uint32_t dim32;
    uint64_t n64, mh;
    int64_t ep, mx;
    memcpy(&dim32, h + 8, 4);
    memcpy(&n64, h + 16, 8);
    memcpy(&ep, h + 24, 8);
    memcpy(&mx, h + 32, 8);
    memcpy(&mh, h + 40, 8);
    size_t off[4];
    if (memcmp(h, "XKSMVIX1", 8) || dim32 != v->dim || ep != epoch || mh != fnv64(v->model) ||
        n64 == 0 || n64 > ((uint64_t)1 << 40) ||
        snap_size((size_t)n64, v->dim, off) != (size_t)st.st_size) {
        munmap(m, (size_t)st.st_size);
        return 0;
    }
    vseg *s = &v->seg[0];
    s->n = (size_t)n64;
    s->ids = (xksm_id *)(void *)((uint8_t *)m + off[0]);
    s->scale = (float *)(void *)((uint8_t *)m + off[1]);
    s->code = (int8_t *)(void *)((uint8_t *)m + off[2]);
    s->vec = (float *)(void *)((uint8_t *)m + off[3]);
    s->owned = 0;
    v->map = m;
    v->map_len = (size_t)st.st_size;
    *maxid = mx;
    return 1;
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
    /* destructive for a snapshot: replacing a row, or inserting below the current max id */
    int destructive = 0;
    sqlite3_stmt *q = NULL;
    if (sqlite3_prepare_v2(x->db,
                           "SELECT EXISTS (SELECT 1 FROM memory_vec WHERE memory_id = ?1 AND "
                           "model = ?2), EXISTS (SELECT 1 FROM memory_vec WHERE memory_id > ?1 "
                           "AND model = ?2)",
                           -1, &q, NULL) == SQLITE_OK &&
        sqlite3_bind_int64(q, 1, memory) == SQLITE_OK &&
        sqlite3_bind_text(q, 2, model, -1, SQLITE_STATIC) == SQLITE_OK &&
        sqlite3_step(q) == SQLITE_ROW)
        destructive = sqlite3_column_int(q, 0) || sqlite3_column_int(q, 1);
    sqlite3_finalize(q);
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
    if (rc == XKSM_OK) {
        if (destructive)
            xksm__vector_epoch_bump(x);
        else
            xksm__vindex_free(x); /* the tail grew: reload maps the snapshot again */
    }
    return rc;
}

/* rows of a model with memory_id > after, into an owned segment */
static xksm_status load_rows(xksm *x, const char *model, size_t dim, int64_t after, vseg *s)
{
    sqlite3_stmt *st = NULL;
    size_t cap = 0;
    xksm_status rc = XKSM_OK;
    s->owned = 1;
    if (sqlite3_prepare_v2(x->db,
                           "SELECT memory_id, vec, code, scale FROM memory_vec WHERE model = ?1 "
                           "AND memory_id > ?2 ORDER BY memory_id",
                           -1, &st, NULL) != SQLITE_OK ||
        sqlite3_bind_text(st, 1, model, -1, SQLITE_STATIC) != SQLITE_OK ||
        sqlite3_bind_int64(st, 2, after) != SQLITE_OK) {
        sqlite3_finalize(st);
        return xksm__fail(x, XKSM_EIO, "vector load: %s", sqlite3_errmsg(x->db));
    }
    int r;
    while ((r = sqlite3_step(st)) == SQLITE_ROW) {
        if (s->n == cap) {
            size_t nc = cap ? 2 * cap : 1024;
            xksm_id *ids = realloc(s->ids, nc * sizeof *ids);
            if (ids)
                s->ids = ids;
            float *sc = realloc(s->scale, nc * sizeof *sc);
            if (sc)
                s->scale = sc;
            int8_t *cd = realloc(s->code, nc * dim);
            if (cd)
                s->code = cd;
            float *vv = realloc(s->vec, nc * dim * sizeof *vv);
            if (vv)
                s->vec = vv;
            if (!ids || !sc || !cd || !vv) {
                rc = XKSM_ENOMEM;
                break;
            }
            cap = nc;
        }
        if ((size_t)sqlite3_column_bytes(st, 1) != dim * sizeof(float) ||
            (size_t)sqlite3_column_bytes(st, 2) != dim)
            continue; /* CHECK constraints make this unreachable; never trust blobs blindly */
        s->ids[s->n] = sqlite3_column_int64(st, 0);
        memcpy(s->vec + s->n * dim, sqlite3_column_blob(st, 1), dim * sizeof(float));
        memcpy(s->code + s->n * dim, sqlite3_column_blob(st, 2), dim);
        s->scale[s->n] = (float)sqlite3_column_double(st, 3);
        s->n++;
    }
    if (rc == XKSM_OK && r != SQLITE_DONE)
        rc = xksm__fail(x, XKSM_EIO, "vector load: %s", sqlite3_errmsg(x->db));
    sqlite3_finalize(st);
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
    xksm_status rc = model_dim(x, model, &v->dim);
    if (rc != XKSM_OK || v->dim == 0)
        return rc;
    int64_t epoch = epoch_now(x), maxid = 0;
    if (epoch >= 0 && snap_map(x, v, epoch, &maxid)) {
        rc = load_rows(x, model, v->dim, maxid, &v->seg[1]);
        /* a long tail: fold everything into a new snapshot */
        if (rc == XKSM_OK && v->seg[1].n > 1024 && v->seg[1].n * 8 > v->seg[0].n) {
            seg_free(&v->seg[0]);
            seg_free(&v->seg[1]);
            munmap(v->map, v->map_len);
            v->map = NULL;
            rc = load_rows(x, model, v->dim, INT64_MIN, &v->seg[0]);
            if (rc == XKSM_OK)
                snap_write(x, v, epoch);
        }
    } else {
        rc = load_rows(x, model, v->dim, INT64_MIN, &v->seg[0]);
        if (rc == XKSM_OK)
            snap_write(x, v, epoch);
    }
    if (rc != XKSM_OK)
        xksm__vindex_free(x);
    return rc;
}

typedef struct idhit {
    xksm_id id;
    float score;
} idhit;

static int by_score(const void *a, const void *b)
{
    const idhit *p = a, *q = b;
    if (p->score != q->score)
        return p->score < q->score ? 1 : -1;
    return p->id < q->id ? -1 : p->id > q->id;
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
    size_t total = v->seg[0].n + v->seg[1].n;
    if (total == 0)
        return XKSM_OK; /* no vectors for this model */
    if (dim != v->dim)
        return xksm__fail(x, XKSM_EINVAL, "query dim %zu, model %s has %zu", dim, model, v->dim);

    float *qu = malloc(dim * sizeof *qu);
    int8_t *qc = malloc(dim);
    xksm_hit *cand = NULL, *exact = NULL;
    idhit *merged = NULL;
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
        if (R > total)
            R = total;
        free(cand);
        free(exact);
        free(merged);
        cand = malloc(R * sizeof *cand);
        exact = malloc(R * sizeof *exact);
        merged = malloc(2 * R * sizeof *merged);
        if (!cand || !exact || !merged) {
            rc = XKSM_ENOMEM;
            goto done;
        }
        size_t nm = 0;
        for (int g = 0; g < 2; ++g) { /* each segment: lattice candidates, exact rerank */
            const vseg *s = &v->seg[g];
            if (!s->n)
                continue;
            size_t r = R < s->n ? R : s->n;
            size_t nc = xksm__lattice_topk(pool, s->code, s->scale, s->n, dim, qc, r, cand);
            size_t ne = xksm__f32_rerank(s->vec, dim, qu, cand, nc, nc, exact);
            for (size_t i = 0; i < ne; ++i)
                merged[nm++] = (idhit){s->ids[exact[i].index], exact[i].score};
        }
        qsort(merged, nm, sizeof *merged, by_score);
        out->count = 0;
        for (size_t i = 0; i < nm && out->count < limit; ++i) {
            sqlite3_bind_int64(meta, 1, merged[i].id);
            if (sqlite3_step(meta) == SQLITE_ROW) {
                const char *ns = (const char *)sqlite3_column_text(meta, 1);
                int cat = sqlite3_column_int(meta, 2);
                if (sqlite3_column_int(meta, 0) == XKSM_STATE_ACTIVE &&
                    (!o.ns || (ns && !strcmp(ns, o.ns))) &&
                    (!o.categories || (cat >= 0 && cat < 32 && (o.categories >> cat) & 1u)))
                    out->hits[out->count++] = (xksm_search_hit){merged[i].id, merged[i].score, NULL};
            }
            sqlite3_reset(meta);
        }
        if (out->count == limit || R == total)
            break; /* enough, or every vector was considered */
    }
done:
    sqlite3_finalize(meta);
    free(qu);
    free(qc);
    free(cand);
    free(exact);
    free(merged);
    if (rc != XKSM_OK)
        xksm_search_free(out);
    return rc;
}
