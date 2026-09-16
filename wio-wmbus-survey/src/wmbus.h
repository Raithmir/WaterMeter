// wM-Bus T1 (3-of-6, frame format A) and C1 (NRZ, frame format A or B) decoder,
// plus Diehl IZAR/PRIOS payload decode and OMS device type names.
// Plain C++, no Arduino dependencies, so it can be unit-tested on a PC.
// IZAR logic ported from wmbusmeters (GPL-3.0): manufacturer_specificities.cc / driver_izar.cc
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>

namespace wmbus {

static const size_t MAX_FRAME = 255;  // T1: 255 raw -> 170 decoded; C1: up to 253 bytes

struct Telegram {
  uint8_t data[MAX_FRAME];  // link-layer frame with CRCs removed, data[0] = L field
  size_t len = 0;
  uint16_t mfctCode() const { return data[2] | (data[3] << 8); }
  // Diehl PRIOS frames (non-SAP, CI 0xA0-0xA7 / 0x71) carry the real address 2 bytes later,
  // with version/type moved in front of it (same transform wmbusmeters applies).
  bool diehlSwapped() const {
    uint16_t m = mfctCode();
    bool diehl = m == 0x11A5 /*DME*/ || m == 0x16F4 /*EWT*/ || m == 0x2324 /*HYD*/;
    return diehl && len >= 11 && (data[1] == 0x44 || data[1] == 0x46) &&
           ((data[10] >= 0xA0 && data[10] <= 0xA7) || data[10] == 0x71);
  }
  uint32_t id() const {
    const uint8_t *a = data + (diehlSwapped() ? 6 : 4);
    return (uint32_t)a[3] << 24 | a[2] << 16 | a[1] << 8 | a[0];
  }
  void mfct(char out[4]) const {
    uint16_t m = mfctCode();
    out[0] = '@' + ((m >> 10) & 0x1F);
    out[1] = '@' + ((m >> 5) & 0x1F);
    out[2] = '@' + (m & 0x1F);
    out[3] = 0;
  }
  uint8_t version() const { return diehlSwapped() ? data[4] : data[8]; }
  uint8_t type() const { return diehlSwapped() ? data[5] : data[9]; }
  uint8_t ci() const { return data[10]; }
};

enum class Mode : uint8_t { NONE = 0, T1 = 1, C1A = 2, C1B = 3 };

inline const char *modeStr(Mode m) {
  switch (m) {
    case Mode::T1: return "T1";
    case Mode::C1A: return "C1a";
    case Mode::C1B: return "C1b";
    default: return "?";
  }
}

// ---- 3-of-6 ----
static const uint8_t ENC36[16] = {0x16, 0x0D, 0x0E, 0x0B, 0x1C, 0x19, 0x1A, 0x13,
                                  0x2C, 0x25, 0x26, 0x23, 0x34, 0x31, 0x32, 0x29};

inline int dec36(uint8_t sym) {
  for (int i = 0; i < 16; i++)
    if (ENC36[i] == sym) return i;
  return -1;
}

inline uint8_t readBits(const uint8_t *buf, size_t bitPos, int n) {
  uint8_t v = 0;
  for (int i = 0; i < n; i++) {
    size_t p = bitPos + i;
    v = (v << 1) | ((buf[p >> 3] >> (7 - (p & 7))) & 1);
  }
  return v;
}

// Decode `count` bytes from a 3-of-6 bitstream. Returns false on an invalid symbol.
inline bool decode36(const uint8_t *raw, size_t rawLen, uint8_t *out, size_t count) {
  if ((count * 12 + 7) / 8 > rawLen) return false;
  for (size_t i = 0; i < count; i++) {
    int hi = dec36(readBits(raw, i * 12, 6));
    int lo = dec36(readBits(raw, i * 12 + 6, 6));
    if (hi < 0 || lo < 0) return false;
    out[i] = (uint8_t)(hi << 4 | lo);
  }
  return true;
}

// ---- CRC (EN 13757-4: poly 0x3D65, init 0, xorout 0xFFFF) ----
inline uint16_t crc16(const uint8_t *d, size_t n) {
  uint16_t crc = 0;
  for (size_t i = 0; i < n; i++) {
    crc ^= (uint16_t)d[i] << 8;
    for (int b = 0; b < 8; b++) crc = (crc & 0x8000) ? (crc << 1) ^ 0x3D65 : (crc << 1);
  }
  return ~crc;
}

// Total on-air bytes (incl. CRCs) for a format A frame with length field L.
inline size_t formatALength(uint8_t L) {
  if (L < 9) return 0;
  size_t rest = L - 9;  // data bytes after the 10-byte first block
  return 12 + (rest / 16) * 18 + ((rest % 16) ? (rest % 16) + 2 : 0);
}

enum class Result { OK, BAD_SYMBOL, TOO_SHORT, TOO_LONG, BAD_CRC };

inline const char *resultStr(Result r) {
  switch (r) {
    case Result::OK: return "ok";
    case Result::BAD_SYMBOL: return "3of6";
    case Result::TOO_SHORT: return "short";
    case Result::TOO_LONG: return "long";
    default: return "crc";
  }
}

// Format A: 10-byte first block, then blocks of up to 16 bytes, each followed by a 2-byte CRC.
inline Result stripFormatA(const uint8_t *buf, size_t total, Telegram &t) {
  size_t in = 0, outLen = 0, blockLen = 10;
  while (in < total) {
    size_t remaining = total - in - 2;
    size_t n = remaining < blockLen ? remaining : blockLen;
    uint16_t c = crc16(buf + in, n);
    if (buf[in + n] != (c >> 8) || buf[in + n + 1] != (c & 0xFF)) return Result::BAD_CRC;
    memcpy(t.data + outLen, buf + in, n);
    outLen += n;
    in += n + 2;
    blockLen = 16;
  }
  t.len = outLen;
  return Result::OK;
}

// raw: bytes received after the 0x543D sync word (T1, 3-of-6 encoded).
inline Result decodeT1(const uint8_t *raw, size_t rawLen, Telegram &t) {
  uint8_t L;
  if (!decode36(raw, rawLen, &L, 1)) return Result::BAD_SYMBOL;
  size_t total = formatALength(L);
  if (total == 0) return Result::TOO_SHORT;
  if (total > MAX_FRAME || (total * 12 + 7) / 8 > rawLen) return Result::TOO_LONG;

  uint8_t buf[MAX_FRAME];
  if (!decode36(raw, rawLen, buf, total)) return Result::BAD_SYMBOL;
  return stripFormatA(buf, total, t);
}

// ---- C1 ----
// Format B: L counts every byte after L, CRCs included. One CRC over the first 126 bytes
// (or the whole frame if shorter), and a second CRC over the remainder for long frames.
inline Result stripFormatB(const uint8_t *buf, size_t len, Telegram &t) {
  if (len < 12) return Result::TOO_SHORT;
  size_t crc1 = len <= 128 ? len - 2 : 126;
  uint16_t c = crc16(buf, crc1);
  if (buf[crc1] != (c >> 8) || buf[crc1 + 1] != (c & 0xFF)) return Result::BAD_CRC;
  memcpy(t.data, buf, crc1);
  t.len = crc1;
  if (len > 128) {
    size_t n = len - 2 - (crc1 + 2);
    c = crc16(buf + crc1 + 2, n);
    if (buf[len - 2] != (c >> 8) || buf[len - 1] != (c & 0xFF)) return Result::BAD_CRC;
    memcpy(t.data + t.len, buf + crc1 + 2, n);
    t.len += n;
  }
  return Result::OK;
}

// raw: bytes after the 0x543D sync word. C1 frames continue with 0x54CD (format A) or 0x543D (B),
// then plain NRZ bytes. 0x54 can never start a valid 3-of-6 stream, so this can't clash with T1.
inline bool isC1(const uint8_t *raw, size_t rawLen) {
  return rawLen >= 3 && raw[0] == 0x54 && (raw[1] == 0xCD || raw[1] == 0x3D);
}

inline Result decodeC1(const uint8_t *raw, size_t rawLen, Telegram &t, Mode &mode) {
  const uint8_t *buf = raw + 2;
  size_t avail = rawLen - 2;
  uint8_t L = buf[0];
  if (raw[1] == 0xCD) {
    mode = Mode::C1A;
    size_t total = formatALength(L);
    if (total == 0) return Result::TOO_SHORT;
    if (total > avail) return Result::TOO_LONG;
    return stripFormatA(buf, total, t);
  }
  mode = Mode::C1B;
  size_t total = (size_t)L + 1;
  if (total > avail) return Result::TOO_LONG;
  return stripFormatB(buf, total, t);
}

// Decode whatever arrived after the sync word.
inline Result decode(const uint8_t *raw, size_t rawLen, Telegram &t, Mode &mode) {
  mode = Mode::NONE;
  if (isC1(raw, rawLen)) return decodeC1(raw, rawLen, t, mode);
  Result r = decodeT1(raw, rawLen, t);
  if (r == Result::OK) mode = Mode::T1;
  return r;
}

// ---- Diehl IZAR (PRIOS LFSR scrambling) ----
inline uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

// Default PRIOS keys "39BC8A10E66D83F8" and "51728910E66D83F8", each folded as key1 ^ key2.
static const uint32_t PRIOS_KEYS[2] = {0x39BC8A10UL ^ 0xE66D83F8UL, 0x51728910UL ^ 0xE66D83F8UL};

inline bool isDiehl(const Telegram &t) {
  char m[4];
  t.mfct(m);
  return !strcmp(m, "SAP") || !strcmp(m, "DME") || !strcmp(m, "HYD") || !strcmp(m, "EWT");
}

// On success writes total litres (and last-month litres, if present).
inline bool decodeIzar(const Telegram &t, uint32_t &litres, uint32_t *lastMonth = nullptr) {
  if (t.len < 20 || !isDiehl(t)) return false;
  for (uint32_t key : PRIOS_KEYS) {
    key ^= be32(t.data + 2);
    key ^= be32(t.data + 6);
    key ^= be32(t.data + 10);
    size_t size = t.len - 15;
    uint8_t dec[MAX_FRAME];
    bool ok = true;
    for (size_t i = 0; i < size; i++) {
      for (int j = 0; j < 8; j++) {
        uint8_t bit = ((key & 0x2) != 0) ^ ((key & 0x4) != 0) ^ ((key & 0x800) != 0) ^ ((key & 0x80000000UL) != 0);
        key = (key << 1) | bit;
      }
      dec[i] = t.data[i + 15] ^ (key & 0xFF);
      if (i == 0 && dec[0] != 0x4B) { ok = false; break; }
    }
    if (!ok || size < 5) continue;
    litres = dec[1] | dec[2] << 8 | dec[3] << 16 | (uint32_t)dec[4] << 24;
    if (lastMonth && size >= 9) *lastMonth = dec[5] | dec[6] << 8 | dec[7] << 16 | (uint32_t)dec[8] << 24;
    return true;
  }
  return false;
}

// ---- IZAR alarm flags (plain header bytes 11-13, per wmbusmeters driver_izar) ----
enum : uint16_t {
  ALM_GENERAL = 1 << 0,
  ALM_LEAK_NOW = 1 << 1,
  ALM_LEAK_PREV = 1 << 2,
  ALM_BLOCKED = 1 << 3,
  ALM_BACKFLOW = 1 << 4,
  ALM_UNDERFLOW = 1 << 5,
  ALM_OVERFLOW = 1 << 6,
  ALM_SUBMARINE = 1 << 7,
  ALM_SENSOR_FRAUD_NOW = 1 << 8,
  ALM_SENSOR_FRAUD_PREV = 1 << 9,
  ALM_MECH_FRAUD_NOW = 1 << 10,
  ALM_MECH_FRAUD_PREV = 1 << 11,
};

// Only meaningful when decodeIzar() succeeded for this telegram.
inline uint16_t izarAlarms(const Telegram &t) {
  if (t.len < 14) return 0;
  uint8_t a = t.data[11], b = t.data[12], c = t.data[13];
  uint16_t f = 0;
  if (a & 0x80) f |= ALM_GENERAL;
  if (b & 0x80) f |= ALM_LEAK_NOW;
  if (b & 0x40) f |= ALM_LEAK_PREV;
  if (b & 0x20) f |= ALM_BLOCKED;
  if (c & 0x80) f |= ALM_BACKFLOW;
  if (c & 0x40) f |= ALM_UNDERFLOW;
  if (c & 0x20) f |= ALM_OVERFLOW;
  if (c & 0x10) f |= ALM_SUBMARINE;
  if (c & 0x08) f |= ALM_SENSOR_FRAUD_NOW;
  if (c & 0x04) f |= ALM_SENSOR_FRAUD_PREV;
  if (c & 0x02) f |= ALM_MECH_FRAUD_NOW;
  if (c & 0x01) f |= ALM_MECH_FRAUD_PREV;
  return f;
}

// Short, space-separated text (fits the 128px OLED). Returns "ok" when clear.
inline void alarmText(uint16_t f, char *out, size_t n) {
  static const struct { uint16_t bit; const char *txt; } names[] = {
      {ALM_LEAK_NOW, "LEAK"},        {ALM_BLOCKED, "blocked"},   {ALM_BACKFLOW, "backflow"},
      {ALM_OVERFLOW, "over"},        {ALM_UNDERFLOW, "under"},   {ALM_SUBMARINE, "submerged"},
      {ALM_SENSOR_FRAUD_NOW, "tamperS"}, {ALM_MECH_FRAUD_NOW, "tamperM"}, {ALM_LEAK_PREV, "leak(was)"},
      {ALM_SENSOR_FRAUD_PREV, "tamperS(was)"}, {ALM_MECH_FRAUD_PREV, "tamperM(was)"},
  };
  out[0] = 0;
  size_t used = 0;
  for (auto &nm : names) {
    if (!(f & nm.bit)) continue;
    int w = snprintf(out + used, n - used, "%s%s", used ? " " : "", nm.txt);
    if (w < 0 || used + w >= n) break;
    used += w;
  }
  if (!used) snprintf(out, n, "%s", (f & ALM_GENERAL) ? "general" : "ok");
}

// ---- OMS / EN 13757-3 device type (A-field) ----
// shortName fits the list view (<=5 chars), longName the detail view (<=12 chars).
inline void deviceType(uint8_t t, const char *&shortName, const char *&longName) {
  struct T { uint8_t code; const char *s, *l; };
  static const T types[] = {
      {0x00, "other", "other"},      {0x01, "oil", "oil"},           {0x02, "elec", "electricity"},
      {0x03, "gas", "gas"},          {0x04, "heat", "heat (out)"},   {0x05, "steam", "steam"},
      {0x06, "warm", "warm water"},  {0x07, "water", "water"},       {0x08, "HCA", "heat cost al"},
      {0x09, "air", "compr. air"},   {0x0A, "cool", "cooling out"},  {0x0B, "cool", "cooling in"},
      {0x0C, "heat", "heat (in)"},   {0x0D, "h/c", "heat/cooling"},  {0x0E, "bus", "bus/system"},
      {0x0F, "unkn", "unknown"},     {0x15, "hot", "hot water"},     {0x16, "cold", "cold water"},
      {0x17, "h/c w", "hot/cold wtr"}, {0x18, "press", "pressure"},  {0x19, "A/D", "A/D conv"},
      {0x1A, "smoke", "smoke alarm"}, {0x1B, "room", "room sensor"}, {0x1C, "gasdt", "gas detector"},
      {0x20, "brkr", "breaker"},     {0x21, "valve", "valve"},       {0x25, "displ", "display unit"},
      {0x28, "waste", "waste water"}, {0x29, "garb", "garbage"},     {0x31, "ctrl", "comms ctrl"},
      {0x32, "rptr", "repeater"},    {0x33, "rptr", "repeater"},     {0x36, "radio", "radio conv"},
      {0x37, "radio", "radio conv"},
  };
  for (const T &e : types)
    if (e.code == t) { shortName = e.s; longName = e.l; return; }
  shortName = "?";
  longName = "unknown type";
}

}  // namespace wmbus
