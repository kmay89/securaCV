#include "canary/ui/theme.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>

namespace canary::ui {

using canary::fleet::Badge;
using canary::fleet::Link;
using canary::fleet::Sev;

lv_color_t sev_color(Sev s, bool night) {
  if (night) return (s >= Sev::Alert) ? ncol_alert() : ncol_muted();
  switch (s) {
    case Sev::Ok:
    case Sev::Notice: return col_ok();
    case Sev::Warn:   return col_warn();
    case Sev::Alert:
    case Sev::Tamper: return col_alert();
  }
  return col_muted();
}

lv_color_t badge_color(Badge b, bool night) {
  if (night) return ncol_muted();
  switch (b) {
    case Badge::Verified: return col_ok();
    case Badge::Signed:   return col_signed();
    case Badge::Failed:   return col_alert();
    case Badge::Unsigned: return col_muted();
    case Badge::Unknown:  return col_faint();
  }
  return col_faint();
}

// The strong word is earned, never assumed (same rule as the HA card).
const char* badge_text(Badge b) {
  switch (b) {
    case Badge::Verified: return LV_SYMBOL_OK " verified";
    case Badge::Signed:   return "signed";
    case Badge::Failed:   return LV_SYMBOL_CLOSE " failed";
    case Badge::Unsigned: return "unsigned";
    case Badge::Unknown:  return "—";
  }
  return "—";
}

const char* link_label(Link l) {
  switch (l) {
    case Link::Online:  return "online";
    case Link::Stale:   return "stale";
    case Link::Lost:    return "lost";
    case Link::Offline: return "offline";
    default:            return "—";
  }
}

void format_age(uint32_t now_ms, uint32_t then_ms, char* out, int cap) {
  if (!out || cap <= 0) return;
  const int32_t d = (int32_t)(now_ms - then_ms);
  const uint32_t s = d > 0 ? (uint32_t)d / 1000UL : 0;
  if (s < 60)            snprintf(out, cap, "%lus", (unsigned long)s);
  else if (s < 3600)     snprintf(out, cap, "%lum", (unsigned long)(s / 60));
  else if (s < 86400)    snprintf(out, cap, "%luh", (unsigned long)(s / 3600));
  else                   snprintf(out, cap, "%lud", (unsigned long)(s / 86400));
}

namespace {

struct EventCopy {
  const char* wire;
  const char* human;
};

// Curated copy for the fleet's known vocabulary. Written from the user's
// side of the screen: what happened, not how the system classified it.
constexpr EventCopy EVENT_COPY[] = {
    {"presence_detected",              "Presence detected"},
    {"presence_cleared",               "All clear"},
    {"occupancy_changed",              "Occupancy changed"},
    {"presence_in_restricted_zone",    "Person in restricted zone"},
    {"vehicle_presence_after_hours",   "Vehicle after hours"},
    {"boundary_crossing_object_large", "Vehicle crossed boundary"},
    {"boundary_crossing_object_small", "Animal crossed boundary"},
    {"contact_state_change",           "Door or window changed"},
    {"object_removed_from_zone",       "Object removed"},
    {"acoustic_impulse_in_zone",       "Loud sound"},
    {"smoke_alarm_t3",                 "Smoke-alarm pattern heard"},
    {"co_alarm_t4",                    "CO-alarm pattern heard"},
    {"glass_break",                    "Glass break heard"},
    {"knock",                          "Knock heard"},
    {"doorbell",                       "Doorbell heard"},
    {"silent_panic",                   "Silent panic"},
    {"enclosure_tamper",               "Enclosure tampered"},
    {"camera_tamper",                  "Camera tampered"},
    {"temp_drift",                     "Temperature tamper"},
    {"chain_verify_failed",            "Chain verification FAILED"},
    {"boot",                           "Started up"},
};

}  // namespace

const char* humanize_event(const char* wire, char* buf, size_t cap) {
  if (!buf || cap == 0) return "";
  if (!wire || !wire[0]) { buf[0] = '\0'; return buf; }

  for (const auto& e : EVENT_COPY) {
    if (strcmp(e.wire, wire) == 0) {
      snprintf(buf, cap, "%s", e.human);
      return buf;
    }
  }

  // Unknown vocabulary: de-underscore + sentence case. The glass never
  // shows raw wire text, and never crashes on words it hasn't met.
  size_t o = 0;
  for (size_t i = 0; wire[i] && o + 1 < cap; i++) {
    char c = wire[i];
    if (c == '_' || c == '.') c = ' ';
    if (o == 0) c = (char)toupper((unsigned char)c);
    buf[o++] = c;
  }
  buf[o] = '\0';
  return buf;
}

lv_obj_t* mk_qrcode(lv_obj_t* parent, int32_t size_px) {
#if LVGL_VERSION_MAJOR >= 9
  // v9 split creation into setters (and renders on update).
  lv_obj_t* qr = lv_qrcode_create(parent);
  lv_qrcode_set_size(qr, size_px);
  lv_qrcode_set_dark_color(qr, lv_color_black());
  lv_qrcode_set_light_color(qr, lv_color_white());
  return qr;
#else
  return lv_qrcode_create(parent, size_px, lv_color_black(),
                          lv_color_white());
#endif
}

}  // namespace canary::ui
