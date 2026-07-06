// src/fleet/fleet_model.cpp — the non-template pieces of the fleet model.
#include "canary/fleet/fleet_model.h"

namespace canary::fleet {

namespace {

bool contains(const char* hay, const char* needle) {
  if (!hay || !needle || !needle[0]) return false;
  for (const char* h = hay; *h; h++) {
    const char* a = h;
    const char* b = needle;
    while (*a && *b && *a == *b) { a++; b++; }
    if (!*b) return true;
  }
  return false;
}

}  // namespace

// Substring heuristics on purpose (see header). Order matters: the ladder is
// checked worst-first so "enclosure_tamper" never downgrades to Notice via
// a weaker match.
Sev classify_event(const char* e) {
  if (!e || !e[0]) return Sev::Notice;
  if (contains(e, "tamper") || contains(e, "panic")) return Sev::Tamper;
  if (contains(e, "smoke") || contains(e, "co_alarm") ||
      contains(e, "glass") || contains(e, "fire") ||
      contains(e, "chain_verify_failed"))
    return Sev::Alert;
  if (contains(e, "restricted") || contains(e, "after_hours") ||
      contains(e, "boundary") || contains(e, "removed") ||
      contains(e, "knock") || contains(e, "doorbell"))
    return Sev::Warn;
  if (contains(e, "cleared") || contains(e, "boot")) return Sev::Ok;
  // presence_detected, occupancy_changed, motion, contact_state_change,
  // and anything this vocabulary hasn't met yet.
  return Sev::Notice;
}

const char* sev_name(Sev s) {
  switch (s) {
    case Sev::Ok:     return "ok";
    case Sev::Notice: return "notice";
    case Sev::Warn:   return "warn";
    case Sev::Alert:  return "alert";
    case Sev::Tamper: return "tamper";
  }
  return "ok";
}

}  // namespace canary::fleet
