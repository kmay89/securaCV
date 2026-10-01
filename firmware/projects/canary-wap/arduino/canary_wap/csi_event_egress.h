/**
 * @file csi_event_egress.h
 * @brief Where a committed csi_event goes after the chokepoint: the SD event
 *        log, and the MQTT `events` / `tamper` topics, with the delivery
 *        watermark the reconnect backfill keeps.
 *
 * csi_integration.cpp's csi_event_on_committed() hands every committed row
 * here; csi_mqtt.cpp owns the wire (publish_event_row,
 * publish_tamper_bridge) and this file decides what goes when. Split out
 * of csi_mqtt.cpp and the commit hook so the decisions can be driven on
 * the host (tests_host/test_wap_event_egress.cpp) against the real SD
 * event log and CSI library.
 *
 * The delivery watermark is the highest event id handed to the broker. It
 * comes back across a reboot from a CEILING in NVS (csi_mqtt::
 * NVS_KEY_DELIVERED, F47), written with csi_event_backfill.h's rule before
 * an id is handed over.
 */

#ifndef SECURACV_WAP_CSI_EVENT_EGRESS_H
#define SECURACV_WAP_CSI_EVENT_EGRESS_H

#include <csi_event.h>
#include <stdint.h>

namespace csi_event_egress {

/* Restore the delivery watermark from NVS. Once per boot: a second call is
 * a no-op (a runtime MQTT re-init keeps the exact RAM watermark). Needs the
 * event-id floor already restored (csi_integration::event_id_floor_stored). */
void begin();

/* The chokepoint's commit hook, after csi_integration's privacy gate. */
void on_committed(uint32_t                  event_id,
                  const char*               module_id,
                  const char*               type_name,
                  csi_event_category_t      category,
                  csi_privacy_class_t       privacy,
                  const csi_event_values_t* values);

/* Main loop: the SD backfill a broker (re)connect asked for. */
void pump();

/* The delivery watermark (diagnostics and host tests). */
uint32_t watermark();

#ifdef CSI_TEST_HOST_BUILD
/* Host tests only: forget this "boot"'s RAM state, to simulate a reboot. */
void test_reset();
#endif

}  // namespace csi_event_egress

#endif  // SECURACV_WAP_CSI_EVENT_EGRESS_H
