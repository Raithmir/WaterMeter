// Minimal FAT12 formatter for the 2 MB QSPI flash. SdFat's FatFormatter refuses volumes
// under 6 MB, so this writes the same "superfloppy" layout CircuitPython uses on small
// flash chips: no partition table, 512-byte sectors, 1 sector per cluster.
#pragma once
#include <stdint.h>
#include <string.h>

namespace fat12 {

inline void put16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
inline void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }

// label: up to 11 characters, upper case, shown as the drive name on the host.
// Flash: anything with sectorCount(), writeSectors() and syncBlocks() (Adafruit_SPIFlash).
template <typename Flash>
bool format(Flash &flash, const char *label, uint32_t serial) {
  const uint16_t ROOT_ENTRIES = 512;  // 32 sectors
  const uint8_t NUM_FATS = 2;
  const uint32_t total = flash.sectorCount();
  if (total < 64 || total > 0xFFFF) return false;
  const uint16_t rootSectors = ROOT_ENTRIES * 32 / 512;
  // FAT12: 1.5 bytes per cluster, +2 reserved entries. Over-estimating clusters is harmless.
  const uint16_t fatSectors = ((total + 2) * 3 / 2 + 511) / 512;
  const uint32_t dataStart = 1 + NUM_FATS * fatSectors + rootSectors;
  if (total - dataStart >= 4085) return false;  // would have to be FAT16

  uint8_t buf[512];
  char name[11];
  memset(name, ' ', sizeof(name));
  memcpy(name, label, strlen(label) < sizeof(name) ? strlen(label) : sizeof(name));

  // boot sector / BPB
  memset(buf, 0, sizeof(buf));
  buf[0] = 0xEB; buf[1] = 0x3C; buf[2] = 0x90;
  memcpy(buf + 3, "MSDOS5.0", 8);
  put16(buf + 11, 512);            // bytes per sector
  buf[13] = 1;                     // sectors per cluster
  put16(buf + 14, 1);              // reserved sectors
  buf[16] = NUM_FATS;
  put16(buf + 17, ROOT_ENTRIES);
  put16(buf + 19, total);          // total sectors (16-bit)
  buf[21] = 0xF8;                  // media: fixed disk
  put16(buf + 22, fatSectors);
  put16(buf + 24, 32);             // sectors per track (unused)
  put16(buf + 26, 2);              // heads (unused)
  buf[36] = 0x80;                  // drive number
  buf[38] = 0x29;                  // extended boot signature
  put32(buf + 39, serial);         // volume serial
  memcpy(buf + 43, name, 11);
  memcpy(buf + 54, "FAT12   ", 8);
  buf[510] = 0x55; buf[511] = 0xAA;
  if (!flash.writeSectors(0, buf, 1)) return false;

  // FATs: first two entries reserved (media byte + end-of-chain), rest free
  for (uint8_t f = 0; f < NUM_FATS; f++) {
    for (uint16_t s = 0; s < fatSectors; s++) {
      memset(buf, 0, sizeof(buf));
      if (s == 0) { buf[0] = 0xF8; buf[1] = 0xFF; buf[2] = 0xFF; }
      if (!flash.writeSectors(1 + f * fatSectors + s, buf, 1)) return false;
    }
  }

  // root directory, first entry = volume label
  for (uint16_t s = 0; s < rootSectors; s++) {
    memset(buf, 0, sizeof(buf));
    if (s == 0) {
      memcpy(buf, name, 11);
      buf[11] = 0x08;  // ATTR_VOLUME_ID
    }
    if (!flash.writeSectors(1 + NUM_FATS * fatSectors + s, buf, 1)) return false;
  }
  return flash.syncBlocks();
}

}  // namespace fat12
