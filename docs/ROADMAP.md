# Roadmap

Order is risk-first: every item closes a documented gap before a new feature
opens one.

## 0.5 — Correctness & breadth

- [ ] LLVM version matrix in CI (18 / 19 / nightly) — run the *same* pass-line
      over all, keep `opt -passes=verify` green.
- [ ] Full pipeline under ASan: build passes + `opt` with sanitizers, fuzz the
      IR entry points (this is where demotion bugs hide).
- [ ] Equivalence fixture farm: extend the plain-vs-hardened checker to a
      corpus (crc-style nested loops, string-heavy I/O, recursive Fibonacci,
      switch-heavy state machines, bitfield math). Every fixture must match
      **exact stdout**, closing the CFF regression class documented in
      `docs/PASSES.md`.
- [ ] `-O0` polish: verify `-disable-O0-optnone` interplay across all three FP
      passes for unoptimized debug builds.
- [ ] C++ / interprocedural hardening pass-through: end-to-end C++ demo,
      virtual-dispatch flattening decision tree.

## 0.6 — Toughness

- [ ] Opaque-predicate strong constants: feed real hard problems
      (3-SAT / factorization-size vector reductions) into `boguscf` instead of
      the hash-ish LCG, maintain fold-correctness.
- [ ] Variable-initializer scrambling via `llvm.global_ctors` ordering shuffle
      (with a *verifier that re-runs the same ctors against original data*).
- [ ] `shadowc-virt` (CPython-style virtualization) — enterprise weave,
      reserved; design doc + spike.

## 0.7 — Form factor & tooling

- [ ] `shadowc` pip/`uv` distribution (no source tree required) + a small
      `shadowc-opt` IR browser.
- [ ] Output manifests (JSON: input hash, seed, pass set, target triple,
      verification result).
- [ ] Packaging for MetaSploit-style "hardened test.bin in CI" one-liners.

## 0.8 — Enterprise Vault deepening

- [ ] Anti-debug trap pass (`ptrace` + `getppid`-guard), Vault-gated.
- [ ] String-key rotation per shipment (key derived from `--seed`, so
      `--seed N` shipments stay reproducible).
- [ ] TLS license-server gate (replacing the offline HMAC demo path), with
      offline-grace logic tested.

## Furnished baseline

Finished for 0.5: community hard predicates, all four Vault passes
(string/virt/trap/init), per-vendor salt rotation, build manifests, 36-test
equivalence fixture farm, LLVM 18/19 × opt-level CI matrix with ASan lane.
Frozen baseline 0.4: gatekeeper + 3 community passes + string Vault pass,
determinism under `--seed`, arm64/host targets, 20-test suite, firmware demo,
CI, full docs.