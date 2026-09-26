// Survey table entry, time, position estimate and CSV rows.
// Plain C++, no Arduino dependencies, so it can be unit-tested on a PC (test/test_survey.cpp).
#pragma once
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "wmbus.h"

#define MAX_SAMPLES 5                // strongest GPS-tagged receptions kept per meter
#define LOG_INTERVAL_S (6 * 3600UL)  // a meter heard again within this counts as the same walk

namespace survey {

struct Sample {
  int32_t lat, lon;  // degrees * 1e7
  int16_t rssi;
};

// Saved as-is in the survey snapshots.
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
  uint32_t billingLitres;  // reading on the billing date (0 = unknown)
  uint32_t billingDate;    // YYYYMMDD billing date (0 = unknown or not reached)
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
// Saved surveys are read back byte for byte. Changing Meter means a new SURVEY_VERSION and a
// conversion in main.cpp's loadSnapshot(), then updating this size.
static_assert(sizeof(Meter) == 132, "Meter layout changed: see comment above");

// ---------- time ----------
// Seconds since 1970 (days from civil, Howard Hinnant).
inline uint32_t epochFromCivil(int y, int m, int d, int hh, int mm, int ss) {
  y -= m <= 2;
  int era = y / 400;
  unsigned yoe = y - era * 400;
  unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  long days = era * 146097L + doe - 719468L;
  return days * 86400UL + hh * 3600UL + mm * 60UL + ss;
}

inline void civilDate(uint32_t t, unsigned &y, unsigned &m, unsigned &d) {
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

// 14:05 23/09 UTC
inline void formatUtc(uint32_t t, char *out, size_t n) {
  unsigned y, m, d;
  civilDate(t, y, m, d);
  uint32_t secs = t % 86400;
  snprintf(out, n, "%02lu:%02lu %02u/%02u UTC", (unsigned long)(secs / 3600), (unsigned long)(secs / 60 % 60), d, m);
}

// 2026-09-23T14:05:12Z
inline void isoUtc(uint32_t t, char *out, size_t n) {
  unsigned y, m, d;
  civilDate(t, y, m, d);
  uint32_t secs = t % 86400;
  snprintf(out, n, "%04u-%02u-%02uT%02lu:%02lu:%02luZ", y, m, d, (unsigned long)(secs / 3600),
           (unsigned long)(secs / 60 % 60), (unsigned long)(secs % 60));
}

// ---------- position estimate ----------
// Keeps the MAX_SAMPLES strongest receptions. true if this one was kept.
inline bool addSample(Meter &m, int32_t lat, int32_t lon, int16_t rssi) {
  Sample s = {lat, lon, rssi};
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
inline bool estimatePosition(const Meter &m, double &lat, double &lon, float &spread) {
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

// ---------- names ----------
// Diehl PRIOS frames don't carry a standard type byte, so decoded IZAR meters are "water".
inline void typeNames(const Meter &m, const char *&shortName, const char *&longName) {
  if (m.hasLitres) {
    shortName = longName = "water";
    return;
  }
  wmbus::deviceType(m.type, shortName, longName);
}

inline const char *modeName(const Meter &m) { return wmbus::modeStr((wmbus::Mode)m.mode); }

// Number printed on the meter (IZAR meters made by Sappel only), else empty. out needs 12 bytes.
inline bool meterSerial(const Meter &m, char out[12]) {
  out[0] = 0;
  return m.hasIzarInfo && wmbus::izarSerial(m.mfct, m.id, m.ver, m.type, out);
}

// ---------- logging decisions ----------
// One history row per meter per walk, plus one whenever its alarms change.
inline bool historyDue(const Meter &m, uint32_t now) {
  return m.heardSinceLog && (now >= m.loggedUtc + LOG_INTERVAL_S || m.alarms != m.loggedAlarms);
}

// ---------- CSV rows ----------
// Each writes one row without the line ending into out and returns its length. Labels are
// already free of commas and quotes (main.cpp's setLabel), and no other field can hold one.
static const char SURVEY_HEADER[] =
    "id,label,serial,mode,mfct,type,ver,litres,alarms,rssi,best_rssi,count,utc,lat,lon,spread_m,samples,"
    "billing_litres,billing_date,battery_years,period_s";
static const char HISTORY_HEADER[] =
    "utc,id,label,mfct,type,litres,billing_litres,billing_date,alarms,battery_years,rssi,serial";
static const char RAW_HEADER[] = "utc,id,label,mode,mfct,type,rssi,telegram";

// Keeps appending after a truncated write without running past the end.
struct Out {
  char *buf;
  size_t cap, len = 0;
  Out(char *b, size_t n) : buf(b), cap(n) { if (n) b[0] = 0; }
  template <typename... A>
  void f(const char *fmt, A... a) {
    if (len + 1 >= cap) return;
    int w = snprintf(buf + len, cap - len, fmt, a...);
    if (w > 0) len = len + w < cap ? len + w : cap - 1;
  }
  void s(const char *str) { f("%s", str); }
};

inline void billingCols(Out &o, const Meter &m) {
  if (m.hasIzarInfo && m.billingDate)
    o.f("%lu,%04lu-%02lu-%02lu", (unsigned long)m.billingLitres, (unsigned long)(m.billingDate / 10000),
        (unsigned long)(m.billingDate / 100 % 100), (unsigned long)(m.billingDate % 100));
  else
    o.s(",");
}

// survey.csv / serial dump row.
inline size_t surveyRow(char *out, size_t n, const Meter &m, const char *label) {
  Out o(out, n);
  const char *sn, *ln;
  typeNames(m, sn, ln);
  char at[48] = "", serial[12];
  meterSerial(m, serial);
  o.f("%08lx,%s,%s,%s,%s,%s (%02x),%02x,", (unsigned long)m.id, label ? label : "", serial, modeName(m), m.mfct, ln,
      m.type, m.ver);
  if (m.hasLitres) {
    o.f("%lu", (unsigned long)m.litres);
    wmbus::alarmText(m.alarms, at, sizeof(at));
  }
  char ts[48] = "";
  if (m.utc) isoUtc(m.utc, ts, sizeof(ts));
  o.f(",%s,%d,%d,%u,%s,", at, m.lastRssi, m.bestRssi, m.count, ts);
  double lat, lon;
  float spread;
  if (estimatePosition(m, lat, lon, spread)) o.f("%.7f,%.7f,%.1f,%u", lat, lon, spread, m.nSamples);
  else o.s(",,,0");
  o.s(",");
  if (m.hasIzarInfo) {
    billingCols(o, m);
    o.f(",%.1f,%lu", m.battHalfYears / 2.0, (unsigned long)m.periodS);
  } else {
    o.s(",,,");
  }
  return o.len;
}

// history.csv row, stamped `now`.
inline size_t historyRow(char *out, size_t n, const Meter &m, const char *label, uint32_t now) {
  Out o(out, n);
  char ts[48], at[48] = "";
  isoUtc(now, ts, sizeof(ts));
  const char *sn, *ln;
  typeNames(m, sn, ln);
  o.f("%s,%08lx,%s,%s,%s,", ts, (unsigned long)m.id, label ? label : "", m.mfct, ln);
  if (m.hasLitres) {
    o.f("%lu", (unsigned long)m.litres);
    wmbus::alarmText(m.alarms, at, sizeof(at));
  }
  o.s(",");
  billingCols(o, m);
  o.f(",%s,", at);
  if (m.hasIzarInfo) o.f("%.1f", m.battHalfYears / 2.0);
  char serial[12];
  meterSerial(m, serial);
  o.f(",%d,%s", m.lastRssi, serial);  // last, so older files' columns still line up
  return o.len;
}

// raw.csv row: the telegram with CRCs removed (as wmbusmeters takes it). now = 0 leaves the time blank.
inline size_t rawRow(char *out, size_t n, const Meter &m, const char *label, const wmbus::Telegram &t, uint32_t now) {
  Out o(out, n);
  char ts[48] = "";
  if (now) isoUtc(now, ts, sizeof(ts));
  const char *sn, *ln;
  typeNames(m, sn, ln);
  o.f("%s,%08lx,%s,%s,%s,%s (%02x),%d,", ts, (unsigned long)m.id, label ? label : "", modeName(m), m.mfct, ln, m.type,
      m.lastRssi);
  for (size_t i = 0; i < t.len; i++) o.f("%02x", t.data[i]);
  return o.len;
}

// Room for any row above (a raw row holds up to 255 telegram bytes as hex).
static const size_t ROW_MAX = 96 + 2 * wmbus::MAX_FRAME;

}  // namespace survey
