/* relate_llama.c: model-driven query expansion. An embedding model (llama.cpp GGUF) embeds
 * every content word of the store's vocabulary (semantic_vocab) once; each content word of
 * a query (known to the store or not) is embedded and mapped to its nearest vocabulary
 * words, which are appended to the query. Candidates are only the store's own words, so a
 * multilingual model cannot answer with translations. Writes the expanded ground-truth file
 * (same TAB format) for eval_retrieval and logs every expansion.
 * usage: relate_llama MODEL.gguf STORE QUERIES.tsv OUT.tsv [k=3] [min_cos=0.6] */
#define _POSIX_C_SOURCE 200809L /* clock_gettime under strict -std=c17 */
#include "llama.h"

#include <math.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { NSEQ = 256, MAXW = 48 };

static const char *const stop[] = {
    "the", "and", "for", "are", "was", "were", "with", "that", "this", "from", "into", "onto",
    "what", "which", "who", "whom", "whose", "when", "where", "why", "how", "can", "could",
    "should", "would", "will", "shall", "may", "might", "must", "does", "did", "done", "doing",
    "has", "have", "had", "having", "its", "his", "her", "hers", "their", "theirs", "our",
    "ours", "your", "yours", "mine", "you", "they", "them", "she", "him", "not", "but", "nor",
    "yet", "all", "any", "each", "every", "some", "such", "than", "then", "there", "here",
    "about", "above", "below", "after", "before", "over", "under", "again", "also", "just",
    "only", "very", "too", "more", "most", "less", "least", "other", "same", "own", "out",
    "off", "upon", "via", "per", "been", "being", "is", "am", "an", "as", "at", "be", "by",
    "do", "if", "in", "it", "me", "my", "no", "of", "on", "or", "so", "to", "up", "us", "we",
    "get", "got", "use", "used", "using", "one", "two", "new", "way", "may", "many", "much",
};

static int is_stop(const char *w)
{
    size_t n = strlen(w), digits = 0;
    if (n < 3)
        return 1;
    for (size_t i = 0; i < n; ++i)
        digits += w[i] >= '0' && w[i] <= '9';
    if (digits == n)
        return 1;
    for (size_t i = 0; i < sizeof stop / sizeof *stop; ++i)
        if (!strcmp(w, stop[i]))
            return 1;
    return 0;
}

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

typedef struct enc {
    struct llama_context *ctx;
    const struct llama_vocab *vocab;
    struct llama_batch batch;
    int dim, nseq;
    float *out[NSEQ];
    double t_infer;
} enc;

static int flush(enc *e)
{
    if (!e->nseq)
        return 1;
    double t = now_s();
    if (llama_encode(e->ctx, e->batch) != 0)
        return 0;
    for (int s = 0; s < e->nseq; ++s) {
        const float *v = llama_get_embeddings_seq(e->ctx, s);
        if (!v)
            return 0;
        double nr = 0;
        for (int d = 0; d < e->dim; ++d)
            nr += (double)v[d] * v[d];
        nr = nr > 0 ? 1.0 / sqrt(nr) : 0.0;
        for (int d = 0; d < e->dim; ++d)
            e->out[s][d] = (float)(v[d] * nr); /* unit length: dot = cosine */
    }
    e->t_infer += now_s() - t;
    e->batch.n_tokens = 0;
    e->nseq = 0;
    return 1;
}

static int add(enc *e, const char *word, float *out)
{
    llama_token tok[64];
    int n = llama_tokenize(e->vocab, word, (int32_t)strlen(word), tok, 64, true, false);
    if (n <= 0)
        return 0;
    if (e->batch.n_tokens + n > NSEQ * 8 || e->nseq == NSEQ)
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
    return 1;
}

int main(int argc, char **argv)
{
    if (argc < 5) {
        fprintf(stderr, "usage: %s MODEL.gguf STORE QUERIES.tsv OUT.tsv [k=3] [min_cos=0.6]\n",
                argv[0]);
        return 2;
    }
    int k = argc > 5 ? atoi(argv[5]) : 3;
    double min_cos = argc > 6 ? strtod(argv[6], NULL) : 0.6;
    if (k < 1 || k > 16)
        return 2;
    llama_backend_init();
    struct llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = -1;
    struct llama_model *model = llama_model_load_from_file(argv[1], mp);
    if (!model)
        return 1;
    struct llama_context_params cp = llama_context_default_params();
    cp.embeddings = true;
    cp.n_ctx = NSEQ * 8;
    cp.n_batch = NSEQ * 8;
    cp.n_ubatch = NSEQ * 8;
    cp.n_seq_max = NSEQ;
    enc e = {0};
    e.ctx = llama_init_from_model(model, cp);
    if (!e.ctx)
        return 1;
    e.vocab = llama_model_get_vocab(model);
    e.dim = llama_model_n_embd_out(model);
    e.batch = llama_batch_init(NSEQ * 8, 0, 1);

    /* candidate words: the store's vocabulary minus stop words, digits, short words */
    sqlite3 *db = NULL;
    sqlite3_stmt *st = NULL;
    if (sqlite3_open(argv[2], &db) != SQLITE_OK ||
        sqlite3_prepare_v2(db, "SELECT word FROM semantic_vocab ORDER BY id", -1, &st, NULL) !=
            SQLITE_OK)
        return 1;
    size_t cap = 1 << 15, nw = 0;
    char (*word)[MAXW] = malloc(sizeof *word * cap);
    while (sqlite3_step(st) == SQLITE_ROW) {
        const char *w = (const char *)sqlite3_column_text(st, 0);
        if (strlen(w) >= MAXW || is_stop(w))
            continue;
        if (nw == cap) {
            cap *= 2;
            word = realloc(word, sizeof *word * cap);
        }
        strcpy(word[nw++], w);
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    float *wv = malloc(sizeof(float) * (size_t)e.dim * nw);
    double t0 = now_s();
    for (size_t i = 0; i < nw; ++i)
        if (!add(&e, word[i], wv + i * (size_t)e.dim))
            memset(wv + i * (size_t)e.dim, 0, sizeof(float) * (size_t)e.dim);
    if (!flush(&e))
        return 1;
    printf("vocabulary table: %zu content words embedded in %.3f s (inference %.3f s)\n", nw,
           now_s() - t0, e.t_infer);

    FILE *in = fopen(argv[3], "r"), *out = fopen(argv[4], "w");
    if (!in || !out)
        return 1;
    char line[2048];
    float *qv = malloc(sizeof(float) * (size_t)e.dim);
    size_t nq = 0, added = 0;
    double tq = now_s();
    e.t_infer = 0;
    while (fgets(line, sizeof line, in)) {
        line[strcspn(line, "\r\n")] = 0;
        char *t1 = strchr(line, '\t'), *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
        if (!t1 || !t2 || line[0] == '#')
            continue;
        *t1 = *t2 = 0;
        char expanded[4096];
        snprintf(expanded, sizeof expanded, "%s", t1 + 1);
        char q[2048];
        snprintf(q, sizeof q, "%s", t1 + 1);
        printf("[%s] %s ->", line, t1 + 1);
        for (char *w = strtok(q, " \t,.?!;:()\"'"); w; w = strtok(NULL, " \t,.?!;:()\"'")) {
            char lw[MAXW];
            size_t n = 0;
            for (; w[n] && n < MAXW - 1; ++n)
                lw[n] = (w[n] >= 'A' && w[n] <= 'Z') ? (char)(w[n] - 'A' + 'a') : w[n];
            lw[n] = 0;
            if (is_stop(lw))
                continue;
            if (!add(&e, lw, qv) || !flush(&e))
                continue;
            size_t best[16];
            double bs[16];
            for (int i = 0; i < k; ++i)
                bs[i] = -2;
            for (size_t i = 0; i < nw; ++i) {
                if (!strcmp(word[i], lw))
                    continue;
                const float *v = wv + i * (size_t)e.dim;
                double c = 0;
                for (int d = 0; d < e.dim; ++d)
                    c += (double)qv[d] * v[d];
                for (int t = 0; t < k; ++t)
                    if (c > bs[t]) {
                        for (int u = k - 1; u > t; --u) {
                            bs[u] = bs[u - 1];
                            best[u] = best[u - 1];
                        }
                        bs[t] = c;
                        best[t] = i;
                        break;
                    }
            }
            for (int t = 0; t < k; ++t)
                if (bs[t] >= min_cos) {
                    strncat(expanded, " ", sizeof expanded - strlen(expanded) - 1);
                    strncat(expanded, word[best[t]], sizeof expanded - strlen(expanded) - 1);
                    printf(" %s:%s(%.2f)", lw, word[best[t]], bs[t]);
                    ++added;
                }
        }
        printf("\n");
        fprintf(out, "%s\t%s\t%s\n", line, expanded, t2 + 1);
        ++nq;
    }
    fclose(in);
    fclose(out);
    printf("queries: %zu expanded with %zu related words in %.3f s (inference %.3f s)\n", nq,
           added, now_s() - tq, e.t_infer);
    llama_batch_free(e.batch);
    llama_free(e.ctx);
    llama_model_free(model);
    llama_backend_free();
    return 0;
}
