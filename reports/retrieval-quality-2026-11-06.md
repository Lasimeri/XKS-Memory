# Retrieval quality on private notes, 2026-11-06

Ground truth: for a set of notes whose content is known, each query names the note that answers it, so ranking quality can be measured (recall@k, mean reciprocal rank).

Corpus: 361 Markdown notes, 3,130,582 bytes, the developer's own Claude Code memory files (not published; imported locally with `xksm import`, 2,859 chunks, 2,856 memories). The query set (50 queries) is private as well. Tool: `benchmarks/eval_retrieval.c` (generic: TAB-separated class, query, expected source names). Hits are collapsed to distinct sources in rank order.

Query classes:

- `kw` (25): queries built from the note's own distinctive terms, as a search for a known fact. Written by the author with knowledge of the notes, so optimistic.
- `para` (25): paraphrases of the same questions that avoid those terms.

Command: `build/eval_retrieval STORE QUERIES.tsv any|all`

```
mode ANY
kw      25 queries  recall@1 0.880  recall@5 1.000  recall@10 1.000  MRR 0.923
para    25 queries  recall@1 0.160  recall@5 0.280  recall@10 0.440  MRR 0.224
mode ALL
kw      25 queries  recall@1 0.800  recall@5 0.920  recall@10 0.920  MRR 0.843
para    25 queries  recall@1 0.000  recall@5 0.000  recall@10 0.000  MRR 0.000
```

Reading:

- ANY (default) beats ALL in both classes; ALL fails every paraphrase.
- Lexical retrieval answers term-matching queries well (recall@5 1.000, MRR 0.923) and paraphrases poorly (recall@10 0.440, MRR 0.224). That gap is what semantic (embedding) retrieval must close; it is the baseline the vector and hybrid paths will be measured against once an embedding model is wired in.
- The notes include an index note that summarizes every other note; it competes with the answering notes and counts as a miss when ranked above them.
