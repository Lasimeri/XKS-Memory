# XKS-Memory: notes for coding agents

- Core: C17, CMake + Ninja. `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release &&
  cmake --build build && ctest --test-dir build`.
- Sanitizer builds before every push that touches C:
  `-DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-sanitize-recover=all"` (build-asan) and
  `-DCMAKE_C_FLAGS=-fsanitize=thread` (build-tsan); all tests must pass in both.
- TUI: Rust crate in `tui/` (`xksm-tui`), links `libxksm.a` through `include/xksm.h` (`tui/src/ffi.rs`
  mirrors the C structs field for field; keep them in sync). Build: `cd tui && cargo build
  --release --offline`. No other Rust.
- Agent harnesses: `cli/harness.c`, `integrations/`; test with `tests/cli_harness.sh` (ctest
  `cli_harness`). Never commit stores, transcripts or memory content.
- Authoritative store: SQLite. Everything else (FTS5, caches, vector codes) is derived.
- Module map and tests: ARCHITECTURE.md "Built modules". Private headers live next to
  their sources (`src/core/internal.h`, `pool.h`, `sha256.h`, `src/vector/*.h`).
- Threads: POSIX threads only (C11 `<threads.h>` is invisible to ThreadSanitizer with
  glibc); two pools per handle, recall and embedding, 4 workers each by default, 1..128
  (`XKSM_THREADS`, `XKSM_EMBED_THREADS`).
- Kernels: every SIMD kernel has a scalar reference, is selected by CPUID at run time, is
  tested against the reference, and is kept only if measured faster.
- Optional backends (`backends/knc` = vector work on the Xeon Phi MVEX units, `backends/cuda`
  = future GPU work; CMake `XKSM_KNC`, `XKSM_CUDA`,
  default OFF) must never be required.
- Zero compiler warnings (`-Werror` is on). Every feature lands with a test.
- No runtime network access, no telemetry, no shell execution from imported content.
- Performance claims need a measured number, the command, and the hardware; raw output
  goes to `benchmarks/out/` (not committed), curated results to `reports/`.
- Never commit databases, credentials, or personal data.
