// Wio Tracker L1 Pro - wM-Bus T1/C1 walk-around survey tool (IZAR readings decoded)
// SX1262 in GFSK, 868.95 MHz, 100 kbps, sync 0x543D, fixed 255-byte capture.

#include <Arduino.h>
#include <Wire.h>
#include <RadioLib.h>
#include <U8g2lib.h>
#include <TinyGPSPlus.h>
#include <InternalFileSystem.h>
#include <Adafruit_SPIFlash.h>
#include <Adafruit_TinyUSB.h>
#include <cmath>
#include "wmbus.h"
#include "fat12.h"

// SdFat also defines a global File, so name the LittleFS one explicitly
using LfsFile = Adafruit_LittleFS_Namespace::File;
using Adafruit_LittleFS_Namespace::FILE_O_READ;
using Adafruit_LittleFS_Namespace::FILE_O_WRITE;

// ---------- config ----------
#define MAX_METERS 512          // survey lives on the 2 MB QSPI flash
#define MAX_LABELS 128          // labels stay in the 28 KB internal flash
#define MAX_SAMPLES 5            // strongest GPS-tagged receptions kept per meter
#define ROWS 6
#define DISPLAY_SH1106 1         // L1 uses an SH1106 (per Zephyr board file); 0 = SSD1306
#define SURVEY_SAVE_MS 30000     // save survey at most this often (only when something worth keeping changed)
#define FALLBACK_SAVE_MS 600000  // without the snapshot ring (internal flash: ~10k erase cycles)
#define LABEL_SAVE_MS 3000       // save labels this long after the last edit
#define FS_MARKER "/wmsurvey"    // flash is formatted once if this is missing
#define STATE_FILE "state.bin"      // QSPI, hidden: ring of survey snapshots (see "snapshot ring")
#define STATE_BYTES (1024 * 1024UL)  // smaller sizes are tried if there's no free 1 MB run
#define RING_MIN_BLOCKS 48           // >= 2 x largest snapshot (512 meters + full logs = 19 blocks) + header
#define SURVEY_FILE "/survey.bin"    // older builds (QSPI or internal flash); fallback if the ring fails
#define SURVEY_CSV "/survey.csv"     // QSPI, written when a computer is plugged in
#define DRIVE_LABEL "WMBUS"
#define LABEL_FILE "/labels.bin"
#define FILE_VERSION_LABELS 2
#define SURVEY_VERSION 3         // 3 added IZAR last-month/battery/period and log state.
                                 // Changing Meter means a new version AND a conversion in loadSnapshot(),
                                 // or the saved survey is dropped on upgrade.
#define HISTORY_FILE "/history.csv"  // one row per meter per walk
#define RAW_FILE "/raw.csv"          // raw telegrams of meters whose reading isn't decoded
#define LOG_INTERVAL_S (6 * 3600UL)  // a meter heard again within this counts as the same walk

// ---------- hardware ----------
SX1262 radio = new Module(SX126X_CS, SX126X_DIO1, SX126X_RESET, SX126X_BUSY);
#if DISPLAY_SH1106
U8G2_SH1106_128X64_NONAME_F_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE);
#else
U8G2_SSD1306_128X64_NONAME_F_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE);
#endif
TinyGPSPlus gps;
Adafruit_FlashTransport_QSPI flashTransport;  // pins from variant.h

// The USB task reads the flash for the computer while the main loop writes survey snapshots,
// so every sector access goes through one mutex.
SemaphoreHandle_t flashMutex = nullptr;
struct FlashLock {
  FlashLock() { if (flashMutex) xSemaphoreTake(flashMutex, portMAX_DELAY); }
  ~FlashLock() { if (flashMutex) xSemaphoreGive(flashMutex); }
};
class LockedFlash : public Adafruit_SPIFlash {
 public:
  using Adafruit_SPIFlash::Adafruit_SPIFlash;
  bool readSector(uint32_t block, uint8_t *dst) override {
    FlashLock l;
    return Adafruit_SPIFlash::readSector(block, dst);
  }
  bool writeSector(uint32_t block, const uint8_t *src) override {
    FlashLock l;
    return Adafruit_SPIFlash::writeSector(block, src);
  }
  bool readSectors(uint32_t block, uint8_t *dst, size_t ns) override {
    FlashLock l;
    return Adafruit_SPIFlash::readSectors(block, dst, ns);
  }
  bool writeSectors(uint32_t block, const uint8_t *src, size_t ns) override {
    FlashLock l;
    return Adafruit_SPIFlash::writeSectors(block, src, ns);
  }
  bool syncDevice() override {
    FlashLock l;
    return Adafruit_SPIFlash::syncDevice();
  }
};
LockedFlash flash(&flashTransport);

// JEDEC ID straight from the chip (the library reports ffffff for any chip it doesn't know).
// 856015 = P25Q16H; 000000/ffffff = nothing answering.
uint32_t chipJedecId() {
  FlashLock l;
  uint8_t id[3] = {0, 0, 0};
  flashTransport.readCommand(0x9F, id, 3);
  return (uint32_t)id[0] << 16 | id[1] << 8 | id[2];
}
FatVolume fatfs;
Adafruit_USBD_MSC usbDrive;

// ---------- state ----------
struct Sample {
  int32_t lat, lon;  // degrees * 1e7
  int16_t rssi;
};

struct Meter {
  uint32_t id;
  char mfct[4];
  uint8_t ver, type;
  uint8_t mode;  // wmbus::Mode
  uint16_t alarms;
  uint32_t litres;
  bool hasLitres;
  int16_t lastRssi, bestRssi;
  uint16_t count;
  uint32_t utc;  // last heard, seconds since 1970 from GPS (0 = unknown)
  uint8_t nSamples;
  Sample samples[MAX_SAMPLES];
  // IZAR extras (hasIzarInfo)
  bool hasIzarInfo;
  uint8_t battHalfYears;     // remaining battery life, half years
  uint32_t periodS;          // transmit interval
  uint32_t lastMonthLitres;  // reading at the start of the month (0 = unknown)
  uint32_t lastMonthDate;    // YYYYMMDD of that reading (0 = unknown)
  // log state, saved so a reboot mid-walk doesn't log meters twice
  uint32_t loggedUtc;        // last history.csv row
  uint16_t loggedAlarms;     // alarms in that row
  uint32_t rawLoggedUtc;     // last raw.csv row
  // runtime only (reset on load)
  uint32_t lastSeen;
  bool thisSession;
  bool heardSinceLog;
  bool rawThisSession;
};

// Survey format 2 (previous builds), converted on load.
struct MeterV2 {
  uint32_t id;
  char mfct[4];
  uint8_t ver, type;
  uint8_t mode;
  uint16_t alarms;
  uint32_t litres;
  bool hasLitres;
  int16_t lastRssi, bestRssi;
  uint16_t count;
  uint32_t utc;
  uint8_t nSamples;
  Sample samples[MAX_SAMPLES];
  uint32_t lastSeen;
  bool thisSession;
};

// House label and/or AES key for one meter. An entry can hold just a key (empty text).
struct Label {
  uint32_t id;
  char text[8];
  uint8_t key[16];  // all-zero = try Sensus default if applicable
};

bool hasKey(const Label &l) {
  for (uint8_t b : l.key)
    if (b) return true;
  return false;
}

Meter meters[MAX_METERS];
int meterCount = 0;
int order[MAX_METERS];
Label labels[MAX_LABELS];
int labelCount = 0;
int lastLabelNum = 0;  // starting point for labelling the next house

int sel = 0, top = 0;
uint32_t selId = 0;  // selection follows the meter, not the row, when the list re-sorts
bool detail = false;
bool sortByRssi = true;
uint32_t okCount = 0, errCount = 0;
bool surveyDirty = false, labelsDirty = false;
uint32_t lastSurveySave = 0, lastLabelEdit = 0;
bool fsOk = false;    // internal flash (labels)
bool qspiOk = false;  // QSPI flash (survey + USB drive)
// While a computer has the USB drive we must not write to the QSPI filesystem, or its cached
// view goes stale. Survey saves wait until it is unplugged; the data stays in RAM meanwhile.
bool driveToHost = false;

volatile bool rxFlag = false;
uint8_t raw[255];

void onRx() { rxFlag = true; }

// ---------- helpers ----------
float batteryVolts() { return analogRead(PIN_VBAT) * AREF_VOLTAGE / 4095.0f * ADC_MULTIPLIER; }

void beep(uint16_t freq, uint16_t ms) { tone(PIN_BUZZER, freq, ms); }

// Beep sequences play from loop(), so the radio keeps being serviced while they sound
// (tone() itself doesn't block).
struct BeepSeq {
  uint8_t left;
  uint16_t freq, ms, gapMs;
  uint32_t next;
} beepSeq = {};

void beepRepeat(uint8_t n, uint16_t freq, uint16_t ms, uint16_t gapMs) {
  beepSeq = {n, freq, ms, gapMs, millis()};
}

void handleBeeps() {
  if (!beepSeq.left || (int32_t)(millis() - beepSeq.next) < 0) return;
  beep(beepSeq.freq, beepSeq.ms);
  beepSeq.left--;
  beepSeq.next = millis() + beepSeq.ms + beepSeq.gapMs;
}

// Diehl PRIOS frames don't carry a standard type byte, so decoded IZAR meters are "water".
void typeNames(const Meter &m, const char *&shortName, const char *&longName) {
  if (m.hasLitres) {
    shortName = longName = "water";
    return;
  }
  wmbus::deviceType(m.type, shortName, longName);
}

const char *modeName(const Meter &m) { return wmbus::modeStr((wmbus::Mode)m.mode); }

// Before its first fix the GPS reports its own clock, which can be anything (TinyGPSPlus
// accepts it), so time is only trusted once a position fix has been seen.
bool gpsTimeTrusted = false;

uint32_t gpsEpoch() {
  if (!gpsTimeTrusted) {
    if (!gps.location.isValid()) return 0;
    gpsTimeTrusted = true;
  }
  if (!gps.date.isValid() || !gps.time.isValid() || gps.date.year() < 2024 || gps.date.year() > 2079) return 0;
  // days from civil (Howard Hinnant)
  int y = gps.date.year(), m = gps.date.month(), d = gps.date.day();
  y -= m <= 2;
  int era = y / 400;
  unsigned yoe = y - era * 400;
  unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  long days = era * 146097L + doe - 719468L;
  return days * 86400UL + gps.time.hour() * 3600UL + gps.time.minute() * 60UL + gps.time.second();
}

void civilDate(uint32_t t, unsigned &y, unsigned &m, unsigned &d) {
  long days = t / 86400 + 719468;  // civil from days (Howard Hinnant)
  long era = days / 146097;
  unsigned doe = days - era * 146097;
  unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  unsigned mp = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  m = mp < 10 ? mp + 3 : mp - 9;
  y = yoe + era * 400 + (m <= 2);
}

void formatUtc(uint32_t t, char *out, size_t n) {
  unsigned y, m, d;
  civilDate(t, y, m, d);
  uint32_t secs = t % 86400;
  snprintf(out, n, "%02lu:%02lu %02u/%02u UTC", (unsigned long)(secs / 3600), (unsigned long)(secs / 60 % 60), d, m);
}

// 2026-09-23T14:05:12Z
void isoUtc(uint32_t t, char *out, size_t n) {
  unsigned y, m, d;
  civilDate(t, y, m, d);
  uint32_t secs = t % 86400;
  snprintf(out, n, "%04u-%02u-%02uT%02lu:%02lu:%02luZ", y, m, d, (unsigned long)(secs / 3600),
           (unsigned long)(secs / 60 % 60), (unsigned long)(secs % 60));
}

// ---------- labels ----------
Label *findLabel(uint32_t id) {
  for (int i = 0; i < labelCount; i++)
    if (labels[i].id == id) return &labels[i];
  return nullptr;
}

// House label text, or nullptr if the meter has none (a key-only entry has none).
const char *getLabel(uint32_t id) {
  const Label *l = findLabel(id);
  return l && l->text[0] ? l->text : nullptr;
}

// Entry for this meter, created empty if needed. nullptr if the table is full.
Label *labelEntry(uint32_t id) {
  if (Label *l = findLabel(id)) return l;
  if (labelCount >= MAX_LABELS) {
    Serial.println("# label table full");
    return nullptr;
  }
  Label *l = &labels[labelCount++];
  memset(l, 0, sizeof(*l));
  l->id = id;
  return l;
}

// Drop an entry once it holds neither text nor key.
void pruneLabel(Label *l) {
  if (!l->text[0] && !hasKey(*l)) *l = labels[--labelCount];
}

// Empty/null text removes the label; a key stored for the meter is kept.
void setLabel(uint32_t id, const char *text) {
  if (!text || !*text) {
    Label *l = findLabel(id);
    if (!l) return;
    l->text[0] = 0;
    pruneLabel(l);
  } else {
    Label *l = labelEntry(id);
    if (!l) return;
    strncpy(l->text, text, sizeof(l->text) - 1);
    l->text[sizeof(l->text) - 1] = 0;
    for (char *p = l->text; *p; p++)
      if (*p == ',' || *p == '"') *p = ';';  // keep the CSVs parseable
    int n = atoi(text);
    if (n > 0) lastLabelNum = n;
  }
  labelsDirty = true;
  lastLabelEdit = millis();
}

// Joystick left/right: step the house number. Unlabelled meters start next to the last one used.
void stepLabel(uint32_t id, int dir) {
  const char *cur = getLabel(id);
  int n = cur ? atoi(cur) + dir : lastLabelNum + dir;
  if (n < 0) n = 0;
  if (n > 9999) n = 9999;
  char buf[8];
  if (n == 0) buf[0] = 0;
  else snprintf(buf, sizeof(buf), "%d", n);
  setLabel(id, buf);
  if (n > 0) lastLabelNum = n;
}

// ---------- position estimate ----------
// true if the sample was kept
bool addSample(Meter &m, int16_t rssi) {
  if (!gps.location.isValid() || gps.location.age() > 5000) return false;
  Sample s = {(int32_t)lround(gps.location.lat() * 1e7), (int32_t)lround(gps.location.lng() * 1e7), rssi};
  if (m.nSamples < MAX_SAMPLES) {
    m.samples[m.nSamples++] = s;
    return true;
  }
  int weakest = 0;
  for (int i = 1; i < MAX_SAMPLES; i++)
    if (m.samples[i].rssi < m.samples[weakest].rssi) weakest = i;
  if (rssi <= m.samples[weakest].rssi) return false;
  m.samples[weakest] = s;
  return true;
}

// Weighted centroid of the strongest receptions (weight = linear power relative to best).
// spread = weighted RMS distance of samples from the centroid, in metres.
bool estimatePosition(const Meter &m, double &lat, double &lon, float &spread) {
  if (!m.nSamples) return false;
  int16_t best = -32768;
  for (int i = 0; i < m.nSamples; i++) if (m.samples[i].rssi > best) best = m.samples[i].rssi;
  const Sample &ref = m.samples[0];
  double sw = 0, dy = 0, dx = 0;
  double w[MAX_SAMPLES];
  for (int i = 0; i < m.nSamples; i++) {
    w[i] = pow(10.0, (m.samples[i].rssi - best) / 10.0);
    sw += w[i];
    dy += w[i] * (m.samples[i].lat - ref.lat);
    dx += w[i] * (m.samples[i].lon - ref.lon);
  }
  lat = (ref.lat + dy / sw) / 1e7;
  lon = (ref.lon + dx / sw) / 1e7;
  const double mPerE7 = 0.011132;  // metres per 1e-7 degree of latitude
  const double cosLat = cos(lat * M_PI / 180.0);
  double var = 0;
  for (int i = 0; i < m.nSamples; i++) {
    double ny = (m.samples[i].lat / 1e7 - lat) * 1e7 * mPerE7;
    double nx = (m.samples[i].lon / 1e7 - lon) * 1e7 * mPerE7 * cosLat;
    var += w[i] * (nx * nx + ny * ny);
  }
  spread = sqrt(var / sw);
  return true;
}

// ---------- persistence ----------
struct FileHeader {
  uint32_t magic;
  uint16_t version;
  uint16_t count;
};

template <typename F>
bool writeRecords(F &f, uint32_t magic, uint16_t version, const void *data, size_t itemSize, int count) {
  FileHeader h = {magic, version, (uint16_t)count};
  bool ok = f.write((const uint8_t *)&h, sizeof(h)) == sizeof(h);
  return ok && f.write((const uint8_t *)data, itemSize * count) == itemSize * count;
}

template <typename F>
int readRecords(F &f, uint32_t magic, uint16_t version, void *data, size_t itemSize, int maxCount) {
  FileHeader h;
  if (f.read(&h, sizeof(h)) == sizeof(h) && h.magic == magic && h.version == version &&
      h.count <= maxCount && f.size() == sizeof(h) + itemSize * h.count &&
      f.read(data, itemSize * h.count) == (int)(itemSize * h.count))
    return h.count;
  return 0;
}

// Internal flash. Written to a temp file and renamed so a power cut keeps the old copy.
bool saveFile(const char *path, uint32_t magic, uint16_t version, const void *data, size_t itemSize, int count) {
  if (!fsOk) return false;
  const char *tmp = "/tmp.bin";
  InternalFS.remove(tmp);
  LfsFile f(InternalFS);
  if (!f.open(tmp, FILE_O_WRITE)) return false;
  bool ok = writeRecords(f, magic, version, data, itemSize, count);
  f.close();
  if (!ok) {
    InternalFS.remove(tmp);
    return false;
  }
  InternalFS.remove(path);
  return InternalFS.rename(tmp, path);
}

int loadFile(const char *path, uint32_t magic, uint16_t version, void *data, size_t itemSize, int maxCount) {
  if (!fsOk || !InternalFS.exists(path)) return 0;
  LfsFile f(InternalFS);
  if (!f.open(path, FILE_O_READ)) return 0;
  int n = readRecords(f, magic, version, data, itemSize, maxCount);
  f.close();
  return n;
}

const uint32_t MAGIC_SURVEY = 0x53525659;  // "SRVY"
const uint32_t MAGIC_LABELS = 0x4C41424C;  // "LABL"

// Current format, or format 2 converted field by field.
template <typename F>
int readSurvey(F &f) {
  FileHeader h;
  if (f.read(&h, sizeof(h)) != sizeof(h) || h.magic != MAGIC_SURVEY || h.count > MAX_METERS) return 0;
  if (h.version == SURVEY_VERSION && f.size() == sizeof(h) + sizeof(Meter) * h.count)
    return f.read(meters, sizeof(Meter) * h.count) == (int)(sizeof(Meter) * h.count) ? h.count : 0;
  if (h.version != 2 || f.size() != sizeof(h) + sizeof(MeterV2) * h.count) return 0;
  for (int i = 0; i < h.count; i++) {
    MeterV2 o;
    if (f.read(&o, sizeof(o)) != sizeof(o)) return 0;
    Meter &m = meters[i];
    memset(&m, 0, sizeof(m));
    m.id = o.id;
    memcpy(m.mfct, o.mfct, sizeof(m.mfct));
    m.ver = o.ver;
    m.type = o.type;
    m.mode = o.mode;
    m.alarms = o.alarms;
    m.litres = o.litres;
    m.hasLitres = o.hasLitres;
    m.lastRssi = o.lastRssi;
    m.bestRssi = o.bestRssi;
    m.count = o.count;
    m.utc = o.utc;
    m.nSamples = o.nSamples;
    memcpy(m.samples, o.samples, sizeof(m.samples));
  }
  Serial.printf("# converted survey from format 2 (%d meters)\n", h.count);
  return h.count;
}

void printCsvHeader(Print &out);
void sortMeters();
void printMeterCsv(Print &out, const Meter &m);

// ---------- history / raw logs ----------
// Rows are collected in RAM and appended to the file with the next survey save (at most every
// 30 s), so the flash sees one write per batch rather than one per row.
struct LogFile {
  const char *path, *header;
  char buf[4096];
  size_t len;
};
LogFile historyLog = {HISTORY_FILE, "utc,id,label,mfct,type,litres,last_month_litres,last_month_date,alarms,battery_years,rssi\n"};
LogFile rawLog = {RAW_FILE, "utc,id,label,mode,mfct,type,rssi,telegram\n"};

bool flushLog(LogFile &lf) {
  if (!lf.len) return true;
  if (!qspiOk || driveToHost) return false;
  bool isNew = !fatfs.exists(lf.path);
  File32 f = fatfs.open(lf.path, O_WRONLY | O_CREAT | O_APPEND);
  if (!f) return false;
  bool ok = !isNew || f.write(lf.header, strlen(lf.header)) == strlen(lf.header);
  ok = ok && f.write(lf.buf, lf.len) == lf.len;
  ok = f.close() && ok;
  if (ok) lf.len = 0;
  else Serial.printf("# %s append FAILED\n", lf.path);
  return ok;
}

// false = buffer full and can't be flushed right now (computer has the drive); try later.
bool logLine(LogFile &lf, const char *line) {
  size_t n = strlen(line);
  if (lf.len + n > sizeof(lf.buf) && !flushLog(lf)) return false;
  if (lf.len + n > sizeof(lf.buf)) return false;
  memcpy(lf.buf + lf.len, line, n);
  lf.len += n;
  return true;
}

// Print the file, then any rows still waiting in RAM.
void printLog(LogFile &lf) {
  File32 f = qspiOk ? fatfs.open(lf.path, O_RDONLY) : File32();
  if (f) {
    uint8_t chunk[256];
    int n;
    while ((n = f.read(chunk, sizeof(chunk))) > 0) Serial.write(chunk, n);
    f.close();
  } else {
    Serial.print(lf.header);
  }
  Serial.write((const uint8_t *)lf.buf, lf.len);
}

// ---------- snapshot ring ----------
// The survey is saved as snapshots written one after another through a pre-allocated,
// contiguous state.bin, wrapping at the end. The FAT tables and directory are never touched by
// a save, and every part of the ring wears evenly (rewriting a FAT file every 30 s would wear
// out the directory's flash block in days of continuous use). A snapshot is only trusted if its
// CRC checks out, so a power cut mid-save falls back to the previous one.
// Ring block 0 holds a random ring ID; snapshots carry it, so stale data left in the flash from
// before a FORMAT is never mistaken for a snapshot.
const uint32_t MAGIC_RING = 0x474E4952;  // "RING"
const uint32_t MAGIC_SNAP = 0x50414E53;  // "SNAP"
struct RingHeader {
  uint32_t magic, ringId;
};
struct SnapHeader {
  uint32_t magic, ringId, seq;
  uint16_t version, count;      // count = meters
  uint16_t historyLen, rawLen;  // log rows not yet appended to the CSV files
  uint32_t crc;                 // CRC-32 of everything after the header
};
bool ringOk = false;
uint32_t ringBase = 0, ringBlocks = 0;  // first sector (4 KB aligned) and size in 4 KB blocks
uint32_t ringId = 0, snapSeq = 0, snapNext = 1;  // next snapshot: sequence number, block

uint32_t crc32(uint32_t crc, const uint8_t *p, size_t n) {
  crc = ~crc;
  while (n--) {
    crc ^= *p++;
    for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320UL & (0 - (crc & 1)));
  }
  return ~crc;
}

uint32_t hwRandom() {
  uint32_t v = 0;
  NRF_RNG->CONFIG = 1;  // bias correction
  NRF_RNG->TASKS_START = 1;
  for (int i = 0; i < 4; i++) {
    NRF_RNG->EVENTS_VALRDY = 0;
    while (!NRF_RNG->EVENTS_VALRDY) {}
    v = v << 8 | NRF_RNG->VALUE;
  }
  NRF_RNG->TASKS_STOP = 1;
  return v;
}

bool writeRingHeader() {
  uint8_t sec[512];
  memset(sec, 0xFF, sizeof(sec));
  ringId = hwRandom();
  RingHeader rh = {MAGIC_RING, ringId};
  memcpy(sec, &rh, sizeof(rh));
  snapSeq = 0;
  snapNext = 1;
  return flash.writeSectors(ringBase, sec, 1) && flash.syncBlocks();
}

bool ringInit() {
  FatFile root, f;
  if (!root.openRoot(&fatfs)) return false;
  bool created = false;
  uint32_t first = 0, last = 0;
  if (f.open(&root, STATE_FILE, O_RDONLY)) {
    bool usable = f.fileSize() && f.contiguousRange(&first, &last);
    f.close();
    if (!usable) {  // e.g. left empty by an interrupted create: start again
      first = 0;
      root.remove(STATE_FILE);
    }
  }
  if (!first) {
    // Needs one free contiguous run; files from older builds can split the free space.
    for (uint32_t size = STATE_BYTES; size >= 256 * 1024UL && !created; size /= 2) {
      created = f.createContiguous(&root, STATE_FILE, size);
      if (!created) root.remove(STATE_FILE);  // a failed attempt leaves an empty file behind
    }
    if (!created) {
      flash.syncBlocks();
      return false;
    }
    f.attrib(FS_ATTRIB_HIDDEN);
    bool ok = f.contiguousRange(&first, &last);
    f.close();
    if (!ok) return false;
  }
  flash.syncBlocks();
  ringBase = (first + 7) & ~7UL;
  ringBlocks = (last + 1 - ringBase) / 8;
  if (ringBlocks < RING_MIN_BLOCKS) return false;
  RingHeader rh;
  uint8_t sec[512];
  if (!flash.readSectors(ringBase, sec, 1)) return false;
  memcpy(&rh, sec, sizeof(rh));
  if (created || rh.magic != MAGIC_RING) return writeRingHeader();
  ringId = rh.ringId;
  return true;
}

// Reads `n` bytes of snapshot payload, starting `offset` bytes into the snapshot.
bool readSnapBytes(uint32_t block, size_t offset, uint8_t *dst, size_t n, uint32_t &crc) {
  uint8_t sec[512];
  while (n) {
    uint32_t s = ringBase + block * 8 + offset / 512;
    size_t o = offset % 512, k = min(n, sizeof(sec) - o);
    if (!flash.readSectors(s, sec, 1)) return false;
    memcpy(dst, sec + o, k);
    crc = crc32(crc, sec + o, k);
    dst += k;
    offset += k;
    n -= k;
  }
  return true;
}

// Newest valid snapshot into meters[] and the log buffers. -1 if there is none.
int loadSnapshot() {
  static uint16_t cand[256];  // blocks with a plausible header, newest first
  static uint32_t candSeq[256];
  int nc = 0;
  uint8_t sec[512];
  for (uint32_t b = 1; b < ringBlocks && nc < 256; b++) {
    if (!flash.readSectors(ringBase + b * 8, sec, 1)) continue;
    SnapHeader h;
    memcpy(&h, sec, sizeof(h));
    if (h.magic != MAGIC_SNAP || h.ringId != ringId || h.version != SURVEY_VERSION || h.count > MAX_METERS ||
        h.historyLen > sizeof(historyLog.buf) || h.rawLen > sizeof(rawLog.buf))
      continue;
    if (h.seq > snapSeq) snapSeq = h.seq;
    int i = nc++;
    while (i > 0 && candSeq[i - 1] < h.seq) {
      cand[i] = cand[i - 1];
      candSeq[i] = candSeq[i - 1];
      i--;
    }
    cand[i] = b;
    candSeq[i] = h.seq;
  }
  for (int i = 0; i < nc; i++) {
    uint32_t b = cand[i];
    SnapHeader h;
    uint32_t crc = 0, ignore = 0;
    size_t off = sizeof(h);
    if (!readSnapBytes(b, 0, (uint8_t *)&h, sizeof(h), ignore)) continue;
    size_t mb = h.count * sizeof(Meter);
    if (!readSnapBytes(b, off, (uint8_t *)meters, mb, crc) ||
        !readSnapBytes(b, off + mb, (uint8_t *)historyLog.buf, h.historyLen, crc) ||
        !readSnapBytes(b, off + mb + h.historyLen, (uint8_t *)rawLog.buf, h.rawLen, crc) || crc != h.crc) {
      Serial.printf("# snapshot %lu damaged, trying an older one\n", (unsigned long)h.seq);
      continue;
    }
    historyLog.len = h.historyLen;
    rawLog.len = h.rawLen;
    size_t total = sizeof(h) + mb + h.historyLen + h.rawLen;
    snapNext = b + (total + 4095) / 4096;
    return h.count;
  }
  historyLog.len = rawLog.len = 0;
  return -1;
}

bool saveSnapshot() {
  const struct { const uint8_t *p; size_t n; } parts[] = {
      {(const uint8_t *)meters, meterCount * sizeof(Meter)},
      {(const uint8_t *)historyLog.buf, historyLog.len},
      {(const uint8_t *)rawLog.buf, rawLog.len},
  };
  SnapHeader h = {MAGIC_SNAP, ringId, snapSeq + 1, SURVEY_VERSION, (uint16_t)meterCount,
                  (uint16_t)historyLog.len, (uint16_t)rawLog.len, 0};
  size_t total = sizeof(h);
  for (auto &pt : parts) {
    h.crc = crc32(h.crc, pt.p, pt.n);
    total += pt.n;
  }
  uint32_t blocks = (total + 4095) / 4096;
  if (snapNext + blocks > ringBlocks) snapNext = 1;  // wrap (block 0 is the ring header)
  uint8_t sec[512];
  size_t fill = sizeof(h);
  memcpy(sec, &h, sizeof(h));
  uint32_t sector = ringBase + snapNext * 8;
  bool ok = true;
  for (auto &pt : parts) {
    size_t done = 0;
    while (done < pt.n) {
      size_t k = min(pt.n - done, sizeof(sec) - fill);
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
  snapSeq++;
  snapNext += blocks;
  return ok;
}

// survey.csv for the USB drive, strongest meters first.
bool writeSurveyCsv() {
  File32 f = fatfs.open(SURVEY_CSV, O_WRONLY | O_CREAT | O_TRUNC);
  if (!f) return false;
  printCsvHeader(f);
  for (int i = 0; i < meterCount; i++) printMeterCsv(f, meters[order[i]]);
  return f.close();
}

// Fallback when the ring can't be set up: rewrite survey.bin through the FAT (wears the flash
// much faster, see above).
bool saveSurveyQspi() {
  const char *tmp = "/survey.tmp";
  fatfs.remove(tmp);
  File32 f = fatfs.open(tmp, O_WRONLY | O_CREAT | O_TRUNC);
  if (!f) return false;
  bool ok = writeRecords(f, MAGIC_SURVEY, SURVEY_VERSION, meters, sizeof(Meter), meterCount);
  ok = f.close() && ok;
  if (!ok) {
    fatfs.remove(tmp);
    return false;
  }
  fatfs.remove(SURVEY_FILE);
  ok = fatfs.rename(tmp, SURVEY_FILE);
  fatfs.attrib(SURVEY_FILE, FS_ATTRIB_HIDDEN);
  return flash.syncBlocks() && ok;
}

// Waiting log rows are kept in each snapshot, so they only need appending to the CSV files
// (a FAT update, which wears the flash) when a computer is plugged in or a buffer is getting
// full. Never while a computer has the drive: its view of the FAT would go stale.
bool saveSurvey(bool flushLogs = false) {
  auto due = [&](const LogFile &lf) { return flushLogs || !ringOk || lf.len > sizeof(lf.buf) * 3 / 4; };
  if (qspiOk && !driveToHost && (due(historyLog) || due(rawLog))) {
    flushLog(historyLog);
    flushLog(rawLog);
    flash.syncBlocks();
  }
  bool ok;
  if (ringOk) ok = saveSnapshot();
  else if (qspiOk) ok = !driveToHost && saveSurveyQspi();
  else ok = saveFile(SURVEY_FILE, MAGIC_SURVEY, SURVEY_VERSION, meters, sizeof(Meter), meterCount);
  Serial.printf("# survey saved (%d meters) %s\n", meterCount, ok ? "ok" : "FAILED");
  surveyDirty = false;
  lastSurveySave = millis();
  return ok;
}

void saveLabels() {
  bool ok = saveFile(LABEL_FILE, MAGIC_LABELS, FILE_VERSION_LABELS, labels, sizeof(Label), labelCount);
  Serial.printf("# labels saved (%d) %s\n", labelCount, ok ? "ok" : "FAILED");
  labelsDirty = false;
}

void fsInit() {
  fsOk = InternalFS.begin();
  if (fsOk && !InternalFS.exists(FS_MARKER)) {
    // First boot after flashing (e.g. over Meshtastic): wipe its settings to free the 28 KB area.
    Serial.println("# formatting internal flash");
    InternalFS.format();
    fsOk = InternalFS.begin();
    LfsFile f(InternalFS);
    if (fsOk && f.open(FS_MARKER, FILE_O_WRITE)) {
      f.write("1");
      f.close();
    }
  }
  if (!fsOk) Serial.println("# internal flash unavailable - labels will not be saved");

  if (qspiOk && !fatfs.begin(&flash)) {
    // Only format a blank chip. A filesystem that fails to mount may still hold the survey and
    // history, so leave it alone; FORMAT wipes it deliberately.
    uint8_t sec[512];
    bool blank = flash.readSectors(0, sec, 1) && !(sec[510] == 0x55 && sec[511] == 0xAA);
    if (blank) {
      Serial.println("# formatting QSPI flash");
      qspiOk = fat12::format(flash, DRIVE_LABEL, NRF_FICR->DEVICEID[0]) && fatfs.begin(&flash);
    } else {
      Serial.println("# QSPI filesystem won't mount - not formatting it (send FORMAT to wipe)");
      qspiOk = false;
    }
  }
  if (!qspiOk) Serial.println("# QSPI flash unavailable - survey saved to internal flash (limited size)");

  if (qspiOk) {
    ringOk = ringInit();
    if (!ringOk) Serial.println("# state.bin unavailable - saving survey.bin instead (more flash wear)");
  }
  int loaded = ringOk ? loadSnapshot() : -1;
  if (loaded >= 0) meterCount = loaded;
  // No snapshot yet: survey.bin from an older build (QSPI, then internal flash). Moved into the
  // ring once loaded.
  bool legacy = false;
  if (loaded < 0 && qspiOk) {
    File32 f = fatfs.open(SURVEY_FILE, O_RDONLY);
    if (f) {
      meterCount = readSurvey(f);
      f.close();
      legacy = ringOk && meterCount > 0;  // unreadable: leave the file alone
    }
  }
  if (loaded < 0 && !meterCount && fsOk && InternalFS.exists(SURVEY_FILE)) {
    LfsFile f(InternalFS);
    if (f.open(SURVEY_FILE, FILE_O_READ)) {
      meterCount = readSurvey(f);
      f.close();
    }
    legacy = qspiOk && meterCount > 0;
  }
  for (int i = 0; i < meterCount; i++) {
    meters[i].lastSeen = 0;
    meters[i].thisSession = false;
    meters[i].heardSinceLog = false;
    meters[i].rawThisSession = false;
  }
  labelCount = loadFile(LABEL_FILE, MAGIC_LABELS, FILE_VERSION_LABELS, labels, sizeof(Label), MAX_LABELS);
  for (int i = 0; i < labelCount; i++) {
    labels[i].text[sizeof(labels[i].text) - 1] = 0;
    int n = atoi(labels[i].text);
    if (n > lastLabelNum) lastLabelNum = n;
  }
  Serial.printf("# loaded %d meters, %d labels\n", meterCount, labelCount);
  if (legacy) {
    sortMeters();
    if (saveSurvey()) {
      if (ringOk) {
        fatfs.remove(SURVEY_FILE);
        fatfs.remove("/survey.tmp");
        flash.syncBlocks();
      }
      if (fsOk) InternalFS.remove(SURVEY_FILE);
      Serial.println("# survey moved to the new save format");
    }
  }
}

// ---------- USB drive ----------
// The QSPI flash shows up on a computer as a read-only drive holding survey.csv.
int32_t driveRead(uint32_t lba, void *buf, uint32_t size) {
  return flash.readBlocks(lba, (uint8_t *)buf, size / 512) ? (int32_t)size : -1;
}
int32_t driveWrite(uint32_t, uint8_t *, uint32_t) { return -1; }
void driveFlush() {}
bool driveReady() { return driveToHost; }
bool driveWritable() { return false; }

// Added even if the QSPI flash failed: the computer then shows a drive with no media, which
// tells that apart from a firmware without the drive.
void usbDriveInit() {
  usbDrive.setID("Seeed", "wM-Bus survey", "1.0");
  usbDrive.setCapacity(qspiOk ? flash.sectorCount() : 4096, 512);
  usbDrive.setReadWriteCallback(driveRead, driveWrite, driveFlush);
  usbDrive.setReadyCallback(driveReady);
  usbDrive.setWritableCallback(driveWritable);
  usbDrive.setUnitReady(true);
  usbDrive.begin();
  // USB starts before setup(), so the computer may already have read the device description
  // without the drive - even if it hasn't finished connecting yet. Always reconnect.
  TinyUSBDevice.detach();
  delay(20);
  TinyUSBDevice.attach();
}

// Computer attached: write fresh files, then hand the drive over and stop writing to it.
// Unplugged (or charger only): take it back.
void handleUsbDrive() {
  bool host = TinyUSBDevice.mounted();
  if (host && !driveToHost && qspiOk) {
    if (labelsDirty) saveLabels();
    saveSurvey(true);  // also appends waiting log rows
    if (!writeSurveyCsv()) Serial.println("# survey.csv FAILED");
    flash.syncBlocks();
    driveToHost = true;
  } else if (!host && driveToHost) {
    driveToHost = false;
  }
}

// ---------- listing ----------
void sortMeters() {
  for (int i = 0; i < meterCount; i++) order[i] = i;
  for (int i = 1; i < meterCount; i++) {
    int k = order[i], j = i - 1;
    while (j >= 0) {
      const Meter &a = meters[order[j]], &b = meters[k];
      bool swap;
      if (sortByRssi) swap = a.bestRssi < b.bestRssi;
      else if (a.thisSession != b.thisSession) swap = b.thisSession;
      else if (a.thisSession) swap = a.lastSeen < b.lastSeen;
      else swap = a.utc < b.utc;
      if (!swap) break;
      order[j + 1] = order[j];
      j--;
    }
    order[j + 1] = k;
  }
  // keep the same meter selected
  int found = -1;
  for (int i = 0; i < meterCount; i++)
    if (meters[order[i]].id == selId) { found = i; break; }
  if (found >= 0) {
    sel = found;
  } else {
    if (sel >= meterCount) sel = meterCount ? meterCount - 1 : 0;
    selId = meterCount ? meters[order[sel]].id : 0;
  }
}

void printCsvHeader(Print &out) {
  out.println("id,label,mode,mfct,type,ver,litres,alarms,rssi,best_rssi,count,utc,lat,lon,spread_m,samples,"
              "last_month_litres,last_month_date,battery_years,period_s");
}

// last_month_litres,last_month_date,battery_years,period_s (empty when unknown)
void printIzarExtras(Print &out, const Meter &m) {
  if (!m.hasIzarInfo) {
    out.print(",,,");
    return;
  }
  if (m.lastMonthDate)
    out.printf("%lu,%04lu-%02lu-%02lu", (unsigned long)m.lastMonthLitres, (unsigned long)(m.lastMonthDate / 10000),
               (unsigned long)(m.lastMonthDate / 100 % 100), (unsigned long)(m.lastMonthDate % 100));
  else
    out.print(',');
  out.printf(",%.1f,%lu", m.battHalfYears / 2.0, (unsigned long)m.periodS);
}

void printMeterCsv(Print &out, const Meter &m) {
  const char *lbl = getLabel(m.id);
  char at[48] = "";
  const char *sn, *ln;
  typeNames(m, sn, ln);
  out.printf("%08lx,%s,%s,%s,%s (%02x),%02x,", (unsigned long)m.id, lbl ? lbl : "", modeName(m), m.mfct, ln,
                m.type, m.ver);
  if (m.hasLitres) {
    out.print(m.litres);
    wmbus::alarmText(m.alarms, at, sizeof(at));
  }
  char ts[24] = "";
  if (m.utc) isoUtc(m.utc, ts, sizeof(ts));
  out.printf(",%s,%d,%d,%u,%s,", at, m.lastRssi, m.bestRssi, m.count, ts);
  double lat, lon;
  float spread;
  if (estimatePosition(m, lat, lon, spread)) {
    out.print(lat, 7);
    out.print(',');
    out.print(lon, 7);
    out.printf(",%.1f,%u", spread, m.nSamples);
  } else {
    out.print(",,,0");
  }
  out.print(',');
  printIzarExtras(out, m);
  out.println();
}

// ---------- radio ----------
void radioInit() {
  int st = radio.beginFSK(868.95, 100.0, 50.0, 234.3, 10, 8, SX126X_DIO3_TCXO_VOLTAGE, false);
  // preamble length 8 -> 8-bit preamble detector. T1 only guarantees a 38-chip preamble,
  // and detector + 16-bit sync must fit inside it.
  if (st != RADIOLIB_ERR_NONE) {
    Serial.printf("radio init failed %d\n", st);
    oled.clearBuffer();
    oled.drawStr(0, 10, "Radio init failed");
    oled.sendBuffer();
    while (true) delay(1000);
  }
  radio.setRfSwitchPins(SX126X_RXEN, RADIOLIB_NC);
  radio.setDio2AsRfSwitch(true);
  uint8_t sync[] = {0x54, 0x3D};
  radio.setSyncWord(sync, 2);
  radio.setCRC(0);
  radio.setWhitening(false);
  radio.setEncoding(RADIOLIB_ENCODING_NRZ);
  radio.setDataShaping(RADIOLIB_SHAPING_NONE);
  radio.fixedPacketLengthMode(255);
  radio.setRxBoostedGainMode(true);
  radio.setPacketReceivedAction(onRx);
  radio.startReceive();
}

// Table full: drop the oldest unlabelled meter not heard this session. -1 if none can go.
int evictSlot() {
  int victim = -1;
  for (int i = 0; i < meterCount; i++) {
    const Meter &m = meters[i];
    if (m.thisSession || getLabel(m.id)) continue;
    if (victim < 0 || m.utc < meters[victim].utc) victim = i;
  }
  if (victim >= 0) Serial.printf("# table full, dropped %08lx\n", (unsigned long)meters[victim].id);
  return victim;
}

// Raw telegram (CRCs removed, as wmbusmeters takes it) of a meter we can't read, once per walk.
// Before a GPS fix the row has no time; it is still logged, once per power-on.
void logRaw(Meter &m, const wmbus::Telegram &t, uint32_t now) {
  if (!qspiOk || m.rawThisSession || (now && now < m.rawLoggedUtc + LOG_INTERVAL_S)) return;
  char line[80 + 2 * wmbus::MAX_FRAME];
  char ts[24] = "";
  if (now) isoUtc(now, ts, sizeof(ts));
  const char *lbl = getLabel(m.id), *sn, *ln;
  typeNames(m, sn, ln);
  int n = snprintf(line, sizeof(line), "%s,%08lx,%s,%s,%s,%s (%02x),%d,", ts, (unsigned long)m.id, lbl ? lbl : "",
                   modeName(m), m.mfct, ln, m.type, m.lastRssi);
  for (size_t i = 0; i < t.len && n < (int)sizeof(line) - 3; i++) n += snprintf(line + n, sizeof(line) - n, "%02x", t.data[i]);
  snprintf(line + n, sizeof(line) - n, "\n");
  if (!logLine(rawLog, line)) return;
  m.rawThisSession = true;
  if (now) m.rawLoggedUtc = now;
  surveyDirty = true;
}

// One history row per meter per walk, plus one whenever its alarms change. Needs GPS time,
// so meters heard before the first fix are logged (with the fix time) once it arrives.
void logHistory() {
  if (!qspiOk) return;
  uint32_t now = gpsEpoch();
  if (!now) return;
  for (int i = 0; i < meterCount; i++) {
    Meter &m = meters[i];
    if (!m.heardSinceLog) continue;
    if (now < m.loggedUtc + LOG_INTERVAL_S && m.alarms == m.loggedAlarms) continue;
    char line[200], ts[24], at[48] = "";
    isoUtc(now, ts, sizeof(ts));
    const char *lbl = getLabel(m.id), *sn, *ln;
    typeNames(m, sn, ln);
    int n = snprintf(line, sizeof(line), "%s,%08lx,%s,%s,%s,", ts, (unsigned long)m.id, lbl ? lbl : "", m.mfct, ln);
    if (m.hasLitres) {
      n += snprintf(line + n, sizeof(line) - n, "%lu", (unsigned long)m.litres);
      wmbus::alarmText(m.alarms, at, sizeof(at));
    }
    n += snprintf(line + n, sizeof(line) - n, ",");
    if (m.hasIzarInfo && m.lastMonthDate)
      n += snprintf(line + n, sizeof(line) - n, "%lu,%04lu-%02lu-%02lu", (unsigned long)m.lastMonthLitres,
                    (unsigned long)(m.lastMonthDate / 10000), (unsigned long)(m.lastMonthDate / 100 % 100),
                    (unsigned long)(m.lastMonthDate % 100));
    else
      n += snprintf(line + n, sizeof(line) - n, ",");
    n += snprintf(line + n, sizeof(line) - n, ",%s,", at);
    if (m.hasIzarInfo) n += snprintf(line + n, sizeof(line) - n, "%.1f", m.battHalfYears / 2.0);
    snprintf(line + n, sizeof(line) - n, ",%d\n", m.lastRssi);
    if (!logLine(historyLog, line)) return;  // full and the computer has the drive: next time
    m.loggedUtc = now;
    m.loggedAlarms = m.alarms;
    m.heardSinceLog = false;
    surveyDirty = true;
  }
}

void handlePacket() {
  int16_t rssi = (int16_t)radio.getRSSI();
  int st = radio.readData(raw, sizeof(raw));
  radio.startReceive();
  if (st != RADIOLIB_ERR_NONE) return;

  wmbus::Telegram t;
  wmbus::Mode mode;
  if (wmbus::decode(raw, sizeof(raw), t, mode) != wmbus::Result::OK) {
    errCount++;  // mostly noise / other traffic
    return;
  }
  okCount++;
  digitalWrite(PIN_LED1, HIGH);

  uint32_t id = t.id();
  int idx = -1;
  for (int i = 0; i < meterCount; i++)
    if (meters[i].id == id) { idx = i; break; }

  if (idx < 0) {
    if (meterCount < MAX_METERS) idx = meterCount++;
    else if ((idx = evictSlot()) < 0) return;
    Meter &m = meters[idx];
    memset(&m, 0, sizeof(m));
    m.id = id;
    m.bestRssi = -200;
  }

  Meter &m = meters[idx];
  t.mfct(m.mfct);
  m.ver = t.version();
  m.type = t.type();
  m.mode = (uint8_t)mode;

  // Save-worthy state before this telegram (signal, count and times alone don't warrant a save).
  bool isNewMeter = m.count == 0;
  uint32_t prevLitres = m.litres, prevLastMonth = m.lastMonthLitres;
  int16_t prevBest = m.bestRssi;
  uint32_t l, lastMonth = 0, lastMonthDate = 0;
  uint16_t prevAlarms = m.alarms;
  char mfct4[4]; t.mfct(mfct4);
  if (wmbus::decodeIzar(t, l, &lastMonth, &lastMonthDate)) {
    m.litres = l;
    m.hasLitres = true;
    m.alarms = wmbus::izarAlarms(t);
    m.hasIzarInfo = true;
    m.lastMonthLitres = lastMonth;
    m.lastMonthDate = lastMonthDate;
    m.battHalfYears = wmbus::izarBatteryHalfYears(t);
    m.periodS = wmbus::izarPeriodS(t);
  } else if (strcmp(mfct4, "SEN") == 0) {
    const Label *lbl = findLabel(id);
    const uint8_t *key = lbl && hasKey(*lbl) ? lbl->key : nullptr;  // nullptr = Sensus default key
    if (wmbus::decodeSensus(t, l, key)) {
      m.litres = l;
      m.hasLitres = true;
      m.alarms = wmbus::omsStatusAlarms(t);
    }
  }
  bool newLeak = (m.alarms & wmbus::ALM_LEAK_NOW) && !(prevAlarms & wmbus::ALM_LEAK_NOW);
  m.lastRssi = rssi;
  if (rssi > m.bestRssi) m.bestRssi = rssi;
  m.count++;
  m.lastSeen = millis();
  m.thisSession = true;
  m.heardSinceLog = true;
  uint32_t now = gpsEpoch();
  if (now) m.utc = now;
  if (!m.hasLitres) logRaw(m, t, now);
  bool sampled = addSample(m, rssi);
  if (isNewMeter || sampled || m.litres != prevLitres || m.alarms != prevAlarms || m.bestRssi > prevBest ||
      m.lastMonthLitres != prevLastMonth)
    surveyDirty = true;

  if (Serial.availableForWrite() >= 64) printMeterCsv(Serial, m);  // a stalled terminal mustn't block the loop
  if (newLeak) beepRepeat(3, 3200, 120, 60);
  sortMeters();
}

// ---------- UI ----------
void drawList() {
  char line[32];
  char ok[8];
  if (okCount < 10000) snprintf(ok, sizeof(ok), "%lu", (unsigned long)okCount);
  else snprintf(ok, sizeof(ok), "%luk", (unsigned long)(okCount > 999999 ? 999 : okCount / 1000));
  snprintf(line, sizeof(line), "N%d ok%s er%lu S%lu %.1fV", meterCount, ok,
           (unsigned long)(errCount > 999 ? 999 : errCount), (unsigned long)gps.satellites.value(), batteryVolts());
  oled.drawStr(0, 7, line);
  oled.drawHLine(0, 9, 128);

  if (meterCount == 0) {
    oled.drawStr(0, 24, "Listening T1 868.95...");
    oled.drawStr(0, 34, sortByRssi ? "sort: best RSSI" : "sort: last seen");
    if (!fsOk) oled.drawStr(0, 44, "flash error: no labels");
    if (!qspiOk) oled.drawStr(0, 54, "QSPI error: small survey");
    return;
  }
  if (sel < top) top = sel;
  if (sel >= top + ROWS) top = sel - ROWS + 1;

  for (int r = 0; r < ROWS && top + r < meterCount; r++) {
    const Meter &m = meters[order[top + r]];
    char name[10], val[12];
    const char *lbl = getLabel(m.id);
    if (lbl) snprintf(name, sizeof(name), "#%-7.7s", lbl);
    else snprintf(name, sizeof(name), "%08lx", (unsigned long)m.id);
    if (m.hasLitres) snprintf(val, sizeof(val), "%lu.%03lu", (unsigned long)(m.litres / 1000), (unsigned long)(m.litres % 1000));
    else {
      const char *sn, *ln;
      typeNames(m, sn, ln);
      snprintf(val, sizeof(val), "%s %s", m.mfct, sn);
    }
    // flag: L = leaking now, l = leaked previously, ! = other alarm
    char flag = ' ';
    if (m.alarms & wmbus::ALM_LEAK_NOW) flag = 'L';
    else if (m.alarms & wmbus::ALM_LEAK_PREV) flag = 'l';
    else if (m.alarms) flag = '!';
    // '*' after RSSI = not heard since power-on (values come from the saved survey)
    snprintf(line, sizeof(line), "%s%c%9s %4d%s", name, flag, val, m.bestRssi, m.thisSession ? "" : "*");
    int y = 18 + r * 9;
    if (top + r == sel) {
      oled.drawBox(0, y - 7, 128, 9);
      oled.setDrawColor(0);
      oled.drawStr(1, y, line);
      oled.setDrawColor(1);
    } else {
      oled.drawStr(1, y, line);
    }
  }
}

void drawDetail() {
  const Meter &m = meters[order[sel]];
  char line[32];
  const char *lbl = getLabel(m.id);
  if (lbl) snprintf(line, sizeof(line), "%08lx  House %s", (unsigned long)m.id, lbl);
  else snprintf(line, sizeof(line), "%08lx  <> to label", (unsigned long)m.id);
  oled.drawStr(0, 7, line);

  if (m.hasLitres) {
    snprintf(line, sizeof(line), "%lu.%03lu m3  %s %s", (unsigned long)(m.litres / 1000), (unsigned long)(m.litres % 1000),
             m.mfct, modeName(m));
    oled.drawStr(0, 16, line);
    char at[26];
    wmbus::alarmText(m.alarms, at, sizeof(at));
    if (m.alarms & (wmbus::ALM_LEAK_NOW | wmbus::ALM_BLOCKED | wmbus::ALM_BACKFLOW | wmbus::ALM_SUBMARINE |
                    wmbus::ALM_ERROR)) {
      oled.drawBox(0, 18, 128, 9);
      oled.setDrawColor(0);
      oled.drawStr(1, 25, at);
      oled.setDrawColor(1);
    } else {
      oled.drawStr(0, 25, at);
    }
  } else {
    const char *sn, *ln;
    typeNames(m, sn, ln);
    snprintf(line, sizeof(line), "%s %s %s", m.mfct, ln, modeName(m));
    oled.drawStr(0, 16, line);
    oled.drawStr(0, 25, "reading not decoded");
  }

  snprintf(line, sizeof(line), "rssi %d best %d x%u", m.lastRssi, m.bestRssi, m.count);
  oled.drawStr(0, 34, line);

  double lat, lon;
  float spread;
  if (estimatePosition(m, lat, lon, spread)) {
    snprintf(line, sizeof(line), "%.5f,%.5f", lat, lon);
    oled.drawStr(0, 43, line);
    snprintf(line, sizeof(line), "pos from %u fixes +-%.0fm", m.nSamples, spread);
    oled.drawStr(0, 52, line);
  } else {
    oled.drawStr(0, 43, "no position yet");
  }

  if (m.thisSession) {
    snprintf(line, sizeof(line), "seen %lus ago", (unsigned long)((millis() - m.lastSeen) / 1000));
  } else if (m.utc) {
    char ts[24];
    formatUtc(m.utc, ts, sizeof(ts));
    snprintf(line, sizeof(line), "seen %s", ts);
  } else {
    snprintf(line, sizeof(line), "seen: earlier survey");
  }
  oled.drawStr(0, 62, line);
}

void drawScreen() {
  oled.clearBuffer();
  if (detail && meterCount > 0) drawDetail();
  else drawList();
  oled.sendBuffer();
}

// ---------- buttons ----------
struct Btn {
  uint8_t pin;
  bool repeat;
  bool down;
  uint32_t changed, lastFire;
};
Btn bUp = {TB_UP, true}, bDown = {TB_DOWN, true}, bLeft = {TB_LEFT, true}, bRight = {TB_RIGHT, true},
    bPress = {TB_PRESS, false}, bUser = {CANCEL_BUTTON_PIN, false};
Btn *allBtns[] = {&bUp, &bDown, &bLeft, &bRight, &bPress, &bUser};

// Fires once on press; repeating buttons also fire every 80 ms after being held 400 ms.
bool pressed(Btn &b) {
  bool isDown = !digitalRead(b.pin);  // active low
  uint32_t now = millis();
  if (isDown != b.down && now - b.changed > 30) {
    b.down = isDown;
    b.changed = now;
    if (isDown) {
      b.lastFire = now;
      return true;
    }
    return false;
  }
  if (b.repeat && b.down && now - b.changed > 400 && now - b.lastFire > 80) {
    b.lastFire = now;
    return true;
  }
  return false;
}

void handleButtons() {
  bool redraw = false;
  if (pressed(bUp) && sel > 0) { selId = meters[order[--sel]].id; redraw = true; }
  if (pressed(bDown) && sel < meterCount - 1) { selId = meters[order[++sel]].id; redraw = true; }
  bool l = pressed(bLeft), r = pressed(bRight);
  if (detail && meterCount > 0 && (l || r)) {
    uint32_t id = meters[order[sel]].id;
    stepLabel(id, r ? 1 : -1);
    redraw = true;
  }
  if (pressed(bPress)) {
    detail = !detail;
    redraw = true;
  }
  if (pressed(bUser)) {
    sortByRssi = !sortByRssi;
    sortMeters();
    redraw = true;
  }
  if (redraw) drawScreen();
}

// ---------- serial commands ----------
char cmd[48];
size_t cmdLen = 0;

void runCommand(char *c) {
  if (!strcmp(c, "d")) {
    Serial.println("# dump");
    printCsvHeader(Serial);
    for (int i = 0; i < meterCount; i++) printMeterCsv(Serial, meters[order[i]]);
    Serial.println("# end");
  } else if (!strcmp(c, "c")) {
    meterCount = 0;
    sel = top = 0;
    selId = 0;
    okCount = errCount = 0;
    surveyDirty = true;
    lastSurveySave = 0;
    Serial.println("# survey cleared (labels kept)");
  } else if (c[0] == 'l' && (c[1] == ' ' || c[1] == 0)) {
    // l <id> [label]   - set or remove a label
    char *idStr = strtok(c + 1, " ");
    char *text = strtok(nullptr, "");
    if (!idStr) {
      Serial.println("# usage: l <id> [label]");
      return;
    }
    uint32_t id = strtoul(idStr, nullptr, 16);
    setLabel(id, text);
    Serial.printf("# %08lx -> %s\n", (unsigned long)id, text ? text : "(removed)");
    sortMeters();
  } else if (!strcmp(c, "L")) {
    Serial.println("# labels");
    for (int i = 0; i < labelCount; i++)
      Serial.printf("%08lx,%s%s\n", (unsigned long)labels[i].id, labels[i].text, hasKey(labels[i]) ? ",key" : "");
    Serial.println("# end");
  } else if (!strcmp(c, "s")) {
    Serial.println("# status");
    Serial.printf("build %s %s\n", __DATE__, __TIME__);
    Serial.printf("qspi %s, jedec %06lx, %lu KB\n", qspiOk ? "ok" : "FAILED", (unsigned long)chipJedecId(),
                  (unsigned long)(flash.size() / 1024));
    Serial.printf("ring %s: %lu blocks, snapshot %lu, next block %lu\n", ringOk ? "ok" : "off", (unsigned long)ringBlocks,
                  (unsigned long)snapSeq, (unsigned long)snapNext);
    Serial.printf("internal flash %s, meters %d, labels %d, log rows waiting %u+%u bytes\n", fsOk ? "ok" : "FAILED",
                  meterCount, labelCount, (unsigned)historyLog.len, (unsigned)rawLog.len);
    Serial.printf("usb mounted %d, drive %s\n", TinyUSBDevice.mounted(), driveToHost ? "with computer" : "held");
    Serial.printf("gps chars %lu, sentences ok %lu bad %lu, sats %lu, fix %s, time %s\n",
                  (unsigned long)gps.charsProcessed(), (unsigned long)gps.passedChecksum(),
                  (unsigned long)gps.failedChecksum(), (unsigned long)gps.satellites.value(),
                  gps.location.isValid() ? "yes" : "no", gpsTimeTrusted ? "trusted" : "not yet");
    Serial.println("# end");
  } else if (!strcmp(c, "h")) {
    printLog(historyLog);
    Serial.println("# end");
  } else if (!strcmp(c, "r")) {
    printLog(rawLog);
    Serial.println("# end");
  } else if (!strcmp(c, "HCLEAR")) {
    if (driveToHost) {
      Serial.println("# unplug the USB drive first (or eject it and use a charger)");
      return;
    }
    historyLog.len = rawLog.len = 0;
    if (qspiOk) {
      fatfs.remove(HISTORY_FILE);
      fatfs.remove(RAW_FILE);
      flash.syncBlocks();
    }
    for (int i = 0; i < meterCount; i++) {
      meters[i].loggedUtc = meters[i].rawLoggedUtc = 0;
      meters[i].rawThisSession = false;
    }
    surveyDirty = true;
    Serial.println("# history and raw logs deleted");
  } else if (!strcmp(c, "FORMAT")) {
    driveToHost = false;
    delay(200);  // let any USB read in progress finish before erasing
    if (qspiOk) fat12::format(flash, DRIVE_LABEL, NRF_FICR->DEVICEID[0]);
    InternalFS.format();
    Serial.println("# flash formatted, rebooting");
    delay(100);
    NVIC_SystemReset();
  } else if (c[0] == 'k' && (c[1] == ' ' || c[1] == 0)) {
    // k <id> [32 hex digits]  - set (or with no key, clear) a meter's AES key, e.g. for Sensus
    char *idStr = strtok(c + 1, " ");
    char *hexKey = strtok(nullptr, " ");
    if (!idStr) {
      Serial.println("# usage: k <id> [32 hex digits]");
      return;
    }
    uint32_t id = strtoul(idStr, nullptr, 16);
    uint8_t key[16] = {0};
    if (hexKey) {
      bool ok = strlen(hexKey) == 32;
      for (int i = 0; ok && i < 32; i++) ok = isxdigit((unsigned char)hexKey[i]);
      if (!ok) {
        Serial.println("# key must be exactly 32 hex digits - nothing changed");
        return;
      }
      for (int i = 0; i < 16; i++) {
        char byte[3] = {hexKey[i * 2], hexKey[i * 2 + 1], 0};
        key[i] = strtoul(byte, nullptr, 16);
      }
    }
    Label *l = hexKey ? labelEntry(id) : findLabel(id);
    if (l) {
      memcpy(l->key, key, sizeof(key));
      pruneLabel(l);
      labelsDirty = true;
      lastLabelEdit = millis();
    }
    Serial.printf("# key for %08lx: %s\n", (unsigned long)id, hexKey ? (l ? "set" : "NOT set") : "cleared");
  } else if (*c) {
    Serial.println("# commands: s=status  d=dump  c=clear survey  l <id> [label]  L=list labels  k <id> [hexkey]  h=history"
                   "  r=raw telegrams  HCLEAR=delete history+raw  FORMAT=erase all");
  }
}

void handleSerial() {
  while (Serial.available()) {
    char ch = Serial.read();
    if (ch == '\r' || ch == '\n') {
      cmd[cmdLen] = 0;
      runCommand(cmd);
      cmdLen = 0;
    } else if (cmdLen < sizeof(cmd) - 1) {
      cmd[cmdLen++] = ch;
    }
  }
}

// ---------- main ----------
void setup() {
  flashMutex = xSemaphoreCreateMutex();
  // The P25Q16H isn't in the library's auto-detect list, so name it (from variant.h).
  static const SPIFlash_Device_t qspiDevices[] = {EXTERNAL_FLASH_DEVICES};
  qspiOk = flash.begin(qspiDevices, sizeof(qspiDevices) / sizeof(qspiDevices[0]));
  usbDriveInit();
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 2000) delay(10);

  pinMode(PIN_GPS_STANDBY, OUTPUT);
  digitalWrite(PIN_GPS_STANDBY, HIGH);  // HIGH = awake on L76K
  Serial1.begin(GPS_BAUDRATE);

  for (auto *b : allBtns) pinMode(b->pin, INPUT_PULLUP);
  analogReadResolution(12);

  // OLED address: Zephyr's board file says 0x3D, U8g2 defaults to 0x3C - probe both.
  Wire.begin();
  uint8_t oledAddr = 0x3C;
  Wire.beginTransmission(0x3D);
  if (Wire.endTransmission() == 0) oledAddr = 0x3D;
  oled.setI2CAddress(oledAddr * 2);  // U8g2 takes the 8-bit address
  oled.begin();
  oled.setFont(u8g2_font_5x8_tf);
  oled.clearBuffer();
  oled.drawStr(0, 10, "wM-Bus survey");
  oled.sendBuffer();

  fsInit();
  sortMeters();
  // Storage status for a moment, so problems show without a serial terminal.
  char line[32];
  oled.drawStr(0, 22, "built " __DATE__);
  if (qspiOk) snprintf(line, sizeof(line), "flash ok %luK ring %lu", (unsigned long)(flash.size() / 1024),
                       (unsigned long)ringBlocks);
  else snprintf(line, sizeof(line), "FLASH FAIL id %06lx", (unsigned long)chipJedecId());
  oled.drawStr(0, 32, line);
  snprintf(line, sizeof(line), "%d meters, %d labels", meterCount, labelCount);
  oled.drawStr(0, 42, line);
  oled.sendBuffer();
  delay(2000);
  radioInit();
  beep(2000, 60);
  Serial.println("# wM-Bus T1 survey ready. Send 'help' for commands");
  printCsvHeader(Serial);
}

void loop() {
  while (Serial1.available()) gps.encode(Serial1.read());

  if (rxFlag) {
    rxFlag = false;
    handlePacket();
  }

  handleButtons();
  handleSerial();
  handleBeeps();

  uint32_t now = millis();
  if (labelsDirty && now - lastLabelEdit > LABEL_SAVE_MS) saveLabels();
  static uint32_t lastLogCheck = 0;
  if (now - lastLogCheck > 1000) {
    lastLogCheck = now;
    logHistory();
  }
  handleUsbDrive();
  if (surveyDirty && (ringOk || !driveToHost) && now - lastSurveySave > (ringOk ? SURVEY_SAVE_MS : FALLBACK_SAVE_MS))
    saveSurvey();

  static uint32_t lastDraw = 0;
  if (now - lastDraw > 500) {
    lastDraw = now;
    digitalWrite(PIN_LED1, LOW);
    drawScreen();
  }
  delay(1);
}
