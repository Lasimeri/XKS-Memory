# XKS-Memory

Offline, local, long-term memory for AI coding agents and language models. Every turn of an
agent session is stored on your machine with its exact time and embedded without a neural
network. The related earlier turns are recalled into the next prompt. Nothing leaves the
machine.

- **Engine:** C17 library `libxksm` (C ABI, `include/xksm.h`), SQLite as the authoritative
  store, AVX2/FMA kernels with plain-C fallbacks, a POSIX thread pool.
- **Agents:** a Claude Code plugin (hooks + MCP server), an omp extension, and generic CLI
  commands any other harness can call. Each harness is kept separate.
- **Interfaces:** the `xksm` CLI and the `xksm-tui` terminal interface (Rust, std only: no
  terminal crates).
- **Embedding:** a count-based word-vector table learned from your own store, plus an optional
  "semantic port" of word relations computed once offline. No model runs at query time
  ([docs/EMBEDDING.md](docs/EMBEDDING.md)).

**Status: alpha.** Everything listed under "Working today" is built, tested and run on real
data. [ROADMAP.md](ROADMAP.md) lists every phase with its state. Measurements are in
[reports/](reports/).

## Contents

1. [Working today](#working-today)
2. [How it works](#how-it-works)
3. [Install](#install)
4. [Use with Claude Code](#use-with-claude-code)
5. [Use with omp and other harnesses](#use-with-omp-and-other-harnesses)
6. [The terminal interface](#the-terminal-interface)
7. [Recall accuracy](#recall-accuracy)
8. [Benchmarks](#benchmarks)
9. [Privacy and data](#privacy-and-data)
10. [Build and test](#build-and-test)
11. [Documentation](#documentation)

## Working today

| Area | What it does |
|---|---|
| Agent memory | Every turn is stored: prompts, assistant text, tool calls with their inputs, tool results, thinking. Each has its exact time (microseconds), role and session, and is embedded immediately. Related earlier turns of the same harness are recalled into each prompt. |
| Harness separation | One store, separate namespaces per harness (`claude-code`, `claude-code-trace`, `omp`, ...). Each session is listed with turn count, time range and working directory. A whole session can be forgotten (secure delete). Logging can be paused per harness. |
| Claude Code | Plugin with UserPromptSubmit, Stop, PreCompact and SessionEnd hooks. MCP tools `recall`, `search`, `remember`, `sessions`, `stats`. Backfill of existing transcripts. |
| omp | Extension on `before_agent_start` (recall injected as a message) and `message_end` (every message stored). |
| Recall | Hybrid: BM25 on the query as typed, fused (RRF) with a lattice vector scan over a query expanded through the semantic port. It degrades gracefully: without a port there is no expansion; without a word-vector table it is BM25 only. |
| Embedding | PPMI over word co-occurrence, randomized SVD on the CPU, SIF memory vectors. The table rebuilds itself when the store grows by 25 %; new memories are embedded as they arrive. |
| Import | Text, Markdown, JSON, JSONL, CSV. Structured records become `path: value` lines. Strict parsers report the line and column of any error. Original bytes are kept as evidence. |
| Search | BM25 with quoted terms (no FTS5 injection), namespace and category filters, snippets. Function words are dropped from ANY queries. |
| Vectors | Square-lattice int8 codes with exact f32 rerank. The index is a memory-mapped snapshot file plus a small RAM tail. |
| Threads | Two 4-core pools per process: recall (vector scans, port expansion) and embedding (table build: co-occurrence, PPMI, SVD, memory vectors); BM25 runs concurrently with the vector branch of recall on a second read connection. `XKSM_THREADS`, `XKSM_EMBED_THREADS` change them. |
| Storage | SQLite (WAL, secure delete), schema v4 with ordered migrations, SHA-256 (SHA-NI) content hashes, per-namespace dedup, a RAM record cache (5 to 50 % of records). |
| Terminal interface | `xksm-tui`: sessions and their turns, recall and word search, memory detail and deletion, embedding coverage, table rebuild, port, logging state. |

## How it works

```
╔══════════════════════════════════════════════════════════════════════════════════╗
║ AGENT HARNESSES (this machine)                                                   ║
║ ┌──────────────────────────────┐  ┌──────────────────────────────┐ ┌───────────┐ ║
║ │ Claude Code plugin           │  │ omp extension                │ │ any other │ ║
║ │ hooks: prompt, stop, compact │  │ before_agent_start,          │ │ xksm turn │ ║
║ │ MCP: recall search remember  │  │ message_end                  │ │ / context │ ║
║ └──────────────┬───────────────┘  └──────────────┬───────────────┘ └─────┬─────┘ ║
╚════════════════╪═════════════════════════════════╪═══════════════════════╪═══════╝
                 ▼                                 ▼                       ▼        
╔══════════════════════════════════════════════════════════════════════════════════╗
║ xksm CLI + libxksm (C17, CPU: AVX2/FMA, 8-thread pool, no neural model)          ║
║ ┌──────────────────────────────┐  ┌──────────────────────────────────────────┐   ║
║ │ store a turn                 │  │ recall for a prompt                      │   ║
║ │ exact time, role, session    │  │ BM25 on the typed query                  │   ║
║ │ chunk, SHA-256, dedup        │  │ + lattice scan, port-expanded query      │   ║
║ │ embed now (fold-in)          │  │ RRF k = 60, same harness only            │   ║
║ └──────────────┬───────────────┘  └─────────────────────┬────────────────────┘   ║
║                ▼                                        ▼                        ║
║ ┌──────────────────────────────────────────────────────────────────────────────┐ ║
║ │ ~/XKS-memories/.index/agents.xksm (SQLite, 600): memories, FTS5, vectors,    │ ║
║ │ sessions; ~/XKS-memories/HARNESS/START_SESSION/: one .md file per turn       │ ║
║ └──────────────────────────────────────────────────────────────────────────────┘ ║
╚══════════════════════════════════════════════════════════════════════════════════╝
  offline, once: tools/port_build + an embedding model -> the semantic port file    
```

The word-vector table is learned from the store's own text: which words occur near which. A
query's vector also adds the store words most related to each query word. Those relations
are looked up in the port (434,204 words, computed once by
`nomic-embed-text-v2-moe` in F32) and grouped by script and by code, so a word never relates
to its own translation. Full description: [docs/EMBEDDING.md](docs/EMBEDDING.md).

## Install

Requirements: a C17 compiler, CMake 3.20+, Ninja, SQLite 3.35+ (development files); Rust
(cargo) for the terminal interface; optionally OpenCL for the GPU path of the table build.

```
tools/install.sh                 # build, install to ~/.local, register the Claude Code plugin
tools/install.sh --backfill      # also import existing Claude Code transcripts
tools/install.sh --port FILE     # also record a semantic port (see docs/EMBEDDING.md)
tools/install.sh --no-claude --prefix /opt/xksm
```

What gets installed:

| Path | Contents |
|---|---|
| `~/.local/bin/xksm`, `~/.local/bin/xksm-tui` | the CLI and the terminal interface |
| `~/.local/share/xks-memory/claude-code/` | Claude Code marketplace + plugin |
| `~/.local/share/xks-memory/omp/xks-memory.ts`, `mcp.json` | omp extension and MCP server definition |
| `~/.local/lib/libxksm.a`, `~/.local/include/xksm.h` | the library |
| `~/XKS-memories/` | your memories (created on first use; never part of the install): `.index/agents.xksm` is the search index, and `HARNESS/YYYY-MM-DD_HHMMSS_SESSION/` holds `session.md` plus one file per turn, named by time and role (`2026-11-06_11-09-01.517_user_e336ebc9.md`) |

The application and your data are separate: reinstalling or deleting the repository never
touches the store.

## Use with Claude Code

`tools/install.sh` runs:

```
claude plugin marketplace add ~/.local/share/xks-memory/claude-code
claude plugin install xks-memory@xks-memory
```

Then every new session is recorded and recalled. In the session, the MCP tools are available
as `mcp__plugin_xks-memory_xks-memory__recall` (and `search`, `remember`, `sessions`,
`stats`). Import earlier sessions with `xksm transcripts`. Turn it off with
`xksm pause claude-code on` or `claude plugin uninstall xks-memory@xks-memory`.

Verified with Claude Code 2.1.316: a new session answered with a code word that existed only
in an earlier session, from the recalled context; tool calls, tool results and the MCP call
itself were stored with their times.

## Use with omp and other harnesses

omp: link the extension into the profile's hook directory and add the MCP server (`integrations/omp/mcp.json` to `~/.omp/agent/mcp.json` or `~/.omp/profiles/NAME/agent/mcp.json`; it runs `xksm mcp --harness omp`), for example
`ln -s ~/.local/share/xks-memory/omp/xks-memory.ts ~/.omp/agent/hooks/pre/`. Verified with
omp 18.17.0: the injected recall message reached a new session.

Any harness can use two commands:

```
echo "$text"   | xksm turn --harness NAME --session ID --role user|assistant|tool_use|tool_result|thinking|note
echo "$prompt" | xksm context --harness NAME --session ID      # prints recalled turns
```

Session management:

```
xksm sessions [HARNESS]        # most recent first: turns, time range, working directory
xksm forget HARNESS SESSION    # delete every memory of a session (secure delete)
xksm pause HARNESS on|off      # stop or resume logging
xksm mcp --harness NAME        # MCP server over stdio
```

Details: [docs/AGENTS_INTEGRATION.md](docs/AGENTS_INTEGRATION.md).

## The terminal interface

`xksm-tui [STORE]` (default: the agent store). Five views; `?` shows every key. Live: every
200 ms the interface asks the store whether anything was committed (`xksm_change_counter`,
one `PRAGMA data_version`), and on a change reloads what is on screen: the session list, an
open turn list (the selection follows the newest turn), the shown memory page, the search
results (re-run) and the statistics; rows that arrived are marked `+`, the header shows the
time and size of the last change, and an open memory deleted elsewhere is flagged. Measured
(Ryzen 7 5800X, a 4,141-turn session open, tmux 200x50): a turn written by another process
is on screen 185 ms after the write; idle CPU 0 ticks in 10 s, 10 writes in 10 s cost 1 tick
(10 ms). `a` turns live updates off, `r` reloads at once.

| View | Keys |
|---|---|
| 1 Sessions | `h` harness filter, Enter: turns of a session, Enter: full text, `x` forget the session, `e` export it to `~/xksm-export-HARNESS-SESSION.md` (mode 600) |
| 2 Search | `/` query, `m` recall (words + meaning) or words (BM25), `s` scope (dialogue, + tool calls and results, every harness), `d` delete a memory |
| 3 Memories | every namespace with its embedding coverage; Enter: its memories newest first (pages of 200), Enter: full text, `d` delete, `i` import a text, Markdown, JSON, JSONL or CSV file (into the selected namespace; harness namespaces get `documents` instead, so imported files never enter an agent's recall) |
| 4 Embedding | word-vector table and port state, embedding coverage per namespace; `b` rebuild, `u` embed missing memories, `p` set the port |
| 5 Store | file, schema, size, mode, live counts per harness (dialogue and trace); `P` pause or resume a harness |
| everywhere | `n` add a note (namespace `notes`, current time, embedded at once), `r` reload, `q` quit |

On a 150,951-memory store: recall 137.5 ms on the first query, 26.0 ms warm; word search
28.3 ms.

## Recall accuracy

Ground truth: 50 questions about the author's own notes (2,856 memories). Each question has
the notes that answer it. 25 are **term** questions, which use words found in the answer;
25 are **paraphrase** questions, which describe the answer in other words. The metrics are
recall@k and MRR (mean reciprocal rank of the first correct hit). The set is private;
`benchmarks/eval_retrieval.c` is the tool. One question is 0.04 of a recall value, so
differences of one or two questions are within this set's resolution.

Current engine (stop words dropped from lexical queries):

| Configuration | Term r@1 | Term r@10 | Term MRR | Para r@1 | Para r@10 | Para MRR |
|---|---|---|---|---|---|---|
| BM25 only | 0.880 | 1.000 | 0.923 | 0.280 | 0.480 | **0.322** |
| Hybrid, no port | 0.880 | 1.000 | 0.920 | 0.240 | 0.400 | 0.272 |
| **Hybrid + port v2, every word (default)** | 0.800 | 1.000 | 0.860 | 0.160 | **0.520** | 0.251 |

References and earlier variants (stop words kept, all measured on the same set):

| Configuration | Model at query time | Term MRR | Para MRR | Para r@10 |
|---|---|---|---|---|
| nomic-embed-text-v2-moe Q4_0, vectors only | yes | 1.000 | 0.321 | 0.600 |
| nomic + BM25 hybrid | yes | 0.980 | 0.404 | 0.560 |
| BM25 only | no | 0.923 | 0.224 | 0.440 |
| Word-vector table only | no | 0.887 | 0.192 | 0.320 |
| Hybrid + port v2, every word | no | 0.860 | 0.213 | 0.480 |
| Static nomic token vectors, pooled | no | 0.403 | 0.044 | 0.080 |

Findings:

- Dropping function words from BM25 queries left term MRR unchanged in every mode. It raised
  paraphrase MRR by 18 to 44 %: BM25 0.224 to 0.322, equal to the neural model's
  vectors-only 0.321.
- The default has the best paraphrase recall@10 (0.520) of the model-free configurations.
  Plain BM25 has the best MRR on this set.
- The int8 expansion path reproduces the f32 path exactly: 642 of 642 expansions identical
  across the 50 questions.

## Benchmarks

AMD Ryzen 7 5800X (8 cores), 32 GB, NVMe; RTX 3090 Ti for the optional GPU path. The command
and raw results for each line are in [reports/](reports/). Single runs unless a range is
given.

| Operation | Result |
|---|---|
| Agent hook, UserPromptSubmit (recall + store + embed), 150,951-memory store | 0.17 to 0.18 s per prompt (3 runs), RSS 573 MB (mostly shared mappings); was 0.63 to 0.66 s and 1.54 GB before the index snapshots |
| Agent hook, Stop (async) | 0.13 s |
| Recall of one prompt, 150,951 memories (`xksm context`) | 0.14 s with BM25 and the vector branch running concurrently, 0.16 to 0.18 s sequential; identical output |
| Backfill of 66 Claude Code transcripts (1.35 GB, 320,503 lines) | 35.96 s, including the table build: 96,193 turns, 150,951 memories, 150,934 embedded, 0 unparseable lines |
| Vector index load, 150,934 vectors | 0.211 s from SQLite; 0.031 s from the mapped snapshot; warm scan 0.0006 s |
| BM25 depth 50, 150,951 memories | 0.212 s; 0.039 s with function words dropped |
| Word-vector table build, 150,973 memories (18.7 M tokens, vocabulary 152,574) | 11.02 s on 4 embedding threads (was 17.18 s on 8 before the parallel co-occurrence, PPMI and counting-sort CSR; 22.31 s on 1 thread); pairs and non-zeros identical |
| Word-vector table build, 2,856 memories (vocabulary 16,972) | 0.773 s on 4 embedding threads (1.008 s before; SVD 7.39x faster than the first version); 0.882 s with the OpenCL GPU |
| Lattice vector scan + exact rerank | 2.90 to 3.09x faster than an exact f32 scan at recall@10 1.0 |
| BM25 search, 24,088 real memories | 1.82 to 1.85 ms median |
| RAM cache hit | 0.12 to 0.20 us, vs 3.6 to 3.7 us from SQLite |
| Insert | 14,000 to 15,700 memories/s batched |
| Import of 779 text files, 22.4 MB | 2.2 to 2.3 s |
| SHA-256 (SHA-NI) | 2.369 GB/s, 6.0x the portable kernel |
| Semantic port build (434,204 words, F32 model on the RTX 3090 Ti) | 49.0 s, once |

## Privacy and data

- Local only: no network access, no telemetry, no account. The port is built offline from
  local files; nothing is downloaded at run time.
- The store is created mode 600 in a mode-700 directory. Index snapshots are removed as soon
  as a memory is deleted, so deleted vectors do not survive on disk.
- What is stored: everything a harness sends: prompts, replies, tool calls, tool results
  (64 KiB per block by default, `XKSM_TRACE_MAX`), thinking. Use `xksm pause` and
  `xksm forget` to control it.
- The repository never contains memory data: stores and benchmark outputs are ignored by git.

## Build and test

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build && ctest --test-dir build
(cd tui && cargo build --release --offline)
```

16 test programs (including an end-to-end CLI harness test) pass in a normal, an
AddressSanitizer + UndefinedBehaviorSanitizer and a ThreadSanitizer build, with zero compiler
warnings (`-Werror`). Contributor rules: [CLAUDE.md](CLAUDE.md), [CONTRIBUTING.md](CONTRIBUTING.md).

## Documentation

| Document | Contents |
|---|---|
| [docs/EMBEDDING.md](docs/EMBEDDING.md) | the embedding and recall system, every parameter and measurement |
| [docs/AGENTS_INTEGRATION.md](docs/AGENTS_INTEGRATION.md) | Claude Code, omp, the generic CLI, sessions, data layout |
| [docs/DESIGN_DECISIONS.md](docs/DESIGN_DECISIONS.md) | every design decision with its evidence |
| [ARCHITECTURE.md](ARCHITECTURE.md) | modules and tests |
| [ROADMAP.md](ROADMAP.md) | phases and state |
| [docs/KNC_VECTOR_PLAN.md](docs/KNC_VECTOR_PLAN.md) | the Xeon Phi 7120A vector backend plan |

## Scope

XKS-Memory processes only what you or your agent harness hands to it. It does not watch
files, capture traffic or read anything on its own. License: MIT.
