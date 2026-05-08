/**
 * @file ble_events_module.cpp
 * @brief Implementation of ble.events module + emit helpers. See header.
 */

#include "ble_events_module.h"
#include "csi_event.h"

#include <string.h>

namespace {

/* The allow-list per event type is what makes the spec §10 privacy
 * constraints enforceable. If a caller (or a future contributor) tries
 * to populate a field outside this list — say, breathing_rate_bpm or
 * motion_score — the chokepoint zeroes it before persistence. NO event
 * here permits CSI_FIELD_BREATHING_RATE / MOTION_SCORE / BREATHING_SCORE
 * because BLE events have nothing to say about CSI features. */
const csi_event_decl_t EVENTS[] = {
  {
    /* type_name */                "ble_initialized",
    /* allowed_fields */            CSI_FIELD_STATE_NAME
                                  | CSI_FIELD_TIME_BUCKET,
    /* privacy */                   CSI_PRIVACY_P0,
    /* default_ceiling_per_hour */  6,
  },
  {
    /* type_name */                "ble_init_failed",
    /* allowed_fields */            CSI_FIELD_STATE_NAME
                                  | CSI_FIELD_NOTE
                                  | CSI_FIELD_TIME_BUCKET,
    /* privacy */                   CSI_PRIVACY_P0,
    /* default_ceiling_per_hour */  6,
  },
  {
    /* type_name */                "ble_client_connected",
    /* allowed_fields */            CSI_FIELD_NOTE
                                  | CSI_FIELD_TIME_BUCKET,
    /* privacy */                   CSI_PRIVACY_P0,
    /* default_ceiling_per_hour */  12,
  },
  {
    /* type_name */                "ble_client_disconnected",
    /* allowed_fields */            CSI_FIELD_NOTE
                                  | CSI_FIELD_TIME_BUCKET,
    /* privacy */                   CSI_PRIVACY_P0,
    /* default_ceiling_per_hour */  12,
  },
  {
    /* type_name */                "chirp_sent",
    /* allowed_fields */            CSI_FIELD_STATE_NAME
                                  | CSI_FIELD_TIME_BUCKET,
    /* privacy */                   CSI_PRIVACY_P0,
    /* default_ceiling_per_hour */  20,
  },
  {
    /* type_name */                "chirp_received",
    /* allowed_fields */            CSI_FIELD_STATE_NAME
                                  | CSI_FIELD_NOTE
                                  | CSI_FIELD_TIME_BUCKET,
    /* privacy */                   CSI_PRIVACY_P0,
    /* default_ceiling_per_hour */  20,
  },
  {
    /* type_name */                "canary_discovered",
    /* allowed_fields */            CSI_FIELD_NOTE
                                  | CSI_FIELD_TIME_BUCKET,
    /* privacy */                   CSI_PRIVACY_P0,
    /* default_ceiling_per_hour */  10,
  },
  {
    /* type_name */                "canary_lost",
    /* allowed_fields */            CSI_FIELD_NOTE
                                  | CSI_FIELD_TIME_BUCKET,
    /* privacy */                   CSI_PRIVACY_P0,
    /* default_ceiling_per_hour */  10,
  },
};

void on_init(const csi_module_settings_t* /*s*/) {}
void on_tick(const csi_features_t*        /*f*/) {}

const csi_module_t MODULE = {
  /* id */                 "ble.events",
  /* default_privacy */    CSI_PRIVACY_P0,
  /* events */             EVENTS,
  /* event_count */        sizeof(EVENTS) / sizeof(EVENTS[0]),
  /* init */               on_init,
  /* tick */               on_tick,
  /* on_event_dismissed */ nullptr,
  /* deinit */             nullptr,
};

/* Helper: copy `peer_hash_hex` into the values' note field, truncating
 * at 16 ASCII chars (8 bytes binary == an Ed25519 pubkey-hash prefix
 * per spec §10). The chokepoint's sanitize_strings will reject any
 * non-printable bytes so a buggy caller can't smuggle binary through. */
void put_peer_hash_(csi_event_values_t* v, const char* peer_hash_hex) {
  if (!peer_hash_hex || !peer_hash_hex[0]) return;
  v->present_fields |= CSI_FIELD_NOTE;
  size_t n = strnlen(peer_hash_hex, 16);
  for (size_t i = 0; i < n && i + 1 < sizeof(v->note); ++i) {
    v->note[i] = peer_hash_hex[i];
  }
  size_t cap = (n < sizeof(v->note) - 1) ? n : sizeof(v->note) - 1;
  v->note[cap] = '\0';
}

void put_state_(csi_event_values_t* v, const char* tag) {
  if (!tag || !tag[0]) return;
  v->present_fields |= CSI_FIELD_STATE_NAME;
  strncpy(v->state_name, tag, sizeof(v->state_name) - 1);
  v->state_name[sizeof(v->state_name) - 1] = '\0';
}

}  /* namespace */

extern "C" {

const csi_module_t* ble_events_module(void) { return &MODULE; }

void ble_events_emit_initialized(void) {
  csi_event_values_t v;
  csi_event_values_init(&v);
  v.category       = CSI_CATEGORY_EVENT;
  v.present_fields = CSI_FIELD_TIME_BUCKET;
  put_state_(&v, "ok");
  (void)csi_event_emit("ble.events", "ble_initialized", &v);
}

void ble_events_emit_init_failed(const char* reason) {
  csi_event_values_t v;
  csi_event_values_init(&v);
  v.category       = CSI_CATEGORY_EVENT;
  v.present_fields = CSI_FIELD_TIME_BUCKET;
  put_state_(&v, "failed");
  if (reason && reason[0]) {
    v.present_fields |= CSI_FIELD_NOTE;
    strncpy(v.note, reason, sizeof(v.note) - 1);
    v.note[sizeof(v.note) - 1] = '\0';
  }
  (void)csi_event_emit("ble.events", "ble_init_failed", &v);
}

void ble_events_emit_client_connected(const char* peer_hash_hex) {
  csi_event_values_t v;
  csi_event_values_init(&v);
  v.category       = CSI_CATEGORY_EVENT;
  v.present_fields = CSI_FIELD_TIME_BUCKET;
  put_peer_hash_(&v, peer_hash_hex);
  (void)csi_event_emit("ble.events", "ble_client_connected", &v);
}

void ble_events_emit_client_disconnected(const char* peer_hash_hex) {
  csi_event_values_t v;
  csi_event_values_init(&v);
  v.category       = CSI_CATEGORY_EVENT;
  v.present_fields = CSI_FIELD_TIME_BUCKET;
  put_peer_hash_(&v, peer_hash_hex);
  (void)csi_event_emit("ble.events", "ble_client_disconnected", &v);
}

void ble_events_emit_chirp_sent(const char* chirp_type) {
  csi_event_values_t v;
  csi_event_values_init(&v);
  v.category       = CSI_CATEGORY_EVENT;
  v.present_fields = CSI_FIELD_TIME_BUCKET;
  put_state_(&v, chirp_type ? chirp_type : "?");
  (void)csi_event_emit("ble.events", "chirp_sent", &v);
}

void ble_events_emit_chirp_received(const char* chirp_type,
                                    const char* peer_hash_hex) {
  csi_event_values_t v;
  csi_event_values_init(&v);
  v.category       = CSI_CATEGORY_EVENT;
  v.present_fields = CSI_FIELD_TIME_BUCKET;
  put_state_(&v, chirp_type ? chirp_type : "?");
  put_peer_hash_(&v, peer_hash_hex);
  (void)csi_event_emit("ble.events", "chirp_received", &v);
}

void ble_events_emit_canary_discovered(const char* peer_hash_hex) {
  csi_event_values_t v;
  csi_event_values_init(&v);
  v.category       = CSI_CATEGORY_EVENT;
  v.present_fields = CSI_FIELD_TIME_BUCKET;
  put_peer_hash_(&v, peer_hash_hex);
  (void)csi_event_emit("ble.events", "canary_discovered", &v);
}

void ble_events_emit_canary_lost(const char* peer_hash_hex) {
  csi_event_values_t v;
  csi_event_values_init(&v);
  v.category       = CSI_CATEGORY_EVENT;
  v.present_fields = CSI_FIELD_TIME_BUCKET;
  put_peer_hash_(&v, peer_hash_hex);
  (void)csi_event_emit("ble.events", "canary_lost", &v);
}

}  /* extern "C" */
