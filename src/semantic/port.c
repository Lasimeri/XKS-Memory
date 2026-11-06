/* port.c: memory-mapped semantic port: keys hashed at open, vectors used in place. */
#define _POSIX_C_SOURCE 200809L
#include "port.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

struct xksm_port {
    const uint8_t *map;
    size_t size, dim, n;
    const char **key;
    uint8_t *klen, *bucket;
    const float *vec;
    uint32_t *slot, mask;
};

static uint64_t fnv(const char *s, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i)
        h = (h ^ (uint8_t)s[i]) * 1099511628211ull;
    return h;
}

void xksm__port_close(xksm_port *p)
{
    if (!p)
        return;
    if (p->map)
        munmap((void *)p->map, p->size);
    free(p->key);
    free(p->klen);
    free(p->bucket);
    free(p->slot);
    free(p);
}

xksm_port *xksm__port_open(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return NULL;
    struct stat st;
    xksm_port *p = calloc(1, sizeof *p);
    if (!p || fstat(fd, &st) != 0 || st.st_size < 16) {
        close(fd);
        free(p);
        return NULL;
    }
    p->size = (size_t)st.st_size;
    void *m = mmap(NULL, p->size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (m == MAP_FAILED) {
        free(p);
        return NULL;
    }
    p->map = m;
    uint32_t d32, n32;
    memcpy(&d32, p->map + 8, 4);
    memcpy(&n32, p->map + 12, 4);
    if (memcmp(p->map, "XKSMPRT1", 8) || !d32 || !n32) {
        xksm__port_close(p);
        return NULL;
    }
    p->dim = d32;
    p->n = n32;
    p->key = malloc(sizeof *p->key * p->n);
    p->klen = malloc(p->n);
    p->bucket = malloc(p->n);
    uint32_t cap = 1;
    while (cap < 2 * n32)
        cap <<= 1;
    p->mask = cap - 1;
    p->slot = calloc(cap, sizeof *p->slot);
    if (!p->key || !p->klen || !p->bucket || !p->slot) {
        xksm__port_close(p);
        return NULL;
    }
    size_t off = 16;
    for (size_t i = 0; i < p->n; ++i) {
        if (off + 2 > p->size) {
            xksm__port_close(p);
            return NULL;
        }
        p->bucket[i] = p->map[off];
        p->klen[i] = p->map[off + 1];
        p->key[i] = (const char *)p->map + off + 2;
        off += 2u + p->klen[i];
        uint32_t h = (uint32_t)fnv(p->key[i], p->klen[i]) & p->mask;
        while (p->slot[h])
            h = (h + 1) & p->mask;
        p->slot[h] = (uint32_t)i + 1;
    }
    off = (off + 63) / 64 * 64;
    if (off + sizeof(float) * p->dim * p->n != p->size) {
        xksm__port_close(p);
        return NULL;
    }
    p->vec = (const float *)(const void *)(p->map + off);
    return p;
}

size_t xksm__port_dim(const xksm_port *p) { return p ? p->dim : 0; }
size_t xksm__port_size(const xksm_port *p) { return p ? p->n : 0; }
const float *xksm__port_vec(const xksm_port *p, size_t i) { return p->vec + i * p->dim; }
int xksm__port_bucket(const xksm_port *p, size_t i) { return p->bucket[i]; }

int64_t xksm__port_find(const xksm_port *p, const char *s, size_t n)
{
    if (!p || n == 0 || n > 255)
        return -1;
    for (uint32_t h = (uint32_t)fnv(s, n) & p->mask; p->slot[h]; h = (h + 1) & p->mask) {
        uint32_t i = p->slot[h] - 1;
        if (p->klen[i] == n && !memcmp(p->key[i], s, n))
            return i;
    }
    return -1;
}

int xksm__port_stop(const char *w, size_t n)
{
    static const char *const stop[] = {
        "the", "and", "for", "are", "was", "were", "with", "that", "this", "from", "into",
        "onto", "what", "which", "who", "whom", "whose", "when", "where", "why", "how", "can",
        "could", "should", "would", "will", "shall", "may", "might", "must", "does", "did",
        "done", "doing", "has", "have", "had", "having", "its", "his", "her", "hers", "their",
        "theirs", "our", "ours", "your", "yours", "mine", "you", "they", "them", "she", "him",
        "not", "but", "nor", "yet", "all", "any", "each", "every", "some", "such", "than",
        "then", "there", "here", "about", "above", "below", "after", "before", "over", "under",
        "again", "also", "just", "only", "very", "too", "more", "most", "less", "least", "other",
        "same", "own", "out", "off", "upon", "via", "per", "been", "being", "get", "got", "use",
        "used", "using", "one", "two", "new", "way", "many", "much"};
    if (n < 3)
        return 1;
    size_t digits = 0;
    for (size_t i = 0; i < n; ++i)
        digits += w[i] >= '0' && w[i] <= '9';
    if (digits == n)
        return 1;
    for (size_t i = 0; i < sizeof stop / sizeof *stop; ++i)
        if (strlen(stop[i]) == n && !memcmp(w, stop[i], n))
            return 1;
    return 0;
}
