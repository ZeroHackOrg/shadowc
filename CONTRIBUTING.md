# Contributing

Thanks for helping harden hardening. Please read `README.md` and
`docs/DEVELOPMENT.md` first.

## Ground rules

- **Correctness is the product.** A pass that breaks semantics is worse than no
  pass. Plain-vs-hardened stdout equivalence is the minimum bar for any FP/morph
  change; `opt -passes=verify` must stay green.
- **Dependency discipline.** The Python layer stays stdlib-only; the plugin
  stays LLVM-18-first (≥ 16). Don't sneak a pip/npm dependency in without a
  deliberate (and documented) decision.
- **Determinism.** `--seed N` must reproduce byte-identical output. If a change
  breaks that, it blocks the release.
- **No gold-plating subagents** — small, reviewable diffs, one concern each.

## Workflow

1. Fork; branch from `main`.
2. Implement + add a test that would have caught the bug (see the CFF
   entry-successor regression as the canonical model).
3. Local gate before pushing:

   ```sh
   python3 tools/check_toolchain.py
   python3 -m pytest tests/ -q
   ./bin/shadowc examples/firmware_template.c -o /tmp/check.bin \
       && /tmp/check.bin   # expect crc=54313c60 line
   ```

4. Open a PR; reference the issue; keep the diff surgical.

## Tips from the trench

- Demotion-heavy changes (CFF): the flatten step runs `DemotePHIToStack` /
  `DemoteRegToStack`; verify with a nested-loop accumulator fixture, not a
  single straight-line function. The dispatcher initial state must be the
  original *entry successor*, never the first block in layout order.
- LLVM API drift: build against `llvm-18`, then re-verify on 19+ in CI before
  advertising support.
- CLI surface changes go through `tests/test_pipeline.py` (flag → argv)
  so operator docs and UX stay in lockstep.

## Commit style

- Imperative subject, ≤ 72 chars, scope prefix where useful (`cff:`,
  `gatekeeper:`, `cli:`).
- Reference the fixture/issue that the change pins.
- No generated code, no build artifacts, no secrets in commits.

## Reviewing

- Verify the plain-vs-hardened claim on the actual fixture, not just the diff.
- Ask "what does this break for a multi-loop / non-trivial entry?", then run
  the equivalence corpus.

## Licensing

By contributing you agree your work is Apache-2.0 (community surface) with the
Vault passes following the repo's commercial terms. See `LICENSE` and
`docs/COMMERCIAL.md`.