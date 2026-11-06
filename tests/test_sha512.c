/* test_sha512.c
 *   no arguments: FIPS 180-2 vectors, then chunked streaming equals one-shot;
 *   FILE...:      print "<hex>  <file>" per file (sha512sum format) for cross-checks. */
#include "test.h"

#include "sha512.h"

#include <stdlib.h>
#include <string.h>

static void hex(const uint8_t d[64], char out[129])
{
    static const char digits[] = "0123456789abcdef";
    for (int i = 0; i < 64; ++i) {
        out[2 * i] = digits[d[i] >> 4];
        out[2 * i + 1] = digits[d[i] & 15];
    }
    out[128] = 0;
}

static int vec(const void *msg, size_t n, const char *want)
{
    uint8_t d[64];
    char h[129];
    xksm__sha512(msg, n, d);
    hex(d, h);
    if (strcmp(h, want))
        fprintf(stderr, "  got  %s\n  want %s\n", h, want);
    return !strcmp(h, want);
}

static int hash_files(int n, char **paths)
{
    static uint8_t buf[1 << 16];
    int bad = 0;
    for (int i = 0; i < n; ++i) {
        FILE *f = fopen(paths[i], "rb");
        if (!f) {
            perror(paths[i]);
            bad = 1;
            continue;
        }
        xksm_sha512 c;
        uint8_t d[64];
        char h[129];
        size_t got;
        xksm__sha512_init(&c);
        while ((got = fread(buf, 1, sizeof buf, f)) > 0)
            xksm__sha512_update(&c, buf, got);
        fclose(f);
        xksm__sha512_final(&c, d);
        hex(d, h);
        printf("%s  %s\n", h, paths[i]);
    }
    return bad;
}

int main(int argc, char **argv)
{
    if (argc > 1)
        return hash_files(argc - 1, argv + 1);

    CHECK(vec("", 0,
              "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce"
              "47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e"));
    CHECK(vec("abc", 3,
              "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
              "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f"));
    const char *m896 = "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmn"
                       "hijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu";
    CHECK(vec(m896, strlen(m896),
              "8e959b75dae313da8cf4f72814fc143f8f7779c6eb9f7fa17299aeadb6889018"
              "501d289e4900f7e4331b99dec4b5433ac7d329eeb6dd26545e96e55b874be909"));
    char *mil = malloc(1000000);
    CHECK(mil != NULL);
    if (mil) {
        memset(mil, 'a', 1000000);
        CHECK(vec(mil, 1000000,
                  "e718483d0ce769644e2e42c7bc15b4638e1f98b13b2044285632a803afa973eb"
                  "de0ff244877ea60a4cb0432ce577c31beb009c5c2c49aa2e4eadb217ad8cc09b"));
        free(mil);
    }

    enum { MAXN = 4096 };
    static uint8_t buf[MAXN];
    uint64_t r = 0x9e3779b97f4a7c15u;
    for (size_t i = 0; i < MAXN; ++i) {
        r ^= r << 13; r ^= r >> 7; r ^= r << 17;
        buf[i] = (uint8_t)(r >> 56);
    }
    int stream_bad = 0;
    for (size_t chunk = 1; chunk <= 260; ++chunk) {
        uint8_t one[64], two[64];
        xksm_sha512 c;
        xksm__sha512(buf, MAXN, one);
        xksm__sha512_init(&c);
        for (size_t off = 0; off < MAXN; off += chunk)
            xksm__sha512_update(&c, buf + off, MAXN - off < chunk ? MAXN - off : chunk);
        xksm__sha512_final(&c, two);
        stream_bad += memcmp(one, two, 64) != 0;
    }
    CHECK(stream_bad == 0);
    return test_report("sha512");
}
