/* xksm: command-line front end to libxksm. */
#include "xksm.h"

#include <stdio.h>
#include <string.h>

static int usage(void)
{
    fputs("usage: xksm init <store>                create or migrate a store\n"
          "       xksm info <store>                versions, record count, cache state\n"
          "       xksm import <store> <file>...    import the named text/Markdown files\n"
          "       xksm search <store> <words>...   best 10 active memories (any word, BM25)\n"
          "       xksm semantic <store>             build the semantic model and memory vectors\n"
          "       xksm version\n",
          stderr);
    return 2;
}

static int open_or_report(const char *path, xksm **x)
{
    xksm_status s = xksm_open(path, x);
    if (s != XKSM_OK) {
        fprintf(stderr, "xksm: %s (status %d)\n", xksm_errmsg(*x), (int)s);
        xksm_close(*x);
        return 1;
    }
    return 0;
}

static int cmd_semantic(xksm *x)
{
    xksm_semantic_report r;
    if (xksm_semantic_build(x, NULL, &r) != XKSM_OK) {
        fprintf(stderr, "xksm: %s\n", xksm_errmsg(x));
        return 1;
    }
    printf("semantic model: %zu memories (%zu embedded), %zu tokens, vocabulary %zu, %llu pairs, "
           "PPMI nnz %llu\n",
           r.memories, r.embedded, r.tokens, r.vocab, (unsigned long long)r.pairs,
           (unsigned long long)r.nnz);
    printf("svd on %s\n", r.gpu_used ? r.device : "CPU");
    printf("seconds: tokens %.3f  cooc %.3f  ppmi %.3f  svd %.3f  embed %.3f  store %.3f  total %.3f\n",
           r.t_tokens, r.t_cooc, r.t_ppmi, r.t_svd, r.t_embed, r.t_store, r.t_total);
    return 0;
}

static int cmd_search(xksm *x, int nwords, char **words)
{
    char q[XKSM_MAX_QUERY + 1];
    size_t o = 0;
    for (int i = 0; i < nwords; ++i) {
        size_t l = strlen(words[i]);
        if (o + l + 2 > sizeof q) {
            fprintf(stderr, "xksm: query longer than %d bytes\n", XKSM_MAX_QUERY);
            return 1;
        }
        if (o)
            q[o++] = (char)0x20;
        memcpy(q + o, words[i], l);
        o += l;
    }
    q[o] = 0;
    xksm_search_result r;
    if (xksm_search(x, q, NULL, &r) != XKSM_OK) {
        fprintf(stderr, "xksm: %s\n", xksm_errmsg(x));
        return 1;
    }
    for (size_t i = 0; i < r.count; ++i)
        printf("%2zu. memory %lld  score %.3f\n    %s\n", i + 1, (long long)r.hits[i].id,
               r.hits[i].score, r.hits[i].snippet);
    if (!r.count)
        printf("no matches\n");
    xksm_search_free(&r);
    return 0;
}

static int cmd_import(xksm *x, int nfiles, char **files)
{
    xksm_import_opts o = {0};
    o.category = XKSM_CAT_PROJECT;
    int failed = 0;
    size_t added = 0, dups = 0, chunks = 0;
    for (int i = 0; i < nfiles; ++i) {
        xksm_import_result r;
        xksm_status s = xksm_import_file(x, files[i], &o, &r);
        if (s == XKSM_OK) {
            printf("%s: source %lld, %zu chunks, %zu new, %zu duplicate\n", files[i],
                   (long long)r.source_id, r.chunks, r.added, r.duplicates);
            added += r.added;
            dups += r.duplicates;
            chunks += r.chunks;
        } else if (s == XKSM_EXISTS) {
            printf("%s: already stored as source %lld\n", files[i], (long long)r.source_id);
        } else {
            fprintf(stderr, "%s\n", xksm_errmsg(x)); /* message names the file */
            failed = 1;
        }
    }
    printf("total: %zu chunks, %zu new memories, %zu duplicates\n", chunks, added, dups);
    return failed;
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "version")) {
        printf("xksm %s, schema %d\n", xksm_version(), XKSM_SCHEMA_VERSION);
        return 0;
    }
    if (argc < 3)
        return usage();

    xksm *x = NULL;
    const char *cmd = argv[1];
    if ((!strcmp(cmd, "init") || !strcmp(cmd, "info")) && argc == 3) {
        if (open_or_report(argv[2], &x))
            return 1;
        xksm_cache_info ci;
        xksm_cache_stats(x, &ci);
        printf("store %s: schema %d (library %s), %lld memories, RAM cache capacity %u\n",
               argv[2], xksm_schema_version(x), xksm_version(), (long long)ci.records,
               ci.capacity);
        xksm_close(x);
        return 0;
    }
    if (!strcmp(cmd, "semantic") && argc == 3) {
        if (open_or_report(argv[2], &x))
            return 1;
        int rc = cmd_semantic(x);
        xksm_close(x);
        return rc;
    }
    if (!strcmp(cmd, "search") && argc >= 4) {
        if (open_or_report(argv[2], &x))
            return 1;
        int rc = cmd_search(x, argc - 3, argv + 3);
        xksm_close(x);
        return rc;
    }
    if (!strcmp(cmd, "import") && argc >= 4) {
        if (open_or_report(argv[2], &x))
            return 1;
        int rc = cmd_import(x, argc - 3, argv + 3);
        xksm_close(x);
        return rc;
    }
    return usage();
}
