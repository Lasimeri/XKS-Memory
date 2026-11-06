# XKS-Memory

An offline, open-source, hardware-accelerated long-term memory engine for large language models.

**Status: pre-alpha. Nothing below the "Planned" line is implemented yet.** Every feature is
listed in [ROADMAP.md](ROADMAP.md) with its state; a feature is marked done only after it has
been built, run and its output checked.

## What it is

XKS-Memory stores what you explicitly give it (notes, documents, conversation exports, model
summaries), indexes it lexically, semantically and by time, and assembles the most relevant
pieces into a token-budgeted context block for a local language model. It runs entirely on
your machine: no telemetry, no cloud service, no account, no network access at runtime.

- **Core:** C17 library `libxksm` with a stable C ABI (`include/xksm.h`).
- **Storage:** SQLite (transactional, authoritative); every other index is rebuildable from it.
- **Interface:** terminal UI written in Rust (`tui/`), plus the `xksm` command-line tool and a
  loopback-only local HTTP API for LLM integration.
- **Acceleration (optional):** CUDA backend for vector work, experimental Intel Knights
  Corner (Xeon Phi) backend for bitmap and integer filtering. Both are off unless built and
  justified by measurement; the CPU path is always complete.

## Scope

XKS-Memory is an independent memory-management project. Its layering (local indexing,
short-lived caches in front of longer-lived searchable records, plugin processing stages,
federated queries over several indexes, result aggregation) draws on general architecture
described in publicly released historical documents about the XKeyscore system. It has no
affiliation with or endorsement from any government agency and contains no code or
non-public material from any such system.

It contains **no** packet capture, network inspection, communications interception,
monitoring, user tracking or identity correlation. It processes only data its owner
deliberately imports.

## Planned

| Area | Plan |
|---|---|
| Memory categories | working, episodic, semantic, procedural, project, archival |
| Retrieval | FTS5 BM25 lexical, exact and ANN vector, temporal, structured; reciprocal rank fusion with per-result explanation |
| Lifecycle | candidate, active, consolidated, superseded, archived, expired, deleted; deletion propagates to every index and cache |
| Context assembly | ranked, cited excerpts under a token budget; retrieved text is always lower-trust data |
| Integration | local HTTP API (127.0.0.1), llama.cpp / OpenAI-compatible / Ollama-compatible clients, optional MCP tools |

## Build (once code lands)

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build
cargo build --release --manifest-path tui/Cargo.toml
```

Requirements: a C17 compiler, CMake >= 3.20, SQLite >= 3.35 with FTS5, Rust (TUI only).
CUDA and KNC toolchains are optional.

## License

MIT, see [LICENSE](LICENSE).
