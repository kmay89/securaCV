/*
 * SecuraCV Canary — CSI module pipeline integration (PIO build)
 *
 * Bridges the canary product's CSI feed (the `csi::` adapter in
 * `firmware/canary/lib/securacv_csi/`, over the canonical HAL in
 * `firmware/common/csi/src/csi_hal.cpp`) into the cross-product CSI
 * module pipeline + privacy chokepoint + 10-min sliding-window
 * bundler that lives in `firmware/common/csi/`.
 *
 * Why a bridge: main.cpp should not have to include every module header
 * to feed one window, so the bridge is a small C ABI. It takes
 * `const void*` for historical reasons — the canary build once carried a
 * second `csi_features_t` typedef and the two could not meet in one TU.
 * Since roadmap 22 `securacv_csi.h` includes `csi_types.h`, there is one
 * struct, and main.cpp's `static_assert(sizeof(csi_features_t) == 36)`
 * pins the layout the pointer is cast back to.
 *
 * Privacy: every event committed by the modules registered here flows
 * through `csi_event_emit()`, which strips fields outside the per-event
 * allow-list, coarsens timestamps to 10-minute buckets, and enforces
 * per-module hourly ceilings before anything is published. Witness
 * chain integration is a stub on this build (canary has its own
 * witness pipeline already wired to `sensing_feed_*`).
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#ifndef SECURACV_CANARY_CSI_MODULES_INTEGRATION_H
#define SECURACV_CANARY_CSI_MODULES_INTEGRATION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize the module pipeline once at boot, after the canary CSI
 * HAL has been started. Registers the v1 modules — core.presence,
 * core.breathing, core.activity_ribbon, meta.daily_summary,
 * anomaly.baseline — with the common chokepoint, opens any persisted
 * settings from NVS, and primes per-module state.
 *
 * Idempotent: a second call is a no-op (re-registering the same module
 * id replaces the prior registration in place; settings are re-read).
 *
 * @return true on success; false if init failed (e.g. settings open
 *         failure mid-init — modules then run with built-in defaults).
 */
bool securacv_csi_modules_init(void);

/**
 * Hand the module pipeline one feature window. Called from inside the
 * existing `csi::set_features_callback` lambda after the canary
 * sensing aggregator has consumed the same window. Safe from any
 * task / timer the canary CSI HAL invokes the callback from.
 *
 * `features_blob` MUST point to a `csi_features_t` (csi_types.h — the
 * one definition both sides use); main.cpp's static_assert pins its size.
 *
 * Pass nullptr to no-op (e.g. before init has run).
 */
void securacv_csi_modules_feed(const void* features_blob);

/**
 * Feed the system.integrity tamper watcher once per main loop with the
 * facts only main.cpp can see together: the boot's reset classification
 * (crash / watchdog / brownout, the canary-wap reset_is_crash mapping)
 * and the SD state in the module's pinned ABSENT=0 / MOUNTED=1 / ERROR=2
 * numbering. Plain-typed on purpose — it keeps main.cpp free of the
 * module headers. (The typedef collision that first forced this shape is
 * gone since roadmap 22; the narrow ABI stays because it is the right
 * shape.)
 *
 * Safe to call before init(): an emit before the module is registered is
 * silently dropped by the chokepoint, and the module re-attempts the
 * boot classification (bounded) until one emit is accepted.
 */
void securacv_csi_modules_tamper_watch(int reset_was_crash,
                                       int reset_was_watchdog,
                                       int reset_was_brownout,
                                       uint8_t sd_state);

/**
 * Tear down the pipeline. Optional — only needed if the host wants
 * to disable module dispatch at runtime (e.g. user toggled a feature
 * flag mid-session). Releases any per-module state; safe to call
 * even when init() was not called.
 */
void securacv_csi_modules_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* SECURACV_CANARY_CSI_MODULES_INTEGRATION_H */
