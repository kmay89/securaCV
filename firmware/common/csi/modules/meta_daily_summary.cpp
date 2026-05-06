/**
 * @file modules/meta_daily_summary.cpp
 * @brief Day-boundary summary emitter.
 */

#include "meta_daily_summary.h"
#include "../csi_event.h"

#include <string.h>
#include <stdio.h>

namespace {

uint16_t s_last_minutes = 0xffff;   /* sentinel: no clock yet */
bool     s_emitted_today = false;

const csi_event_decl_t EVENTS[] = {
  {
    /* type_name */               "daily_summary",
    /* allowed_fields */           CSI_FIELD_NOTE
                                 | CSI_FIELD_BUNDLED_COUNT
                                 | CSI_FIELD_TIME_BUCKET,
    /* privacy */                 CSI_PRIVACY_P0,
    /* default_ceiling_per_hour */ 0,    /* no per-hour cap; emits at most once per day */
  },
};

void emit_summary() {
  /* Walk the event ring; tally active periods and anomalies (P0 only,
   * to stay contract-clean). */
  csi_event_record_t buffer[64];
  size_t n = csi_event_recent(buffer, sizeof(buffer) / sizeof(buffer[0]));

  uint16_t active_periods = 0;
  uint16_t anomaly_count  = 0;
  uint16_t quiet_periods  = 0;
  for (size_t i = 0; i < n; ++i) {
    const csi_event_record_t* r = &buffer[i];
    if (r->category == CSI_CATEGORY_ANOMALY) anomaly_count++;
    if (strcmp(r->values.state_name, "active") == 0)   active_periods++;
    else if (strcmp(r->values.state_name, "empty") == 0) quiet_periods++;
  }

  csi_event_values_t v;
  csi_event_values_init(&v);
  v.category       = CSI_CATEGORY_EVENT;
  v.present_fields = CSI_FIELD_NOTE | CSI_FIELD_BUNDLED_COUNT | CSI_FIELD_TIME_BUCKET;
  v.bundled_count  = (uint16_t)n;

  /* The note carries a single short tag, ASCII only. We use a stable schema
   * the dashboard's daily-summary card knows how to render. */
  snprintf(v.note, sizeof(v.note), "a%u q%u x%u",
           (unsigned)active_periods, (unsigned)quiet_periods, (unsigned)anomaly_count);

  (void)csi_event_emit("meta.daily_summary", "daily_summary", &v);
}

void on_init(const csi_module_settings_t* /*s*/) {
  s_last_minutes = 0xffff;
  s_emitted_today = false;
}

void on_tick(const csi_features_t* /*f*/) {
  /* No clock supplied yet — nothing to do. */
  if (s_last_minutes == 0xffff) return;
  /* The last 5 minutes of the day is the emit window; we want one summary
   * per midnight rollover even if the clock jumps. */
  if (s_last_minutes >= 1435 /* 23:55 */ && !s_emitted_today) {
    emit_summary();
    s_emitted_today = true;
  }
  if (s_last_minutes < 30) {
    s_emitted_today = false;
  }
}

const csi_module_t MODULE = {
  /* id */                 "meta.daily_summary",
  /* default_privacy */    CSI_PRIVACY_P0,
  /* events */             EVENTS,
  /* event_count */        sizeof(EVENTS) / sizeof(EVENTS[0]),
  /* init */               on_init,
  /* tick */               on_tick,
  /* on_event_dismissed */ nullptr,
  /* deinit */             nullptr,
};

}  /* namespace */

extern "C" {
const csi_module_t* meta_daily_summary_module(void) { return &MODULE; }
void meta_daily_summary_set_clock(uint16_t minutes_of_day) {
  s_last_minutes = minutes_of_day;
}
}
