//===- InstructionSubstitution.cpp - Instruction substitution -------------===//
//
// shadowc Community Core pass.
//
// Rewrites familiar arithmetic/logic instructions with bitwise-equivalent,
// longer formula chains:
//
//   a +  b  ->  (a ^ b) + ((a & b) << 1)      [recursively re-expanded]
//   a -  b  ->  a + (~b) + 1
//   a &  b  ->  (a ^ b) ^ (a | b)
//   a |  b  ->  (a ^ b) + (a & b)
//   a ^  b  ->  (a | b) ^ (a & b)
//
// Each pass application deepens the expansion (controlled by the
// -shadowc-substitution-depth option), so a trivial `x + y` in the source can
// bloom into dozens of nested bitwise operations. Decompiled pseudocode then
// reads like crypto rather than application logic.
//
//===----------------------------------------------------------------------===//

#include "shadowc/Passes.h"

#include "Utils.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Type.h"
#include "llvm/Support/CommandLine.h"

#define DEBUG_TYPE "shadowc-substitution"

using namespace llvm;

namespace shadowc {

static cl::opt<unsigned> SubstDepth(
    "shadowc-substitution-depth", cl::init(2), cl::Hidden,
    cl::desc("Recursion depth of the substitution formulas (0 = no-op)"));

static bool isSubstitutable(unsigned Opcode) {
  return Opcode == Instruction::Add || Opcode == Instruction::Sub ||
         Opcode == Instruction::And || Opcode == Instruction::Or ||
         Opcode == Instruction::Xor;
}

/// Returns an equivalent expression tree for `L <Op> R`, expanding nested
/// substitutions up to \p Depth.
static Value *substitute(BinaryOperator::BinaryOps Op, Value *L, Value *R,
                         IRBuilder<> &B, unsigned Depth) {
  IntegerType *Ty = cast<IntegerType>(L->getType());
  switch (Op) {
  case BinaryOperator::Add: {
    // a + b = (a ^ b) + ((a & b) << 1)
    Value *X = B.CreateXor(L, R);
    Value *A = B.CreateAnd(L, R);
    Value *Shifted =
        B.CreateShl(A, ConstantInt::get(Ty, 1));
    if (Depth == 0)
      return B.CreateAdd(X, Shifted);
    return substitute(Op, X, Shifted, B, Depth - 1);
  }
  case BinaryOperator::Sub: {
    // a - b = a + (~b) + 1
    Value *Not = B.CreateXor(R, ConstantInt::get(
                                   Ty, APInt::getMaxValue(Ty->getBitWidth())));
    Value *Inc = ConstantInt::get(Ty, 1);
    Value *Sum = B.CreateAdd(L, Not);
    if (Depth == 0)
      return B.CreateAdd(Sum, Inc);
    return substitute(BinaryOperator::Add, Sum, Inc, B, Depth - 1);
  }
  case BinaryOperator::And: {
    // a & b = (a ^ b) ^ (a | b)
    Value *X = B.CreateXor(L, R);
    Value *O = B.CreateOr(L, R);
    return B.CreateXor(X, O);
  }
  case BinaryOperator::Or: {
    // a | b = (a ^ b) + (a & b)
    Value *X = B.CreateXor(L, R);
    Value *A = B.CreateAnd(L, R);
    if (Depth == 0)
      return B.CreateAdd(X, A);
    return substitute(BinaryOperator::Add, X, A, B, Depth - 1);
  }
  case BinaryOperator::Xor: {
    // a ^ b = (a | b) ^ (a & b)
    Value *O = B.CreateOr(L, R);
    Value *A = B.CreateAnd(L, R);
    return B.CreateXor(O, A);
  }
  default:
    return nullptr;
  }
}

PreservedAnalyses InstructionSubstitutionPass::run(Function &F,
                                                   FunctionAnalysisManager &) {
  if (F.isDeclaration())
    return PreservedAnalyses::all();
  if (!shouldProcess(F, "substitution"))
    return PreservedAnalyses::all();
  if (SubstDepth == 0)
    return PreservedAnalyses::all();

  // Snapshot rewards before we start splicing instructions.
  SmallVector<BinaryOperator *, 32> Targets;
  for (BasicBlock &BB : F)
    for (Instruction &I : BB)
      if (auto *Bin = dyn_cast<BinaryOperator>(&I))
        if (isSubstitutable(Bin->getOpcode()))
          Targets.push_back(Bin);

  bool Changed = false;
  for (BinaryOperator *Bin : Targets) {
    if (!Bin->getParent())
      continue; // erased already (unlikely; cheap guard)
    IntegerType *Ty = dyn_cast<IntegerType>(Bin->getType());
    if (!Ty || Ty->getBitWidth() < 2)
      continue;

    IRBuilder<> B(Bin);
    Value *L = Bin->getOperand(0);
    Value *R = Bin->getOperand(1);
    if (!isa<IntegerType>(L->getType()) || !isa<IntegerType>(R->getType()))
      continue;
    if (L->getType() != R->getType())
      continue;

    Value *New = substitute(Bin->getOpcode(), L, R, B, SubstDepth.getValue());
    if (!New)
      continue;
    Bin->replaceAllUsesWith(New);
    Bin->eraseFromParent();
    Changed = true;
  }

  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

} // namespace shadowc