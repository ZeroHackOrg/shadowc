# Testing / Correctness Model

This document is the honest account of what the test suite proves, how it is
organized, how to run it, and — explicitly — **what it does not prove**. Read
this before treating a green test run as "no errors".

## 1. What "passing" means here

The suite is *behavioral equivalence* testing, not formal verification. A
green suite means:

> every program in the pinned corpus, compiled at the tested optimisation
> level and run through the tested pass pipeline, still produces byte-identical
> stdout to an unhardened build — and the produced IR is structurally valid
> (`opt -passes=verify`) and the intended mitigation topology is actually
> present.

It is deliberately *not* a mathematical proof that arbitrary input preserves
semantics. See §4.

## 2. Verification layers

### L1 — Orchestration unit tests (`test_pipeline.py`, `test_gatekeeper.py`)
No LLVM needed. Locks the *decision* layer: pass pipeline strings per tier,
vault pass naming surface, licence gate acceptance/rejection, CLI status/exit
codes, source-extension errors.

### L2 — Pass structural tests (`test_obfuscation_passes.py`, `test_vault_passes.py`)
Compile a fixed C program at the suite's `-O` level, run each pass (or the
vault weave) through `opt`, then:

- assert the **mitigation topology** is present in the IR:
  - CFF: dispatcher state-machine + early return
  - bogus CF: unreachable clones fed by parity predicates
  - substitution: mixed expression shapes
  - hard predicates: `shadowc.hard.dead` shadow paths + predicate body
  - trap: `ptrace(TRACEME)` guard + fail-closed `exit(173)`
  - virt: `shadowc.vm.prog` blob + `eval32`/`eval64` interpreters + routed calls
  - init: `zeroinitializer` globals + masked payload globals + rebuild ctors
- run `opt -passes=verify` on the result (invalid IR = hard failure)
- link and run the hardened binary and diff stdout against the plain build.

When `strace` is installed the trap pass is additionally executed **under a
tracer** and must terminate with exit code 173 (fail-closed). It is skipped
(not failed) when `strace` is absent — CI installs it.

### L3 — Equivalence fixture farm (`test_equivalence.py`)
Six deterministic C programs under `tests/fixtures/` with **pinned golden
outputs** in `tests/expected/`:

| fixture | exercises |
|---|---|
| `crc_nested.c` | nested loops + byte/bit CRC (the CFF entry-successor regression shape), pinned digest |
| `fib.c` | recursion + 64-bit tail-loop math |
| `switch_machine.c` | switch state machine (CFF `lowerSwitch`) |
| `bitfield.c` | bitfields, masks, unions, shifts |
| `strings_io.c` | printf/memcpy string pipeline (no string vault) |
| `stats.c` | nested loops, arrays, modulo arithmetic |

Each is built three ways — plain `clang`, community pipeline, enterprise
vault pipeline — and *all three* must equal the pinned expected stdout.
The pipeline string used is the exact one the CLI would emit (including the
gatekeeper-issued token for enterprise). `test_cff_regression_fixture_value_is_pinned`
keeps the CRC digest honest: it must never change silently.

### L4 — Multi-language lanes (`test_multilang.py`)
Same contract, additional front-ends that genuinely produce/consume a
hardenable IR surface:

- **C++** (`clang++-18` front-end): `fixtures_cpp/rtx.cpp` mixes
  `std::vector`, `std::string` builders, a constructor-priority static global,
  and an exception (`throw/catch`) path. Plain == community == enterprise.
- **Python** via **Cython** (`--embed`, standalone CPython main): `py_arith.py`
  is compiled to C, then hardened with the identical IR pipeline. The lane is
  **Enterprise-Vault-gated**: community/min tiers are refused up front with an
  actionable "mint a token" error (asserted in
  `test_python_lane_rejected_without_enterprise_license`); enterprise runs
  prove plain == hardened == golden and plaintext-free output.
  `python3-dev` is required; the test skips when Cython is missing.
- **Go**: Go has **no** clang-front-ended IR pipeline, so Go source itself is
  *rejected with actionable guidance* (asserted). The supported hardening
  surface is the **native cgo boundary**: `examples/golang_cgo/native.c` is
  built by shadowc (enterprise tier) via `build_native.sh`, archived, and
  cgo-linked from `main.go`; a `go build` of the plain vs hardened archive must
  emit identical output. Requires a `go` toolchain; skips when absent.
- A `.pyx` and `.rs` inputs are explicitly routed with clear errors rather
  than silently mis-handled.

### L5 — End-to-end CLI (`verify.yml` CI + manual smoke)
- community + enterprise builds of the firmware sample; stdout identical to
  the pinned crc (hardened binary prints it)
- enterprise build is **plaintext-free** under `strings` (device secret and
  OTA endpoint absent)
- `--manifest` emits and its fields validate
- `--seed 7` twice → `cmp` byte-equal (determinism)
- vault enterprise `--seed 7` twice → byte-equal; `SHADOWC_SALT=A/B` →
  byte-different (per-vendor rotation)
- arm64 cross **object** and **exe** emission/link (`file` → aarch64; local
  `test_targets.py` + CI cross step)
- unlicensed enterprise build fails cleanly (gate negative path)
- trap-under-tracer fail-closed `exit(173)` — local now that strace is
  installed, and in CI

### L6 — CI matrix
`.github/workflows/verify.yml` runs the L2/L3/L4 suites under
`SHADOWC_TEST_OPTLEVEL ∈ {-O0, -O1, -O2}` on **LLVM 18 and 19**, an **ASan +
UBSan** build lane, and the gate-negative lane. Cython is installed for the
Python lane; `strace` for the trap-under-tracer path.

## 3. Coverage matrix

| dimension | covered | not covered locally |
|---|---|---|
| languages | C, C++ (clang++), Python (Cython embed), Go (native cgo archive) | in-place Go/Rust/C#/SWIFT (no IR front-end); `rustc --emit=llvm-ir` documented, untested |
| opt levels | `-O0`, `-O1`, `-O2` (LLVM 18 local; 19 in CI) | `-O3`, `-Os`, cross-arch opt variants |
| LLVM | 18 (local), 19 (CI) | ≥ 16 (code claims support, no CI) |
| tiers | min, community, enterprise (all 8 passes) | — |
| targets | host x86_64; arm64 object + exe link (local + CI, apt `gcc-aarch64-linux-gnu`) | arm32, riscv |
| Python lane | gated to **enterprise**; community/min rejection + enterprise plain-equivalence both asserted | — |
| semantics | fixture corpus, golden-pinned | arbitrary user programs (see §4) |
| sanitizers | ASan+UBSan CI lane on pass suite | MSan, TSan |

## 4. Where "it works, no errors" would be wrong

The following are true, by design — a green suite does **not** cover them:

1. **Correctness is test-proven, not mathematically proven.** The passes are
   heuristic (deterministic-random sampling with `-fraction` knobs). The farm
   covers representative patterns; *a program we have not tested can still
   mis-morph* — especially adversarial CFGs, `longjmp`/`setjmp`, hand-written
   assembly, absolute-address/IP-relative code, or programs that read their
   own `.rodata`. Equivalence is checked by stdout equality on a pinned corpus,
   not by a refinement/SMT prover.
2. **Structural validity is `opt -passes=verify`, not a full proof.** It
   catches invalid IR (the classic width/PHI/GEP bugs) at build time, but
   "verify passed" does not prove semantic preservation in the corners above.
3. **Environment surface is wider than what a laptop/CI has.** Anything not
   exercised locally: LLVM 19 (CI only), arm32, riscv, LLVM ≥ 16, Go without
   the `go` toolchain. `strace` and `gcc-aarch64-linux-gnu` are now installed
   locally, so trap-under-tracer and arm64 exe-link are verified here too.
   Tests for genuinely missing tools *skip*, not fail — a skipped row is an
   unverified claim.
4. **A single seed is not the whole polymorphic space.** Two builds may
   exercise different pred/int-substitution shapes for the same source; the
   farm tests a fixed seed (`42`) plus CLI determinism tests (`7`). Other seeds
   are believed equivalent but are only sampled, not exhausted.
5. **Hardening is stealth work, not secrecy.** `shadowc-string`/`shadowc-init`
   are build-time XOR ciphers; defeating *cheap* harvest is the goal. A
   determined analyst with the binary and time can always recover. No test
   claims cryptanalytic strength.
6. **Pass interplay is tested in one fixed order.** The pipelines run
   `function(sub,bogus,cff,hardpred)` then `string,virt,trap,init`. Different
   orderings, or future passes, are not equivalently verified.

## 5. Failure semantics

| symptom | meaning | action |
|---|---|---|
| `LLVM ERROR: Broken module found` / opt verify crash | a pass emitted invalid IR — a **hard blocker** | fix the pass (see lib/passes); add a regression fixture that triggered it |
| output mismatch (plain vs hardened vs golden) | semantic regression | fix the pass; never "update" the expected file to paper over it. `crc_nested` is deliberately pinned to catch this class permanently |
| structural assertion fails (e.g. eval32 missing) | mitigation not actually injected (fraction/seed draw or pass skip) | check `-fraction` knobs / seed / whether the function is exempt (`shadowc-skip`) |
| suite skips | environment lacks a tool (go/cython/python3-dev/cross-gcc) | run the step in CI, or `pip install cython` / apt install the missing tool |
| Python lane at community/min | user tried a paid feature unlicensed | intentional: `_emit_ir` refuses; mint a token + `--level enterprise` |
| L1 failures | orchestration regressions (pipeline strings, gate, CLI) | fix `src/shadowc/*`, `src/gatekeeper.py` |

## 6. Running the suite

```sh
# build the pass plugin first
cmake -S . -B build -DLT_LLVM_INSTALL_DIR=/usr/lib/llvm-18 -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# everything
python3 -m pytest tests/ -q

# one language / one pass / one tier
python3 -m pytest tests/test_multilang.py -q -k "cpp or python or cgo"
python3 -m pytest tests/test_vault_passes.py -q
python3 -m pytest tests/test_multilang.py -q -k cpp -k "enterprise"

# the equivalences at a different optimisation level (CI does all three)
SHADOWC_TEST_OPTLEVEL=-O0 python3 -m pytest tests/test_equivalence.py tests/test_multilang.py -q
SHADOWC_TEST_OPTLEVEL=-O2 python3 -m pytest tests/test_equivalence.py tests/test_multilang.py -q

# multi-language smoke via the real CLI
./bin/shadowc tests/fixtures_cpp/rtx.cpp -o /tmp/rtx.h && /tmp/rtx.h
./bin/shadowc --lang python tests/fixtures_py/py_arith.py -o /tmp/pya.h && /tmp/pya.h
```

## 7. Adding a fixture

1. Add `tests/fixtures*/<name>.(c|cpp|py)` — deterministic stdout only, no
   network, no CPU-timing dependence.
2. Generate the golden file with a **plain** build (`tests/expected/<name>.txt`),
   review it, commit it with the fixture.
3. Register it in the relevant parametric test (`fixture_ids()` for C, the
   multilang tests for C++/Python/Go).
4. Rule: when a commit changes a golden value, that is a **semantic change** —
   it must be a deliberate, reviewed fixture edit, never a test accommodation.

## 8. Definition of done (release gate)

A release is "done" only when all of

- [ ] `pytest tests/ -q` green at `-O0`, `-O1`, `-O2` on LLVM 18
- [ ] CI matrix green (LLVM 19, ASan/UBSan lane, gate-negative lane)
- [ ] enterprise build: plaintext-free, deterministic under `--seed`, runtime
      identical to plain, manifest intact
- [ ] arm64 object emission **and exe link** verified (cross-gcc installed)
- [ ] C++ lane green; Python (Cython) lane green at **enterprise** and its
      community/min rejection asserted; Go (cgo archive) lane green
- [ ] docs: CHANGELOG + TESTING matrix updated; README sample versions
      consistent

Until then the label is `unreleased` / `-rc`, and the honest answer to "is it
stable?" is *"proven on the tested corpus, unproven beyond it"*.

## 9. Known gaps / roadmap

- **TinyGo lane (experimental)**: emitting TinyGo IR and running shadowc
  passes over it is directionally possible but the final link must go back
  through TinyGo's own toolchain, so in-place Go hardening does not persist.
  Tracked in `docs/ROADMAP.md`; not claimed as supported.
- **Rust**: `rustc --emit=llvm-ir` input is documented but untested (needs a
  fixture that exercises `panic`/abort paths through the passes).
- **Formal equivalence** (alive2-style refinement checks on emitted IR) would
  upgrade §4.1 from corpus-proven to proof-proven; not implemented.
- **TLS licence server** gate (replacing the offline HMAC demo path) — deferred
  by scope.