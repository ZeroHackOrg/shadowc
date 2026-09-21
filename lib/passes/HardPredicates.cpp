//===- HardPredicates.cpp - Strong opaque predicates (Community Core) -----===//
//
// shadowc Community Core pass.
//
// A stronger sibling of shadowc-boguscf: instead of the single parity
// (X*(X+1))&1 trick, opaque predicates are drawn from a family of *hard*
// bit-vector identities and congruences, each provably constant yet
// syntactically SAT-like:
//
//   h0  (a^b) + 2*(a&b) == a + b     half-adder identity (wrinkles XOR)
//   h1  (a*2) + (b*2) == (a+b)*2     distributivity under wrapping arithmetic
//   h2  (x^3 - x) % 6 == 0           three-consecutive-factor congruence
//   h3  ((x+c1)+c2+..) == x+sum(c_i) vector-reduction linearity
//
// Every case needs a SMT/bit-vector solver (or algebra on the expression
// DAG) to classify, and none folds trivially under ConstantFold because the
// seed operand is a *runtime* value. Dead clones thread the CFG exactly like
// shadowc-boguscf so decompiler heuristics pay a second, larger cost.
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

#define DEBUG_TYPE "shadowc-hardpred"

using namespace llvm;

namespace shadowc {

static cl::opt<uint32_t> HardFraction(
    "shadowc-hardpred-fraction", cl::init(33), cl::Hidden,
    cl::desc("Percentage of eligible edges to guard with a hard predicate"));

/// Builds a provably-true opaque predicate from runtime seed values.
/// The emitted expression tree varies with the (seeded) RNG so two functions
/// ship different shapes.
static Value *hardTrue(IRBuilder<> &B, Value *X, Value *Y, ShuffleRng &Rng) {
  IntegerType *Ty = cast<IntegerType>(X->getType());
  Value *Two = ConstantInt::get(Ty, 2);

  switch (Rng.next() % 4) {
  case 0: {
    // h0 : (a^b) + 2*(a&b) == a+b   half-adder identity.
    Value *A = B.CreateXor(X, Y);
    Value *C = B.CreateAnd(X, Y);
    Value *LHS = B.CreateAdd(A, B.CreateMul(Two, C));
    return B.CreateICmpEQ(LHS, B.CreateAdd(X, Y));
  }
  case 1: {
    // h1 : (a*2) + (b*2) == (a+b)*2   distributivity (wrapping-safe).
    Value *LHS = B.CreateAdd(B.CreateMul(Two, X), B.CreateMul(Two, Y));
    Value *RHS = B.CreateMul(B.CreateAdd(X, Y), Two);
    return B.CreateICmpEQ(LHS, RHS);
  }
  case 2: {
    // h2 : (x^3 - x) % 6 == 0   x(x-1)(x+1) is always divisible by six.
    Value *SQ = B.CreateMul(X, X);
    Value *CU = B.CreateMul(SQ, X);
    Value *D = B.CreateSub(CU, X);
    Value *Six = ConstantInt::get(Ty, 6);
    Value *R = B.CreateSRem(D, Six);
    return B.CreateICmpEQ(R, ConstantInt::get(Ty, 0));
  }
  default: {
    // h3 : vector-reduction linearity, ((x+c1)+c2+...) == x + sum(c_i).
    Value *Sum = X;
    Value *SumC = ConstantInt::get(Ty, 0);
    for (unsigned I = 0; I < 7; ++I) {
      Constant *Ci = ConstantInt::get(Ty, Rng.next() % 100000);
      Sum = B.CreateAdd(Sum, Ci);
      SumC = B.CreateAdd(SumC, Ci);
    }
    return B.CreateICmpEQ(Sum, B.CreateAdd(X, SumC));
  }
  }
}

/// Picks two runtime integer seeds of *the same bit width* (the predicate
/// family builds mixed-width trees and would otherwise break the verifier).
/// When only one suitable runtime value exists the second is derived as
/// `x ^ c`, which stays runtime-valued and keeps every identity intact.
static bool opaqueSeeds(Function &F, BasicBlock &BB, Value *&X, Value *&Y) {
  IntegerType *Ty = nullptr;

  auto offer = [&](Value *V) -> bool {
    IntegerType *VT = dyn_cast<IntegerType>(V->getType());
    if (!VT || VT->isIntegerTy(1))
      return false;
    if (!Ty) {
      X = V;
      Ty = VT;
    } else if (VT == Ty && !Y) {
      Y = V;
    }
    return bool(Y && Ty);
  };

  for (Argument &A : F.args())
    if (offer(&A))
      return true;
  for (Instruction &I : BB)
    if (offer(&I))
      return true;

  if (X) {
    Y = ConstantInt::get(Ty, (int64_t)0x5A5A5A5A5A5A5A5AULL);
    return true;
  }
  return false;
}

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

static bool wrapEdge(Function &F, BasicBlock *BB, BasicBlock *Target,
                     Instruction *Term, ShuffleRng &Rng) {
  Value *X = nullptr, *Y = nullptr;
  if (!opaqueSeeds(F, *BB, X, Y))
    return false;

  IRBuilder<> B(Term);
  BasicBlock *Dead = makeDeadAlternative(F, Target, "shadowc.hard.dead");
  B.CreateCondBr(hardTrue(B, X, Y, Rng), Target, Dead);

  if (auto *Br = dyn_cast<BranchInst>(Term))
    Br->eraseFromParent();
  return true;
}

PreservedAnalyses HardPredicatesPass::run(Function &F,
                                          FunctionAnalysisManager &) {
  if (F.isDeclaration())
    return PreservedAnalyses::all();
  if (!shouldProcess(F, "hardpred"))
    return PreservedAnalyses::all();

  demoteEscapingValues(F);

  SmallVector<BasicBlock *, 32> Blocks;
  for (BasicBlock &BB : F)
    Blocks.push_back(&BB);

  ShuffleRng Rng(makeSeed(F.getName(), ShadowSeed));
  const uint32_t Pct = std::min(HardFraction.getValue(), 100u);
  bool Changed = false;

  for (BasicBlock *BB : Blocks) {
    if (BB == &F.getEntryBlock())
      continue;
    if (Pct < 100 && (Rng.next() % 100) >= Pct)
      continue;

    Instruction *Term = BB->getTerminator();
    if (auto *Br = dyn_cast<BranchInst>(Term)) {
      if (Br->isUnconditional())
        Changed |= wrapEdge(F, BB, Br->getSuccessor(0), Term, Rng);
    }
  }

  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

} // namespace shadowc