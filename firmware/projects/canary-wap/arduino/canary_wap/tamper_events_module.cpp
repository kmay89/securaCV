/**
 * @file tamper_events_module.cpp
 * @brief Implementation of system.integrity module + watcher. See header.
 */

#include "tamper_events_module.h"

#include "csi_bundler.h"

#include <string.h>

namespace {

/* One event type. The allow-list is the privacy contract: a tamper event
 * carries the KIND (state_name — a const.py vocabulary word, never free
 * text) and the coarse time bucket. Nothing else exists to leak: no
 * counts, no addresses, no timings finer than the bucket.
 *
 * State-bearing so the kind rides `state` down every existing rail — but
 * a tamper must not LINGER in an open bundle: the bundler's gap window is
 * two minutes, and the exact failure this event records (another crash,
 * another power cut) would erase a buffered bundle before it ever reached
 * the signed chain. So emit_kind() force-closes its bundle immediately
 * (csi_bundler_flush_key): every tamper is sealed the moment it is
 * accepted. The anti-noise mechanism is the watcher itself — it emits
 * only on transitions and one boot story per boot — not the fold.
 *
 * Ceiling 12/h: the acoustic module's per-life-safety-type budget. Real
 * tampers are rare; the only source that could exceed it is a flapping
 * card, and the ceiling is exactly the guard for that. */
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

/* Mirrors the canary-wap sketch's hardware_state.h SdState numeric values
 * (that header defines the sketch's globals and is single-include by
 * design, so the values are pinned here instead of included). A host with
 * no SD state machine feeds one of these as a constant — the watcher
 * adopts it on the first call and never emits an SD kind.
 * ABSENT=0 / MOUNTED=1 / ERROR=2. */
constexpr uint8_t kSdAbsent  = 0;
constexpr uint8_t kSdMounted = 1;
constexpr uint8_t kSdError   = 2;

/* Watcher memory. Loop-task-only, like every csi_event emitter. */
bool    g_boot_reported   = false;
uint8_t g_boot_attempts   = 0;
bool    g_sd_adopted      = false;
uint8_t g_sd_prev         = kSdAbsent;
/* The standing conditions (see tamper_events_active_kind in the header).
 * Two levels, because they end differently: the boot story stands for the
 * whole boot, the SD story ends when the card comes back — and recovery
 * of the card must not erase how the boot began. */
char    g_boot_kind[24]   = "";
char    g_sd_kind[24]     = "";

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
  const uint32_t id = csi_event_emit("system.integrity", "tamper", &v);
  /* Durability over bundling: a state-bearing admit only OPENS a bundle,
   * and an open bundle is RAM. Seal it now — the failure being recorded
   * is precisely the one that would destroy a two-minute buffer. */
  if (id != 0u) csi_bundler_flush_key("system.integrity", "tamper", kind);
  return id;
}

void remember_kind(char (&slot)[24], const char* kind) {
  strncpy(slot, kind, sizeof(slot) - 1);
  slot[sizeof(slot) - 1] = '\0';
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
      if (emit_kind(kind) != 0u) {
        g_boot_reported = true;
        remember_kind(g_boot_kind, kind);   /* stands for the whole boot */
      }
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
      if (emit_kind("sd_error") != 0u) remember_kind(g_sd_kind, "sd_error");
    } else if (g_sd_prev == kSdMounted && sd_state == kSdAbsent) {
      if (emit_kind("sd_remove") != 0u) remember_kind(g_sd_kind, "sd_remove");
    } else if (sd_state == kSdMounted) {
      /* MOUNTED after either is recovery, not a tamper — and it ENDS the
       * standing SD story: the wire's present tense drops it on the next
       * poll. The boot kind, if one stands, stays: the card coming back
       * says nothing about how the boot began. */
      g_sd_kind[0] = '\0';
    }
    g_sd_prev = sd_state;
  }
}

const char* tamper_events_active_kind(void) {
  /* The SD story speaks first when both stand — it is the newer news and
   * the actionable one; the boot story resurfaces once the card is back. */
  return g_sd_kind[0] ? g_sd_kind : g_boot_kind;
}

void tamper_events_reset(void) {
  g_boot_reported = false;
  g_boot_attempts = 0;
  g_sd_adopted = false;
  g_sd_prev = kSdAbsent;
  g_boot_kind[0] = '\0';
  g_sd_kind[0] = '\0';
}

}  /* extern "C" */
