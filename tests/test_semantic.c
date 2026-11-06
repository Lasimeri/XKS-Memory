/* test_semantic.c: the count-based semantic model on a synthetic corpus with planted
 * topics: same-topic memories must be nearer than cross-topic ones; query embedding,
 * persistence across reopen, error paths. argv[1] = scratch store. */
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

static uint64_t rng = 0x9e3779b97f4a7c15u;
static uint64_t next(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
}

enum { TOPICS = 6, WORDS = 40, MEMS = 900, FILLERS = 30 };

int main(int argc, char **argv)
{
    if (argc != 2)
        return 2;
    remove_db(argv[1]);
    xksm *x = NULL;
    CHECK(xksm_open(argv[1], &x) == XKSM_OK);
    float q[64];
    CHECK(xksm_semantic_embed(x, "t0w1", 4, q, 64) == XKSM_ENOTFOUND); /* no model yet */
    CHECK(xksm_semantic_dim(x) == 0);

    /* memory m belongs to topic m % 6; its words come from that topic's 40-word pool plus
     * shared filler words, so topic words co-occur only within their topic */
    static xksm_id id[MEMS];
    for (int m = 0; m < MEMS; ++m) {
        char t[1024];
        int o = snprintf(t, sizeof t, "m%04d", m);
        for (int w = 0; w < 30; ++w) {
            if (next() % 3 == 0)
                o += snprintf(t + o, sizeof t - (size_t)o, " filler%02d", (int)(next() % FILLERS));
            else
                o += snprintf(t + o, sizeof t - (size_t)o, " t%dw%02d", m % TOPICS,
                              (int)(next() % WORDS));
        }
        xksm_memory_in in = {0};
        in.category = XKSM_CAT_SEMANTIC;
        in.text = t;
        in.text_len = (size_t)o;
        CHECK(xksm_memory_add(x, &in, &id[m]) == XKSM_OK);
    }
    xksm_semantic_opts so = {0};
    so.dim = 32;
    xksm_semantic_report rep;
    CHECK(xksm_semantic_build(x, &so, &rep) == XKSM_OK);
    printf("model: %zu memories, %zu embedded, vocab %zu, nnz %llu, %.3f s\n", rep.memories,
           rep.embedded, rep.vocab, (unsigned long long)rep.nnz, rep.t_total);
    CHECK(rep.memories == MEMS && rep.embedded == MEMS && xksm_semantic_dim(x) == 32);

    /* a query made of topic-3 words retrieves topic-3 memories */
    int good = 0, total = 0;
    for (int topic = 0; topic < TOPICS; ++topic) {
        char qt[128];
        snprintf(qt, sizeof qt, "t%dw03 t%dw17 t%dw29", topic, topic, topic);
        CHECK(xksm_semantic_embed(x, qt, strlen(qt), q, 32) == XKSM_OK);
        xksm_vector_opts vo = {NULL, 0, 10, 0};
        xksm_search_result r;
        CHECK(xksm_vector_search(x, XKSM_SEMANTIC_MODEL, q, 32, &vo, &r) == XKSM_OK);
        for (size_t h = 0; h < r.count; ++h) {
            int m = 0;
            while (m < MEMS && id[m] != r.hits[h].id)
                ++m;
            good += m < MEMS && m % TOPICS == topic;
            ++total;
        }
        xksm_search_free(&r);
    }
    printf("topic precision@10: %d / %d\n", good, total);
    CHECK(total == 60 && good >= 57);

    /* persisted: a fresh handle embeds identically */
    float a[32], b[32];
    CHECK(xksm_semantic_embed(x, "t2w05 filler01", 14, a, 32) == XKSM_OK);
    xksm_close(x);
    CHECK(xksm_open(argv[1], &x) == XKSM_OK);
    CHECK(xksm_semantic_embed(x, "t2w05 filler01", 14, b, 32) == XKSM_OK);
    CHECK(!memcmp(a, b, sizeof a));

    /* semantic port: a hand-made XKSMPRT1 file (dim 8, bucket code): topic words tXwYY point
     * at axis X, the unknown word zzalpha at axis 3; recall of zzalpha must find topic 3 */
    {
        char pp[4096];
        snprintf(pp, sizeof pp, "%s.port", argv[1]);
        enum { PD = 8, PN = TOPICS * WORDS + 1 };
        static float pv[PN * PD];
        FILE *pf = fopen(pp, "wb");
        CHECK(pf != NULL);
        uint32_t d32 = PD, n32 = PN;
        size_t off = 16;
        fwrite("XKSMPRT1", 1, 8, pf);
        fwrite(&d32, 4, 1, pf);
        fwrite(&n32, 4, 1, pf);
        for (int i = 0; i < PN; ++i) {
            char kw[16];
            int kl = i < TOPICS * WORDS ? snprintf(kw, sizeof kw, "t%dw%02d", i / WORDS, i % WORDS)
                                        : snprintf(kw, sizeof kw, "zzalpha");
            uint8_t hdr[2] = {11, (uint8_t)kl};
            fwrite(hdr, 1, 2, pf);
            fwrite(kw, 1, (size_t)kl, pf);
            off += 2u + (size_t)kl;
            float *v = pv + (size_t)i * PD;
            if (i < TOPICS * WORDS) {
                v[i / WORDS] = 0.995f;
                v[6 + (i % 2)] = 0.0999f;
            } else {
                v[3] = 1.0f;
            }
        }
        static const char zero[64];
        fwrite(zero, 1, (64 - off % 64) % 64, pf);
        fwrite(pv, sizeof pv, 1, pf);
        fclose(pf);

        size_t ex_all = 0, ex_unk = 99;
        float e[32];
        CHECK(xksm_semantic_port_set(x, pp) == XKSM_OK);
        CHECK(xksm_semantic_embed_expanded(x, "t3w05", 5, NULL, e, 32, &ex_all) == XKSM_OK);
        xksm_expand_opts uo = {0};
        uo.unknown_only = 1;
        CHECK(xksm_semantic_embed_expanded(x, "t3w05", 5, &uo, e, 32, &ex_unk) == XKSM_OK);
        printf("port: known word expanded by %zu (default), %zu (unknown_only)\n", ex_all, ex_unk);
        CHECK(ex_all == 5 && ex_unk == 0);
        for (int pass = 0; pass < 3; ++pass) { /* 0 now, 1 after reopen, 2 after a rebuild */
            if (pass == 1) {
                xksm_close(x);
                CHECK(xksm_open(argv[1], &x) == XKSM_OK);
            } else if (pass == 2) {
                CHECK(xksm_semantic_build(x, &so, &rep) == XKSM_OK);
            }
            xksm_hybrid_result r;
            CHECK(xksm_semantic_recall(x, "zzalpha", NULL, NULL, &r) == XKSM_OK);
            int hit3 = 0;
            for (size_t h = 0; h < r.count; ++h) {
                int m = 0;
                while (m < MEMS && id[m] != r.hits[h].id)
                    ++m;
                hit3 += m < MEMS && m % TOPICS == 3;
            }
            printf("port recall (pass %d): unknown word -> %d / %zu topic-3 memories\n", pass, hit3,
                   r.count);
            CHECK(r.count == 10 && hit3 >= 9);
            xksm_hybrid_free(&r);
        }
        CHECK(xksm_semantic_embed_expanded(x, "zzalpha", 7, &uo, e, 32, NULL) == XKSM_OK);
        /* recall == hybrid search over the expanded query vector, hit for hit */
        {
            const char *qq = "t4w03 t4w11 filler02";
            xksm_hybrid_result ra, rb;
            CHECK(xksm_semantic_recall(x, qq, NULL, NULL, &ra) == XKSM_OK);
            CHECK(xksm_semantic_embed_expanded(x, qq, strlen(qq), NULL, e, 32, NULL) == XKSM_OK);
            CHECK(xksm_hybrid_search(x, qq, XKSM_SEMANTIC_MODEL, e, 32, NULL, &rb) == XKSM_OK);
            int same = ra.count == rb.count && ra.count > 0;
            for (size_t h = 0; same && h < ra.count; ++h)
                same = ra.hits[h].id == rb.hits[h].id && ra.hits[h].score == rb.hits[h].score;
            CHECK(same);
            xksm_hybrid_free(&ra);
            xksm_hybrid_free(&rb);
            /* nothing known or related: lexical list alone, no error */
            CHECK(xksm_semantic_recall(x, "m0007", NULL, NULL, &ra) == XKSM_OK);
            CHECK(ra.count >= 1);
            xksm_hybrid_free(&ra);
        }
        pf = fopen(pp, "wb");
        CHECK(pf != NULL);
        fputs("not a port", pf);
        fclose(pf);
        CHECK(xksm_semantic_port_set(x, pp) == XKSM_EIO);
        remove(pp);
    }

    /* errors */
    CHECK(xksm_semantic_embed(x, "zzzunknown", 10, a, 32) == XKSM_EINVAL);
    CHECK(xksm_semantic_embed(x, "t2w05", 5, a, 31) == XKSM_EINVAL);
    xksm_close(x);
    remove_db(argv[1]);
    return test_report("semantic");
}
