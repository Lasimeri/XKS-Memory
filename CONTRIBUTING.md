# Contributing

1. Open an issue describing the change before large work.
2. Core code is C17 and must build with zero warnings under `-Wall -Wextra -Wpedantic`.
   TUI code is Rust and must pass `cargo clippy -- -D warnings`.
3. Every change keeps the CPU-only build complete and its tests passing.
4. Accelerator code (CUDA, KNC) stays inside `backends/` and behind CMake options that are
   off by default.
5. Performance claims in pull requests include the command, the hardware, the raw numbers
   and their spread. Unmeasured claims are not merged.
6. No network access at runtime, no telemetry, no bundled model weights without a verified
   redistributable license.
7. Never commit memory databases, credentials or personal data. See `.gitignore`.

By contributing you agree that your contribution is licensed under the MIT License.
