//===- Passes.h - shadowc pass declarations ------------------------------===//
//
// Declarations for the shadowc LLVM new pass manager passes.
//
//===----------------------------------------------------------------------===//
//
// Community Core (public repository):
//   * shadowc-cff          - Control Flow Flattening
//   * shadowc-boguscf      - Bogus control flow + dead path insertion
//   * shadowc-substitution - Instruction substitution
//   * shadowc-hardpred     - Strong opaque predicates (identity/congruence family)
//
// Enterprise Vault (license gated in the tooling layer, not the C++):
//   * shadowc-string       - Cryptographic string virtualization
//   * shadowc-trap         - Anti-debug trap injection (ptrace guard)
//   * shadowc-virt         - Operational virtualization (bytecode interpreter)
//   * shadowc-init         - Global initializer scrambling (ctor-based rebuild)
//
//===----------------------------------------------------------------------===//

#ifndef SHADOWC_INCLUDE_SHADOWC_PASSES_H
#define SHADOWC_INCLUDE_SHADOWC_PASSES_H

#include "llvm/IR/PassManager.h"

namespace shadowc {

//----------------------------------------------------------------------------
// Community Core
//----------------------------------------------------------------------------

/// Flattens the structured control flow of a function into a single
/// switch-dispatch loop. Destroys the natural CFG that decompilers replay.
struct ControlFlowFlatteningPass : public llvm::PassInfoMixin<ControlFlowFlatteningPass> {
  llvm::PreservedAnalyses run(llvm::Function &F, llvm::FunctionAnalysisManager &AM);
};

/// Injects unreachable-but-statically-present branch paths guarded by opaque
/// predicates, plus (where safe) clones of target blocks, so static analysis
/// and decompilers are forced to map code that never executes.
struct BogusControlFlowPass : public llvm::PassInfoMixin<BogusControlFlowPass> {
  llvm::PreservedAnalyses run(llvm::Function &F, llvm::FunctionAnalysisManager &AM);
};

/// Rewrites arithmetic/logic instructions with bitwise-equivalent, longer
/// formula chains so itemised logic reconstruction becomes impractical.
struct InstructionSubstitutionPass : public llvm::PassInfoMixin<InstructionSubstitutionPass> {
  llvm::PreservedAnalyses run(llvm::Function &F, llvm::FunctionAnalysisManager &AM);
};

/// Injects *strong* opaque predicates built from arithmetic identities and
/// vector congruences that require real bit-vector reasoning to classify,
/// every one provably constant (so semantics are preserved) while looking
/// like a SAT-reducible condition at the IR level.
struct HardPredicatesPass : public llvm::PassInfoMixin<HardPredicatesPass> {
  llvm::PreservedAnalyses run(llvm::Function &F, llvm::FunctionAnalysisManager &AM);
};

//----------------------------------------------------------------------------
// Enterprise Vault
//----------------------------------------------------------------------------

/// Xor-encrypts every string global in the module and injects a load-time
/// unpacking constructor. `strings <binary>` then yields ciphertext only.
struct StringEncryptionPass : public llvm::PassInfoMixin<StringEncryptionPass> {
  llvm::PreservedAnalyses run(llvm::Module &M, llvm::ModuleAnalysisManager &AM);
};

/// Injects an anti-debug guard: `ptrace(PTRACE_TRACEME)` from every entry.
/// A tracing parent already owns us, TRACEME returns -1 and the process is
/// trapped out of normal execution (fail-closed, no output, exit 173).
struct AntiDebugPass : public llvm::PassInfoMixin<AntiDebugPass> {
  llvm::PreservedAnalyses run(llvm::Module &M, llvm::ModuleAnalysisManager &AM);
};

/// Operational virtualization: selected integer arithmetic is rerouted through
/// a bytecode interpreter. The operator lives in an (XOR-masked) data blob,
/// not in the instruction stream.
struct VirtualizationPass : public llvm::PassInfoMixin<VirtualizationPass> {
  llvm::PreservedAnalyses run(llvm::Module &M, llvm::ModuleAnalysisManager &AM);
};

/// Scrambles global initializers: the real data is stashed in an encrypted
/// side-global and a randomized-order load-time constructor rebuilds it, so
/// the static image no longer exposes the original (.data/.rodata) constants.
struct InitScramblePass : public llvm::PassInfoMixin<InitScramblePass> {
  llvm::PreservedAnalyses run(llvm::Module &M, llvm::ModuleAnalysisManager &AM);
};

} // namespace shadowc

#endif // SHADOWC_INCLUDE_SHADOWC_PASSES_H