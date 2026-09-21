#!/usr/bin/env bash
# Harden the *native* side of a Go program at the cgo boundary.
#
# Go has no clang-front-ended IR pipeline, so in-place Go hardening does not
# exist. What CAN be hardened is the C code at the cgo boundary - the surface
# where memory-safety bugs actually live. This script builds that C core with
# shadowc (Enterprise Vault tier), archives it, and prints the cgo link line.
#
# Usage:
#   TOKEN=$(python3 tools/gen_license.py --client cgo --salt ZH_COMMUNITY_DEMO_2026 \
#          | awk '/^token/{print $3}')
#   bash examples/golang_cgo/build_native.sh "$TOKEN"
#   cd examples/golang_cgo
#   go build -o app . && ./app
#
# Expected output:  go crc=a9b23d54 chk=e2b7de2a
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"

if [[ $# -lt 1 ]]; then
  echo "usage: build_native.sh <vault token>" >&2
  echo "  mint a token with tools/gen_license.py first (see README)" >&2
  exit 2
fi
token="$1"
arch="${SHADOWC_GO_ARCH:-$(uname -m)}"

echo "== shadowc hardening native.c (--emit obj, Enterprise Vault) =="
SHADOWC_TOKEN="$token" "$root/bin/shadowc" \
  --level enterprise --client-id cgo --emit obj \
  "$here/native.c" -o "$here/libshadownative.o"

echo "== archiving for cgo =="
ar rcs "$here/libshadownative.a" "$here/libshadownative.o"
rm -f "$here/libshadownative.o"

echo "== cgo link =="
echo "  cd examples/golang_cgo && go build -o app . && ./app"
echo "  (main.go links -L\${SRCDIR} -l:libshadownative.a automatically)"
echo "done (arch $arch): $(ls -la "$here/libshadownative.a" | awk '{print $5" bytes"}')"