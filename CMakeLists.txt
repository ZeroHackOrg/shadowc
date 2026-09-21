cmake_minimum_required(VERSION 3.20)
project(shadowc VERSION 0.5.0 DESCRIPTION "Shadow-Compile: LLVM IR obfuscation engine" LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---------------------------------------------------------------------------
# Locate an installed LLVM. The CI and contributors pass
#   -DLT_LLVM_INSTALL_DIR=/usr/lib/llvm-18
# (project-template convention). Fall back to a sensible default.
# ---------------------------------------------------------------------------
if(NOT LT_LLVM_INSTALL_DIR)
  find_package(LLVM QUIET CONFIG)
  if(NOT LLVM_PACKAGE_VERSION)
    set(LT_LLVM_INSTALL_DIR "/usr/lib/llvm-18")
  endif()
endif()

if(LT_LLVM_INSTALL_DIR)
  list(APPEND CMAKE_PREFIX_PATH "${LT_LLVM_INSTALL_DIR}/lib/cmake/llvm")
  list(APPEND CMAKE_MODULE_PATH "${LT_LLVM_INSTALL_DIR}/lib/cmake/llvm")
endif()

find_package(LLVM REQUIRED CONFIG)

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

if(LLVM_PACKAGE_VERSION VERSION_LESS "16")
  message(FATAL_ERROR "shadowc requires LLVM >= 16 (found ${LLVM_PACKAGE_VERSION})")
endif()

if("${LLVM_PACKAGE_VERSION}" STRLESS "18")
  message(WARNING "shadowc is validated against LLVM 18. You are building against ${LLVM_PACKAGE_VERSION}.")
endif()

include_directories(${LLVM_INCLUDE_DIRS})
separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND "${LLVM_DEFINITIONS}")
add_definitions(${LLVM_DEFINITIONS_LIST})
include(AddLLVM)

# Project headers (include/shadowc/)
include_directories(${PROJECT_SOURCE_DIR}/include)

option(SHADOWC_BUILD_POSSIBLE_TESTS "Build shadowc integration tests" ON)
option(SHADOWC_BUILD_ENTERPRISE "Compile the Enterprise Vault gated passes" ON)

add_subdirectory(lib/passes)