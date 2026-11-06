/* test_lexical.c: FTS5 retrieval: match modes, identifiers, prefixes, filters, state
 * exclusion, ranking, snippets, RAW errors, hostile input. argv[1] = scratch store. */
#include "test.h"

#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>

static void remove_db(const char *p)
{
    char buf[1024];
    remove(p);
    snprintf(buf, sizeof buf, "%s-wal", p);
    remove(buf);
    snprintf(buf, sizeof buf, "%s-shm", p);
    remove(buf);
}

static xksm_id add(xksm *x, const char *ns, xksm_category cat, const char *t)
{
    xksm_memory_in in = {0};
    in.ns = ns;
    in.category = cat;
    in.text = t;
    in.text_len = strlen(t);
    xksm_id id = 0;
    return xksm_memory_add(x, &in, &id) == XKSM_OK ? id : 0;
}

/* run a search; returns hit count or -1 on error; ids[] receives ids in rank order */
static int run(xksm *x, const char *q, xksm_match m, const char *ns, uint32_t cats,
               xksm_id *ids, xksm_search_result *keep)
{
    xksm_search_opts o = {ns, cats, m, 10};
    xksm_search_result r;
    if (xksm_search(x, q, &o, &r) != XKSM_OK)
        return -1;
    for (size_t i = 0; i < r.count && ids; ++i)
        ids[i] = r.hits[i].id;
    int n = (int)r.count;
    if (keep)
        *keep = r;
    else
        xksm_search_free(&r);
    return n;
}

static int has(const xksm_id *ids, int n, xksm_id id)
{
    for (int i = 0; i < n; ++i)
        if (ids[i] == id)
            return 1;
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 2)
        return 2;
    remove_db(argv[1]);
    xksm *x = NULL;
    CHECK(xksm_open(argv[1], &x) == XKSM_OK);
    xksm_id a = add(x, NULL, XKSM_CAT_SEMANTIC, "build libxksm.so with sm_86 on the 3090 Ti");
    xksm_id b = add(x, NULL, XKSM_CAT_PROCEDURAL, "run ctest after every build");
    xksm_id c = add(x, "proj", XKSM_CAT_SEMANTIC, "the 3090 Ti has 24 GB of GDDR6X");
    xksm_id d = add(x, NULL, XKSM_CAT_SEMANTIC, "ctest must pass under ThreadSanitizer");
    xksm_id e = add(x, NULL, XKSM_CAT_EPISODIC, "deleted memory secretword");
    xksm_id f = add(x, NULL, XKSM_CAT_SEMANTIC, "old fact supersededmarker");
    CHECK(a && b && c && d && e && f);
    CHECK(xksm_memory_delete(x, e) == XKSM_OK);
    xksm_close(x);
    sqlite3 *db = NULL; /* supersede f the way consolidation will: state = 3 */
    CHECK(sqlite3_open(argv[1], &db) == SQLITE_OK);
    char sql[96];
    snprintf(sql, sizeof sql, "UPDATE memory SET state = 3 WHERE id = %lld", (long long)f);
    CHECK(sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK);
    sqlite3_close(db);
    CHECK(xksm_open(argv[1], &x) == XKSM_OK);

    xksm_id ids[16];
    int n;
    n = run(x, "3090 ctest", XKSM_MATCH_ANY, NULL, 0, ids, NULL);
    CHECK(n == 4 && has(ids, n, a) && has(ids, n, b) && has(ids, n, c) && has(ids, n, d));
    CHECK(run(x, "3090 ctest", XKSM_MATCH_ALL, NULL, 0, ids, NULL) == 0);
    n = run(x, "3090 Ti", XKSM_MATCH_ALL, NULL, 0, ids, NULL);
    CHECK(n == 2 && has(ids, n, a) && has(ids, n, c));
    n = run(x, "libxksm.so", XKSM_MATCH_ANY, NULL, 0, ids, NULL);
    CHECK(n == 1 && ids[0] == a);
    n = run(x, "sm_86", XKSM_MATCH_ANY, NULL, 0, ids, NULL);
    CHECK(n == 1 && ids[0] == a);
    n = run(x, "Thread*", XKSM_MATCH_ANY, NULL, 0, ids, NULL);
    CHECK(n == 1 && ids[0] == d);

    /* filters */
    n = run(x, "3090", XKSM_MATCH_ANY, "proj", 0, ids, NULL);
    CHECK(n == 1 && ids[0] == c);
    n = run(x, "ctest", XKSM_MATCH_ANY, NULL, 1u << XKSM_CAT_PROCEDURAL, ids, NULL);
    CHECK(n == 1 && ids[0] == b);

    /* deleted and superseded memories are never returned */
    CHECK(run(x, "secretword", XKSM_MATCH_ANY, NULL, 0, ids, NULL) == 0);
    CHECK(run(x, "supersededmarker", XKSM_MATCH_ANY, NULL, 0, ids, NULL) == 0);

    /* ranking: the memory matching most terms first; scores non-increasing; snippets */
    xksm_search_result r;
    n = run(x, "3090 Ti 24 GB", XKSM_MATCH_ANY, NULL, 0, ids, &r);
    CHECK(n == 2 && r.hits[0].id == c && r.hits[0].score >= r.hits[1].score);
    xksm_search_free(&r);
    n = run(x, "ctest", XKSM_MATCH_ANY, NULL, 1u << XKSM_CAT_PROCEDURAL, ids, &r);
    CHECK(n == 1 && r.hits[0].snippet && strstr(r.hits[0].snippet, "[ctest]") != NULL);
    xksm_search_free(&r);

    /* RAW: FTS5 syntax is honoured, errors are reported, not crashed on */
    n = run(x, "3090 NOT ctest", XKSM_MATCH_RAW, NULL, 0, ids, NULL);
    CHECK(n == 2 && has(ids, n, a) && has(ids, n, c));
    xksm_search_opts raw = {NULL, 0, XKSM_MATCH_RAW, 10};
    CHECK(xksm_search(x, "ctest AND NOT", &raw, &r) == XKSM_EINVAL);
    CHECK(r.count == 0 && r.hits == NULL);

    /* hostile input in quoted modes: operators and quotes are plain text */
    const char *hostile[] = {"NEAR(a b) \" - OR AND (", "\"", "ctest\" OR \"3090", "*", "-ctest",
                             "{text}: ctest", "a\"\"b"};
    int errors = 0;
    for (size_t i = 0; i < sizeof hostile / sizeof *hostile; ++i) {
        xksm_search_opts any = {NULL, 0, XKSM_MATCH_ANY, 10};
        xksm_status s = xksm_search(x, hostile[i], &any, &r);
        errors += s != XKSM_OK && s != XKSM_EINVAL;
        if (s == XKSM_OK)
            xksm_search_free(&r);
    }
    CHECK(errors == 0);
    /* the injected OR inside one term stays inside quotes: no extra matches */
    n = run(x, "ctest\" OR \"3090", XKSM_MATCH_ALL, NULL, 0, ids, NULL);
    CHECK(n == 0);

    /* limits and validation */
    xksm_search_opts any = {NULL, 0, XKSM_MATCH_ANY, 10};
    CHECK(xksm_search(x, "   ", &any, &r) == XKSM_EINVAL);
    CHECK(xksm_search(x, "bad \xC0\x80", &any, &r) == XKSM_EINVAL);
    char many[1024] = {0};
    for (int i = 0; i < 80; ++i)
        strcat(many, i == 79 ? "ctest " : "zz ");
    CHECK(xksm_search(x, many, &any, &r) == XKSM_OK); /* 80 terms: first 64 used */
    CHECK(r.count == 0);                               /* "ctest" was term 80 */
    xksm_search_free(&r);

    /* two-stage search == full-join reference, under heavy filtering (forces widening) */
    xksm_close(x);
    remove_db(argv[1]);
    CHECK(xksm_open(argv[1], &x) == XKSM_OK);
    uint64_t rr = 0x2545F4914F6CDD1Du;
#define NEXT() (rr ^= rr << 13, rr ^= rr >> 7, rr ^= rr << 17, rr)
    static const char *const nss[3] = {"n0", "n1", "n2"};
    enum { NM = 3000 };
    {
        static xksm_memory_in in[NM];
        static char txt[NM][160];
        static xksm_id mid[NM];
        static xksm_status mst[NM];
        for (int i = 0; i < NM; ++i) {
            int o = snprintf(txt[i], sizeof txt[i], "u%05d", i);
            int words = 6 + (int)(NEXT() % 7);
            for (int w = 0; w < words; ++w)
                o += snprintf(txt[i] + o, sizeof txt[i] - (size_t)o, " w%02d", (int)(NEXT() % 40));
            in[i].ns = nss[NEXT() % 3];
            in[i].category = (xksm_category)(NEXT() % 6);
            in[i].text = txt[i];
            in[i].text_len = (size_t)o;
        }
        CHECK(xksm_memory_add_batch(x, in, NM, mid, mst) == XKSM_OK);
    }
    xksm_close(x);
    CHECK(sqlite3_open(argv[1], &db) == SQLITE_OK);
    CHECK(sqlite3_exec(db, "UPDATE memory SET state = 3 + (id % 3) WHERE id % 5 < 2", NULL, NULL,
                       NULL) == SQLITE_OK); /* 40 % superseded / archived / expired */
    CHECK(xksm_open(argv[1], &x) == XKSM_OK);
    sqlite3_stmt *ref = NULL;
    CHECK(sqlite3_prepare_v2(
              db,
              "SELECT m.id, -bm25(memory_fts) FROM memory_fts JOIN memory m ON m.id = "
              "memory_fts.rowid WHERE memory_fts MATCH ?1 AND m.state = 1 AND (?2 IS NULL OR "
              "m.namespace = ?2) AND (?3 = 0 OR ((1 << m.category) & ?3) != 0) "
              "ORDER BY bm25(memory_fts), m.id LIMIT ?4",
              -1, &ref, NULL) == SQLITE_OK);
    int diffs = 0, widened_cases = 0;
    for (int t = 0; t < 300; ++t) {
        char q[64], fq[96];
        int nw = 1 + (int)(NEXT() % 3), o = 0, fo = 0;
        xksm_match m = NEXT() % 2 ? XKSM_MATCH_ALL : XKSM_MATCH_ANY;
        for (int w = 0; w < nw; ++w) {
            int k = (int)(NEXT() % 40);
            o += snprintf(q + o, sizeof q - (size_t)o, "%sw%02d", w ? " " : "", k);
            fo += snprintf(fq + fo, sizeof fq - (size_t)fo, "%s\"w%02d\"",
                           w ? (m == XKSM_MATCH_ALL ? " AND " : " OR ") : "", k);
        }
        const char *ns = NEXT() % 2 ? nss[NEXT() % 3] : NULL;
        uint32_t cats = NEXT() % 2 ? (uint32_t)(1 + NEXT() % 63) : 0;
        size_t lim = 1 + (size_t)(NEXT() % 30);
        xksm_search_opts so = {ns, cats, m, lim};
        xksm_search_result got;
        if (xksm_search(x, q, &so, &got) != XKSM_OK) {
            ++diffs;
            continue;
        }
        sqlite3_reset(ref);
        sqlite3_bind_text(ref, 1, fq, -1, SQLITE_TRANSIENT);
        ns ? sqlite3_bind_text(ref, 2, ns, -1, SQLITE_STATIC) : sqlite3_bind_null(ref, 2);
        sqlite3_bind_int64(ref, 3, cats);
        sqlite3_bind_int64(ref, 4, (sqlite3_int64)lim);
        size_t k = 0;
        while (sqlite3_step(ref) == SQLITE_ROW) {
            if (k >= got.count || got.hits[k].id != sqlite3_column_int64(ref, 0) ||
                got.hits[k].score != sqlite3_column_double(ref, 1))
                ++diffs;
            ++k;
        }
        diffs += k != got.count;
        widened_cases += ns && cats && got.count == lim; /* filters keep under 1/6 of rows */
        xksm_search_free(&got);
    }
    sqlite3_finalize(ref);
    sqlite3_close(db);
    printf("two-stage vs full join: %d differences over 300 queries (%d heavily filtered full "
           "pages)\n", diffs, widened_cases);
    CHECK(diffs == 0);

    xksm_close(x);
    remove_db(argv[1]);
    return test_report("lexical");
}
