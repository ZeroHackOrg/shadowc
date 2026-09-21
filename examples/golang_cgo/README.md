# Go, hardened at the cgo boundary

Go has **no clang-front-ended IR pipeline**, so in-place Go hardening does not
exist and shadowc refuses `.go` input with explicit guidance (there is no
silent mis-handling). What *can* be hardened is the **native surface**: the C
code behind `import "C"` — which is precisely where memory-safety bugs live —
and the hardened constants/data that Go's logic depends on.

This example is that boundary, made operational:

```
native.c   the native core (image data + checksum logic), hardened by shadowc
main.go    the Go side; trivial until the archive is linked
build_native.sh   one-shot: shadowc --emit obj -> ar -> libshadownative.a
```

## Build

```bash
# 1. mint a vault token (enterprise lane)
TOKEN=$(python3 tools/gen_license.py --client cgo --salt ZH_COMMUNITY_DEMO_2026 \
       | awk '/^token/{print $3}')

# 2. harden the native core and archive it for cgo
bash examples/golang_cgo/build_native.sh "$TOKEN"

# 3. link + run from the Go side
cd examples/golang_cgo
go build -o app .
./app
```

Expected output (arbitrary binary, so only equality matters):

```
go crc=a9b23d54 chk=e2b7de2a
```

The archive that lands in `libshadownative.a` carries the full Enterprise
Vault weave (string/virt/trap/init); `go build`'s result is identical whether
it links the plain or the hardened core — that is the correctness contract
`tests/test_multilang.py::test_go_cgo_native_boundary_is_hardened` proves
automatically.

## Honest limits

- The **Go side** itself is untouched — this is not "Go hardening".
- Hosting the cgo boundary in a cross-compile (`CGO_ENABLED=1 GOOS=`)
  requires a matching cross C toolchain for the final link.
- `TinyGo`-emitted IR is a documented experimental idea, not a supported lane.