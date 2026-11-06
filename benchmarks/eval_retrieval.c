/* eval_retrieval.c: retrieval quality against a ground-truth query set.
 * Query file: one query per line, TAB-separated: CLASS <TAB> QUERY <TAB> EXPECTED[,EXPECTED...]
 * where EXPECTED is a source name: the file name of an imported source without directory
 * and without ".md". Hits are collapsed to distinct sources in rank order; a query scores
 * at the first rank holding an expected source. Reports recall@1/@5/@10 and MRR per class.
 * usage: eval_retrieval STORE QUERIES.tsv [any|all] */
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
    xksm_match mode = argc > 3 && !strcmp(argv[3], "all") ? XKSM_MATCH_ALL : XKSM_MATCH_ANY;
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
        xksm_search_result r;
        int rank = 0;
        if (xksm_search(x, query, &o, &r) == XKSM_OK) {
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
    printf("mode %s\n", mode == XKSM_MATCH_ALL ? "ALL" : "ANY");
    for (int i = 0; i < nc; ++i)
        printf("%-6s %3d queries  recall@1 %.3f  recall@5 %.3f  recall@10 %.3f  MRR %.3f\n",
               c[i].name, c[i].n, (double)c[i].r1 / c[i].n, (double)c[i].r5 / c[i].n,
               (double)c[i].r10 / c[i].n, c[i].mrr / c[i].n);
    sqlite3_finalize(src);
    sqlite3_close(db);
    xksm_close(x);
    return 0;
}
