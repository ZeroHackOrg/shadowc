//===- OpaquePredicates.cpp - Bogus control flow + opaque predicates ------===//
//
// shadowc Community Core pass.
//
// For straight-line edges of a block, a *provably dead* alternative path is
// threaded into the CFG behind an opaque predicate:
//
//   patch(v, target):
//     %t = opaque predicate (always true, provably hard statically)
//     br i1 %t, label %target, label %shadowc.dead
//   shadowc.dead:                             ; never executed
//     <clone of `target` (""same shape"" so decompilers map it)>
//     <rejoins the surrounding CFG>
//
// Conditional edges are filtered through a small trampoline block that applies
// the same gate to one side. Static tools must map, model and 'solve' paths
// that never run. Clones are drawn from the safe phi-free subset of blocks and
// every dead predecessor contributes `undef` to downstream PHIs, keeping the
// IR verifier-clean.
//
//===----------------------------------------------------------------------===//

#include "shadowc/Passes.h"

#include "Utils.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Type.h"
#include "llvm/Support/CommandLine.h"

#include <algorithm>

#define DEBUG_TYPE "shadowc-boguscf"

using namespace llvm;

namespace shadowc {

static cl::opt<uint32_t> BogusFraction(
    "shadowc-boguscf-fraction", cl::init(35), cl::Hidden,
    cl::desc("Percentage of eligible edges to wrap in opaque predicates"));

// ---------------------------------------------------------------------------
// Opaque predicates
//
//   mt  := (X*X + X) & 1 == 0     always true   (X*(X+1) is always even, even
//                                                under wrapping arithmetic)
//   mf  := (X*X)     & 2 == 2     always false  (X^2 mod 4 is 0 or 1)
//
// Both need polynomial reasoning to classify and are computed against a
// *runtime* value so ConstantFold / value-range provers will not collapse
// them to constants at build time.
// ---------------------------------------------------------------------------

static Value *opaqueTrue(IRBuilder<> &B, Value *X) {
  IntegerType *Ty = cast<IntegerType>(X->getType());
  Value *One = ConstantInt::get(Ty, 1);
  Value *SQ = B.CreateMul(X, X);
  Value *Sum = B.CreateAdd(SQ, X);
  Value *Low = B.CreateAnd(Sum, One);
  return B.CreateICmpEQ(Low, ConstantInt::get(Ty, 0));
}

static Value *opaqueFalse(IRBuilder<> &B, Value *X) {
  IntegerType *Ty = cast<IntegerType>(X->getType());
  Value *Two = ConstantInt::get(Ty, 2);
  Value *SQ = B.CreateMul(X, X);
  Value *Mask = B.CreateAnd(SQ, Two);
  return B.CreateICmpEQ(Mask, Two);
}

/// Picks the runtime value used to drive an opaque predicate inside \p BB.
/// Prefers the function's first integer argument (opaque and dominates every
/// block); falls back to an integer value defined in the block itself.
/// Returns nullptr when no suitable value exists (edge is skipped).
static Value *opaqueSeed(Function &F, BasicBlock &BB) {
  for (Argument &A : F.args())
    if (A.getType()->isIntegerTy())
      return &A;
  for (Instruction &I : BB)
    if (I.getType()->isIntegerTy() && !I.getType()->isIntegerTy(1))
      return &I;
  return nullptr;
}

/// Builds a dead block that shadows \p Target. Prefers a full clone of the
/// target where that is safe; otherwise an empty block that simply chains back
/// into the live flow.
static BasicBlock *makeDeadAlternative(Function &F, BasicBlock *Target,
                                       const Twine &Hint) {
  if (isClonable(*Target)) {
    BasicBlock *Dead = cloneBasicBlock(*Target, Hint);
    if (Dead) {
      for (BasicBlock *Succ : successors(Dead))
        for (PHINode &Phi : Succ->phis())
          Phi.addIncoming(UndefValue::get(Phi.getType()), Dead);
      return Dead;
    }
  }
  BasicBlock *Dead = BasicBlock::Create(F.getContext(), Hint, &F);
  BranchInst::Create(Target, Dead);
  return Dead;
}

/// Rewrites `br %target` so the live path is guarded by an opaque predicate
/// and a dead duplicate threads the CFG alongside it.
static bool wrapUnconditional(Function &F, BasicBlock *BB,
                              BasicBlock *Target, BranchInst *Br) {
  Value *Seed = opaqueSeed(F, *BB);
  if (!Seed)
    return false;

  IRBuilder<> B(Br);
  BasicBlock *Dead = makeDeadAlternative(F, Target, "shadowc.dead");
  B.CreateCondBr(opaqueTrue(B, Seed), Target, Dead);
  Br->eraseFromParent();
  return true;
}

/// For a conditional `br %cond, %T, %F`, filters one side through a trampoline
/// that applies the opaque gate, so decompilers follow a dead shadow of that
/// side. Keeps the original branch condition intact.
static bool wrapConditionalSide(Function &F, BasicBlock *BB, BasicBlock *Target,
                                Instruction *Original, unsigned Side) {
  Value *Seed = opaqueSeed(F, *BB);
  if (!Seed)
    return false;

  BasicBlock *Filter =
      BasicBlock::Create(F.getContext(), "shadowc.gate", &F);
  BasicBlock *Dead = makeDeadAlternative(F, Target, "shadowc.dead");

  IRBuilder<> FB(Filter);
  FB.CreateCondBr(opaqueTrue(FB, Seed), Target, Dead);

  cast<BranchInst>(Original)->setSuccessor(Side, Filter);
  return true;
}

PreservedAnalyses BogusControlFlowPass::run(Function &F,
                                            FunctionAnalysisManager &) {
  if (F.isDeclaration())
    return PreservedAnalyses::all();
  if (!shouldProcess(F, "boguscf"))
    return PreservedAnalyses::all();

  // Safe CFG surgery groundwork: spill escaping SSA values first.
  demoteEscapingValues(F);

  SmallVector<BasicBlock *, 32> Blocks;
  for (BasicBlock &BB : F)
    Blocks.push_back(&BB);

  ShuffleRng Rng(makeSeed(F.getName(), ShadowSeed));
  const uint32_t Pct = std::min(BogusFraction.getValue(), 100u);
  bool Changed = false;

  for (auto It = Blocks.begin(); It != Blocks.end(); ++It) {
    BasicBlock *BB = *It;
    if (BB == &F.getEntryBlock())
      continue;
    if (Pct < 100 && (Rng.next() % 100) >= Pct)
      continue;

    Instruction *Term = BB->getTerminator();
    if (auto *Br = dyn_cast<BranchInst>(Term)) {
      if (Br->isUnconditional()) {
        Changed |= wrapUnconditional(F, BB, Br->getSuccessor(0), Br);
      } else {
        Changed |= wrapConditionalSide(F, BB, Br->getSuccessor(0), Br, 0);
        Changed |= wrapConditionalSide(F, BB, Br->getSuccessor(1), Br, 1);
      }
    }
  }

  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

} // namespace shadowc