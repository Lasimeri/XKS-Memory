/* embed_llama.c: reference embeddings from a llama.cpp GGUF embedding model, for comparing
 * against the built-in semantic model. Embeds every active memory (prefix "search_document: ")
 * into memory_vec under MODEL_NAME, and every query of a ground-truth file (prefix
 * "search_query: ") into QVEC_OUT (float32[dim] per query, file order) for eval_retrieval.
 * Prefixes as required by nomic-embed models; inputs truncated at 512 tokens.
 * usage: embed_llama MODEL.gguf STORE QUERIES.tsv QVEC_OUT MODEL_NAME */
#define _POSIX_C_SOURCE 200809L /* clock_gettime under strict -std=c17 */
#include "llama.h"
#include "xksm.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { MAXTOK = 512, NSEQ = 64, NBATCH = 16384 };

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

typedef struct emb {
    struct llama_context *ctx;
    const struct llama_vocab *vocab;
    struct llama_batch batch;
    int dim, nseq;
    float *out[NSEQ]; /* destination of each pending sequence */
    double t_infer;
    long tokens, truncated;
} emb;

static int flush(emb *e)
{
    if (!e->nseq)
        return 1;
    double t = now_s();
    if (llama_encode(e->ctx, e->batch) != 0) {
        fprintf(stderr, "llama_encode failed\n");
        return 0;
    }
    for (int s = 0; s < e->nseq; ++s) {
        const float *v = llama_get_embeddings_seq(e->ctx, s);
        if (!v) {
            fprintf(stderr, "no pooled embedding for sequence %d\n", s);
            return 0;
        }
        memcpy(e->out[s], v, sizeof(float) * (size_t)e->dim);
    }
    e->t_infer += now_s() - t;
    e->batch.n_tokens = 0;
    e->nseq = 0;
    return 1;
}

/* queue prefix + text; its embedding lands in out when the batch is flushed */
static int add(emb *e, const char *prefix, const char *text, float *out)
{
    static llama_token tok[MAXTOK + 64];
    size_t pl = strlen(prefix), tl = strlen(text);
    char *s = malloc(pl + tl + 1);
    if (!s)
        return 0;
    memcpy(s, prefix, pl);
    memcpy(s + pl, text, tl + 1);
    int n = llama_tokenize(e->vocab, s, (int32_t)(pl + tl), tok, MAXTOK, true, false);
    if (n < 0) { /* longer than MAXTOK: llama_tokenize returns -needed; keep the first MAXTOK */
        static llama_token *big;
        static int bigcap;
        if (-n > bigcap) {
            bigcap = -n;
            big = realloc(big, sizeof *big * (size_t)bigcap);
        }
        n = llama_tokenize(e->vocab, s, (int32_t)(pl + tl), big, bigcap, true, false);
        memcpy(tok, big, sizeof *tok * MAXTOK);
        n = MAXTOK;
        e->truncated++;
    }
    free(s);
    if (e->batch.n_tokens + n > NBATCH || e->nseq == NSEQ)
        if (!flush(e))
            return 0;
    for (int i = 0; i < n; ++i) {
        int k = e->batch.n_tokens++;
        e->batch.token[k] = tok[i];
        e->batch.pos[k] = i;
        e->batch.n_seq_id[k] = 1;
        e->batch.seq_id[k][0] = e->nseq;
        e->batch.logits[k] = 1;
    }
    e->out[e->nseq++] = out;
    e->tokens += n;
    return 1;
}

int main(int argc, char **argv)
{
    if (argc != 6) {
        fprintf(stderr, "usage: %s MODEL.gguf STORE QUERIES.tsv QVEC_OUT MODEL_NAME\n", argv[0]);
        return 2;
    }
    llama_backend_init();
    struct llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = -1; /* everything on the GPU */
    double t0 = now_s();
    struct llama_model *model = llama_model_load_from_file(argv[1], mp);
    if (!model)
        return 1;
    struct llama_context_params cp = llama_context_default_params();
    cp.embeddings = true;
    cp.n_ctx = NBATCH;
    cp.n_batch = NBATCH;
    cp.n_ubatch = NBATCH;
    cp.n_seq_max = NSEQ;
    emb e = {0};
    e.ctx = llama_init_from_model(model, cp);
    if (!e.ctx)
        return 1;
    e.vocab = llama_model_get_vocab(model);
    e.dim = llama_model_n_embd_out(model);
    e.batch = llama_batch_init(NBATCH, 0, 1);
    double t_load = now_s() - t0;
    printf("model loaded in %.3f s: dim %d, pooling %d\n", t_load, e.dim,
           (int)llama_pooling_type(e.ctx));

    /* memories */
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
        texts[n] = malloc(strlen(t) + 1);
        strcpy(texts[n], t);
        ++n;
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    float *vec = malloc(sizeof(float) * (size_t)e.dim * n);
    double t1 = now_s();
    for (size_t i = 0; i < n; ++i)
        if (!add(&e, "search_document: ", texts[i], vec + i * (size_t)e.dim))
            return 1;
    if (!flush(&e))
        return 1;
    double t_docs = now_s() - t1;
    printf("memories: %zu embedded, %ld tokens (%ld truncated at %d), inference %.3f s, "
           "with tokenization %.3f s: %.1f memories/s, %.0f tokens/s (inference only)\n",
           n, e.tokens, e.truncated, MAXTOK, e.t_infer, t_docs, (double)n / t_docs,
           (double)e.tokens / e.t_infer);

    /* store under MODEL_NAME (replacing earlier vectors of this model) */
    xksm *x = NULL;
    if (xksm_open(argv[2], &x) != XKSM_OK)
        return 1;
    double t2 = now_s();
    size_t bad = 0;
    for (size_t i = 0; i < n; ++i)
        bad += xksm_vector_put(x, ids[i], argv[5], vec + i * (size_t)e.dim, (size_t)e.dim) != XKSM_OK;
    printf("stored %zu vectors as \"%s\" in %.3f s (%zu failed)\n", n - bad, argv[5],
           now_s() - t2, bad);
    xksm_close(x);

    /* queries */
    FILE *f = fopen(argv[3], "r"), *o = fopen(argv[4], "wb");
    if (!f || !o)
        return 1;
    char line[2048];
    size_t nq = 0;
    float *qv = malloc(sizeof(float) * (size_t)e.dim * 256);
    e.t_infer = 0;
    e.tokens = 0;
    double t3 = now_s();
    while (fgets(line, sizeof line, f) && nq < 256) {
        line[strcspn(line, "\r\n")] = 0;
        char *t1p = strchr(line, '\t'), *t2p = t1p ? strchr(t1p + 1, '\t') : NULL;
        if (!t1p || !t2p || line[0] == '#')
            continue;
        *t2p = 0;
        if (!add(&e, "search_query: ", t1p + 1, qv + nq * (size_t)e.dim))
            return 1;
        ++nq;
    }
    if (!flush(&e))
        return 1;
    fwrite(qv, sizeof(float) * (size_t)e.dim, nq, o);
    fclose(o);
    fclose(f);
    printf("queries: %zu embedded in %.3f s (inference %.3f s), written to %s\n", nq,
           now_s() - t3, e.t_infer, argv[4]);
    llama_batch_free(e.batch);
    llama_free(e.ctx);
    llama_model_free(model);
    llama_backend_free();
    return 0;
}
