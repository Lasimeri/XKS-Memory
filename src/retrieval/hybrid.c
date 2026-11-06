/* hybrid.c: reciprocal rank fusion of lexical and vector results. */
#include "internal.h"

#include <stdlib.h>
#include <string.h>

static int by_fused(const void *a, const void *b)
{
    const xksm_hybrid_hit *x = a, *y = b;
    if (x->score != y->score)
        return x->score < y->score ? 1 : -1;
    return x->id > y->id ? 1 : x->id < y->id ? -1 : 0; /* deterministic ties */
}

static xksm_hybrid_hit *find_or_add(xksm_hybrid_hit *h, size_t *n, xksm_id id)
{
    for (size_t i = 0; i < *n; ++i) /* at most 2 x depth entries: linear is fine */
        if (h[i].id == id)
            return &h[i];
    xksm_hybrid_hit *e = &h[(*n)++];
    memset(e, 0, sizeof *e);
    e->id = id;
    return e;
}

xksm_status xksm_hybrid_search(xksm *x, const char *text, const char *model, const float *q,
                               size_t dim, const xksm_hybrid_opts *opts,
                               xksm_hybrid_result *out)
{
    if (!x || !out)
        return XKSM_EINVAL;
    memset(out, 0, sizeof *out);
    xksm_hybrid_opts o = {0};
    if (opts)
        o = *opts;
    size_t limit = o.limit ? o.limit : 10, depth = o.depth ? o.depth : 50;
    double k = o.k > 0 ? o.k : 60.0;
    double wl = o.w_lexical, wv = o.w_vector;
    if (wl == 0.0 && wv == 0.0)
        wl = wv = 1.0;
    if (wl < 0 || wv < 0)
        return xksm__fail(x, XKSM_EINVAL, "negative RRF weight");
    if (depth < limit)
        depth = limit;
    int use_text = text && text[0] && wl > 0, use_vec = model && q && dim && wv > 0;
    if (!use_text && !use_vec)
        return xksm__fail(x, XKSM_EINVAL, "hybrid search needs text or a query vector");

    xksm_search_result lex = {0}, vec = {0};
    xksm_status rc;
    if (use_text) {
        xksm_search_opts so = {o.ns, o.categories, o.match, depth};
        if ((rc = xksm_search(x, text, &so, &lex)) != XKSM_OK)
            return rc;
    }
    if (use_vec) {
        xksm_vector_opts vo = {o.ns, o.categories, depth, 0};
        if ((rc = xksm_vector_search(x, model, q, dim, &vo, &vec)) != XKSM_OK) {
            xksm_search_free(&lex);
            return rc;
        }
    }
    xksm_hybrid_hit *h = calloc(lex.count + vec.count + 1, sizeof *h);
    if (!h) {
        xksm_search_free(&lex);
        xksm_search_free(&vec);
        return XKSM_ENOMEM;
    }
    size_t n = 0;
    for (size_t i = 0; i < lex.count; ++i) {
        xksm_hybrid_hit *e = find_or_add(h, &n, lex.hits[i].id);
        e->lexical_rank = i + 1;
        e->lexical_score = lex.hits[i].score;
        e->score += wl / (k + (double)(i + 1));
        e->snippet = lex.hits[i].snippet; /* ownership moves to the hybrid result */
        lex.hits[i].snippet = NULL;
    }
    for (size_t i = 0; i < vec.count; ++i) {
        xksm_hybrid_hit *e = find_or_add(h, &n, vec.hits[i].id);
        e->vector_rank = i + 1;
        e->vector_score = vec.hits[i].score;
        e->score += wv / (k + (double)(i + 1));
    }
    xksm_search_free(&lex);
    xksm_search_free(&vec);
    qsort(h, n, sizeof *h, by_fused);
    for (size_t i = limit; i < n; ++i) /* drop the tail */
        free(h[i].snippet);
    out->hits = h;
    out->count = n < limit ? n : limit;
    return XKSM_OK;
}

void xksm_hybrid_free(xksm_hybrid_result *r)
{
    if (!r)
        return;
    for (size_t i = 0; i < r->count; ++i)
        free(r->hits[i].snippet);
    free(r->hits);
    r->hits = NULL;
    r->count = 0;
}
