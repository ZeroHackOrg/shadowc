//===- ControlFlowFlattening.cpp - Control Flow Flattening ----------------===//
//
// shadowc Community Core pass.
//
// Collapses the structured CFG of a function (loops, if/else trees, switch
// chains) into a single finite state machine:
//
//   entry:
//     %sw  = alloca i32
//     store i32 <start>, %sw
//     br %loop.entry
//   loop.entry:
//     %v = load i32, %sw
//     switch i32 %v, default -> loop.end, <case const> -> case_i, ...
//   case_i:
//     <original body instructions>
//     %next = select|<constant>
//     store i32 %next, %sw
//     br loop.end
//   loop.end:
//     br loop.entry
//
// The dispatcher switch is the only branching point; every original block
// becomes a straight-line "state" that fans back into the single loop. This
// destroys the linear control-flow view that decompilers replay and forces a
// state-machine reconstruction instead.
//
//===----------------------------------------------------------------------===//
//
// Correctness notes:
//   * Switch instructions are lowered to branches first (so every terminator
//     has <= 2 successors).
//   * Registers and PHIs that escape their defining block are demoted to
//     stack slots first, so the flattened function stays valid SSA.
//
//===----------------------------------------------------------------------===//

#include "shadowc/Passes.h"

#include "Utils.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Type.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Transforms/Utils/LowerSwitch.h"

#include <algorithm>
#include <cstdint>
#include <numeric>
#include <random>
#include <unordered_map>

#define DEBUG_TYPE "shadowc-cff"

using namespace llvm;

namespace shadowc {

static cl::opt<uint32_t> FlattenPercent(
    "shadowc-cff-fraction", cl::init(100), cl::Hidden,
    cl::desc("Percentage of eligible functions to flatten (1-100)"));

/// Implements the flattening transform. Exposed here (not in the public
/// header) because it is only ever invoked through the pass runner.
bool flatten(llvm::Function &F, uint64_t BaseSeed);

PreservedAnalyses ControlFlowFlatteningPass::run(Function &F,
                                                 FunctionAnalysisManager &AM) {
  if (F.isDeclaration())
    return PreservedAnalyses::all();
  if (!shouldProcess(F, "cff"))
    return PreservedAnalyses::all();

  // Community "skip annotation" escape hatch that the public repo documents:
  //   __attribute__((annotate("shadowc-skip")))  -> handled by shouldProcess.

  const uint32_t Pct = std::min(FlattenPercent.getValue(), 100u);
  ShuffleRng Rng(makeSeed(F.getName(), ShadowSeed));
  if (Pct < 100 && (Rng.next() % 100) >= Pct)
    return PreservedAnalyses::all();

  // Lower switch instructions before flattening so every block has <= 2
  // successors. LowerSwitchPass is a new-PM pass; run it through the AM.
  PreservedAnalyses Lower = LowerSwitchPass().run(F, AM);

  bool Changed = flatten(F, ShadowSeed);

  // LowerSwitchPass already reported what it preserved; our transform makes
  // everything stale.
  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

bool flatten(Function &F, uint64_t BaseSeed) {
  LLVMContext &Ctx = F.getContext();

  SmallVector<BasicBlock *, 32> OrigBB;
  for (BasicBlock &BB : F) {
    if (isa<InvokeInst>(BB.getTerminator()))
      return false; // exception-control regions are left untouched
    OrigBB.push_back(&BB);
  }
  if (OrigBB.size() <= 1)
    return false;

  BasicBlock *Entry = &F.getEntryBlock();
  if (Entry->getTerminator()->getNumSuccessors() == 0)
    return false; // entry is a terminator-only head; nothing to flatten

  // Every terminator must be simple (<= 2 successors) after lowering.
  for (BasicBlock *BB : OrigBB)
    if (BB->getTerminator()->getNumSuccessors() > 2)
      return false;

  // Demote PHIs and escaping registers first so that the CFG surgery below
  // can never invalidate SSA (phi-pred edges and cross-block dominance).
  demoteEscapingValues(F);

  // If the entry block is itself a branch/decision point, peel its terminator
  // into a fresh leader block so the entry degenerates to a plain init head.
  if (Entry->getTerminator()->getNumSuccessors() > 1) {
    Entry->splitBasicBlock(Entry->getTerminator(), "shadowc.first");
    OrigBB.clear();
    for (BasicBlock &BB : F)
      OrigBB.push_back(&BB);
  }

  // The flattened machine must start at the block the original entry reached.
  // The entry now has exactly one successor (either its original single target
  // or the freshly peeled "shadowc.first" leader); layout order and this target
  // are unrelated, so relying on Cases.front() here is a latent miscompile
  // whenever the first body block in module layout is NOT the entry successor.
  if (Entry->getTerminator()->getNumSuccessors() != 1)
    return false;
  BasicBlock *InitTarget = Entry->getTerminator()->getSuccessor(0);

  SmallVector<BasicBlock *, 32> Cases;
  for (BasicBlock *BB : OrigBB)
    if (BB != Entry)
      Cases.push_back(BB);
  if (Cases.empty())
    return false;

  // -----------------------------------------------------------------------
  // Case numbering. Distinct constants 0..N-1 in shuffled order. Deterministic
  // when -shadowc-seed is non-zero (reproducible CI), nondeterministic when
  // seeded by the operating system (the "polymorphic" build property).
  // -----------------------------------------------------------------------
  const size_t N = Cases.size();
  std::vector<uint32_t> CaseNum(N);
  {
    std::vector<size_t> Order(N);
    std::iota(Order.begin(), Order.end(), 0);
    if (BaseSeed != 0) {
      // Use std::shuffle with our deterministic engine when seeded.
      std::shuffle(Order.begin(), Order.end(), makeStdRng(BaseSeed));
    } else {
      std::mt19937 Gen(std::random_device{}());
      std::shuffle(Order.begin(), Order.end(), Gen);
    }
    for (size_t I = 0; I < N; ++I)
      CaseNum[Order[I]] = static_cast<uint32_t>(I);
  }
  std::unordered_map<BasicBlock *, ConstantInt *> CaseConst;
  for (size_t I = 0; I < N; ++I)
    CaseConst[Cases[I]] = i32Const(Ctx, CaseNum[I]);

  // -----------------------------------------------------------------------
  // Scaffolding
  // -----------------------------------------------------------------------
  IRBuilder<> EB(Entry);
  EB.SetInsertPoint(Entry->getFirstInsertionPt());
  Value *SwitchVar = EB.CreateAlloca(EB.getInt32Ty(), nullptr, "shadowc.sw");

  BasicBlock *LoopEntry = BasicBlock::Create(Ctx, "shadowc.loopentry", &F);
  BasicBlock *LoopEnd = BasicBlock::Create(Ctx, "shadowc.loopend", &F);
  BasicBlock *SwDefault = BasicBlock::Create(Ctx, "shadowc.default", &F);

  IRBuilder<> LB(LoopEntry);
  Value *SwitchCond = LB.CreateLoad(EB.getInt32Ty(), SwitchVar, "shadowc.tv");
  SwitchInst *Switch = LB.CreateSwitch(SwitchCond, SwDefault);
  BranchInst::Create(LoopEnd, SwDefault);
  BranchInst::Create(LoopEntry, LoopEnd);

  // initial state = the block the unmodified entry actually entered
  EB.CreateStore(CaseConst[InitTarget], SwitchVar);

  for (size_t I = 0; I < N; ++I) {
    BasicBlock *Cas = Cases[I];
    Cas->moveBefore(LoopEnd);
    Switch->addCase(CaseConst[Cas], Cas);
  }

  // Entry fans into the dispatcher (replacing both the peeled branch and any
  // original terminator).
  Entry->getTerminator()->eraseFromParent();
  BranchInst::Create(LoopEntry, Entry);

  // -----------------------------------------------------------------------
  // Rewire every state's exit to "store next state + jump back to loop".
  // -----------------------------------------------------------------------
  ConstantInt *Fallback = CaseConst[Cases.front()];
  for (size_t I = 0; I < N; ++I) {
    BasicBlock *Cas = Cases[I];
    Instruction *Term = Cas->getTerminator();
    const unsigned S = Term->getNumSuccessors();
    if (S == 0)
      continue; // ret / unreachable: exits the loop, as before

    auto NextConst = [&](BasicBlock *Succ) -> ConstantInt * {
      auto It = CaseConst.find(Succ);
      return (It != CaseConst.end()) ? It->second : Fallback;
    };

    if (S == 1) {
      ConstantInt *Next = NextConst(Term->getSuccessor(0));
      Term->eraseFromParent();
      IRBuilder<> CB(Cas);
      CB.CreateStore(Next, SwitchVar);
      CB.CreateBr(LoopEnd);
    } else if (S == 2) {
      Value *Cond = cast<BranchInst>(Term)->getCondition();
      ConstantInt *NextT = NextConst(Term->getSuccessor(0));
      ConstantInt *NextF = NextConst(Term->getSuccessor(1));
      Term->eraseFromParent();
      IRBuilder<> CB(Cas);
      Value *Sel = CB.CreateSelect(Cond, NextT, NextF);
      CB.CreateStore(Sel, SwitchVar);
      CB.CreateBr(LoopEnd);
    }
  }

  LLVM_DEBUG(dbgs() << "[shadowc-cff] flattened '" << F.getName()
                    << "' (" << N << " states)\n");
  return true;
}

} // namespace shadowc