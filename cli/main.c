/* xksm: command-line front end to libxksm. */
#include "harness.h"
#include "xksm.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static int usage(void)
{
    fputs("usage: xksm init <store>                create or migrate a store\n"
          "       xksm info <store>                versions, record count, cache state\n"
          "       xksm import <store> <file>...    import the named text/Markdown files\n"
          "       xksm search <store> <words>...   best 10 active memories (any word, BM25)\n"
          "       xksm semantic <store>             build the semantic model and memory vectors\n"
          "       xksm port <store> <file>          record the semantic port (tools/port_build) in the store\n"
          "       xksm recall <store> <words>...   best 10: BM25 + port-expanded vector scan (RRF)\n"
          "agent harnesses (Claude Code, omp, ...; docs/AGENTS_INTEGRATION.md):\n"
          "       xksm hook [--harness H]          Claude Code hook (stdin: hook JSON)\n"
          "       xksm turn --harness H --session S --role R [--cwd D] [--time-us T] < text\n"
          "       xksm context --harness H [--session S] < prompt   recalled turns\n"
          "       xksm transcripts [--harness H] [FILE...]   backfill Claude Code transcripts\n"
          "       xksm sessions [HARNESS]          recorded sessions, most recent first\n"
          "       xksm forget HARNESS SESSION      delete every memory of a session\n"
          "       xksm pause HARNESS on|off        stop or resume logging\n"
          "       xksm mcp [--harness H]           MCP server over stdio\n"
          "       xksm mirror                      rewrite the session files under ~/XKS-memories\n"
          "       xksm verify                      recompute every SHA-256 (memories, sources, session files)\n"
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

/* recall = 1: the default setup (hybrid: BM25 + port-expanded vector scan, RRF) */
static int cmd_search(xksm *x, int nwords, char **words, int recall)
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
    if (recall) {
        xksm_hybrid_result h;
        if (xksm_semantic_recall(x, q, NULL, NULL, &h) != XKSM_OK) {
            fprintf(stderr, "xksm: %s\n", xksm_errmsg(x));
            return 1;
        }
        for (size_t i = 0; i < h.count; ++i) {
            xksm_memory m = {0};
            const char *snip = h.hits[i].snippet;
            if (!snip && xksm_memory_get(x, h.hits[i].id, &m) == XKSM_OK)
                snip = m.text;
            printf("%2zu. memory %lld  score %.4f  (lexical rank %zu, vector rank %zu)\n    %.200s\n",
                   i + 1, (long long)h.hits[i].id, h.hits[i].score, h.hits[i].lexical_rank,
                   h.hits[i].vector_rank, snip ? snip : "");
            xksm_memory_free(&m);
        }
        if (!h.count)
            printf("no matches\n");
        xksm_hybrid_free(&h);
        return 0;
    }
    xksm_search_result r;
    xksm_status s = xksm_search(x, q, NULL, &r);
    if (s != XKSM_OK) {
        fprintf(stderr, "xksm: %s\n", xksm_errmsg(x));
        return 1;
    }
    for (size_t i = 0; i < r.count; ++i)
    {
        xksm_memory m = {0};
        const char *snip = r.hits[i].snippet;
        if (!snip && xksm_memory_get(x, r.hits[i].id, &m) == XKSM_OK)
            snip = m.text;
        printf("%2zu. memory %lld  score %.3f\n    %.200s\n", i + 1, (long long)r.hits[i].id,
               r.hits[i].score, snip ? snip : "");
        xksm_memory_free(&m);
    }
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
    umask(077); /* stores, WAL files and index snapshots are private to the user */
    if (argc == 2 && !strcmp(argv[1], "version")) {
        printf("xksm %s, schema %d\n", xksm_version(), XKSM_SCHEMA_VERSION);
        return 0;
    }
    if (argc >= 2) { /* agent-harness commands */
        const char *c = argv[1];
        const char *har = argc >= 4 && !strcmp(argv[2], "--harness") ? argv[3] : NULL;
        if (!strcmp(c, "hook"))
            return xksm_cli_hook(har);
        if (!strcmp(c, "mcp"))
            return xksm_cli_mcp(har);
        if (!strcmp(c, "turn"))
            return xksm_cli_turn(argc - 2, argv + 2);
        if (!strcmp(c, "context"))
            return xksm_cli_context(argc - 2, argv + 2);
        if (!strcmp(c, "transcripts"))
            return xksm_cli_transcripts(argc - 2, argv + 2);
        if (!strcmp(c, "verify") && argc == 2)
            return xksm_cli_verify();
        if (!strcmp(c, "mirror") && argc == 2)
            return xksm_cli_mirror();
        if (!strcmp(c, "sessions"))
            return xksm_cli_sessions(argc >= 3 ? argv[2] : NULL);
        if (!strcmp(c, "forget") && argc == 4)
            return xksm_cli_forget(argv[2], argv[3]);
        if (!strcmp(c, "pause") && argc == 4)
            return xksm_cli_pause(argv[2], argv[3]);
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
    if (!strcmp(cmd, "port") && argc == 4) {
        if (open_or_report(argv[2], &x))
            return 1;
        int rc = xksm_semantic_port_set(x, argv[3]) != XKSM_OK;
        if (rc)
            fprintf(stderr, "xksm: %s\n", xksm_errmsg(x));
        else
            printf("port %s recorded in %s\n", argv[3], argv[2]);
        xksm_close(x);
        return rc;
    }
    if ((!strcmp(cmd, "search") || !strcmp(cmd, "recall")) && argc >= 4) {
        if (open_or_report(argv[2], &x))
            return 1;
        int rc = cmd_search(x, argc - 3, argv + 3, !strcmp(cmd, "recall"));
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
