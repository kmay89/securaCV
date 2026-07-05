/**
 * @file vault_events_module.cpp
 * @brief Implementation of media.vault module + emit helper. See header.
 */

#include "build_config.h"

#if FEATURE_VAULT_SNAPSHOT

#include "vault_events_module.h"

#include <string.h>

namespace {

/* One event type. The allow-list is the privacy contract: a sealed-frame
 * event may carry the trigger tag (state), the ciphertext hash prefix
 * (note — pure integrity data, reveals nothing about image content), and
 * the coarse time bucket. No confidence, no signals, and structurally no
 * way to attach image bytes. */
const csi_event_decl_t EVENTS[] = {
  {
    /* type_name */                "frame_sealed",
    /* allowed_fields */            CSI_FIELD_STATE_NAME
                                  | CSI_FIELD_NOTE
                                  | CSI_FIELD_TIME_BUCKET,
    /* privacy */                   CSI_PRIVACY_P0,
    /* default_ceiling_per_hour */  12,
  },
};

void on_init(const csi_module_settings_t* /*s*/) {}
void on_tick(const csi_features_t*        /*f*/) {}

const csi_module_t MODULE = {
  /* id */                 "media.vault",
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

const csi_module_t* vault_events_module(void) { return &MODULE; }

void vault_events_emit_frame_sealed(const char* trigger_tag,
                                    const char* ct_hash_hex16) {
  if (!trigger_tag || !ct_hash_hex16) return;

  csi_event_values_t v;
  csi_event_values_init(&v);
  v.category       = CSI_CATEGORY_EVENT;
  v.present_fields = CSI_FIELD_TIME_BUCKET | CSI_FIELD_STATE_NAME
                   | CSI_FIELD_NOTE;
  strncpy(v.state_name, trigger_tag, sizeof(v.state_name) - 1);
  v.state_name[sizeof(v.state_name) - 1] = '\0';
  strncpy(v.note, ct_hash_hex16, sizeof(v.note) - 1);
  v.note[sizeof(v.note) - 1] = '\0';
  (void)csi_event_emit("media.vault", "frame_sealed", &v);
}

}  /* extern "C" */

#endif /* FEATURE_VAULT_SNAPSHOT */
