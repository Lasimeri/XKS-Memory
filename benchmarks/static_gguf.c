/* static_gguf.c: static embeddings from a GGUF embedding model's weights, no forward pass.
 * Reads the GGUF directly (own parser, mmap), tokenizes with the model's Unigram (UGM)
 * vocabulary by Viterbi over the piece log-probabilities, dequantizes only the token-
 * embedding rows that are used (F32 / F16 / Q6_K), and pools SIF-weighted with weights
 * a / (a + p(piece)), p = exp(score); the first principal component of the memory vectors
 * is removed (Arora et al. 2017). Writes memory vectors under MODEL_NAME and query vectors
 * (prefixes "search_document: " / "search_query: ") to QVEC_OUT for eval_retrieval.
 * Normalization approximation: whitespace collapsed, U+2581 word prefix; SentencePiece's
 * precompiled NFKC map is not applied. With -DXKSM_WITH_LLAMA the tokenization is compared
 * with llama.cpp's for every memory.
 * usage: static_gguf MODEL.gguf STORE QUERIES.tsv QVEC_OUT MODEL_NAME [sif_a=1e-3] */
#define _POSIX_C_SOURCE 200809L
#include "xksm.h"

#include <fcntl.h>
#include <math.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#ifdef XKSM_WITH_LLAMA
#include "llama.h"
#endif

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

/* ---- GGUF ---------------------------------------------------------------------- */
enum { G_U8, G_I8, G_U16, G_I16, G_U32, G_I32, G_F32, G_BOOL, G_STR, G_ARR, G_U64, G_I64, G_F64 };
static const size_t gsize[13] = {1, 1, 2, 2, 4, 4, 4, 1, 0, 0, 8, 8, 8};

typedef struct rd {
    const uint8_t *p, *end;
    int bad;
} rd;

static uint64_t rd_u(rd *r, size_t n)
{
    if ((size_t)(r->end - r->p) < n) {
        r->bad = 1;
        return 0;
    }
    uint64_t v = 0;
    memcpy(&v, r->p, n); /* GGUF is little-endian, as is x86-64 */
    r->p += n;
    return v;
}

static const char *rd_str(rd *r, uint64_t *len)
{
    *len = rd_u(r, 8);
    if (r->bad || (uint64_t)(r->end - r->p) < *len) {
        r->bad = 1;
        return NULL;
    }
    const char *s = (const char *)r->p;
    r->p += *len;
    return s;
}

static void skip_value(rd *r, uint32_t t)
{
    if (t == G_STR) {
        uint64_t l;
        rd_str(r, &l);
    } else if (t == G_ARR) {
        uint32_t et = (uint32_t)rd_u(r, 4);
        uint64_t n = rd_u(r, 8);
        for (uint64_t i = 0; i < n && !r->bad; ++i)
            skip_value(r, et);
    } else if (t < 13)
        rd_u(r, gsize[t]);
    else
        r->bad = 1;
}

typedef struct model {
    uint32_t nvocab, dim, ttype;  /* token_embd type: 0 F32, 1 F16, 14 Q6_K */
    const char **piece;            /* points into the mmap, not NUL-terminated */
    uint32_t *plen;
    float *score;
    int32_t *type;                 /* token_type */
    const uint8_t *embd;           /* token_embd data */
    size_t row_bytes;
    float **row;                   /* lazily dequantized rows */
    uint32_t *slot, mask, maxlen;  /* piece hash table (id + 1) */
} model;

static float half_to_float(uint16_t h)
{
    uint32_t s = (uint32_t)(h & 0x8000) << 16, e = (h >> 10) & 0x1F, m = h & 0x3FF, f;
    if (e == 0) {
        if (m == 0)
            f = s;
        else { /* subnormal */
            e = 113;
            while (!(m & 0x400)) {
                m <<= 1;
                --e;
            }
            f = s | (e << 23) | ((m & 0x3FF) << 13);
        }
    } else if (e == 31)
        f = s | 0x7F800000u | (m << 13);
    else
        f = s | ((e + 112) << 23) | (m << 13);
    float out;
    memcpy(&out, &f, 4);
    return out;
}

/* Q6_K, as ggml's dequantize_row_q6_K: 256 values per 210-byte block */
static void deq_q6k(const uint8_t *blk, float *y, size_t n)
{
    for (size_t b = 0; b < n / 256; ++b, blk += 210) {
        const uint8_t *ql = blk, *qh = blk + 128;
        const int8_t *sc = (const int8_t *)(blk + 192);
        uint16_t dh;
        memcpy(&dh, blk + 208, 2);
        float d = half_to_float(dh);
        for (int half = 0; half < 2; ++half) {
            for (int l = 0; l < 32; ++l) {
                int is = l / 16;
                int q1 = ((ql[l] & 0xF) | (((qh[l] >> 0) & 3) << 4)) - 32;
                int q2 = ((ql[l + 32] & 0xF) | (((qh[l] >> 2) & 3) << 4)) - 32;
                int q3 = ((ql[l] >> 4) | (((qh[l] >> 4) & 3) << 4)) - 32;
                int q4 = ((ql[l + 32] >> 4) | (((qh[l] >> 6) & 3) << 4)) - 32;
                y[l + 0] = d * sc[is + 0] * (float)q1;
                y[l + 32] = d * sc[is + 2] * (float)q2;
                y[l + 64] = d * sc[is + 4] * (float)q3;
                y[l + 96] = d * sc[is + 6] * (float)q4;
            }
            y += 128;
            ql += 64;
            qh += 32;
            sc += 8;
        }
    }
}

static const float *row(model *m, uint32_t id)
{
    if (m->row[id])
        return m->row[id];
    float *r = malloc(sizeof(float) * m->dim);
    if (!r)
        return NULL;
    const uint8_t *src = m->embd + (size_t)id * m->row_bytes;
    if (m->ttype == 0)
        memcpy(r, src, sizeof(float) * m->dim);
    else if (m->ttype == 1)
        for (uint32_t i = 0; i < m->dim; ++i) {
            uint16_t h;
            memcpy(&h, src + 2 * i, 2);
            r[i] = half_to_float(h);
        }
    else
        deq_q6k(src, r, m->dim);
    return m->row[id] = r;
}

static uint64_t fnv(const char *s, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i)
        h = (h ^ (uint8_t)s[i]) * 1099511628211ull;
    return h;
}

static int64_t find_piece(const model *m, const char *s, size_t n)
{
    for (uint32_t h = (uint32_t)fnv(s, n) & m->mask; m->slot[h]; h = (h + 1) & m->mask) {
        uint32_t id = m->slot[h] - 1;
        if (m->plen[id] == n && !memcmp(m->piece[id], s, n))
            return id;
    }
    return -1;
}

static int load(model *m, const uint8_t *base, size_t size)
{
    memset(m, 0, sizeof *m);
    rd r = {base, base + size, 0};
    if (size < 24 || memcmp(base, "GGUF", 4))
        return 0;
    r.p += 4;
    uint32_t ver = (uint32_t)rd_u(&r, 4);
    uint64_t nt = rd_u(&r, 8), nkv = rd_u(&r, 8);
    uint64_t align = 32;
    if (ver != 3)
        return 0;
    for (uint64_t k = 0; k < nkv && !r.bad; ++k) {
        uint64_t kl;
        const char *key = rd_str(&r, &kl);
        uint32_t t = (uint32_t)rd_u(&r, 4);
        int is_tok = kl == 21 && !memcmp(key, "tokenizer.ggml.tokens", 21);
        int is_sc = kl == 21 && !memcmp(key, "tokenizer.ggml.scores", 21);
        int is_ty = kl == 25 && !memcmp(key, "tokenizer.ggml.token_type", 25);
        if (kl == 17 && !memcmp(key, "general.alignment", 17) && t == G_U32) {
            align = rd_u(&r, 4);
        } else if ((is_tok || is_sc || is_ty) && t == G_ARR) {
            uint32_t et = (uint32_t)rd_u(&r, 4);
            uint64_t n = rd_u(&r, 8);
            if (!m->nvocab)
                m->nvocab = (uint32_t)n;
            if (n != m->nvocab)
                return 0;
            if (is_tok && et == G_STR) {
                m->piece = malloc(sizeof *m->piece * n);
                m->plen = malloc(sizeof *m->plen * n);
                for (uint64_t i = 0; i < n && !r.bad; ++i) {
                    uint64_t l;
                    m->piece[i] = rd_str(&r, &l);
                    m->plen[i] = (uint32_t)l;
                }
            } else if (is_sc && et == G_F32) {
                m->score = malloc(sizeof *m->score * n);
                for (uint64_t i = 0; i < n && !r.bad; ++i) {
                    uint32_t u = (uint32_t)rd_u(&r, 4);
                    memcpy(&m->score[i], &u, 4);
                }
            } else if (is_ty && et == G_I32) {
                m->type = malloc(sizeof *m->type * n);
                for (uint64_t i = 0; i < n && !r.bad; ++i)
                    m->type[i] = (int32_t)rd_u(&r, 4);
            } else
                for (uint64_t i = 0; i < n && !r.bad; ++i)
                    skip_value(&r, et);
        } else
            skip_value(&r, t);
    }
    uint64_t embd_off = 0;
    int found = 0;
    for (uint64_t i = 0; i < nt && !r.bad; ++i) {
        uint64_t nl;
        const char *name = rd_str(&r, &nl);
        uint32_t nd = (uint32_t)rd_u(&r, 4);
        uint64_t ne[4] = {1, 1, 1, 1};
        for (uint32_t d = 0; d < nd && d < 4; ++d)
            ne[d] = rd_u(&r, 8);
        uint32_t ty = (uint32_t)rd_u(&r, 4);
        uint64_t off = rd_u(&r, 8);
        if (nl == 17 && !memcmp(name, "token_embd.weight", 17)) {
            m->dim = (uint32_t)ne[0];
            m->ttype = ty;
            embd_off = off;
            found = ne[1] == m->nvocab;
            printf("token_embd.weight: %llu x %llu, ggml type %u\n", (unsigned long long)ne[0],
                   (unsigned long long)ne[1], ty);
        }
    }
    if (r.bad || !found || !m->piece || !m->score || !m->type)
        return 0;
    if (m->ttype == 0)
        m->row_bytes = 4u * m->dim;
    else if (m->ttype == 1)
        m->row_bytes = 2u * m->dim;
    else if (m->ttype == 14 && m->dim % 256 == 0)
        m->row_bytes = (size_t)m->dim / 256 * 210;
    else
        return 0; /* type not supported by this tool */
    size_t data = (size_t)(r.p - base);
    data = (data + align - 1) / align * align;
    if (data + embd_off + m->row_bytes * m->nvocab > size)
        return 0;
    m->embd = base + data + embd_off;
    m->row = calloc(m->nvocab, sizeof *m->row);
    uint32_t cap = 1;
    while (cap < 2 * m->nvocab)
        cap <<= 1;
    m->mask = cap - 1;
    m->slot = calloc(cap, sizeof *m->slot);
    if (!m->row || !m->slot)
        return 0;
    for (uint32_t i = 0; i < m->nvocab; ++i) {
        if (m->type[i] != 1 || !m->plen[i]) /* normal pieces only */
            continue;
        uint32_t h = (uint32_t)fnv(m->piece[i], m->plen[i]) & m->mask;
        while (m->slot[h])
            h = (h + 1) & m->mask;
        m->slot[h] = i + 1;
        if (m->plen[i] > m->maxlen)
            m->maxlen = m->plen[i];
    }
    return 1;
}

/* Unigram Viterbi segmentation; returns token count (ids into out, up to cap) */
static size_t tokenize(const model *m, const char *text, size_t n, int32_t *out, size_t cap,
                       char *buf, double *best, int32_t *from, int32_t *tokid)
{
    size_t bl = 0; /* normalize: U+2581 before every word, whitespace runs collapsed */
    int sp = 1;
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)text[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') {
            sp = 1;
            continue;
        }
        if (sp) {
            buf[bl++] = (char)0xE2;
            buf[bl++] = (char)0x96;
            buf[bl++] = (char)0x81;
            sp = 0;
        }
        buf[bl++] = (char)c;
    }
    best[0] = 0;
    for (size_t i = 1; i <= bl; ++i)
        best[i] = -INFINITY;
    for (size_t i = 0; i < bl; ++i) {
        if (best[i] == -INFINITY)
            continue;
        if (((unsigned char)buf[i] & 0xC0) == 0x80)
            continue; /* never start inside a UTF-8 sequence */
        size_t cl = 1; /* length of the character at i, for the unknown fallback */
        unsigned char c0 = (unsigned char)buf[i];
        cl = c0 < 0x80 ? 1 : c0 < 0xE0 ? 2 : c0 < 0xF0 ? 3 : 4;
        if (i + cl > bl)
            cl = bl - i;
        int any = 0;
        for (size_t l = 1; l <= m->maxlen && i + l <= bl; ++l) {
            int64_t id = find_piece(m, buf + i, l);
            if (id < 0)
                continue;
            any |= l == cl;
            double s = best[i] + m->score[id];
            if (s > best[i + l]) {
                best[i + l] = s;
                from[i + l] = (int32_t)i;
                tokid[i + l] = (int32_t)id;
            }
        }
        if (!any) { /* unknown character: <unk> (id 3) with a large penalty */
            double s = best[i] - 20.0;
            if (s > best[i + cl]) {
                best[i + cl] = s;
                from[i + cl] = (int32_t)i;
                tokid[i + cl] = 3;
            }
        }
    }
    size_t k = 0; /* backtrack */
    for (size_t i = bl; i > 0 && best[bl] > -INFINITY;) {
        if (k < cap)
            out[k++] = tokid[i];
        i = (size_t)from[i];
    }
    for (size_t a = 0, b = k ? k - 1 : 0; a < b; ++a, --b) {
        int32_t t = out[a];
        out[a] = out[b];
        out[b] = t;
    }
    return k;
}

int main(int argc, char **argv)
{
    if (argc < 6)
        return 2;
    double sif_a = argc > 6 ? strtod(argv[6], NULL) : 1e-3;
    int fd = open(argv[1], O_RDONLY);
    struct stat stt;
    if (fd < 0 || fstat(fd, &stt) != 0)
        return 1;
    const uint8_t *base = mmap(NULL, (size_t)stt.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (base == MAP_FAILED)
        return 1;
    model m;
    double t0 = now_s();
    if (!load(&m, base, (size_t)stt.st_size)) {
        fprintf(stderr, "GGUF not usable\n");
        return 1;
    }
    printf("vocab %u pieces, dim %u, max piece %u bytes, parsed in %.3f s\n", m.nvocab, m.dim,
           m.maxlen, now_s() - t0);

    sqlite3 *db = NULL;
    sqlite3_stmt *st = NULL;
    if (sqlite3_open(argv[2], &db) != SQLITE_OK ||
        sqlite3_prepare_v2(db, "SELECT id, text FROM memory WHERE state = 1 ORDER BY id", -1, &st,
                           NULL) != SQLITE_OK)
        return 1;
    size_t cap = 4096, n = 0;
    xksm_id *ids = malloc(sizeof *ids * cap);
    char **texts = malloc(sizeof *texts * cap);
    while (sqlite3_step(st) == SQLITE_ROW) {
        if (n == cap) {
            cap *= 2;
            ids = realloc(ids, sizeof *ids * cap);
            texts = realloc(texts, sizeof *texts * cap);
        }
        ids[n] = sqlite3_column_int64(st, 0);
        const char *t = (const char *)sqlite3_column_text(st, 1);
        texts[n] = malloc(strlen(t) + 32);
        strcpy(texts[n], "search_document: ");
        strcat(texts[n], t);
        ++n;
    }
    sqlite3_finalize(st);
    sqlite3_close(db);

    size_t bufcap = 1 << 20, tcap = 1 << 18;
    char *buf = malloc(bufcap * 2);
    double *best = malloc(sizeof *best * (bufcap * 2 + 1));
    int32_t *from = malloc(sizeof *from * (bufcap * 2 + 1)), *tokid = malloc(sizeof *tokid * (bufcap * 2 + 1));
    int32_t *toks = malloc(sizeof *toks * tcap);
    float *vec = calloc(n * m.dim, sizeof *vec);
    long ntok = 0, nunk = 0;
#ifdef XKSM_WITH_LLAMA
    llama_backend_init();
    struct llama_model_params mp = llama_model_default_params();
    mp.vocab_only = true;
    struct llama_model *lm = llama_model_load_from_file(argv[1], mp);
    const struct llama_vocab *lv = lm ? llama_model_get_vocab(lm) : NULL;
    int32_t *ltok = malloc(sizeof *ltok * tcap);
    long same = 0, compared = 0, tok_same = 0, tok_total = 0;
#endif
    double t1 = now_s();
    for (size_t i = 0; i < n; ++i) {
        size_t len = strlen(texts[i]);
        if (len * 2 > bufcap)
            len = bufcap / 2;
        size_t k = tokenize(&m, texts[i], len, toks, tcap, buf, best, from, tokid);
#ifdef XKSM_WITH_LLAMA
        if (lv) {
            int lk = llama_tokenize(lv, texts[i], (int32_t)len, ltok, (int32_t)tcap, false, false);
            compared++;
            same += lk == (int)k && !memcmp(ltok, toks, sizeof *toks * k);
            for (size_t j = 0; j < k && (int)j < lk; ++j)
                tok_same += ltok[j] == toks[j];
            tok_total += lk > (int)k ? lk : (long)k;
        }
#endif
        float *v = vec + i * m.dim;
        for (size_t j = 0; j < k; ++j) {
            if (toks[j] == 3) {
                ++nunk;
                continue;
            }
            const float *e = row(&m, (uint32_t)toks[j]);
            float w = (float)(sif_a / (sif_a + exp((double)m.score[toks[j]])));
            for (uint32_t d = 0; d < m.dim; ++d)
                v[d] += w * e[d];
        }
        if (k)
            for (uint32_t d = 0; d < m.dim; ++d)
                v[d] /= (float)k;
        ntok += (long)k;
    }
    double t_docs = now_s() - t1;
    size_t rows_used = 0;
    for (uint32_t i = 0; i < m.nvocab; ++i)
        rows_used += m.row[i] != NULL;
    printf("memories: %zu, %ld tokens (%ld unknown), %zu distinct embedding rows dequantized "
           "(%.1f MB), %.3f s: %.0f memories/s\n",
           n, ntok, nunk, rows_used, (double)rows_used * m.dim * 4 / 1e6, t_docs,
           (double)n / t_docs);
#ifdef XKSM_WITH_LLAMA
    printf("tokenizer vs llama.cpp: %ld / %ld memories identical, %.4f of token positions equal\n",
           same, compared, tok_total ? (double)tok_same / (double)tok_total : 0.0);
#endif

    /* optional: compare every used embedding row with a second GGUF of the same model
     * (e.g. Q6_K against F32): cosine and relative L2 error per row */
    const char *cmp = getenv("XKSM_COMPARE_GGUF");
    if (cmp) {
        int fd2 = open(cmp, O_RDONLY);
        struct stat st2;
        model m2;
        const uint8_t *b2 = MAP_FAILED;
        if (fd2 >= 0 && fstat(fd2, &st2) == 0)
            b2 = mmap(NULL, (size_t)st2.st_size, PROT_READ, MAP_PRIVATE, fd2, 0);
        if (b2 != MAP_FAILED && load(&m2, b2, (size_t)st2.st_size) && m2.dim == m.dim &&
            m2.nvocab == m.nvocab) {
            double cmin = 1, csum = 0, rmax = 0, rsum = 0;
            size_t nr = 0;
            for (uint32_t i = 0; i < m.nvocab; ++i) {
                if (!m.row[i])
                    continue;
                const float *a = m.row[i], *b = row(&m2, i);
                double ab = 0, aa = 0, bb = 0, dd = 0;
                for (uint32_t d = 0; d < m.dim; ++d) {
                    ab += (double)a[d] * b[d];
                    aa += (double)a[d] * a[d];
                    bb += (double)b[d] * b[d];
                    dd += ((double)a[d] - b[d]) * ((double)a[d] - b[d]);
                }
                double c = ab / sqrt(aa * bb), re = sqrt(dd / bb);
                cmin = c < cmin ? c : cmin;
                csum += c;
                rmax = re > rmax ? re : rmax;
                rsum += re;
                ++nr;
            }
            printf("rows vs %s (type %u): %zu rows, cosine mean %.6f min %.6f, relative L2 error "
                   "mean %.4f max %.4f\n",
                   cmp, m2.ttype, nr, csum / (double)nr, cmin, rsum / (double)nr, rmax);
        } else
            printf("compare: %s not usable\n", cmp);
    }

    /* common component (first right singular vector, no centering) */
    float *pc = calloc(m.dim, sizeof *pc);
    double *acc = malloc(sizeof *acc * m.dim);
    for (uint32_t d = 0; d < m.dim; ++d)
        pc[d] = 1.0f / sqrtf((float)m.dim);
    for (int it = 0; it < 100; ++it) {
        memset(acc, 0, sizeof *acc * m.dim);
        for (size_t i = 0; i < n; ++i) {
            const float *v = vec + i * m.dim;
            double p = 0;
            for (uint32_t d = 0; d < m.dim; ++d)
                p += (double)v[d] * pc[d];
            for (uint32_t d = 0; d < m.dim; ++d)
                acc[d] += p * v[d];
        }
        double nr = 0;
        for (uint32_t d = 0; d < m.dim; ++d)
            nr += acc[d] * acc[d];
        nr = sqrt(nr);
        for (uint32_t d = 0; d < m.dim; ++d)
            pc[d] = (float)(acc[d] / nr);
    }
    xksm *x = NULL;
    if (xksm_open(argv[2], &x) != XKSM_OK)
        return 1;
    size_t bad = 0;
    for (size_t i = 0; i < n; ++i) {
        float *v = vec + i * m.dim;
        double p = 0;
        for (uint32_t d = 0; d < m.dim; ++d)
            p += (double)v[d] * pc[d];
        for (uint32_t d = 0; d < m.dim; ++d)
            v[d] -= (float)p * pc[d];
        bad += xksm_vector_put(x, ids[i], argv[5], v, m.dim) != XKSM_OK;
    }
    xksm_close(x);
    printf("stored %zu vectors as \"%s\" (%zu failed)\n", n - bad, argv[5], bad);

    FILE *f = fopen(argv[3], "r"), *o = fopen(argv[4], "wb");
    if (!f || !o)
        return 1;
    char line[2048], q[2200];
    size_t nq = 0;
    float *qv = malloc(sizeof(float) * m.dim);
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *a = strchr(line, '\t'), *b = a ? strchr(a + 1, '\t') : NULL;
        if (!a || !b || line[0] == '#')
            continue;
        *b = 0;
        snprintf(q, sizeof q, "search_query: %s", a + 1);
        size_t k = tokenize(&m, q, strlen(q), toks, tcap, buf, best, from, tokid);
        memset(qv, 0, sizeof(float) * m.dim);
        for (size_t j = 0; j < k; ++j) {
            if (toks[j] == 3)
                continue;
            const float *e = row(&m, (uint32_t)toks[j]);
            float w = (float)(sif_a / (sif_a + exp((double)m.score[toks[j]])));
            for (uint32_t d = 0; d < m.dim; ++d)
                qv[d] += w * e[d];
        }
        double p = 0;
        for (uint32_t d = 0; d < m.dim; ++d)
            p += (double)qv[d] * pc[d];
        for (uint32_t d = 0; d < m.dim; ++d)
            qv[d] -= (float)p * pc[d];
        fwrite(qv, sizeof(float), m.dim, o);
        ++nq;
    }
    fclose(o);
    fclose(f);
    printf("queries: %zu written to %s\n", nq, argv[4]);
    return 0;
}
