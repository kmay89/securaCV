/**
 * @file acoustic_events_module.cpp
 * @brief Implementation of acoustic.events module + emit helpers. See header.
 */

#include "build_config.h"

#if FEATURE_ACOUSTIC_EVENTS

#include "acoustic_events_module.h"
#include "securacv_audio.h"

#include <string.h>

namespace {

/* Allow-lists keep these emits to exactly what the detector knows: a
 * state tag, a confidence word, and the time bucket. Nothing here may
 * carry CSI features (motion/breathing fields are absent on purpose) and
 * there is no audio payload in existence to attach. */
const csi_event_decl_t EVENTS[] = {
  {
    /* type_name */                "smoke_alarm_t3",
    /* allowed_fields */            CSI_FIELD_STATE_NAME
                                  | CSI_FIELD_CONFIDENCE
                                  | CSI_FIELD_TIME_BUCKET,
    /* privacy */                   CSI_PRIVACY_P0,
    /* default_ceiling_per_hour */  12,
  },
  {
    /* type_name */                "co_alarm_t4",
    /* allowed_fields */            CSI_FIELD_STATE_NAME
                                  | CSI_FIELD_CONFIDENCE
                                  | CSI_FIELD_TIME_BUCKET,
    /* privacy */                   CSI_PRIVACY_P0,
    /* default_ceiling_per_hour */  12,
  },
  {
    /* type_name */                "knock",
    /* allowed_fields */            CSI_FIELD_STATE_NAME
                                  | CSI_FIELD_CONFIDENCE
                                  | CSI_FIELD_TIME_BUCKET,
    /* privacy */                   CSI_PRIVACY_P0,
    /* default_ceiling_per_hour */  12,
  },
  {
    /* type_name */                "doorbell",
    /* allowed_fields */            CSI_FIELD_STATE_NAME
                                  | CSI_FIELD_CONFIDENCE
                                  | CSI_FIELD_TIME_BUCKET,
    /* privacy */                   CSI_PRIVACY_P0,
    /* default_ceiling_per_hour */  12,
  },
  {
    /* type_name */                "glass_break",
    /* allowed_fields */            CSI_FIELD_STATE_NAME
                                  | CSI_FIELD_CONFIDENCE
                                  | CSI_FIELD_TIME_BUCKET,
    /* privacy */                   CSI_PRIVACY_P0,
    /* default_ceiling_per_hour */  6,
  },
  {
    /* type_name */                "mic_muted",
    /* allowed_fields */            CSI_FIELD_STATE_NAME
                                  | CSI_FIELD_TIME_BUCKET,
    /* privacy */                   CSI_PRIVACY_P0,
    /* default_ceiling_per_hour */  6,
  },
  {
    /* type_name */                "mic_unmuted",
    /* allowed_fields */            CSI_FIELD_STATE_NAME
                                  | CSI_FIELD_TIME_BUCKET,
    /* privacy */                   CSI_PRIVACY_P0,
    /* default_ceiling_per_hour */  6,
  },
};

void on_init(const csi_module_settings_t* /*s*/) {}
void on_tick(const csi_features_t*        /*f*/) {}

const csi_module_t MODULE = {
  /* id */                 "acoustic.events",
  /* default_privacy */    CSI_PRIVACY_P0,
  /* events */             EVENTS,
  /* event_count */        sizeof(EVENTS) / sizeof(EVENTS[0]),
  /* init */               on_init,
  /* tick */               on_tick,
  /* on_event_dismissed */ nullptr,
  /* deinit */             nullptr,
};

void put_state_(csi_event_values_t* v, const char* tag) {
  v->present_fields |= CSI_FIELD_STATE_NAME;
  strncpy(v->state_name, tag, sizeof(v->state_name) - 1);
  v->state_name[sizeof(v->state_name) - 1] = '\0';
}

void put_confidence_(csi_event_values_t* v, uint8_t score) {
  v->present_fields |= CSI_FIELD_CONFIDENCE;
  const char* word = (score >= 80) ? "confirmed"
                   : (score >= 60) ? "observed"
                                   : "tentative";
  strncpy(v->confidence, word, sizeof(v->confidence) - 1);
  v->confidence[sizeof(v->confidence) - 1] = '\0';
}

}  /* namespace */

extern "C" {

const csi_module_t* acoustic_events_module(void) { return &MODULE; }

void acoustic_events_emit_detection(uint8_t event_type, uint8_t confidence) {
  const char* type_name;
  const char* state;
  csi_event_category_t category;
  switch (event_type) {
    case AUDIO_EVENT_T3_SMOKE_ALARM:
      /* Life-safety + shatter rank as anomalies so the Today summary's
       * "unusual" count and HA's prominent surfacing pick them up. */
      type_name = "smoke_alarm_t3"; state = "smoke_alarm";
      category = CSI_CATEGORY_ANOMALY;
      break;
    case AUDIO_EVENT_T4_CO_ALARM:
      type_name = "co_alarm_t4";    state = "co_alarm";
      category = CSI_CATEGORY_ANOMALY;
      break;
    case AUDIO_EVENT_GLASS_BREAK:
      type_name = "glass_break";    state = "glass_break";
      category = CSI_CATEGORY_ANOMALY;
      break;
    case AUDIO_EVENT_KNOCK:
      type_name = "knock";          state = "knock";
      category = CSI_CATEGORY_EVENT;
      break;
    case AUDIO_EVENT_DOORBELL:
      type_name = "doorbell";       state = "doorbell";
      category = CSI_CATEGORY_EVENT;
      break;
    default:
      return;  /* unknown detector output — nothing to publish */
  }

  csi_event_values_t v;
  csi_event_values_init(&v);
  v.category       = category;
  v.present_fields = CSI_FIELD_TIME_BUCKET;
  put_state_(&v, state);
  put_confidence_(&v, confidence);
  (void)csi_event_emit("acoustic.events", type_name, &v);
}

void acoustic_events_emit_mute(bool muted) {
  csi_event_values_t v;
  csi_event_values_init(&v);
  v.category       = CSI_CATEGORY_EVENT;
  v.present_fields = CSI_FIELD_TIME_BUCKET;
  put_state_(&v, muted ? "mic_muted" : "mic_on");
  (void)csi_event_emit("acoustic.events",
                       muted ? "mic_muted" : "mic_unmuted", &v);
}

}  /* extern "C" */

#endif /* FEATURE_ACOUSTIC_EVENTS */
