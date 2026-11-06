# Research notes

Sources were read on 2026-11-06. Each section separates what a source documents from the
engineering decision taken here. Decisions are numbered in
[DESIGN_DECISIONS.md](DESIGN_DECISIONS.md); sources are tabulated in
[SOURCE_MATRIX.md](SOURCE_MATRIX.md).

## 1. Historical architecture (XKeyscore, public documents)

### Source status

| Source | Status |
|---|---|
| NSA training presentation, 2008-02-25, released 2013-07-31 (ACLU document page; Wikimedia Commons file, 32 pages, 27.26 MB, public domain under 17 U.S.C. 105 per Commons) | Located. Pages are scanned images; no text was extracted from it for these notes. |
| S. Gallagher, "Building a panopticon: The evolution of the NSA's XKeyscore", Ars Technica, 2013-08-09 | Read. All architectural facts below come from this secondary analysis. |
| ACLU commentary | Not read yet. |

No undocumented internal algorithm is reproduced or claimed. The facts below are
system-level descriptions only.

### Documented concepts (per the Ars analysis)

1. **Local processing at each site** on arrays of commodity Linux nodes, instead of moving
   raw data to one central store.
2. **Two retention tiers at each site**: a short-lived cache of raw data (about 3 days) and a
   longer-lived local database of processed metadata (up to about 30 days).
3. **Plugin analysis engines**: independent extraction stages run over the data as it is
   processed; the set of queryable fields is whatever the deployed plugins extract.
4. **Federated search**: a query is distributed to every site, each site answers from its
   own store, and the results are returned and aggregated.
5. **A REST web front end** for forming queries.
6. **Rule cost versus throughput**: the predecessor system (Narus) lost capacity as more rules
   were enabled (the article quotes about 12 of 20 Gbit/s handled with every filter on),
   which pushed the design toward few, cheap rules up front.

### Adaptation

| Concept | Adapted as |
|---|---|
| Local processing | Everything runs on the owner's machine; there is no central service at all. |
| Two retention tiers | Working memory (RAM, time-bounded) in front of persistent memory (SQLite), plus an optional archival tier. |
| Plugins | Ingestion and consolidation are tables of stage functions (`xksm_stage`), each a C function pointer with a declared input and output. |
| Federated search | One query fans out to independent retrievers (lexical, vector, temporal, structured); a fusion step merges their ranked lists. |
| Rule cost | Each stage and backend carries a measured cost; the scheduler skips stages and accelerators whose cost is not repaid. |

Excluded completely: collection, capture, interception, session reconstruction, any
identity or communications analysis. XKS-Memory only reads files its owner imports.

## 2. SQLite and FTS5

Documented (sqlite.org/fts5.html):

- FTS5 is part of the amalgamation since 3.9.0 (2015-10-14); `SQLITE_ENABLE_FTS5` enables it.
- **External content tables** index rows stored in an ordinary table; the index can be
  regenerated with the `'rebuild'` command, so it is a derived structure.
- `bm25()` ranking, `highlight()`, `snippet()`; `ORDER BY rank`.
- Query syntax: phrases, `+` concatenation, prefix `*`, initial-token `^`, `NEAR(...)`,
  `AND`/`OR`/`NOT`, column filters.
- Barewords may contain only ASCII letters, digits, `_`, code point 26 and non-ASCII
  characters; anything else must be double-quoted. Technical identifiers such as `libxksm.so`
  therefore need quoting, which the query layer does for the user.
- `'secure-delete'` removes deleted entries from the index instead of leaving tombstones;
  `'integrity-check'` validates the index against its content.

Measured on the development host: system SQLite 3.54.0, `sqlite_compileoption_used('ENABLE_FTS5')`
returns 1 and `create virtual table t using fts5(x)` succeeds.

## 3. Not yet read

FAISS, Tantivy, Apache Arrow, Apache Parquet, Mem0, LangGraph memory concepts, llama.cpp
embedding and tokenizer endpoints, Model Context Protocol. Each is read before the phase that
depends on it (vector index: phase 5; integration: phase 8).

## 4. Verified on the development host (SQLite 3.54.0, 2026-11-06)

External-content FTS5 table over `memory(text)`, `tokenize="unicode61 tokenchars '_'"`,
`'secure-delete'` on, insert/delete triggers:

| Check | Result |
|---|---|
| `MATCH 'sm_86'` against "...sm_86..." and "sm 86..." | only the `sm_86` row, bm25 -0.4117 |
| `MATCH 'libxksm.so'` (bareword) | `fts5: syntax error near "."` |
| `MATCH '"libxksm.so"'` (quoted) | matched |
| delete row, then `MATCH` its unique word | 0 rows |
| `'integrity-check'` after delete | passed |
| `'rebuild'` | passed, index answers queries afterwards |
