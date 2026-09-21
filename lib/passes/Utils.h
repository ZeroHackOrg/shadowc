//===- Utils.h - shared helpers for shadowc passes -----------------------===//
//
// Shared utilities: annotation parsing (shadowc-skip), SSA demotion, seeded
// scrambling and a small statistics facade.
//
//===----------------------------------------------------------------------===//

#ifndef SHADOWC_LIB_PASSES_UTILS_H
#define SHADOWC_LIB_PASSES_UTILS_H

#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/Support/CommandLine.h"
#include <cstdint>
#include <random>
#include <vector>

namespace shadowc {

/// Returns the set of `shadowc` annotation tags declared for \p F through
/// llvm.global.annotations (e.g. "shadowc-skip", "shadowc-nocff").
std::vector<std::string> functionAnnotations(llvm::Function &F);

/// Returns true if \p F may be transformed.
/// A function is skipped when it is a declaration, has available-externally
/// linkage, carries the "shadowc-skip" annotation, or carries a
/// "shadowc-no<tag>" annotation. The tag name is passed without the
/// "shadowc-" prefix (e.g. "cff").
bool shouldProcess(llvm::Function &F, llvm::StringRef Tag);

/// Returns a non-zero static secret derived from the build/run seed and a
/// per-function fold, so two builds (or two functions) permute differently.
std::uint64_t makeSeed(llvm::StringRef FunctionName, std::uint64_t BaseSeed);

/// Small xorshift PRNG with a deterministic seed. Behaves identically across
/// platforms, which keeps integration tests reproducible.
struct ShuffleRng {
  std::uint64_t State;
  explicit ShuffleRng(std::uint64_t Seed) : State(Seed ? Seed : 0x9E3779B97F4A7C15ULL) {}
  std::uint32_t next();
};

/// Demotes registers/PHIs that escape their defining block to stack slots.
/// Required before CFG restructuring so a flattened function stays valid SSA.
/// Runs at most \p MaxIterations sweep passes.
void demoteEscapingValues(llvm::Function &F, unsigned MaxIterations = 16);

/// Returns true if \p BB can be cloned safely (no PHIs, no invokes, and every
/// value referenced by its instructions is defined inside the block or is a
/// constant / global / function argument).
bool isClonable(llvm::BasicBlock &BB);

/// Deep clones \p BB (phi-free, per isClonable). Returns the new block with
/// operands wired to the cloned instructions inside it.
llvm::BasicBlock *cloneBasicBlock(llvm::BasicBlock &BB, const llvm::Twine &Name);

/// Converts a 32-bit unsigned constant to an i32 ConstantInt (helper kept so
/// passes never depend on the SwitchInst case API shape).
llvm::ConstantInt *i32Const(llvm::LLVMContext &Ctx, std::uint32_t V);

/// Global command-line seed (-shadowc-seed). 0 requests a nondeterministic
/// (polymorphic) build; a non-zero value makes every scramble reproducible so
/// CI can assert on output shape.
extern llvm::cl::opt<std::uint32_t> ShadowSeed;

/// A std::mt19937 seeded deterministically from \p BaseSeed.
inline std::mt19937 makeStdRng(std::uint64_t BaseSeed) {
  return std::mt19937(static_cast<std::uint32_t>(BaseSeed ^ 0x9E3779B9ULL));
}

} // namespace shadowc

#endif // SHADOWC_LIB_PASSES_UTILS_H