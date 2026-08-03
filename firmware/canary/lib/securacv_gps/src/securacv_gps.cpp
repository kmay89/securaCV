/*
 * SecuraCV Canary — GPS/GNSS Management Implementation
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#include "securacv_gps.h"
#include "../../../../common/gnss/gnss_time.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>

// ════════════════════════════════════════════════════════════════════════════
// UTILITY FUNCTIONS
// ════════════════════════════════════════════════════════════════════════════

const char* fix_mode_name(GpsFixMode m) {
  switch (m) {
    case FIX_MODE_NONE: return "None";
    case FIX_MODE_2D:   return "2D";
    case FIX_MODE_3D:   return "3D";
    default:            return "?";
  }
}

const char* quality_name(int q) {
  switch (q) {
    case 0: return "Inv";
    case 1: return "GPS";
    case 2: return "DGPS";
    case 4: return "RTK";
    case 5: return "FRTK";
    default: return "?";
  }
}

float knots_to_mps(float knots) {
  return knots * 0.514444f;
}

float knots_to_kmh(float knots) {
  return knots * 1.852f;
}

// ════════════════════════════════════════════════════════════════════════════
// GPS MANAGER IMPLEMENTATION
// ════════════════════════════════════════════════════════════════════════════

GpsManager::GpsManager()
  : m_serial(nullptr), m_rb_head(0), m_rb_tail(0), m_rb_count(0),
    m_line_len(0), m_sentence_count(0), m_checksum_errors(0), m_first_fix_ms(0),
    m_gga_count(0), m_rmc_count(0), m_gsa_count(0), m_gsv_count(0), m_vtg_count(0),
    m_speed_ema_mps(0.0f), m_motion_release_since_ms(0) {
  memset(&m_fix, 0, sizeof(m_fix));
  m_fix.hdop = 99.9;
  m_fix.pdop = 99.9;
  m_fix.vdop = 99.9;
  m_fix.fix_mode = FIX_MODE_NONE;
  memset(&m_utc, 0, sizeof(m_utc));
  memset(&m_motion, 0, sizeof(m_motion));
  memset(m_rb, 0, sizeof(m_rb));
  memset(m_line_buf, 0, sizeof(m_line_buf));
}

void GpsManager::begin(HardwareSerial& serial, uint32_t baud, int rx_pin, int tx_pin) {
  m_serial = &serial;
  m_serial->begin(baud, SERIAL_8N1, rx_pin, tx_pin);
  Serial.printf("[GPS] UART: %u baud, RX=GPIO%d, TX=GPIO%d\n", baud, rx_pin, tx_pin);
}

void GpsManager::update() {
  if (!m_serial) return;

  // Read data into ring buffer
  while (m_serial->available()) {
    if (m_rb_count < RB_SIZE) {
      m_rb[m_rb_head] = m_serial->read();
      m_rb_head = (m_rb_head + 1) % RB_SIZE;
      m_rb_count++;
    } else {
      break;
    }
  }

  // Parse NMEA lines
  char line[256];
  size_t len;
  while (readNmeaLine(line, sizeof(line), &len)) {
    parseNmea(line);
  }
}

float GpsManager::getSpeedMps() const {
  return knots_to_mps(m_fix.speed_knots);
}

bool GpsManager::readNmeaLine(char* out, size_t cap, size_t* len) {
  while (m_rb_count > 0) {
    uint8_t b = m_rb[m_rb_tail];
    m_rb_tail = (m_rb_tail + 1) % RB_SIZE;
    m_rb_count--;

    if (b == '\n' || b == '\r') {
      if (m_line_len > 0) {
        size_t copy_len = (m_line_len < cap - 1) ? m_line_len : cap - 1;
        memcpy(out, m_line_buf, copy_len);
        out[copy_len] = '\0';
        *len = copy_len;
        m_line_len = 0;
        return true;
      }
    } else if (m_line_len < sizeof(m_line_buf) - 1) {
      m_line_buf[m_line_len++] = b;
    }
  }
  return false;
}

static int parse_int(const char* s, int def) {
  if (!s || !*s) return def;
  return atoi(s);
}

static double parse_double(const char* s, double def) {
  if (!s || !*s) return def;
  return atof(s);
}

static char* get_field(char* s, int field) {
  int f = 0;
  char* p = s;
  while (*p && f < field) {
    if (*p == ',') f++;
    p++;
  }
  if (f != field) return nullptr;
  return p;
}

// Validate NMEA checksum (XOR of all chars between $ and *)
static bool validate_nmea_checksum(const char* line) {
  if (line[0] != '$') return false;

  const char* star = strchr(line, '*');
  if (!star || star - line < 2) return false;

  // Calculate XOR checksum of characters between $ and *
  uint8_t calc = 0;
  for (const char* p = line + 1; p < star; p++) {
    calc ^= (uint8_t)*p;
  }

  // Parse expected checksum (2 hex digits after *)
  if (strlen(star) < 3) return false;
  char hex[3] = { star[1], star[2], '\0' };
  uint8_t expected = (uint8_t)strtoul(hex, nullptr, 16);

  return calc == expected;
}

void GpsManager::parseNmea(char* line) {
  m_sentence_count++;

  #if DEBUG_NMEA
  Serial.println(line);
  #endif

  // Verify sentence format
  if (line[0] != '$') return;
  char* star = strchr(line, '*');
  if (!star) return;

  // Validate checksum for data integrity
  if (!validate_nmea_checksum(line)) {
    m_checksum_errors++;
    return;
  }

  // Parse sentence type
  char* type = line + 3;  // Skip $XX

  if (strncmp(type, "GGA", 3) == 0) {
    m_gga_count++;
    char* lat_str = get_field(line, 2);
    char* lat_dir = get_field(line, 3);
    char* lon_str = get_field(line, 4);
    char* lon_dir = get_field(line, 5);
    char* quality = get_field(line, 6);
    char* sats = get_field(line, 7);
    char* hdop_str = get_field(line, 8);
    char* alt_str = get_field(line, 9);
    char* geoid_str = get_field(line, 11);

    m_fix.quality = parse_int(quality, 0);
    m_fix.satellites = parse_int(sats, 0);
    m_fix.hdop = parse_double(hdop_str, 99.9);
    m_fix.altitude_m = parse_double(alt_str, 0);
    m_fix.geoid_sep_m = parse_double(geoid_str, 0);

    if (lat_str && *lat_str) {
      double lat_raw = parse_double(lat_str, 0);
      int lat_deg = (int)(lat_raw / 100);
      double lat_min = lat_raw - lat_deg * 100;
      m_fix.lat = lat_deg + lat_min / 60.0;
      if (lat_dir && *lat_dir == 'S') m_fix.lat = -m_fix.lat;
    }

    if (lon_str && *lon_str) {
      double lon_raw = parse_double(lon_str, 0);
      int lon_deg = (int)(lon_raw / 100);
      double lon_min = lon_raw - lon_deg * 100;
      m_fix.lon = lon_deg + lon_min / 60.0;
      if (lon_dir && *lon_dir == 'W') m_fix.lon = -m_fix.lon;
    }

    m_fix.valid = (m_fix.quality > 0);
    m_fix.last_gga_ms = millis();
    m_fix.last_update_ms = millis();

    if (m_fix.valid && m_first_fix_ms == 0) {
      m_first_fix_ms = millis();
    }
  }
  else if (strncmp(type, "RMC", 3) == 0) {
    m_rmc_count++;
    char* time_str = get_field(line, 1);
    char* status = get_field(line, 2);
    char* speed = get_field(line, 7);
    char* course = get_field(line, 8);
    char* date_str = get_field(line, 9);

    // Status 'A' = active/valid fix, 'V' = void (no fix, or a warm-up
    // sentence the receiver emits before it has one). A void RMC can still
    // carry a stale or all-zero time/date/speed/course — those fields must
    // not be trusted, or presented as a fix, when status isn't 'A'.
    bool rmc_active = status && *status == 'A';

    if (rmc_active && speed && *speed) {
      m_fix.speed_knots = parse_double(speed, 0);
      m_fix.speed_kmh = knots_to_kmh(m_fix.speed_knots);
      // Smooth raw speed for the motion filter so a single noisy RMC doesn't
      // flap the displayed speed. Alpha mirrors the WAP project (0.15).
      const float alpha = 0.15f;
      float mps = knots_to_mps(m_fix.speed_knots);
      m_speed_ema_mps = m_speed_ema_mps * (1.0f - alpha) + mps * alpha;
    }

    if (rmc_active && course && *course) {
      m_fix.course_deg = parse_double(course, 0);
    }

    if (rmc_active && time_str && strlen(time_str) >= 6 &&
        date_str && strlen(date_str) >= 6) {
      int hour = (time_str[0] - '0') * 10 + (time_str[1] - '0');
      int minute = (time_str[2] - '0') * 10 + (time_str[3] - '0');
      int second = (time_str[4] - '0') * 10 + (time_str[5] - '0');
      int centisecond = (strlen(time_str) > 7) ? parse_int(time_str + 7, 0) : 0;
      int day = (date_str[0] - '0') * 10 + (date_str[1] - '0');
      int month = (date_str[2] - '0') * 10 + (date_str[3] - '0');
      int year = 2000 + (date_str[4] - '0') * 10 + (date_str[5] - '0');

      // Guard against a corrupt-but-checksum-valid sentence, or a receiver
      // that hasn't loaded ephemeris yet and emits all-zero fields, before
      // this ever reaches getUtcTime() / a system-clock sync.
      if (securacv::gnss::gnss_calendar_valid(year, month, day, hour, minute, second)) {
        m_utc.hour = hour;
        m_utc.minute = minute;
        m_utc.second = second;
        m_utc.centisecond = centisecond;
        m_utc.day = day;
        m_utc.month = month;
        m_utc.year = year;
        m_utc.valid = true;
        m_utc.last_seen_ms = millis();
      }
    }

    m_fix.last_rmc_ms = millis();
  }
  else if (strncmp(type, "GSA", 3) == 0) {
    m_gsa_count++;
    char* mode = get_field(line, 2);
    char* pdop = get_field(line, 15);
    char* hdop = get_field(line, 16);
    char* vdop = get_field(line, 17);

    if (mode && *mode) {
      m_fix.fix_mode = (GpsFixMode)parse_int(mode, 1);
    }
    m_fix.pdop = parse_double(pdop, 99.9);
    if (hdop && *hdop) m_fix.hdop = parse_double(hdop, m_fix.hdop);
    m_fix.vdop = parse_double(vdop, 99.9);
    m_fix.last_gsa_ms = millis();
  }
  else if (strncmp(type, "GSV", 3) == 0) {
    m_gsv_count++;
    char* siv = get_field(line, 3);
    if (siv && *siv) {
      m_fix.sats_in_view = parse_int(siv, 0);
    }
  }
  else if (strncmp(type, "VTG", 3) == 0) {
    m_vtg_count++;
    char* course = get_field(line, 1);
    char* speed_kmh = get_field(line, 7);
    if (course && *course) {
      m_fix.course_deg = parse_double(course, m_fix.course_deg);
    }
    if (speed_kmh && *speed_kmh) {
      m_fix.speed_kmh = parse_double(speed_kmh, m_fix.speed_kmh);
    }
  }
}

// ════════════════════════════════════════════════════════════════════════════
// MOTION FILTER (anchor lock for stationary deployments)
// ════════════════════════════════════════════════════════════════════════════
//
// Mounted cameras sit still in a window. The L76K still emits ~0.3-0.7 m/s
// of "speed" and a few meters of position scatter from multipath. Surfacing
// that raw makes a stationary deployment look like it's drifting — kills
// credibility for a witness device. The motion filter holds the published
// position to a smoothed centroid while we're at rest, and only releases it
// once we see sustained motion (matching the witness state machine).

static const double STATIONARY_RADIUS_M = 8.0;
static const double HDOP_LOCK_MAX       = 5.0;
static const float  STATIC_THRESHOLD    = 0.4f;
static const float  MOVING_THRESHOLD    = 0.8f;
static const float  ANCHOR_EMA_ALPHA    = 0.10f;
static const uint32_t MOTION_RELEASE_MS = 3000;

static double motion_haversine_m(double lat1, double lon1, double lat2, double lon2) {
  // Spherical Earth approximation; accurate to ~0.5% over short distances,
  // which is well inside GNSS noise. Only used for "is this fix near the
  // anchor?" decisions, never for any logged position.
  const double R = 6371000.0;
  const double d2r = 0.017453292519943295;
  double phi1 = lat1 * d2r;
  double phi2 = lat2 * d2r;
  double dphi = (lat2 - lat1) * d2r;
  double dlam = (lon2 - lon1) * d2r;
  double s1 = sin(dphi * 0.5);
  double s2 = sin(dlam * 0.5);
  double a = s1 * s1 + cos(phi1) * cos(phi2) * s2 * s2;
  if (a < 0) a = 0; if (a > 1) a = 1;
  return 2.0 * R * asin(sqrt(a));
}

void GpsManager::updateMotion(bool stationary_hint) {
  uint32_t now = millis();
  m_motion.raw_speed_mps = m_speed_ema_mps;

  if (!m_fix.valid) {
    m_motion.is_stationary = false;
    m_motion.has_anchor = false;
    m_motion.display_lat = 0.0;
    m_motion.display_lon = 0.0;
    m_motion.display_alt_m = 0.0;
    m_motion.display_speed_mps = 0.0f;
    m_motion.anchor_samples = 0;
    m_motion_release_since_ms = 0;
    return;
  }

  bool fix_trustworthy = (m_fix.hdop > 0 && m_fix.hdop <= HDOP_LOCK_MAX);
  // Caller may already track a state machine (STATE_STATIONARY etc.). When
  // they do, we trust their hint; otherwise fall back to the raw speed EMA.
  bool want_locked = stationary_hint || (m_speed_ema_mps <= STATIC_THRESHOLD);
  if (m_speed_ema_mps >= MOVING_THRESHOLD) want_locked = false;

  if (want_locked) {
    if (!m_motion.has_anchor) {
      m_motion.has_anchor = true;
      m_motion.is_stationary = true;
      m_motion.anchor_samples = 1;
      m_motion.display_lat = m_fix.lat;
      m_motion.display_lon = m_fix.lon;
      m_motion.display_alt_m = m_fix.altitude_m;
      m_motion_release_since_ms = 0;
    } else {
      double drift = motion_haversine_m(m_motion.display_lat, m_motion.display_lon,
                                        m_fix.lat, m_fix.lon);
      if (drift <= STATIONARY_RADIUS_M && fix_trustworthy) {
        // Slow EMA so a single bad fix can't yank the displayed position.
        const double a = (double)ANCHOR_EMA_ALPHA;
        m_motion.display_lat   = m_motion.display_lat   * (1.0 - a) + m_fix.lat        * a;
        m_motion.display_lon   = m_motion.display_lon   * (1.0 - a) + m_fix.lon        * a;
        m_motion.display_alt_m = m_motion.display_alt_m * (1.0 - a) + m_fix.altitude_m * a;
        if (m_motion.anchor_samples < UINT32_MAX) m_motion.anchor_samples++;
        m_motion_release_since_ms = 0;
      } else if (drift > STATIONARY_RADIUS_M) {
        // Out-of-radius fix: could be real motion or a multipath outlier.
        // Don't release immediately — wait for sustained drift.
        if (m_motion_release_since_ms == 0) m_motion_release_since_ms = now;
        if ((now - m_motion_release_since_ms) >= MOTION_RELEASE_MS) {
          m_motion.is_stationary = false;
          m_motion.has_anchor = false;
        }
      }
    }
  } else {
    m_motion.is_stationary = false;
    m_motion.has_anchor = false;
    m_motion_release_since_ms = 0;
  }

  if (m_motion.is_stationary && m_motion.has_anchor) {
    m_motion.display_speed_mps = 0.0f;
  } else {
    m_motion.display_lat = m_fix.lat;
    m_motion.display_lon = m_fix.lon;
    m_motion.display_alt_m = m_fix.altitude_m;
    m_motion.display_speed_mps = (m_speed_ema_mps < STATIC_THRESHOLD) ? 0.0f : m_speed_ema_mps;
  }
}
