/* harness.c: agent-harness integration of the xksm CLI (Claude Code, omp, any other).
 * Every turn of a session is stored with its exact time, role and session, embedded with the
 * store's word-vector table, and recalled into the next prompt of the same harness.
 *
 *   xksm hook [--harness H]          Claude Code hook (stdin = hook JSON): UserPromptSubmit,
 *                                    Stop, SessionEnd, PreCompact. Default harness claude-code.
 *   xksm turn --harness H --session S --role R [--cwd D] [--time-us T]
 *                                    store one turn read from stdin (any harness).
 *   xksm context --harness H [--session S] [--k N] [--chars N]
 *                                    print recalled turns for the prompt on stdin (any harness).
 *   xksm transcripts [--harness H] [FILE...]
 *                                    backfill from Claude Code transcript JSONL (default: every
 *                                    top-level transcript under ~/.claude/projects).
 *   xksm sessions [HARNESS]          list sessions, most recent first.
 *   xksm forget HARNESS SESSION      delete every memory of a session.
 *   xksm pause HARNESS on|off        stop or resume logging for a harness.
 *   xksm mcp [--harness H]           MCP server over stdio: recall, search, remember, stats,
 *                                    sessions.
 * Store: $XKSM_STORE, else $XDG_DATA_HOME/xksm/agents.xksm, else ~/.local/share/xksm/agents.xksm.
 * Namespaces per harness: H = dialogue (prompts, assistant text, notes), H-trace = tool calls,
 * tool results, thinking. Source uri: xksm-turn://H/SESSION/ROLE/TIME_US. Nothing leaves the
 * machine. */
#define _DEFAULT_SOURCE /* timegm, localtime_r, getline, clock_gettime */
#include "harness.h"

#include "../src/core/sha256.h"
#include "../src/ingest/structured.h"

#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define VERSION_STR "0.1.0"
#define DEFAULT_HARNESS "claude-code"

/* ---- small utilities ---------------------------------------------------------------- */
typedef struct buf {
    char *p;
    size_t n, cap;
} buf;

static int bput(buf *b, const char *s, size_t n)
{
    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (cap < b->n + n + 1)
            cap *= 2;
        char *p = realloc(b->p, cap);
        if (!p)
            return 0;
        b->p = p;
        b->cap = cap;
    }
    if (n)
        memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
    return 1;
}
static int bputs(buf *b, const char *s) { return bput(b, s, strlen(s)); }
static const char *bs(const buf *b) { return b->p ? b->p : ""; }

/* JSON string body escaping (RFC 8259) */
static int bjson(buf *b, const char *s, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)s[i];
        char e[8];
        int ok;
        if (c == '"')
            ok = bput(b, "\\\"", 2);
        else if (c == '\\')
            ok = bput(b, "\\\\", 2);
        else if (c == '\n')
            ok = bput(b, "\\n", 2);
        else if (c == '\r')
            ok = bput(b, "\\r", 2);
        else if (c == '\t')
            ok = bput(b, "\\t", 2);
        else if (c < 0x20) {
            snprintf(e, sizeof e, "\\u%04x", c);
            ok = bput(b, e, 6);
        } else
            ok = bput(b, &s[i], 1);
        if (!ok)
            return 0;
    }
    return 1;
}

/* longest prefix of s[0..n) of at most max bytes that ends on a UTF-8 boundary */
static size_t utf8_cut(const char *s, size_t n, size_t max)
{
    if (n <= max)
        return n;
    size_t c = max;
    while (c > 0 && ((unsigned char)s[c] & 0xC0) == 0x80)
        --c;
    return c;
}

static int64_t now_us(void)
{
    struct timespec t;
    timespec_get(&t, TIME_UTC);
    return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

/* exact local time with milliseconds and UTC offset: 2026-11-06T10:35:03.123-06:00 */
static void fmt_time(int64_t us, char out[40])
{
    time_t s = (time_t)(us / 1000000);
    struct tm tm;
    localtime_r(&s, &tm);
    char a[24], z[8];
    strftime(a, sizeof a, "%Y-%m-%dT%H:%M:%S", &tm);
    strftime(z, sizeof z, "%z", &tm); /* +hhmm */
    snprintf(out, 40, "%s.%03d%.3s:%.2s", a, (int)((us / 1000) % 1000), z, z + 3);
}

/* ISO 8601 UTC "YYYY-MM-DDTHH:MM:SS(.fff)Z" -> microseconds since the epoch; 0 if invalid */
static int64_t parse_iso(const char *s, size_t n)
{
    char t[40];
    if (n < 19 || n >= sizeof t)
        return 0;
    memcpy(t, s, n);
    t[n] = 0;
    struct tm tm = {0};
    if (sscanf(t, "%4d-%2d-%2dT%2d:%2d:%2d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour,
               &tm.tm_min, &tm.tm_sec) != 6)
        return 0;
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    int64_t frac = 0;
    if (t[19] == '.') {
        int64_t scale = 100000;
        for (size_t i = 20; i < n && t[i] >= '0' && t[i] <= '9'; ++i, scale /= 10)
            frac += (t[i] - '0') * scale;
    }
    return (int64_t)timegm(&tm) * 1000000 + frac;
}

static int read_all(FILE *f, buf *b)
{
    char tmp[65536];
    size_t got;
    while ((got = fread(tmp, 1, sizeof tmp, f)) > 0)
        if (!bput(b, tmp, got))
            return 0;
    return bput(b, "", 0) && !ferror(f);
}

static size_t env_size(const char *name, size_t def)
{
    const char *e = getenv(name);
    if (!e || !*e)
        return def;
    char *end;
    unsigned long long v = strtoull(e, &end, 10);
    return *end ? def : (size_t)v;
}

static int pis(const char *p, size_t n, const char *s)
{
    size_t l = strlen(s);
    return n == l && !memcmp(p, s, l);
}

/* harness names become namespace and uri parts: [a-z0-9._-], 1..48 */
static int harness_ok(const char *h)
{
    size_t n = strlen(h);
    if (!n || n > 48)
        return 0;
    for (size_t i = 0; i < n; ++i)
        if (!((h[i] >= 'a' && h[i] <= 'z') || (h[i] >= '0' && h[i] <= '9') || h[i] == '-' ||
              h[i] == '_' || h[i] == '.'))
            return 0;
    return 1;
}

/* ---- the store --------------------------------------------------------------------------- */
static int store_path(char *out, size_t cap)
{
    const char *e = getenv("XKSM_STORE");
    if (e && *e)
        return snprintf(out, cap, "%s", e) < (int)cap;
    const char *xdg = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
    char dir[3072];
    if (xdg && *xdg)
        snprintf(dir, sizeof dir, "%s/xksm", xdg);
    else if (home && *home)
        snprintf(dir, sizeof dir, "%s/.local/share/xksm", home);
    else
        return 0;
    for (char *p = dir + 1; *p; ++p) /* mkdir -p, private to the user */
        if (*p == '/') {
            *p = 0;
            mkdir(dir, 0700);
            *p = '/';
        }
    if (mkdir(dir, 0700) != 0 && errno != EEXIST)
        return 0;
    return snprintf(out, cap, "%s/agents.xksm", dir) < (int)cap;
}

static xksm *open_store(void)
{
    char path[4096];
    if (!store_path(path, sizeof path)) {
        fprintf(stderr, "xksm: no store path (set XKSM_STORE or HOME)\n");
        return NULL;
    }
    umask(077); /* the store and its WAL are private to the user */
    xksm *x = NULL;
    if (xksm_open(path, &x) != XKSM_OK) {
        fprintf(stderr, "xksm: %s: %s\n", path, xksm_errmsg(x));
        xksm_close(x);
        return NULL;
    }
    return x;
}

/* one harness context: names derived once */
typedef struct hctx {
    xksm *x;
    char harness[49], ns[49], ns_trace[56];
} hctx;

static int hctx_init(hctx *h, xksm *x, const char *harness)
{
    if (!harness_ok(harness)) {
        fprintf(stderr, "xksm: harness name must be 1..48 of [a-z0-9._-]: %s\n", harness);
        return 0;
    }
    h->x = x;
    snprintf(h->harness, sizeof h->harness, "%s", harness);
    snprintf(h->ns, sizeof h->ns, "%s", harness);
    snprintf(h->ns_trace, sizeof h->ns_trace, "%s-trace", harness);
    return 1;
}

static int paused(const hctx *h)
{
    const char *e = getenv("XKSM_PAUSE");
    if (e && *e && strcmp(e, "0"))
        return 1;
    char key[96], v[4];
    size_t n = 0;
    snprintf(key, sizeof key, "%s:paused", h->harness);
    return xksm_kv_get(h->x, key, v, sizeof v, &n) == XKSM_OK && n == 1 && v[0] == '1';
}

static void hex16(const char *text, size_t len, char out[33])
{
    static const char hx[] = "0123456789abcdef";
    uint8_t d[32];
    xksm__sha256(text, len, d);
    for (int i = 0; i < 16; ++i) {
        out[2 * i] = hx[d[i] >> 4];
        out[2 * i + 1] = hx[d[i] & 15];
    }
    out[32] = 0;
}

/* one turn (or trace block) = one source with exact time, role and session; every chunk
 * carries event_us; the text's first line is the header "TIME | ROLE | session SID" */
static xksm_status store_turn(const hctx *h, int trace_ns, const char *role, const char *session,
                              const char *cwd, const char *text, size_t len, int64_t event_us)
{
    if (!len)
        return XKSM_OK;
    const char *sid = session && *session ? session : "unknown";
    char t[40], uri[640], head[256];
    fmt_time(event_us, t);
    snprintf(uri, sizeof uri, "xksm-turn://%s/%s/%s/%lld", h->harness, sid, role,
             (long long)event_us);
    int hl = snprintf(head, sizeof head, "%s | %s | session %.8s\n", t, role, sid);
    buf body = {0};
    if (hl < 0 || !bput(&body, head, (size_t)hl) || !bput(&body, text, len)) {
        free(body.p);
        return XKSM_ENOMEM;
    }
    xksm_import_opts o = {0};
    o.ns = trace_ns ? h->ns_trace : h->ns;
    o.category = XKSM_CAT_EPISODIC;
    o.format = XKSM_FMT_TEXT;
    o.event_us = event_us;
    xksm_import_result r;
    xksm_status s = xksm_import_buffer(h->x, uri, body.p, body.n, &o, &r);
    free(body.p);
    if (s == XKSM_EXISTS)
        return XKSM_OK;
    if (s != XKSM_OK) {
        fprintf(stderr, "xksm: store %s turn: %s\n", role, xksm_errmsg(h->x));
        return s;
    }
    xksm_session_touch(h->x, h->harness, sid, cwd, event_us);
    return XKSM_OK;
}

/* ---- live dedup: a dialogue text stored from hook input is not stored again when the
 * transcript copy arrives (counter per harness, session, role, text hash) ----------------- */
static void live_key(const hctx *h, const char *session, const char *role, const char *text,
                     size_t len, char *key, size_t cap)
{
    char hx[33];
    hex16(text, len, hx);
    snprintf(key, cap, "%s:live:%s:%s:%s", h->harness, session ? session : "", role, hx);
}

/* ---- Claude Code transcript lines ------------------------------------------------------- */
typedef struct block {
    char type[24];
    buf text, name, input, result, thinking;
} block;

typedef struct line {
    char type[24];
    int sidechain, meta;
    int64_t ts;
    char session[96];
    buf content; /* message.content when it is a string */
    block *b;
    size_t nb;
} line;

static block *blk(line *L, size_t i)
{
    if (i >= 4096)
        return NULL;
    if (i >= L->nb) {
        block *nb = realloc(L->b, sizeof *nb * (i + 1));
        if (!nb)
            return NULL;
        memset(nb + L->nb, 0, sizeof *nb * (i + 1 - L->nb));
        L->b = nb;
        L->nb = i + 1;
    }
    return &L->b[i];
}

static int line_cb(void *ctx, const char *path, size_t pl, int kind, const char *v, size_t vl)
{
    line *L = ctx;
    if (pis(path, pl, "type") && kind == XKSM_JSON_STRING) {
        snprintf(L->type, sizeof L->type, "%.*s", (int)vl, v);
    } else if (pis(path, pl, "isSidechain")) {
        L->sidechain = kind == XKSM_JSON_BOOL && v[0] == 't';
    } else if (pis(path, pl, "isMeta")) {
        L->meta = kind == XKSM_JSON_BOOL && v[0] == 't';
    } else if (pis(path, pl, "timestamp") && kind == XKSM_JSON_STRING) {
        L->ts = parse_iso(v, vl);
    } else if (pis(path, pl, "sessionId") && kind == XKSM_JSON_STRING) {
        snprintf(L->session, sizeof L->session, "%.*s", (int)vl, v);
    } else if (pis(path, pl, "message.content") && kind == XKSM_JSON_STRING) {
        return bput(&L->content, v, vl);
    } else if (pl > 16 && !memcmp(path, "message.content[", 16)) {
        size_t i = 16, idx = 0;
        while (i < pl && path[i] >= '0' && path[i] <= '9')
            idx = idx * 10 + (size_t)(path[i++] - '0');
        if (i >= pl || path[i] != ']')
            return 1;
        ++i;
        const char *r = path + i;
        size_t rl = pl - i;
        block *B = blk(L, idx);
        if (!B)
            return 1;
        if (pis(r, rl, ".type") && kind == XKSM_JSON_STRING)
            snprintf(B->type, sizeof B->type, "%.*s", (int)vl, v);
        else if (pis(r, rl, ".text") && kind == XKSM_JSON_STRING)
            return bput(&B->text, v, vl);
        else if (pis(r, rl, ".name") && kind == XKSM_JSON_STRING)
            return bput(&B->name, v, vl);
        else if (pis(r, rl, ".thinking") && kind == XKSM_JSON_STRING)
            return bput(&B->thinking, v, vl);
        else if (rl >= 6 && !memcmp(r, ".input", 6) && kind != XKSM_JSON_NULL) {
            const char *k = r + 6;
            size_t kl = rl - 6;
            if (kl && *k == '.')
                ++k, --kl;
            return (!kl || (bput(&B->input, k, kl) && bput(&B->input, ": ", 2))) &&
                   bput(&B->input, v, vl) && bput(&B->input, "\n", 1);
        } else if ((pis(r, rl, ".content") ||
                    (rl > 14 && !memcmp(r, ".content[", 9) && !memcmp(r + rl - 5, ".text", 5))) &&
                   kind == XKSM_JSON_STRING) {
            if (B->result.n && !bput(&B->result, "\n", 1))
                return 0;
            return bput(&B->result, v, vl);
        }
    }
    return 1;
}

static void line_free(line *L)
{
    for (size_t i = 0; i < L->nb; ++i) {
        free(L->b[i].text.p);
        free(L->b[i].name.p);
        free(L->b[i].input.p);
        free(L->b[i].result.p);
        free(L->b[i].thinking.p);
    }
    free(L->b);
    free(L->content.p);
}

/* harness plumbing in user text (reminders, slash-command echoes), not conversation */
static int plumbing(const char *s, size_t n)
{
    static const char *const pre[] = {"<system-reminder>", "<command-", "<local-command-",
                                      "Caveat: The messages below", "[Request interrupted"};
    while (n && (*s == ' ' || *s == '\n' || *s == '\t' || *s == '\r'))
        ++s, --n;
    for (size_t i = 0; i < sizeof pre / sizeof *pre; ++i)
        if (n >= strlen(pre[i]) && !memcmp(s, pre[i], strlen(pre[i])))
            return 1;
    return n == 0;
}

typedef struct ingest_stats {
    size_t lines, bad_lines, dialogue, trace, skipped_live;
    char seen[64][33]; /* hashes of dialogue texts stored in this call (ring) */
    size_t nseen;
} ingest_stats;

static int was_seen(const ingest_stats *st, const char *hx)
{
    size_t n = st->nseen < 64 ? st->nseen : 64;
    for (size_t i = 0; i < n; ++i)
        if (!strcmp(st->seen[i], hx))
            return 1;
    return 0;
}

static void dialogue(const hctx *h, const char *session, const char *cwd, const char *role,
                     const char *t, size_t n, int64_t ts, ingest_stats *st)
{
    char key[320], v[32];
    size_t vl = 0;
    live_key(h, session, role, t, n, key, sizeof key);
    if (xksm_kv_get(h->x, key, v, sizeof v - 1, &vl) == XKSM_OK) {
        int64_t left = 0;
        if (xksm_kv_add(h->x, key, -1, &left) == XKSM_OK && left <= 0)
            xksm_kv_delete(h->x, key);
        st->skipped_live++;
        return;
    }
    if (store_turn(h, 0, role, session, cwd, t, n, ts) == XKSM_OK) {
        st->dialogue++;
        hex16(t, n, st->seen[st->nseen++ % 64]);
        char lk[256];
        snprintf(lk, sizeof lk, "%s:last:%s:%s", h->harness, session ? session : "", role);
        xksm_kv_put(h->x, lk, st->seen[(st->nseen - 1) % 64], 32); /* across calls */
    }
}

static void trace(const hctx *h, const char *session, const char *cwd, const char *role,
                  const char *t, size_t n, int64_t ts, ingest_stats *st)
{
    size_t cap = env_size("XKSM_TRACE_MAX", 65536); /* bytes per block; 0 = unlimited */
    if (!n)
        return;
    xksm_status s;
    if (cap && n > cap) {
        buf b = {0};
        char note[96];
        size_t keep = utf8_cut(t, n, cap);
        int nl = snprintf(note, sizeof note, "\n[truncated: %zu of %zu bytes kept]", keep, n);
        s = bput(&b, t, keep) && nl > 0 && bput(&b, note, (size_t)nl)
                ? store_turn(h, 1, role, session, cwd, b.p, b.n, ts)
                : XKSM_ENOMEM;
        free(b.p);
    } else {
        s = store_turn(h, 1, role, session, cwd, t, n, ts);
    }
    if (s == XKSM_OK)
        st->trace++;
}

static void ingest_line(const hctx *h, const char *s, size_t n, const char *fallback_session,
                        const char *cwd, ingest_stats *st)
{
    line L = {0};
    st->lines++;
    if (xksm__json_walk(s, n, line_cb, &L, NULL, NULL) != 1) {
        st->bad_lines++;
        line_free(&L);
        return;
    }
    const char *sid = L.session[0] ? L.session : fallback_session;
    int64_t ts = L.ts ? L.ts : now_us();
    int user = !strcmp(L.type, "user"), asst = !strcmp(L.type, "assistant");
    if ((user || asst) && !L.sidechain && !L.meta) {
        if (user && L.content.n && !plumbing(L.content.p, L.content.n))
            dialogue(h, sid, cwd, "user", L.content.p, L.content.n, ts, st);
        for (size_t i = 0; i < L.nb; ++i) {
            block *B = &L.b[i];
            if (!strcmp(B->type, "text") && B->text.n && !plumbing(B->text.p, B->text.n)) {
                dialogue(h, sid, cwd, user ? "user" : "assistant", B->text.p, B->text.n, ts, st);
            } else if (!strcmp(B->type, "tool_use")) {
                buf t = {0};
                if (bputs(&t, "tool call: ") && bput(&t, bs(&B->name), B->name.n) &&
                    bput(&t, "\n", 1) && bput(&t, bs(&B->input), B->input.n))
                    trace(h, sid, cwd, "tool_use", t.p, t.n, ts, st);
                free(t.p);
            } else if (!strcmp(B->type, "tool_result") && B->result.n) {
                trace(h, sid, cwd, "tool_result", B->result.p, B->result.n, ts, st);
            } else if (!strcmp(B->type, "thinking") && B->thinking.n) {
                trace(h, sid, cwd, "thinking", B->thinking.p, B->thinking.n, ts, st);
            }
        }
    }
    line_free(&L);
}

/* the complete lines of a transcript after the recorded offset (at most budget bytes, 0 =
 * all); the offset is kept per harness and transcript path in the store */
static void ingest_transcript(const hctx *h, const char *path, const char *session,
                              const char *cwd, size_t budget, ingest_stats *st)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return;
    struct stat sb;
    if (fstat(fileno(f), &sb) != 0 || !S_ISREG(sb.st_mode)) {
        fclose(f);
        return;
    }
    char key[4200], v[32] = {0};
    snprintf(key, sizeof key, "%s:transcript:%s", h->harness, path);
    size_t vl = 0;
    long long off = 0;
    if (xksm_kv_get(h->x, key, v, sizeof v - 1, &vl) == XKSM_OK) {
        v[vl < sizeof v ? vl : sizeof v - 1] = 0;
        off = atoll(v);
    }
    if (off < 0 || off > (long long)sb.st_size)
        off = 0; /* rewritten or truncated: start over (dedup absorbs repeats) */
    size_t want = (size_t)(sb.st_size - off);
    if (budget && want > budget)
        want = budget;
    if (!want || fseeko(f, (off_t)off, SEEK_SET) != 0) {
        fclose(f);
        return;
    }
    char *data = malloc(want);
    size_t got = data ? fread(data, 1, want, f) : 0;
    fclose(f);
    size_t done = 0;
    for (size_t ls = 0; ls < got;) {
        size_t le = ls;
        while (le < got && data[le] != '\n')
            ++le;
        if (le >= got)
            break; /* incomplete last line: next time */
        if (le > ls)
            ingest_line(h, data + ls, le - ls, session, cwd, st);
        ls = le + 1;
        done = ls;
    }
    if (!done && budget && got == budget) { /* one line longer than the budget: skip it */
        done = got;
        st->bad_lines++;
    }
    free(data);
    snprintf(v, sizeof v, "%lld", off + (long long)done);
    xksm_kv_put(h->x, key, v, strlen(v));
}

/* keep the word-vector table current: build when absent (>= 200 memories) or after 25 %
 * growth (allow_build), and embed every memory that has no vector yet */
static void maintain(xksm *x, int allow_build)
{
    int64_t live = xksm_memory_count(x, NULL); /* live: other processes insert too */
    size_t built = xksm_semantic_built_memories(x), total = live > 0 ? (size_t)live : 0;
    int need = allow_build && ((built == 0 && total >= 200) || (built && total * 4 >= built * 5));
    if (need) {
        char v[32] = {0};
        size_t vl = 0;
        int64_t now = now_us();
        if (xksm_kv_get(x, "build_started_us", v, sizeof v - 1, &vl) == XKSM_OK &&
            now - atoll(v) < 120LL * 1000000)
            need = 0; /* another process is building */
        else {
            snprintf(v, sizeof v, "%lld", (long long)now);
            xksm_kv_put(x, "build_started_us", v, strlen(v));
        }
    }
    if (need) {
        xksm_semantic_report r;
        if (xksm_semantic_build(x, NULL, &r) != XKSM_OK)
            fprintf(stderr, "xksm: word-vector build: %s\n", xksm_errmsg(x));
        xksm_kv_delete(x, "build_started_us");
    }
    xksm_status s = xksm_semantic_update(x, NULL);
    if (s != XKSM_OK && s != XKSM_ENOTFOUND)
        fprintf(stderr, "xksm: embed: %s\n", xksm_errmsg(x));
}

/* ---- recall formatting ------------------------------------------------------------------ */
/* harness, session and role from xksm-turn://H/SESSION/ROLE/TIME */
static void uri_parts(const char *uri, char *harness, size_t hc, char *sid, size_t sc, char *role,
                      size_t rc)
{
    harness[0] = sid[0] = role[0] = 0;
    if (strncmp(uri, "xksm-turn://", 12))
        return;
    const char *p = uri + 12, *a = strchr(p, '/');
    if (!a)
        return;
    snprintf(harness, hc, "%.*s", (int)(a - p), p);
    const char *b = strchr(a + 1, '/');
    if (!b)
        return;
    snprintf(sid, sc, "%.*s", (int)(b - a - 1), a + 1);
    const char *c = strchr(b + 1, '/');
    snprintf(role, rc, "%.*s", (int)(c ? c - b - 1 : (long)strlen(b + 1)), b + 1);
}

/* append up to k hits as lines "[time | harness | role | session | memory id] text" */
static size_t format_hits(xksm *x, const xksm_id *ids, size_t n, const char *exclude_session,
                          size_t k, size_t per_hit, size_t total_cap, int show_harness, buf *out)
{
    size_t shown = 0;
    for (size_t i = 0; i < n && shown < k; ++i) {
        xksm_memory m;
        if (xksm_memory_get(x, ids[i], &m) != XKSM_OK)
            continue;
        char uri[700] = {0}, har[64], sid[128], role[32];
        if (m.source_id)
            xksm_source_info(x, m.source_id, uri, sizeof uri, NULL);
        uri_parts(uri, har, sizeof har, sid, sizeof sid, role, sizeof role);
        if (exclude_session && *exclude_session && !strcmp(sid, exclude_session)) {
            xksm_memory_free(&m);
            continue;
        }
        char t[40];
        fmt_time(m.event_us != XKSM_TIME_NONE ? m.event_us : m.created_us, t);
        const char *body = m.text;
        size_t bl = m.text_len;
        const char *nl = memchr(body, '\n', bl), *tag = strstr(body, " | session ");
        if (nl && tag && tag < nl) { /* our header line: shown as the bracket instead */
            bl -= (size_t)(nl + 1 - body);
            body = nl + 1;
        }
        size_t keep = utf8_cut(body, bl, per_hit);
        buf ln = {0};
        char head[256];
        int hl = show_harness
                     ? snprintf(head, sizeof head, "[%s | %s | %s | session %.8s | memory %lld] ", t,
                                har[0] ? har : "-", role[0] ? role : "note", sid[0] ? sid : "-",
                                (long long)m.id)
                     : snprintf(head, sizeof head, "[%s | %s | session %.8s | memory %lld] ", t,
                                role[0] ? role : "note", sid[0] ? sid : "-", (long long)m.id);
        int ok = hl > 0 && bput(&ln, head, (size_t)hl);
        for (size_t j = 0; ok && j < keep; ++j) {
            char c = body[j] == '\n' || body[j] == '\r' || body[j] == '\t' ? ' ' : body[j];
            ok = bput(&ln, &c, 1);
        }
        ok = ok && (keep == bl || bputs(&ln, " [...]")) && bput(&ln, "\n", 1);
        if (ok && out->n + ln.n <= total_cap) {
            bput(out, ln.p, ln.n);
            ++shown;
        }
        free(ln.p);
        xksm_memory_free(&m);
        if (out->n + 80 > total_cap)
            break;
    }
    return shown;
}

static size_t recall_text(xksm *x, const char *query, const char *ns, const char *exclude_session,
                          size_t k, size_t per_hit, size_t cap, buf *out)
{
    xksm_hybrid_opts ho = {0};
    ho.ns = ns;
    ho.limit = k * 3 + 8; /* room for hits of the current session, which are skipped */
    xksm_hybrid_result r;
    if (xksm_semantic_recall(x, query, NULL, &ho, &r) != XKSM_OK)
        return 0;
    xksm_id *ids = malloc(sizeof *ids * (r.count ? r.count : 1));
    size_t shown = 0;
    if (ids) {
        for (size_t i = 0; i < r.count; ++i)
            ids[i] = r.hits[i].id;
        shown = format_hits(x, ids, r.count, exclude_session, k, per_hit, cap, ns == NULL, out);
        free(ids);
    }
    xksm_hybrid_free(&r);
    return shown;
}

static size_t context_block(const hctx *h, const char *prompt, const char *session, buf *out)
{
    size_t k = env_size("XKSM_RECALL_K", 4), cap = env_size("XKSM_CONTEXT_CHARS", 4000);
    if (cap > 9500)
        cap = 9500; /* Claude Code caps a context string at 10,000 characters */
    if (!k || !*prompt)
        return 0;
    bputs(out, "XKS-Memory (local store) recalled these earlier turns of this harness related to "
               "the prompt; each shows when it happened:\n");
    size_t base = out->n;
    size_t shown = recall_text(h->x, prompt, h->ns, session, k, 600, base + cap, out);
    if (!shown)
        out->n = 0;
    return shown;
}

/* ---- Claude Code hook ------------------------------------------------------------------- */
typedef struct hook_in {
    buf event, session, transcript, prompt, last, cwd;
} hook_in;

static int hook_cb(void *ctx, const char *path, size_t pl, int kind, const char *v, size_t vl)
{
    hook_in *h = ctx;
    if (kind != XKSM_JSON_STRING)
        return 1;
    if (pis(path, pl, "hook_event_name"))
        return bput(&h->event, v, vl);
    if (pis(path, pl, "session_id"))
        return bput(&h->session, v, vl);
    if (pis(path, pl, "transcript_path"))
        return bput(&h->transcript, v, vl);
    if (pis(path, pl, "prompt"))
        return bput(&h->prompt, v, vl);
    if (pis(path, pl, "last_assistant_message"))
        return bput(&h->last, v, vl);
    if (pis(path, pl, "cwd"))
        return bput(&h->cwd, v, vl);
    return 1;
}

/* store a dialogue text from hook input unless the transcript pass of this call already
 * stored it; register it so a later transcript copy is skipped */
static void live(const hctx *h, const char *session, const char *cwd, const char *role,
                 const buf *t, const ingest_stats *st)
{
    if (!t->n || plumbing(t->p, t->n))
        return;
    char hx[33];
    hex16(t->p, t->n, hx);
    char lk[256], last[33] = {0};
    size_t ll = 0;
    snprintf(lk, sizeof lk, "%s:last:%s:%s", h->harness, session ? session : "", role);
    int stored = was_seen(st, hx) ||
                 (xksm_kv_get(h->x, lk, last, 32, &ll) == XKSM_OK && ll == 32 && !memcmp(last, hx, 32));
    if (stored) /* the transcript copy is already in the store (this or an earlier call) */
        return;
    if (store_turn(h, 0, role, session, cwd, t->p, t->n, now_us()) == XKSM_OK) {
        char key[320];
        live_key(h, session, role, t->p, t->n, key, sizeof key);
        xksm_kv_add(h->x, key, 1, NULL);
    }
}

static double prof_t(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

/* XKSM_PROFILE=1: phase times of a hook call on stderr */
static void prof(const char *what, double *t)
{
    double n = prof_t();
    if (getenv("XKSM_PROFILE"))
        fprintf(stderr, "xksm profile: %-10s %.4f s\n", what, n - *t);
    *t = n;
}

int xksm_cli_hook(const char *harness)
{
    buf in = {0};
    double pt = prof_t();
    hook_in hi = {0};
    if (!read_all(stdin, &in) || xksm__json_walk(in.p, in.n, hook_cb, &hi, NULL, NULL) != 1) {
        fprintf(stderr, "xksm hook: unreadable hook input\n");
        goto done; /* never break the session: exit 0 */
    }
    xksm *x = open_store();
    hctx h;
    if (!x)
        goto done;
    if (!hctx_init(&h, x, harness ? harness : DEFAULT_HARNESS) || paused(&h)) {
        xksm_close(x);
        goto done;
    }
    prof("open", &pt);
    const char *ev = bs(&hi.event), *sid = bs(&hi.session), *cwd = bs(&hi.cwd);
    ingest_stats st = {0};
    if (!strcmp(ev, "UserPromptSubmit")) {
        if (hi.transcript.n) /* the previous turn's tool calls and results, bounded */
            ingest_transcript(&h, bs(&hi.transcript), sid, cwd,
                              env_size("XKSM_INGEST_BUDGET", 8u << 20), &st);
        prof("ingest", &pt);
        buf ctx = {0};
        size_t shown = context_block(&h, bs(&hi.prompt), sid, &ctx);
        prof("recall", &pt);
        live(&h, sid, cwd, "user", &hi.prompt, &st);
        prof("store", &pt);
        maintain(x, 0); /* embed now; table builds happen at Stop */
        prof("embed", &pt);
        if (shown) {
            buf o = {0};
            bputs(&o, "{\"hookSpecificOutput\":{\"hookEventName\":\"UserPromptSubmit\","
                      "\"additionalContext\":\"");
            bjson(&o, ctx.p, ctx.n);
            bputs(&o, "\"}}\n");
            fwrite(o.p, 1, o.n, stdout);
            free(o.p);
        }
        free(ctx.p);
    } else if (!strcmp(ev, "Stop") || !strcmp(ev, "SessionEnd") || !strcmp(ev, "PreCompact")) {
        if (hi.transcript.n)
            ingest_transcript(&h, bs(&hi.transcript), sid, cwd, 0, &st);
        if (!strcmp(ev, "Stop")) /* the transcript may lag the final message */
            live(&h, sid, cwd, "assistant", &hi.last, &st);
        maintain(x, 1);
    }
    xksm_close(x);
    prof("close", &pt);
done:
    free(in.p);
    free(hi.event.p);
    free(hi.session.p);
    free(hi.transcript.p);
    free(hi.prompt.p);
    free(hi.last.p);
    free(hi.cwd.p);
    return 0;
}

/* ---- generic harness commands ----------------------------------------------------------- */
static const char *opt(int argc, char **argv, const char *name)
{
    for (int i = 0; i + 1 < argc; ++i)
        if (!strcmp(argv[i], name))
            return argv[i + 1];
    return NULL;
}

static int trace_role(const char *role)
{
    return !strcmp(role, "tool_use") || !strcmp(role, "tool_result") || !strcmp(role, "thinking");
}

int xksm_cli_turn(int argc, char **argv)
{
    const char *har = opt(argc, argv, "--harness"), *sid = opt(argc, argv, "--session"),
               *role = opt(argc, argv, "--role"), *cwd = opt(argc, argv, "--cwd"),
               *tus = opt(argc, argv, "--time-us");
    static const char *const roles[] = {"user", "assistant", "note", "tool_use", "tool_result",
                                        "thinking"};
    int role_ok = 0;
    for (size_t i = 0; role && i < sizeof roles / sizeof *roles; ++i)
        role_ok |= !strcmp(role, roles[i]);
    if (!har || !sid || !*sid || !role_ok) {
        fprintf(stderr, "usage: xksm turn --harness H --session S --role "
                        "user|assistant|note|tool_use|tool_result|thinking [--cwd D] "
                        "[--time-us T] < text\n");
        return 2;
    }
    buf in = {0};
    if (!read_all(stdin, &in))
        return 1;
    xksm *x = open_store();
    hctx h;
    int rc = 1;
    if (x && hctx_init(&h, x, har)) {
        rc = 0;
        if (!paused(&h) && in.n) {
            int64_t t = tus ? (int64_t)strtoll(tus, NULL, 10) : now_us();
            rc = store_turn(&h, trace_role(role), role, sid, cwd, in.p, in.n, t) != XKSM_OK;
            maintain(x, !strcmp(role, "assistant")); /* end of a turn: table may rebuild */
        }
    }
    xksm_close(x);
    free(in.p);
    return rc;
}

int xksm_cli_context(int argc, char **argv)
{
    const char *har = opt(argc, argv, "--harness"), *sid = opt(argc, argv, "--session");
    if (!har) {
        fprintf(stderr, "usage: xksm context --harness H [--session S] < prompt\n");
        return 2;
    }
    buf in = {0};
    if (!read_all(stdin, &in))
        return 1;
    xksm *x = open_store();
    hctx h;
    int rc = 1;
    if (x && hctx_init(&h, x, har)) {
        buf out = {0};
        if (context_block(&h, bs(&in), sid, &out))
            fwrite(out.p, 1, out.n, stdout);
        free(out.p);
        rc = 0;
    }
    xksm_close(x);
    free(in.p);
    return rc;
}

static int ends(const char *s, const char *suf)
{
    size_t a = strlen(s), b = strlen(suf);
    return a >= b && !strcmp(s + a - b, suf);
}

int xksm_cli_transcripts(int argc, char **argv)
{
    const char *har = DEFAULT_HARNESS;
    int first = 0;
    if (argc >= 2 && !strcmp(argv[0], "--harness")) {
        har = argv[1];
        first = 2;
    }
    xksm *x = open_store();
    hctx h;
    if (!x || !hctx_init(&h, x, har)) {
        xksm_close(x);
        return 1;
    }
    ingest_stats st = {0};
    size_t nf = 0;
    if (argc > first) {
        for (int i = first; i < argc; ++i, ++nf)
            ingest_transcript(&h, argv[i], "", NULL, 0, &st);
    } else {
        const char *home = getenv("HOME");
        char root[4096];
        snprintf(root, sizeof root, "%s/.claude/projects", home ? home : "");
        DIR *d = opendir(root);
        struct dirent *e;
        while (d && (e = readdir(d))) {
            if (e->d_name[0] == '.')
                continue;
            char sub[4400];
            snprintf(sub, sizeof sub, "%s/%s", root, e->d_name);
            DIR *d2 = opendir(sub);
            struct dirent *f;
            while (d2 && (f = readdir(d2))) {
                if (!ends(f->d_name, ".jsonl"))
                    continue;
                char p[4700];
                snprintf(p, sizeof p, "%s/%s", sub, f->d_name);
                ingest_transcript(&h, p, "", NULL, 0, &st);
                ++nf;
            }
            if (d2)
                closedir(d2);
        }
        if (d)
            closedir(d);
    }
    maintain(x, 1);
    xksm_cache_info ci;
    xksm_cache_stats(x, &ci);
    printf("harness %s: %zu transcripts, %zu lines (%zu unparseable): %zu dialogue and %zu trace "
           "turns stored, %zu already stored live; store holds %lld memories, word-vector table "
           "built from %zu\n",
           h.harness, nf, st.lines, st.bad_lines, st.dialogue, st.trace, st.skipped_live,
           (long long)ci.records, xksm_semantic_built_memories(x));
    xksm_close(x);
    return 0;
}

int xksm_cli_sessions(const char *harness)
{
    xksm *x = open_store();
    if (!x)
        return 1;
    xksm_session_info *l = NULL;
    size_t n = 0;
    int rc = xksm_session_list(x, harness, 0, &l, &n) != XKSM_OK;
    for (size_t i = 0; !rc && i < n; ++i) {
        char a[40], b[40];
        fmt_time(l[i].first_us, a);
        fmt_time(l[i].last_us, b);
        printf("%-14s %-38s %6lld turns  %s .. %s  %s\n", l[i].harness, l[i].id,
               (long long)l[i].turns, a, b, l[i].cwd ? l[i].cwd : "");
    }
    if (!rc && !n)
        printf("no sessions\n");
    xksm_session_list_free(l, n);
    xksm_close(x);
    return rc;
}

int xksm_cli_forget(const char *harness, const char *session)
{
    xksm *x = open_store();
    if (!x)
        return 1;
    size_t d = 0;
    xksm_status s = xksm_session_forget(x, harness, session, &d);
    if (s == XKSM_OK)
        printf("forgot %s session %s: %zu memories deleted\n", harness, session, d);
    else
        fprintf(stderr, "xksm: %s\n", xksm_errmsg(x));
    xksm_close(x);
    return s != XKSM_OK;
}

int xksm_cli_pause(const char *harness, const char *arg)
{
    int on = !strcmp(arg, "on");
    if ((!on && strcmp(arg, "off")) || !harness_ok(harness)) {
        fprintf(stderr, "usage: xksm pause HARNESS on|off\n");
        return 2;
    }
    xksm *x = open_store();
    if (!x)
        return 1;
    char key[96];
    snprintf(key, sizeof key, "%s:paused", harness);
    xksm_status s = xksm_kv_put(x, key, on ? "1" : "0", 1);
    printf("%s logging %s\n", harness, on ? "paused" : "active");
    xksm_close(x);
    return s != XKSM_OK;
}

/* ---- MCP server (JSON-RPC 2.0 over stdio, one message per line) ------------------------- */
typedef struct rpc {
    buf method, id, name, query, text, scope, proto;
    int id_kind;
    long limit;
} rpc;

static int rpc_cb(void *ctx, const char *path, size_t pl, int kind, const char *v, size_t vl)
{
    rpc *r = ctx;
    if (pis(path, pl, "id")) {
        r->id_kind = kind;
        return bput(&r->id, v, vl);
    }
    if (kind == XKSM_JSON_STRING) {
        if (pis(path, pl, "method"))
            return bput(&r->method, v, vl);
        if (pis(path, pl, "params.name"))
            return bput(&r->name, v, vl);
        if (pis(path, pl, "params.protocolVersion"))
            return bput(&r->proto, v, vl);
        if (pis(path, pl, "params.arguments.query"))
            return bput(&r->query, v, vl);
        if (pis(path, pl, "params.arguments.text"))
            return bput(&r->text, v, vl);
        if (pis(path, pl, "params.arguments.scope"))
            return bput(&r->scope, v, vl);
    }
    if (pis(path, pl, "params.arguments.limit") && kind == XKSM_JSON_NUMBER)
        r->limit = strtol(v, NULL, 10);
    return 1;
}

static void rpc_free(rpc *r)
{
    free(r->method.p);
    free(r->id.p);
    free(r->name.p);
    free(r->query.p);
    free(r->text.p);
    free(r->scope.p);
    free(r->proto.p);
}

static void send_line(const buf *b)
{
    fwrite(bs(b), 1, b->n, stdout);
    fputc('\n', stdout);
    fflush(stdout);
}

static void put_id(buf *o, const rpc *r)
{
    if (r->id_kind == XKSM_JSON_STRING) {
        bputs(o, "\"");
        bjson(o, bs(&r->id), r->id.n);
        bputs(o, "\"");
    } else {
        bput(o, bs(&r->id), r->id.n);
    }
}

static void reply(const rpc *r, const char *result_json)
{
    buf o = {0};
    bputs(&o, "{\"jsonrpc\":\"2.0\",\"id\":");
    put_id(&o, r);
    bputs(&o, ",\"result\":");
    bputs(&o, result_json);
    bputs(&o, "}");
    send_line(&o);
    free(o.p);
}

static void reply_error(const rpc *r, int code, const char *msg)
{
    buf o = {0};
    bputs(&o, "{\"jsonrpc\":\"2.0\",\"id\":");
    if (r && r->id.n)
        put_id(&o, r);
    else
        bputs(&o, "null");
    char c[64];
    snprintf(c, sizeof c, ",\"error\":{\"code\":%d,\"message\":\"", code);
    bputs(&o, c);
    bjson(&o, msg, strlen(msg));
    bputs(&o, "\"}}");
    send_line(&o);
    free(o.p);
}

static void reply_text(const rpc *r, const char *text, size_t n, int is_error)
{
    buf o = {0};
    bputs(&o, "{\"content\":[{\"type\":\"text\",\"text\":\"");
    bjson(&o, text, n);
    bputs(&o, is_error ? "\"}],\"isError\":true}" : "\"}],\"isError\":false}");
    reply(r, o.p);
    free(o.p);
}

static const char k_tools[] =
    "{\"tools\":["
    "{\"name\":\"recall\",\"description\":\"Recall earlier turns of this agent harness from the "
    "local XKS-Memory store by meaning and words (hybrid BM25 + word-vector search). Each "
    "result shows when it happened (local time with UTC offset), the role, the session and the "
    "text. scope: 'dialogue' (default) = prompts, replies, notes; 'all' = also tool calls, tool "
    "results and thinking; 'all-harnesses' = every harness and imported document.\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\","
    "\"description\":\"What to recall\"},\"limit\":{\"type\":\"integer\",\"description\":"
    "\"Results, default 8, at most 50\"},\"scope\":{\"type\":\"string\",\"enum\":[\"dialogue\","
    "\"all\",\"all-harnesses\"]}},\"required\":[\"query\"]}},"
    "{\"name\":\"search\",\"description\":\"Exact word search (BM25) over the local XKS-Memory "
    "store, for names, identifiers, error strings and dates such as 2026-11-05. Same scopes as "
    "recall.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"query\":{\"type\":"
    "\"string\"},\"limit\":{\"type\":\"integer\"},\"scope\":{\"type\":\"string\",\"enum\":"
    "[\"dialogue\",\"all\",\"all-harnesses\"]}},\"required\":[\"query\"]}},"
    "{\"name\":\"remember\",\"description\":\"Store a note in the local XKS-Memory store with "
    "the current time, for later recall.\",\"inputSchema\":{\"type\":\"object\","
    "\"properties\":{\"text\":{\"type\":\"string\"}},\"required\":[\"text\"]}},"
    "{\"name\":\"sessions\",\"description\":\"List the recorded sessions of this harness, "
    "most recent first, with turn counts and time ranges.\",\"inputSchema\":{\"type\":"
    "\"object\",\"properties\":{\"limit\":{\"type\":\"integer\"}}}},"
    "{\"name\":\"stats\",\"description\":\"Size and state of the local XKS-Memory store.\","
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{}}}"
    "]}";

static void tool_call(const hctx *h, const rpc *r)
{
    const char *name = bs(&r->name), *scope = bs(&r->scope);
    size_t limit = r->limit > 0 ? (size_t)(r->limit > 50 ? 50 : r->limit) : 8;
    int all_h = !strcmp(scope, "all-harnesses"), all = !strcmp(scope, "all");
    buf out = {0};
    if (!strcmp(name, "recall") || !strcmp(name, "search")) {
        if (!r->query.n) {
            reply_text(r, "query is required", 17, 1);
            return;
        }
        /* the dialogue namespace, plus the trace namespace for 'all' (two lists merged by
         * fused order is not defined across namespaces: show dialogue first, then trace) */
        const char *nss[2] = {all_h ? NULL : h->ns, all ? h->ns_trace : NULL};
        size_t shown = 0;
        for (int q = 0; q < 2; ++q) {
            if (q == 1 && !nss[1])
                break;
            if (!strcmp(name, "recall")) {
                shown += recall_text(h->x, bs(&r->query), nss[q], NULL, limit, 1500, 60000, &out);
            } else {
                xksm_search_opts so = {0};
                so.ns = nss[q];
                so.limit = limit;
                xksm_search_result sr;
                if (xksm_search(h->x, bs(&r->query), &so, &sr) == XKSM_OK) {
                    xksm_id *ids = malloc(sizeof *ids * (sr.count ? sr.count : 1));
                    if (ids) {
                        for (size_t i = 0; i < sr.count; ++i)
                            ids[i] = sr.hits[i].id;
                        shown += format_hits(h->x, ids, sr.count, NULL, limit, 1500, 60000,
                                             nss[q] == NULL, &out);
                        free(ids);
                    }
                    xksm_search_free(&sr);
                }
            }
        }
        if (!shown)
            bputs(&out, "no matching memories");
        reply_text(r, bs(&out), out.n, 0);
    } else if (!strcmp(name, "remember")) {
        if (!r->text.n) {
            reply_text(r, "text is required", 16, 1);
            return;
        }
        int64_t t = now_us();
        xksm_status s = store_turn(h, 0, "note", "notes", NULL, bs(&r->text), r->text.n, t);
        maintain(h->x, 0);
        char tm[40], msg[160];
        fmt_time(t, tm);
        snprintf(msg, sizeof msg, s == XKSM_OK ? "stored at %s" : "not stored: %s",
                 s == XKSM_OK ? tm : xksm_errmsg(h->x));
        reply_text(r, msg, strlen(msg), s != XKSM_OK);
    } else if (!strcmp(name, "sessions")) {
        xksm_session_info *l = NULL;
        size_t n = 0;
        if (xksm_session_list(h->x, h->harness, r->limit > 0 ? (size_t)r->limit : 20, &l, &n) ==
            XKSM_OK)
            for (size_t i = 0; i < n; ++i) {
                char a[40], b[40], ln[512];
                fmt_time(l[i].first_us, a);
                fmt_time(l[i].last_us, b);
                int k = snprintf(ln, sizeof ln, "session %s: %lld turns, %s .. %s%s%s\n", l[i].id,
                                 (long long)l[i].turns, a, b, l[i].cwd ? ", " : "",
                                 l[i].cwd ? l[i].cwd : "");
                if (k > 0)
                    bput(&out, ln, (size_t)k < sizeof ln ? (size_t)k : sizeof ln - 1);
            }
        xksm_session_list_free(l, n);
        if (!out.n)
            bputs(&out, "no sessions recorded");
        reply_text(r, bs(&out), out.n, 0);
    } else if (!strcmp(name, "stats")) {
        xksm_cache_info ci;
        xksm_cache_stats(h->x, &ci);
        char path[4096], msg[4600];
        store_path(path, sizeof path);
        int n = snprintf(msg, sizeof msg,
                         "store %s: %lld memories in total; harness %s; word-vector table built "
                         "from %zu memories (dimension %zu); logging %s",
                         path, (long long)xksm_memory_count(h->x, NULL), h->harness,
                         xksm_semantic_built_memories(h->x), xksm_semantic_dim(h->x),
                         paused(h) ? "paused" : "active");
        reply_text(r, msg, (size_t)(n > 0 ? n : 0), 0);
    } else {
        reply_error(r, -32602, "unknown tool");
    }
    free(out.p);
}

int xksm_cli_mcp(const char *harness)
{
    xksm *x = open_store();
    hctx h;
    if (!x || !hctx_init(&h, x, harness ? harness : DEFAULT_HARNESS)) {
        xksm_close(x);
        return 1;
    }
    char *ln = NULL;
    size_t cap = 0;
    ssize_t len;
    while ((len = getline(&ln, &cap, stdin)) >= 0) {
        while (len && (ln[len - 1] == '\n' || ln[len - 1] == '\r'))
            ln[--len] = 0;
        if (!len)
            continue;
        rpc r = {0};
        if (xksm__json_walk(ln, (size_t)len, rpc_cb, &r, NULL, NULL) != 1) {
            reply_error(NULL, -32700, "parse error");
            rpc_free(&r);
            continue;
        }
        const char *m = bs(&r.method);
        if (!strcmp(m, "initialize")) {
            buf o = {0};
            bputs(&o, "{\"protocolVersion\":\"");
            if (r.proto.n)
                bjson(&o, bs(&r.proto), r.proto.n);
            else
                bputs(&o, "2025-06-18");
            bputs(&o, "\",\"capabilities\":{\"tools\":{}},\"serverInfo\":{\"name\":\"xks-memory\","
                      "\"version\":\"" VERSION_STR "\"},\"instructions\":\"Local long-term memory "
                      "of earlier turns of this agent harness. Use recall to find what was said or "
                      "done before, search for exact names or dates, remember to store a note.\"}");
            reply(&r, o.p);
            free(o.p);
        } else if (!strcmp(m, "tools/list")) {
            reply(&r, k_tools);
        } else if (!strcmp(m, "tools/call")) {
            tool_call(&h, &r);
        } else if (!strcmp(m, "ping")) {
            reply(&r, "{}");
        } else if (r.id.n) {
            reply_error(&r, -32601, "method not found");
        } /* notifications have no id and get no answer */
        rpc_free(&r);
    }
    free(ln);
    xksm_close(x);
    return 0;
}

const char *xksm_cli_harness_version(void) { return VERSION_STR; }
