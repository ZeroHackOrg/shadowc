//===- StringEncryption.cpp - String virtualization (Enterprise Vault) -----===//
//
// shadowc Enterprise Vault pass.
//
// Every string global is XOR-encrypted at build time with a per-string key,
// and a load-time unpacker (an LLVM-level global constructor) is emitted that
// decrypts the bytes into writable memory just before `main` runs. The static
// image therefore contains ciphertext only:
//
//   $ strings binary.bin            ->  no readable literals
//   $ hexdump -C .rodata            ->  keystream-shaped bytes
//
// This is the community-transparency compromise: the pass ships as source but
// the shadowc tooling layer refuses to run it without a valid Enterprise
// licence token (see src/gatekeeper.py).
//
//===----------------------------------------------------------------------===//

#include "shadowc/Passes.h"

#include "Utils.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"

#include <cstdint>
#include <string>
#include <vector>

#define DEBUG_TYPE "shadowc-string"

using namespace llvm;

namespace shadowc {

static cl::opt<uint32_t> StringKeySpace(
    "shadowc-string-key-space", cl::init(251), cl::Hidden,
    cl::desc("XOR key space used by string virtualization (key = 1..N)"));

static cl::opt<std::string> StringSalt(
    "shadowc-string-salt", cl::init("shadowc"), cl::Hidden,
    cl::desc("Per-shipment salt mixed into the XOR keystream, so two vendors "
             "with the same --seed still ship different ciphertext."));

/// FNV-1a over \p S - a fast, deterministic fold used to mix the salt and the
/// module into the sealed keystream.
static uint64_t fnv1a(StringRef S, uint64_t H = 0xCBF29CE484222325ULL) {
  for (char C : S)
    H = (H ^ static_cast<uint8_t>(C)) * 0x100000001B3ULL;
  return H;
}

struct StringEntry {
  GlobalVariable *GV;
  uint64_t Length;
  uint8_t Key;
};

/// Emits the load-time XOR unpacker for a single encrypted string global.
Function *makeDecryptor(Module &M, GlobalVariable *GV, uint8_t Key,
                        uint64_t Length);

PreservedAnalyses StringEncryptionPass::run(Module &M, ModuleAnalysisManager &) {
  LLVMContext &Ctx = M.getContext();

  SmallVector<StringEntry, 32> Strings;
  unsigned Index = 0;
  for (GlobalVariable &GV : M.globals()) {
    if (!GV.hasInitializer() || GV.isThreadLocal())
      continue;

    // Skip globals the author marked "do not encrypt" via metadata
    // (attach a !shadowc.noencrypt node, or scope it from the tooling layer).
    if (GV.getMetadata("shadowc.noencrypt"))
      continue;

    ConstantDataArray *Arr = dyn_cast<ConstantDataArray>(GV.getInitializer());
    if (!Arr || Arr->getElementType() != Type::getInt8Ty(Ctx))
      continue;
    if (Arr->getNumElements() == 0)
      continue;

    uint64_t N = Arr->getNumElements();
    // Entropy = seed + per-module salt + per-string index. A fixed --seed and
    // salt reproduce byte-identical ciphertext (reproducible shipments);
    // changing the salt rotates every key. The module identifier is *not*
    // mixed in: temporary file paths vary between runs and would break the
    // determinism contract.
    ShuffleRng Rng(makeSeed(std::to_string(Index++),
                            ShadowSeed ^ fnv1a(StringSalt.getValue())));
    uint8_t Key =
        static_cast<uint8_t>((Rng.next() % (StringKeySpace - 1)) + 1);
    if (Key == 0)
      Key = 1;

    Strings.push_back({&GV, N, Key});
  }

  if (Strings.empty())
    return PreservedAnalyses::all();

  for (StringEntry &SE : Strings) {
    ConstantDataArray *Arr = cast<ConstantDataArray>(SE.GV->getInitializer());
    std::vector<uint8_t> Encrypted;
    Encrypted.reserve(SE.Length);
    for (uint64_t I = 0; I < SE.Length; ++I)
      Encrypted.push_back(
          static_cast<uint8_t>(Arr->getElementAsInteger(I) ^ SE.Key));

    Constant *NewInit = ConstantDataArray::get(Ctx, Encrypted);
    SE.GV->setInitializer(NewInit);
    SE.GV->setConstant(false); // encrypted bytes must live in writable memory

    Function *Decryptor = makeDecryptor(M, SE.GV, SE.Key, SE.Length);
    appendToGlobalCtors(M, Decryptor, 65535);
  }

  return PreservedAnalyses::none();
}

Function *makeDecryptor(Module &M, GlobalVariable *GV, uint8_t Key,
                        uint64_t Length) {
  LLVMContext &Ctx = M.getContext();

  FunctionType *FTy = FunctionType::get(Type::getVoidTy(Ctx), false);
  std::string Name = "shadowc.xor." + GV->getName().str();
  Function *F = Function::Create(FTy, Function::InternalLinkage, Name, M);

  BasicBlock *EntryB = BasicBlock::Create(Ctx, "entry", F);
  BasicBlock *LoopB = BasicBlock::Create(Ctx, "loop", F);
  BasicBlock *ExitB = BasicBlock::Create(Ctx, "exit", F);

  IRBuilder<> B(EntryB);
  B.CreateBr(LoopB);

  B.SetInsertPoint(LoopB);
  PHINode *Idx = B.CreatePHI(B.getInt64Ty(), 2, "i");
  Idx->addIncoming(B.getInt64(0), EntryB);

  Value *Ptr = B.CreateGEP(GV->getValueType(), GV, {B.getInt64(0), Idx});
  Value *Byte = B.CreateLoad(B.getInt8Ty(), Ptr);
  Value *Dec = B.CreateXor(Byte, B.getInt8(Key));
  B.CreateStore(Dec, Ptr);

  Value *Next = B.CreateAdd(Idx, B.getInt64(1));
  Idx->addIncoming(Next, LoopB);
  Value *Continue = B.CreateICmpULT(Next, B.getInt64(Length));
  B.CreateCondBr(Continue, LoopB, ExitB);

  B.SetInsertPoint(ExitB);
  B.CreateRetVoid();
  return F;
}

} // namespace shadowc