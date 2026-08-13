// test_calendar_math.cpp — the mini calendar's arithmetic
// (canary-display include/canary/ui/calendar_math.h), pinned.
//
// A bedside calendar that puts the 12th on a Tuesday when it is a Wednesday
// is a quiet lie in the most trusted spot in the house. Everything here is
// checked against independently-known dates (not against the same formula).

#include <cstdio>

#include "canary/ui/calendar_math.h"

static int g_fail = 0;
#define CHECK(cond, ...)                               \
  do {                                                 \
    if (!(cond)) {                                     \
      std::printf("FAIL %s:%d: ", __FILE__, __LINE__); \
      std::printf(__VA_ARGS__);                        \
      std::printf("\n");                               \
      g_fail++;                                        \
    }                                                  \
  } while (0)

using namespace canary::ui;

int main() {
  // Known weekdays (0 = Sunday), spanning centuries and both leap sides.
  CHECK(cal_weekday(2026, 8, 12) == 3, "2026-08-12 is a Wednesday");
  CHECK(cal_weekday(2000, 1, 1) == 6, "2000-01-01 was a Saturday");
  CHECK(cal_weekday(2024, 2, 29) == 4, "2024-02-29 was a Thursday");
  CHECK(cal_weekday(1970, 1, 1) == 4, "the epoch began on a Thursday");
  CHECK(cal_weekday(2100, 3, 1) == 1, "2100-03-01 is a Monday (2100 not leap)");

  // Leap rules: the century exception and its exception.
  CHECK(cal_is_leap(2024) && !cal_is_leap(2026), "simple leap years");
  CHECK(!cal_is_leap(1900) && cal_is_leap(2000), "century rule + 400 rule");
  CHECK(cal_days_in_month(2024, 2) == 29, "leap February");
  CHECK(cal_days_in_month(2026, 2) == 28, "plain February");
  CHECK(cal_days_in_month(2026, 8) == 31, "August");
  CHECK(cal_days_in_month(2026, 9) == 30, "September");

  // Grid geometry: August 2026 starts on a Saturday (col 6), so the 12th
  // sits in row 2, col 3 (Wednesday), and the month needs 6 rows.
  int row, col;
  cal_cell_of(2026, 8, 1, &row, &col);
  CHECK(row == 0 && col == 6, "Aug 1 2026: row 0, Saturday column");
  cal_cell_of(2026, 8, 12, &row, &col);
  CHECK(row == 2 && col == 3, "Aug 12 2026: row 2, Wednesday column");
  CHECK(cal_rows_in_month(2026, 8) == 6, "August 2026 spans 6 grid rows");
  // February 2026 starts on a Sunday and fits exactly 4 rows.
  CHECK(cal_rows_in_month(2026, 2) == 4, "February 2026 fits 4 rows");
  // No month ever needs more than 6 rows — the layout's fixed budget.
  for (int y = 2024; y <= 2060; y++)
    for (int m = 1; m <= 12; m++)
      CHECK(cal_rows_in_month(y, m) <= 6, "%d-%02d exceeds 6 rows", y, m);

  // Every day of a month lands in a unique cell, in reading order,
  // starting at the first-weekday offset.
  int last = cal_weekday(2026, 8, 1) - 1;
  for (int d = 1; d <= cal_days_in_month(2026, 8); d++) {
    cal_cell_of(2026, 8, d, &row, &col);
    const int idx = row * 7 + col;
    CHECK(idx == last + 1, "day %d is not in reading order", d);
    last = idx;
  }

  // Names and headers.
  CHECK(cal_month_name(8)[0] == 'A', "August names itself");
  CHECK(cal_col_letter(0)[0] == 'S' && cal_col_letter(1)[0] == 'M',
        "the grid reads Sunday-first");

  if (g_fail) {
    std::printf("test_calendar_math: %d FAILURE(S)\n", g_fail);
    return 1;
  }
  std::printf("test_calendar_math: all checks passed\n");
  return 0;
}
