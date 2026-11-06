/* cache.c: Tier-1 RAM record cache. Capacity = ceil(records / 4) (25 % of the store),
 * read-through, CLOCK eviction, open-addressing index with Fibonacci hashing and
 * backward-shift deletion (no tombstones). Not thread-safe: one handle per thread. */
#include "internal.h"

#include <stdlib.h>
#include <string.h>

typedef struct entry {
    xksm_memory m;   /* m.ns and m.text point into blob */
    char *blob;
    uint32_t bytes;  /* RAM charged to this entry (blob + entry + index slots) */
    uint8_t ref;     /* CLOCK reference bit */
} entry;

struct xksm_cache {
    entry *e;        /* dense entries [0, size) */
    uint32_t size, cap, hand;
    int32_t *slot;   /* index table: entry number or -1; length 1 << bits */
    unsigned bits;
    int64_t records; /* rows in the store, drives cap */
    uint64_t hits, misses, evictions;
    size_t bytes, limit; /* RAM in use; optional ceiling (0 = none, the default) */
};

static inline uint32_t hash_id(int64_t id, unsigned bits)
{
    return (uint32_t)(((uint64_t)id * 0x9E3779B97F4A7C15ull) >> (64 - bits));
}

static uint32_t target_cap(int64_t records)
{
    int64_t t = (records + 3) / 4; /* ceil(records / 4) */
    return t > (int64_t)UINT32_MAX / 4 ? UINT32_MAX / 4 : (uint32_t)t;
}

/* table slot holding id, or the empty slot where it would go */
static uint32_t find(const struct xksm_cache *c, int64_t id)
{
    uint32_t mask = (1u << c->bits) - 1, i = hash_id(id, c->bits);
    while (c->slot[i] >= 0 && c->e[c->slot[i]].m.id != id)
        i = (i + 1) & mask;
    return i;
}

/* remove table slot i, shifting later members of the probe run back into the gap */
static void slot_remove(struct xksm_cache *c, uint32_t i)
{
    uint32_t mask = (1u << c->bits) - 1, j = i;
    c->slot[i] = -1;
    for (;;) {
        j = (j + 1) & mask;
        if (c->slot[j] < 0)
            return;
        uint32_t home = hash_id(c->e[c->slot[j]].m.id, c->bits);
        /* move j into the gap at i unless its home lies cyclically in (i, j] */
        if (((j - home) & mask) >= ((j - i) & mask)) {
            c->slot[i] = c->slot[j];
            c->slot[j] = -1;
            i = j;
        }
    }
}

static int rehash(struct xksm_cache *c, unsigned bits)
{
    int32_t *s = malloc(sizeof(int32_t) << bits);
    if (!s)
        return 0;
    memset(s, 0xff, sizeof(int32_t) << bits);
    free(c->slot);
    c->slot = s;
    c->bits = bits;
    for (uint32_t k = 0; k < c->size; ++k)
        c->slot[find(c, c->e[k].m.id)] = (int32_t)k;
    return 1;
}

/* drop entry k: move the last entry into its place to keep the array dense */
static void drop(struct xksm_cache *c, uint32_t k)
{
    slot_remove(c, find(c, c->e[k].m.id));
    c->bytes -= c->e[k].bytes;
    free(c->e[k].blob);
    uint32_t last = --c->size;
    if (k != last) {
        c->e[k] = c->e[last];
        c->slot[find(c, c->e[k].m.id)] = (int32_t)k;
    }
    if (c->hand >= c->size)
        c->hand = 0;
}

/* CLOCK: clear reference bits until an unreferenced entry is found, unlink and free it,
 * advance the hand past it, and return its index as a hole for in-place replacement.
 * Terminates: one full sweep clears every bit. Requires size > 0. */
static uint32_t victim(struct xksm_cache *c)
{
    for (;;) {
        if (c->hand >= c->size)
            c->hand = 0;
        entry *e = &c->e[c->hand];
        if (e->ref) {
            e->ref = 0;
            ++c->hand;
            continue;
        }
        uint32_t k = c->hand++;
        slot_remove(c, find(c, e->m.id));
        c->bytes -= e->bytes;
        free(e->blob);
        ++c->evictions;
        return k;
    }
}

/* shrink path: evict by CLOCK, then close the hole by moving the last entry into it */
static void evict_one(struct xksm_cache *c)
{
    uint32_t k = victim(c), last = --c->size;
    if (k != last) {
        c->e[k] = c->e[last];
        c->slot[find(c, c->e[k].m.id)] = (int32_t)k;
    }
    if (c->hand >= c->size)
        c->hand = 0;
}

/* apply the 25 % rule after the record count changed */
static void resize(struct xksm_cache *c)
{
    uint32_t want = target_cap(c->records);
    while (c->size > want)
        evict_one(c);
    if (want > c->cap) {
        uint32_t grow = c->cap ? c->cap : 16;
        while (grow < want)
            grow *= 2;
        entry *e = realloc(c->e, sizeof(entry) * grow);
        if (!e)
            return; /* keep the old capacity; the cache is an optimization */
        c->e = e;
        unsigned bits = c->bits ? c->bits : 5;
        while ((1u << bits) < 2 * grow) /* load factor <= 0.5 */
            ++bits;
        if (bits != c->bits && !rehash(c, bits))
            return;
        c->cap = grow;
    }
}

xksm_status xksm__cache_init(xksm *x)
{
    struct xksm_cache *c = calloc(1, sizeof *c);
    if (!c)
        return XKSM_ENOMEM;
    x->cache = c;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(x->db, "SELECT count(*) FROM memory", -1, &st, NULL) != SQLITE_OK ||
        sqlite3_step(st) != SQLITE_ROW) {
        sqlite3_finalize(st);
        return xksm__fail(x, XKSM_EIO, "count records: %s", sqlite3_errmsg(x->db));
    }
    c->records = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    resize(c);
    return XKSM_OK;
}

void xksm__cache_free(xksm *x)
{
    struct xksm_cache *c = x->cache;
    if (!c)
        return;
    for (uint32_t k = 0; k < c->size; ++k)
        free(c->e[k].blob);
    free(c->e);
    free(c->slot);
    free(c);
    x->cache = NULL;
}

void xksm__cache_records_delta(xksm *x, int64_t delta)
{
    struct xksm_cache *c = x->cache;
    if (!c)
        return;
    c->records += delta;
    if (c->records < 0)
        c->records = 0;
    resize(c);
}

/* copy out: caller-owned strings, freed by xksm_memory_free */
static int copy_out(const xksm_memory *src, xksm_memory *dst)
{
    *dst = *src;
    size_t nl = strlen(src->ns);
    dst->ns = malloc(nl + 1);
    dst->text = malloc(src->text_len + 1);
    if (!dst->ns || !dst->text) {
        free(dst->ns);
        free(dst->text);
        dst->ns = dst->text = NULL;
        return 0;
    }
    memcpy(dst->ns, src->ns, nl + 1);
    memcpy(dst->text, src->text, src->text_len + 1);
    return 1;
}

int xksm__cache_get(xksm *x, xksm_id id, xksm_memory *out, xksm_status *rc)
{
    struct xksm_cache *c = x->cache;
    if (!c || !c->size) {
        if (c)
            ++c->misses;
        return 0;
    }
    int32_t k = c->slot[find(c, id)];
    if (k < 0) {
        ++c->misses;
        return 0;
    }
    c->e[k].ref = 1;
    ++c->hits;
    *rc = copy_out(&c->e[k].m, out) ? XKSM_OK : XKSM_ENOMEM;
    return 1;
}

void xksm__cache_put(xksm *x, const xksm_memory *m)
{
    struct xksm_cache *c = x->cache;
    if (!c || c->cap == 0 || target_cap(c->records) == 0)
        return;
    if (c->slot[find(c, m->id)] >= 0)
        return; /* already resident */
    size_t nl = strlen(m->ns);
    size_t blen = nl + 1 + m->text_len + 1;
    size_t cost = blen + sizeof(entry) + 2 * sizeof(int32_t);
    if (c->limit && cost > c->limit)
        return; /* larger than the whole ceiling: never cached */
    while (c->limit && c->size && c->bytes + cost > c->limit)
        evict_one(c); /* make room under the byte ceiling */
    char *blob = malloc(blen);
    if (!blob)
        return;
    memcpy(blob, m->ns, nl + 1);
    memcpy(blob + nl + 1, m->text, m->text_len + 1);
    uint32_t k;
    if (c->size < target_cap(c->records) && c->size < c->cap)
        k = c->size++;           /* room: append */
    else
        k = victim(c);           /* full: replace the CLOCK victim in place */
    entry *e = &c->e[k];
    e->m = *m;
    e->m.ns = blob;
    e->m.text = blob + nl + 1;
    e->blob = blob;
    e->bytes = (uint32_t)cost;
    e->ref = 0; /* earns its bit on the first hit; a one-time scan is evicted first */
    c->bytes += cost;
    c->slot[find(c, m->id)] = (int32_t)k;
}

void xksm__cache_evict(xksm *x, xksm_id id)
{
    struct xksm_cache *c = x->cache;
    if (!c || !c->size)
        return;
    int32_t k = c->slot[find(c, id)];
    if (k >= 0)
        drop(c, (uint32_t)k);
}

void xksm_cache_set_byte_limit(xksm *x, size_t bytes)
{
    struct xksm_cache *c = x ? x->cache : NULL;
    if (!c)
        return;
    c->limit = bytes;
    while (c->limit && c->size && c->bytes > c->limit)
        evict_one(c);
}

void xksm_cache_stats(const xksm *x, xksm_cache_info *out)
{
    memset(out, 0, sizeof *out);
    const struct xksm_cache *c = x ? x->cache : NULL;
    if (!c)
        return;
    out->records = c->records;
    out->capacity = target_cap(c->records);
    out->resident = c->size;
    out->hits = c->hits;
    out->misses = c->misses;
    out->evictions = c->evictions;
    out->bytes = c->bytes;
    out->byte_limit = c->limit;
}
