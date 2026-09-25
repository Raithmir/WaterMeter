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
#include <bluefruit.h>
#include <cmath>
#include "wmbus.h"
#include "survey.h"
#include "snapring.h"
#include "fat12.h"

// SdFat also defines a global File, so name the LittleFS one explicitly
using LfsFile = Adafruit_LittleFS_Namespace::File;
using Adafruit_LittleFS_Namespace::FILE_O_READ;
using Adafruit_LittleFS_Namespace::FILE_O_WRITE;

// ---------- config ----------
#define MAX_METERS 512          // survey lives on the 2 MB QSPI flash
#define MAX_LABELS 128          // labels stay in the 28 KB internal flash
#define ROWS 6
#define DISPLAY_SH1106 1         // L1 uses an SH1106 (per Zephyr board file); 0 = SSD1306
#define SURVEY_SAVE_MS 30000     // save survey at most this often (only when something worth keeping changed)
#define FALLBACK_SAVE_MS 600000  // without the snapshot ring (internal flash: ~10k erase cycles)
#define LABEL_SAVE_MS 3000       // save labels this long after the last edit
#define LOW_BATT_V 3.50f         // warn below this (LiPo), cleared again above LOW_BATT_V + 0.1
#define GPS_MAX_AGE_MS 1500      // position samples need a fix at most this old
#define GPS_MAX_HDOP 2.5         // ... and at least this good (HDOP ~1 = good, >2.5 = metres worse)
#define FS_MARKER "/wmsurvey"    // flash is formatted once if this is missing
#define STATE_FILE "state.bin"      // QSPI, hidden: ring of survey snapshots (see "snapshot ring")
#define STATE_BYTES (1024 * 1024UL)  // smaller sizes are tried if there's no free 1 MB run
#define RING_MIN_BLOCKS 48           // >= 2 x largest snapshot (512 meters + full logs = 19 blocks) + header
#define SURVEY_FILE "/survey.bin"    // older builds (QSPI or internal flash); fallback if the ring fails
#define SURVEY_CSV "/survey.csv"     // QSPI, written when a computer is plugged in
#define DRIVE_LABEL "WMBUS"
#define LABEL_FILE "/labels.bin"
#define FILE_VERSION_LABELS 2
#define SETTINGS_FILE "/settings.bin"  // internal flash, saved LABEL_SAVE_MS after the last change
#define FILE_VERSION_SETTINGS 1
#define SURVEY_VERSION 3         // 3 added IZAR last-month/battery/period and log state.
                                 // Changing Meter (survey.h) means a new version AND a conversion in
                                 // loadSnapshot(), or the saved survey is dropped on upgrade.
#define HISTORY_FILE "/history.csv"  // one row per meter per walk
#define RAW_FILE "/raw.csv"          // raw telegrams of meters whose reading isn't decoded

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
using namespace survey;

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
bool diag = false;      // diagnostics screen (joystick left/right from the list)
bool screenOn = true;
uint32_t lastInput = 0;  // last button press, for the screen timeout
float battV = 0;         // smoothed battery voltage
bool battLow = false;
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

// Settings screen (joystick left from the list). Adding a field means a new FILE_VERSION_SETTINGS;
// an older file is then ignored and the defaults used.
const uint32_t SCREEN_OFF_CHOICES[] = {30000, 60000, 120000, 300000, 0};  // 0 = never
const char *const SCREEN_OFF_NAMES[] = {"30 s", "1 min", "2 min", "5 min", "never"};
const int N_SCREEN_OFF = sizeof(SCREEN_OFF_CHOICES) / sizeof(SCREEN_OFF_CHOICES[0]);
struct Settings {
  bool ble;           // Bluetooth link to the phone page
  bool gps;           // off = GPS module in standby (saves power, no positions)
  uint8_t screenOff;  // index into SCREEN_OFF_CHOICES
  bool beeps;
  bool sortByRssi;    // also toggled by the user button
} settings = {false, true, 2, true, true};
bool settingsDirty = false;
uint32_t lastSettingsEdit = 0;

// Log output: USB serial, plus the phone's console when one is listening (see bluetooth).
class BleLog : public Print {
 public:
  size_t write(uint8_t c) override { return write(&c, 1); }
  size_t write(const uint8_t *b, size_t n) override;
} bleLog;
class Console : public Print {
 public:
  size_t write(uint8_t c) override { return write(&c, 1); }
  size_t write(const uint8_t *b, size_t n) override {
    bleLog.write(b, n);
    return Serial.write(b, n);
  }
} con;

void onRx() { rxFlag = true; }

// ---------- helpers ----------
float batteryVolts() { return analogRead(PIN_VBAT) * AREF_VOLTAGE / 4095.0f * ADC_MULTIPLIER; }

void beep(uint16_t freq, uint16_t ms) {
  if (settings.beeps) tone(PIN_BUZZER, freq, ms);
}

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

void setScreen(bool on) {
  screenOn = on;
  oled.setPowerSave(!on);
}

// Something needs attention (leak, low battery): screen on and restart the timeout.
void wakeScreen() {
  lastInput = millis();
  if (!screenOn) setScreen(true);
}

// Called every 500 ms. Averaged, because the reading dips while the radio or GPS draw current.
void updateBattery() {
  float v = batteryVolts();
  battV = battV ? battV * 0.8f + v * 0.2f : v;
  if (!battLow && battV < LOW_BATT_V) {
    battLow = true;
    beepRepeat(2, 800, 300, 200);
    wakeScreen();
  } else if (battLow && battV > LOW_BATT_V + 0.1f) {
    battLow = false;  // charging
  }
}

void handleBeeps() {
  if (!beepSeq.left || (int32_t)(millis() - beepSeq.next) < 0) return;
  beep(beepSeq.freq, beepSeq.ms);
  beepSeq.left--;
  beepSeq.next = millis() + beepSeq.ms + beepSeq.gapMs;
}

// Before its first fix the GPS reports its own clock, which can be anything (TinyGPSPlus
// accepts it), so time is only trusted once a position fix has been seen.
bool gpsTimeTrusted = false;

// With the GPS switched off (or its time going stale) the clock runs on from the last GPS time.
uint32_t gpsEpoch() {
  static uint32_t lastT = 0, lastAt = 0;
  if (!gpsTimeTrusted) {
    if (!gps.location.isValid()) return 0;
    gpsTimeTrusted = true;
  }
  if (gps.date.isValid() && gps.time.isValid() && gps.time.age() < 3000 && gps.date.year() >= 2024 &&
      gps.date.year() <= 2079) {
    lastT = epochFromCivil(gps.date.year(), gps.date.month(), gps.date.day(), gps.time.hour(), gps.time.minute(),
                           gps.time.second());
    lastAt = millis();
    return lastT;
  }
  return lastT ? lastT + (millis() - lastAt) / 1000 : 0;
}

void applyGps() { digitalWrite(PIN_GPS_STANDBY, settings.gps ? HIGH : LOW); }  // LOW = standby on L76K

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
    con.println("# label table full");
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
// true if the sample was kept. The L76K sends a fix every second, so a fresh one is at most ~1 s
// old; older means the last update was lost, and walking on it would put the sample metres off.
bool addSample(Meter &m, int16_t rssi) {
  if (!gps.location.isValid() || gps.location.age() > GPS_MAX_AGE_MS) return false;
  if (gps.hdop.isValid() && gps.hdop.hdop() > GPS_MAX_HDOP) return false;  // poor satellite geometry
  return survey::addSample(m, (int32_t)lround(gps.location.lat() * 1e7), (int32_t)lround(gps.location.lng() * 1e7),
                           rssi);
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
const uint32_t MAGIC_SETTINGS = 0x53455447;  // "SETG"

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
  con.printf("# converted survey from format 2 (%d meters)\n", h.count);
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
LogFile historyLog = {HISTORY_FILE, HISTORY_HEADER};
LogFile rawLog = {RAW_FILE, RAW_HEADER};

bool flushLog(LogFile &lf) {
  if (!lf.len) return true;
  if (!qspiOk || driveToHost) return false;
  bool isNew = !fatfs.exists(lf.path);
  File32 f = fatfs.open(lf.path, O_WRONLY | O_CREAT | O_APPEND);
  if (!f) return false;
  bool ok = !isNew || (f.write(lf.header, strlen(lf.header)) == strlen(lf.header) && f.write('\n') == 1);
  ok = ok && f.write(lf.buf, lf.len) == lf.len;
  ok = f.close() && ok;
  if (ok) lf.len = 0;
  else con.printf("# %s append FAILED\n", lf.path);
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
    Serial.println(lf.header);
  }
  Serial.write((const uint8_t *)lf.buf, lf.len);
}

// ---------- snapshot ring ----------
// The survey (meters + log rows waiting for the CSV files) is saved through a ring of snapshots
// in the hidden state.bin; see snapring.h.
bool ringOk = false;
snapring::Ring<LockedFlash> ring(flash);

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
  uint32_t base = (first + 7) & ~7UL;
  uint32_t blocks = (last + 1 - base) / 8;
  if (blocks < RING_MIN_BLOCKS) return false;
  return ring.begin(base, blocks, created, hwRandom());
}

// Survey + waiting log rows, as the ring stores them.
snapring::Contents snapContents() {
  return {meters, sizeof(Meter), (uint16_t)meterCount, MAX_METERS,
          (uint8_t *)historyLog.buf, (uint16_t)historyLog.len, sizeof(historyLog.buf),
          (uint8_t *)rawLog.buf, (uint16_t)rawLog.len, sizeof(rawLog.buf)};
}

// Newest valid snapshot into meters[] and the log buffers. -1 if there is none.
int loadSnapshot() {
  snapring::Contents c = snapContents();
  int n = ring.load(SURVEY_VERSION, c, [](uint32_t seq) {
    con.printf("# snapshot %lu damaged, trying an older one\n", (unsigned long)seq);
  });
  historyLog.len = c.aLen;
  rawLog.len = c.bLen;
  return n;
}

bool saveSnapshot() { return ring.save(SURVEY_VERSION, snapContents()); }

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
  con.printf("# survey saved (%d meters) %s\n", meterCount, ok ? "ok" : "FAILED");
  surveyDirty = false;
  lastSurveySave = millis();
  return ok;
}

void saveLabels() {
  bool ok = saveFile(LABEL_FILE, MAGIC_LABELS, FILE_VERSION_LABELS, labels, sizeof(Label), labelCount);
  con.printf("# labels saved (%d) %s\n", labelCount, ok ? "ok" : "FAILED");
  labelsDirty = false;
}

void saveSettings() {
  bool ok = saveFile(SETTINGS_FILE, MAGIC_SETTINGS, FILE_VERSION_SETTINGS, &settings, sizeof(settings), 1);
  con.printf("# settings saved %s\n", ok ? "ok" : "FAILED");
  settingsDirty = false;
}

// Defaults stay if there is no file (or one from another settings version).
void loadSettings() {
  Settings s;
  if (loadFile(SETTINGS_FILE, MAGIC_SETTINGS, FILE_VERSION_SETTINGS, &s, sizeof(s), 1) == 1) settings = s;
  if (settings.screenOff >= N_SCREEN_OFF) settings.screenOff = 2;
}

void fsInit() {
  fsOk = InternalFS.begin();
  if (fsOk && !InternalFS.exists(FS_MARKER)) {
    // First boot after flashing (e.g. over Meshtastic): wipe its settings to free the 28 KB area.
    con.println("# formatting internal flash");
    InternalFS.format();
    fsOk = InternalFS.begin();
    LfsFile f(InternalFS);
    if (fsOk && f.open(FS_MARKER, FILE_O_WRITE)) {
      f.write("1");
      f.close();
    }
  }
  if (!fsOk) con.println("# internal flash unavailable - labels will not be saved");

  if (qspiOk && !fatfs.begin(&flash)) {
    // Only format a blank chip. A filesystem that fails to mount may still hold the survey and
    // history, so leave it alone; FORMAT wipes it deliberately.
    uint8_t sec[512];
    bool blank = flash.readSectors(0, sec, 1) && !(sec[510] == 0x55 && sec[511] == 0xAA);
    if (blank) {
      con.println("# formatting QSPI flash");
      qspiOk = fat12::format(flash, DRIVE_LABEL, NRF_FICR->DEVICEID[0]) && fatfs.begin(&flash);
    } else {
      con.println("# QSPI filesystem won't mount - not formatting it (send FORMAT to wipe)");
      qspiOk = false;
    }
  }
  if (!qspiOk) con.println("# QSPI flash unavailable - survey saved to internal flash (limited size)");

  if (qspiOk) {
    ringOk = ringInit();
    if (!ringOk) con.println("# state.bin unavailable - saving survey.bin instead (more flash wear)");
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
  con.printf("# loaded %d meters, %d labels\n", meterCount, labelCount);
  if (legacy) {
    sortMeters();
    if (saveSurvey()) {
      if (ringOk) {
        fatfs.remove(SURVEY_FILE);
        fatfs.remove("/survey.tmp");
        flash.syncBlocks();
      }
      if (fsOk) InternalFS.remove(SURVEY_FILE);
      con.println("# survey moved to the new save format");
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
    if (settingsDirty) saveSettings();
    saveSurvey(true);  // also appends waiting log rows
    if (!writeSurveyCsv()) con.println("# survey.csv FAILED");
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
      if (settings.sortByRssi) swap = a.bestRssi < b.bestRssi;
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

void printCsvHeader(Print &out) { out.println(SURVEY_HEADER); }

void printMeterCsv(Print &out, const Meter &m) {
  char row[ROW_MAX];
  surveyRow(row, sizeof(row), m, getLabel(m.id));
  out.println(row);
}

// ---------- radio ----------
void radioInit() {
  int st = radio.beginFSK(868.95, 100.0, 50.0, 234.3, 10, 8, SX126X_DIO3_TCXO_VOLTAGE, false);
  // preamble length 8 -> 8-bit preamble detector. T1 only guarantees a 38-chip preamble,
  // and detector + 16-bit sync must fit inside it.
  if (st != RADIOLIB_ERR_NONE) {
    con.printf("radio init failed %d\n", st);
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
  if (victim >= 0) con.printf("# table full, dropped %08lx\n", (unsigned long)meters[victim].id);
  return victim;
}

// Raw telegram (CRCs removed, as wmbusmeters takes it) of a meter we can't read, once per walk.
// Before a GPS fix the row has no time; it is still logged, once per power-on.
void logRaw(Meter &m, const wmbus::Telegram &t, uint32_t now) {
  if (!qspiOk || m.rawThisSession || (now && now < m.rawLoggedUtc + LOG_INTERVAL_S)) return;
  char line[ROW_MAX + 1];
  size_t n = rawRow(line, sizeof(line) - 1, m, getLabel(m.id), t, now);
  strcpy(line + n, "\n");
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
    if (!historyDue(m, now)) continue;
    char line[ROW_MAX + 1];
    size_t n = historyRow(line, sizeof(line) - 1, m, getLabel(m.id), now);
    strcpy(line + n, "\n");
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
  uint32_t prevLitres = m.litres, prevBilling = m.billingLitres;
  int16_t prevBest = m.bestRssi;
  uint32_t l, billing = 0, billingDate = 0;
  uint16_t prevAlarms = m.alarms;
  char mfct4[4]; t.mfct(mfct4);
  if (wmbus::decodeIzar(t, l, &billing, &billingDate)) {
    m.litres = l;
    m.hasLitres = true;
    m.alarms = wmbus::izarAlarms(t);
    m.hasIzarInfo = true;
    m.billingLitres = billing;
    m.billingDate = billingDate;
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
      m.billingLitres != prevBilling)
    surveyDirty = true;

  if (Serial.availableForWrite() >= 64) printMeterCsv(Serial, m);  // a stalled terminal mustn't block the loop
  if (newLeak) {
    beepRepeat(3, 3200, 120, 60);
    wakeScreen();
  }
  sortMeters();
}

// ---------- bluetooth ----------
// Off unless turned on in settings (the SoftDevice isn't even started until then). A phone running
// the web page in web/ mirrors the screen, presses the buttons and uses the serial console, which
// also downloads the CSV files. Every characteristic needs an encrypted link paired with the
// passkey shown on the screen.
// Notifications are only queued while the SoftDevice has room for them (TX_QUEUE), so the loop
// never waits on the phone.

// Screen mirror: notify [page 0-7][first column][pixels...], pixel bytes as in the U8g2 buffer
// (one byte = 8 pixels down, lowest bit on top). Keys: write one byte per press, 'U' 'D' 'L' 'R'
// 'P' (joystick press) 'B' (user button), | 0x80 = repeat from holding it down.
BLEService screenSvc("5f6d0001-2a8b-4c1e-9d3f-7b1a0c4e8d21");
BLECharacteristic screenChr("5f6d0002-2a8b-4c1e-9d3f-7b1a0c4e8d21");
BLECharacteristic keysChr("5f6d0003-2a8b-4c1e-9d3f-7b1a0c4e8d21");
// Console: the Nordic UART Service, so BLE serial terminal apps work too.
BLEService consoleSvc(BLEUART_UUID_SERVICE);
BLECharacteristic consoleTx(BLEUART_UUID_CHR_TXD);
BLECharacteristic consoleRx(BLEUART_UUID_CHR_RXD);

const uint32_t TX_QUEUE = 3;  // SoftDevice notification queue with BANDWIDTH_MAX

// Byte queue with one writer and one reader (N a power of two).
template <size_t N>
struct ByteRing {
  uint8_t buf[N];
  volatile uint32_t head = 0, tail = 0;  // free running
  uint32_t used() const { return head - tail; }
  uint32_t space() const { return N - used(); }
  bool put(uint8_t b) {
    if (!space()) return false;
    buf[head % N] = b;
    asm volatile("" ::: "memory");  // byte stored before the reader sees it
    head = head + 1;
    return true;
  }
  void put(const uint8_t *b, size_t n) {
    for (size_t i = 0; i < n; i++) put(b[i]);
  }
  int get() {
    if (!used()) return -1;
    uint8_t b = buf[tail % N];
    asm volatile("" ::: "memory");
    tail = tail + 1;
    return b;
  }
  size_t peek(uint8_t *out, size_t n) const {
    n = min((uint32_t)n, used());
    for (size_t i = 0; i < n; i++) out[i] = buf[(tail + i) % N];
    return n;
  }
  void drop(size_t n) { tail = tail + n; }
  void clear() { tail = head; }
};

ByteRing<16> keyRing;       // BLE task -> loop
ByteRing<256> cmdRing;      // BLE task -> loop
ByteRing<4096> txRing;      // loop -> console notifications
bool bleBegun = false;
char bleName[12];
volatile bool screenSub = false, consoleSub = false, screenResend = false;
volatile uint32_t txDone = 0;  // notifications sent (BLE task)
uint32_t txSent = 0;           // notifications queued (loop)
bool bleWasLinked = false;
bool forgetBonds = false;  // "Forget phones", waiting for the link to close
uint32_t forgotAt = 0;     // ... shows "done" for a moment
uint8_t mirrorSent[128 * 8];   // screen as the phone last got it
char pairCode[7];
volatile bool pairPending = false;  // passkey on screen until paired (or PAIR_SHOW_MS)
volatile bool pairFailed = false;
uint32_t pairSince = 0;
bool pairShown = false;
#define PAIR_SHOW_MS 60000

bool bleLinked() { return bleBegun && Bluefruit.connected(); }

// Downloads (d/h/r from the phone): "# file <name>", the rows, "# end". Other log output is held
// back meanwhile so it can't land inside the file.
struct Download {
  char kind;             // 0 = none, 'd' survey, 'h' history, 'r' raw
  int next;              // d: next meter
  LogFile *log;          // h/r
  uint32_t size, off;    // h/r: file length when the download started, bytes sent
  uint32_t bufLen, bufOff;  // h/r: rows waiting in RAM at the start (copied to dlBuf)
} dl = {};
char dlBuf[sizeof(historyLog.buf)];

size_t BleLog::write(const uint8_t *b, size_t n) {
  if (consoleSub && !dl.kind && txRing.space() >= n) txRing.put(b, n);  // whole or not at all
  return n;
}

void txText(const char *t) { txRing.put((const uint8_t *)t, strlen(t)); }

void startDownload(char kind) {
  dl = {};
  dl.kind = kind;
  if (kind == 'd') {
    txText("# file survey.csv\n");
    txText(SURVEY_HEADER);
    txText("\n");
    return;
  }
  dl.log = kind == 'h' ? &historyLog : &rawLog;
  txText(kind == 'h' ? "# file history.csv\n" : "# file raw.csv\n");
  File32 f = qspiOk ? fatfs.open(dl.log->path, O_RDONLY) : File32();
  if (f) {
    dl.size = f.fileSize();
    f.close();
  } else {
    txText(dl.log->header);
    txText("\n");
  }
  dl.bufLen = dl.log->len;
  memcpy(dlBuf, dl.log->buf, dl.bufLen);
}

// Tops up txRing with the next part of the download.
void pumpDownload() {
  while (dl.kind && txRing.space() >= ROW_MAX + 16) {
    if (dl.kind == 'd') {
      if (dl.next < meterCount) {
        char row[ROW_MAX];
        const Meter &m = meters[dl.next++];
        surveyRow(row, sizeof(row), m, getLabel(m.id));
        txText(row);
        txText("\n");
        continue;
      }
    } else if (dl.off < dl.size) {
      uint8_t chunk[512];
      File32 f = fatfs.open(dl.log->path, O_RDONLY);
      int n = f && f.seekSet(dl.off) ? f.read(chunk, min((uint32_t)sizeof(chunk), dl.size - dl.off)) : -1;
      if (f) f.close();
      if (n > 0) {
        txRing.put(chunk, n);
        dl.off += n;
        continue;
      }
      txText("# read failed\n");
    } else if (dl.bufOff < dl.bufLen) {
      uint32_t n = min((uint32_t)512, dl.bufLen - dl.bufOff);
      txRing.put((const uint8_t *)dlBuf + dl.bufOff, n);
      dl.bufOff += n;
      continue;
    }
    txText("# end\n");
    dl.kind = 0;
  }
}

// One notification, if the SoftDevice queue has room for it.
bool bleSend(BLECharacteristic &chr, const uint8_t *d, uint16_t n) {
  if (txSent - txDone >= TX_QUEUE || !chr.notify(d, n)) return false;
  txSent++;
  return true;
}

uint16_t blePayload() {
  BLEConnection *c = Bluefruit.Connection(Bluefruit.connHandle());
  return c ? min(c->getMtu() - 3, 244) : 20;
}

void drawScreen();

// Sends the parts of the screen that changed since the phone last got them.
void pushScreen() {
  if (screenResend) {
    screenResend = false;
    drawScreen();
    const uint8_t *buf = oled.getBufferPtr();
    for (size_t i = 0; i < sizeof(mirrorSent); i++) mirrorSent[i] = ~buf[i];  // all different
  }
  const uint8_t *buf = oled.getBufferPtr();
  int maxPx = blePayload() - 2;
  for (int p = 0; p < 8; p++) {
    const uint8_t *row = buf + p * 128;
    uint8_t *sent = mirrorSent + p * 128;
    int x = 0;
    while (true) {
      while (x < 128 && row[x] == sent[x]) x++;
      if (x == 128) break;
      int n = min(128 - x, maxPx);
      while (n > 1 && row[x + n - 1] == sent[x + n - 1]) n--;
      uint8_t pkt[246] = {(uint8_t)p, (uint8_t)x};
      memcpy(pkt + 2, row + x, n);
      if (!bleSend(screenChr, pkt, n + 2)) return;
      memcpy(sent + x, row + x, n);
      x += n;
    }
  }
}

void pushConsole() {
  uint16_t max = blePayload();
  while (txRing.used()) {
    uint8_t pkt[244];
    size_t n = txRing.peek(pkt, max);
    if (!bleSend(consoleTx, pkt, n)) return;
    txRing.drop(n);
  }
}

// ---- callbacks (BLE task) ----
void onBleEvent(ble_evt_t *e) {
  if (e->header.evt_id == BLE_GATTS_EVT_HVN_TX_COMPLETE) txDone = txDone + e->evt.gatts_evt.params.hvn_tx_complete.count;
}

void onConnect(uint16_t conn) {
  BLEConnection *c = Bluefruit.Connection(conn);
  c->requestPHY();
  c->requestDataLengthUpdate();
  c->requestMtuExchange(247);  // a whole screen row per notification
}

void onDisconnect(uint16_t, uint8_t) {
  screenSub = consoleSub = false;
  pairPending = false;
}

void onCccd(uint16_t, BLECharacteristic *chr, uint16_t v) {
  bool on = v & BLE_GATT_HVX_NOTIFICATION;
  if (chr == &screenChr) {
    screenSub = on;
    if (on) screenResend = true;
  } else {
    consoleSub = on;
  }
}

void onKeys(uint16_t, BLECharacteristic *, uint8_t *d, uint16_t n) { keyRing.put(d, n); }
void onConsoleRx(uint16_t, BLECharacteristic *, uint8_t *d, uint16_t n) { cmdRing.put(d, n); }

bool onPasskey(uint16_t, uint8_t const passkey[6], bool) {
  memcpy(pairCode, passkey, 6);
  pairCode[6] = 0;
  pairPending = true;
  return true;
}

void onPairDone(uint16_t, uint8_t status) {
  pairPending = false;
  if (status != BLE_GAP_SEC_STATUS_SUCCESS) pairFailed = true;
}

// ---- start / stop ----
void bleStart() {
  if (!bleBegun) {
    Bluefruit.autoConnLed(false);  // its LED pin isn't wired to anything we use
    Bluefruit.configPrphBandwidth(BANDWIDTH_MAX);
    if (!Bluefruit.begin()) {
      con.println("# bluetooth failed to start");
      return;
    }
    bleBegun = true;
    Bluefruit.setName(bleName);
    Bluefruit.setEventCallback(onBleEvent);
    Bluefruit.Security.setIOCaps(true, false, false);  // display only: the phone asks for the code
    Bluefruit.Security.setMITM(true);
    Bluefruit.Security.setPairPasskeyCallback(onPasskey);
    Bluefruit.Security.setPairCompleteCallback(onPairDone);
    Bluefruit.Periph.setConnectCallback(onConnect);
    Bluefruit.Periph.setDisconnectCallback(onDisconnect);
    Bluefruit.Periph.setConnInterval(6, 24);  // 7.5-30 ms

    screenSvc.begin();
    screenChr.setProperties(CHR_PROPS_NOTIFY);
    screenChr.setPermission(SECMODE_ENC_WITH_MITM, SECMODE_NO_ACCESS);
    screenChr.setMaxLen(246);
    screenChr.setCccdWriteCallback(onCccd, false);
    screenChr.begin();
    keysChr.setProperties(CHR_PROPS_WRITE | CHR_PROPS_WRITE_WO_RESP);
    keysChr.setPermission(SECMODE_NO_ACCESS, SECMODE_ENC_WITH_MITM);
    keysChr.setMaxLen(16);
    keysChr.setWriteCallback(onKeys, false);
    keysChr.begin();

    consoleSvc.begin();
    consoleTx.setProperties(CHR_PROPS_NOTIFY);
    consoleTx.setPermission(SECMODE_ENC_WITH_MITM, SECMODE_NO_ACCESS);
    consoleTx.setMaxLen(244);
    consoleTx.setCccdWriteCallback(onCccd, false);
    consoleTx.begin();
    consoleRx.setProperties(CHR_PROPS_WRITE | CHR_PROPS_WRITE_WO_RESP);
    consoleRx.setPermission(SECMODE_NO_ACCESS, SECMODE_ENC_WITH_MITM);
    consoleRx.setMaxLen(244);
    consoleRx.setWriteCallback(onConsoleRx, false);
    consoleRx.begin();

    Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
    Bluefruit.Advertising.addService(screenSvc);
    Bluefruit.ScanResponse.addName();
    Bluefruit.Advertising.setInterval(32, 1600);  // 20 ms for the first 30 s, then 1 s
    Bluefruit.Advertising.setFastTimeout(30);
  }
  Bluefruit.Advertising.restartOnDisconnect(true);
  Bluefruit.Advertising.start(0);
}

void bleStop() {
  if (!bleBegun) return;
  Bluefruit.Advertising.restartOnDisconnect(false);
  Bluefruit.Advertising.stop();
  if (BLEConnection *c = Bluefruit.Connection(Bluefruit.connHandle())) c->disconnect();
}

void runCommand(char *c, bool fromBle);

void bleLoop() {
  if (!bleBegun) return;
  if (pairFailed) {
    pairFailed = false;
    beep(400, 300);
  }
  if (pairPending && !pairShown) {  // the code has to be readable on the device itself
    pairShown = true;
    pairSince = millis();
    wakeScreen();
    drawScreen();
  }
  if (pairShown && (!pairPending || millis() - pairSince > PAIR_SHOW_MS)) {
    pairShown = pairPending = false;
    drawScreen();
  }
  if (!Bluefruit.connected()) {
    if (bleWasLinked) {  // queued notifications were dropped with the link
      txSent = txDone;
      dl.kind = 0;
      txRing.clear();
      cmdRing.clear();
      keyRing.clear();
    }
    bleWasLinked = false;
    if (forgetBonds) {
      forgetBonds = false;
      Bluefruit.Periph.clearBonds();
      if (settings.ble) bleStart();
      forgotAt = millis();
      drawScreen();
    }
    return;
  }
  bleWasLinked = true;

  static char line[48];
  static size_t len = 0;
  int ch;
  while ((ch = cmdRing.get()) >= 0) {
    if (ch == '\r' || ch == '\n') {
      line[len] = 0;
      if (len) runCommand(line, true);
      len = 0;
    } else if (len < sizeof(line) - 1) {
      line[len++] = ch;
    }
  }
  if (!consoleSub) {
    dl.kind = 0;
    txRing.clear();
  }
  pumpDownload();
  if (screenSub) pushScreen();
  if (consoleSub) pushConsole();
}

// ---------- UI ----------
void drawList() {
  char line[32];
  // frame counters are on the diagnostics screen
  char sats[8] = "S-";  // GPS off
  if (settings.gps) snprintf(sats, sizeof(sats), "S%lu", (unsigned long)gps.satellites.value());
  snprintf(line, sizeof(line), "N%d  %s  %.2fV%s%s", meterCount, sats, battV, battLow ? " LOW" : "",
           bleLinked() ? " BT" : "");
  oled.drawStr(0, 7, line);
  oled.drawHLine(0, 9, 128);

  if (meterCount == 0) {
    oled.drawStr(0, 24, "Listening T1 868.95...");
    oled.drawStr(0, 34, settings.sortByRssi ? "sort: best RSSI" : "sort: last seen");
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
    snprintf(line, sizeof(line), "%s%c%9s %4d%s", name, flag, val, m.lastRssi, m.thisSession ? "" : "*");
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

// Everything the serial 's' command covers, for checking without a computer. Lines fit 25 chars.
void drawDiag() {
  char line[32];
  oled.drawStr(0, 7, "Diag  built " __DATE__);
  oled.drawHLine(0, 9, 128);
  snprintf(line, sizeof(line), "ok %lu  err %lu", (unsigned long)okCount, (unsigned long)errCount);
  oled.drawStr(0, 18, line);
  if (!settings.gps) snprintf(line, sizeof(line), "GPS off%s", gpsTimeTrusted ? ", clock running" : "");
  else snprintf(line, sizeof(line), "GPS %lu sats, %s", (unsigned long)gps.satellites.value(),
                gps.location.isValid() ? (gpsTimeTrusted ? "fix" : "fix, no time") : "no fix");
  oled.drawStr(0, 27, line);
  snprintf(line, sizeof(line), "batt %.2fV%s%s", battV, battLow ? " LOW" : "", driveToHost ? " PC drive" : "");
  oled.drawStr(0, 36, line);
  if (qspiOk) snprintf(line, sizeof(line), "flash %luK ring %s", (unsigned long)(flash.size() / 1024), ringOk ? "ok" : "off");
  else snprintf(line, sizeof(line), "FLASH FAIL id %06lx", (unsigned long)chipJedecId());
  oled.drawStr(0, 45, line);
  snprintf(line, sizeof(line), "snap %lu  %d labels%s", (unsigned long)ring.seq, labelCount, fsOk ? "" : " NOSAVE");
  oled.drawStr(0, 54, line);
  uint32_t up = millis() / 60000;
  snprintf(line, sizeof(line), "log %u+%uB up %luh%02lum", (unsigned)historyLog.len, (unsigned)rawLog.len,
           (unsigned long)(up / 60), (unsigned long)(up % 60));
  oled.drawStr(0, 63, line);
}

enum { SET_BT, SET_GPS, SET_SCREEN, SET_BEEPS, SET_SORT, SET_FORGET, N_SETTINGS };
bool settingsOpen = false;
int setSel = 0;

void drawSettings() {
  char line[32];
  snprintf(line, sizeof(line), "Settings  %s", bleName);
  oled.drawStr(0, 7, line);
  oled.drawHLine(0, 9, 128);
  for (int i = 0; i < N_SETTINGS; i++) {
    const char *name = "", *val = "";
    switch (i) {
      case SET_BT: name = "Bluetooth"; val = !settings.ble ? "off" : bleLinked() ? "linked" : "on"; break;
      case SET_GPS: name = "GPS"; val = settings.gps ? "on" : "off"; break;
      case SET_SCREEN: name = "Screen off"; val = SCREEN_OFF_NAMES[settings.screenOff]; break;
      case SET_BEEPS: name = "Beeps"; val = settings.beeps ? "on" : "off"; break;
      case SET_SORT: name = "Sort"; val = settings.sortByRssi ? "best RSSI" : "last seen"; break;
      case SET_FORGET: name = "Forget phones"; val = forgotAt && millis() - forgotAt < 3000 ? "done" : "<>"; break;
    }
    snprintf(line, sizeof(line), "%-14s%10s", name, val);
    int y = 18 + i * 9;
    if (i == setSel) {
      oled.drawBox(0, y - 7, 128, 9);
      oled.setDrawColor(0);
      oled.drawStr(1, y, line);
      oled.setDrawColor(1);
    } else {
      oled.drawStr(1, y, line);
    }
  }
}

void drawPairing() {
  oled.drawStr(0, 7, "Bluetooth pairing");
  oled.drawHLine(0, 9, 128);
  oled.drawStr(0, 20, "Enter this code on");
  oled.drawStr(0, 29, "the phone:");
  oled.setFont(u8g2_font_10x20_tn);
  oled.drawStr(34, 54, pairCode);
  oled.setFont(u8g2_font_5x8_tf);
}

// Also drawn with the screen off while the phone is mirroring it.
void drawScreen() {
  if (!screenOn && !screenSub) return;
  oled.clearBuffer();
  if (pairShown) drawPairing();
  else if (settingsOpen) drawSettings();
  else if (diag) drawDiag();
  else if (detail && meterCount > 0) drawDetail();
  else drawList();
  if (screenOn) oled.sendBuffer();
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

// The initial press, not a repeat from holding the button.
bool firstFire(const Btn &b) { return b.lastFire == b.changed; }

void settingsChanged() {
  settingsDirty = true;
  lastSettingsEdit = millis();
}

void changeSetting(int i, int dir) {
  switch (i) {
    case SET_BT:
      settings.ble = !settings.ble;
      if (settings.ble) bleStart();
      else bleStop();
      break;
    case SET_GPS:
      settings.gps = !settings.gps;
      applyGps();
      break;
    case SET_SCREEN:
      settings.screenOff = constrain(settings.screenOff + dir, 0, N_SCREEN_OFF - 1);
      break;
    case SET_BEEPS: settings.beeps = !settings.beeps; break;
    case SET_SORT:
      settings.sortByRssi = !settings.sortByRssi;
      sortMeters();
      break;
    case SET_FORGET:
      // Cleared by bleLoop once the link is down: a disconnect still writes to the phone's bond.
      if (!bleBegun) return;
      bleStop();
      forgetBonds = true;
      return;
  }
  settingsChanged();
}

// A key pressed on the phone page: 'U' 'D' 'L' 'R' 'P' 'B', | 0x80 = repeat.
// It doesn't wake the screen or restart its timeout: the phone shows its own copy.
bool remoteKey(bool &up, bool &down, bool &l, bool &r, bool &press, bool &user, bool &lrOnce) {
  int k = keyRing.get();
  if (k < 0) return false;
  bool rep = k & 0x80;
  switch (k & 0x7f) {
    case 'U': up = true; break;
    case 'D': down = true; break;
    case 'L': l = true; break;
    case 'R': r = true; break;
    case 'P': press = !rep; break;
    case 'B': user = !rep; break;
    default: return false;
  }
  lrOnce = (l || r) && !rep;
  return true;
}

void handleButtons() {
  bool up = pressed(bUp), down = pressed(bDown), l = pressed(bLeft), r = pressed(bRight), press = pressed(bPress),
       user = pressed(bUser);
  bool lrOnce = (l && firstFire(bLeft)) || (r && firstFire(bRight));  // holding mustn't flip screens
  if (up || down || l || r || press || user) {
    lastInput = millis();
    if (!screenOn || pairShown) {  // the press that wakes the screen (or hides the code) does nothing else
      setScreen(true);
      pairShown = pairPending = false;
      drawScreen();
      return;
    }
  } else if (!remoteKey(up, down, l, r, press, user, lrOnce)) {
    return;
  }
  bool lOnce = l && lrOnce, rOnce = r && lrOnce;
  if (settingsOpen) {
    if (up && setSel > 0) setSel--;
    if (down && setSel < N_SETTINGS - 1) setSel++;
    if (lrOnce) changeSetting(setSel, r ? 1 : -1);
    if (press) settingsOpen = false;
  } else if (diag) {
    if (lrOnce || press) diag = false;
  } else {
    if (up && sel > 0) selId = meters[order[--sel]].id;
    if (down && sel < meterCount - 1) selId = meters[order[++sel]].id;
    if (detail && meterCount > 0) {
      if (l || r) stepLabel(meters[order[sel]].id, r ? 1 : -1);
    } else if (lOnce) {
      settingsOpen = true;
    } else if (rOnce) {
      diag = true;
    }
    if (press) detail = !detail;
  }
  if (user) {
    settings.sortByRssi = !settings.sortByRssi;
    sortMeters();
    settingsChanged();
  }
  drawScreen();
}

// ---------- serial commands ----------
char cmd[48];
size_t cmdLen = 0;

void runCommand(char *c, bool fromBle = false) {
  // Dumps asked for by the phone stream a chunk at a time; on serial they print all at once.
  if (fromBle && (!strcmp(c, "d") || !strcmp(c, "h") || !strcmp(c, "r"))) {
    startDownload(c[0]);
  } else if (!strcmp(c, "d")) {
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
    con.println("# survey cleared (labels kept)");
  } else if (c[0] == 'l' && (c[1] == ' ' || c[1] == 0)) {
    // l <id> [label]   - set or remove a label
    char *idStr = strtok(c + 1, " ");
    char *text = strtok(nullptr, "");
    if (!idStr) {
      con.println("# usage: l <id> [label]");
      return;
    }
    uint32_t id = strtoul(idStr, nullptr, 16);
    setLabel(id, text);
    con.printf("# %08lx -> %s\n", (unsigned long)id, text ? text : "(removed)");
    sortMeters();
  } else if (!strcmp(c, "L")) {
    con.println("# labels");
    for (int i = 0; i < labelCount; i++)
      con.printf("%08lx,%s%s\n", (unsigned long)labels[i].id, labels[i].text, hasKey(labels[i]) ? ",key" : "");
    con.println("# end");
  } else if (!strcmp(c, "s")) {
    con.println("# status");
    con.printf("build %s %s\n", __DATE__, __TIME__);
    con.printf("qspi %s, jedec %06lx, %lu KB\n", qspiOk ? "ok" : "FAILED", (unsigned long)chipJedecId(),
                  (unsigned long)(flash.size() / 1024));
    con.printf("ring %s: %lu blocks, snapshot %lu, next block %lu\n", ringOk ? "ok" : "off", (unsigned long)ring.blocks,
                  (unsigned long)ring.seq, (unsigned long)ring.next);
    con.printf("internal flash %s, meters %d, labels %d, log rows waiting %u+%u bytes\n", fsOk ? "ok" : "FAILED",
                  meterCount, labelCount, (unsigned)historyLog.len, (unsigned)rawLog.len);
    con.printf("usb mounted %d, drive %s\n", TinyUSBDevice.mounted(), driveToHost ? "with computer" : "held");
    con.printf("battery %.2f V%s, frames ok %lu err %lu\n", battV, battLow ? " LOW" : "", (unsigned long)okCount,
                  (unsigned long)errCount);
    con.printf("gps chars %lu, sentences ok %lu bad %lu, sats %lu, fix %s, time %s\n",
                  (unsigned long)gps.charsProcessed(), (unsigned long)gps.passedChecksum(),
                  (unsigned long)gps.failedChecksum(), (unsigned long)gps.satellites.value(),
                  gps.location.isValid() ? "yes" : "no", gpsTimeTrusted ? "trusted" : "not yet");
    con.printf("gps fix age %lu ms, hdop %.1f\n", (unsigned long)gps.location.age(), gps.hdop.hdop());
    con.printf("bluetooth %s %s, gps %s, screen off %s, beeps %s\n", bleName,
               !settings.ble ? "off" : bleLinked() ? "linked" : "advertising", settings.gps ? "on" : "off",
               SCREEN_OFF_NAMES[settings.screenOff], settings.beeps ? "on" : "off");
    con.println("# end");
  } else if (!strcmp(c, "h")) {
    printLog(historyLog);
    Serial.println("# end");
  } else if (!strcmp(c, "r")) {
    printLog(rawLog);
    Serial.println("# end");
  } else if (!strcmp(c, "HCLEAR")) {
    if (driveToHost) {
      con.println("# unplug the USB drive first (or eject it and use a charger)");
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
    con.println("# history and raw logs deleted");
  } else if (!strcmp(c, "FORMAT")) {
    driveToHost = false;
    delay(200);  // let any USB read in progress finish before erasing
    if (qspiOk) fat12::format(flash, DRIVE_LABEL, NRF_FICR->DEVICEID[0]);
    InternalFS.format();
    con.println("# flash formatted, rebooting");
    delay(100);
    NVIC_SystemReset();
  } else if (c[0] == 'k' && (c[1] == ' ' || c[1] == 0)) {
    // k <id> [32 hex digits]  - set (or with no key, clear) a meter's AES key, e.g. for Sensus
    char *idStr = strtok(c + 1, " ");
    char *hexKey = strtok(nullptr, " ");
    if (!idStr) {
      con.println("# usage: k <id> [32 hex digits]");
      return;
    }
    uint32_t id = strtoul(idStr, nullptr, 16);
    uint8_t key[16] = {0};
    if (hexKey) {
      bool ok = strlen(hexKey) == 32;
      for (int i = 0; ok && i < 32; i++) ok = isxdigit((unsigned char)hexKey[i]);
      if (!ok) {
        con.println("# key must be exactly 32 hex digits - nothing changed");
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
    con.printf("# key for %08lx: %s\n", (unsigned long)id, hexKey ? (l ? "set" : "NOT set") : "cleared");
  } else if (*c) {
    con.println("# commands: s=status  d=dump  c=clear survey  l <id> [label]  L=list labels  k <id> [hexkey]  h=history"
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
  digitalWrite(PIN_GPS_STANDBY, HIGH);  // awake until the settings are loaded
  Serial1.begin(GPS_BAUDRATE);

  for (auto *b : allBtns) pinMode(b->pin, INPUT_PULLUP);
  analogReadResolution(12);
  battV = batteryVolts();

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
  loadSettings();
  applyGps();
  sortMeters();
  snprintf(bleName, sizeof(bleName), "WMBUS-%04lX", (unsigned long)(NRF_FICR->DEVICEID[0] & 0xFFFF));
  // Storage status for a moment, so problems show without a serial terminal.
  char line[32];
  oled.drawStr(0, 22, "built " __DATE__);
  if (qspiOk) snprintf(line, sizeof(line), "flash ok %luK ring %lu", (unsigned long)(flash.size() / 1024),
                       (unsigned long)ring.blocks);
  else snprintf(line, sizeof(line), "FLASH FAIL id %06lx", (unsigned long)chipJedecId());
  oled.drawStr(0, 32, line);
  snprintf(line, sizeof(line), "%d meters, %d labels", meterCount, labelCount);
  oled.drawStr(0, 42, line);
  oled.sendBuffer();
  delay(2000);
  radioInit();
  // After fsInit: the snapshot ring seeds itself from the RNG, which the SoftDevice takes over.
  if (settings.ble) bleStart();
  beep(2000, 60);
  lastInput = millis();
  con.println("# wM-Bus T1 survey ready. Send 'help' for commands");
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
  bleLoop();
  handleBeeps();

  uint32_t now = millis();
  if (labelsDirty && now - lastLabelEdit > LABEL_SAVE_MS) saveLabels();
  if (settingsDirty && now - lastSettingsEdit > LABEL_SAVE_MS) saveSettings();
  static uint32_t lastLogCheck = 0;
  if (now - lastLogCheck > 1000) {
    lastLogCheck = now;
    logHistory();
  }
  handleUsbDrive();
  if (surveyDirty && (ringOk || !driveToHost) && now - lastSurveySave > (ringOk ? SURVEY_SAVE_MS : FALLBACK_SAVE_MS))
    saveSurvey();

  uint32_t screenOffMs = SCREEN_OFF_CHOICES[settings.screenOff];
  if (screenOn && screenOffMs && !pairShown && now - lastInput > screenOffMs) setScreen(false);
  static uint32_t lastDraw = 0;
  if (now - lastDraw > 500) {
    lastDraw = now;
    digitalWrite(PIN_LED1, LOW);
    updateBattery();
    drawScreen();
  }
  delay(1);
}
