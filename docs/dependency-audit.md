# dependency audit

reviewed 2026-07-29. review again by 2026-10-29 or before changing the pinned fff revision.

## reviewed inputs

- fff submodule: `63b126e7b0034d3a25b1c84d1972e1b84903c169`
- rust toolchain: `1.97.0`
- dependency lock: `vendor/fff/Cargo.lock`
- audit command: `cargo audit --file vendor/fff/Cargo.lock`
- reachability command: `cargo +1.97.0 tree --manifest-path vendor/fff/Cargo.toml -p fff-c --edges normal,build`

The reviewed revision fixes literal operator-like search tokens such as `!=`.
The fff C API and `Cargo.lock` are unchanged from the previous pin. Upstream
`main` was at `2cf871210b7c10cb1bb3e99d54b36125c6b63ed2`; its later changes only affect
Neovim UI and documentation, Nix build flags, and a benchmark.

## findings

`cargo audit` reports `RUSTSEC-2026-0176` and `RUSTSEC-2026-0177` for pyo3 0.24.2 in the workspace lockfile. pyo3 is not in the normal or build dependency graph of the shipped `fff-c` package, so neither advisory is reachable from `mereader-tui`.

The audit also reports these warnings:

- `RUSTSEC-2025-0141`: bincode 1.3.3 is unmaintained. It is present through heed. `mereader-tui` passes null fff frecency and query-history database paths, so fff does not open either heed-backed database.
- `RUSTSEC-2026-0183` and `RUSTSEC-2026-0184`: git2 0.20.4 has unsound `Remote::list()` and buffer-created blame APIs. Neither affected API is called by the production fff source.
- `RUSTSEC-2026-0097`: rand 0.8.5 can be unsound with a custom logger and `rand::rng()`. This version is build-only through phf, doxygen-rs, lmdb-master-sys, and heed; the affected runtime combination is absent.

No reported high- or critical-severity advisory is reachable from the shipped binary. The warnings above are accepted only for this pinned revision and must be reassessed by the review date.
