// Snapshot ring: the survey is saved as snapshots written one after another through a
// pre-allocated, contiguous file (state.bin), wrapping at the end. The FAT tables and directory
// are never touched by a save, and every part of the ring wears evenly (rewriting a FAT file
// every 30 s would wear out the directory's flash block in days of continuous use). A snapshot
// is only trusted if its CRC checks out, so a power cut mid-save falls back to the previous one.
// Ring block 0 holds a random ring ID; snapshots carry it, so stale data left in the flash from
// before a FORMAT is never mistaken for a snapshot.
//
// Plain C++ over any flash with readSectors/writeSectors/syncBlocks (512-byte sectors), so it
// can be unit-tested on a PC (test/test_survey.cpp).
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace snapring {

const uint32_t MAGIC_RING = 0x474E4952;  // "RING"
const uint32_t MAGIC_SNAP = 0x50414E53;  // "SNAP"
const size_t SECTOR = 512, BLOCK = 4096, SECTORS_PER_BLOCK = BLOCK / SECTOR;

struct RingHeader {
  uint32_t magic, ringId;
};
struct SnapHeader {
  uint32_t magic, ringId, seq;
  uint16_t version, count;      // count = items (meters)
  uint16_t aLen, bLen;          // two extra byte blobs (the log rows not yet in the CSV files)
  uint32_t crc;                 // CRC-32 of everything after the header
};
static_assert(sizeof(SnapHeader) == 24, "snapshot header is on the flash: don't change it");

inline uint32_t crc32(uint32_t crc, const uint8_t *p, size_t n) {
  crc = ~crc;
  while (n--) {
    crc ^= *p++;
    for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320UL & (0 - (crc & 1)));
  }
  return ~crc;
}

// What a snapshot holds: `count` items of itemSize bytes, then blobs a and b.
struct Contents {
  void *items;
  size_t itemSize;
  uint16_t count, maxCount;
  uint8_t *a;
  uint16_t aLen, aMax;
  uint8_t *b;
  uint16_t bLen, bMax;
};

template <typename Flash>
class Ring {
 public:
  explicit Ring(Flash &f) : flash(f) {}

  uint32_t base = 0, blocks = 0;  // first sector (4 KB aligned) and size in 4 KB blocks
  uint32_t id = 0, seq = 0, next = 1;  // ring ID; newest sequence number; next snapshot's block

  // Adopt the ring at `base` (in sectors), or start a new one with ID newId if `fresh` or
  // block 0 holds no ring header.
  bool begin(uint32_t baseSector, uint32_t nBlocks, bool fresh, uint32_t newId) {
    base = baseSector;
    blocks = nBlocks;
    uint8_t sec[SECTOR];
    if (!flash.readSectors(base, sec, 1)) return false;
    RingHeader rh;
    memcpy(&rh, sec, sizeof(rh));
    if (fresh || rh.magic != MAGIC_RING) return writeHeader(newId);
    id = rh.ringId;
    return true;
  }

  // New ring ID: every snapshot written so far stops counting.
  bool writeHeader(uint32_t newId) {
    uint8_t sec[SECTOR];
    memset(sec, 0xFF, sizeof(sec));
    id = newId;
    RingHeader rh = {MAGIC_RING, id};
    memcpy(sec, &rh, sizeof(rh));
    seq = 0;
    next = 1;
    return flash.writeSectors(base, sec, 1) && flash.syncBlocks();
  }

  // Newest valid snapshot of this version into c. Returns the item count, or -1 if there is none
  // (then aLen = bLen = 0). `onDamaged(seq)` is called for each newer snapshot that failed its CRC.
  template <typename OnDamaged>
  int load(uint16_t version, Contents &c, OnDamaged onDamaged) {
    static uint16_t cand[256];  // blocks with a plausible header, newest first
    static uint32_t candSeq[256];
    int nc = 0;
    uint8_t sec[SECTOR];
    for (uint32_t blk = 1; blk < blocks && nc < 256; blk++) {
      if (!flash.readSectors(base + blk * SECTORS_PER_BLOCK, sec, 1)) continue;
      SnapHeader h;
      memcpy(&h, sec, sizeof(h));
      if (h.magic != MAGIC_SNAP || h.ringId != id || h.version != version || h.count > c.maxCount ||
          h.aLen > c.aMax || h.bLen > c.bMax)
        continue;
      if (h.seq > seq) seq = h.seq;
      int i = nc++;
      while (i > 0 && candSeq[i - 1] < h.seq) {
        cand[i] = cand[i - 1];
        candSeq[i] = candSeq[i - 1];
        i--;
      }
      cand[i] = blk;
      candSeq[i] = h.seq;
    }
    for (int i = 0; i < nc; i++) {
      uint32_t blk = cand[i];
      SnapHeader h;
      uint32_t crc = 0, ignore = 0;
      size_t off = sizeof(h);
      if (!readBytes(blk, 0, (uint8_t *)&h, sizeof(h), ignore)) continue;
      size_t ib = h.count * c.itemSize;
      if (!readBytes(blk, off, (uint8_t *)c.items, ib, crc) || !readBytes(blk, off + ib, c.a, h.aLen, crc) ||
          !readBytes(blk, off + ib + h.aLen, c.b, h.bLen, crc) || crc != h.crc) {
        onDamaged(h.seq);
        continue;
      }
      c.count = h.count;
      c.aLen = h.aLen;
      c.bLen = h.bLen;
      next = blk + (sizeof(h) + ib + h.aLen + h.bLen + BLOCK - 1) / BLOCK;
      return h.count;
    }
    c.aLen = c.bLen = 0;
    return -1;
  }
  int load(uint16_t version, Contents &c) {
    return load(version, c, [](uint32_t) {});
  }

  // Write c (count/aLen/bLen used) as the newest snapshot.
  bool save(uint16_t version, const Contents &c) {
    const struct { const uint8_t *p; size_t n; } parts[] = {
        {(const uint8_t *)c.items, c.count * c.itemSize},
        {c.a, c.aLen},
        {c.b, c.bLen},
    };
    SnapHeader h = {MAGIC_SNAP, id, seq + 1, version, c.count, c.aLen, c.bLen, 0};
    size_t total = sizeof(h);
    for (auto &pt : parts) {
      h.crc = crc32(h.crc, pt.p, pt.n);
      total += pt.n;
    }
    uint32_t nb = (total + BLOCK - 1) / BLOCK;
    if (next + nb > blocks) next = 1;  // wrap (block 0 is the ring header)
    uint8_t sec[SECTOR];
    size_t fill = sizeof(h);
    memcpy(sec, &h, sizeof(h));
    uint32_t sector = base + next * SECTORS_PER_BLOCK;
    bool ok = true;
    for (auto &pt : parts) {
      size_t done = 0;
      while (done < pt.n) {
        size_t k = pt.n - done < sizeof(sec) - fill ? pt.n - done : sizeof(sec) - fill;
        memcpy(sec + fill, pt.p + done, k);
        done += k;
        fill += k;
        if (fill == sizeof(sec)) {
          ok = flash.writeSectors(sector++, sec, 1) && ok;
          fill = 0;
        }
      }
    }
    if (fill) {
      memset(sec + fill, 0xFF, sizeof(sec) - fill);
      ok = flash.writeSectors(sector, sec, 1) && ok;
    }
    ok = flash.syncBlocks() && ok;
    seq++;
    next += nb;
    return ok;
  }

 private:
  Flash &flash;

  // Reads n bytes of snapshot `blk`, starting `offset` bytes in, adding them to crc.
  bool readBytes(uint32_t blk, size_t offset, uint8_t *dst, size_t n, uint32_t &crc) {
    uint8_t sec[SECTOR];
    while (n) {
      uint32_t s = base + blk * SECTORS_PER_BLOCK + offset / SECTOR;
      size_t o = offset % SECTOR, k = n < SECTOR - o ? n : SECTOR - o;
      if (!flash.readSectors(s, sec, 1)) return false;
      memcpy(dst, sec + o, k);
      crc = crc32(crc, sec + o, k);
      dst += k;
      offset += k;
      n -= k;
    }
    return true;
  }
};

}  // namespace snapring
