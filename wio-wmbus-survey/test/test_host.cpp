// Host-side test: g++ -std=c++17 -Isrc test/test_host.cpp -o t && ./t   (from wio-wmbus-survey/)
// Vectors from wmbusmeters simulations/simulation_izars.txt
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include "wmbus.h"
using namespace wmbus;

static std::vector<uint8_t> hex(const std::string &s) {
  std::vector<uint8_t> v;
  for (size_t i = 0; i + 1 < s.size(); i += 2) v.push_back(strtol(s.substr(i, 2).c_str(), nullptr, 16));
  return v;
}

// Add format A CRCs, 3-of-6 encode, pad to 255 with noise.
static std::vector<uint8_t> onAir(const std::vector<uint8_t> &f) {
  std::vector<uint8_t> w;
  size_t i = 0, blk = 10;
  while (i < f.size()) {
    size_t n = std::min(blk, f.size() - i);
    uint16_t c = crc16(&f[i], n);
    w.insert(w.end(), f.begin() + i, f.begin() + i + n);
    w.push_back(c >> 8);
    w.push_back(c & 0xFF);
    i += n;
    blk = 16;
  }
  std::vector<uint8_t> raw(255);
  for (auto &b : raw) b = rand();
  size_t bit = 0;
  auto put = [&](uint8_t sym) {
    for (int k = 5; k >= 0; k--, bit++) {
      uint8_t &b = raw[bit >> 3];
      uint8_t m = 0x80 >> (bit & 7);
      b = ((sym >> k) & 1) ? (b | m) : (b & ~m);
    }
  };
  for (uint8_t b : w) { put(ENC36[b >> 4]); put(ENC36[b & 15]); }
  return raw;
}

static std::vector<uint8_t> withCrcA(const std::vector<uint8_t> &f) {
  std::vector<uint8_t> w;
  size_t i = 0, blk = 10;
  while (i < f.size()) {
    size_t n = std::min(blk, f.size() - i);
    uint16_t c = crc16(&f[i], n);
    w.insert(w.end(), f.begin() + i, f.begin() + i + n);
    w.push_back(c >> 8);
    w.push_back(c & 0xFF);
    i += n;
    blk = 16;
  }
  return w;
}

// C1 on air: 0x54 marker byte pair, then plain bytes, padded to 255 with noise.
static std::vector<uint8_t> c1Air(std::vector<uint8_t> f, bool formatB) {
  std::vector<uint8_t> body;
  if (!formatB) {
    body = withCrcA(f);
  } else {
    // format B: L counts all bytes after L including CRCs
    size_t n = f.size();
    size_t crcCount = (n + 2) <= 128 ? 1 : 2;
    f[0] = (uint8_t)(n + 2 * crcCount - 1);
    if (crcCount == 1) {
      body = f;
      uint16_t c = crc16(f.data(), n);
      body.push_back(c >> 8); body.push_back(c & 0xFF);
    } else {
      body.assign(f.begin(), f.begin() + 126);
      uint16_t c = crc16(f.data(), 126);
      body.push_back(c >> 8); body.push_back(c & 0xFF);
      body.insert(body.end(), f.begin() + 126, f.end());
      c = crc16(f.data() + 126, n - 126);
      body.push_back(c >> 8); body.push_back(c & 0xFF);
    }
  }
  std::vector<uint8_t> raw = {0x54, (uint8_t)(formatB ? 0x3D : 0xCD)};
  raw.insert(raw.end(), body.begin(), body.end());
  while (raw.size() < 255) raw.push_back(rand());
  return raw;
}

static int c1Tests() {
  int fails = 0;
  // Kamstrup-style header: L C M(KAM=0x2C2D) A(id 12345678, ver 1B, type 16 cold water) CI + payload
  std::vector<uint8_t> f = hex("2344" "2D2C" "78563412" "1B16" "8D2000112233445566778899AABBCCDDEEFF0011223344");
  f[0] = (uint8_t)(f.size() - 1);
  for (bool b : {false, true}) {
    for (size_t extra : {0, 140}) {  // 140 extra bytes forces the 2-CRC format B path
      auto g = f;
      for (size_t i = 0; i < extra; i++) g.push_back(i);
      g[0] = (uint8_t)(g.size() - 1);
      if (!b && extra) continue;  // format A long frame would exceed 255 raw bytes budget test-wise; skip
      auto raw = c1Air(g, b);
      Telegram t;
      Mode m;
      Result r = decode(raw.data(), raw.size(), t, m);
      const char *sn, *ln;
      deviceType(t.type(), sn, ln);
      char mf[4];
      t.mfct(mf);
      bool ok = r == Result::OK && t.id() == 0x12345678 && t.type() == 0x16 && !strcmp(mf, "KAM") &&
                m == (b ? Mode::C1B : Mode::C1A) && t.len == g.size() &&
                !memcmp(t.data + 1, g.data() + 1, g.size() - 1);
      printf("C1 %s len=%zu res=%s mode=%s id=%08x %s %s %s\n", b ? "B" : "A", g.size(), resultStr(r), modeStr(m),
             t.id(), mf, ln, ok ? "PASS" : "FAIL");
      fails += !ok;
      raw[20] ^= 1;
      if (decode(raw.data(), raw.size(), t, m) == Result::OK) { printf("  corruption not detected!\n"); fails++; }
    }
  }
  return fails;
}

int main() {
  uint8_t chk[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
  printf("crc check 0x%04X (expect 0xC2B7)\n", crc16(chk, 9));
  // serial: wmbusmeters' prefix + serial_number ("" = it gives none: not a SAP frame)
  struct { const char *hex; uint32_t id; uint32_t litres, billing, billingDate, battHalfYears, period; const char *serial; } v[] = {
      {"1944304C72242421D401A2013D4013DD8B46A4999C1293E582CC", 0x21242472, 3488, 3486, 20190930, 29, 8, "C19UA145842"},
      {"2944A511780729662366A20118001378D3B3DB8CEDD77731F25832AAF3DA8CADF9774EA673172E8C61F2", 0x66236629, 16760, 11840, 20191130, 24, 8, ""},
      {"1944A511780779194820A121170013355F8EDB2D03C6912B1E37", 0x20481979, 4366, 0, 20201231, 23, 8, ""},
      // same frame with the billing date's year zeroed: no billing date reached yet
      {"1944A511780779194820A121170013355F8EDB2D03C6912B9E17", 0x20481979, 4366, 0, 0, 23, 8, ""},
      {"1944304c9c5824210c04a363140013716577ec59e8663ab0d31c", 0x2124589c, 38944, 38691, 20210201, 20, 32, "H19CA159196"},
  };
  int fails = 0;
  for (auto &t : v) {
    auto f = hex(t.hex);
    auto raw = onAir(f);
    Telegram tg;
    Mode mode;
    Result r = decode(raw.data(), raw.size(), tg, mode);
    uint32_t l = 0;
    bool ok = r == Result::OK && mode == Mode::T1 && tg.len == f.size() && decodeIzar(tg, l) && l == t.litres && tg.id() == t.id;
    char m[4];
    tg.mfct(m);
    printf("%s id=%08x mfct=%s res=%s litres=%u %s\n", t.hex, tg.id(), m, resultStr(r), l, ok ? "PASS" : "FAIL");
    char at[40];
    wmbus::alarmText(izarAlarms(tg), at, sizeof(at));
    printf("  alarms: %s\n", at);
    uint32_t lm = 0, lmDate = 0;
    decodeIzar(tg, l, &lm, &lmDate);
    bool extraOk = lm == t.billing && lmDate == t.billingDate && izarBatteryHalfYears(tg) == t.battHalfYears &&
                   izarPeriodS(tg) == t.period;
    printf("  billing: %u l on %u  battery: %.1f y  period: %u s %s\n", lm, lmDate,
           izarBatteryHalfYears(tg) / 2.0, izarPeriodS(tg), extraOk ? "PASS" : "FAIL");
    fails += !extraOk;
    char serial[12] = "";
    izarSerial(m, tg.id(), tg.version(), tg.type(), serial);
    bool serialOk = !strcmp(serial, t.serial);
    printf("  serial: %s %s\n", serial, serialOk ? "PASS" : "FAIL");
    fails += !serialOk;
    fails += !ok;
    raw[3] ^= 0x10;  // corrupt -> must not decode OK
    Result r2 = decode(raw.data(), raw.size(), tg, mode);
    if (r2 == Result::OK) { printf("  corruption not detected!\n"); fails++; }
  }
  {
    // A label read off a meter: H25XA036488 is broadcast as 217e06c8 (bytes 8-9 as wmbusmeters reads them).
    char serial[12];
    bool ok = izarSerial("SAP", 0x217E06C8, 0x60, 0x04, serial) && !strcmp(serial, "H25XA036488") &&
              !izarSerial("DME", 0x217E06C8, 0x60, 0x04, serial);
    printf("serial H25XA036488 -> %s %s\n", serial, ok ? "PASS" : "FAIL");
    fails += !ok;
  }
  fails += c1Tests();
  {
    // C1 format B, L=128 (129 bytes): first CRC valid, second block empty. Must be rejected, not overrun.
    std::vector<uint8_t> raw(255, 0xAA);
    raw[0] = 0x54; raw[1] = 0x3D; raw[2] = 128;
    uint16_t c = crc16(&raw[2], 126);
    raw[2 + 126] = c >> 8; raw[2 + 127] = c & 0xFF;
    Telegram tg;
    Mode mode;
    Result r = decode(raw.data(), raw.size(), tg, mode);
    bool ok = r != Result::OK;
    printf("C1 B len=129 edge res=%s %s\n", resultStr(r), ok ? "PASS" : "FAIL");
    fails += !ok;
  }
  // Sensus iPERL default-key decrypt test using real telegram from wmbusmeters#878
  // Telegram (CRCs stripped): 1E44AE4C6478842068077A89001005C6FD4FAA63F85837E5CC1B3EC6D0DB3B
  {
    auto raw = c1Air(hex("1E44AE4C6478842068077A89001005C6FD4FAA63F85837E5CC1B3EC6D0DB3B"), false);
    Telegram t; Mode m; Result r = decode(raw.data(), raw.size(), t, m);
    uint32_t l = 0;
    bool ok = r == Result::OK && t.id() == 0x20847864 && decodeSensus(t, l) && l == 623823;
    char mf[4]; t.mfct(mf);
    printf("Sensus iPERL id=%08x %s litres=%u %s\n", t.id(), mf, l, ok ? "PASS" : "FAIL");
    fails += !ok;
  }
  printf("%s\n", fails ? "FAILURES" : "ALL PASS");
  return fails;
}
