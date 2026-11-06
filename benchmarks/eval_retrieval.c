/* eval_retrieval.c: retrieval quality against a ground-truth query set.
 * Query file: one query per line, TAB-separated: CLASS <TAB> QUERY <TAB> EXPECTED[,EXPECTED...]
 * where EXPECTED is a source name: the file name of an imported source without directory
 * and without ".md". Hits are collapsed to distinct sources in rank order; a query scores
 * at the first rank holding an expected source. Reports recall@1/@5/@10 and MRR per class.
 * usage: eval_retrieval STORE QUERIES.tsv [any|all|vec|hybrid]
 *   vec: semantic model vectors only; hybrid: reciprocal rank fusion of ANY + vec.
 * usage: eval_retrieval STORE QUERIES.tsv qvec|qhybrid QVEC_FILE MODEL DIM
 *   precomputed query vectors (float32[DIM] per query, file order) searched under MODEL. */
#include "xksm.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { MAXC = 8 };
typedef struct cls {
    char name[16];
    int n, r1, r5, r10;
    double mrr;
} cls;

/* basename without ".md" */
static void source_name(const char *uri, char *out, size_t cap)
{
    const char *b = strrchr(uri, '/');
    b = b ? b + 1 : uri;
    size_t n = strlen(b);
    if (n > 3 && !strcmp(b + n - 3, ".md"))
        n -= 3;
    if (n >= cap)
        n = cap - 1;
    memcpy(out, b, n);
    out[n] = 0;
}

static int expected(const char *list, const char *name)
{
    size_t k = strlen(name);
    for (const char *p = list; *p;) {
        const char *e = strchr(p, ',');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (n == k && !strncmp(p, name, k))
            return 1;
        if (!e)
            break;
        p = e + 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 3)
        return 2;
    const char *ms = argc > 3 ? argv[3] : "any";
    int use_vec = !strcmp(ms, "vec"), use_hyb = !strcmp(ms, "hybrid");
    /* xhybrid: lexical side uses QUERIES.tsv, vector side embeds the same line of argv[4]
     * (e.g. a model-expanded copy of the queries) */
    FILE *xf = NULL;
    if (!strcmp(ms, "xhybrid")) {
        if (argc < 5 || !(xf = fopen(argv[4], "r")))
            return 2;
        use_hyb = 1;
    }
    int use_q = !strcmp(ms, "qvec") || !strcmp(ms, "qhybrid");
    const char *qmodel = XKSM_SEMANTIC_MODEL;
    float *qfile = NULL;
    size_t qdim = 0, qn = 0, qi = 0;
    if (use_q) {
        if (argc < 7)
            return 2;
        qmodel = argv[5];
        qdim = (size_t)strtoul(argv[6], NULL, 10);
        FILE *qf = fopen(argv[4], "rb");
        if (!qf || !qdim)
            return 1;
        fseek(qf, 0, SEEK_END);
        qn = (size_t)ftell(qf) / (sizeof(float) * qdim);
        fseek(qf, 0, SEEK_SET);
        qfile = malloc(sizeof(float) * qdim * (qn ? qn : 1));
        if (!qfile || fread(qfile, sizeof(float) * qdim, qn, qf) != qn)
            return 1;
        fclose(qf);
        use_vec = !strcmp(ms, "qvec");
        use_hyb = !strcmp(ms, "qhybrid");
    }
    xksm_match mode = !strcmp(ms, "all") ? XKSM_MATCH_ALL : XKSM_MATCH_ANY;
    xksm *x = NULL;
    sqlite3 *db = NULL;
    sqlite3_stmt *src = NULL;
    if (xksm_open(argv[1], &x) != XKSM_OK || sqlite3_open(argv[1], &db) != SQLITE_OK ||
        sqlite3_prepare_v2(db,
                           "SELECT s.uri FROM memory m JOIN source s ON s.id = m.source_id "
                           "WHERE m.id = ?1",
                           -1, &src, NULL) != SQLITE_OK)
        return 1;
    FILE *f = fopen(argv[2], "r");
    if (!f)
        return 1;
    cls c[MAXC];
    memset(c, 0, sizeof c);
    int nc = 0;
    char line[2048];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *t1 = strchr(line, '\t'), *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
        if (!t1 || !t2 || line[0] == '#')
            continue;
        *t1 = *t2 = 0;
        const char *klass = line, *query = t1 + 1, *want = t2 + 1;
        int ci = 0;
        while (ci < nc && strcmp(c[ci].name, klass))
            ++ci;
        if (ci == nc) {
            if (nc == MAXC)
                continue;
            snprintf(c[nc++].name, sizeof c[0].name, "%.15s", klass); /* truncation intended */
        }
        xksm_search_opts o = {NULL, 0, mode, 60};
        xksm_search_result r = {0};
        xksm_status qs = XKSM_OK;
        if (use_vec || use_hyb) {
            size_t dim = use_q ? qdim : xksm_semantic_dim(x);
            float *qv = dim ? malloc(sizeof(float) * dim) : NULL;
            int have = 0;
            if (qv && use_q && qi < qn) {
                memcpy(qv, qfile + qi * qdim, sizeof(float) * qdim);
                have = 1;
            } else if (qv && !use_q && xf) {
                char xl[4096];
                const char *vt = query;
                while (fgets(xl, sizeof xl, xf)) {
                    xl[strcspn(xl, "\r\n")] = 0;
                    char *x1 = strchr(xl, (int)0x09), *x2 = x1 ? strchr(x1 + 1, (int)0x09) : NULL;
                    if (!x1 || !x2 || xl[0] == (char)0x23)
                        continue;
                    *x2 = 0;
                    vt = x1 + 1;
                    break;
                }
                have = xksm_semantic_embed(x, vt, strlen(vt), qv, dim) == XKSM_OK;
            } else if (qv && !use_q)
                have = xksm_semantic_embed(x, query, strlen(query), qv, dim) == XKSM_OK;
            ++qi;
            if (use_vec) {
                xksm_vector_opts vo = {NULL, 0, 60, 0};
                qs = have ? xksm_vector_search(x, qmodel, qv, dim, &vo, &r) : XKSM_EINVAL;
            } else {
                xksm_hybrid_opts ho = {0};
                ho.limit = 60;
                xksm_hybrid_result h;
                qs = xksm_hybrid_search(x, query, have ? qmodel : NULL, have ? qv : NULL,
                                        have ? dim : 0, &ho, &h);
                if (qs == XKSM_OK) { /* reuse the search-result shape: ids in fused order */
                    r.hits = calloc(h.count + 1, sizeof *r.hits);
                    for (size_t i = 0; r.hits && i < h.count; ++i)
                        r.hits[i].id = h.hits[i].id;
                    r.count = r.hits ? h.count : 0;
                    xksm_hybrid_free(&h);
                }
            }
            free(qv);
        } else {
            qs = xksm_search(x, query, &o, &r);
        }
        int rank = 0;
        if (qs == XKSM_OK) {
            char seen[10][128];
            int ns = 0;
            for (size_t i = 0; i < r.count && ns < 10 && !rank; ++i) {
                sqlite3_reset(src);
                sqlite3_bind_int64(src, 1, r.hits[i].id);
                if (sqlite3_step(src) != SQLITE_ROW)
                    continue;
                char name[128];
                source_name((const char *)sqlite3_column_text(src, 0), name, sizeof name);
                int dup = 0;
                for (int s = 0; s < ns; ++s)
                    dup |= !strcmp(seen[s], name);
                if (dup)
                    continue;
                strcpy(seen[ns++], name);
                if (expected(want, name))
                    rank = ns;
            }
            xksm_search_free(&r);
        }
        c[ci].n++;
        c[ci].r1 += rank == 1;
        c[ci].r5 += rank >= 1 && rank <= 5;
        c[ci].r10 += rank >= 1 && rank <= 10;
        c[ci].mrr += rank ? 1.0 / rank : 0.0;
    }
    fclose(f);
    printf("mode %s\n", ms);
    for (int i = 0; i < nc; ++i)
        printf("%-6s %3d queries  recall@1 %.3f  recall@5 %.3f  recall@10 %.3f  MRR %.3f\n",
               c[i].name, c[i].n, (double)c[i].r1 / c[i].n, (double)c[i].r5 / c[i].n,
               (double)c[i].r10 / c[i].n, c[i].mrr / c[i].n);
    sqlite3_finalize(src);
    sqlite3_close(db);
    xksm_close(x);
    return 0;
}
