// include/canary/ui/calendar_math.h — the month, as arithmetic.
//
// The Calendar clock face (clock_styles.h) draws a mini month grid beside
// the digits. Everything it needs to lay one out is pure integer math, so it
// lives here, LVGL-free and Arduino-free, host-tested
// (firmware/tests_host/test_calendar_math.cpp) — a wrong weekday on a
// bedside calendar is the kind of quiet lie this project exists to not tell.
//
// Conventions: months are 1..12, weekday 0 = Sunday (the grid's first
// column, matching how US wall calendars read — the same audience the
// 12-hour default serves).
#pragma once
#include <stdint.h>

namespace canary::ui {

inline bool cal_is_leap(int y) {
  return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

inline int cal_days_in_month(int y, int m) {
  static const uint8_t D[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (m < 1 || m > 12) return 30;
  if (m == 2 && cal_is_leap(y)) return 29;
  return D[m - 1];
}

// Sakamoto's method: weekday of y-m-d, 0 = Sunday. Valid for the Gregorian
// calendar — every date this product will ever draw.
inline int cal_weekday(int y, int m, int d) {
  static const int T[12] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  if (m < 3) y -= 1;
  return (y + y / 4 - y / 100 + y / 400 + T[m - 1] + d) % 7;
}

// Which grid cell (row, col) a day of this month lands in, with col 0 =
// Sunday. Row count for the month never exceeds 6.
inline void cal_cell_of(int y, int m, int d, int* row, int* col) {
  const int first = cal_weekday(y, m, 1);
  const int idx = first + (d - 1);
  *row = idx / 7;
  *col = idx % 7;
}

inline int cal_rows_in_month(int y, int m) {
  const int first = cal_weekday(y, m, 1);
  const int days = cal_days_in_month(y, m);
  return (first + days + 6) / 7;
}

inline const char* cal_month_name(int m) {
  static const char* N[12] = {"January", "February", "March",     "April",
                              "May",     "June",     "July",      "August",
                              "September", "October", "November", "December"};
  return (m >= 1 && m <= 12) ? N[m - 1] : "";
}

// The grid's column headers, Sunday-first, one letter each — a mini
// calendar has no room for more, and the single letters read fine because
// the layout carries the meaning.
inline const char* cal_col_letter(int col) {
  static const char* L[7] = {"S", "M", "T", "W", "T", "F", "S"};
  return (col >= 0 && col < 7) ? L[col] : "";
}

}  // namespace canary::ui
