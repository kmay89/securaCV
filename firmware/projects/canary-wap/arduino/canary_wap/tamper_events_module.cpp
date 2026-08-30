/**
 * @file tamper_events_module.cpp
 * @brief Implementation of system.integrity module + watcher. See header.
 */

#include "build_config.h"

#include "tamper_events_module.h"

#include <string.h>

namespace {

/* One event type. The allow-list is the privacy contract: a tamper event
 * carries the KIND (state_name — a const.py vocabulary word, never free
 * text) and the coarse time bucket. Nothing else exists to leak: no
 * counts, no addresses, no timings finer than the bucket.
 *
 * State-bearing on purpose: the bundler keys on (module,type,state_name),
 * so a storm of one kind (an SD card flapping between error and mounted)
 * folds into ONE open bundle — the phone sees "SD card failing · ongoing"
 * instead of forty rows — while a DIFFERENT kind opens its own bundle,
 * because "the card failed" and "the box rebooted" are different stories.
 *
 * Ceiling 12/h: the acoustic module's per-life-safety-type budget. Real
 * tampers are rare; a source that could exceed this is a flapping card,
 * which the bundler already folds. */
const csi_event_decl_t EVENTS[] = {
  {
    /* type_name */                "tamper",
    /* allowed_fields */            CSI_FIELD_STATE_NAME
                                  | CSI_FIELD_TIME_BUCKET,
    /* privacy */                   CSI_PRIVACY_P0,
    /* default_ceiling_per_hour */  12,
  },
};

void on_init(const csi_module_settings_t* /*s*/) {}
void on_tick(const csi_features_t*        /*f*/) {}

const csi_module_t MODULE = {
  /* id */                 "system.integrity",
  /* default_privacy */    CSI_PRIVACY_P0,
  /* events */             EVENTS,
  /* event_count */        sizeof(EVENTS) / sizeof(EVENTS[0]),
  /* init */               on_init,
  /* tick */               on_tick,
  /* on_event_dismissed */ nullptr,
  /* deinit */             nullptr,
};

/* Mirrors hardware_state.h's SdState numeric values (that header defines
 * the sketch's globals and is single-include by design, so the values are
 * pinned here instead of included). ABSENT=0 / MOUNTED=1 / ERROR=2. */
constexpr uint8_t kSdAbsent  = 0;
constexpr uint8_t kSdMounted = 1;
constexpr uint8_t kSdError   = 2;

/* Watcher memory. Loop-task-only, like every csi_event emitter. */
bool    g_boot_reported   = false;
uint8_t g_boot_attempts   = 0;
bool    g_sd_adopted      = false;
uint8_t g_sd_prev         = kSdAbsent;

uint32_t emit_kind(const char* kind) {
  csi_event_values_t v;
  csi_event_values_init(&v);
  /* ANOMALY: a tamper must not be held by the quiet-hours gate — the
   * night the box reboots unexpectedly is precisely when the record
   * matters (the acoustic life-safety rationale). */
  v.category       = CSI_CATEGORY_ANOMALY;
  v.present_fields = CSI_FIELD_STATE_NAME | CSI_FIELD_TIME_BUCKET;
  strncpy(v.state_name, kind, sizeof(v.state_name) - 1);
  v.state_name[sizeof(v.state_name) - 1] = '\0';
  return csi_event_emit("system.integrity", "tamper", &v);
}

}  /* namespace */

extern "C" {

const csi_module_t* tamper_events_module(void) { return &MODULE; }

void tamper_events_watch(int reset_was_crash, int reset_was_watchdog,
                         int reset_was_brownout, uint8_t sd_state) {
  /* Boot classification: at most one report per boot, chosen by the
   * doctrine mapping in the header. Retried a bounded number of times so
   * a call that races module registration (or an unlucky rejection) does
   * not lose the boot story — and gives up quietly rather than looping
   * forever on a chokepoint that keeps saying no. */
  if (!g_boot_reported && g_boot_attempts < 100) {
    if (!reset_was_crash) {
      g_boot_reported = true;  /* a clean boot has nothing to confess */
    } else {
      const char* kind = reset_was_watchdog ? "watchdog"
                         : reset_was_brownout ? "power_loss"
                                              : "unexpected_reboot";
      ++g_boot_attempts;
      if (emit_kind(kind) != 0u) g_boot_reported = true;
    }
  }

  /* SD transitions. The first call adopts the current state silently —
   * booting with no card is a configuration, not a removal. */
  if (!g_sd_adopted) {
    g_sd_prev = sd_state;
    g_sd_adopted = true;
    return;
  }
  if (sd_state != g_sd_prev) {
    if (g_sd_prev == kSdMounted && sd_state == kSdError) {
      (void)emit_kind("sd_error");
    } else if (g_sd_prev == kSdMounted && sd_state == kSdAbsent) {
      (void)emit_kind("sd_remove");
    }
    /* MOUNTED after either is recovery, not a tamper: the bundler's quiet
     * gap closes the open bundle and the phone's flag drops on its own. */
    g_sd_prev = sd_state;
  }
}

void tamper_events_reset(void) {
  g_boot_reported = false;
  g_boot_attempts = 0;
  g_sd_adopted = false;
  g_sd_prev = kSdAbsent;
}

}  /* extern "C" */
