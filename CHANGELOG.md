# Changelog

## Unreleased

- Repository created: license, project documents, directory layout.
- Phase 1: research notes, source matrix, design decisions D1-D11.
- Phase 2: CMake build, `libxksm` C ABI (`include/xksm.h`), schema v1 (source, memory, external-content FTS5 with secure-delete, change log), ordered migrations with refusal of newer stores, `xksm init|info|version`, schema test.
- Checksums: SHA-256 (32-byte schema columns) with a run-time-selected SHA-NI kernel and a portable scalar kernel; FIPS and OpenSSL cross-checked; benchmark `bench_sha256`. SHA-512 (CPU and batched CUDA) was built, measured and replaced; measurements kept in `reports/sha512-2026-11-06.md`.
- Plan for SHA-256 on the Knights Corner card: `docs/KNC_SHA256_PLAN.md`.
