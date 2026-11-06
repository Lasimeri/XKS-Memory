# Changelog

## Unreleased

- Repository created: license, project documents, directory layout.
- Phase 1: research notes, source matrix, design decisions D1-D11.
- Phase 2: CMake build, `libxksm` C ABI (`include/xksm.h`), schema v1 (source, memory, external-content FTS5 with secure-delete, change log), ordered migrations with refusal of newer stores, `xksm init|info|version`, schema test.
- SHA-512 checksums (schema v1 checksum columns are 64 bytes): portable C implementation, test against FIPS vectors, CPU benchmark, optional CUDA batch kernel and benchmark (`-DXKSM_CUDA=ON`), measurements in `reports/sha512-2026-11-06.md`.
