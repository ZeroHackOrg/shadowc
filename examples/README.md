# shadowc examples

This directory holds firmware-style C samples used to exercise the shadowc
pipeline end to end. The reference workflow (from the project README) is:

```sh
./bin/shadowc --target=arm64 ../examples/firmware_template.c -o secure_output.bin
```

## Running the example on the host

```sh
./bin/shadowc examples/firmware_template.c -o /tmp/secure.bin
/tmp/secure.bin
```

Every hardened build must print exactly:

```
board=StayGuard fw=firmware.bin v0.5.0-rc1 crc=<crc> sep=<len> ota=<len>
```

(crc/sep/ota are deterministic; the prefix is fixed). Compare it against the
unprotected build to confirm the passes preserved semantics:

```sh
clang-18 -O1 examples/firmware_template.c -o /tmp/plain.bin
/tmp/plain.bin
```

## Cross-compiling for ARM64 from an x86_64 builder

`llc-18` on the host can emit ARM64 code, so the hardening pipeline itself
needs no cross toolchain. Building the final **executable** relinks through
clang's discovery of a cross GCC toolchain (`gcc-aarch64-linux-gnu` on
Debian/Ubuntu); without one, emit the object and link on the device:

```sh
./bin/shadowc --target=arm64 examples/firmware_template.c -o /tmp/secure_arm64.bin
file /tmp/secure_arm64.bin         # ELF 64-bit LSB, ARM aarch64

# without a cross linker installed:
./bin/shadowc --target=arm64 --emit obj examples/firmware_template.c -o /tmp/secure_arm64.o
```

Copy the binary to the device and run it there to see the same output line.

## Enterprise tier (string encryption)

```sh
SHADOWC_TOKEN=<granted token> ./bin/shadowc \
  --level enterprise --client-id MyDevice01 examples/firmware_template.c \
  -o /tmp/secure_ent.bin
strings /tmp/secure_ent.bin | grep -i secret   # no output
```