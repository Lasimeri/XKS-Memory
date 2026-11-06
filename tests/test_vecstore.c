/* test_vecstore.c: vector store: self-query, recall vs brute force, delete propagation,
 * replacement, dims, models, missing memories, persistence, filters, superseded state.
 * argv[1] = scratch store. */
#include "test.h"

#include <math.h>
#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>

enum { N = 600, D = 64 };
static float vec[N][D];
static xksm_id id[N];

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
static float uni(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return (float)((double)(rng >> 11) * (1.0 / 9007199254740992.0)) - 0.5f;
}

static double cosine(const float *a, const float *b)
{
    double ab = 0, aa = 0, bb = 0;
    for (int i = 0; i < D; ++i) {
        ab += (double)a[i] * b[i];
        aa += (double)a[i] * a[i];
        bb += (double)b[i] * b[i];
    }
    return ab / sqrt(aa * bb);
}

int main(int argc, char **argv)
{
    if (argc != 2)
        return 2;
    remove_db(argv[1]);
    xksm *x = NULL;
    CHECK(xksm_open(argv[1], &x) == XKSM_OK);
    int put_bad = 0;
    for (int i = 0; i < N; ++i) {
        char t[32];
        snprintf(t, sizeof t, "vector memory %d", i);
        xksm_memory_in in = {0};
        in.ns = i % 3 == 0 ? "a" : "b";
        in.category = i % 2 ? XKSM_CAT_EPISODIC : XKSM_CAT_SEMANTIC;
        in.text = t;
        in.text_len = strlen(t);
        CHECK(xksm_memory_add(x, &in, &id[i]) == XKSM_OK);
        int c = i % 20; /* 20 clusters */
        for (int j = 0; j < D; ++j)
            vec[i][j] = (j == c ? 3.0f : 0.0f) + uni();
        put_bad += xksm_vector_put(x, id[i], "m64", vec[i], D) != XKSM_OK;
    }
    CHECK(put_bad == 0);

    /* self-query: itself first, cosine about 1 */
    xksm_search_result r;
    xksm_vector_opts o = {NULL, 0, 10, 100};
    CHECK(xksm_vector_search(x, "m64", vec[123], D, &o, &r) == XKSM_OK);
    CHECK(r.count == 10 && r.hits[0].id == id[123] && fabs(r.hits[0].score - 1.0) < 1e-5);
    for (size_t i = 1; i < r.count; ++i)
        CHECK(r.hits[i - 1].score >= r.hits[i].score);
    xksm_search_free(&r);

    /* recall@10 against brute force over 50 random queries */
    int found = 0, total = 0;
    for (int t = 0; t < 50; ++t) {
        float q[D];
        for (int j = 0; j < D; ++j)
            q[j] = (j == t % 20 ? 3.0f : 0.0f) + uni();
        int best[10];
        double bs[10];
        for (int k = 0; k < 10; ++k)
            best[k] = -1, bs[k] = -2;
        for (int i = 0; i < N; ++i) {
            double s = cosine(q, vec[i]);
            for (int k = 0; k < 10; ++k)
                if (s > bs[k]) {
                    for (int m = 9; m > k; --m)
                        bs[m] = bs[m - 1], best[m] = best[m - 1];
                    bs[k] = s;
                    best[k] = i;
                    break;
                }
        }
        CHECK(xksm_vector_search(x, "m64", q, D, &o, &r) == XKSM_OK);
        for (size_t h = 0; h < r.count; ++h)
            for (int k = 0; k < 10; ++k)
                found += r.hits[h].id == id[best[k]];
        total += 10;
        xksm_search_free(&r);
    }
    printf("vector store recall@10 vs brute force: %.4f\n", (double)found / total);
    CHECK(found == total); /* rerank 100 of 600: exact on this data */

    /* delete propagates: the deleted memory never comes back */
    CHECK(xksm_memory_delete(x, id[123]) == XKSM_OK);
    CHECK(xksm_vector_search(x, "m64", vec[123], D, &o, &r) == XKSM_OK);
    int resurrected = 0;
    for (size_t h = 0; h < r.count; ++h)
        resurrected += r.hits[h].id == id[123];
    CHECK(resurrected == 0 && r.count == 10);
    xksm_search_free(&r);

    /* replace: memory 5 moves to cluster 7 */
    float moved[D];
    for (int j = 0; j < D; ++j)
        moved[j] = (j == 7 ? 3.0f : 0.0f);
    CHECK(xksm_vector_put(x, id[5], "m64", moved, D) == XKSM_OK);
    CHECK(xksm_vector_search(x, "m64", moved, D, &o, &r) == XKSM_OK);
    int hit5 = 0;
    for (size_t h = 0; h < r.count; ++h)
        hit5 += r.hits[h].id == id[5];
    CHECK(hit5 == 1);
    xksm_search_free(&r);

    /* errors and models */
    CHECK(xksm_vector_put(x, id[1], "m64", moved, D - 1) == XKSM_EINVAL);
    CHECK(xksm_vector_put(x, 999999, "m64", moved, D) == XKSM_ENOTFOUND);
    CHECK(xksm_vector_search(x, "m64", moved, D - 1, &o, &r) == XKSM_EINVAL);
    CHECK(xksm_vector_search(x, "other", moved, D, &o, &r) == XKSM_OK && r.count == 0);
    CHECK(xksm_vector_put(x, id[1], "small", moved, 8) == XKSM_OK); /* own dim per model */

    /* filters */
    xksm_vector_opts fa = {"a", 1u << XKSM_CAT_SEMANTIC, 10, 100};
    CHECK(xksm_vector_search(x, "m64", vec[60], D, &fa, &r) == XKSM_OK);
    int wrong = 0;
    for (size_t h = 0; h < r.count; ++h) {
        xksm_memory m;
        if (xksm_memory_get(x, r.hits[h].id, &m) != XKSM_OK) {
            ++wrong;
            continue;
        }
        wrong += strcmp(m.ns, "a") || m.category != XKSM_CAT_SEMANTIC;
        xksm_memory_free(&m);
    }
    CHECK(r.count == 10 && wrong == 0);
    xksm_search_free(&r);

    /* persistence + superseded exclusion */
    xksm_close(x);
    sqlite3 *db = NULL;
    CHECK(sqlite3_open(argv[1], &db) == SQLITE_OK);
    char sql[96];
    snprintf(sql, sizeof sql, "UPDATE memory SET state = 3 WHERE id = %lld", (long long)id[200]);
    CHECK(sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK);
    sqlite3_close(db);
    CHECK(xksm_open(argv[1], &x) == XKSM_OK);
    CHECK(xksm_vector_search(x, "m64", vec[200], D, &o, &r) == XKSM_OK);
    int sup = 0;
    for (size_t h = 0; h < r.count; ++h)
        sup += r.hits[h].id == id[200];
    CHECK(sup == 0 && r.count == 10);
    xksm_search_free(&r);
    CHECK(xksm_vector_search(x, "m64", vec[300], D, &o, &r) == XKSM_OK);
    CHECK(r.count == 10 && r.hits[0].id == id[300]);
    xksm_search_free(&r);

    xksm_close(x);
    remove_db(argv[1]);
    return test_report("vecstore");
}
