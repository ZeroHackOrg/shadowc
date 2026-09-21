//===- InitScramble.cpp - Global initializer scrambling (Enterprise Vault) ==//
//
// shadowc Enterprise Vault pass.
//
// The static image must not expose plaintext data constants. Instead of
// keeping `@lut = [1,2,3,4]` in .data, the real initializer is rebuilt at
// load time:
//
//   @lut      = global [4 x i32] zeroinitializer        ; writable, empty here
//   @shadowc.init.lut = [4 x i32] [c1, c2, c3, c4]      ; data XOR-masked
//
//   shadowc.ctor.lut():  for i in 0..3: lut[i] = ci ^ mask_i; (order set by
//                        a *randomized* llvm.global_ctors priority, so two
//                        seeded builds order the ctors differently)
//
// The initializer bytes remain decodable only by combining both globals with
// the (seed-derived, per-module-keyed) masks - the same honesty contract as
// shadowc-string. A randomized ctor order further defeats analyses that
// assume "first constructor wins".
//
// Compiled into the plugin; the shadowc tooling layer refuses to schedule it
// without a granted Enterprise Vault token (src/gatekeeper.py).
//
//===----------------------------------------------------------------------===//

#include "shadowc/Passes.h"

#include "Utils.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"
#include "llvm/IR/Metadata.h"

#include <cstdint>

#define DEBUG_TYPE "shadowc-init"

using namespace llvm;

namespace shadowc {

static cl::opt<uint32_t> InitFraction(
    "shadowc-init-fraction", cl::init(50), cl::Hidden,
    cl::desc("Percentage of eligible integer globals to scramble"));
static cl::opt<uint32_t> InitCap(
    "shadowc-init-cap", cl::init(48), cl::Hidden,
    cl::desc("Hard upper bound of scrambled globals per module"));

static bool eligible(const GlobalVariable &GV) {
  if (!GV.hasInitializer() || GV.isThreadLocal() || GV.hasAppendingLinkage())
    return false;
  if (GV.getName().starts_with("shadowc.") || GV.getName().starts_with("llvm."))
    return false;
  const Constant *Init = GV.getInitializer();
  // ConstantInt scalars, or integer arrays (i16/i32/i64); strings (i8) are
  // owned by shadowc-string and must not be touched here.
  if (auto *CI = dyn_cast<ConstantInt>(Init)) {
    return CI->getBitWidth() >= 16;
  }
  if (auto *Arr = dyn_cast<ConstantDataArray>(Init)) {
    IntegerType *ET = dyn_cast<IntegerType>(Arr->getElementType());
    return ET && ET->getBitWidth() >= 16 && Arr->getNumElements() > 0;
  }
  return false;
}

PreservedAnalyses InitScramblePass::run(Module &M, ModuleAnalysisManager &) {
  LLVMContext &Ctx = M.getContext();

  SmallVector<GlobalVariable *, 32> Targets;
  for (GlobalVariable &GV : M.globals())
    if (eligible(GV))
      Targets.push_back(&GV);
  if (Targets.empty())
    return PreservedAnalyses::all();

  ShuffleRng Rng(makeSeed("shadowc.init", ShadowSeed));
  const uint32_t Pct = std::min(InitFraction.getValue(), 100u);
  const uint32_t Cap = InitCap.getValue();

  SmallVector<GlobalVariable *, 32> Picked;
  for (GlobalVariable *GV : Targets) {
    if (Pct < 100 && (Rng.next() % 100) >= Pct)
      continue;
    Picked.push_back(GV);
    if (Picked.size() >= Cap)
      break;
  }
  if (Picked.empty())
    return PreservedAnalyses::all();

  for (GlobalVariable *GV : Picked) {
    SmallVector<uint64_t, 8> Values;
    uint64_t Width = 64;
    Constant *Init = GV->getInitializer();
    if (auto *CI = dyn_cast<ConstantInt>(Init)) {
      Width = CI->getBitWidth();
      Values.push_back(CI->getZExtValue());
    } else if (auto *Arr = dyn_cast<ConstantDataArray>(Init)) {
      Width = Arr->getElementType()->getIntegerBitWidth();
      for (uint64_t I = 0; I < Arr->getNumElements(); ++I)
        Values.push_back(Arr->getElementAsInteger(I));
    }
    if (Values.empty())
      continue;

    IntegerType *Ty = IntegerType::get(Ctx, Width);
    APInt Maskor(Width, Rng.next());
    Maskor = Maskor.zextOrTrunc(Width);

    std::vector<Constant *> Masked;
    Masked.reserve(Values.size());
    for (uint64_t V : Values) {
      APInt Cipher(Width, V);
      Cipher ^= Maskor;
      Masked.push_back(ConstantInt::get(Ty, Cipher));
    }

    // Side global holding the masked payload; kept opaque-by-name.
    ArrayType *ATy = ArrayType::get(Ty, Masked.size());
    auto *Payload = new GlobalVariable(
        M, ATy, true, GlobalValue::PrivateLinkage,
        ConstantArray::get(ATy, Masked), "shadowc.init." + GV->getName());

    // Empty the live global. Scalars zero as a plain constant; aggregates as
    // an aggregate zero (never mix the two).
    GV->setInitializer(Constant::getNullValue(GV->getValueType()));
    GV->setConstant(false);

    // Load-time rebuild ctor.
    std::string CtorName = "shadowc.ctor." + GV->getName().str();
    FunctionType *FTy = FunctionType::get(Type::getVoidTy(Ctx), false);
    Function *Ctor =
        Function::Create(FTy, Function::InternalLinkage, CtorName, M);

    BasicBlock *EntryB = BasicBlock::Create(Ctx, "entry", Ctor);
    BasicBlock *LoopB = BasicBlock::Create(Ctx, "loop", Ctor);
    BasicBlock *ExitB = BasicBlock::Create(Ctx, "exit", Ctor);

    IRBuilder<> B(EntryB);
    B.CreateBr(LoopB);

    B.SetInsertPoint(LoopB);
    PHINode *Idx = B.CreatePHI(B.getInt64Ty(), 2, "i");
    Idx->addIncoming(B.getInt64(0), EntryB);

    Value *Dst, *Src;
    if (isa<ConstantDataArray>(Init)) {
      // Array payloads are addressed as base + [0][i].
      Dst = B.CreateGEP(GV->getValueType(), GV, {B.getInt64(0), Idx});
      Src = B.CreateGEP(Payload->getValueType(), Payload,
                        {B.getInt64(0), Idx});
    } else {
      // Scalar globals are already the element: base + [i] is the whole
      // (single-element) slot, which keeps the index space uniform.
      Dst = B.CreateGEP(GV->getValueType(), GV, {Idx});
      Src = B.CreateGEP(Payload->getValueType(), Payload, {Idx});
    }
    Value *Cipher = B.CreateLoad(Ty, Src);
    Value *Plain =
        B.CreateXor(Cipher, ConstantInt::get(Ctx, APInt(Width, Maskor.getZExtValue())));
    B.CreateStore(Plain, Dst);

    Value *Next = B.CreateAdd(Idx, B.getInt64(1));
    Idx->addIncoming(Next, LoopB);
    B.CreateCondBr(B.CreateICmpULT(Next, B.getInt64(Masked.size())), LoopB, ExitB);

    B.SetInsertPoint(ExitB);
    B.CreateRetVoid();

    // Randomized ctor priority - the *relative* order of the rebuild ctors
    // changes per (seed, per-global key), so "which init ran first" is data.
    // Everyone stays well below user ctors (65535).
    uint32_t Prio = 1 + (Rng.next() % 900);
    appendToGlobalCtors(M, Ctor, Prio);
  }

  NamedMDNode *Tags = M.getOrInsertNamedMetadata("shadowc.instrumented");
  Tags->addOperand(MDNode::get(
      Ctx, MDString::get(Ctx, "shadowc.init:" + Twine(Picked.size()).str())));
  return PreservedAnalyses::none();
}

} // namespace shadowc