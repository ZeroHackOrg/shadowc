# Shadow-Compile (a.k.a. `shadowc`)

**LLVM IR obfuscation engine with a Split-Core licensing gate.**

Shadow-Compile hardens C/C++ (and LLVM IR) binaries for firmware and other
distribution-facing code. It rewrites control flow into opaque state machines,
blends honest code with unreachable clones, substitutes arithmetic, and — under
the Enterprise Vault — encrypts string literals so secrets never sit in
`.rodata`.

The repository ships the full Community toolchain and the gate logic. The
*Enterprise Vault* passes are compiled behind a license check (see
[`docs/COMMERCIAL.md`](docs/COMMERCIAL.md)).

---

## Feature Matrix

| Layer | Pass | What it does | Tier |
|-------|------|--------------|------|
| FP/function | `shadowc-boguscf` | Opaque predicates + unreachable clones via a crypto-looking RNG | Community |
| FP/function | `shadowc-substitution` | Mixed instruction substitution (`x ^ y` ⇄ `(x \| y) & ~(x & y)`, `x + y` ⇄ `x - (-y)`, `x * (2^16)` ⇄ `x << 16`) | Community |
| FP/function | `shadowc-cff` | Control-flow flattening — every block becomes a case in one state machine | Community |
| FP/function | `shadowc-hardpred` | SAT-hard opaque predicates (half-adder, distributivity, mod-6 congruence) | Community |
| Module | `shadowc-string` | String global encryption by XOR + injected runtime decryptor ctors | **Enterprise Vault** |
| Module | `shadowc-trap` | `ptrace` anti-debug guard, fail-closed `exit(173)` | **Enterprise Vault** |
| Module | `shadowc-virt` | Operator virtualization into a masked bytecode interpreter | **Enterprise Vault** |
| Module | `shadowc-init` | Global initializer scrambling rebuilt from masked ctors | **Enterprise Vault** |

All Community passes run as a combined `function(...)` pipeline in a single
`opt` invocation, deterministically under `--seed`. The four Vault passes are
gated behind an Enterprise token.

---

## Highlights

- **Single-command hardening.** `./bin/shadowc firmware.c -o firmware_hardened`
  runs clang → opt(passes) → llc → link and prints the artifact path.
- **Deterministic by design.** `--seed N` reproduces byte-identical output;
  the default is a per-build random seed, so each shipment is a fresh morph.
- **Cross-target.** Produce hardened `arm64` / `aarch64` / `amd64` / `x86_64`
  objects; link on the target once the cross toolchain is installed (CI proves
  this with `gcc-aarch64-linux-gnu`).
- **Split-Core gatekeeper.** Vault passes are gated behind an HMAC token, not
  VCS branches. The public tree stays academically reviewable; the licensed
  salt stays private. See [`tools/gen_license.py`](tools/gen_license.py).
- **Tests.** 36 pytest cases cover the gatekeeper, the pipeline, every pass on
  generated IR (with vault structural assertions), and a six-program
  plain-vs-hardened equivalence fixture farm pinned to golden outputs —
  runnable per `-O` level via `SHADOWC_TEST_OPTLEVEL`.
- **Validated against LLVM 18** (`clang-18`, `opt-18`, `llc-18`). Supports
  LLVM ≥ 16 with a warning below 18.

Example run with the bundled firmware sample:

```
$ ./bin/shadowc examples/firmware_template.c -o /tmp/secure.bin
shadowc: hardened artifact written to /tmp/secure.bin

$ /tmp/secure.bin
board=StayGuard fw=firmware.bin v0.5.0-rc1 crc=54313c60 sep=27 ota=36
```

The hardened binary's output is byte-identical to a plain build; under
`--level enterprise` the device-secret and OTA strings are no longer visible
via `strings`.

---

## Install & Build

### Prerequisites

| Component | Version | Notes |
|-----------|---------|-------|
| LLVM dev | **18.x** (≥ 16) | `clang-18`, `opt-18`, `llc-18`, `libLLVM-18` |
| CMake | ≥ 3.20 | |
| C++17 toolchain | gcc/clang | |
| Python | ≥ 3.9 | stdlib only |

Ubuntu 24.04:

```sh
sudo apt-get install -y llvm-18 llvm-18-dev clang-18 cmake build-essential
```

The Python layer has zero third-party dependencies (argparse + stdlib only).

### Build the pass plugin

```sh
cmake -S . -B build -DLT_LLVM_INSTALL_DIR=/usr/lib/llvm-18 -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

Artifact: `build/lib/passes/ShadowCPasses.so`.

For a reproducible convenience build run `scripts/build.sh`; to validate the
toolchain first run `python3 tools/check_toolchain.py`.

### Quick check

```sh
./bin/shadowc --status
python3 -m pytest tests/ -q
```

---

## Usage

```sh
./bin/shadowc <source> [--target arm64|amd64|host] [--level community|enterprise|min]
              [--emit exe|obj|ll] [--seed N] [--salt S] [--keep-temps] [-v]
```

| Flag | Default | Meaning |
|------|---------|---------|
| `--target` | `host` | Codegen target: `host`, `arm64`, `aarch64`, `amd64`, `x86_64` |
| `--level` | `community` | `community` (3 passes), `min` (CFF only), `enterprise` (+ Vault weaves) |
| `--emit` | `exe` | `exe` (linked), `obj` (relocatable), `ll` (hardened IR) |
| `--seed` | random | Fixed scramble seed ⇒ byte-reproducible output |
| `--opt-level` | `1` | clang `-O` used while emitting IR |
| `--no-strip` | off | Keep symbol table in the hardened artifact |
| `--passes` | – | Explicit comma-separated pass list (overrides `--level`) |
| `--client-id` / `--token` | anonymous / env | Enterprise vault unlock (`SHADOWC_TOKEN`) |
| `--salt` | demo salt | Gatekeeper salt (`SHADOWC_SALT`); see COMMERCIAL guide |

### Community — plain source to hardened binary

```sh
./bin/shadowc ../examples/firmware_template.c -o secure_output.bin
```

### Enterprise — unlock the Vault, encrypt strings

```sh
TOKEN=$(python3 tools/gen_license.py --client acme-fw --fresh | awk '/^token/ {print $3}')
SHADOWC_TOKEN=$TOKEN ./bin/shadowc --level enterprise --client-id acme-fw \
    ../examples/firmware_template.c -o secure_output.bin
```

`strings secure_output.bin | grep -i secret` now finds nothing.

### Cross-target — build hardened arm64 artifacts

```sh
./bin/shadowc --target arm64 ../examples/firmware_template.c -o secure_arm64.o   # object
./bin/shadowc --target arm64 ../examples/firmware_template.c -o secure_arm64.bin # needs cross linker
```

The final cross-link reuses clang’s discovery of the cross GCC toolchain
(`gcc-aarch64-linux-gnu`). Without it, emit `obj` and link on the device. The
CI workflow installs the cross toolchain and exercises the full path.

### Determinism / polymorphism

```sh
./bin/shadowc -O1 --seed 1234 app.c -o a.bin && ./bin/shadowc -O1 --seed 1234 app.c -o b.bin
sha256sum a.bin b.bin      # identical -> reproducible shipments
./bin/shadowc app.c -o a.bin && ./bin/shadowc app.c -o b.bin
cmp -s a.bin b.bin; echo $? # different -> per-build polymorphism
```

---

## Repository layout

```
bin/shadowc            CLI launcher (no install needed)
src/gatekeeper.py      Split-Core license gate (HMAC vault unlock)
src/shadowc/           CLI + pipeline orchestration (clang -> opt -> llc -> ld)
lib/passes/            C++ LLVM pass plugin (ShadowCPasses.so)
  ControlFlowFlattening.cpp   shadowc-cff
  BogusControlFlow.cpp        shadowc-boguscf
  Substitution.cpp            shadowc-substitution
  HardPredicates.cpp          shadowc-hardpred
  StringEncryption.cpp        shadowc-string   (Enterprise Vault)
  AntiDebug.cpp               shadowc-trap     (Enterprise Vault)
  Virtualization.cpp          shadowc-virt     (Enterprise Vault)
  InitScramble.cpp            shadowc-init     (Enterprise Vault)
  Utils.cpp                   annotations, RNG, helpers
include/shadowc/       plugin headers
tools/                 gen_license.py, check_toolchain.py
scripts/build.sh       convenience build
tests/, examples/      pytest suite + firmware demo
.vault/                (private) licensed salt + enterprise fixtures
```

## Documentation

- [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) — design, threat model, pipeline,
  Split-Core gate semantics.
- [`docs/PASSES.md`](docs/PASSES.md) — every pass, IR-level before/after, limits.
- [`docs/DEVELOPMENT.md`](docs/DEVELOPMENT.md) — hacking, testing, LLVM API notes.
- [`docs/COMMERCIAL.md`](docs/COMMERCIAL.md) — licensing, vault unlock, salt lifecycle.
- [`docs/ROADMAP.md`](docs/ROADMAP.md) — what ships next.

## License

Dual: source is Apache-2.0 with a zero-cost academic Community tier; the
Enterprise Vault passes are product-licensed via the Split-Core gate. See
[`LICENSE`](LICENSE) and [`vault/README.md`](vault/README.md).

**Security note:** obfuscation raises the cost of analysis; it is not
encryption-based DRM. Compile in the presence of an attacker who has your
binary, never your build secrets. See [`SECURITY.md`](SECURITY.md).