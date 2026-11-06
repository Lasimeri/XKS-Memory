/* distill_llama.c: precompute a static token-vector table from a llama.cpp embedding model
 * (Model2Vec-style distillation): every vocabulary token is run through the model once, alone
 * between the BOS and EOS tokens, and the model's own pooled output is kept. Control and
 * unknown tokens get a zero vector. Output: float32[n_vocab][dim], row = token id.
 * usage: distill_llama MODEL.gguf OUT_TABLE */
#define _POSIX_C_SOURCE 200809L /* clock_gettime under strict -std=c17 */
#include "llama.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { NSEQ = 256 };

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s MODEL.gguf OUT_TABLE\n", argv[0]);
        return 2;
    }
    llama_backend_init();
    struct llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = -1;
    struct llama_model *model = llama_model_load_from_file(argv[1], mp);
    if (!model)
        return 1;
    struct llama_context_params cp = llama_context_default_params();
    cp.embeddings = true;
    cp.n_ctx = NSEQ * 3;
    cp.n_batch = NSEQ * 3;
    cp.n_ubatch = NSEQ * 3;
    cp.n_seq_max = NSEQ;
    struct llama_context *ctx = llama_init_from_model(model, cp);
    if (!ctx)
        return 1;
    const struct llama_vocab *vocab = llama_model_get_vocab(model);
    const int32_t nv = llama_vocab_n_tokens(vocab);
    const int dim = llama_model_n_embd_out(model);
    const llama_token bos = llama_vocab_bos(vocab), eos = llama_vocab_eos(vocab);
    FILE *out = fopen(argv[2], "wb");
    float *buf = calloc((size_t)NSEQ * (size_t)dim, sizeof *buf);
    int32_t *ids = malloc(sizeof *ids * NSEQ);
    struct llama_batch batch = llama_batch_init(NSEQ * 3, 0, 1);
    if (!out || !buf || !ids)
        return 1;
    printf("vocab %d, dim %d, pooling %d, bos %d eos %d\n", nv, dim, (int)llama_pooling_type(ctx),
           bos, eos);
    double t0 = now_s(), t_inf = 0;
    long run = 0, zero = 0;
    for (int32_t base = 0; base < nv; base += NSEQ) {
        int32_t n = nv - base < NSEQ ? nv - base : NSEQ;
        batch.n_tokens = 0;
        int ns = 0;
        for (int32_t k = 0; k < n; ++k) {
            llama_token t = base + k;
            if (llama_vocab_is_control(vocab, t) || llama_vocab_get_attr(vocab, t) & LLAMA_TOKEN_ATTR_UNKNOWN)
                continue;
            llama_token seq[3] = {bos, t, eos};
            for (int i = 0; i < 3; ++i) {
                int j = batch.n_tokens++;
                batch.token[j] = seq[i];
                batch.pos[j] = i;
                batch.n_seq_id[j] = 1;
                batch.seq_id[j][0] = ns;
                batch.logits[j] = 1;
            }
            ids[ns++] = k;
        }
        memset(buf, 0, sizeof *buf * (size_t)n * (size_t)dim);
        if (ns) {
            double t = now_s();
            if (llama_encode(ctx, batch) != 0) {
                fprintf(stderr, "llama_encode failed at token %d\n", base);
                return 1;
            }
            for (int s = 0; s < ns; ++s) {
                const float *v = llama_get_embeddings_seq(ctx, s);
                if (!v)
                    return 1;
                memcpy(buf + (size_t)ids[s] * (size_t)dim, v, sizeof *buf * (size_t)dim);
            }
            t_inf += now_s() - t;
        }
        run += ns;
        zero += n - ns;
        if (fwrite(buf, sizeof *buf * (size_t)dim, (size_t)n, out) != (size_t)n)
            return 1;
    }
    fclose(out);
    double total = now_s() - t0;
    printf("distilled %ld tokens (%ld control/unknown left zero) in %.3f s (inference %.3f s): "
           "%.0f tokens/s; table %d x %d f32 -> %s\n",
           run, zero, total, t_inf, (double)run / total, nv, dim, argv[2]);
    llama_batch_free(batch);
    llama_free(ctx);
    llama_model_free(model);
    llama_backend_free();
    return 0;
}
