# Architecture

Shadow-Compile is a *Split-Core* obfuscation engine: the compiler front end and
the licensing root live together, but the heavy-duty *Vault* weaves are gated by
an offline gatekeeper. Everything above the gate is readable in this repository;
everything below it is a runtime-encoded build-time decision.

## Threat model

We raise the cost of static and semi-automated reverse engineering for an
attacker who possesses a distributed image:

- **Controlled-flow extraction** – blocking the naive "lift the control flow
  graph" attack used by FLIRT / decompiler automation.
- **String harvesting** – preventing `strings` and symbol-tab triage from
  exposing device secrets, API roots, or OTA endpoints.
- **Signature matching** – morph-by-default defeats byte-identical rebuilds
  across shipments.

Out of scope: full binary sonification against a determined reverse engineer
with unlimited compute, and all DRM-style "can the owner of the CPU derive the
secret" claims. Obfuscation is a *cost function*, not a cage.

## Pipeline

```
 source ─clang(-Ox)─► input.ll ─opt(+ShadowCPasses.so)─► hardened.ll
                                             │
                  ┌──────────────────────────┴──────────────────────────┐
                  │ --level community │  function(shadowc-substitution,  │
                  │                   │          shadowc-boguscf,        │
                  │                   │          shadowc-cff,            │
                  │                   │          shadowc-hardpred)       │
                  │ --level min      │  function(shadowc-cff)            │
                  │ --level enterprise│ + shadowc-string, shadowc-virt,  │
                  │                   │   shadowc-trap, shadowc-init     │
                  │                   │   (Vault, token-gated)           │
                  └──────────────────────────┬──────────────────────────┘
                                             ▼
                                   hardened.ll ─llc─► hardened.o
                                             │
                       ─emit ll │ ─emit obj  │ ─emit exe
                                 ▼            ▼
                          hardened.ll    hardened.o ─clang -no-pie─► exe
```

Stages live in `src/shadowc/pipeline.py`:

1. **`_emit_ir`** – `clang -O<k> -S -emit-llvm -Xclang -disable-O0-optnone`.
   The `-disable-O0-optnone` flag guarantees `opt` may optimise even at `-O0`.
2. **`_run_opt`** – one `opt` invocation with `-load-pass-plugin` and a
   `-shadowc-seed` (defaults to a fresh random seed per build ⇒ polymorphism).
   Pass selection is tier-driven (`COMMUNITY_PASSES`, `VAULT_PASSES`) but
   overridable with `--passes`.
3. **`_emit_target`** – `llc` to `.o`, then link. Host links with
   `clang -no-pie` (string encryption lowers globals to writable `.data`; a PIE
   link rejects the now-non-constant initialisers). Cross targets relink through
   `clang --target=<triple>`, which finds Debian/Ubuntu cross GCC toolchains.
   Stripping is controlled by `--no-strip`.

Tool location is version-pinned: `clang-18`/`opt-18`/`llc-18` are preferred so
an unrelated newer LLVM on `PATH` can never emit IR the plugin cannot parse
(mirrors `tools/check_toolchain.py` and `tests/conftest.py`).

## Split-Core gate semantics

The gate is a *decision* layer, not a *repository* split.

- **Community** – all four FP passes live in this tree, build with stock
  CMake, and are gated only by your conscience.
- **Enterprise Vault** – `shadowc-string`, `shadowc-trap`, `shadowc-virt` and
  `shadowc-init` are compiled against the same headers but *enforced* at
  runtime: `ZeroHackLicenseGate.require()` raises unless an HMAC-SHA256 token
  over `client_id` matches. The salt is the unlock secret.
  The public tree bounds the Vault unlock to a **demo salt**; a licensed build
  ships with a per-customer salt and, in production wiring, a TLS license
  server (see `docs/COMMERCIAL.md`).

Key invariant: **a vault pass never runs without a granted token**, so a leaked
community build cannot silently upgrade itself.

## Determinism contract

`--seed N` fixes every RNG in the plugin (per-function shuffled case IDs,
bogus-CTF seed patterns, substitution choices), giving byte-reproducible output
for reproducible shipments. Default is entropy ⇒ each build is a fresh morph.

## Cross-target contract

`llc` emits target `hardened.o` for the requested triple; the object is
self-contained. Link remains a *host*-side or *device*-side decision:

- object emit always works everywhere;
- exe emit requires clang to find a cross linker, surfacing a precise error
  naming `gcc-aarch64-linux-gnu` or suggesting `--emit obj`.

## Alignment with the split-brain reality

A typical firmware CI ships: a *local* hardening step into bit-perfect artifacts
plus a *device* flash that must not rebuild. The Split-Core model matches:
compile-time secrets (salt) never reach the device; the device only ever sees
morphed, string-free code.