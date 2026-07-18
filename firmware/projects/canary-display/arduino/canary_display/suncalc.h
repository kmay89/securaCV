// include/canary/care/suncalc.h — on-device sunrise/sunset (nightstand wave).
//
// The simplified NOAA sunrise equation: ±1–4 min at mid-latitudes, a few
// hundred double ops run once a day. Computed on-device on purpose — sun
// times keep working when the hub is down, and lat/lon never leaves the
// device (privacy posture: the glass asks nobody where it lives).
//
// Pure math, no Arduino types — host-tested against NOAA reference times.
#pragma once
#include <math.h>
#include <stdint.h>

namespace canary::care {

namespace suncalc_detail {
constexpr double DEG = 0.017453292519943295;  // pi/180
// Gregorian calendar date -> Julian day number (valid for 1901–2099).
inline double julian_day(int y, int m, int d) {
  if (m <= 2) { y -= 1; m += 12; }
  const int a = y / 100;
  const int b = 2 - a + a / 4;
  return floor(365.25 * (y + 4716)) + floor(30.6001 * (m + 1)) + d + b -
         1524.5;
}
}  // namespace suncalc_detail

// Sunrise/sunset as minutes past local midnight for the given civil date.
// lat north-positive, lon east-positive (degrees); tz_offset_min is the
// local UTC offset INCLUDING DST for that date. Returns false for polar
// day/night (no event).
inline bool sun_times(int year, int month, int day, double lat, double lon,
                      int tz_offset_min, int* rise_min, int* set_min) {
  using namespace suncalc_detail;
  const double jd = julian_day(year, month, day);
  const double n = ceil(jd - 2451545.0 + 0.0008);
  const double jstar = n - lon / 360.0;
  const double M = fmod(357.5291 + 0.98560028 * jstar, 360.0);
  const double C = 1.9148 * sin(M * DEG) + 0.0200 * sin(2 * M * DEG) +
                   0.0003 * sin(3 * M * DEG);
  const double lambda = fmod(M + C + 180.0 + 102.9372, 360.0);
  const double jtransit = 2451545.0 + jstar + 0.0053 * sin(M * DEG) -
                          0.0069 * sin(2 * lambda * DEG);
  const double sindecl = sin(lambda * DEG) * sin(23.4397 * DEG);
  const double cosdecl = cos(asin(sindecl));
  // -0.833°: refraction (34') + solar semidiameter (16').
  const double cosw = (sin(-0.833 * DEG) - sin(lat * DEG) * sindecl) /
                      (cos(lat * DEG) * cosdecl);
  if (cosw > 1.0 || cosw < -1.0) return false;
  const double w = acos(cosw) / DEG;  // hour angle, degrees

  // Julian date -> minutes past local midnight of the requested civil day.
  // julian_day() already lands on 0h UT (a .5 value), so local midnight is
  // just the timezone shift away — no extra half-day.
  const double day_start_jd = jd - (double)tz_offset_min / 1440.0;
  const double rise = (jtransit - w / 360.0 - day_start_jd) * 1440.0;
  const double set = (jtransit + w / 360.0 - day_start_jd) * 1440.0;
  *rise_min = (int)(rise + 0.5);
  *set_min = (int)(set + 0.5);
  // Normalize into the local day (the transit anchor can land the event
  // just outside 0..1440 near timezone edges).
  while (*rise_min < 0) *rise_min += 1440;
  while (*rise_min >= 1440) *rise_min -= 1440;
  while (*set_min < 0) *set_min += 1440;
  while (*set_min >= 1440) *set_min -= 1440;
  return true;
}

}  // namespace canary::care
