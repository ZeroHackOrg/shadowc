# Changelog

All notable changes to this project are documented here. The format is based
on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project
adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- **Hard opaque predicates** (`shadowc-hardpred`, community): predicates drawn
  from a family of SAT-hard bit-vector identities (half-adder, distributivity,
  `x^3-x ≡ 0 (mod 6)`, vector-reduction linearity) threaded through dead
  control-flow clones; seeds are runtime values of matching bit width.
- **Anti-debug trap** (`shadowc-trap`, vault): `ptrace(TRACEME)`-failure guard
  at process entry, fail-closed `exit(173)`.
- **Operator virtualization** (`shadowc-virt`, vault): selected integer
  arithmetic is lifted into a masked bytecode blob consumed by seeded
  `eval32`/`eval64` switch interpreters.
- **Initializer scrambling** (`shadowc-init`, vault): writable globals are
  zeroed and rebuilt at load time by ctors from XOR-masked payload globals,
  with seeded ctor priorities.
- **Seed-derived string key rotation** (`shadowc-string`): per-vendor salt
  (`SHADOWC_SALT`, `--string-salt`) mixes a seeded keystream, so every vendor
  gets a different ciphertext for the same source.
- **Build manifests** (`--manifest`): JSON with source/artifact hashes, seed,
  pass list, target and a runtime stdout fingerprint for provenance.
- **Equivalence fixture farm** (36 tests): six deterministic programs
  (nested-loop CRC, recursion, switch machine, bitfields, strings, stats)
  pinned to golden outputs and verified plain == community == enterprise.
- Vault structural tests for every enterprise pass, incl. `strace`-verified
  fail-closed behavior when strace is present.
- CI matrix: LLVM 18/19 × `-O0`/`-O1`/`-O2`, plus ASan/UBSan lane and token
  gate-negative lane.

### Fixed

- **Control-flow flattening no longer miscompiles multi-loop functions.** The
  state machine started at the first body block in *module layout* rather than
  at the block the original entry reached; when a loop head was laid out after
  unrelated bodies (e.g. an image-init loop preceding a CRC loop), code ran on
  uninitialized state and produced wrong output. The dispatcher now seeds from
  the original entry successor (or the peeled `shadowc.first` leader).
- **Hard predicates no longer mix operand widths.** `opaqueSeeds` could pick a
  `i32` seed and an `i64` value, producing a `mul i32 2, i64` that failed the
  IR verifier under the full enterprise pipeline; seeds are now width-matched.
- **Initializer scrambling handles scalar globals.** The rebuild ctor emits a
  one-index GEP and scalar zero for non-array globals instead of an invalid
  two-index GEP / aggregate zero.
- **Seed determinism restored.** The string pass no longer folds the (random)
  temporary module path into its keystream; a fixed `--seed 7` rebuilds
  byte-identical binaries again.
- Cross-target `--emit exe`: link failures now report a precise hint (install
  `gcc-aarch64-linux-gnu` or use `--emit obj`) instead of a raw `ld` trace.

## [0.5.0] - 2026-09

### Added

- Split-Core gatekeeper (`ZeroHackLicenseGate`): HMAC-SHA256 vault unlock over
  `client_id` + salt; `shadowc-string` gated behind it.
- CLI + pipeline (`bin/shadowc`, `src/shadowc/`): clang → opt → llc → link,
  `--level community|enterprise|min`, `--seed` determinism, `--emit
  exe|obj|ll`, cross targets, `--status`, `--version`.
- Passes: `shadowc-cff` (flattening), `shadowc-boguscf` (opaque CF), controlled
  `shadowc-substitution` (`+`, `-`, `*`, `^`), `shadowc-string`
  (XOR-injection, Vault).
- Tooling: `tools/gen_license.py`, `tools/check_toolchain.py`,
  `scripts/build.sh`.
- Tests (20): gatekeeper, pipeline, all passes + plain-vs-hardened equivalence.
- Docs: architecture, passes, development, commercial, roadmap; LICENSE,
  SECURITY, CONTRIBUTING; CI workflow with arm64 cross-link verification.

### Fixed

- Tool version pinning: pipeline resolves `clang-18`/`opt-18`/`llc-18` first so
  a newer distro `clang` can never emit IR a pinned `opt-18` cannot parse.
- Vault pass naming (`shadowc-string` vs `string`) normalized across gatekeeper
  and pipeline.
- `functionAnnotations` in `Utils.cpp`: safe under LLVM 18 where the annotation
  operand is a bare `Function` (not a bitcast `ConstantExpr`) and the note may
  be a direct `GlobalVariable` (no GEP wrapper).
- `-no-pie` linking once string globals become writable.