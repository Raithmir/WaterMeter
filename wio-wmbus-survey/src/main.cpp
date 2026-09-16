// Wio Tracker L1 Pro - wM-Bus T1/C1 walk-around survey tool (IZAR readings decoded)
// SX1262 in GFSK, 868.95 MHz, 100 kbps, sync 0x543D, fixed 255-byte capture.

#include <Arduino.h>
#include <Wire.h>
#include <RadioLib.h>
#include <U8g2lib.h>
#include <TinyGPSPlus.h>
#include <InternalFileSystem.h>
#include <cmath>
#include "wmbus.h"

using namespace Adafruit_LittleFS_Namespace;

// ---------- config ----------
#define MAX_METERS 64
#define MAX_LABELS 64
#define MAX_SAMPLES 5            // strongest GPS-tagged receptions kept per meter
#define ROWS 6
#define DISPLAY_SH1106 1         // L1 uses an SH1106 (per Zephyr board file); 0 = SSD1306
#define SURVEY_SAVE_MS 30000     // save survey at most this often
#define LABEL_SAVE_MS 3000       // save labels this long after the last edit
#define FS_MARKER "/wmsurvey"    // flash is formatted once if this is missing
#define SURVEY_FILE "/survey.bin"
#define LABEL_FILE "/labels.bin"
#define FILE_VERSION 2

// ---------- hardware ----------
SX1262 radio = new Module(SX126X_CS, SX126X_DIO1, SX126X_RESET, SX126X_BUSY);
#if DISPLAY_SH1106
U8G2_SH1106_128X64_NONAME_F_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE);
#else
U8G2_SSD1306_128X64_NONAME_F_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE);
#endif
TinyGPSPlus gps;

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
  // runtime only (reset on load)
  uint32_t lastSeen;
  bool thisSession;
};

struct Label {
  uint32_t id;
  char text[8];
};

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
bool fsOk = false;

volatile bool rxFlag = false;
uint8_t raw[255];

void onRx() { rxFlag = true; }

// ---------- helpers ----------
float batteryVolts() { return analogRead(PIN_VBAT) * AREF_VOLTAGE / 4095.0f * ADC_MULTIPLIER; }

void beep(uint16_t freq, uint16_t ms) { tone(PIN_BUZZER, freq, ms); }

// Diehl PRIOS frames don't carry a standard type byte, so decoded IZAR meters are "water".
void typeNames(const Meter &m, const char *&shortName, const char *&longName) {
  if (m.hasLitres) {
    shortName = longName = "water";
    return;
  }
  wmbus::deviceType(m.type, shortName, longName);
}

const char *modeName(const Meter &m) { return wmbus::modeStr((wmbus::Mode)m.mode); }

uint32_t gpsEpoch() {
  if (!gps.date.isValid() || !gps.time.isValid() || gps.date.year() < 2024) return 0;
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

void formatUtc(uint32_t t, char *out, size_t n) {
  long days = t / 86400;
  uint32_t secs = t % 86400;
  // civil from days
  days += 719468;
  long era = days / 146097;
  unsigned doe = days - era * 146097;
  unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  unsigned mp = (5 * doy + 2) / 153;
  unsigned d = doy - (153 * mp + 2) / 5 + 1;
  unsigned m = mp < 10 ? mp + 3 : mp - 9;
  snprintf(out, n, "%02lu:%02lu %02u/%02u UTC", (unsigned long)(secs / 3600), (unsigned long)(secs / 60 % 60), d, m);
}

// ---------- labels ----------
const char *getLabel(uint32_t id) {
  for (int i = 0; i < labelCount; i++)
    if (labels[i].id == id) return labels[i].text;
  return nullptr;
}

void setLabel(uint32_t id, const char *text) {
  int idx = -1;
  for (int i = 0; i < labelCount; i++)
    if (labels[i].id == id) { idx = i; break; }
  if (!text || !*text) {  // remove
    if (idx >= 0) labels[idx] = labels[--labelCount];
  } else {
    if (idx < 0) {
      if (labelCount >= MAX_LABELS) return;
      idx = labelCount++;
      labels[idx].id = id;
    }
    strncpy(labels[idx].text, text, sizeof(labels[idx].text) - 1);
    labels[idx].text[sizeof(labels[idx].text) - 1] = 0;
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
void addSample(Meter &m, int16_t rssi) {
  if (!gps.location.isValid() || gps.location.age() > 5000) return;
  Sample s = {(int32_t)lround(gps.location.lat() * 1e7), (int32_t)lround(gps.location.lng() * 1e7), rssi};
  if (m.nSamples < MAX_SAMPLES) {
    m.samples[m.nSamples++] = s;
    return;
  }
  int weakest = 0;
  for (int i = 1; i < MAX_SAMPLES; i++)
    if (m.samples[i].rssi < m.samples[weakest].rssi) weakest = i;
  if (rssi > m.samples[weakest].rssi) m.samples[weakest] = s;
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

bool saveFile(const char *path, uint32_t magic, const void *data, size_t itemSize, int count) {
  if (!fsOk) return false;
  const char *tmp = "/tmp.bin";
  InternalFS.remove(tmp);
  File f(InternalFS);
  if (!f.open(tmp, FILE_O_WRITE)) return false;
  FileHeader h = {magic, FILE_VERSION, (uint16_t)count};
  bool ok = f.write((const uint8_t *)&h, sizeof(h)) == sizeof(h);
  ok = ok && f.write((const uint8_t *)data, itemSize * count) == itemSize * count;
  f.close();
  if (!ok) {
    InternalFS.remove(tmp);
    return false;
  }
  InternalFS.remove(path);
  return InternalFS.rename(tmp, path);
}

int loadFile(const char *path, uint32_t magic, void *data, size_t itemSize, int maxCount) {
  if (!fsOk || !InternalFS.exists(path)) return 0;
  File f(InternalFS);
  if (!f.open(path, FILE_O_READ)) return 0;
  FileHeader h;
  int n = 0;
  if (f.read(&h, sizeof(h)) == sizeof(h) && h.magic == magic && h.version == FILE_VERSION &&
      h.count <= maxCount && f.size() == sizeof(h) + itemSize * h.count) {
    if (f.read(data, itemSize * h.count) == (int)(itemSize * h.count)) n = h.count;
  }
  f.close();
  return n;
}

const uint32_t MAGIC_SURVEY = 0x53525659;  // "SRVY"
const uint32_t MAGIC_LABELS = 0x4C41424C;  // "LABL"

void saveSurvey() {
  bool ok = saveFile(SURVEY_FILE, MAGIC_SURVEY, meters, sizeof(Meter), meterCount);
  Serial.printf("# survey saved (%d meters) %s\n", meterCount, ok ? "ok" : "FAILED");
  surveyDirty = false;
  lastSurveySave = millis();
}

void saveLabels() {
  bool ok = saveFile(LABEL_FILE, MAGIC_LABELS, labels, sizeof(Label), labelCount);
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
    File f(InternalFS);
    if (fsOk && f.open(FS_MARKER, FILE_O_WRITE)) {
      f.write("1");
      f.close();
    }
  }
  if (!fsOk) {
    Serial.println("# flash filesystem unavailable - nothing will be saved");
    return;
  }
  meterCount = loadFile(SURVEY_FILE, MAGIC_SURVEY, meters, sizeof(Meter), MAX_METERS);
  for (int i = 0; i < meterCount; i++) {
    meters[i].lastSeen = 0;
    meters[i].thisSession = false;
  }
  labelCount = loadFile(LABEL_FILE, MAGIC_LABELS, labels, sizeof(Label), MAX_LABELS);
  for (int i = 0; i < labelCount; i++) {
    labels[i].text[sizeof(labels[i].text) - 1] = 0;
    int n = atoi(labels[i].text);
    if (n > lastLabelNum) lastLabelNum = n;
  }
  Serial.printf("# loaded %d meters, %d labels\n", meterCount, labelCount);
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

void printCsvHeader() {
  Serial.println("id,label,mode,mfct,type,ver,litres,alarms,rssi,best_rssi,count,utc,lat,lon,spread_m,samples");
}

void printMeterCsv(const Meter &m) {
  const char *lbl = getLabel(m.id);
  char at[48] = "";
  const char *sn, *ln;
  typeNames(m, sn, ln);
  Serial.printf("%08lx,%s,%s,%s,%s (%02x),%02x,", (unsigned long)m.id, lbl ? lbl : "", modeName(m), m.mfct, ln,
                m.type, m.ver);
  if (m.hasLitres) {
    Serial.print(m.litres);
    wmbus::alarmText(m.alarms, at, sizeof(at));
  }
  Serial.printf(",%s,%d,%d,%u,%lu,", at, m.lastRssi, m.bestRssi, m.count, (unsigned long)m.utc);
  double lat, lon;
  float spread;
  if (estimatePosition(m, lat, lon, spread)) {
    Serial.print(lat, 7);
    Serial.print(',');
    Serial.print(lon, 7);
    Serial.printf(",%.1f,%u", spread, m.nSamples);
  } else {
    Serial.print(",,,0");
  }
  Serial.println();
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

  bool isNew = false;
  if (idx < 0) {
    if (meterCount < MAX_METERS) idx = meterCount++;
    else if ((idx = evictSlot()) < 0) return;
    Meter &m = meters[idx];
    memset(&m, 0, sizeof(m));
    m.id = id;
    m.bestRssi = -200;
    isNew = true;
  }

  Meter &m = meters[idx];
  if (!m.thisSession) isNew = true;  // first time heard since power-on: still worth a beep
  t.mfct(m.mfct);
  m.ver = t.version();
  m.type = t.type();
  m.mode = (uint8_t)mode;

  uint32_t l;
  uint16_t prevAlarms = m.alarms;
  if (wmbus::decodeIzar(t, l)) {
    m.litres = l;
    m.hasLitres = true;
    m.alarms = wmbus::izarAlarms(t);
  }
  bool newLeak = (m.alarms & wmbus::ALM_LEAK_NOW) && !(prevAlarms & wmbus::ALM_LEAK_NOW);
  m.lastRssi = rssi;
  if (rssi > m.bestRssi) m.bestRssi = rssi;
  m.count++;
  m.lastSeen = millis();
  m.thisSession = true;
  uint32_t now = gpsEpoch();
  if (now) m.utc = now;
  addSample(m, rssi);
  surveyDirty = true;

  printMeterCsv(m);
  if (newLeak) {
    for (int i = 0; i < 3; i++) { beep(3200, 120); delay(180); }
  } else if (isNew) {
    beep(2700, 40);
  }
  sortMeters();
}

// ---------- UI ----------
void drawList() {
  char line[32];
  snprintf(line, sizeof(line), "N%d ok%lu er%lu S%lu %.1fV", meterCount, (unsigned long)okCount,
           (unsigned long)(errCount > 999 ? 999 : errCount), (unsigned long)gps.satellites.value(), batteryVolts());
  oled.drawStr(0, 7, line);
  oled.drawHLine(0, 9, 128);

  if (meterCount == 0) {
    oled.drawStr(0, 24, "Listening T1 868.95...");
    oled.drawStr(0, 34, sortByRssi ? "sort: best RSSI" : "sort: last seen");
    if (!fsOk) oled.drawStr(0, 44, "flash error: no saving");
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
    if (m.alarms & (wmbus::ALM_LEAK_NOW | wmbus::ALM_BLOCKED | wmbus::ALM_BACKFLOW | wmbus::ALM_SUBMARINE)) {
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
    oled.drawStr(0, 43, "no GPS fix yet");
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
    beep(2000, 15);
  }
  if (pressed(bUser)) {
    sortByRssi = !sortByRssi;
    sortMeters();
    redraw = true;
    beep(1500, 15);
  }
  if (redraw) drawScreen();
}

// ---------- serial commands ----------
char cmd[48];
size_t cmdLen = 0;

void runCommand(char *c) {
  if (!strcmp(c, "d")) {
    Serial.println("# dump");
    printCsvHeader();
    for (int i = 0; i < meterCount; i++) printMeterCsv(meters[order[i]]);
    Serial.println("# end");
  } else if (!strcmp(c, "c")) {
    meterCount = 0;
    sel = top = 0;
    selId = 0;
    okCount = errCount = 0;
    saveSurvey();
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
    for (int i = 0; i < labelCount; i++) Serial.printf("%08lx,%s\n", (unsigned long)labels[i].id, labels[i].text);
    Serial.println("# end");
  } else if (!strcmp(c, "FORMAT")) {
    InternalFS.format();
    Serial.println("# flash formatted, rebooting");
    delay(100);
    NVIC_SystemReset();
  } else if (*c) {
    Serial.println("# commands: d=dump  c=clear survey  l <id> [label]  L=list labels  FORMAT=erase all");
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
  radioInit();
  beep(2000, 60);
  Serial.println("# wM-Bus T1 survey ready. Send 'help' for commands");
  printCsvHeader();
}

void loop() {
  while (Serial1.available()) gps.encode(Serial1.read());

  if (rxFlag) {
    rxFlag = false;
    handlePacket();
  }

  handleButtons();
  handleSerial();

  uint32_t now = millis();
  if (labelsDirty && now - lastLabelEdit > LABEL_SAVE_MS) saveLabels();
  if (surveyDirty && now - lastSurveySave > SURVEY_SAVE_MS) saveSurvey();

  static uint32_t lastDraw = 0;
  if (now - lastDraw > 500) {
    lastDraw = now;
    digitalWrite(PIN_LED1, LOW);
    drawScreen();
  }
  delay(1);
}
