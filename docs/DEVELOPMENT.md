# Development

## Prerequisites

Same toolchain as README: LLVM 18 dev (`clang-18 opt-18 llc-18 libLLVM-18`),
CMake ≥ 3.20, C++17, Python 3.9+.

## Build & test loop

```sh
cmake -S . -B build -DLT_LLVM_INSTALL_DIR=/usr/lib/llvm-18 -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
/tmp/.../pytest # or: python3 -m pytest tests/ -q
```

Run the toolchain linter before diving in:

```sh
python3 tools/check_toolchain.py
```

It mirrors the exact pinning rules the pipeline uses (`clang-18` ⟶ `clang` ⟶
`clang-17`), so a mismatch here is a mismatch in CI.

## Editing the passes

- `lib/passes/Utils.cpp` — shared plumbing: annotation parsing
  (`functionAnnotations` unwraps `ConstantExpr` casts and GEP-free note
  globals), seeded RNG, `shouldProcess`.
- A new pass = new `llvm::PassPlugin`-registered `.cpp` + name it
  `shadowc-*`; register in `lib/passes/CMakeLists.txt` so the combined
  pipeline can drive it.
- Use the `llvm::XX &`-level IR, not legacy passes; pin API calls to LLVM 18.
  When you bump to LLVM 19+: build first, run tests, then check the
  `LT_LLVM_INSTALL_DIR` CI matrix.

After a change to `ControlFlowFlattening.cpp` (or any demotion-heavy path) the
*minimum* regression to run:

```sh
python3 -m pytest tests/test_obfuscation_passes.py -q -k cff
```

## Determinism development aids

- `--seed N` end-to-end: two builds with the same seed must byte-match.
  `tests/test_pipeline.py` pins this.
- `--seed 0` is the "shared default RNG" mode; do not assert `0` produces the
  same artifact as an unset seed.

## Debugging IR round trips

```sh
opt-18 -load-pass-plugin=build/lib/passes/ShadowCPasses.so \
  -shadowc-seed=42 -passes='function(shadowc-substitution,shadowc-boguscf,shadowc-cff)' \
  -S input.ll -o hardened.ll
llc-18 hardened.ll -o hardened.o
```

`--keep-temps` on the CLI preserves the intermediate `hardened.ll`/`.o`/`.exe`
for inspection instead of cleaning the temp dir.

## Python layer

- `src/shadowc/pipeline.py` — orchestration only. No LLVM bindings, no
  subprocess wizardry beyond pinned tool resolution.
- `src/gatekeeper.py` — the license gate, deliberately dependency-free
  (`hmac`/`hashlib` only) so CI and packaged CLI never need wheels.

Keep operator CLI changes covered by `tests/test_gatekeeper.py` +
`tests/test_pipeline.py`; both run on a stock Python with the plugin built.

## CI

`.github/workflows/verify.yml` — Ubuntu 24.04, apt LLVM 18, cross package
`gcc-aarch64-linux-gnu` (exercises the arm64 `-o exe` path), pytest. See
`docs/ROADMAP.md` for the planned LLVM-matrix and sanitizer lane.

## Style

- C++17, no exceptions in the plugin; LLVM style for IR code (use
  `emit_uREG`-style names consistent with the existing passes).
- Python: stdlib only, linear, comment-forward; docstrings on every public
  function.
- No comments unless they encode *contracts* (the CFF entry-succ invariant,
  `-no-pie` rationale, tool-version pinning rationale).

## Releasing (0.5.x cadence)

1. Run `pytest -q` + `check_toolchain.py` + the firmware equivalence check
   (also at `-O0`/`-O2` via `SHADOWC_TEST_OPTLEVEL`).
2. Bump `Project()` version in `CMakeLists.txt`, `pyproject.toml`,
   `src/shadowc/__init__.py` together (the CLI `--version` reads the Python one).
3. Update `CHANGELOG.md`, tag `vX.Y.Z`.