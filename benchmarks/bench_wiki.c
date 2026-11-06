/* bench_wiki.c: import a MediaWiki XML dump slice (pages-articles) and time it.
 * Keeps namespace-0 pages that are not redirects; decodes XML entities; imports each
 * article as its own source "PREFIX:Title" through xksm_import_buffer. A page cut off at
 * the end of the slice is ignored. Parsing and importing are timed separately.
 * usage: bench_wiki DUMP.xml STORE [uri_prefix=enwiki] [max_articles=0 (all)] */
#define _POSIX_C_SOURCE 200809L /* clock_gettime under strict -std=c17 */
#include "xksm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

/* bounded substring search in [p, end) */
static const char *find(const char *p, const char *end, const char *s)
{
    size_t n = strlen(s);
    for (; p + n <= end; ++p) {
        p = memchr(p, s[0], (size_t)(end - p));
        if (!p || p + n > end)
            return NULL;
        if (!memcmp(p, s, n))
            return p;
    }
    return NULL;
}

static size_t put_utf8(char *o, unsigned long cp)
{
    if (cp < 0x80) { o[0] = (char)cp; return 1; }
    if (cp < 0x800) { o[0] = (char)(0xC0 | cp >> 6); o[1] = (char)(0x80 | (cp & 63)); return 2; }
    if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD;
    if (cp < 0x10000) {
        o[0] = (char)(0xE0 | cp >> 12); o[1] = (char)(0x80 | (cp >> 6 & 63));
        o[2] = (char)(0x80 | (cp & 63)); return 3;
    }
    if (cp > 0x10FFFF) cp = 0xFFFD;
    if (cp >= 0x10000 && cp <= 0x10FFFF) {
        o[0] = (char)(0xF0 | cp >> 18); o[1] = (char)(0x80 | (cp >> 12 & 63));
        o[2] = (char)(0x80 | (cp >> 6 & 63)); o[3] = (char)(0x80 | (cp & 63)); return 4;
    }
    return put_utf8(o, 0xFFFD);
}

/* decode XML entities of [s, e) into out (decoded text is never longer); returns length */
static size_t decode(const char *s, const char *e, char *out)
{
    size_t o = 0;
    while (s < e) {
        if (*s != '&') { out[o++] = *s++; continue; }
        const char *semi = memchr(s, ';', (size_t)(e - s) < 12 ? (size_t)(e - s) : 12);
        if (!semi) { out[o++] = *s++; continue; }
        size_t n = (size_t)(semi - s);
        if (n == 3 && !memcmp(s, "&lt", 3)) out[o++] = '<';
        else if (n == 3 && !memcmp(s, "&gt", 3)) out[o++] = '>';
        else if (n == 4 && !memcmp(s, "&amp", 4)) out[o++] = '&';
        else if (n == 5 && !memcmp(s, "&quot", 5)) out[o++] = '"';
        else if (n == 5 && !memcmp(s, "&apos", 5)) out[o++] = '\'';
        else if (n > 2 && s[1] == '#') {
            unsigned long cp = s[2] == 'x' ? strtoul(s + 3, NULL, 16) : strtoul(s + 2, NULL, 10);
            o += put_utf8(out + o, cp);
        } else { out[o++] = *s++; continue; } /* unknown: keep the '&' literally */
        s = semi + 1;
    }
    return o;
}

int main(int argc, char **argv)
{
    if (argc < 3)
        return 2;
    const char *prefix = argc > 3 ? argv[3] : "enwiki";
    long max = argc > 4 ? strtol(argv[4], NULL, 10) : 0;
    FILE *f = fopen(argv[1], "rb");
    if (!f)
        return 1;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)size + 1);
    if (!buf || fread(buf, 1, (size_t)size, f) != (size_t)size)
        return 1;
    fclose(f);
    const char *end = buf + size;

    /* pass 1: parse + decode every article into one arena (timed alone) */
    typedef struct art { size_t title_off, title_len, text_off, text_len; } art;
    size_t cap = 1 << 16, n = 0, arena_used = 0;
    art *a = malloc(cap * sizeof *a);
    char *arena = malloc((size_t)size);
    long pages = 0, skipped_ns = 0, redirects = 0, cut = 0;
    const char *stop = buf; /* end of the last complete page parsed */
    double t0 = now_s();
    for (const char *p = buf; (p = find(p, end, "<page>"));) {
        const char *pe = find(p, end, "</page>");
        if (!pe) { ++cut; break; }
        stop = pe + 7;
        ++pages;
        const char *ns = find(p, pe, "<ns>");
        if (!ns || strncmp(ns + 4, "0<", 2)) { ++skipped_ns; p = pe; continue; }
        if (find(p, pe, "<redirect")) { ++redirects; p = pe; continue; }
        const char *ts = find(p, pe, "<title>"), *te = ts ? find(ts, pe, "</title>") : NULL;
        const char *xs = find(p, pe, "<text"), *xb = xs ? memchr(xs, '>', (size_t)(pe - xs)) : NULL;
        const char *xe = xb ? find(xb, pe, "</text>") : NULL;
        if (!te || !xe || xb[-1] == '/') { p = pe; continue; } /* empty <text/> */
        if (n == cap) {
            cap *= 2;
            a = realloc(a, cap * sizeof *a);
            if (!a) return 1;
        }
        a[n].title_off = arena_used;
        a[n].title_len = decode(ts + 7, te, arena + arena_used);
        arena_used += a[n].title_len;
        a[n].text_off = arena_used;
        a[n].text_len = decode(xb + 1, xe, arena + arena_used);
        arena_used += a[n].text_len;
        ++n;
        p = pe;
        if (max && (long)n >= max)
            break;
    }
    double parse_s = now_s() - t0;
    printf("slice %ld bytes: %ld pages, %ld other namespaces, %ld redirects, %ld cut at the end; "
           "%zu articles, %.1f MB decoded text; parse %.3f s (%.1f MB/s)\n",
           size, pages, skipped_ns, redirects, cut, n,
           (double)(arena_used) / 1e6, parse_s, (double)(stop - buf) / parse_s / 1e6);

    /* pass 2: import (timed alone) */
    xksm *x = NULL;
    if (xksm_open(argv[2], &x) != XKSM_OK) {
        fprintf(stderr, "open: %s\n", xksm_errmsg(x));
        return 1;
    }
    xksm_import_opts o = {0};
    o.category = XKSM_CAT_SEMANTIC;
    o.format = XKSM_FMT_TEXT;
    size_t chunks = 0, added = 0, dups = 0, refused = 0, text_bytes = 0;
    char uri[512];
    t0 = now_s();
    for (size_t i = 0; i < n; ++i) {
        int ul = snprintf(uri, sizeof uri, "%s:%.*s", prefix, (int)a[i].title_len,
                          arena + a[i].title_off);
        (void)ul;
        xksm_import_result r;
        xksm_status s = xksm_import_buffer(x, uri, arena + a[i].text_off, a[i].text_len, &o, &r);
        if (s == XKSM_OK) {
            chunks += r.chunks;
            added += r.added;
            dups += r.duplicates;
            text_bytes += a[i].text_len;
        } else if (s != XKSM_EXISTS) {
            ++refused;
            if (refused <= 3)
                fprintf(stderr, "refused: %s\n", xksm_errmsg(x));
        }
    }
    double import_s = now_s() - t0;
    xksm_cache_info ci;
    xksm_cache_stats(x, &ci);
    xksm_close(x);
    printf("import: %zu articles (%zu refused), %.1f MB text, %zu chunks, %zu new memories, "
           "%zu duplicate chunks; %.3f s: %.2f MB/s, %.0f memories/s; store now %lld memories\n",
           n - refused, refused, (double)text_bytes / 1e6, chunks, added, dups, import_s,
           (double)text_bytes / import_s / 1e6, (double)added / import_s, (long long)ci.records);
    free(buf);
    free(arena);
    free(a);
    return 0;
}
