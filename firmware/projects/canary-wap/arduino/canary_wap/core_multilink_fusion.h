/**
 * @file core_multilink_fusion.h
 * @brief core.multilink_fusion — promotes single-link motion observations to
 *        `confirmed` confidence when ≥2 independent links agree.
 *
 * Sits alongside `core.presence`: presence keeps emitting single-link
 * "observed" events from local features; fusion adds a separate
 * `motion_confirmed` event when the local Sensor's view AND at least one
 * paired peer Sensor's view both report motion within a 3-second sliding
 * window. The Hub UI joins the two streams.
 *
 * Per-link state is keyed on the 8-byte sender fingerprint
 * (mesh_crypto::FINGERPRINT_LEN). The integration layer (PR 2h /
 * main.cpp) invokes ingest_peer_features() when a verified CSI_FEATURES
 * envelope arrives via mesh_envelope::parse_and_verify().
 *
 * This module is allocation-free and holds no globals beyond its own
 * fixed-size link table. It MUST NOT depend on or reach into other
 * modules' state — coordination is via published events.
 */

#ifndef SECURACV_CSI_MODULE_CORE_MULTILINK_FUSION_H
#define SECURACV_CSI_MODULE_CORE_MULTILINK_FUSION_H

#include "csi_module.h"

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Per-peer link state is keyed on the 8-byte Ed25519-pubkey fingerprint
 * defined by the mesh layer (mesh_crypto::FINGERPRINT_LEN). We re-declare
 * the constant here rather than #include the mesh header — this module
 * lives under firmware/common/csi/ which is a leaf of the dependency
 * graph (mesh sits one layer up). The two are pinned at 8 bytes by
 * static_assert in the .cpp. */
#define CORE_MULTILINK_FINGERPRINT_LEN 8

/* Max peer links. Matches MESH_MAX_PEERS-1 in mesh_transport.h
 * (one slot lost to self). */
#define CORE_MULTILINK_MAX_PEERS 7

/* Sliding-window age for fusion. Peer features older than this are
 * considered stale and don't count toward the confirmation gate. The
 * canonical CSI window is 1 s, so 3 s = 3 windows of slack matches
 * the design doc's "3-window sliding overlap". */
#define CORE_MULTILINK_PEER_STALE_MS 3000

/** Returns the static singleton manifest. Pass to csi_module_register(). */
const csi_module_t* core_multilink_fusion_module(void);

/**
 * Feed a paired peer's 1 Hz feature window into the fusion table.
 *
 * The integration layer calls this after verifying an incoming
 * CSI_FEATURES mesh frame (mesh_envelope::parse_and_verify succeeded;
 * sender_fp matched a known peer pubkey). The fingerprint passed here
 * MUST be the same 8-byte value the mesh layer uses for that peer —
 * we look up by exact-match.
 *
 * Idempotent: ingesting twice with the same fingerprint overwrites the
 * previous entry. If the table is full and the fingerprint is new, the
 * oldest entry is evicted (peer churn is more important than retaining
 * a stale link).
 */
void core_multilink_fusion_ingest_peer_features(
    const uint8_t fingerprint[CORE_MULTILINK_FINGERPRINT_LEN],
    const csi_features_t* features);

/* ──────────────────────────────────────────────────────────────────────────
 * TEST HOOKS (host build only)
 * ────────────────────────────────────────────────────────────────────────── */

#ifdef CSI_TEST_HOST_BUILD
/* Reset all internal state — used by the test harness between cases. */
void core_multilink_fusion_test_reset(void);

/* Inject a virtual clock for staleness tests. The default uses millis()
 * on device; on host the default is a process-static counter. */
void core_multilink_fusion_test_set_now_ms(uint32_t now_ms);

/* Inspect the per-link table. Returns the number of in-use slots. */
size_t core_multilink_fusion_test_link_count(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* SECURACV_CSI_MODULE_CORE_MULTILINK_FUSION_H */
