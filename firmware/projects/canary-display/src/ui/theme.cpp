#include "canary/ui/theme.h"
#include <stdio.h>

namespace canary::ui {

void format_age(uint32_t now_ms, uint32_t then_ms, char* out, int cap) {
  if (!out || cap <= 0) return;
  const int32_t d = (int32_t)(now_ms - then_ms);
  const uint32_t s = d > 0 ? (uint32_t)d / 1000UL : 0;
  if (s < 60)            snprintf(out, cap, "%lus", (unsigned long)s);
  else if (s < 3600)     snprintf(out, cap, "%lum", (unsigned long)(s / 60));
  else if (s < 86400)    snprintf(out, cap, "%luh", (unsigned long)(s / 3600));
  else                   snprintf(out, cap, "%lud", (unsigned long)(s / 86400));
}

}  // namespace canary::ui
