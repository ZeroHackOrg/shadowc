//===- Virtualization.cpp - Operational virtualization (Enterprise Vault) ==//
//
// shadowc Enterprise Vault pass.
//
// Selected integer arithmetic is "virtualized": the operator is removed from
// the instruction stream and replayed at runtime by a bytecode interpreter.
//
//   before                         after
//   %z = add i32 %a, %b            %z = call i32 @shadowc.vm.eval32(i32 7, i32 %a, i32 %b)
//
//   ops = bytes stored in a global blob `shadowc.vm.prog` (XOR-masked so the
//   opcodes are not readable in .rodata); the interpreter entry switch-ecodes
//   an opcode byte back and evaluates. The operator of record has moved from
//   the executable stream into *data*, which is the property obfuscation looks
//   for: a lift fails wherever the semantics live in the ciphertext-like blob.
//
// Supported ops: add, sub, mul, and, or, xor - all bit-correct under
// per-width wrapping, so the interpreter trivially preserves semantics.
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
#include "llvm/IR/Metadata.h"
#include "llvm/Support/CommandLine.h"

#include <cstdint>

#define DEBUG_TYPE "shadowc-virt"

using namespace llvm;

namespace shadowc {

enum : uint8_t {
  OpAdd = 1,
  OpSub = 2,
  OpMul = 3,
  OpAnd = 4,
  OpOr = 5,
  OpXor = 6,
};

static cl::opt<uint32_t> VirtFraction(
    "shadowc-virt-fraction", cl::init(30), cl::Hidden,
    cl::desc("Percentage of eligible integer arithmetic opcodes to virtualize"));
static cl::opt<uint32_t> VirtCap(
    "shadowc-virt-cap", cl::init(96), cl::Hidden,
    cl::desc("Hard upper bound of virtualized opcodes per module"));

static uint8_t opcodeOf(const BinaryOperator &I) {
  switch (I.getOpcode()) {
  case Instruction::Add:
    return OpAdd;
  case Instruction::Sub:
    return OpSub;
  case Instruction::Mul:
    return OpMul;
  case Instruction::And:
    return OpAnd;
  case Instruction::Or:
    return OpOr;
  case Instruction::Xor:
    return OpXor;
  default:
    return 0;
  }
}

/// Builds the interpreter body shared by both widths. The interpreter reads
/// the opcode from the blob global, unmasks it and dispatches through a
/// switch whose arms perform the native op on the (already width-correct)
/// operands - a direct-threaded dispatcher in miniature.
static void emitInterpreter(Module &M, GlobalVariable *Prog, uint8_t Mask,
                            IntegerType *Ty, const char *Name) {
  LLVMContext &Ctx = M.getContext();

  FunctionType *FTy = FunctionType::get(Ty, {Type::getInt32Ty(Ctx), Ty, Ty},
                                        false);
  Function *F = Function::Create(FTy, Function::InternalLinkage, Name, M);

  BasicBlock *EntryB = BasicBlock::Create(Ctx, "entry", F);
  BasicBlock *BadB = BasicBlock::Create(Ctx, "badop", F);

  IRBuilder<> B(EntryB);
  Value *Idx = F->getArg(0);
  Value *A = F->getArg(1);
  Value *BV = F->getArg(2);

  Value *Ptr = B.CreateGEP(Prog->getValueType(), Prog,
                           {B.getInt64(0), B.CreateZExt(Idx, B.getInt64Ty())});
  Value *Enc = B.CreateLoad(B.getInt8Ty(), Ptr);
  Value *Dec = B.CreateXor(Enc, B.getInt8(Mask), "op");
  Value *Sel = B.CreateZExt(Dec, B.getInt32Ty());

  SwitchInst *Sw = B.CreateSwitch(Sel, BadB, 6);
  B.SetInsertPoint(BadB);
  B.CreateUnreachable();

  auto addArm = [&](unsigned Op, unsigned CaseValue) {
    BasicBlock *BB = BasicBlock::Create(Ctx, "vm." + Twine(CaseValue), F);
    IRBuilder<> EB(BB);
    Value *V = nullptr;
    switch (Op) {
    case OpAdd:  V = EB.CreateAdd(A, BV); break;
    case OpSub:  V = EB.CreateSub(A, BV); break;
    case OpMul:  V = EB.CreateMul(A, BV); break;
    case OpAnd:  V = EB.CreateAnd(A, BV); break;
    case OpOr:   V = EB.CreateOr(A, BV); break;
    case OpXor:  V = EB.CreateXor(A, BV); break;
    default:     break;
    }
    EB.CreateRet(V);
    Sw->addCase(ConstantInt::get(Ctx, APInt(32, CaseValue)), BB);
  };
  addArm(OpAdd, OpAdd);
  addArm(OpSub, OpSub);
  addArm(OpMul, OpMul);
  addArm(OpAnd, OpAnd);
  addArm(OpOr, OpOr);
  addArm(OpXor, OpXor);
}

PreservedAnalyses VirtualizationPass::run(Module &M, ModuleAnalysisManager &) {
  LLVMContext &Ctx = M.getContext();

  struct Cand {
    BinaryOperator *Inst;
    uint8_t Op;
    uint8_t Width64;
  };
  SmallVector<Cand, 64> Candidates;

  for (Function &F : M) {
    if (F.isDeclaration() || F.getName().starts_with("shadowc."))
      continue;
    if (!shouldProcess(F, "virt"))
      continue;
    for (BasicBlock &BB : F) {
      for (Instruction &I : BB) {
        auto *Bin = dyn_cast<BinaryOperator>(&I);
        if (!Bin)
          continue;
        if (!cast<IntegerType>(Bin->getType())->isIntegerTy(32) &&
            !cast<IntegerType>(Bin->getType())->isIntegerTy(64))
          continue;
        uint8_t Op = opcodeOf(*Bin);
        if (!Op)
          continue;
        Candidates.push_back({Bin, Op, cast<IntegerType>(Bin->getType())->getBitWidth() == 64});
      }
    }
  }
  if (Candidates.empty())
    return PreservedAnalyses::all();

  ShuffleRng Rng(makeSeed("shadowc.vm", ShadowSeed));
  const uint32_t Pct = std::min(VirtFraction.getValue(), 100u);
  const uint32_t Cap = VirtCap.getValue();

  SmallVector<Cand, 64> Picked;
  for (Cand &C : Candidates) {
    if (Pct < 100 && (Rng.next() % 100) >= Pct)
      continue;
    Picked.push_back(C);
    if (Picked.size() >= Cap)
      break;
  }
  if (Picked.empty())
    return PreservedAnalyses::all();

  // Bytecode blob: one masked opcode byte per virtualized instruction.
  uint8_t Mask = static_cast<uint8_t>(1 + (Rng.next() % 255));
  std::vector<uint8_t> Bytes;
  Bytes.reserve(Picked.size());
  for (Cand &C : Picked)
    Bytes.push_back(static_cast<uint8_t>(C.Op ^ Mask));

  auto *Prog = new GlobalVariable(
      M, ArrayType::get(Type::getInt8Ty(Ctx), Bytes.size()), true,
      GlobalValue::PrivateLinkage, ConstantDataArray::get(Ctx, Bytes),
      "shadowc.vm.prog");

  // Emit the interpreters *before* mutating the operand of users.
  emitInterpreter(M, Prog, Mask, Type::getInt32Ty(Ctx), "shadowc.vm.eval32");
  emitInterpreter(M, Prog, Mask, Type::getInt64Ty(Ctx), "shadowc.vm.eval64");

  for (size_t I = 0; I < Picked.size(); ++I) {
    Cand &C = Picked[I];
    IRBuilder<> B(C.Inst);
    Function *VM = C.Width64 ? M.getFunction("shadowc.vm.eval64")
                             : M.getFunction("shadowc.vm.eval32");
    Value *Idx = B.getInt32(I);
    Value *Call = B.CreateCall(VM, {Idx, C.Inst->getOperand(0), C.Inst->getOperand(1)},
                               C.Inst->getName());
    C.Inst->replaceAllUsesWith(Call);
    C.Inst->eraseFromParent();
  }

  NamedMDNode *Tags = M.getOrInsertNamedMetadata("shadowc.instrumented");
  Tags->addOperand(MDNode::get(
      Ctx, MDString::get(Ctx, "shadowc.vm:" + Twine(Picked.size()).str())));
  return PreservedAnalyses::none();
}

} // namespace shadowc