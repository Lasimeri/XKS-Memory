/* test_agents.c: the library side of agent-harness memory: kv state, the session registry
 * and forgetting a session, import header + event time, the mapped vector-index snapshot
 * (results equal to a store without snapshots: after a full snapshot, after appends to the
 * tail, after a destructive replace; files removed on delete), the fold-in watermark, and
 * recall without a word-vector table. argv[1] = scratch database path. */
#define _POSIX_C_SOURCE 200809L /* setenv */
#include "test.h"

#include <dirent.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static void remove_db(const char *p)
{
    char buf[2048];
    remove(p);
    snprintf(buf, sizeof buf, "%s-wal", p);
    remove(buf);
    snprintf(buf, sizeof buf, "%s-shm", p);
    remove(buf);
    snprintf(buf, sizeof buf, "%s.ptgt", p);
    remove(buf);
    /* snapshots: <path>.vidx-<hash> */
    const char *slash = strrchr(p, '/');
    char dir[1024], pre[512];
    snprintf(dir, sizeof dir, "%.*s", slash ? (int)(slash - p) : 1, slash ? p : ".");
    snprintf(pre, sizeof pre, "%s.vidx-", slash ? slash + 1 : p);
    DIR *d = opendir(dir);
    struct dirent *e;
    while (d && (e = readdir(d)))
        if (!strncmp(e->d_name, pre, strlen(pre))) {
            snprintf(buf, sizeof buf, "%s/%s", dir, e->d_name);
            remove(buf);
        }
    if (d)
        closedir(d);
}

static int snapshots(const char *p)
{
    const char *slash = strrchr(p, '/');
    char dir[1024], pre[512];
    snprintf(dir, sizeof dir, "%.*s", slash ? (int)(slash - p) : 1, slash ? p : ".");
    snprintf(pre, sizeof pre, "%s.vidx-", slash ? slash + 1 : p);
    int n = 0;
    DIR *d = opendir(dir);
    struct dirent *e;
    while (d && (e = readdir(d)))
        n += !strncmp(e->d_name, pre, strlen(pre));
    if (d)
        closedir(d);
    return n;
}

static uint64_t rng = 0x853c49e6748fea9bu;
static float unif(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return (float)((double)(rng >> 11) * (1.0 / 9007199254740992.0)) - 0.5f;
}

enum { DIM = 32 };

/* the same query against snapshot and no-snapshot handles must give the same hits */
static int same_results(const char *path, const float *q)
{
    xksm *a = NULL, *b = NULL;
    xksm_search_result ra, rb;
    xksm_vector_opts vo = {NULL, 0, 25, 0};
    int same = 0;
    if (xksm_open(path, &a) == XKSM_OK &&
        xksm_vector_search(a, "m", q, DIM, &vo, &ra) == XKSM_OK) { /* snapshot path */
        setenv("XKSM_NO_VIDX", "1", 1);
        if (xksm_open(path, &b) == XKSM_OK &&
            xksm_vector_search(b, "m", q, DIM, &vo, &rb) == XKSM_OK) {
            same = ra.count == rb.count && ra.count > 0;
            for (size_t i = 0; same && i < ra.count; ++i)
                same = ra.hits[i].id == rb.hits[i].id && fabs(ra.hits[i].score - rb.hits[i].score) < 1e-6;
            xksm_search_free(&rb);
        }
        unsetenv("XKSM_NO_VIDX");
        xksm_search_free(&ra);
    }
    xksm_close(a);
    xksm_close(b);
    return same;
}

int main(int argc, char **argv)
{
    if (argc != 2)
        return 2;
    const char *P = argv[1];
    remove_db(P);
    xksm *x = NULL;
    CHECK(xksm_open(P, &x) == XKSM_OK);
    CHECK(xksm_schema_version(x) == 4);

    /* kv */
    char v[64];
    size_t vl = 0;
    int64_t c = 0;
    CHECK(xksm_kv_get(x, "k", v, sizeof v, &vl) == XKSM_ENOTFOUND);
    CHECK(xksm_kv_put(x, "k", "value-1", 7) == XKSM_OK);
    CHECK(xksm_kv_get(x, "k", v, sizeof v, &vl) == XKSM_OK && vl == 7 && !memcmp(v, "value-1", 7));
    CHECK(xksm_kv_get(x, "k", v, 3, &vl) == XKSM_OK && vl == 7 && !memcmp(v, "val", 3));
    CHECK(xksm_kv_add(x, "n", 5, &c) == XKSM_OK && c == 5);
    CHECK(xksm_kv_add(x, "n", -2, &c) == XKSM_OK && c == 3);
    CHECK(xksm_kv_delete(x, "n") == XKSM_OK && xksm_kv_get(x, "n", v, sizeof v, &vl) == XKSM_ENOTFOUND);

    /* import header and event time on every chunk; source info */
    {
        static char big[6000];
        size_t k = 0;
        for (int p = 0; p < 12; ++p)
            k += (size_t)snprintf(big + k, sizeof big - k, "paragraph %d %s\n\n", p,
                                  "alpha beta gamma delta epsilon zeta eta theta iota kappa lambda mu "
                                  "nu xi omicron pi rho sigma tau upsilon phi chi psi omega");
        xksm_import_opts o = {0};
        o.category = XKSM_CAT_EPISODIC;
        o.chunk_target = 256;
        o.event_us = 1700000000123456;
        o.header = "HDR line\n";
        xksm_import_result r;
        CHECK(xksm_import_buffer(x, "xksm-turn://h1/s1/user/1", big, k, &o, &r) == XKSM_OK);
        CHECK(r.chunks > 2 && r.added == r.chunks);
        int all = 1;
        for (xksm_id id = 1; id <= (xksm_id)r.added; ++id) {
            xksm_memory m;
            if (xksm_memory_get(x, id, &m) != XKSM_OK) {
                all = 0;
                continue;
            }
            all &= !strncmp(m.text, "HDR line\n", 9) && m.event_us == 1700000000123456;
            xksm_memory_free(&m);
        }
        CHECK(all);
        char uri[128];
        int64_t imp = 0;
        CHECK(xksm_source_info(x, r.source_id, uri, sizeof uri, &imp) == XKSM_OK &&
              !strcmp(uri, "xksm-turn://h1/s1/user/1") && imp > 0);
        CHECK(xksm_source_info(x, 999, uri, sizeof uri, &imp) == XKSM_ENOTFOUND);
        o.header = "";
        CHECK(xksm_import_buffer(x, "u2", "x", 1, &o, &r) == XKSM_OK);
    }

    /* sessions: touch, list order, per harness, forget removes memories and sources */
    {
        CHECK(xksm_session_touch(x, "h1", "s1", "/w", 100) == XKSM_OK);
        CHECK(xksm_session_touch(x, "h1", "s1", NULL, 50) == XKSM_OK);
        CHECK(xksm_session_touch(x, "h1", "s2", NULL, 300) == XKSM_OK);
        CHECK(xksm_session_touch(x, "h2", "s1", NULL, 200) == XKSM_OK);
        CHECK(xksm_session_touch(x, "", "s1", NULL, 1) == XKSM_EINVAL);
        xksm_session_info *l = NULL;
        size_t n = 0;
        CHECK(xksm_session_list(x, NULL, 0, &l, &n) == XKSM_OK && n == 3);
        CHECK(n == 3 && !strcmp(l[0].id, "s2") && !strcmp(l[1].harness, "h2"));
        CHECK(n == 3 && l[2].first_us == 50 && l[2].last_us == 100 && l[2].turns == 2 &&
              l[2].cwd && !strcmp(l[2].cwd, "/w"));
        xksm_session_list_free(l, n);
        CHECK(xksm_session_list(x, "h1", 1, &l, &n) == XKSM_OK && n == 1 && !strcmp(l[0].id, "s2"));
        xksm_session_list_free(l, n);
        xksm_import_opts o = {0};
        o.category = XKSM_CAT_EPISODIC;
        o.ns = "h2";
        xksm_import_result r;
        CHECK(xksm_import_buffer(x, "xksm-turn://h2/s1/user/5", "other harness text", 18, &o, &r) == XKSM_OK);
        xksm_cache_info ci;
        xksm_cache_stats(x, &ci);
        int64_t before = ci.records;
        size_t del = 0;
        CHECK(xksm_session_forget(x, "h1", "s1", &del) == XKSM_OK && del > 2);
        xksm_cache_stats(x, &ci);
        CHECK(ci.records == before - (int64_t)del);
        CHECK(xksm_source_info(x, 1, NULL, 0, NULL) == XKSM_ENOTFOUND); /* h1/s1 source gone */
        CHECK(xksm_session_list(x, "h1", 0, &l, &n) == XKSM_OK && n == 1); /* s2 stays */
        xksm_session_list_free(l, n);
        CHECK(xksm_session_list(x, "h2", 0, &l, &n) == XKSM_OK && n == 1); /* other harness stays */
        xksm_session_list_free(l, n);
        xksm_search_result sr;
        CHECK(xksm_search(x, "harness", NULL, &sr) == XKSM_OK && sr.count == 1);
        xksm_search_free(&sr);
    }
    xksm_close(x);
    remove_db(P);

    /* vector snapshot: equal results with and without it, through appends, a destructive
     * replace and a delete */
    CHECK(xksm_open(P, &x) == XKSM_OK);
    enum { N = 3000 };
    static float vec[N + 400][DIM];
    for (int i = 0; i < N + 400; ++i)
        for (int d = 0; d < DIM; ++d)
            vec[i][d] = unif();
    xksm_id ids[N + 400];
    for (int i = 0; i < N + 400; ++i) {
        char t[32];
        int tl = snprintf(t, sizeof t, "memory %d", i);
        xksm_memory_in in = {0};
        in.category = XKSM_CAT_SEMANTIC;
        in.text = t;
        in.text_len = (size_t)tl;
        CHECK(xksm_memory_add(x, &in, &ids[i]) == XKSM_OK);
    }
    for (int i = 0; i < N; ++i)
        xksm_vector_put(x, ids[i], "m", vec[i], DIM);
    float q[DIM];
    for (int d = 0; d < DIM; ++d)
        q[d] = unif();
    xksm_search_result r0;
    xksm_vector_opts vo = {NULL, 0, 25, 0};
    CHECK(xksm_vector_search(x, "m", q, DIM, &vo, &r0) == XKSM_OK); /* full load, writes it */
    xksm_search_free(&r0);
    CHECK(snapshots(P) == 1);
    CHECK(same_results(P, q)); /* mapped snapshot */
    for (int i = N; i < N + 300; ++i) /* appends: tail of 300 rows */
        CHECK(xksm_vector_put(x, ids[i], "m", vec[i], DIM) == XKSM_OK);
    CHECK(snapshots(P) == 1); /* appends do not rewrite or remove it */
    float q2[DIM];
    memcpy(q2, vec[N + 10], sizeof q2); /* a vector that lives in the tail */
    CHECK(same_results(P, q2));
    {
        xksm_search_result rt;
        CHECK(xksm_vector_search(x, "m", q2, DIM, &vo, &rt) == XKSM_OK && rt.count > 0 &&
              rt.hits[0].id == ids[N + 10]);
        xksm_search_free(&rt);
    }
    CHECK(xksm_vector_put(x, ids[5], "m", vec[N + 350], DIM) == XKSM_OK); /* replace: destructive */
    CHECK(snapshots(P) == 0);
    float q3[DIM];
    memcpy(q3, vec[N + 350], sizeof q3);
    CHECK(same_results(P, q3)); /* rewritten on load, the replaced row found */
    {
        xksm_search_result rt;
        CHECK(xksm_vector_search(x, "m", q3, DIM, &vo, &rt) == XKSM_OK && rt.count > 0 &&
              rt.hits[0].id == ids[5]);
        xksm_search_free(&rt);
    }
    CHECK(snapshots(P) == 1);
    CHECK(xksm_memory_delete(x, ids[5]) == XKSM_OK); /* delete: snapshot removed at once */
    CHECK(snapshots(P) == 0);
    {
        xksm_search_result rt;
        CHECK(xksm_vector_search(x, "m", q3, DIM, &vo, &rt) == XKSM_OK);
        int gone = 1;
        for (size_t i = 0; i < rt.count; ++i)
            gone &= rt.hits[i].id != ids[5];
        CHECK(gone);
        xksm_search_free(&rt);
    }
    CHECK(same_results(P, q));
    xksm_close(x);
    remove_db(P);

    /* recall without a word-vector table: lexical alone, no error; fold-in watermark */
    CHECK(xksm_open(P, &x) == XKSM_OK);
    {
        xksm_memory_in in = {0};
        in.category = XKSM_CAT_EPISODIC;
        in.text = "the gamma placeholder note";
        in.text_len = strlen(in.text);
        xksm_id id;
        CHECK(xksm_memory_add(x, &in, &id) == XKSM_OK);
        xksm_hybrid_result hr;
        CHECK(xksm_semantic_recall(x, "gamma", NULL, NULL, &hr) == XKSM_OK && hr.count == 1);
        xksm_hybrid_free(&hr);
        CHECK(xksm_semantic_update(x, NULL) == XKSM_ENOTFOUND);
        /* a table: memories m%04d drawn from 3 topic pools */
        for (int m = 0; m < 400; ++m) {
            char t[512];
            int o = snprintf(t, sizeof t, "m%04d", m);
            for (int w = 0; w < 25; ++w)
                o += snprintf(t + o, sizeof t - (size_t)o, " t%dw%02d", m % 3, (int)((rng = rng * 6364136223846793005u + 1) >> 59));
            in.text = t;
            in.text_len = (size_t)o;
            CHECK(xksm_memory_add(x, &in, &id) == XKSM_OK);
        }
        xksm_semantic_opts so = {0};
        so.dim = 16;
        xksm_semantic_report rep;
        CHECK(xksm_semantic_build(x, &so, &rep) == XKSM_OK);
        CHECK(xksm_semantic_built_memories(x) == 401);
        size_t n = 99;
        CHECK(xksm_semantic_update(x, &n) == XKSM_OK && n == 0); /* all embedded by the build */
        in.text = "t1w03 t1w07 new";
        in.text_len = strlen(in.text);
        CHECK(xksm_memory_add(x, &in, &id) == XKSM_OK);
        CHECK(xksm_semantic_update(x, &n) == XKSM_OK && n == 1); /* only the new one */
        CHECK(xksm_semantic_update(x, &n) == XKSM_OK && n == 0); /* watermark: nothing again */
        CHECK(xksm_kv_get(x, "semantic:embedded_upto", v, sizeof v - 1, &vl) == XKSM_OK);
        v[vl] = 0;
        CHECK(atoll(v) == id);
    }
    xksm_close(x);
    remove_db(P);
    return test_report("agents");
}
