# XKS-Memory: notes for coding agents (AGENTS.md mirrors CLAUDE.md)

- Core: C17, built with CMake (`cmake -S . -B build && cmake --build build && ctest --test-dir build`).
- TUI: Rust crate in `tui/`, links `libxksm` through `include/xksm.h`. No other Rust.
- Authoritative store: SQLite. Everything else is derived and rebuildable.
- Module boundaries: `src/<module>/` per stage; `backends/cuda` and `backends/knc` are
  optional (CMake options `XKSM_CUDA`, `XKSM_KNC`, default OFF) and must never be required.
- Zero compiler warnings. Every feature lands with a test.
- No runtime network access, no telemetry, no shell execution from imported content.
- Performance claims need a measured number, the command, and the hardware.
- Never commit databases, credentials, or personal data.
