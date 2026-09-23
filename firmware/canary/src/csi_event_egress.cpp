/*
 * SecuraCV Canary — committed csi_event egress (backlog F29). See header.
 */

#include "csi_event_egress.h"
#include "canary_config.h"

#if FEATURE_CSI

#include <Arduino.h>
#include <Preferences.h>

#include "csi_event.h"
#include "csi_event_id_floor.h"

#if FEATURE_HA_MQTT
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <string.h>

#include "csi_event_wire.h"
#include "identity/device_signature.h"
#include "securacv_mqtt.h"
#include "securacv_witness.h"
#endif

namespace {

/* ── Event-id floor (NVS) ─────────────────────────────────────────────────
 * The policy is common/csi/src/csi_event_id_floor.h, shared with the
 * canary-wap's csi_integration.cpp and host-tested across modeled reboots:
 * s_id_floor_stored is the value NVS holds, and an allocation at or past
 * it writes id + STRIDE before the id goes out. So NVS is always above
 * every id handed out, and a reboot skips ids at worst and reuses none. */
constexpr const char* kNvsNamespace = "securacv";
constexpr const char* kNvsKeyEventId = "csi.evid";
uint32_t              s_id_floor_stored = 0;

void restore_event_id_floor() {
  Preferences prefs;
  if (!prefs.begin(kNvsNamespace, /*readOnly=*/true)) return;
  const uint32_t persisted = (uint32_t)prefs.getULong(kNvsKeyEventId, 0);
  prefs.end();
  if (persisted > 0) {
    csi_event_set_event_id_floor(persisted);
    s_id_floor_stored = persisted;
  }
}

void persist_event_id_floor(uint32_t new_id) {
  Preferences prefs;
  if (!prefs.begin(kNvsNamespace, /*readOnly=*/false)) return;  // retried next id
  const uint32_t next_floor = csi_event_id_floor::floor_for(new_id);
  const bool wrote = prefs.putULong(kNvsKeyEventId, (unsigned long)next_floor) > 0;
  prefs.end();
  if (wrote) s_id_floor_stored = next_floor;
}

#if FEATURE_HA_MQTT
static_assert(csi_event_wire::SIG_B64URL_CAP == device_signature::SIG_B64URL_CAP,
              "csi_event_wire's signature buffer must hold a device_signature sig");

/* One committed row, copied out of the chokepoint's callback. */
struct CommittedEvent {
  uint32_t           event_id;
  uint32_t           committed_ms;
  char               module_id[CSI_EVENT_NAME_MAX];
  char               type_name[CSI_EVENT_NAME_MAX];
  uint8_t            category;
  uint8_t            privacy;
  csi_event_values_t values;
};

/* Eight rows: the modules' per-hour ceilings keep the commit rate far
 * below one per loop pass, so a full queue means the loop task is stuck —
 * and then dropping (counted) beats blocking the emitter. */
constexpr UBaseType_t kQueueDepth = 8;
constexpr int         kPumpBudget = 4;   // rows per loop pass
QueueHandle_t         s_queue = nullptr;
uint32_t              s_dropped = 0;     // queue full; atomic add
uint32_t              s_dropped_said = 0;  // what the log last reported

void copy_name(char (&dst)[CSI_EVENT_NAME_MAX], const char* src) {
  strncpy(dst, src ? src : "", CSI_EVENT_NAME_MAX - 1);
  dst[CSI_EVENT_NAME_MAX - 1] = '\0';
}

/* The boot story is already on this device's tamper topic: main.cpp
 * publishes the power-events classifier's verdict once per boot
 * (canary_power_events.h — power_loss for a brownout or a restored outage,
 * unexpected_reboot for any fault reset, watchdogs included, with the
 * outage length the csi row cannot carry). Bridging system.integrity's
 * boot kinds too would narrate one reset twice, in two words for a
 * watchdog. So the bridge carries the kinds only this module knows (SD,
 * enclosure); every row, boot kinds included, still rides `events`. */
bool boot_story_bridged_elsewhere(const char* kind) {
  return strcmp(kind, "power_loss") == 0 || strcmp(kind, "watchdog") == 0 ||
         strcmp(kind, "unexpected_reboot") == 0;
}
#endif  // FEATURE_HA_MQTT

}  // namespace

/* ── Strong overrides of csi_event.cpp's weak hooks ───────────────────── */

extern "C" void csi_event_on_id_advance(uint32_t new_id) {
  /* One NVS write per boot that allocates, plus one per STRIDE ids. */
  if (!csi_event_id_floor::must_persist(s_id_floor_stored, new_id)) return;
  persist_event_id_floor(new_id);
}

#if FEATURE_HA_MQTT
extern "C" void csi_event_on_committed(uint32_t                  event_id,
                                       const char*               module_id,
                                       const char*               type_name,
                                       csi_event_category_t      category,
                                       csi_privacy_class_t       privacy,
                                       const csi_event_values_t* values) {
  if (!values || !s_queue) return;
  /* The chokepoint already refuses rows above the ceiling; this keeps the
   * wire surface's rule visible here too, as the canary-wap does. */
  if (privacy > csi_event_get_privacy_ceiling()) return;
  CommittedEvent rec;
  rec.event_id     = event_id;
  rec.committed_ms = millis();
  copy_name(rec.module_id, module_id);
  copy_name(rec.type_name, type_name);
  rec.category     = (uint8_t)category;
  rec.privacy      = (uint8_t)privacy;
  rec.values       = *values;
  /* Never block the emitter (it may be the NimBLE host task). */
  if (xQueueSend(s_queue, &rec, 0) != pdTRUE) {
    __atomic_add_fetch(&s_dropped, 1u, __ATOMIC_RELAXED);
  }
}
#endif  // FEATURE_HA_MQTT

extern "C" void csi_event_egress_begin(void) {
  restore_event_id_floor();
#if FEATURE_HA_MQTT
  if (!s_queue) s_queue = xQueueCreate(kQueueDepth, sizeof(CommittedEvent));
  if (!s_queue) {
    Serial.println("[WARN] CSI event egress queue unavailable - events stay local");
  }
  /* The witness key signs the events body (device_signature's `event`
   * canonical, which HA's verify_event rebuilds). device_signature keeps
   * its own copy of the identity. */
  const DeviceIdentity& dev = witness_get_device();
  static const char kHex[] = "0123456789abcdef";
  char fp_hex[17];
  for (int i = 0; i < 8; ++i) {
    fp_hex[2 * i]     = kHex[(dev.pubkey_fp[i] >> 4) & 0xF];
    fp_hex[2 * i + 1] = kHex[dev.pubkey_fp[i] & 0xF];
  }
  fp_hex[16] = '\0';
  device_signature::init(dev.privkey, dev.pubkey, dev.device_id, fp_hex);
#endif
}

extern "C" void csi_event_egress_pump(void) {
#if FEATURE_HA_MQTT
  if (!s_queue) return;
  /* mqtt_accepting() stays true through a broker outage (the publish then
   * buffers in the MQTT layer's offline queue); it is false only when no
   * broker is configured, and then there is nowhere to carry a row — drain
   * without publishing, so a broker configured later is not flooded with
   * stale history. */
  const bool accepting = mqtt_accepting();
  const uint32_t dropped = __atomic_load_n(&s_dropped, __ATOMIC_RELAXED);
  if (dropped != s_dropped_said) {
    s_dropped_said = dropped;
    Serial.printf("[CSI] egress queue full: %lu committed event(s) not published\n",
                  (unsigned long)dropped);
  }
  CommittedEvent rec;
  for (int budget = kPumpBudget; budget > 0; --budget) {
    if (xQueueReceive(s_queue, &rec, 0) != pdTRUE) break;
    if (!accepting) continue;
    const csi_event_wire::Signer signer = {
      &device_signature::sign_event,
      device_signature::SCHEMA_V,
      device_signature::ALG_NAME,
      device_signature::fingerprint_hex(),
    };
    char body[768];
    const size_t n = csi_event_wire::build_event_body(
        body, sizeof(body), rec.event_id, rec.module_id, rec.type_name,
        (csi_event_category_t)rec.category, (csi_privacy_class_t)rec.privacy,
        &rec.values, rec.committed_ms, /*bundled_count=*/1,
        /*is_replay=*/false, signer);
    if (n == 0) continue;
    mqtt_publish_event(body);
    /* The per-kind tamper bridge, as the canary-wap's csi_mqtt: HA's
     * per-type tamper sensors (SD Removed, Enclosure Open, ...) match
     * {"type": <kind>} on the tamper topic. Not retained — an event, not
     * a state; a retained copy would re-fire HA's edge-latched sensors on
     * every HA restart. */
    char tb[128];
    if (csi_event_wire::build_tamper_bridge_body(tb, sizeof(tb), rec.module_id,
                                                 rec.type_name, &rec.values) > 0 &&
        !boot_story_bridged_elsewhere(rec.values.state_name)) {
      mqtt_publish_tamper(tb, /*retained=*/false);
    }
  }
#endif
}

#else  // !FEATURE_CSI

extern "C" void csi_event_egress_begin(void) {}
extern "C" void csi_event_egress_pump(void) {}

#endif  // FEATURE_CSI
