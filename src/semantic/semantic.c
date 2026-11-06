/* semantic.c: count-based semantic model built from the store's own memories.
 *   tokens -> vocabulary (min count) -> harmonic-window co-occurrence with deterministic
 *   subsampling weights -> PPMI with context distribution smoothing -> randomized SVD ->
 *   word vectors (U + V) Sigma^0.5, row-normalized -> SIF memory vectors minus the first
 *   principal component -> memory_vec under XKSM_SEMANTIC_MODEL.
 * The model (vocabulary, vectors, parameters, component) is persisted for query embedding. */
#define _POSIX_C_SOURCE 200809L /* clock_gettime under strict -std=c17 */
#include "internal.h"

#include "../vector/kernels.h"
#include "rsvd.h"
#include "sparse.h"
#include "port.h"
#include "../vector/backend.h"
#ifdef XKSM_HAVE_OPENCL
#include "../../backends/opencl/ocl_rsvd.h"
#endif

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAXTOK 64

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

/* ---- vocabulary: open addressing over an arena of NUL-terminated words ---------- */
typedef struct vocab {
    char *arena;
    size_t used, cap;
    uint64_t *off;     /* word i starts at arena + off[i] */
    uint64_t *count;
    uint32_t n, ncap;
    uint32_t *slot;    /* table of word index + 1; 0 = empty */
    uint32_t mask;
} vocab;

static uint64_t fnv(const char *s, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i)
        h = (h ^ (uint8_t)s[i]) * 1099511628211ull;
    return h;
}

static int vocab_grow_table(vocab *v)
{
    uint32_t nm = v->mask ? 2 * (v->mask + 1) - 1 : 1023;
    uint32_t *ns = calloc((size_t)nm + 1, sizeof *ns);
    if (!ns)
        return 0;
    for (uint32_t i = 0; i < v->n; ++i) {
        const char *w = v->arena + v->off[i];
        uint32_t h = (uint32_t)fnv(w, strlen(w)) & nm;
        while (ns[h])
            h = (h + 1) & nm;
        ns[h] = i + 1;
    }
    free(v->slot);
    v->slot = ns;
    v->mask = nm;
    return 1;
}

/* index of word, adding it if add; UINT32_MAX if absent (or on allocation failure) */
static uint32_t vocab_find(vocab *v, const char *w, size_t n, int add)
{
    if (!v->slot && !vocab_grow_table(v))
        return UINT32_MAX;
    uint32_t h = (uint32_t)fnv(w, n) & v->mask;
    for (; v->slot[h]; h = (h + 1) & v->mask) {
        const char *x = v->arena + v->off[v->slot[h] - 1];
        if (!strncmp(x, w, n) && x[n] == 0)
            return v->slot[h] - 1;
    }
    if (!add)
        return UINT32_MAX;
    if (v->n == v->ncap) {
        uint32_t nc = v->ncap ? 2 * v->ncap : 4096;
        uint64_t *o = realloc(v->off, nc * sizeof *o), *c = o ? realloc(v->count, nc * sizeof *c) : NULL;
        if (!o || !c)
            return UINT32_MAX;
        v->off = o;
        v->count = c;
        v->ncap = nc;
    }
    if (v->used + n + 1 > v->cap) {
        size_t nc = v->cap ? 2 * v->cap : 1 << 16;
        while (nc < v->used + n + 1)
            nc *= 2;
        char *a = realloc(v->arena, nc);
        if (!a)
            return UINT32_MAX;
        v->arena = a;
        v->cap = nc;
    }
    memcpy(v->arena + v->used, w, n);
    v->arena[v->used + n] = 0;
    v->off[v->n] = v->used;
    v->count[v->n] = 0;
    v->used += n + 1;
    v->slot[h] = ++v->n;
    if ((uint64_t)v->n * 2 > (uint64_t)v->mask + 1 && !vocab_grow_table(v))
        return UINT32_MAX;
    return v->n - 1;
}

static void vocab_free(vocab *v)
{
    free(v->arena);
    free(v->off);
    free(v->count);
    free(v->slot);
    memset(v, 0, sizeof *v);
}

/* next token: ASCII letters / digits / '_' (lowercased) and non-ASCII bytes */
static int tok_char(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '_' || c >= 0x80;
}

static size_t next_token(const char *s, size_t n, size_t *pos, char *out)
{
    size_t i = *pos;
    while (i < n && !tok_char((unsigned char)s[i]))
        ++i;
    size_t len = 0;
    while (i < n && tok_char((unsigned char)s[i])) {
        char c = s[i++];
        if (len < MAXTOK - 1)
            out[len++] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    *pos = i;
    return len;
}

/* ---- co-occurrence accumulator: open addressing, key = (i << 32 | j) ---------------- */
typedef struct pairs {
    uint64_t *key; /* UINT64_MAX = empty */
    float *val;
    uint64_t n, mask;
} pairs;

static int pairs_grow(pairs *p)
{
    uint64_t nm = p->mask ? 2 * (p->mask + 1) - 1 : (1u << 20) - 1;
    uint64_t *nk = malloc(sizeof *nk * (nm + 1));
    float *nv = malloc(sizeof *nv * (nm + 1));
    if (!nk || !nv) {
        free(nk);
        free(nv);
        return 0;
    }
    memset(nk, 0xff, sizeof *nk * (nm + 1));
    for (uint64_t i = 0; p->key && i <= p->mask; ++i)
        if (p->key[i] != UINT64_MAX) {
            uint64_t h = (p->key[i] * 0x9E3779B97F4A7C15ull) >> 20 & nm;
            while (nk[h] != UINT64_MAX)
                h = (h + 1) & nm;
            nk[h] = p->key[i];
            nv[h] = p->val[i];
        }
    free(p->key);
    free(p->val);
    p->key = nk;
    p->val = nv;
    p->mask = nm;
    return 1;
}

static int pairs_add(pairs *p, uint32_t i, uint32_t j, float w)
{
    if ((p->n + 1) * 2 > p->mask + 1 && !pairs_grow(p))
        return 0;
    uint64_t k = (uint64_t)i << 32 | j;
    uint64_t h = (k * 0x9E3779B97F4A7C15ull) >> 20 & p->mask;
    while (p->key[h] != UINT64_MAX && p->key[h] != k)
        h = (h + 1) & p->mask;
    if (p->key[h] == UINT64_MAX) {
        p->key[h] = k;
        p->val[h] = 0;
        p->n++;
    }
    p->val[h] += w;
    return 1;
}

/* ---- the loaded model (for query embedding) ------------------------------------------ */
struct xksm_semantic {
    vocab v;
    size_t dim;
    float *vec;    /* n x dim, row-normalized word vectors */
    float *sif;    /* n: a / (a + p(w)) */
    float *pc;     /* dim: removed common component */
    /* port targets per bucket: store words that have a port vector (built on first use) */
    float *tmat[16];
    uint32_t *tid[16];
    size_t tn[16];
    int targets;
};

void xksm__semantic_free(xksm *x)
{
    struct xksm_semantic *s = x->sem;
    if (!s)
        return;
    vocab_free(&s->v);
    free(s->vec);
    free(s->sif);
    free(s->pc);
    for (int b = 0; b < 16; ++b) {
        free(s->tmat[b]);
        free(s->tid[b]);
    }
    free(s);
    x->sem = NULL;
}

/* SIF average of the known words of text, minus the component; returns known-token count */
static size_t embed_text(const struct xksm_semantic *s, vocab *v, const char *text, size_t n,
                         float *out)
{
    char tok[MAXTOK];
    size_t pos = 0, known = 0, len;
    memset(out, 0, sizeof(float) * s->dim);
    while ((len = next_token(text, n, &pos, tok)) > 0) {
        uint32_t w = vocab_find(v, tok, len, 0);
        if (w == UINT32_MAX)
            continue;
        const float *wv = s->vec + (size_t)w * s->dim;
        for (size_t d = 0; d < s->dim; ++d)
            out[d] += s->sif[w] * wv[d];
        ++known;
    }
    if (!known)
        return 0;
    double dot = 0;
    for (size_t d = 0; d < s->dim; ++d) {
        out[d] /= (float)known;
        dot += (double)out[d] * s->pc[d];
    }
    for (size_t d = 0; d < s->dim; ++d)
        out[d] -= (float)dot * s->pc[d];
    return known;
}

static int meta_put(sqlite3 *db, const char *key, const void *v, size_t n)
{
    sqlite3_stmt *st = NULL;
    int ok = sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO semantic_meta(key, value) VALUES (?1, ?2)",
                                -1, &st, NULL) == SQLITE_OK &&
             sqlite3_bind_text(st, 1, key, -1, SQLITE_STATIC) == SQLITE_OK &&
             sqlite3_bind_blob64(st, 2, v, n, SQLITE_STATIC) == SQLITE_OK &&
             sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

xksm_status xksm_semantic_build(xksm *x, const xksm_semantic_opts *opts,
                                xksm_semantic_report *rep)
{
    if (!x)
        return XKSM_EINVAL;
    xksm_semantic_opts o = {0};
    if (opts)
        o = *opts;
    size_t dim = o.dim ? o.dim : 128, win = o.window ? o.window : 5;
    uint32_t minc = o.min_count ? o.min_count : 2;
    double cds = o.cds > 0 ? o.cds : 0.75, shift = o.shift > 1 ? log(o.shift) : 0.0;
    double sub_t = o.sub_t > 0 ? o.sub_t : 1e-4, sif_a = o.sif_a > 0 ? o.sif_a : 1e-3;
    int q = o.power_iters > 0 ? o.power_iters : 4;
    xksm_semantic_report r;
    memset(&r, 0, sizeof r);
    double t0 = now_s(), t = t0;

    /* pass 1: tokenize every active memory, build the full vocabulary */
    vocab full = {0};
    uint32_t *toks = NULL;
    uint64_t ntok = 0, tcap = 0;
    xksm_id *mid = NULL;
    uint64_t *mstart = NULL; /* token range of memory i: [mstart[i], mstart[i+1]) */
    size_t nm = 0, mcap = 0;
    xksm_status rc = XKSM_OK;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(x->db, "SELECT id, text FROM memory WHERE state = 1 ORDER BY id", -1,
                           &st, NULL) != SQLITE_OK)
        return xksm__fail(x, XKSM_EIO, "semantic: %s", sqlite3_errmsg(x->db));
    while (sqlite3_step(st) == SQLITE_ROW) {
        if (nm + 1 >= mcap) {
            mcap = mcap ? 2 * mcap : 4096;
            xksm_id *a = realloc(mid, mcap * sizeof *a);
            uint64_t *b = a ? realloc(mstart, (mcap + 1) * sizeof *b) : NULL;
            if (!a || !b) { rc = XKSM_ENOMEM; break; }
            mid = a;
            mstart = b;
        }
        mid[nm] = sqlite3_column_int64(st, 0);
        mstart[nm] = ntok;
        const char *text = (const char *)sqlite3_column_text(st, 1);
        size_t n = (size_t)sqlite3_column_bytes(st, 1), pos = 0, len;
        char tok[MAXTOK];
        while ((len = next_token(text, n, &pos, tok)) > 0) {
            uint32_t w = vocab_find(&full, tok, len, 1);
            if (w == UINT32_MAX) { rc = XKSM_ENOMEM; break; }
            full.count[w]++;
            if (ntok == tcap) {
                tcap = tcap ? 2 * tcap : 1 << 20;
                uint32_t *nt = realloc(toks, tcap * sizeof *nt);
                if (!nt) { rc = XKSM_ENOMEM; break; }
                toks = nt;
            }
            toks[ntok++] = w;
        }
        if (rc != XKSM_OK)
            break;
        ++nm;
    }
    sqlite3_finalize(st);
    st = NULL;
    if (mstart)
        mstart[nm] = ntok;
    r.memories = nm;
    r.tokens = (size_t)ntok;

    /* prune: words with count >= min_count, remap token ids */
    vocab v = {0};
    uint32_t *remap = rc == XKSM_OK ? malloc(sizeof *remap * (full.n ? full.n : 1)) : NULL;
    if (rc == XKSM_OK && !remap)
        rc = XKSM_ENOMEM;
    for (uint32_t i = 0; rc == XKSM_OK && i < full.n; ++i) {
        remap[i] = UINT32_MAX;
        if (full.count[i] >= minc) {
            const char *w = full.arena + full.off[i];
            uint32_t id = vocab_find(&v, w, strlen(w), 1);
            if (id == UINT32_MAX) { rc = XKSM_ENOMEM; break; }
            v.count[id] = full.count[i];
            remap[i] = id;
        }
    }
    uint64_t kept_tokens = 0;
    for (uint64_t i = 0; rc == XKSM_OK && i < ntok; ++i) {
        toks[i] = remap[toks[i]];
        kept_tokens += toks[i] != UINT32_MAX;
    }
    free(remap);
    vocab_free(&full);
    r.vocab = v.n;
    r.t_tokens = now_s() - t;
    t = now_s();
    if (rc == XKSM_OK && (v.n <= dim + 16 || kept_tokens == 0))
        rc = xksm__fail(x, XKSM_EINVAL, "semantic: vocabulary %u too small for dim %zu", v.n, dim);

    /* pass 2: symmetric harmonic-window co-occurrence over kept tokens (OOV removed) */
    float *keep = NULL;
    pairs P = {0};
    if (rc == XKSM_OK) {
        keep = malloc(sizeof *keep * v.n);
        if (!keep)
            rc = XKSM_ENOMEM;
        for (uint32_t w = 0; keep && w < v.n; ++w) { /* deterministic subsampling weight */
            double f = (double)v.count[w] / (double)kept_tokens;
            keep[w] = (float)(f > sub_t ? sqrt(sub_t / f) : 1.0);
        }
    }
    uint32_t *seq = rc == XKSM_OK ? malloc(sizeof *seq * (size_t)(ntok ? ntok : 1)) : NULL;
    if (rc == XKSM_OK && !seq)
        rc = XKSM_ENOMEM;
    for (size_t m = 0; rc == XKSM_OK && m < nm; ++m) {
        size_t ns = 0;
        for (uint64_t i = mstart[m]; i < mstart[m + 1]; ++i)
            if (toks[i] != UINT32_MAX)
                seq[ns++] = toks[i];
        for (size_t i = 0; i < ns && rc == XKSM_OK; ++i)
            for (size_t d = 1; d <= win && i + d < ns; ++d) {
                uint32_t a = seq[i], b = seq[i + d];
                float w = keep[a] * keep[b] / (float)d;
                if (!pairs_add(&P, a, b, w) || !pairs_add(&P, b, a, w))
                    rc = XKSM_ENOMEM;
            }
    }
    free(seq);
    r.pairs = P.n;
    r.t_cooc = now_s() - t;
    t = now_s();

    /* PPMI with context distribution smoothing, into CSR */
    xksm_csr A = {0}, At = {0};
    if (rc == XKSM_OK) {
        double *rs = calloc(v.n, sizeof *rs), T = 0, Ta = 0;
        xksm_triplet *tr = malloc(sizeof *tr * (size_t)(P.n ? P.n : 1));
        if (!rs || !tr)
            rc = XKSM_ENOMEM;
        for (uint64_t i = 0; rc == XKSM_OK && i <= P.mask; ++i)
            if (P.key[i] != UINT64_MAX)
                rs[P.key[i] >> 32] += P.val[i];
        for (uint32_t w = 0; rc == XKSM_OK && w < v.n; ++w) {
            T += rs[w];
            Ta += pow(rs[w], cds);
        }
        uint64_t nt = 0;
        for (uint64_t i = 0; rc == XKSM_OK && i <= P.mask; ++i) {
            if (P.key[i] == UINT64_MAX)
                continue;
            uint32_t a = (uint32_t)(P.key[i] >> 32), b = (uint32_t)P.key[i];
            double pmi = log((double)P.val[i] * Ta / (rs[a] * pow(rs[b], cds))) - shift;
            if (pmi > 0)
                tr[nt++] = (xksm_triplet){a, b, (float)pmi};
        }
        (void)T;
        if (rc == XKSM_OK && (!xksm__csr_from_triplets(&A, v.n, v.n, tr, nt) ||
                              !xksm__csr_transpose(&A, &At)))
            rc = XKSM_ENOMEM;
        r.nnz = A.nnz;
        free(rs);
        free(tr);
    }
    free(P.key);
    free(P.val);
    r.t_ppmi = now_s() - t;
    t = now_s();

    /* randomized SVD -> word vectors (U + V) Sigma^0.5, row-normalized */
    float *U = NULL, *Vr = NULL, *sig = NULL;
    if (rc == XKSM_OK) {
        U = malloc(sizeof *U * v.n * dim);
        Vr = malloc(sizeof *Vr * v.n * dim);
        sig = malloc(sizeof *sig * dim);
        if (!U || !Vr || !sig)
            rc = XKSM_ENOMEM;
        else {
            int done = 0;
#ifdef XKSM_HAVE_OPENCL
            if (o.gpu >= 1)
                done = xksm__ocl_rsvd(&A, &At, dim, 16, q, 0x5EED, U, sig, Vr, r.device,
                                      sizeof r.device);
#endif
            r.gpu_used = done;
            if (!done && o.gpu == 1)
                rc = xksm__fail(x, XKSM_EIO, "semantic: GPU required but unavailable");
            else if (!done && !xksm__rsvd(xksm__pool(x), &A, &At, dim, 16, q, 0x5EED, U, sig, Vr))
                rc = xksm__fail(x, XKSM_EIO, "semantic: SVD failed (rank deficient matrix)");
        }
    }
    xksm__csr_free(&A);
    xksm__csr_free(&At);
    if (rc == XKSM_OK) {
        for (uint32_t w = 0; w < v.n; ++w) {
            float *uw = U + (size_t)w * dim;
            const float *vw = Vr + (size_t)w * dim;
            for (size_t d = 0; d < dim; ++d)
                uw[d] = (uw[d] + vw[d]) * sqrtf(sig[d]);
            xksm__normalize(uw, dim);
        }
    }
    free(Vr);
    free(sig);
    r.t_svd = now_s() - t;
    t = now_s();

    /* memory vectors: SIF average, then remove the first principal component */
    struct xksm_semantic *s = NULL;
    float *mv = NULL;
    if (rc == XKSM_OK) {
        s = calloc(1, sizeof *s);
        mv = calloc(nm * dim, sizeof *mv);
        if (!s || !mv)
            rc = XKSM_ENOMEM;
    }
    if (rc == XKSM_OK) {
        s->dim = dim;
        s->vec = U;
        U = NULL;
        s->sif = malloc(sizeof *s->sif * v.n);
        s->pc = calloc(dim, sizeof *s->pc);
        if (!s->sif || !s->pc)
            rc = XKSM_ENOMEM;
    }
    if (rc == XKSM_OK) {
        for (uint32_t w = 0; w < v.n; ++w)
            s->sif[w] = (float)(sif_a / (sif_a + (double)v.count[w] / (double)kept_tokens));
        uint8_t *has = calloc(nm ? nm : 1, 1);
        for (size_t m = 0; has && m < nm; ++m) {
            float *out = mv + m * dim;
            size_t known = 0;
            for (uint64_t i = mstart[m]; i < mstart[m + 1]; ++i) {
                uint32_t w = toks[i];
                if (w == UINT32_MAX)
                    continue;
                const float *wv = s->vec + (size_t)w * dim;
                for (size_t d = 0; d < dim; ++d)
                    out[d] += s->sif[w] * wv[d];
                ++known;
            }
            if (known) {
                for (size_t d = 0; d < dim; ++d)
                    out[d] /= (float)known;
                has[m] = 1;
            }
        }
        /* first right singular vector of the memory matrix by power iteration (no centering) */
        float *pc = s->pc;
        for (size_t d = 0; d < dim; ++d)
            pc[d] = 1.0f / sqrtf((float)dim);
        double *acc = malloc(sizeof *acc * dim);
        for (int it = 0; acc && it < 100; ++it) {
            memset(acc, 0, sizeof *acc * dim);
            for (size_t m = 0; m < nm; ++m) {
                const float *row = mv + m * dim;
                double p = 0;
                for (size_t d = 0; d < dim; ++d)
                    p += (double)row[d] * pc[d];
                for (size_t d = 0; d < dim; ++d)
                    acc[d] += p * row[d];
            }
            double nrm = 0, delta = 0;
            for (size_t d = 0; d < dim; ++d)
                nrm += acc[d] * acc[d];
            nrm = sqrt(nrm);
            if (nrm == 0)
                break;
            for (size_t d = 0; d < dim; ++d) {
                float nv = (float)(acc[d] / nrm);
                delta += fabs((double)nv - pc[d]);
                pc[d] = nv;
            }
            if (delta < 1e-7)
                break;
        }
        free(acc);
        for (size_t m = 0; m < nm; ++m) {
            float *row = mv + m * dim;
            double p = 0;
            for (size_t d = 0; d < dim; ++d)
                p += (double)row[d] * pc[d];
            for (size_t d = 0; d < dim; ++d)
                row[d] -= (float)p * pc[d];
        }
        r.t_embed = now_s() - t;
        t = now_s();

        /* store: model + memory vectors in one transaction */
        int ok = has && sqlite3_exec(x->db, "BEGIN IMMEDIATE", NULL, NULL, NULL) == SQLITE_OK;
        if (ok) {
            sqlite3_stmt *dv = NULL, *ins = NULL;
            ok = sqlite3_exec(x->db,
                              "DELETE FROM semantic_vocab; DELETE FROM semantic_meta;"
                              "DELETE FROM memory_vec WHERE model = '" XKSM_SEMANTIC_MODEL "';",
                              NULL, NULL, NULL) == SQLITE_OK &&
                 sqlite3_prepare_v2(x->db,
                                    "INSERT INTO semantic_vocab(id, word, count, vec) VALUES (?1, ?2, ?3, ?4)",
                                    -1, &dv, NULL) == SQLITE_OK &&
                 sqlite3_prepare_v2(x->db,
                                    "INSERT INTO memory_vec(memory_id, model, dim, vec, code, scale) "
                                    "VALUES (?1, '" XKSM_SEMANTIC_MODEL "', ?2, ?3, ?4, ?5)",
                                    -1, &ins, NULL) == SQLITE_OK;
            for (uint32_t w = 0; ok && w < v.n; ++w) {
                sqlite3_reset(dv);
                ok = sqlite3_bind_int64(dv, 1, w) == SQLITE_OK &&
                     sqlite3_bind_text(dv, 2, v.arena + v.off[w], -1, SQLITE_STATIC) == SQLITE_OK &&
                     sqlite3_bind_int64(dv, 3, (sqlite3_int64)v.count[w]) == SQLITE_OK &&
                     sqlite3_bind_blob64(dv, 4, s->vec + (size_t)w * dim, dim * sizeof(float),
                                         SQLITE_STATIC) == SQLITE_OK &&
                     sqlite3_step(dv) == SQLITE_DONE;
            }
            int8_t *code = malloc(dim);
            for (size_t m = 0; ok && code && m < nm; ++m) {
                if (!has[m])
                    continue;
                float *row = mv + m * dim;
                xksm__normalize(row, dim);
                float scale = xksm__lattice_encode(row, dim, code);
                sqlite3_reset(ins);
                ok = sqlite3_bind_int64(ins, 1, mid[m]) == SQLITE_OK &&
                     sqlite3_bind_int64(ins, 2, (sqlite3_int64)dim) == SQLITE_OK &&
                     sqlite3_bind_blob64(ins, 3, row, dim * sizeof(float), SQLITE_STATIC) == SQLITE_OK &&
                     sqlite3_bind_blob64(ins, 4, code, dim, SQLITE_STATIC) == SQLITE_OK &&
                     sqlite3_bind_double(ins, 5, (double)scale) == SQLITE_OK &&
                     sqlite3_step(ins) == SQLITE_DONE;
                r.embedded++;
            }
            ok = ok && code;
            free(code);
            uint64_t d64 = dim;
            double params[6] = {(double)win, (double)minc, cds, o.shift, sub_t, sif_a};
            ok = ok && meta_put(x->db, "dim", &d64, sizeof d64) &&
                 meta_put(x->db, "pc", s->pc, dim * sizeof(float)) &&
                 meta_put(x->db, "params", params, sizeof params);
            sqlite3_finalize(dv);
            sqlite3_finalize(ins);
            if (ok)
                ok = sqlite3_exec(x->db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK;
            if (!ok) {
                rc = xksm__fail(x, XKSM_EIO, "semantic store: %s", sqlite3_errmsg(x->db));
                sqlite3_exec(x->db, "ROLLBACK", NULL, NULL, NULL);
            }
        } else if (rc == XKSM_OK) {
            rc = has ? xksm__fail(x, XKSM_EIO, "semantic begin: %s", sqlite3_errmsg(x->db))
                     : XKSM_ENOMEM;
        }
        free(has);
    }
    r.t_store = now_s() - t;
    free(mv);
    free(U);
    free(keep);
    free(toks);
    free(mid);
    free(mstart);
    if (rc == XKSM_OK) { /* the fresh model replaces any loaded one */
        xksm__semantic_free(x);
        s->v = v;
        x->sem = s;
        xksm__vindex_free(x);
    } else {
        vocab_free(&v);
        if (s) {
            free(s->vec);
            free(s->sif);
            free(s->pc);
            free(s);
        }
    }
    r.t_total = now_s() - t0;
    if (rep)
        *rep = r;
    return rc;
}

/* load the persisted model into x->sem */
static xksm_status load_model(xksm *x)
{
    if (x->sem)
        return XKSM_OK;
    sqlite3_stmt *st = NULL;
    size_t dim = 0;
    float *pc = NULL;
    if (sqlite3_prepare_v2(x->db, "SELECT key, value FROM semantic_meta", -1, &st, NULL) != SQLITE_OK)
        return xksm__fail(x, XKSM_EIO, "semantic load: %s", sqlite3_errmsg(x->db));
    double a = 1e-3;
    while (sqlite3_step(st) == SQLITE_ROW) {
        const char *k = (const char *)sqlite3_column_text(st, 0);
        size_t n = (size_t)sqlite3_column_bytes(st, 1);
        const void *b = sqlite3_column_blob(st, 1);
        if (!strcmp(k, "dim") && n == sizeof(uint64_t)) {
            uint64_t d;
            memcpy(&d, b, sizeof d);
            dim = (size_t)d;
        } else if (!strcmp(k, "pc") && n % sizeof(float) == 0 && n) {
            free(pc);
            pc = malloc(n);
            if (pc)
                memcpy(pc, b, n);
        } else if (!strcmp(k, "params") && n == 6 * sizeof(double)) {
            double p[6];
            memcpy(p, b, sizeof p);
            a = p[5];
        }
    }
    sqlite3_finalize(st);
    if (!dim || !pc) {
        free(pc);
        return xksm__fail(x, XKSM_ENOTFOUND, "no semantic model built");
    }
    struct xksm_semantic *s = calloc(1, sizeof *s);
    if (!s) {
        free(pc);
        return XKSM_ENOMEM;
    }
    s->dim = dim;
    s->pc = pc;
    uint64_t total = 0, cap = 0;
    xksm_status rc = XKSM_OK;
    if (sqlite3_prepare_v2(x->db, "SELECT id, word, count, vec FROM semantic_vocab ORDER BY id",
                           -1, &st, NULL) != SQLITE_OK)
        rc = xksm__fail(x, XKSM_EIO, "semantic load: %s", sqlite3_errmsg(x->db));
    while (rc == XKSM_OK && sqlite3_step(st) == SQLITE_ROW) {
        const char *w = (const char *)sqlite3_column_text(st, 1);
        if ((size_t)sqlite3_column_bytes(st, 3) != dim * sizeof(float))
            continue;
        uint32_t id = vocab_find(&s->v, w, strlen(w), 1);
        if (id == UINT32_MAX) { rc = XKSM_ENOMEM; break; }
        s->v.count[id] = (uint64_t)sqlite3_column_int64(st, 2);
        total += s->v.count[id];
        if (id >= cap) {
            cap = cap ? 2 * cap : 4096;
            float *nv = realloc(s->vec, sizeof *nv * cap * dim);
            if (!nv) { rc = XKSM_ENOMEM; break; }
            s->vec = nv;
        }
        memcpy(s->vec + (size_t)id * dim, sqlite3_column_blob(st, 3), dim * sizeof(float));
    }
    sqlite3_finalize(st);
    if (rc == XKSM_OK && s->v.n) {
        s->sif = malloc(sizeof *s->sif * s->v.n);
        if (!s->sif)
            rc = XKSM_ENOMEM;
        for (uint32_t w = 0; s->sif && w < s->v.n; ++w)
            s->sif[w] = (float)(a / (a + (double)s->v.count[w] / (double)total));
    }
    if (rc != XKSM_OK || !s->v.n) {
        vocab_free(&s->v);
        free(s->vec);
        free(s->sif);
        free(s->pc);
        free(s);
        return rc != XKSM_OK ? rc : xksm__fail(x, XKSM_ENOTFOUND, "semantic model is empty");
    }
    x->sem = s;
    return XKSM_OK;
}

size_t xksm_semantic_dim(xksm *x)
{
    return x && load_model(x) == XKSM_OK ? x->sem->dim : 0;
}

xksm_status xksm_semantic_embed(xksm *x, const char *text, size_t len, float *out, size_t dim)
{
    if (!x || !text || !out)
        return XKSM_EINVAL;
    xksm_status rc = load_model(x);
    if (rc != XKSM_OK)
        return rc;
    if (dim != x->sem->dim)
        return xksm__fail(x, XKSM_EINVAL, "embed dim %zu, model has %zu", dim, x->sem->dim);
    if (!embed_text(x->sem, &x->sem->v, text, len, out))
        return xksm__fail(x, XKSM_EINVAL, "no known word in the text");
    return XKSM_OK;
}

xksm_status xksm_semantic_port_load(xksm *x, const char *path)
{
    if (!x || !path)
        return XKSM_EINVAL;
    xksm_port *p = xksm__port_open(path);
    if (!p)
        return xksm__fail(x, XKSM_EIO, "port %s: not a readable XKSMPRT1 file", path);
    xksm__port_close(x->port);
    x->port = p;
    if (x->sem) { /* targets depend on the port: rebuild on next use */
        for (int b = 0; b < 16; ++b) {
            free(x->sem->tmat[b]);
            free(x->sem->tid[b]);
            x->sem->tmat[b] = NULL;
            x->sem->tid[b] = NULL;
            x->sem->tn[b] = 0;
        }
        x->sem->targets = 0;
    }
    return XKSM_OK;
}

/* per bucket: the port vectors of the store words that have one (expansion targets) */
static xksm_status build_targets(xksm *x)
{
    struct xksm_semantic *s = x->sem;
    if (s->targets)
        return XKSM_OK;
    size_t pd = xksm__port_dim(x->port), cap[16] = {0};
    for (uint32_t w = 0; w < s->v.n; ++w) {
        const char *word = s->v.arena + s->v.off[w];
        size_t n = strlen(word);
        if (xksm__port_stop(word, n))
            continue;
        int64_t pi = xksm__port_find(x->port, word, n);
        if (pi < 0)
            continue;
        int b = xksm__port_bucket(x->port, (size_t)pi) & 15;
        if (s->tn[b] == cap[b]) {
            cap[b] = cap[b] ? 2 * cap[b] : 1024;
            float *m = realloc(s->tmat[b], sizeof(float) * pd * cap[b]);
            uint32_t *t = m ? realloc(s->tid[b], sizeof *t * cap[b]) : NULL;
            if (!m || !t)
                return XKSM_ENOMEM;
            s->tmat[b] = m;
            s->tid[b] = t;
        }
        memcpy(s->tmat[b] + s->tn[b] * pd, xksm__port_vec(x->port, (size_t)pi), sizeof(float) * pd);
        s->tid[b][s->tn[b]++] = w;
    }
    s->targets = 1;
    return XKSM_OK;
}

xksm_status xksm_semantic_embed_expanded(xksm *x, const char *text, size_t len,
                                         const xksm_expand_opts *opts, float *out, size_t dim,
                                         size_t *expanded)
{
    if (!x || !text || !out)
        return XKSM_EINVAL;
    if (expanded)
        *expanded = 0;
    xksm_status rc = load_model(x);
    if (rc != XKSM_OK)
        return rc;
    struct xksm_semantic *s = x->sem;
    if (dim != s->dim)
        return xksm__fail(x, XKSM_EINVAL, "embed dim %zu, model has %zu", dim, s->dim);
    if (!x->port)
        return xksm__fail(x, XKSM_ENOTFOUND, "no semantic port loaded");
    if ((rc = build_targets(x)) != XKSM_OK)
        return rc;
    xksm_expand_opts o = {0};
    if (opts)
        o = *opts;
    size_t k = o.k ? o.k : 5;
    float min_cos = o.min_cos > 0 ? o.min_cos : 0.6f, alpha = o.alpha > 0 ? o.alpha : 0.5f;
    if (k > 64)
        k = 64;
    size_t pd = xksm__port_dim(x->port);
    xksm_vbackend be = xksm__vbackend_cpu(xksm__pool(x)); /* accelerators plug in here */
    xksm_hit hit[65];
    char tok[MAXTOK];
    size_t pos = 0, tl, known = 0, added = 0;
    memset(out, 0, sizeof(float) * dim);
    while ((tl = next_token(text, len, &pos, tok)) > 0) {
        uint32_t w = vocab_find(&s->v, tok, tl, 0);
        if (w != UINT32_MAX) { /* typed word: full SIF weight */
            const float *wv = s->vec + (size_t)w * dim;
            for (size_t d = 0; d < dim; ++d)
                out[d] += s->sif[w] * wv[d];
            ++known;
        }
        if (xksm__port_stop(tok, tl) || (w != UINT32_MAX && !o.known_too))
            continue;
        int64_t pi = xksm__port_find(x->port, tok, tl);
        if (pi < 0)
            continue;
        int b = xksm__port_bucket(x->port, (size_t)pi) & 15;
        if (!s->tn[b])
            continue;
        size_t nh = be.topk_dot(be.ctx, s->tmat[b], s->tn[b], pd, xksm__port_vec(x->port, (size_t)pi),
                                k + 1, hit);
        size_t taken = 0;
        for (size_t h = 0; h < nh && taken < k; ++h) {
            if (hit[h].score < min_cos)
                break;
            uint32_t t = s->tid[b][hit[h].index];
            if (t == w)
                continue; /* the word itself */
            if (getenv("XKSM_EXP_DEBUG")) /* diagnostic trace: query word -> related store word */
                fprintf(stderr, "expand %.*s -> %s (%.3f)\n", (int)tl, tok,
                        s->v.arena + s->v.off[t], (double)hit[h].score);
            const float *tv = s->vec + (size_t)t * dim;
            float c = alpha * hit[h].score * s->sif[t];
            for (size_t d = 0; d < dim; ++d)
                out[d] += c * tv[d];
            ++taken;
        }
        added += taken;
    }
    if (!known && !added)
        return xksm__fail(x, XKSM_EINVAL, "no known or related word in the text");
    double dot = 0;
    for (size_t d = 0; d < dim; ++d) {
        out[d] /= (float)(known ? known : 1);
        dot += (double)out[d] * s->pc[d];
    }
    for (size_t d = 0; d < dim; ++d)
        out[d] -= (float)dot * s->pc[d];
    if (expanded)
        *expanded = added;
    return XKSM_OK;
}
