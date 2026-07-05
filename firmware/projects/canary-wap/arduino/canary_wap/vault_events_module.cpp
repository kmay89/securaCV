/**
 * @file vault_events_module.cpp
 * @brief Implementation of media.vault module + emit helper. See header.
 */

#include "build_config.h"

#if FEATURE_VAULT_SNAPSHOT

#include "vault_events_module.h"

#include <stdio.h>
#include <string.h>

namespace {

/* One event type. The allow-list is the privacy contract: a sealed-frame
 * event may carry the note ("<trigger> <ct-hash-16hex>" — the tag plus pure
 * integrity data, revealing nothing about image content) and the coarse
 * time bucket. No confidence, no signals, and structurally no way to
 * attach image bytes.
 *
 * Deliberately NO state_name: the bundler folds same-state events into one
 * open bundle without updating `note`, which would silently drop the
 * ciphertext hash of a second seal landing inside the bundle window. A
 * stateless event is a bundler pass-through — every sealed file commits
 * its own hash row.
 *
 * Ceiling 30/h: the capture side allows at most one seal per trigger per
 * cooldown (default 60 s) across three triggers plus manual tests; the
 * acoustic module grants its three life-safety types 12/h EACH, so 30/h
 * for this single combined type keeps every legitimate seal's hash on the
 * chain instead of orphaning the file. */
const csi_event_decl_t EVENTS[] = {
  {
    /* type_name */                "frame_sealed",
    /* allowed_fields */            CSI_FIELD_NOTE
                                  | CSI_FIELD_TIME_BUCKET,
    /* privacy */                   CSI_PRIVACY_P0,
    /* default_ceiling_per_hour */  30,
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
  /* ANOMALY, matching the acoustic module's classification of the very
   * triggers that cause a seal (T3/T4/glass are anomalies there): a
   * life-safety frame seal must not be held by the quiet-hours gate —
   * a night-time alarm is precisely when the evidence hash matters. */
  v.category       = CSI_CATEGORY_ANOMALY;
  v.present_fields = CSI_FIELD_TIME_BUCKET | CSI_FIELD_NOTE;
  /* "<tag> <hash16>" — longest is "smoke <16 hex>" = 22 chars + NUL,
   * inside note[24]. */
  snprintf(v.note, sizeof(v.note), "%s %s", trigger_tag, ct_hash_hex16);
  (void)csi_event_emit("media.vault", "frame_sealed", &v);
}

}  /* extern "C" */

#endif /* FEATURE_VAULT_SNAPSHOT */
