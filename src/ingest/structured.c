/* structured.c: JSON (RFC 8259), JSONL (one JSON value per line) and CSV (RFC 4180, header
 * row) parsed into records, each rendered as text lines "path: value" for indexing.
 * Strict: any syntax error fails the whole parse with its byte offset. */
#include "structured.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ---- growable byte buffer ------------------------------------------------------- */
typedef struct sbuf {
    char *p;
    size_t n, cap;
} sbuf;

static int sb_put(sbuf *s, const char *d, size_t n)
{
    if (s->n + n > s->cap) {
        size_t cap = s->cap ? s->cap : 256;
        while (cap < s->n + n)
            cap *= 2;
        char *p = realloc(s->p, cap);
        if (!p)
            return 0;
        s->p = p;
        s->cap = cap;
    }
    if (n)
        memcpy(s->p + s->n, d, n);
    s->n += n;
    return 1;
}

static int sb_utf8(sbuf *s, uint32_t c)
{
    char e[4];
    size_t n;
    if (c < 0x80) {
        e[0] = (char)c;
        n = 1;
    } else if (c < 0x800) {
        e[0] = (char)(0xC0 | (c >> 6));
        e[1] = (char)(0x80 | (c & 0x3F));
        n = 2;
    } else if (c < 0x10000) {
        e[0] = (char)(0xE0 | (c >> 12));
        e[1] = (char)(0x80 | ((c >> 6) & 0x3F));
        e[2] = (char)(0x80 | (c & 0x3F));
        n = 3;
    } else {
        e[0] = (char)(0xF0 | (c >> 18));
        e[1] = (char)(0x80 | ((c >> 12) & 0x3F));
        e[2] = (char)(0x80 | ((c >> 6) & 0x3F));
        e[3] = (char)(0x80 | (c & 0x3F));
        n = 4;
    }
    return sb_put(s, e, n);
}

/* ---- JSON ------------------------------------------------------------------------ */
typedef struct jp {
    const char *b;
    size_t n, i;
    int depth;
    sbuf path, *out;
    /* walk mode (xksm__json_walk): every leaf goes to cb instead of out */
    xksm_json_cb cb;
    void *ctx;
    sbuf tmp;
    const char *err;
    size_t err_off;
} jp;

enum { J_OK = 1, J_SYNTAX = 0, J_OOM = -1 };

static int jfail(jp *p, const char *msg)
{
    if (!p->err) {
        p->err = msg;
        p->err_off = p->i;
    }
    return J_SYNTAX;
}

static void jws(jp *p)
{
    while (p->i < p->n && (p->b[p->i] == ' ' || p->b[p->i] == '\t' || p->b[p->i] == '\n' ||
                           p->b[p->i] == '\r'))
        ++p->i;
}

static int hex4(jp *p, uint32_t *v)
{
    if (p->n - p->i < 4)
        return jfail(p, "truncated \\u escape");
    uint32_t r = 0;
    for (int k = 0; k < 4; ++k) {
        char c = p->b[p->i++];
        uint32_t d = (c >= '0' && c <= '9')   ? (uint32_t)(c - '0')
                     : (c >= 'a' && c <= 'f') ? (uint32_t)(c - 'a' + 10)
                     : (c >= 'A' && c <= 'F') ? (uint32_t)(c - 'A' + 10)
                                              : 16u;
        if (d == 16)
            return jfail(p, "bad hex digit in \\u escape");
        r = r << 4 | d;
    }
    *v = r;
    return J_OK;
}

/* string at p->i (the opening quote), unescaped into dst (may be NULL: validate only) */
static int jstring(jp *p, sbuf *dst)
{
    ++p->i;
    for (;;) {
        size_t s = p->i;
        while (p->i < p->n && p->b[p->i] != '"' && p->b[p->i] != '\\' &&
               (unsigned char)p->b[p->i] >= 0x20)
            ++p->i;
        if (dst && !sb_put(dst, p->b + s, p->i - s))
            return J_OOM;
        if (p->i >= p->n)
            return jfail(p, "unterminated string");
        char c = p->b[p->i];
        if (c == '"') {
            ++p->i;
            return J_OK;
        }
        if ((unsigned char)c < 0x20)
            return jfail(p, "control character in string");
        ++p->i; /* backslash */
        if (p->i >= p->n)
            return jfail(p, "unterminated escape");
        c = p->b[p->i++];
        static const char from[] = "\"\\/bfnrt", to[] = "\"\\/\b\f\n\r\t";
        const char *f = c ? strchr(from, c) : NULL;
        if (f) {
            if (dst && !sb_put(dst, &to[f - from], 1))
                return J_OOM;
            continue;
        }
        if (c != 'u')
            return jfail(p, "unknown escape");
        uint32_t u;
        int r = hex4(p, &u);
        if (r != J_OK)
            return r;
        if (u >= 0xDC00 && u <= 0xDFFF)
            return jfail(p, "lone low surrogate");
        if (u >= 0xD800 && u <= 0xDBFF) {
            uint32_t lo;
            if (p->n - p->i < 2 || p->b[p->i] != '\\' || p->b[p->i + 1] != 'u')
                return jfail(p, "lone high surrogate");
            p->i += 2;
            if ((r = hex4(p, &lo)) != J_OK)
                return r;
            if (lo < 0xDC00 || lo > 0xDFFF)
                return jfail(p, "high surrogate without low surrogate");
            u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
        }
        if (u == 0)
            u = ' '; /* NUL cannot be stored in memory text */
        if (dst && !sb_utf8(dst, u))
            return J_OOM;
    }
}

static int jnumber(jp *p)
{
    size_t i = p->i;
    const char *b = p->b;
    if (i < p->n && b[i] == '-')
        ++i;
    if (i < p->n && b[i] == '0')
        ++i;
    else if (i < p->n && b[i] >= '1' && b[i] <= '9')
        while (i < p->n && b[i] >= '0' && b[i] <= '9')
            ++i;
    else
        return jfail(p, "unexpected character");
    if (i < p->n && b[i] == '.') {
        ++i;
        if (i >= p->n || b[i] < '0' || b[i] > '9') {
            p->i = i;
            return jfail(p, "bad fraction");
        }
        while (i < p->n && b[i] >= '0' && b[i] <= '9')
            ++i;
    }
    if (i < p->n && (b[i] == 'e' || b[i] == 'E')) {
        ++i;
        if (i < p->n && (b[i] == '+' || b[i] == '-'))
            ++i;
        if (i >= p->n || b[i] < '0' || b[i] > '9') {
            p->i = i;
            return jfail(p, "bad exponent");
        }
        while (i < p->n && b[i] >= '0' && b[i] <= '9')
            ++i;
    }
    p->i = i;
    return J_OK;
}

/* one "path: value" line for a scalar whose text is v[0..len) (already in out after mark) */
static int leaf_begin(jp *p)
{
    if (p->path.n && (!sb_put(p->out, p->path.p, p->path.n) || !sb_put(p->out, ": ", 2)))
        return J_OOM;
    return J_OK;
}

static int jvalue(jp *p);

static int jobject(jp *p)
{
    if (++p->depth > XKSM_JSON_MAX_DEPTH)
        return jfail(p, "nesting deeper than 256");
    ++p->i;
    jws(p);
    if (p->i < p->n && p->b[p->i] == '}') {
        ++p->i;
        --p->depth;
        return J_OK;
    }
    size_t base = p->path.n;
    for (;;) {
        jws(p);
        if (p->i >= p->n || p->b[p->i] != '"')
            return jfail(p, "expected object key");
        if (base && !sb_put(&p->path, ".", 1))
            return J_OOM;
        int r = jstring(p, &p->path);
        if (r != J_OK)
            return r;
        jws(p);
        if (p->i >= p->n || p->b[p->i] != ':')
            return jfail(p, "expected ':'");
        ++p->i;
        if ((r = jvalue(p)) != J_OK)
            return r;
        p->path.n = base;
        jws(p);
        if (p->i < p->n && p->b[p->i] == ',') {
            ++p->i;
            continue;
        }
        if (p->i < p->n && p->b[p->i] == '}') {
            ++p->i;
            --p->depth;
            return J_OK;
        }
        return jfail(p, "expected ',' or '}'");
    }
}

static int jarray(jp *p)
{
    if (++p->depth > XKSM_JSON_MAX_DEPTH)
        return jfail(p, "nesting deeper than 256");
    ++p->i;
    jws(p);
    if (p->i < p->n && p->b[p->i] == ']') {
        ++p->i;
        --p->depth;
        return J_OK;
    }
    size_t base = p->path.n;
    for (size_t k = 0;; ++k) {
        char ix[24];
        int l = 0;
        ix[l++] = '[';
        char d[21];
        int nd = 0;
        size_t v = k;
        do {
            d[nd++] = (char)('0' + v % 10);
            v /= 10;
        } while (v);
        while (nd)
            ix[l++] = d[--nd];
        ix[l++] = ']';
        if (!sb_put(&p->path, ix, (size_t)l))
            return J_OOM;
        int r = jvalue(p);
        if (r != J_OK)
            return r;
        p->path.n = base;
        jws(p);
        if (p->i < p->n && p->b[p->i] == ',') {
            ++p->i;
            continue;
        }
        if (p->i < p->n && p->b[p->i] == ']') {
            ++p->i;
            --p->depth;
            return J_OK;
        }
        return jfail(p, "expected ',' or ']'");
    }
}

static int literal(jp *p, const char *w, size_t n)
{
    if (p->n - p->i < n || memcmp(p->b + p->i, w, n))
        return jfail(p, "unexpected character");
    p->i += n;
    return J_OK;
}

/* walk mode: deliver one leaf; raw lexeme for numbers / literals, unescaped strings */
static int jleaf(jp *p, int kind, const char *v, size_t n)
{
    return p->cb(p->ctx, p->path.p ? p->path.p : "", p->path.n, kind, v, n) ? J_OK : J_OOM;
}

static int jvalue(jp *p)
{
    jws(p);
    if (p->i >= p->n)
        return jfail(p, "expected a value");
    char c = p->b[p->i];
    int r;
    size_t mark = p->out->n;
    switch (c) {
    case '{':
        return jobject(p);
    case '[':
        return jarray(p);
    case '"': {
        if (p->cb) {
            p->tmp.n = 0;
            if ((r = jstring(p, &p->tmp)) != J_OK)
                return r;
            return jleaf(p, XKSM_JSON_STRING, p->tmp.p ? p->tmp.p : "", p->tmp.n);
        }
        if ((r = leaf_begin(p)) != J_OK)
            return r;
        size_t vs = p->out->n;
        if ((r = jstring(p, p->out)) != J_OK)
            return r;
        if (p->out->n == vs) { /* empty string: no line */
            p->out->n = mark;
            return J_OK;
        }
        return sb_put(p->out, "\n", 1) ? J_OK : J_OOM;
    }
    case 'n':
        if ((r = literal(p, "null", 4)) != J_OK)
            return r;
        return p->cb ? jleaf(p, XKSM_JSON_NULL, "null", 4) : J_OK; /* null: no line */
    case 't':
    case 'f': {
        size_t s = p->i;
        if ((r = literal(p, c == 't' ? "true" : "false", c == 't' ? 4 : 5)) != J_OK)
            return r;
        if (p->cb)
            return jleaf(p, XKSM_JSON_BOOL, p->b + s, p->i - s);
        if ((r = leaf_begin(p)) != J_OK)
            return r;
        return sb_put(p->out, p->b + s, p->i - s) && sb_put(p->out, "\n", 1) ? J_OK : J_OOM;
    }
    default: {
        size_t s = p->i;
        if ((r = jnumber(p)) != J_OK)
            return r;
        if (p->cb)
            return jleaf(p, XKSM_JSON_NUMBER, p->b + s, p->i - s);
        if ((r = leaf_begin(p)) != J_OK)
            return r;
        return sb_put(p->out, p->b + s, p->i - s) && sb_put(p->out, "\n", 1) ? J_OK : J_OOM;
    }
    }
}

/* ---- records ------------------------------------------------------------------------ */
static int add_record(xksm_records *R, sbuf *arena, size_t so, size_t sl, size_t to)
{
    if (arena->n == to)
        return 1; /* rendered to nothing */
    if (R->n == R->cap) {
        size_t cap = R->cap ? 2 * R->cap : 64;
        xksm_record *v = realloc(R->v, cap * sizeof *v);
        if (!v)
            return 0;
        R->v = v;
        R->cap = cap;
    }
    size_t tl = arena->n - to;
    if (tl && arena->p[to + tl - 1] == '\n')
        --tl; /* no trailing newline in the memory text */
    R->v[R->n++] = (xksm_record){so, sl, to, tl};
    return 1;
}

/* one JSON value at p->i as one record; the record's raw span is the value itself */
static int json_record(jp *p, xksm_records *R, sbuf *arena)
{
    jws(p);
    size_t so = p->i, to = arena->n;
    p->path.n = 0;
    p->depth = 0;
    int r = jvalue(p);
    if (r != J_OK)
        return r;
    return add_record(R, arena, so, p->i - so, to) ? J_OK : J_OOM;
}

static int parse_json(const char *b, size_t n, int lines, xksm_records *R, sbuf *arena)
{
    jp p = {0};
    p.b = b;
    p.n = n;
    p.out = arena;
    int r = J_OK;
    if (!lines) {
        jws(&p);
        if (p.i < n && b[p.i] == '[') { /* top-level array: one record per element */
            ++p.i;
            jws(&p);
            if (p.i < n && b[p.i] == ']')
                ++p.i;
            else
                for (;;) {
                    if ((r = json_record(&p, R, arena)) != J_OK)
                        break;
                    jws(&p);
                    if (p.i < n && b[p.i] == ',') {
                        ++p.i;
                        continue;
                    }
                    if (p.i < n && b[p.i] == ']') {
                        ++p.i;
                        break;
                    }
                    r = jfail(&p, "expected ',' or ']'");
                    break;
                }
        } else {
            r = json_record(&p, R, arena);
        }
        if (r == J_OK) {
            jws(&p);
            if (p.i != n)
                r = jfail(&p, "data after the JSON value");
        }
    } else {
        for (size_t ls = 0; ls < n && r == J_OK;) {
            size_t le = ls;
            while (le < n && b[le] != '\n')
                ++le;
            size_t e = le;
            if (e > ls && b[e - 1] == '\r')
                --e;
            p.i = ls;
            p.n = e;
            jws(&p);
            if (p.i < e) { /* non-blank line */
                r = json_record(&p, R, arena);
                if (r == J_OK) {
                    jws(&p);
                    if (p.i != e)
                        r = jfail(&p, "data after the JSON value on this line");
                }
            }
            ls = le + 1;
        }
    }
    free(p.path.p);
    R->err = p.err;
    R->err_off = p.err_off;
    return r;
}

/* ---- CSV ------------------------------------------------------------------------------ */
typedef struct field {
    size_t off, len; /* in the field arena */
} field;

/* one row from b[*pos]; fields unescaped into fa; returns 1 row, 0 end, -2 syntax, -1 oom */
static int csv_row(const char *b, size_t n, size_t *pos, sbuf *fa, field **f, size_t *nf,
                   size_t *fcap, size_t *rs, size_t *re, xksm_records *R)
{
    size_t i = *pos;
    while (i < n && (b[i] == '\n' || (b[i] == '\r' && i + 1 < n && b[i + 1] == '\n')))
        i += b[i] == '\r' ? 2 : 1; /* skip empty lines */
    if (i >= n)
        return 0;
    *rs = i;
    *nf = 0;
    fa->n = 0;
    for (;;) {
        if (*nf == *fcap) {
            size_t cap = *fcap ? 2 * *fcap : 16;
            field *v = realloc(*f, cap * sizeof *v);
            if (!v)
                return -1;
            *f = v;
            *fcap = cap;
        }
        size_t fo = fa->n;
        if (i < n && b[i] == '"') {
            ++i;
            for (;;) {
                size_t s = i;
                while (i < n && b[i] != '"')
                    ++i;
                if (!sb_put(fa, b + s, i - s))
                    return -1;
                if (i >= n) {
                    R->err = "unterminated quoted field";
                    R->err_off = *rs;
                    return -2;
                }
                if (i + 1 < n && b[i + 1] == '"') { /* "" = one quote */
                    if (!sb_put(fa, "\"", 1))
                        return -1;
                    i += 2;
                    continue;
                }
                ++i; /* closing quote */
                break;
            }
            if (i < n && b[i] != ',' && b[i] != '\n' && !(b[i] == '\r' && i + 1 < n && b[i + 1] == '\n')) {
                R->err = "character after a closing quote";
                R->err_off = i;
                return -2;
            }
        } else {
            size_t s = i;
            while (i < n && b[i] != ',' && b[i] != '\n' && !(b[i] == '\r' && i + 1 < n && b[i + 1] == '\n')) {
                if (b[i] == '"') {
                    R->err = "quote inside an unquoted field";
                    R->err_off = i;
                    return -2;
                }
                ++i;
            }
            if (!sb_put(fa, b + s, i - s))
                return -1;
        }
        (*f)[(*nf)++] = (field){fo, fa->n - fo};
        if (i < n && b[i] == ',') {
            ++i;
            continue;
        }
        *re = i;
        if (i < n)
            i += b[i] == '\r' ? 2 : 1;
        *pos = i;
        return 1;
    }
}

static int parse_csv(const char *b, size_t n, xksm_records *R, sbuf *arena)
{
    sbuf fa = {0}, hdr = {0};
    field *f = NULL, *h = NULL;
    size_t nf = 0, fcap = 0, nh = 0, pos = 0, rs = 0, re = 0;
    int r = csv_row(b, n, &pos, &fa, &f, &nf, &fcap, &rs, &re, R);
    int rc = 1;
    if (r == 1) { /* header */
        h = malloc(nf * sizeof *h);
        if (!h || !sb_put(&hdr, fa.p ? fa.p : "", fa.n))
            rc = -1;
        else {
            memcpy(h, f, nf * sizeof *h);
            nh = nf;
        }
    } else if (r < 0) {
        rc = r == -1 ? -1 : 0;
    }
    while (rc == 1 && (r = csv_row(b, n, &pos, &fa, &f, &nf, &fcap, &rs, &re, R)) == 1) {
        if (nf != nh) {
            R->err = "row has a different number of fields than the header";
            R->err_off = rs;
            rc = 0;
            break;
        }
        size_t to = arena->n;
        for (size_t j = 0; j < nf && rc == 1; ++j) {
            if (!f[j].len)
                continue; /* empty value: no line */
            int ok;
            if (h[j].len)
                ok = sb_put(arena, hdr.p + h[j].off, h[j].len);
            else {
                char col[32];
                size_t l = 0, v = j + 1;
                char d[21];
                int nd = 0;
                memcpy(col, "column ", 7);
                l = 7;
                do {
                    d[nd++] = (char)('0' + v % 10);
                    v /= 10;
                } while (v);
                while (nd)
                    col[l++] = d[--nd];
                ok = sb_put(arena, col, l);
            }
            ok = ok && sb_put(arena, ": ", 2) && sb_put(arena, fa.p + f[j].off, f[j].len) &&
                 sb_put(arena, "\n", 1);
            if (!ok)
                rc = -1;
        }
        if (rc == 1 && !add_record(R, arena, rs, re - rs, to))
            rc = -1;
    }
    if (rc == 1 && r < 0)
        rc = r == -1 ? -1 : 0;
    free(fa.p);
    free(hdr.p);
    free(f);
    free(h);
    return rc;
}

/* ---- entry ------------------------------------------------------------------------------ */
int xksm__structured(const char *b, size_t n, int kind, xksm_records *out)
{
    memset(out, 0, sizeof *out);
    size_t skip = n >= 3 && !memcmp(b, "\xEF\xBB\xBF", 3) ? 3 : 0; /* UTF-8 BOM */
    sbuf arena = {0};
    int r;
    if (kind == XKSM_ST_CSV)
        r = parse_csv(b + skip, n - skip, out, &arena);
    else
        r = parse_json(b + skip, n - skip, kind == XKSM_ST_JSONL, out, &arena);
    if (skip) {
        for (size_t i = 0; i < out->n; ++i)
            out->v[i].src_off += skip;
        out->err_off += skip;
    }
    out->arena = arena.p;
    out->used = arena.n;
    out->acap = arena.cap;
    return r;
}

void xksm__records_free(xksm_records *r)
{
    free(r->v);
    free(r->arena);
    memset(r, 0, sizeof *r);
}

int xksm__json_walk(const char *b, size_t n, xksm_json_cb cb, void *ctx, const char **err,
                    size_t *err_off)
{
    jp p = {0};
    sbuf sink = {0};
    p.b = b;
    p.n = n;
    p.out = &sink;
    p.cb = cb;
    p.ctx = ctx;
    int r = jvalue(&p);
    if (r == J_OK) {
        jws(&p);
        if (p.i != n)
            r = jfail(&p, "data after the JSON value");
    }
    if (err)
        *err = p.err;
    if (err_off)
        *err_off = p.err_off;
    free(p.path.p);
    free(p.tmp.p);
    free(sink.p);
    return r;
}
