# Passes

All passes live in `lib/passes/` and load through a single pass-plugin
(`ShadowCPasses.so`) developed against LLVM 18. Each pass reads
`shadowc-skip` annotation support (`Utils.cpp`) so hot paths can be excluded:

```c
__attribute__((annotate("shadowc-skip")))
void hot_path(void) { ... }   // kept as-is
```

## `shadowc-cff` — Control-Flow Flattening (Community)

Morphs an unstructured CFG into a single loop with a state dispatch:

Lifting a function that looks like a graph of basic blocks that look like `x`:

```
            ┌───────────────┐
            │  entry        │
            │  store st0     │ ─► dispatch
            └──────┬────────┘
        ┌──────────┴──────────┐
        │  shadowc.loopentry  │  load state → switch
        └──────┬──────────────┘
 case 0 ┌──────┴──────┐   case 1 ┌──────┴──────┐
        │ block A     │          │ block B     │
        │ ...         │          │ ...         │
        │ store next; │          │ store next; │
        └──────┬──────┘          └──────┬──────┘
              └────► shadowc.loopend ◄──┘
```

- **LowerSwitch** first turns any switch into nested `icmp` comparisons; state
  IDs are integer-typed per block.
- **Demotion** — all live-out/loop-carried values (PHIs, escaping values) are
  sunk to per-slot `alloca`s (`DemotePHIToStack` / `DemoteRegToStack` clone),
  because a flattened body can no longer use CFG-PHI semantics: each state block
  reloads its inputs and re-stores its outputs.
- **State shuffle** — case IDs are mapped through a per-function LCG seeded by
  `-shadowc-seed`; constants appear randomized, never `0,1,2,...`.
- **Deterministic entry** — the machine *always* starts at the block the
  original entry reached (`shadowc.first` leader when the entry was a decision
  point). This is the contract that previously produced a miscompile when the
  first body block in *module layout* wasn't the entry successor; the fixed
  machine seeds from the original entry successor instead of layout order.

Limits: the demoted spill/reload adds stack traffic; the dispatcher becomes one
obvious block that a motivated analyst can identify. CFF is rapid-proto-
protective, not a final trick.

## `shadowc-boguscf` — Bogus Control Flow / Opaque Predicates (Community)

For each `br`-terminated block, a clone is created and both sides are wired to
deterministic (but opaque-looking) conditions produced by a seeded
crypto-flavored RNG (a hash-like constructor feeding constant folding). The
clone is unreachable garbage that costs a decompiler-toolkit's heuristics and
inflates the CFG; `opt -passes=verify` still passes because the conditional is
provably conservative under constant folding.

## `shadowc-substitution` — Instruction Substitution (Community)

Replaces arithmetic identities by equivalent mixed forms, mirroring OLLVM-class
operations:

| Observed | Substituted |
|----------|-------------|
| `a ^ b` | `(a \| b) & ~(a & b)` |
| `a + b` | `a - (-b)` |
| `a - b` | `a + (-b)` |
| `a * 2^16` | `a << 16` |
| `a * 2^-16` | `a >> 16` |

## `shadowc-string` — String Encryption (Enterprise Vault)

Module-level: every string-literal global is rewritten as an XOR-ciphered
`[N x i8]` blob; `llvm.global_ctors` gains a decryptor that runs before `main`.
The `.rodata` sections no longer contain readable constants, so `strings` on a
stripped hardened artifact finds neither device secrets nor OTA endpoints.

**Note:** the cipher is a build-time XOR key baked into the injector — honest
about the threat: it removes the *cheap* harvest, not cryptography-grade
confidentiality. Production deployments rotate the key and ship the XOR key via
the private vault. The build key is derived from `--seed` ^ per-vendor salt
(`SHADOWC_SALT`), so each vendor gets a distinct ciphertext for one source.

## `shadowc-hardpred` — Strong Opaque Predicates (Community)

Function-level. Stronger sibling of `shadowc-boguscf`: predicates are drawn
from a family of *hard* bit-vector identities, each provably constant yet
syntactically SAT-like — `(a^b) + 2*(a&b) == a+b`, wrapping distributivity,
`(x^3 - x) % 6 == 0`, and vector-reduction linearity. Every case needs a
SMT/bit-vector solver (or DAG algebra) to classify; dead clones thread the CFG
like boguscf. Seeds are runtime values of matching bit width (a former
mixing-width bug produced `mul i32 2, i64` and is covered by a regression
test).

## `shadowc-trap` — Anti-Debug Guard (Enterprise Vault)

Module-level. Inserts `shadowc.trap` at the process entry point: a
`ptrace(PTRACE_TRACEME)` probe that succeeds only when no debugger/tracer is
attached. On a traced run it fails and the guard calls `exit(173)` — fail
closed, no silent degradation.

## `shadowc-virt` — Operator Virtualization (Enterprise Vault)

Module-level. Selected integer arithmetic is lifted out of the instruction
stream: the operator is replayed at runtime by a switch interpreter
(`shadowc.vm.eval32`/`eval64`) reading opcodes from an XOR-masked bytecode
blob (`shadowc.vm.prog`). The operator of record moves from executable code
into *data*, so a lift fails wherever semantics live in ciphertext. Bit-correct
under per-width wraparound for add/sub/mul/and/or/xor; coverage and the blob
are seeded, hence reproducible under `--seed`.

## `shadowc-init` — Global Initializer Scrambling (Enterprise Vault)

Module-level. Writable constant globals are zeroed in the image and rebuilt at
load time by ctors from XOR-masked payload globals
(`shadowc.init.<name>` → `shadowc.ctor.<name>`). Ctor priorities are seeded, so
the relative order of rebuild ctors changes per build. Scalars and arrays are
addressed with their correct GEP shape (a scalar-global bug produced invalid
two-index GEPs and is regression-tested).

## Common attributes contract

`shadowc-skip` on a function or a call-site excludes that code from the FP
passes. Skip is checked in `shouldProcess` before any rewriting; a `-shadowc-seed`
of `0` is explicitly honoured as "use the shared default RNG" while non-zero
means deterministic.

## Verification story

Each pass is verified two ways:

1. `opt -passes=verify` after transformation (structural invariants: reachable,
   valid CFG, no dominance violations after flattening).
2. Integration equivalence: build demo C, run plain and hardened, byte-compare
   stdout (`tests/test_obfuscation_passes.py`). The CFF entry-fix regression is
   pinned by a nested floop accumulator test whose correct answer is checked
   exactly (`1ec865c1`-class fixtures).