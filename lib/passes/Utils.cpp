//===- Utils.cpp - shared helpers for shadowc passes ---------------------===//

#include "Utils.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/Support/Debug.h"

#include <algorithm>
#include <cctype>

#define DEBUG_TYPE "shadowc-utils"

using namespace llvm;

namespace shadowc {

cl::opt<uint32_t> ShadowSeed(
    "shadowc-seed", cl::init(0), cl::Hidden,
    cl::desc("Seed for shadowc scramblers. 0 = nondeterministic (one unique "
             "binary per build); non-zero = reproducible output for testing."));

static std::string lower(std::string S) {
  std::transform(S.begin(), S.end(), S.begin(),
                 [](unsigned char C) { return std::tolower(C); });
  return S;
}

std::vector<std::string> functionAnnotations(Function &F) {
  std::vector<std::string> Tags;
  Module *M = F.getParent();
  if (!M)
    return Tags;

  GlobalVariable *Glob = M->getGlobalVariable("llvm.global.annotations");
  if (!Glob || !Glob->hasInitializer())
    return Tags;

  Constant *Init = Glob->getInitializer();
  if (!isa<ConstantArray>(Init))
    return Tags;
  ConstantArray *Arr = cast<ConstantArray>(Init);
  for (Value *Op : Arr->operands()) {
    ConstantStruct *Struct = dyn_cast<ConstantStruct>(Op);
    if (!Struct || Struct->getNumOperands() < 2)
      continue;
    // Clang points operand 0 at the annotated function; in current LLVM it
    // arrives as the function itself, in older IR as a single dereferencing
    // (bit)cast ConstantExpr. Unwrap any constant-expression chain.
    Value *Target = Struct->getOperand(0);
    while (auto *Cast = dyn_cast<ConstantExpr>(Target))
      Target = Cast->getOperand(0);
    if (Target != &F)
      continue;

    // Operand 1 carries the "shadowc-skip" string literal. It is either a
    // global of array type, or a GEP ConstantExpr naming that global.
    Value *NoteVal = Struct->getOperand(1);
    while (auto *Cast = dyn_cast<ConstantExpr>(NoteVal))
      NoteVal = Cast->getOperand(0);
    GlobalVariable *Tag = dyn_cast<GlobalVariable>(NoteVal);
    if (!Tag || !Tag->hasInitializer())
      continue;
    ConstantDataSequential *Data =
        dyn_cast<ConstantDataSequential>(Tag->getInitializer());
    if (!Data || !Data->isString())
      continue;
    Tags.push_back(lower(Data->getAsString().str()));
  }
  return Tags;
}

bool shouldProcess(Function &F, StringRef Tag) {
  if (F.isDeclaration() || F.hasAvailableExternallyLinkage())
    return false;

  std::vector<std::string> Tags = functionAnnotations(F);
  for (const std::string &T : Tags) {
    if (T.find("shadowc-skip") != std::string::npos)
      return false;
    if (T.find("shadowc-no" + std::string(Tag)) != std::string::npos)
      return false;
  }
  return true;
}

uint64_t makeSeed(StringRef FunctionName, uint64_t BaseSeed) {
  uint64_t H = BaseSeed;
  for (char C : FunctionName)
    H = (H ^ 0x9E3779B9ULL) + (C * 2654435761ULL);
  return H;
}

uint32_t ShuffleRng::next() {
  State ^= State << 13;
  State ^= State >> 7;
  State ^= State << 17;
  return static_cast<uint32_t>(State);
}

void demoteEscapingValues(Function &F, unsigned MaxIterations) {
  bool Changed = false;
  do {
    Changed = false;
    SmallVector<Instruction *, 32> Regs;
    SmallVector<PHINode *, 32> Phis;

    for (BasicBlock &BB : F) {
      for (Instruction &I : BB) {
        if (auto *Phi = dyn_cast<PHINode>(&I)) {
          Phis.push_back(Phi);
          continue;
        }
        if (I.getType()->isVoidTy())
          continue;
        if (isa<AllocaInst>(I) && I.getParent() == &F.getEntryBlock())
          continue; // already in memory
        if (I.isUsedOutsideOfBlock(&BB)) {
          Regs.push_back(&I);
          continue;
        }
        for (User *U : I.users()) {
          if (isa<PHINode>(U)) {
            Regs.push_back(&I);
            break;
          }
        }
      }
    }

    for (Instruction *I : Regs) {
      DemoteRegToStack(*I);
      Changed = true;
    }
    for (PHINode *P : Phis) {
      DemotePHIToStack(P);
      Changed = true;
    }
  } while (Changed && --MaxIterations > 0);
}

bool isClonable(BasicBlock &BB) {
  Instruction *Term = BB.getTerminator();
  if (!Term || isa<InvokeInst>(Term) || isa<PHINode>(&*BB.begin()))
    return false;

  Function *F = BB.getParent();
  for (Instruction &I : BB) {
    for (Value *Op : I.operands()) {
      if (auto *Inst = dyn_cast<Instruction>(Op)) {
        if (Inst->getParent() != &BB)
          return false; // references a value defined elsewhere
      } else if (auto *Arg = dyn_cast<Argument>(Op)) {
        (void)Arg; // function arguments are fine (they dominate everything)
      } else if (isa<Constant>(Op) || isa<MetadataAsValue>(Op)) {
        // fine
      } else {
        return false;
      }
    }
  }
  return true;
}

BasicBlock *cloneBasicBlock(BasicBlock &BB, const Twine &Name) {
  if (!isClonable(BB))
    return nullptr;

  ValueToValueMapTy VMap;
  BasicBlock *Clone = CloneBasicBlock(&BB, VMap, Name, BB.getParent());

  // Remap internal instruction references to their clones.
  for (Instruction &I : *Clone) {
    for (unsigned Idx = 0; Idx < I.getNumOperands(); ++Idx) {
      Value *Op = I.getOperand(Idx);
      if (auto *InstOp = dyn_cast<Instruction>(Op)) {
        if (Value *Mapped = VMap.lookup(InstOp))
          I.setOperand(Idx, Mapped);
      }
    }
  }
  return Clone;
}

ConstantInt *i32Const(LLVMContext &Ctx, uint32_t V) {
  return ConstantInt::get(Type::getInt32Ty(Ctx), V);
}

} // namespace shadowc