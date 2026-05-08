/**
 * @file meta_quiet_hours.cpp
 * @brief meta.quiet_hours module manifest. See header for design.
 */

#include "meta_quiet_hours.h"

namespace {

const csi_event_decl_t EVENTS[] = {
  {
    /* type_name */                "held_summary",
    /* allowed_fields */            CSI_FIELD_NOTE
                                  | CSI_FIELD_BUNDLED_COUNT
                                  | CSI_FIELD_TIME_BUCKET,
    /* privacy */                   CSI_PRIVACY_P0,
    /* default_ceiling_per_hour */  0,   /* at most one per quiet window close */
  },
};

void on_init(const csi_module_settings_t* /*s*/) {}
void on_tick(const csi_features_t*       /*f*/) {}

const csi_module_t MODULE = {
  /* id */                 "meta.quiet_hours",
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
const csi_module_t* meta_quiet_hours_module(void) { return &MODULE; }
}
