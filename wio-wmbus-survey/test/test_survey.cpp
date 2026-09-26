// Host-side test of the survey storage and CSV code:
//   g++ -std=c++17 -Wall -Isrc test/test_survey.cpp -o ts && ./ts
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include "snapring.h"
#include "survey.h"
using namespace survey;

static int fails = 0;
#define CHECK(cond)                                              \
  do {                                                           \
    if (!(cond)) {                                               \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
      fails++;                                                   \
    }                                                            \
  } while (0)

// ---------- fake flash ----------
// 512-byte sectors in RAM. powerCutAfter >= 0: that many more sector writes land, then the
// power "goes" and every later write is lost (the device would have stopped).
struct FakeFlash {
  std::vector<uint8_t> mem;
  long powerCutAfter = -1;
  explicit FakeFlash(size_t sectors) : mem(sectors * 512, 0xFF) {}
  bool readSectors(uint32_t s, uint8_t *dst, size_t n) {
    if ((s + n) * 512 > mem.size()) return false;
    memcpy(dst, &mem[s * 512], n * 512);
    return true;
  }
  bool writeSectors(uint32_t s, const uint8_t *src, size_t n) {
    if ((s + n) * 512 > mem.size()) return false;
    for (size_t i = 0; i < n; i++) {
      if (powerCutAfter == 0) return true;
      if (powerCutAfter > 0) powerCutAfter--;
      memcpy(&mem[(s + i) * 512], src + i * 512, 512);
    }
    return true;
  }
  bool syncBlocks() { return true; }
};

// Survey-sized state: up to 512 132-byte items plus two 4 KB blobs, like main.cpp.
struct State {
  std::vector<uint8_t> items = std::vector<uint8_t>(512 * 132);
  std::vector<uint8_t> a = std::vector<uint8_t>(4096), b = std::vector<uint8_t>(4096);
  uint16_t count = 0, aLen = 0, bLen = 0;
  snapring::Contents contents() {
    return {items.data(), 132, count, 512, a.data(), aLen, 4096, b.data(), bLen, 4096};
  }
  // Deterministic contents for snapshot number n.
  void fill(unsigned n) {
    count = (n * 37) % 513;
    aLen = (n * 911) % 4097;
    bLen = (n * 353) % 4097;
    for (size_t i = 0; i < items.size(); i++) items[i] = (uint8_t)(n * 7 + i);
    for (size_t i = 0; i < a.size(); i++) a[i] = (uint8_t)(n + i * 3);
    for (size_t i = 0; i < b.size(); i++) b[i] = (uint8_t)(n * 5 + i);
  }
  bool same(const State &o) const {
    return count == o.count && aLen == o.aLen && bLen == o.bLen &&
           !memcmp(items.data(), o.items.data(), count * 132) && !memcmp(a.data(), o.a.data(), aLen) &&
           !memcmp(b.data(), o.b.data(), bLen);
  }
};

const uint16_t VER = 3;
const uint32_t BASE = 64, BLOCKS = 48;  // RING_MIN_BLOCKS, the smallest ring main.cpp accepts

// Power on: adopt the ring and load the newest snapshot.
static int reboot(FakeFlash &fl, State &got, uint32_t *seq = nullptr, std::vector<uint32_t> *damaged = nullptr) {
  snapring::Ring<FakeFlash> r(fl);
  if (!r.begin(BASE, BLOCKS, false, 999)) return -2;
  snapring::Contents c = got.contents();
  int n = r.load(VER, c, [&](uint32_t s) { if (damaged) damaged->push_back(s); });
  got.count = c.count;
  got.aLen = c.aLen;
  got.bLen = c.bLen;
  if (seq) *seq = r.seq;
  return n;
}

static void testRing() {
  FakeFlash fl(BASE + BLOCKS * 8);
  snapring::Ring<FakeFlash> r(fl);
  CHECK(r.begin(BASE, BLOCKS, true, 1234));
  State got;
  CHECK(reboot(fl, got) == -1);  // new ring, nothing saved

  // Save through several wraps; after each, a reboot sees exactly the last one.
  State want;
  for (unsigned n = 1; n <= 120; n++) {
    want.fill(n);
    snapring::Contents c = want.contents();
    CHECK(r.save(VER, c));
    if (n % 7 == 0 || n < 4) {
      uint32_t seq;
      CHECK(reboot(fl, got, &seq) == want.count);
      CHECK(got.same(want));
      CHECK(seq == n);
    }
  }
  printf("ring: 120 saves through %u blocks ok\n", BLOCKS);

  // Power cut at every point of a save: the reboot finds the new snapshot or the previous
  // one, never anything else, and saving carries on from there.
  State prev = want;
  unsigned n = 121, cuts = 0;
  for (long cut = 0; cut < 160; cut += 3, n++) {
    snapring::Ring<FakeFlash> dev(fl);
    CHECK(dev.begin(BASE, BLOCKS, false, 0));
    snapring::Contents lc = got.contents();
    CHECK(dev.load(VER, lc) >= 0);
    want.fill(n);
    fl.powerCutAfter = cut;
    snapring::Contents c = want.contents();
    dev.save(VER, c);
    bool cutShort = fl.powerCutAfter == 0;  // (or the cut fell exactly after the last write)
    fl.powerCutAfter = -1;
    CHECK(reboot(fl, got) >= 0);
    CHECK(got.same(want) || (cutShort && got.same(prev)));
    if (!got.same(want)) cuts++;
    prev = got;
  }
  printf("ring: power cuts ok (%u fell back to the previous snapshot)\n", cuts);
  CHECK(cuts > 0);

  // A damaged newest snapshot is reported and the one before it loaded.
  snapring::Ring<FakeFlash> dev(fl);
  CHECK(dev.begin(BASE, BLOCKS, false, 0));
  snapring::Contents lc = got.contents();
  dev.load(VER, lc);
  State older;
  older.fill(500);
  snapring::Contents oc = older.contents();
  CHECK(dev.save(VER, oc));
  State newest;
  newest.fill(501);
  newest.count = 10;  // keep it inside one block so the byte below is in its payload
  snapring::Contents nc = newest.contents();
  CHECK(dev.save(VER, nc));
  uint32_t newestBlock = dev.next - 1;
  fl.mem[(BASE + newestBlock * 8) * 512 + 100] ^= 0x01;
  std::vector<uint32_t> damaged;
  CHECK(reboot(fl, got, nullptr, &damaged) == older.count);
  CHECK(got.same(older));
  CHECK(damaged.size() == 1 && damaged[0] == dev.seq);
  printf("ring: damaged snapshot skipped ok\n");

  // A different survey version is not loaded (main.cpp then tries its older formats).
  {
    snapring::Ring<FakeFlash> r2(fl);
    CHECK(r2.begin(BASE, BLOCKS, false, 0));
    snapring::Contents c = got.contents();
    CHECK(r2.load(VER + 1, c) == -1);
  }

  // A new ring ID (after FORMAT) makes every old snapshot invisible.
  {
    snapring::Ring<FakeFlash> r2(fl);
    CHECK(r2.begin(BASE, BLOCKS, true, 777));
    CHECK(reboot(fl, got) == -1);
    CHECK(r2.id == 777);
  }
  printf("ring: version and ring ID checks ok\n");
}

// ---------- time ----------
static void testTime() {
  CHECK(epochFromCivil(2026, 9, 23, 14, 5, 12) == 1790172312UL);
  CHECK(epochFromCivil(2024, 2, 29, 23, 59, 59) == 1709251199UL);
  CHECK(epochFromCivil(2079, 12, 31, 0, 0, 0) == 3471206400UL);
  char ts[48];
  isoUtc(1790172312UL, ts, sizeof(ts));
  CHECK(std::string(ts) == "2026-09-23T14:05:12Z");
  formatUtc(1790172312UL, ts, sizeof(ts));
  CHECK(std::string(ts) == "14:05 23/09 UTC");
  // Every day in the range the GPS clock is trusted for round-trips.
  int bad = 0;
  for (uint32_t t = epochFromCivil(2024, 1, 1, 0, 0, 0); t <= 3471206400UL; t += 86400) {
    unsigned y, m, d;
    civilDate(t, y, m, d);
    if (epochFromCivil(y, m, d, 0, 0, 0) != t) bad++;
  }
  CHECK(bad == 0);
  printf("time ok\n");
}

// ---------- position ----------
static void testPosition() {
  Meter m = {};
  // Four equal receptions 10 m north/south/east/west of a point: centroid there, spread 10 m.
  const int32_t lat0 = 515000000, lon0 = -1000000;  // 51.5, -0.1
  const double dLat = 10 / 0.011132, dLon = dLat / cos(51.5 * M_PI / 180);
  addSample(m, lat0 + (int32_t)dLat, lon0, -80);
  addSample(m, lat0 - (int32_t)dLat, lon0, -80);
  addSample(m, lat0, lon0 + (int32_t)dLon, -80);
  addSample(m, lat0, lon0 - (int32_t)dLon, -80);
  double lat, lon;
  float spread;
  CHECK(estimatePosition(m, lat, lon, spread));
  CHECK(fabs(lat - 51.5) < 1e-6 && fabs(lon + 0.1) < 1e-6);
  CHECK(fabs(spread - 10) < 0.05);

  // 10 dB stronger weighs 10x.
  Meter w = {};
  addSample(w, lat0, lon0, -60);
  addSample(w, lat0 + 11000, lon0, -70);
  CHECK(estimatePosition(w, lat, lon, spread));
  CHECK(fabs(lat - (lat0 + 1000) / 1e7) < 1e-7);

  // Only the strongest MAX_SAMPLES are kept.
  Meter k = {};
  for (int i = 0; i < 8; i++) addSample(k, lat0, lon0, -90 + i);
  CHECK(!addSample(k, lat0, lon0, -100));
  int16_t weakest = 0;
  for (int i = 0; i < k.nSamples; i++) weakest = i ? std::min(weakest, k.samples[i].rssi) : k.samples[i].rssi;
  CHECK(k.nSamples == MAX_SAMPLES && weakest == -87);
  Meter none = {};
  CHECK(!estimatePosition(none, lat, lon, spread));
  printf("position ok\n");
}

// ---------- CSV ----------
static int columns(const char *row) {
  int n = 1;
  for (const char *p = row; *p; p++) n += *p == ',';
  return n;
}

static Meter izarMeter() {
  Meter m = {};
  m.id = 0x1a2b3c4d;
  memcpy(m.mfct, "SAP", 4);
  m.ver = 0xa0;
  m.type = 0x85;
  m.mode = (uint8_t)wmbus::Mode::T1;
  m.hasLitres = true;
  m.litres = 123456;
  m.alarms = wmbus::ALM_LEAK_NOW | wmbus::ALM_LEAK_PREV;
  m.lastRssi = -71;
  m.bestRssi = -65;
  m.count = 42;
  m.utc = 1790172312UL;
  m.hasIzarInfo = true;
  m.battHalfYears = 19;
  m.periodS = 8;
  m.billingLitres = 120000;
  m.billingDate = 20260901;
  addSample(m, 515000000, -1000000, -65);
  return m;
}

static void testCsv() {
  char row[ROW_MAX];
  Meter iz = izarMeter();
  surveyRow(row, sizeof(row), iz, "12A");
  CHECK(std::string(row) ==
        "1a2b3c4d,12A,K36H@387917,T1,SAP,water (85),a0,123456,LEAK leak(was),-71,-65,42,2026-09-23T14:05:12Z,"
        "51.5000000,-0.1000000,0.0,1,120000,2026-09-01,9.5,8");
  historyRow(row, sizeof(row), iz, "12A", 1790172312UL);
  CHECK(std::string(row) == "2026-09-23T14:05:12Z,1a2b3c4d,12A,SAP,water,123456,120000,2026-09-01,LEAK leak(was),9.5,-71,K36H@387917");

  // Every kind of meter gives rows with the header's columns.
  Meter kinds[4] = {iz, iz, {}, {}};
  kinds[1].billingDate = 0;  // IZAR frame too short for the billing reading
  kinds[1].nSamples = 0;
  kinds[2].id = 0x12345678;
  memcpy(kinds[2].mfct, "KAM", 4);
  kinds[2].type = 0x16;
  kinds[2].mode = (uint8_t)wmbus::Mode::C1B;
  kinds[3] = kinds[2];
  kinds[3].hasLitres = true;  // e.g. a Sensus: litres and OMS alarms but no IZAR extras
  kinds[3].alarms = wmbus::ALM_POWER_LOW | wmbus::ALM_ERROR;
  wmbus::Telegram t;
  t.len = 20;
  for (size_t i = 0; i < t.len; i++) t.data[i] = (uint8_t)i;
  for (const Meter &m : kinds) {
    for (const char *lbl : {(const char *)nullptr, "7"}) {
      surveyRow(row, sizeof(row), m, lbl);
      CHECK(columns(row) == columns(SURVEY_HEADER));
      historyRow(row, sizeof(row), m, lbl, 1790172312UL);
      CHECK(columns(row) == columns(HISTORY_HEADER));
      rawRow(row, sizeof(row), m, lbl, t, 0);
      CHECK(columns(row) == columns(RAW_HEADER));
    }
  }
  rawRow(row, sizeof(row), kinds[2], nullptr, t, 0);
  CHECK(std::string(row) == ",12345678,,C1b,KAM,cold water (16),0,000102030405060708090a0b0c0d0e0f10111213");

  // Largest raw row fits; a short buffer truncates without overrunning.
  t.len = wmbus::MAX_FRAME;
  size_t n = rawRow(row, sizeof(row), kinds[2], "12345678", t, 1790172312UL);
  CHECK(n < sizeof(row) - 1 && row[n - 1] != ',' && strlen(row) == n);
  char small[40];
  memset(small, 'x', sizeof(small));
  n = rawRow(small, 30, kinds[2], nullptr, t, 0);
  CHECK(n == 29 && strlen(small) == 29 && small[30] == 'x');
  printf("csv ok\n");
}

static void testHistoryDue() {
  Meter m = {};
  m.heardSinceLog = true;
  const uint32_t t0 = 1790172312UL;
  CHECK(historyDue(m, t0));  // never logged
  m.loggedUtc = t0;
  CHECK(!historyDue(m, t0 + LOG_INTERVAL_S - 1));  // same walk
  CHECK(historyDue(m, t0 + LOG_INTERVAL_S));       // next walk
  m.alarms = wmbus::ALM_LEAK_NOW;
  CHECK(historyDue(m, t0 + 60));  // alarm changed mid-walk
  m.heardSinceLog = false;
  CHECK(!historyDue(m, t0 + LOG_INTERVAL_S));  // not heard since the last row
  printf("history timing ok\n");
}

int main() {
  testRing();
  testTime();
  testPosition();
  testCsv();
  testHistoryDue();
  printf(fails ? "%d FAILED\n" : "ALL PASS\n", fails);
  return fails ? 1 : 0;
}
