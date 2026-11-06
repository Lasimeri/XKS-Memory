# Installing and recovering XKS-Memory

XKS-Memory is distributed as source only. Everything below starts from a clone of the
repository; no pre-compiled binary is used or needed. The same steps rebuild a machine whose
copy of the source was lost.

## 1. What lives where

Three things exist, and only the first is in the repository:

| What | Where | Recoverable from |
|---|---|---|
| The program (C library, `xksm` CLI, `xksm-tui`, agent integrations) | this repository; installed to `~/.local` | a clone of the repository (this guide) |
| Your memories | `~/XKS-memories/` (`$XKSM_HOME`): `.index/agents.xksm` (+ `-wal`, `-shm`) is the search index and the only authority; `HARNESS/DATE_SESSION/*.md` are readable copies of each turn; `.vidx-*` and `.ptgt` beside the index are derived caches | a copy of `~/XKS-memories` only; never in the repository |
| The semantic port (optional) | the file recorded in the store (`xksm port`), typically outside both | a copy of the file, or rebuilt (section 5) |

Keep a copy of `~/XKS-memories` (at least `.index/agents.xksm` with its `-wal` file) somewhere
else. The session folders can be rebuilt from the index (`xksm mirror`); the caches rebuild
themselves.

## 2. Requirements

| Needed | Version | Why |
|---|---|---|
| C compiler | C17 (gcc, clang) | the library and the CLI |
| CMake | 3.20 or newer | build |
| SQLite development files | 3.42 or newer, with FTS5 | the store (FTS5 secure-delete) |
| `sqlite3` command | any | the tests read stores with it |
| Ninja | optional | faster builds; otherwise the default generator |
| Rust (cargo) | optional, stable | the terminal interface `xksm-tui` (no crates: builds offline) |
| OpenCL | optional | GPU path of the word-vector table build |

`tools/install.sh --check` tests all of them (it compiles and runs a probe against SQLite) and
prints the package command for the distribution (only the Arch line is run;
the others are the same packages under their Debian and Fedora names, not tested):

```
sudo pacman -S --needed base-devel cmake ninja sqlite rust                          # Arch, CachyOS
sudo apt install build-essential cmake ninja-build libsqlite3-dev sqlite3 cargo      # Debian, Ubuntu
sudo dnf install gcc cmake ninja-build sqlite-devel sqlite cargo                     # Fedora
```

Platforms: Arch Linux x86_64 is built and tested (CachyOS; for v0.2.0 also once in a clean
`archlinux:base-devel` container). Other Linux distributions need only the packages above; the code has scalar fallbacks
for every SIMD kernel, but other architectures and other operating systems have not been run;
Windows is not supported.

## 3. Install from a fresh clone

```
git clone https://github.com/Lasimeri/XKS-Memory
cd XKS-Memory
git checkout v0.2.0              # a release, or stay on main
tools/install.sh
```

`tools/install.sh` checks the requirements, builds in `build-install/` (Release), runs the
whole test suite (nothing is installed if a test fails; `--no-test` skips it), installs to
`~/.local` (`--prefix DIR` for elsewhere), creates `~/XKS-memories` (mode 700) and registers
the Claude Code plugin when the `claude` command exists (`--no-claude` skips it). Running it
again upgrades in place; the memories are never touched.

Check the result:

```
xksm version                     # xksm 0.2.0, schema 4
xksm sessions                    # recorded sessions (empty on a new machine)
xksm verify                      # every SHA-256 of the store recomputed
xksm-tui                         # the terminal interface
```

## 4. Bring back the memories

Copy the saved `~/XKS-memories` back before the first agent session runs (or stop the agents
first), keeping the modes private:

```
cp -a /backup/XKS-memories ~/XKS-memories
chmod 700 ~/XKS-memories ~/XKS-memories/.index
chmod 600 ~/XKS-memories/.index/agents.xksm*
xksm verify                      # memories, sources and session files hash as recorded
xksm mirror                      # rewrites the session folders if only the index was saved
```

A store from an older release is migrated on first open (schema versions only move forward;
a store newer than the program is refused, never altered). Stores from before 0.2 kept in
`~/.local/share/xksm` are moved into `~/XKS-memories/.index` by `tools/install.sh`.

Without a saved store, a Claude Code history can be re-imported from its transcripts
(`~/.claude/projects/*/*.jsonl`, if they still exist): `xksm transcripts`.

## 5. The semantic port (optional)

Recall works without a port: BM25 on the typed words plus the store's own word-vector table.
The port adds query expansion (related words) and is what the measured default
("hybrid + port v2, every word") uses. If the port file survived, record it again:

```
xksm port ~/XKS-memories/.index/agents.xksm /path/to/xksm-port-nomic-f32-v2.port
```

To rebuild it (once, offline; needs llama.cpp built from source and the F32 GGUF of
`nomic-embed-text-v2-moe`, 1,907,985,280 bytes; the v2 build took 51.6 s on an RTX 3090 Ti):

```
cmake -S . -B build -DXKSM_LLAMA_DIR=/path/to/llama.cpp -DXKSM_LLAMA_BUILD=/path/to/llama.cpp/build
cmake --build build --target port_build
tools/port_words.sh words.txt /usr/share/doc /usr/share/man
build/port_build MODEL.f32.gguf ~/XKS-memories/.index/agents.xksm OUT.port --words=words.txt REPO_DIR...
xksm port ~/XKS-memories/.index/agents.xksm OUT.port
```

Details and measurements: [EMBEDDING.md](EMBEDDING.md) section 5. The model runs only here,
never at query time.

## 6. Agent harnesses

- Claude Code: `tools/install.sh` registers the plugin; by hand:
  `claude plugin marketplace add ~/.local/share/xks-memory/claude-code` and
  `claude plugin install xks-memory@xks-memory`. After an upgrade:
  `claude plugin marketplace update xks-memory` and `claude plugin update xks-memory@xks-memory`,
  then restart Claude Code.
- omp: `ln -s ~/.local/share/xks-memory/omp/xks-memory.ts ~/.omp/agent/hooks/pre/` and copy
  `~/.local/share/xks-memory/omp/mcp.json` to `~/.omp/agent/mcp.json` (or the profile's
  `agent/` directory). Details: [AGENTS_INTEGRATION.md](AGENTS_INTEGRATION.md).

## 7. Building by hand and testing

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build            # C tests, CLI end-to-end, TUI unit and golden-screen tests
```

Sanitizer builds (the terminal interface is skipped there):

```
cmake -S . -B build-asan -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-sanitize-recover=all" && cmake --build build-asan && ctest --test-dir build-asan
cmake -S . -B build-tsan -DCMAKE_C_FLAGS=-fsanitize=thread && cmake --build build-tsan && ctest --test-dir build-tsan
```

The version is one number for the library, the CLI, the terminal interface and the Claude Code
plugin; configuring fails if `include/xksm.h`, `CMakeLists.txt`, `tui/Cargo.toml` and the
plugin manifest disagree.

## 8. Checking a release from its source alone

```
tools/release-check.sh                                                   # a clean clone of this checkout
tools/release-check.sh --tag v0.2.0 https://github.com/Lasimeri/XKS-Memory   # the release tarball, no git
```

It installs into a scratch prefix and a scratch memory root, stores a turn, lists it, verifies
every hash and opens the terminal interface headless. Nothing is uploaded; your real install
and memories are not touched.
