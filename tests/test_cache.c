/* test_cache.c: Tier-1 record cache = 25 % of records, CLOCK behaviour, delete
 * propagation, copy independence, capacity tracking, randomized oracle check.
 * argv[1] = scratch database path. */
#include "test.h"

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

static xksm_cache_info stats(xksm *x)
{
    xksm_cache_info i;
    xksm_cache_stats(x, &i);
    return i;
}

/* add memories "record N" for N in [from, to); ids[N] receives each id */
static int add_range(xksm *x, int from, int to, xksm_id *ids)
{
    int n = to - from;
    xksm_memory_in *in = calloc((size_t)n, sizeof *in);
    char (*t)[32] = malloc(sizeof *t * (size_t)n);
    xksm_status *st = malloc(sizeof *st * (size_t)n);
    int ok = in && t && st;
    for (int i = 0; ok && i < n; ++i) {
        int len = snprintf(t[i], sizeof t[i], "record %d", from + i);
        in[i].category = XKSM_CAT_EPISODIC;
        in[i].text = t[i];
        in[i].text_len = (size_t)len;
    }
    ok = ok && xksm_memory_add_batch(x, in, (size_t)n, ids + from, st) == XKSM_OK;
    for (int i = 0; ok && i < n; ++i)
        ok = st[i] == XKSM_OK;
    free(in);
    free(t);
    free(st);
    return ok;
}

static int text_is(xksm *x, xksm_id id, int n)
{
    char want[32];
    snprintf(want, sizeof want, "record %d", n);
    xksm_memory m;
    if (xksm_memory_get(x, id, &m) != XKSM_OK)
        return 0;
    int ok = !strcmp(m.text, want) && m.id == id;
    xksm_memory_free(&m);
    return ok;
}

int main(int argc, char **argv)
{
    if (argc != 2)
        return 2;
    const char *path = argv[1];
    remove_db(path);
    xksm *x = NULL;
    CHECK(xksm_open(path, &x) == XKSM_OK);
    CHECK(stats(x).capacity == 0 && stats(x).resident == 0);

    enum { N = 1200 };
    static xksm_id id[N];
    static int alive[N];
    CHECK(add_range(x, 0, 100, id));
    CHECK(stats(x).records == 100 && stats(x).capacity == 25);

    /* sequential scan: every get misses, 25 stay, the last 25 ids */
    for (int i = 0; i < 100; ++i)
        CHECK(text_is(x, id[i], i));
    xksm_cache_info s = stats(x);
    CHECK(s.misses == 100 && s.hits == 0 && s.resident == 25 && s.evictions == 75);
    for (int i = 75; i < 100; ++i)
        CHECK(text_is(x, id[i], i));
    s = stats(x);
    CHECK(s.hits == 25 && s.misses == 100);

    /* delete of a resident record is never served from the cache afterwards */
    CHECK(text_is(x, id[99], 99));
    CHECK(xksm_memory_delete(x, id[99]) == XKSM_OK);
    xksm_memory m;
    CHECK(xksm_memory_get(x, id[99], &m) == XKSM_ENOTFOUND);
    CHECK(stats(x).records == 99 && stats(x).capacity == 25);

    /* a returned copy is the caller's: scribbling on it does not reach the cache */
    CHECK(xksm_memory_get(x, id[98], &m) == XKSM_OK);
    m.text[0] = 'X';
    xksm_memory_free(&m);
    CHECK(text_is(x, id[98], 98));

    /* capacity follows the store: grow, then shrink with immediate eviction */
    CHECK(add_range(x, 100, 105, id));
    CHECK(stats(x).records == 104 && stats(x).capacity == 26);
    for (int i = 0; i < 30; ++i)
        CHECK(xksm_memory_delete(x, id[i]) == XKSM_OK);
    s = stats(x);
    CHECK(s.records == 74 && s.capacity == 19 && s.resident <= 19);

    /* reopen: empty cache, capacity from the stored count */
    xksm_close(x);
    CHECK(xksm_open(path, &x) == XKSM_OK);
    s = stats(x);
    CHECK(s.records == 74 && s.capacity == 19 && s.resident == 0);

    /* randomized oracle: gets and deletes against ground truth */
    CHECK(add_range(x, 105, N, id));
    for (int i = 0; i < N; ++i)
        alive[i] = !(i < 30 || i == 99);
    uint64_t r = 0x2545F4914F6CDD1Du;
    int bad = 0, cap_violations = 0;
    for (int op = 0; op < 3000; ++op) {
        r ^= r << 13; r ^= r >> 7; r ^= r << 17;
        int k = (int)(r % N);
        if (r % 10 == 0 && alive[k]) {
            bad += xksm_memory_delete(x, id[k]) != XKSM_OK;
            alive[k] = 0;
        } else if (alive[k]) {
            bad += !text_is(x, id[k], k);
        } else {
            bad += xksm_memory_get(x, id[k], &m) != XKSM_ENOTFOUND;
        }
        s = stats(x);
        cap_violations += s.resident > s.capacity;
    }
    printf("random: %d wrong results, %d capacity violations; hits %llu misses %llu "
           "evictions %llu, resident %u / capacity %u of %lld records\n",
           bad, cap_violations, (unsigned long long)s.hits, (unsigned long long)s.misses,
           (unsigned long long)s.evictions, s.resident, s.capacity, (long long)s.records);
    CHECK(bad == 0 && cap_violations == 0);
    int64_t live = 0;
    for (int i = 0; i < N; ++i)
        live += alive[i];
    CHECK(s.records == live && s.capacity == (uint32_t)((live + 3) / 4));

    /* optional byte ceiling: off by default; when set it evicts at once and holds */
    CHECK(s.byte_limit == 0 && s.bytes > 0);
    size_t per = s.bytes / s.resident, lim = per * 40;
    xksm_cache_set_byte_limit(x, lim);
    s = stats(x);
    CHECK(s.byte_limit == lim && s.bytes <= lim && s.resident <= 40 && s.resident > 0);
    int over = 0;
    for (int i = 0; i < N; ++i) {
        if (!alive[i])
            continue;
        bad += !text_is(x, id[i], i);
        s = stats(x);
        over += s.bytes > lim || s.resident > s.capacity;
    }
    CHECK(bad == 0 && over == 0);
    /* ceiling off again: the cache refills to exactly 25 % of the records */
    xksm_cache_set_byte_limit(x, 0);
    for (int pass = 0; pass < 2; ++pass)
        for (int i = 0; i < N; ++i)
            if (alive[i])
                (void)text_is(x, id[i], i);
    s = stats(x);
    CHECK(s.byte_limit == 0 && s.resident == s.capacity);
    /* percentage: clamped to 5..50, capacity follows, shrinking evicts immediately */
    CHECK(stats(x).percent == 25);
    CHECK(xksm_cache_set_percent(x, 80) == 50);
    s = stats(x);
    CHECK(s.percent == 50 && s.capacity == (uint32_t)((s.records * 50 + 99) / 100));
    for (int pass = 0; pass < 2; ++pass)
        for (int i = 0; i < N; ++i)
            if (alive[i])
                (void)text_is(x, id[i], i);
    s = stats(x);
    CHECK(s.resident == s.capacity); /* grew into the larger share */
    CHECK(xksm_cache_set_percent(x, 1) == 5);
    s = stats(x);
    CHECK(s.percent == 5 && s.capacity == (uint32_t)((s.records * 5 + 99) / 100) &&
          s.resident <= s.capacity);
    int bad5 = 0;
    for (int i = 0; i < N; ++i)
        if (alive[i])
            bad5 += !text_is(x, id[i], i);
    CHECK(bad5 == 0 && stats(x).resident <= stats(x).capacity);
    CHECK(xksm_cache_set_percent(x, XKSM_CACHE_PCT_DEFAULT) == 25);

    /* a ceiling smaller than one record: nothing is cached, reads still work */
    xksm_cache_set_byte_limit(x, 8);
    s = stats(x);
    CHECK(s.resident == 0 && s.bytes == 0);
    CHECK(text_is(x, id[200], 200) || !alive[200]);
    CHECK(stats(x).resident == 0);

    xksm_close(x);
    remove_db(path);
    return test_report("cache");
}
