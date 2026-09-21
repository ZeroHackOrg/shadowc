#!/usr/bin/env bash
# ShadowC build helper: configure + build the LLVM pass plugin.
#
#   ./scripts/build.sh                 # LLVM discovered or /usr/lib/llvm-18
#   ./scripts/build.sh /usr/lib/llvm-18
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LLVM_DIR="${1:-${LT_LLVM_INSTALL_DIR:-/usr/lib/llvm-18}}"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"

if ! command -v cmake >/dev/null 2>&1; then
    echo "error: cmake is required to build shadowc. Install it or use a venv."
    exit 1
fi

echo ">>> configuring shadowc ($ROOT) against $LLVM_DIR"
cmake -S "$ROOT" -B "$BUILD_DIR" -DLT_LLVM_INSTALL_DIR="$LLVM_DIR" -DCMAKE_BUILD_TYPE=Release
echo ">>> building pass plugin"
cmake --build "$BUILD_DIR" -j"$(nproc)"

echo ">>> plugin ready: $BUILD_DIR/lib/passes/ShadowCPasses.so"