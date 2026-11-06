/* lexical.c: FTS5 retrieval with BM25 ranking, namespace / category / state filters and
 * snippets. User terms are quoted so query text cannot inject FTS5 operators. */
#include "internal.h"

#include <stdlib.h>
#include <string.h>

static int space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

/* Build the FTS5 expression: "term1" OP "term2" ...; a trailing '*' becomes a prefix
 * query ("term"*). Embedded double quotes are doubled. Returns malloc'd string or NULL;
 * *nterms receives the term count. */
/* English function words: in ANY mode they match nearly every memory (FTS5 then ranks
 * all of them) while adding almost no BM25 weight (idf near 0). Dropped from ANY queries
 * that also have other terms; ALL and RAW queries are taken as written. No length rule:
 * short technical terms (rm, db, io) are kept. */
static int stopword(const char *s, size_t n)
{
    static const char *const w[] = {
        "a", "an", "the", "and", "or", "but", "if", "of", "in", "on", "at", "to",
        "for", "from", "by", "with", "about", "as", "into", "is", "are", "was", "were",
        "be", "been", "being", "am", "do", "does", "did", "have", "has", "had", "how",
        "what", "which", "who", "whom", "why", "when", "where", "this", "that", "these",
        "those", "it", "its", "i", "me", "my", "we", "our", "us", "you", "your", "he",
        "she", "they", "them", "their", "his", "her", "there", "here", "so", "than",
        "then", "too", "very", "can", "could", "should", "would", "will", "just", "all",
        "any", "some", "each", "also", "only", "again", "more", "most", "other"};
    while (n && !((*s >= 0x61 && *s <= 0x7A) || (*s >= 0x41 && *s <= 0x5A) || (*s >= 0x30 && *s <= 0x39)))
        ++s, --n; /* surrounding punctuation */
    while (n && !((s[n - 1] >= 0x61 && s[n - 1] <= 0x7A) || (s[n - 1] >= 0x41 && s[n - 1] <= 0x5A) ||
                  (s[n - 1] >= 0x30 && s[n - 1] <= 0x39)))
        --n;
    if (!n || n > 6)
        return 0;
    char l[8];
    for (size_t i = 0; i < n; ++i)
        l[i] = (s[i] >= 0x41 && s[i] <= 0x5A) ? (char)(s[i] + 32) : s[i];
    for (size_t i = 0; i < sizeof w / sizeof *w; ++i)
        if (strlen(w[i]) == n && !memcmp(l, w[i], n))
            return 1;
    return 0;
}

static char *build(const char *q, xksm_match m, int *nterms)
{
    /* ANY: drop function words when the query has any other term */
    int drop = 0;
    if (m == XKSM_MATCH_ANY && !getenv("XKSM_KEEP_STOPWORDS")) {
        size_t n0 = strlen(q);
        for (size_t i = 0; i < n0 && !drop;) {
            while (i < n0 && space(q[i]))
                ++i;
            size_t s0 = i;
            while (i < n0 && !space(q[i]))
                ++i;
            drop = i > s0 && !stopword(q + s0, i - s0);
        }
    }
    size_t n = strlen(q);
    /* bound: every char doubled (2n) + per term 2 quotes, a star, a 5-byte " AND " (8) */
    char *out = malloc(2 * n + 8 * (size_t)XKSM_MAX_TERMS + 1);
    if (!out)
        return NULL;
    size_t o = 0;
    int terms = 0;
    for (size_t i = 0; i < n;) {
        while (i < n && space(q[i]))
            ++i;
        if (i >= n)
            break;
        size_t s = i;
        while (i < n && !space(q[i]))
            ++i;
        size_t e = i;
        int prefix = e - s > 1 && q[e - 1] == '*';
        if (prefix)
            --e;
        if (e == s || (e - s == 1 && q[s] == (char)0x2A) || (drop && !prefix && stopword(q + s, e - s)))
            continue;
        if (++terms > XKSM_MAX_TERMS)
            break;
        if (terms > 1) {
            const char *op = m == XKSM_MATCH_ALL ? " AND " : " OR ";
            memcpy(out + o, op, strlen(op));
            o += strlen(op);
        }
        out[o++] = '"';
        for (size_t k = s; k < e; ++k) {
            if (q[k] == '"')
                out[o++] = '"';
            out[o++] = q[k];
        }
        out[o++] = '"';
        if (prefix)
            out[o++] = '*';
    }
    out[o] = 0;
    *nterms = terms > XKSM_MAX_TERMS ? XKSM_MAX_TERMS : terms;
    return out;
}

xksm_status xksm_search(xksm *x, const char *query, const xksm_search_opts *opts,
                        xksm_search_result *out)
{
    if (!x || !query || !out)
        return XKSM_EINVAL;
    memset(out, 0, sizeof *out);
    xksm_search_opts o = {0};
    if (opts)
        o = *opts;
    size_t limit = o.limit ? o.limit : 10;
    if (limit > 10000)
        limit = 10000;
    size_t qlen = strlen(query);
    if (qlen > XKSM_MAX_QUERY)
        return xksm__fail(x, XKSM_EINVAL, "query is %zu bytes; limit %d", qlen, XKSM_MAX_QUERY);
    if (!xksm__utf8_valid((const uint8_t *)query, qlen))
        return xksm__fail(x, XKSM_EINVAL, "query is not valid UTF-8");

    char *expr;
    int terms = 1;
    if (o.match == XKSM_MATCH_RAW) {
        expr = malloc(qlen + 1);
        if (expr)
            memcpy(expr, query, qlen + 1);
    } else if (o.match == XKSM_MATCH_ANY || o.match == XKSM_MATCH_ALL) {
        expr = build(query, o.match, &terms);
    } else {
        return xksm__fail(x, XKSM_EINVAL, "match mode %d unknown", (int)o.match);
    }
    if (!expr)
        return XKSM_ENOMEM;
    if (terms == 0 || !expr[0]) {
        free(expr);
        return xksm__fail(x, XKSM_EINVAL, "query has no terms");
    }

    /* Two stages. (1) rank in FTS5 alone, top W rows; (2) filter those rows by point
     * lookups; widen W fourfold if too few survive while more matches exist. Joining every
     * match to `memory` cost about 0.8 us per matching row (5,879 rows: 4 to 5 ms). Snippets
     * are built only for the final hits. */
    xksm_status rc = XKSM_OK;
    sqlite3_stmt *rank = NULL, *meta = NULL, *snip = NULL;
    out->hits = calloc(limit, sizeof *out->hits);
    if (!out->hits) {
        rc = XKSM_ENOMEM;
        goto done;
    }
    if (sqlite3_prepare_v2(x->db,
                           "SELECT rowid, -bm25(memory_fts) FROM memory_fts WHERE memory_fts "
                           "MATCH ?1 ORDER BY bm25(memory_fts), rowid LIMIT ?2",
                           -1, &rank, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(x->db, "SELECT state, namespace, category FROM memory WHERE id = ?1",
                           -1, &meta, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(x->db,
                           "SELECT snippet(memory_fts, 0, '[', ']', '...', 16) FROM memory_fts "
                           "WHERE memory_fts MATCH ?1 AND rowid = ?2",
                           -1, &snip, NULL) != SQLITE_OK ||
        sqlite3_bind_text(rank, 1, expr, -1, SQLITE_STATIC) != SQLITE_OK ||
        sqlite3_bind_text(snip, 1, expr, -1, SQLITE_STATIC) != SQLITE_OK) {
        rc = xksm__fail(x, XKSM_EIO, "search: %s", sqlite3_errmsg(x->db));
        goto done;
    }
    for (size_t w = limit * 4 < 64 ? 64 : limit * 4;; w *= 4) {
        size_t got = 0;
        out->count = 0;
        sqlite3_reset(rank);
        sqlite3_bind_int64(rank, 2, (sqlite3_int64)w);
        int step;
        while (out->count < limit && (step = sqlite3_step(rank)) == SQLITE_ROW) {
            ++got;
            xksm_id id = sqlite3_column_int64(rank, 0);
            double score = sqlite3_column_double(rank, 1);
            sqlite3_bind_int64(meta, 1, id);
            if (sqlite3_step(meta) == SQLITE_ROW) {
                const char *ns = (const char *)sqlite3_column_text(meta, 1);
                int cat = sqlite3_column_int(meta, 2);
                if (sqlite3_column_int(meta, 0) == XKSM_STATE_ACTIVE &&
                    (!o.ns || (ns && !strcmp(ns, o.ns))) &&
                    (!o.categories || (cat >= 0 && cat < 32 && (o.categories >> cat) & 1u)))
                    out->hits[out->count++] = (xksm_search_hit){id, score, NULL};
            }
            sqlite3_reset(meta);
        }
        if (out->count < limit && step != SQLITE_DONE) { /* RAW: FTS5 syntax error */
            rc = xksm__fail(x, o.match == XKSM_MATCH_RAW ? XKSM_EINVAL : XKSM_EIO,
                            "search: %s", sqlite3_errmsg(x->db));
            out->count = 0;
            goto done;
        }
        if (out->count == limit || got < w)
            break; /* enough hits, or every match was examined */
    }
    for (size_t i = 0; i < out->count; ++i) {
        sqlite3_reset(snip);
        sqlite3_bind_int64(snip, 2, out->hits[i].id);
        const char *sn = sqlite3_step(snip) == SQLITE_ROW
                             ? (const char *)sqlite3_column_text(snip, 0)
                             : NULL;
        size_t sl = sn ? strlen(sn) : 0;
        out->hits[i].snippet = malloc(sl + 1);
        if (!out->hits[i].snippet) {
            rc = XKSM_ENOMEM;
            break;
        }
        memcpy(out->hits[i].snippet, sn ? sn : "", sl + 1);
    }
done:
    sqlite3_finalize(rank);
    sqlite3_finalize(meta);
    sqlite3_finalize(snip);
    free(expr);
    if (rc != XKSM_OK)
        xksm_search_free(out);
    return rc;
}

void xksm_search_free(xksm_search_result *r)
{
    if (!r)
        return;
    for (size_t i = 0; i < r->count; ++i)
        free(r->hits[i].snippet);
    free(r->hits);
    r->hits = NULL;
    r->count = 0;
}
