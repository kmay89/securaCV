/*
 * SecuraCV Canary — GPS/GNSS Management
 *
 * L76K GNSS NMEA parsing and fix management.
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#ifndef SECURACV_GPS_H
#define SECURACV_GPS_H

#include <Arduino.h>
#include <stdint.h>
#include "canary_config.h"

// ════════════════════════════════════════════════════════════════════════════
// TYPES
// ════════════════════════════════════════════════════════════════════════════

enum GpsFixMode : uint8_t {
  FIX_MODE_NONE = 1,
  FIX_MODE_2D   = 2,
  FIX_MODE_3D   = 3
};

struct GnssFix {
  bool     valid;
  double   lat;
  double   lon;
  int      quality;
  int      satellites;
  int      sats_in_view;
  double   hdop;
  double   pdop;
  double   vdop;
  double   altitude_m;
  double   geoid_sep_m;
  double   speed_knots;
  double   speed_kmh;
  double   course_deg;
  GpsFixMode fix_mode;
  uint32_t last_update_ms;
  uint32_t last_gga_ms;
  uint32_t last_rmc_ms;
  uint32_t last_gsa_ms;
};

struct GpsUtcTime {
  bool     valid;
  int      year;
  int      month;
  int      day;
  int      hour;
  int      minute;
  int      second;
  int      centisecond;
  uint32_t last_seen_ms;
};

// ════════════════════════════════════════════════════════════════════════════
// GPS MANAGER
// ════════════════════════════════════════════════════════════════════════════

class GpsManager {
public:
  GpsManager();

  // Initialize GPS UART
  void begin(HardwareSerial& serial, uint32_t baud = GPS_BAUD,
             int rx_pin = GPS_RX_PIN, int tx_pin = GPS_TX_PIN);

  // Process incoming data
  void update();

  // Get current fix data
  const GnssFix& getFix() const { return m_fix; }
  GnssFix& getFix() { return m_fix; }

  // Get UTC time
  const GpsUtcTime& getUtcTime() const { return m_utc; }
  GpsUtcTime& getUtcTime() { return m_utc; }

  // Speed in m/s
  float getSpeedMps() const;

  // Statistics
  uint32_t getSentenceCount() const { return m_sentence_count; }

private:
  void parseNmea(char* line);
  bool readNmeaLine(char* out, size_t cap, size_t* len);

  HardwareSerial* m_serial;
  GnssFix m_fix;
  GpsUtcTime m_utc;

  // Ring buffer for NMEA data
  static const size_t RB_SIZE = 2048;
  uint8_t m_rb[RB_SIZE];
  size_t m_rb_head;
  size_t m_rb_tail;
  size_t m_rb_count;

  // Line buffer
  char m_line_buf[256];
  size_t m_line_len;

  uint32_t m_sentence_count;
};

// ════════════════════════════════════════════════════════════════════════════
// UTILITY FUNCTIONS
// ════════════════════════════════════════════════════════════════════════════

// Fix mode name
const char* fix_mode_name(GpsFixMode m);

// Quality name
const char* quality_name(int q);

// Convert knots to m/s
float knots_to_mps(float knots);

// Convert knots to km/h
float knots_to_kmh(float knots);

#endif // SECURACV_GPS_H
