//===- PassRegistry.cpp - shadowc pass plugin entry point -----------------===//
//
// Registers the shadowc passes with the LLVM new pass manager. The pass list
// is intentionally small and explicit:
//
//   opt -load-pass-plugin=libShadowCPasses.so \
//       -passes="function(shadowc-cff,shadowc-boguscf,shadowc-substitution)"
//
// Community Core:  shadowc-cff, shadowc-boguscf, shadowc-substitution,
//                  shadowc-hardpred
// Enterprise Vault: shadowc-string, shadowc-virt, shadowc-trap, shadowc-init
//                  (module passes; loaded only by licensed tooling - see
//                  src/gatekeeper.py)
//
//===----------------------------------------------------------------------===//

#include "shadowc/Passes.h"
#include "shadowc/Version.h"

#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"

using namespace llvm;

static void registerShadowCPasses(PassBuilder &PB) {
  // The passes below are function-scoped.
  PB.registerPipelineParsingCallback(
      [](StringRef Name, FunctionPassManager &FPM,
         ArrayRef<PassBuilder::PipelineElement>) -> bool {
        if (Name == "shadowc-cff") {
          FPM.addPass(shadowc::ControlFlowFlatteningPass());
          return true;
        }
        if (Name == "shadowc-boguscf") {
          FPM.addPass(shadowc::BogusControlFlowPass());
          return true;
        }
        if (Name == "shadowc-substitution") {
          FPM.addPass(shadowc::InstructionSubstitutionPass());
          return true;
        }
        if (Name == "shadowc-hardpred") {
          FPM.addPass(shadowc::HardPredicatesPass());
          return true;
        }
        return false;
      });

  // String virtualization and the Vault weaves operate at module scope.
  PB.registerPipelineParsingCallback(
      [](StringRef Name, ModulePassManager &MPM,
         ArrayRef<PassBuilder::PipelineElement>) -> bool {
        if (Name == "shadowc-string") {
          MPM.addPass(shadowc::StringEncryptionPass());
          return true;
        }
        if (Name == "shadowc-trap") {
          MPM.addPass(shadowc::AntiDebugPass());
          return true;
        }
        if (Name == "shadowc-virt") {
          MPM.addPass(shadowc::VirtualizationPass());
          return true;
        }
        if (Name == "shadowc-init") {
          MPM.addPass(shadowc::InitScramblePass());
          return true;
        }
        return false;
      });
}

llvm::PassPluginLibraryInfo getShadowCPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "shadowc-passes", SHADOWC_VERSION,
          registerShadowCPasses};
}

extern "C" LLVM_ATTRIBUTE_WEAK ::llvm::PassPluginLibraryInfo
llvmGetPassPluginInfo() {
  return getShadowCPluginInfo();
}