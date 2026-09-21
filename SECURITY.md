# Security policy

## Reporting a vulnerability

Please **do not open a public issue** for security findings. E-mail
`security@shadowc.example.invalid` (replace with the maintainer address at
release time) with:

- affected version / commit and branch,
- minimal reproducer (source + `--seed`/CLI flags + platform),
- expected vs observed behavior,
- impact framing (what an attacker gains, and at what cost).

We aim to triage within 5 business days and ship a tracked fix + advisory
before public disclosure.

## What shadowc does and does not claim

Shadow-Compile is an **obfuscation** engine, not a cryptographic protection
layer. In scope of our threat model:

- raising the cost of static CFG extraction and string harvesting,
- blocking naive `strings`/signature match triage on shipped binaries,
- reproducible hardening for firmware/embedded distribution.

Out of scope, even after all roadmap work:

- preventing a determined attacker with full binary access and unbounded
  compute from eventually recovering secrets;
- encrypting data at rest on the device (use real SEs/HSMs/TEEs for that);
- protecting against dynamic debugging of your *build* pipeline (the salt and
  `--seed` are build-time secrets; treat them as such).

## Trusted computing base

Keep **off** the device: `SHADOWC_SALT`, customer tokens, `--seed` for
reproductions that must stay private. Ship only the hardened artifact.

If a salt is compromised, rotate it immediately: tokens are deterministic per
salt (see `docs/COMMERCIAL.md`), so a leaked salt permits minting.

## Supported versions

| Version | Supported |
|---------|-----------|
| 0.5.x | Yes (active) |
| 0.4.x | Yes (maintenance) |
| earlier | No |

Security fixes backport to the latest 0.x line only.

## Dependency supply chain

The Python layer is stdlib-only (no pip dependencies). The C++ plugin links
LLVM from the distro or vendor toolchain; verify your `llvm-config` provenance
and pin the tool versions via `tools/check_toolchain.py` in CI.