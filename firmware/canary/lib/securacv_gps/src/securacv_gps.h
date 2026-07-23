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
#include <ctime>
#include "canary_config.h"
#include "../../../../common/gnss/gnss_time.h"

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

// Motion filter output. The L76K reports ~0.3-0.7 m/s of phantom speed and a
// few metres of position scatter even when the device sits still in a window.
// The witness chain still records the raw GnssFix (so we never lie about what
// the receiver saw), but the API/UI consumes MotionView so a stationary
// mounted camera presents as truly stationary. Real motion still shows up:
// the lock releases once we've seen sustained drift > STATIONARY_RADIUS_M
// or speed >= MOVING_THRESHOLD_MPS for the release window.
struct MotionView {
  double  display_lat;
  double  display_lon;
  double  display_alt_m;
  float   display_speed_mps;   // 0 while locked
  float   raw_speed_mps;       // Smoothed RMC speed (for diagnostics)
  bool    is_stationary;       // true while position is anchor-locked
  bool    has_anchor;
  uint32_t anchor_samples;     // Fixes folded into the anchor centroid
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

  // Speed in m/s (raw — NMEA RMC after knots->m/s)
  float getSpeedMps() const;

  // Motion-filtered view for the API/UI. Locks to an anchor centroid while
  // stationary so the dashboard doesn't render multipath drift as movement.
  // Call updateMotion(state_is_stationary_hint) once per main-loop iteration
  // to advance the filter; getMotionView() is read-only.
  void updateMotion(bool stationary_hint);
  const MotionView& getMotionView() const { return m_motion; }

  // Statistics
  uint32_t getSentenceCount() const { return m_sentence_count; }
  uint32_t getChecksumErrors() const { return m_checksum_errors; }
  uint32_t getFirstFixMs() const { return m_first_fix_ms; }
  uint32_t getGgaCount() const { return m_gga_count; }
  uint32_t getRmcCount() const { return m_rmc_count; }
  uint32_t getGsaCount() const { return m_gsa_count; }
  uint32_t getGsvCount() const { return m_gsv_count; }
  uint32_t getVtgCount() const { return m_vtg_count; }

private:
  void parseNmea(char* line);
  bool readNmeaLine(char* out, size_t cap, size_t* len);

  HardwareSerial* m_serial;
  GnssFix m_fix;
  GpsUtcTime m_utc;
  MotionView m_motion;
  float    m_speed_ema_mps;
  uint32_t m_motion_release_since_ms;

  // Ring buffer for NMEA data
  static const size_t RB_SIZE = 2048;
  uint8_t m_rb[RB_SIZE];
  size_t m_rb_head;
  size_t m_rb_tail;
  size_t m_rb_count;

  // Line buffer
  char m_line_buf[256];
  size_t m_line_len;

  // Statistics (queryable by application for health reporting)
  uint32_t m_sentence_count;
  uint32_t m_checksum_errors;
  uint32_t m_first_fix_ms;
  uint32_t m_gga_count;
  uint32_t m_rmc_count;
  uint32_t m_gsa_count;
  uint32_t m_gsv_count;
  uint32_t m_vtg_count;
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

// Convert a fix's GpsUtcTime to a Unix epoch second, gated on RMC-derived
// calendar validity (see gnss_time.h). Returns false — and leaves *out
// untouched — if utc.valid is false or the calendar fields don't pass
// gnss_calendar_valid() (defense in depth: parseNmea() already screens this
// before setting valid=true, but callers about to trust this for a
// system-clock sync should not have to re-derive that guarantee).
inline bool gps_utc_to_epoch(const GpsUtcTime& utc, time_t* out) {
  if (!utc.valid) return false;
  return securacv::gnss::gnss_utc_to_unix(utc.year, utc.month, utc.day,
                                           utc.hour, utc.minute, utc.second, out);
}

#endif // SECURACV_GPS_H
