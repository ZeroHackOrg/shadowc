/* Equivalence fixture: nested-loop CRC-32 over a seed-built image.
 *
 * This is the exact regression shape that exposed the CFF entry-successor
 * miscompile: an image-init loop in the same function as a byte/bit CRC
 * loop. The expected digest is pinned below - do NOT change this program.
 */
#include <stdint.h>
#include <stdio.h>

static uint32_t crc32_block(const uint8_t *buf, size_t n) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) {
        crc ^= buf[i];
        for (int b = 0; b < 8; ++b) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

int main(void) {
    uint8_t image[8];
    uint32_t seed = 0xA5F0c21u;
    for (unsigned i = 0; i < 8; ++i)
        image[i] = (uint8_t)(seed >> ((i % 4) * 8));
    printf("crc=%.8x\n", crc32_block(image, sizeof image));
    return 0;
}