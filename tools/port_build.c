/* port_build.c: build the semantic "port" file: a static whole-word vector table computed once
 * by an embedding model (llama.cpp GGUF) for the model-free engine. Keys are lowercase words
 * as the engine tokenizes them; each key carries a bucket (Unicode script of its first
 * character, or code) so the engine relates words only within one bucket.
 * Sources: whole-word pieces of the model vocabulary, the store vocabulary (semantic_vocab),
 * code identifiers (contain '_', camelCase, or letters with digits; seen at least twice) from
 * the store's memories and from source files under the given repository directories.
 * File: "XKSMPRT1", u32 dim, u32 n, then n x {u8 bucket, u8 len, bytes}, zero padding to a
 * multiple of 64, then n x dim f32 unit vectors.
 * Extra natural-language words: --words=FILE (one word per line, e.g. harvested from local
 * documentation and man pages), bucketed by script like model words.
 * usage: port_build MODEL.gguf STORE OUT.port [--words=FILE ...] [REPO_DIR ...] */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE /* DT_DIR, DT_REG */
#include "llama.h"

#include <dirent.h>
#include <math.h>
#include <stdint.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { MAXK = 48, NSEQ = 256 };
enum { B_OTHER, B_LATIN, B_CYRILLIC, B_GREEK, B_ARABIC, B_HEBREW, B_DEVANAGARI, B_HAN,
       B_KANA, B_HANGUL, B_THAI, B_CODE, B_COUNT };
static const char *const bname[B_COUNT] = {"other", "latin", "cyrillic", "greek", "arabic",
                                           "hebrew", "devanagari", "han", "kana", "hangul",
                                           "thai", "code"};

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

static unsigned long first_cp(const unsigned char *s)
{
    if (s[0] < 0x80) return s[0];
    if ((s[0] & 0xE0) == 0xC0) return (unsigned long)(s[0] & 0x1F) << 6 | (s[1] & 0x3F);
    if ((s[0] & 0xF0) == 0xE0)
        return (unsigned long)(s[0] & 0x0F) << 12 | (unsigned long)(s[1] & 0x3F) << 6 | (s[2] & 0x3F);
    return (unsigned long)(s[0] & 0x07) << 18 | (unsigned long)(s[1] & 0x3F) << 12 |
           (unsigned long)(s[2] & 0x3F) << 6 | (s[3] & 0x3F);
}

static int script_of(const char *w)
{
    unsigned long c = first_cp((const unsigned char *)w);
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0x24F) ||
        (c >= 0x1E00 && c <= 0x1EFF))
        return B_LATIN;
    if (c >= 0x400 && c <= 0x52F) return B_CYRILLIC;
    if (c >= 0x370 && c <= 0x3FF) return B_GREEK;
    if ((c >= 0x600 && c <= 0x6FF) || (c >= 0x750 && c <= 0x77F)) return B_ARABIC;
    if (c >= 0x590 && c <= 0x5FF) return B_HEBREW;
    if (c >= 0x900 && c <= 0x97F) return B_DEVANAGARI;
    if ((c >= 0x4E00 && c <= 0x9FFF) || (c >= 0x3400 && c <= 0x4DBF)) return B_HAN;
    if (c >= 0x3040 && c <= 0x30FF) return B_KANA;
    if (c >= 0xAC00 && c <= 0xD7AF) return B_HANGUL;
    if (c >= 0xE00 && c <= 0xE7F) return B_THAI;
    return B_OTHER;
}

/* code-shaped: '_' inside, or letters mixed with digits (on the lowercase key) */
static int code_shaped(const char *w)
{
    int letters = 0, digits = 0, under = 0;
    for (const char *p = w; *p; ++p) {
        letters += (*p >= 'a' && *p <= 'z');
        digits += (*p >= '0' && *p <= '9');
        under += *p == '_' && p != w && p[1];
    }
    return under || (letters && digits);
}

/* ---- key set: open addressing over lowercase keys ------------------------------ */
typedef struct keys {
    char (*k)[MAXK];
    uint8_t *bucket;
    uint32_t *count;
    uint32_t n, cap, *slot, mask;
} keys;

static uint64_t fnv(const char *s)
{
    uint64_t h = 1469598103934665603ull;
    for (; *s; ++s)
        h = (h ^ (uint8_t)*s) * 1099511628211ull;
    return h;
}

static int grow(keys *K)
{
    uint32_t nm = K->mask ? 2 * (K->mask + 1) - 1 : (1u << 20) - 1;
    uint32_t *ns = calloc((size_t)nm + 1, sizeof *ns);
    if (!ns)
        return 0;
    for (uint32_t i = 0; i < K->n; ++i) {
        uint32_t h = (uint32_t)fnv(K->k[i]) & nm;
        while (ns[h])
            h = (h + 1) & nm;
        ns[h] = i + 1;
    }
    free(K->slot);
    K->slot = ns;
    K->mask = nm;
    return 1;
}

/* add or count a key; returns its index or UINT32_MAX */
static uint32_t put(keys *K, const char *w, int bucket)
{
    if (!K->slot && !grow(K))
        return UINT32_MAX;
    uint32_t h = (uint32_t)fnv(w) & K->mask;
    for (; K->slot[h]; h = (h + 1) & K->mask) {
        uint32_t i = K->slot[h] - 1;
        if (!strcmp(K->k[i], w)) {
            K->count[i]++;
            if (bucket == B_CODE)
                K->bucket[i] = B_CODE; /* seen as code anywhere: code */
            return i;
        }
    }
    if (K->n == K->cap) {
        K->cap = K->cap ? 2 * K->cap : 1 << 16;
        K->k = realloc(K->k, sizeof *K->k * K->cap);
        K->bucket = realloc(K->bucket, K->cap);
        K->count = realloc(K->count, sizeof *K->count * K->cap);
        if (!K->k || !K->bucket || !K->count)
            return UINT32_MAX;
    }
    strcpy(K->k[K->n], w);
    K->bucket[K->n] = (uint8_t)bucket;
    K->count[K->n] = 1;
    K->slot[h] = ++K->n;
    if ((uint64_t)K->n * 2 > (uint64_t)K->mask + 1 && !grow(K))
        return UINT32_MAX;
    return K->n - 1;
}

/* ---- code identifiers from text ------------------------------------------------- */
static keys code_seen; /* candidate identifiers with counts */

static void harvest(const char *s, size_t n)
{
    size_t i = 0;
    while (i < n) {
        while (i < n && !((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') || s[i] == '_'))
            ++i;
        size_t b = i;
        int camel = 0;
        while (i < n && ((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') ||
                         (s[i] >= '0' && s[i] <= '9') || s[i] == '_')) {
            if (i > b && s[i] >= 'A' && s[i] <= 'Z' && s[i - 1] >= 'a' && s[i - 1] <= 'z')
                camel = 1;
            ++i;
        }
        size_t len = i - b;
        if (len < 3 || len >= MAXK)
            continue;
        char w[MAXK];
        for (size_t k = 0; k < len; ++k)
            w[k] = (s[b + k] >= 'A' && s[b + k] <= 'Z') ? (char)(s[b + k] - 'A' + 'a') : s[b + k];
        w[len] = 0;
        if (camel || code_shaped(w))
            put(&code_seen, w, B_CODE);
    }
}

static int wanted_file(const char *name)
{
    static const char *const ext[] = {".c", ".h", ".cpp", ".hpp", ".cc", ".cu", ".cl", ".rs",
                                      ".py", ".sh", ".cmake"};
    if (!strcmp(name, "CMakeLists.txt"))
        return 1;
    const char *dot = strrchr(name, '.');
    for (size_t i = 0; dot && i < sizeof ext / sizeof *ext; ++i)
        if (!strcmp(dot, ext[i]))
            return 1;
    return 0;
}

static size_t files_read, bytes_read;

static void walk(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d)
        return;
    struct dirent *e;
    char path[4096];
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.' || !strncmp(e->d_name, "build", 5))
            continue; /* .git, hidden, build trees */
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        if (e->d_type == DT_DIR) {
            walk(path);
        } else if (e->d_type == DT_REG && wanted_file(e->d_name)) {
            FILE *f = fopen(path, "rb");
            if (!f)
                continue;
            static char buf[1 << 20];
            size_t got;
            while ((got = fread(buf, 1, sizeof buf, f)) > 0) {
                harvest(buf, got);
                bytes_read += got;
            }
            fclose(f);
            files_read++;
        }
    }
    closedir(d);
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s MODEL.gguf STORE OUT.port [REPO_DIR ...]\n", argv[0]);
        return 2;
    }
    double t0 = now_s();
    llama_backend_init();
    struct llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = -1;
    struct llama_model *model = llama_model_load_from_file(argv[1], mp);
    if (!model)
        return 1;
    const struct llama_vocab *vocab = llama_model_get_vocab(model);
    keys K = {0};
    size_t from_model = 0, from_store = 0, from_code = 0;

    /* 1: whole-word pieces of the model vocabulary */
    int32_t nv = llama_vocab_n_tokens(vocab);
    for (int32_t t = 0; t < nv; ++t) {
        if (!(llama_vocab_get_attr(vocab, t) & LLAMA_TOKEN_ATTR_NORMAL))
            continue;
        const char *p = llama_vocab_get_text(vocab, t);
        if (strncmp(p, "\xE2\x96\x81", 3))
            continue;
        const char *w = p + 3;
        size_t len = strlen(w);
        if (len < 2 || len >= MAXK)
            continue;
        char lw[MAXK];
        for (size_t k = 0; k <= len; ++k)
            lw[k] = (w[k] >= 'A' && w[k] <= 'Z') ? (char)(w[k] - 'A' + 'a') : w[k];
        size_t before = K.n;
        put(&K, lw, code_shaped(lw) ? B_CODE : script_of(lw));
        from_model += K.n > before;
    }
    /* 2: the store vocabulary; 3: code identifiers from memories and repositories */
    sqlite3 *db = NULL;
    sqlite3_stmt *st = NULL;
    if (sqlite3_open(argv[2], &db) != SQLITE_OK)
        return 1;
    if (sqlite3_prepare_v2(db, "SELECT word FROM semantic_vocab", -1, &st, NULL) == SQLITE_OK)
        while (sqlite3_step(st) == SQLITE_ROW) {
            const char *w = (const char *)sqlite3_column_text(st, 0);
            if (strlen(w) >= MAXK)
                continue;
            size_t before = K.n;
            put(&K, w, code_shaped(w) ? B_CODE : script_of(w));
            from_store += K.n > before;
        }
    sqlite3_finalize(st);
    if (sqlite3_prepare_v2(db, "SELECT text FROM memory WHERE state = 1", -1, &st, NULL) == SQLITE_OK)
        while (sqlite3_step(st) == SQLITE_ROW)
            harvest((const char *)sqlite3_column_text(st, 0), (size_t)sqlite3_column_bytes(st, 0));
    sqlite3_finalize(st);
    sqlite3_close(db);
    size_t from_list = 0;
    for (int i = 4; i < argc; ++i) { /* word lists */
        if (strncmp(argv[i], "--words=", 8))
            continue;
        FILE *wf = fopen(argv[i] + 8, "r");
        char wl[256];
        while (wf && fgets(wl, sizeof wl, wf)) {
            wl[strcspn(wl, "\r\n")] = 0;
            size_t len = strlen(wl);
            if (len < 2 || len >= MAXK)
                continue;
            for (size_t q = 0; q < len; ++q)
                wl[q] = (wl[q] >= (char)0x41 && wl[q] <= (char)0x5A) ? (char)(wl[q] + 32) : wl[q];
            size_t before = K.n;
            put(&K, wl, code_shaped(wl) ? B_CODE : script_of(wl));
            from_list += K.n > before;
        }
        if (wf)
            fclose(wf);
    }
    for (int i = 4; i < argc; ++i)
        if (strncmp(argv[i], "--words=", 8))
            walk(argv[i]);
    for (uint32_t i = 0; i < code_seen.n; ++i)
        if (code_seen.count[i] >= 2) {
            size_t before = K.n;
            put(&K, code_seen.k[i], B_CODE);
            from_code += K.n > before;
        }
    size_t per[B_COUNT] = {0};
    for (uint32_t i = 0; i < K.n; ++i)
        per[K.bucket[i]]++;
    printf("word lists: %zu new keys\n", from_list);
    printf("keys: %u (model whole words %zu, store words %zu new, code identifiers %zu new; "
           "%zu repo files, %.1f MB read)\n",
           K.n, from_model, from_store, from_code, files_read, (double)bytes_read / 1e6);
    for (int b = 0; b < B_COUNT; ++b)
        if (per[b])
            printf("  bucket %-10s %zu\n", bname[b], per[b]);

    /* embed every key once on the GPU (whole word, BOS/EOS, the model's pooling) */
    struct llama_context_params cp = llama_context_default_params();
    cp.embeddings = true;
    cp.n_ctx = NSEQ * 16;
    cp.n_batch = NSEQ * 16;
    cp.n_ubatch = NSEQ * 16;
    cp.n_seq_max = NSEQ;
    struct llama_context *ctx = llama_init_from_model(model, cp);
    if (!ctx)
        return 1;
    int dim = llama_model_n_embd_out(model);
    float *vec = malloc(sizeof(float) * (size_t)dim * K.n);
    struct llama_batch batch = llama_batch_init(NSEQ * 16, 0, 1);
    if (!vec)
        return 1;
    uint32_t pending[NSEQ];
    int ns = 0;
    double t1 = now_s();
    llama_token tok[64];
    for (uint32_t i = 0; i <= K.n; ++i) {
        int n = 0;
        if (i < K.n) {
            n = llama_tokenize(vocab, K.k[i], (int32_t)strlen(K.k[i]), tok, 64, true, false);
            if (n <= 0 || n > 64)
                n = 0;
        }
        if (ns && (i == K.n || ns == NSEQ || batch.n_tokens + n > NSEQ * 16)) {
            if (llama_encode(ctx, batch) != 0)
                return 1;
            for (int s = 0; s < ns; ++s) {
                const float *v = llama_get_embeddings_seq(ctx, s);
                float *o = vec + (size_t)pending[s] * (size_t)dim;
                double nr = 0;
                for (int d = 0; d < dim; ++d)
                    nr += (double)v[d] * v[d];
                nr = nr > 0 ? 1.0 / sqrt(nr) : 0;
                for (int d = 0; d < dim; ++d)
                    o[d] = (float)(v[d] * nr);
            }
            batch.n_tokens = 0;
            ns = 0;
        }
        if (i == K.n)
            break;
        if (!n) {
            memset(vec + (size_t)i * (size_t)dim, 0, sizeof(float) * (size_t)dim);
            continue;
        }
        for (int t = 0; t < n; ++t) {
            int j = batch.n_tokens++;
            batch.token[j] = tok[t];
            batch.pos[j] = t;
            batch.n_seq_id[j] = 1;
            batch.seq_id[j][0] = ns;
            batch.logits[j] = 1;
        }
        pending[ns++] = i;
    }
    double t_emb = now_s() - t1;

    FILE *f = fopen(argv[3], "wb");
    if (!f)
        return 1;
    uint32_t d32 = (uint32_t)dim, n32 = K.n;
    fwrite("XKSMPRT1", 1, 8, f);
    fwrite(&d32, 4, 1, f);
    fwrite(&n32, 4, 1, f);
    size_t off = 16;
    for (uint32_t i = 0; i < K.n; ++i) {
        uint8_t b = K.bucket[i], l = (uint8_t)strlen(K.k[i]);
        fwrite(&b, 1, 1, f);
        fwrite(&l, 1, 1, f);
        fwrite(K.k[i], 1, l, f);
        off += 2u + l;
    }
    static const char zero[64];
    fwrite(zero, 1, (64 - off % 64) % 64, f);
    fwrite(vec, sizeof(float) * (size_t)dim, K.n, f);
    long size = ftell(f);
    fclose(f);
    printf("embedded %u keys in %.3f s on the GPU; port %s: %ld bytes; total %.3f s\n", K.n,
           t_emb, argv[3], size, now_s() - t0);
    llama_batch_free(batch);
    llama_free(ctx);
    llama_model_free(model);
    llama_backend_free();
    return 0;
}
