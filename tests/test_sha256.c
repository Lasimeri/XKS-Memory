/* test_sha256.c: FIPS 180-2 vectors through the dispatched kernel; scalar vs SHA-NI
 * on every length 0..4096; chunked streaming equals one-shot.
 * With FILE arguments: print sha256sum-format digests for cross-checks. */
#include "test.h"

#include "sha256.h"

#include <stdlib.h>
#include <string.h>

static void hex(const uint8_t d[32], char out[65])
{
    static const char digits[] = "0123456789abcdef";
    for (int i = 0; i < 32; ++i) {
        out[2 * i] = digits[d[i] >> 4];
        out[2 * i + 1] = digits[d[i] & 15];
    }
    out[64] = 0;
}

static int vec(const void *msg, size_t n, const char *want)
{
    uint8_t d[32];
    char h[65];
    xksm__sha256(msg, n, d);
    hex(d, h);
    if (strcmp(h, want))
        fprintf(stderr, "  got  %s\n  want %s\n", h, want);
    return !strcmp(h, want);
}

static uint64_t rng = 0x9e3779b97f4a7c15u;
static uint8_t next_byte(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return (uint8_t)(rng >> 56);
}

/* Hash with an explicit block kernel (same padding as sha256.c). */
typedef void (*blocks_fn)(uint32_t[8], const uint8_t *, size_t);
static void hash_with(blocks_fn f, const uint8_t *p, size_t n, uint32_t st[8])
{
    static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    uint8_t tail[128] = {0};
    size_t full = n / 64, rem = n % 64, tl = rem < 56 ? 64 : 128;
    memcpy(st, iv, sizeof iv);
    f(st, p, full);
    memcpy(tail, p + 64 * full, rem);
    tail[rem] = 0x80;
    uint64_t bits = (uint64_t)n * 8u;
    for (int i = 0; i < 8; ++i)
        tail[tl - 1 - (size_t)i] = (uint8_t)(bits >> (8 * i));
    f(st, tail, tl / 64);
}

/* FILE... mode: "<hex>  <file>" per file (sha256sum format) for cross-checks. */
static int hash_files(int n, char **paths)
{
    static uint8_t fbuf[1 << 16];
    int bad = 0;
    for (int i = 0; i < n; ++i) {
        FILE *f = fopen(paths[i], "rb");
        if (!f) {
            perror(paths[i]);
            bad = 1;
            continue;
        }
        xksm_sha256 c;
        uint8_t d[32];
        char h[65];
        size_t got;
        xksm__sha256_init(&c);
        while ((got = fread(fbuf, 1, sizeof fbuf, f)) > 0)
            xksm__sha256_update(&c, fbuf, got);
        fclose(f);
        xksm__sha256_final(&c, d);
        hex(d, h);
        printf("%s  %s\n", h, paths[i]);
    }
    return bad;
}

int main(int argc, char **argv)
{
    if (argc > 1)
        return hash_files(argc - 1, argv + 1);
    printf("kernel: %s\n", xksm__sha256_kernel());

    CHECK(vec("", 0, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    CHECK(vec("abc", 3, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    const char *m448 = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    CHECK(vec(m448, strlen(m448),
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
    char *mil = malloc(1000000);
    CHECK(mil != NULL);
    if (mil) {
        memset(mil, 'a', 1000000);
        CHECK(vec(mil, 1000000,
                  "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
        free(mil);
    }

    enum { MAXN = 4096 };
    static uint8_t buf[MAXN];
    for (size_t i = 0; i < MAXN; ++i)
        buf[i] = next_byte();

#ifdef __x86_64__
    if (!strcmp(xksm__sha256_kernel(), "sha-ni")) {
        int mismatches = 0;
        for (size_t n = 0; n <= MAXN; ++n) {
            uint32_t a[8], b[8];
            hash_with(xksm__sha256_blocks_scalar, buf, n, a);
            hash_with(xksm__sha256_blocks_shani, buf, n, b);
            mismatches += memcmp(a, b, sizeof a) != 0;
        }
        printf("scalar vs sha-ni: %d mismatches over lengths 0..%d\n", mismatches, MAXN);
        CHECK(mismatches == 0);
    }
#endif

    /* streaming in odd chunk sizes equals one-shot */
    int stream_bad = 0;
    for (size_t chunk = 1; chunk <= 130; ++chunk) {
        uint8_t one[32], two[32];
        xksm_sha256 c;
        xksm__sha256(buf, MAXN, one);
        xksm__sha256_init(&c);
        for (size_t off = 0; off < MAXN; off += chunk)
            xksm__sha256_update(&c, buf + off, MAXN - off < chunk ? MAXN - off : chunk);
        xksm__sha256_final(&c, two);
        stream_bad += memcmp(one, two, 32) != 0;
    }
    CHECK(stream_bad == 0);

    return test_report("sha256");
}
