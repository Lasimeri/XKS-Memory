/* test_hybrid.c: reciprocal rank fusion equals an independent recomputation from the two
 * component searches; explanation fields; single-list and zero-weight cases; deletion.
 * argv[1] = scratch store. */
#include "test.h"

#include <stdlib.h>
#include <string.h>

enum { N = 300, D = 32 };

static void remove_db(const char *p)
{
    char buf[1024];
    remove(p);
    snprintf(buf, sizeof buf, "%s-wal", p);
    remove(buf);
    snprintf(buf, sizeof buf, "%s-shm", p);
    remove(buf);
}

static uint64_t rng = 0x9e3779b97f4a7c15u;
static uint64_t next(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
}

typedef struct ref {
    xksm_id id;
    double s;
    size_t lr, vr;
} ref;

static int by_s(const void *a, const void *b)
{
    const ref *x = a, *y = b;
    if (x->s != y->s)
        return x->s < y->s ? 1 : -1;
    return x->id > y->id ? 1 : x->id < y->id ? -1 : 0;
}

int main(int argc, char **argv)
{
    if (argc != 2)
        return 2;
    remove_db(argv[1]);
    xksm *x = NULL;
    CHECK(xksm_open(argv[1], &x) == XKSM_OK);
    static float v[N][D];
    static xksm_id id[N];
    for (int i = 0; i < N; ++i) {
        char t[96];
        int o = snprintf(t, sizeof t, "m%03d", i);
        for (int w = 0; w < 5; ++w)
            o += snprintf(t + o, sizeof t - (size_t)o, " w%02d", (int)(next() % 25));
        xksm_memory_in in = {0};
        in.category = XKSM_CAT_SEMANTIC;
        in.text = t;
        in.text_len = (size_t)o;
        CHECK(xksm_memory_add(x, &in, &id[i]) == XKSM_OK);
        for (int j = 0; j < D; ++j)
            v[i][j] = (float)(next() % 1000) / 1000.0f - 0.5f;
        CHECK(xksm_vector_put(x, id[i], "e32", v[i], D) == XKSM_OK);
    }

    /* fused result == RRF recomputed from the component lists, 50 random queries */
    int diffs = 0, both = 0;
    for (int t = 0; t < 50; ++t) {
        char q[32];
        snprintf(q, sizeof q, "w%02d w%02d", (int)(next() % 25), (int)(next() % 25));
        const float *qv = v[next() % N];
        xksm_search_result L, V;
        xksm_search_opts so = {NULL, 0, XKSM_MATCH_ANY, 50};
        xksm_vector_opts vo = {NULL, 0, 50, 0};
        CHECK(xksm_search(x, q, &so, &L) == XKSM_OK);
        CHECK(xksm_vector_search(x, "e32", qv, D, &vo, &V) == XKSM_OK);
        ref r[100];
        size_t nr = 0;
        for (size_t i = 0; i < L.count; ++i)
            r[nr++] = (ref){L.hits[i].id, 1.0 / (60.0 + (double)(i + 1)), i + 1, 0};
        for (size_t i = 0; i < V.count; ++i) {
            size_t k = 0;
            while (k < nr && r[k].id != V.hits[i].id)
                ++k;
            if (k == nr)
                r[nr++] = (ref){V.hits[i].id, 0.0, 0, 0};
            r[k].s += 1.0 / (60.0 + (double)(i + 1));
            r[k].vr = i + 1;
        }
        qsort(r, nr, sizeof *r, by_s);
        xksm_hybrid_opts ho = {0};
        xksm_hybrid_result H;
        CHECK(xksm_hybrid_search(x, q, "e32", qv, D, &ho, &H) == XKSM_OK);
        size_t want = nr < 10 ? nr : 10;
        diffs += H.count != want;
        for (size_t i = 0; i < H.count && i < want; ++i) {
            diffs += H.hits[i].id != r[i].id || H.hits[i].score != r[i].s ||
                     H.hits[i].lexical_rank != r[i].lr || H.hits[i].vector_rank != r[i].vr;
            both += H.hits[i].lexical_rank && H.hits[i].vector_rank;
            diffs += (H.hits[i].lexical_rank > 0) != (H.hits[i].snippet != NULL);
        }
        xksm_hybrid_free(&H);
        xksm_search_free(&L);
        xksm_search_free(&V);
    }
    printf("hybrid vs recomputed RRF: %d differences over 50 queries (%d hits in both lists)\n",
           diffs, both);
    CHECK(diffs == 0);

    /* single list and zero weight: fused order equals that list's order */
    xksm_hybrid_result H;
    xksm_search_result L, V;
    xksm_search_opts so = {NULL, 0, XKSM_MATCH_ANY, 10};
    CHECK(xksm_search(x, "w03 w07", &so, &L) == XKSM_OK);
    xksm_hybrid_opts only_text = {0};
    only_text.w_lexical = 1.0; /* w_vector 0: vector list not run */
    CHECK(xksm_hybrid_search(x, "w03 w07", "e32", v[0], D, &only_text, &H) == XKSM_OK);
    int order_bad = H.count != L.count;
    for (size_t i = 0; i < H.count && i < L.count; ++i)
        order_bad += H.hits[i].id != L.hits[i].id || H.hits[i].vector_rank != 0;
    CHECK(order_bad == 0);
    xksm_hybrid_free(&H);
    xksm_search_free(&L);
    xksm_vector_opts vo = {NULL, 0, 10, 0};
    CHECK(xksm_vector_search(x, "e32", v[9], D, &vo, &V) == XKSM_OK);
    CHECK(xksm_hybrid_search(x, NULL, "e32", v[9], D, NULL, &H) == XKSM_OK);
    order_bad = H.count != V.count;
    for (size_t i = 0; i < H.count && i < V.count; ++i)
        order_bad += H.hits[i].id != V.hits[i].id || H.hits[i].lexical_rank != 0 ||
                     H.hits[i].snippet != NULL;
    CHECK(order_bad == 0 && H.count == 10 && H.hits[0].id == id[9]);
    xksm_hybrid_free(&H);
    xksm_search_free(&V);

    /* deleted memories are absent from both lists, hence from the fusion */
    CHECK(xksm_memory_delete(x, id[9]) == XKSM_OK);
    CHECK(xksm_hybrid_search(x, "m009", "e32", v[9], D, NULL, &H) == XKSM_OK);
    int gone = 1;
    for (size_t i = 0; i < H.count; ++i)
        gone &= H.hits[i].id != id[9];
    CHECK(gone);
    xksm_hybrid_free(&H);

    /* nothing to search */
    CHECK(xksm_hybrid_search(x, NULL, NULL, NULL, 0, NULL, &H) == XKSM_EINVAL);
    xksm_hybrid_opts neg = {0};
    neg.w_lexical = -1.0;
    CHECK(xksm_hybrid_search(x, "w01", NULL, NULL, 0, &neg, &H) == XKSM_EINVAL);

    xksm_close(x);
    remove_db(argv[1]);
    return test_report("hybrid");
}
