/*
 * SecuraCV Canary — GPS/GNSS Management Implementation
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#include "securacv_gps.h"
#include "securacv_witness.h"
#include <string.h>
#include <stdlib.h>

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
    m_line_len(0), m_sentence_count(0) {
  memset(&m_fix, 0, sizeof(m_fix));
  m_fix.hdop = 99.9;
  m_fix.pdop = 99.9;
  m_fix.vdop = 99.9;
  m_fix.fix_mode = FIX_MODE_NONE;
  memset(&m_utc, 0, sizeof(m_utc));
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

void GpsManager::parseNmea(char* line) {
  m_sentence_count++;
  SystemHealth& health = witness_get_health();
  health.gps_sentences++;

  #if DEBUG_NMEA
  Serial.println(line);
  #endif

  // Verify checksum
  if (line[0] != '$') return;
  char* star = strchr(line, '*');
  if (!star) return;

  // Parse sentence type
  char* type = line + 3;  // Skip $XX

  if (strncmp(type, "GGA", 3) == 0) {
    health.gga_count++;
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

    if (m_fix.valid && health.gps_lock_ms == 0) {
      health.gps_lock_ms = millis();
    }
  }
  else if (strncmp(type, "RMC", 3) == 0) {
    health.rmc_count++;
    char* time_str = get_field(line, 1);
    char* speed = get_field(line, 7);
    char* course = get_field(line, 8);
    char* date_str = get_field(line, 9);

    if (speed && *speed) {
      m_fix.speed_knots = parse_double(speed, 0);
      m_fix.speed_kmh = knots_to_kmh(m_fix.speed_knots);
    }

    if (course && *course) {
      m_fix.course_deg = parse_double(course, 0);
    }

    // Parse UTC time
    if (time_str && strlen(time_str) >= 6) {
      m_utc.hour = (time_str[0] - '0') * 10 + (time_str[1] - '0');
      m_utc.minute = (time_str[2] - '0') * 10 + (time_str[3] - '0');
      m_utc.second = (time_str[4] - '0') * 10 + (time_str[5] - '0');
      if (strlen(time_str) > 7) {
        m_utc.centisecond = parse_int(time_str + 7, 0);
      }
    }

    // Parse date
    if (date_str && strlen(date_str) >= 6) {
      m_utc.day = (date_str[0] - '0') * 10 + (date_str[1] - '0');
      m_utc.month = (date_str[2] - '0') * 10 + (date_str[3] - '0');
      int yy = (date_str[4] - '0') * 10 + (date_str[5] - '0');
      m_utc.year = 2000 + yy;
      m_utc.valid = true;
      m_utc.last_seen_ms = millis();
    }

    m_fix.last_rmc_ms = millis();
  }
  else if (strncmp(type, "GSA", 3) == 0) {
    health.gsa_count++;
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
    health.gsv_count++;
    char* siv = get_field(line, 3);
    if (siv && *siv) {
      m_fix.sats_in_view = parse_int(siv, 0);
    }
  }
  else if (strncmp(type, "VTG", 3) == 0) {
    health.vtg_count++;
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
