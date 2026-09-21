//===- AntiDebug.cpp - Anti-debug trap injection (Enterprise Vault) -------===//
//
// shadowc Enterprise Vault pass.
//
// Inserts `shadowc.trap` at the entry of the process entry point:
//
//   %r = call i32 @ptrace(i32 0 /*PTRACE_TRACEME*/, i32 0, ptr null, ptr null)
//   %hijacked = icmp eq i32 %r, -1
//   br i1 %hijacked, label %jail, label %ok
//   jail:  call void @exit(i32 173)     ; we are being traced -> fail closed
//   ok:    ret void
//
// PTRACE_TRACEME "tells the kernel that the calling process shall be traced
// by its parent". Under a debugger/strace/tracer the tracer already owns us,
// TRACEME returns -1 and the payload bails out with exit code 173 (no output,
// no normal completion) so the analyst is cut off at the earliest possible
// point. A normal, untraced run returns 0 and continues identically.
//
// The pass is compiled into the plugin but the shadowc tooling layer refuses
// to schedule it without a granted Enterprise Vault token (src/gatekeeper.py).
//
//===----------------------------------------------------------------------===//

#include "shadowc/Passes.h"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Metadata.h"

#define DEBUG_TYPE "shadowc-trap"

using namespace llvm;

namespace shadowc {

static constexpr int kTrapExitCode = 173;

/// Declares (lazily, once) the C library symbols the guard needs.
static Function *declarePtrace(Module &M) {
  LLVMContext &Ctx = M.getContext();
  FunctionType *PTy = FunctionType::get(
      Type::getInt32Ty(Ctx),
      {Type::getInt32Ty(Ctx), Type::getInt32Ty(Ctx), PointerType::get(Ctx, 0),
       PointerType::get(Ctx, 0)},
      false);
  Function *F = M.getFunction("ptrace");
  if (!F)
    F = Function::Create(PTy, Function::ExternalLinkage, "ptrace", &M);
  return F;
}

static Function *declareExit(Module &M) {
  LLVMContext &Ctx = M.getContext();
  FunctionType *ETy =
      FunctionType::get(Type::getVoidTy(Ctx), {Type::getInt32Ty(Ctx)}, false);
  Function *F = M.getFunction("exit");
  if (!F)
    F = Function::Create(ETy, Function::ExternalLinkage, "exit", &M);
  return F;
}

/// Builds (once) `shadowc.trap` - the ptrace-traceme guard function.
static Function *makeTrapFunction(Module &M) {
  if (Function *Existing = M.getFunction("shadowc.trap"))
    return Existing;

  LLVMContext &Ctx = M.getContext();
  FunctionType *FTy = FunctionType::get(Type::getVoidTy(Ctx), false);
  Function *Trap =
      Function::Create(FTy, Function::InternalLinkage, "shadowc.trap", &M);

  BasicBlock *EntryB = BasicBlock::Create(Ctx, "entry", Trap);
  BasicBlock *JailB = BasicBlock::Create(Ctx, "jail", Trap);
  BasicBlock *OkB = BasicBlock::Create(Ctx, "ok", Trap);

  IRBuilder<> B(EntryB);
  Function *Ptrace = declarePtrace(M);
  Value *R = B.CreateCall(Ptrace,
                          {B.getInt32(0), B.getInt32(0),
                           ConstantPointerNull::get(PointerType::get(Ctx, 0)),
                           ConstantPointerNull::get(PointerType::get(Ctx, 0))});
  Value *Hijacked = B.CreateICmpEQ(R, B.getInt32(-1), "hijacked");
  B.CreateCondBr(Hijacked, JailB, OkB);

  B.SetInsertPoint(JailB);
  B.CreateCall(declareExit(M), B.getInt32(kTrapExitCode));
  B.CreateUnreachable();

  B.SetInsertPoint(OkB);
  B.CreateRetVoid();
  return Trap;
}

/// Plugs the guard into the process entry point. Prefers `main`; otherwise
/// the first non-shadowc definition with a body (best effort).
static Function *findEntry(Module &M) {
  if (Function *Main = M.getFunction("main"))
    return Main;
  for (Function &F : M)
    if (!F.isDeclaration() && !F.getName().starts_with("shadowc."))
      return &F;
  return nullptr;
}

PreservedAnalyses AntiDebugPass::run(Module &M, ModuleAnalysisManager &) {
  Function *Entry = findEntry(M);
  if (!Entry)
    return PreservedAnalyses::all();

  Function *Trap = makeTrapFunction(M);
  BasicBlock &Head = Entry->getEntryBlock();
  IRBuilder<> B(&*Head.getFirstInsertionPt());
  B.CreateCall(Trap);

  NamedMDNode *Tags = M.getOrInsertNamedMetadata("shadowc.instrumented");
  Tags->addOperand(MDNode::get(
      M.getContext(), MDString::get(M.getContext(), "shadowc.trap")));
  return PreservedAnalyses::none();
}

} // namespace shadowc