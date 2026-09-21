// The Go side of the native cgo boundary. Zero-sugar on purpose: the only
// moving part is that native_crc/native_checksum live in a hardened static
// archive produced by shadowc --emit obj.
//
//   go build -o app .
//   ./app
package main

/*
#cgo LDFLAGS: -L${SRCDIR} -l:libshadownative.a
#include <stdint.h>
uint32_t native_img_crc(void);
uint32_t native_checksum(uint32_t);
*/
import "C"

import "fmt"

func main() {
	crc := uint32(C.native_img_crc())
	chk := uint32(C.native_checksum(0xCAFEBABE))
	fmt.Printf("go crc=%.8x chk=%08x\n", crc, chk)
}
