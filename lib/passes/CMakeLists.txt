# ---------------------------------------------------------------------------
# shadowc pass plugin
# ---------------------------------------------------------------------------
# The LLVM passes live in this directory. They are compiled into the
# "ShadowCPasses" loadable module, which is consumed by
#   opt-18 -load-pass-plugin=libShadowCPasses.so -passes=...
# ---------------------------------------------------------------------------

add_llvm_pass_plugin(ShadowCPasses
  PassRegistry.cpp
  Utils.cpp
  ControlFlowFlattening.cpp
  OpaquePredicates.cpp
  InstructionSubstitution.cpp
  HardPredicates.cpp
  StringEncryption.cpp
  AntiDebug.cpp
  Virtualization.cpp
  InitScramble.cpp
)

if(SHADOWC_BUILD_ENTERPRISE)
  target_compile_definitions(ShadowCPasses PUBLIC SHADOWC_ENTERPRISE_ENABLED)
endif()

install(TARGETS ShadowCPasses
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION bin
)