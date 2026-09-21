/* Native C core hardened by shadowc and cgo-linked from Go.
 *
 * Go itself has no clang-front-ended IR pipeline, so the *native* surface
 * (the C parts at the cgo boundary - where actual memory-safety bugs live)
 * is the part that gets hardened. docs/TESTING.md explains the contract.
 */
#include <stdint.h>
#include <stddef.h>

static const uint8_t native_img[4] = {0xA5u, 0xF0u, 0x0Cu, 0x21u};

static uint32_t crc32_block(const uint8_t *buf, size_t n) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) {
        crc ^= buf[i];
        for (int b = 0; b < 8; ++b)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

/* Hardened *data*: the boot image lives inside the hardened native core. */
uint32_t native_img_crc(void) {
    return crc32_block(native_img, sizeof native_img);
}

uint32_t native_checksum(uint32_t v) {
    return (v * 2654435761u) ^ (v >> 16);
}
