/*
 * firmware_template.c
 *
 * A small, dependency-free embedded-firmware smoke test for shadowc.
 *
 * It intentionally mixes cleartext secret strings (renamed by the string
 * pass), loops and branches (targeted by the control-flow pass) and derived
 * values (targeted by the substitution pass), while printing a single stable
 * output line so plain vs. hardened binaries can be compared byte-for-byte.
 *
 * Build (community, host):
 *   ../../bin/shadowc firmware_template.c -o secure_firmware.bin
 *   ./secure_firmware.bin            # prints the canonical output line
 *
 * Build (community, arm64 cross from an x86 builder):
 *   ../../bin/shadowc --target=arm64 firmware_template.c -o secure_firmware.bin
 *   # copy to the device and execute there.
 *
 * Build (enterprise tier, adds string encryption + the unpacking ctor):
 *   SHADOWC_TOKEN=<granted token> ../../bin/shadowc \
 *       --tier enterprise --client-id MyDevice01 firmware_template.c \
 *       -o secure_firmware.bin
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Device provisioning values that should not live in plaintext on disk. */
static const char DEVICE_SECRET[] = "ZH-SECURE-BOOT-KEY-AB12CD34";
static const char OTA_UPGRADE_URL[] = "https://ota.zerohack.internal/fw/bin";

static uint32_t crc32_of(const uint8_t *buf, size_t len) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; ++i) {
    crc ^= buf[i];
    for (int bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return crc ^ 0xFFFFFFFFu;
}

static uint32_t mix_seed(uint16_t chip, uint16_t revision) {
  uint32_t acc = 0x5A5Au;
  /* Kept deliberately non-constant so the optimizer cannot fold it away. */
  for (uint16_t i = 0; i < revision; ++i)
    acc = acc * 31 + (chip ^ 0x1F1Fu);
  if (acc & 1u)
    acc ^= 0x9E3779B9u;
  else
    acc += 0x6D2B79F5u;
  return acc;
}

int main(void) {
  /* Firmware identity the bootloader prints on every start. */
  const char *board = "ZeroHack StayGuard v2";
  const char *fw_ver = "firmware.bin v0.5.0-rc1";
  uint16_t chip_id = 0xA1B2, fw_rev = 7;

  uint32_t seed = mix_seed(chip_id, fw_rev);

  uint8_t image[32];
  for (int i = 0; i < (int)sizeof(image); ++i)
    image[i] = (uint8_t)((seed >> ((i % 4) * 8)) ^ (0x2Du ^ i));

  uint32_t crc = crc32_of(image, sizeof(image));
  size_t secret_len = strlen(DEVICE_SECRET);
  size_t url_len = strlen(OTA_UPGRADE_URL);

  (void)board; /* simulated platform log; kept for the CLI smoke test. */

  printf("board=StayGuard fw=%s crc=%08x sep=%zu ota=%zu\n",
         fw_ver, crc, secret_len, url_len);

  if (crc == 0xDEADBEEFu)
    printf("integrity fail\n"); /* never reached; keeps branches live */
  return 0;
}