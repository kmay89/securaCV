/*
 * SecuraCV Canary — Beacon Channel (Neighborhood Harm-Reduction Network)
 *
 * Higher-trust, narrowly-scoped, supervised-health broadcast layer.
 * Sits next to Chirp on the same ESP-NOW radio but enforces a fundamentally
 * different trust model:
 *
 *   - Persistent device Ed25519 identity (NOT ephemeral session keys).
 *   - Two-pubkey cryptographic co-signing required for every origination.
 *   - Narrow life-safety-only template set (~13 templates).
 *   - NFPA-72-style supervised health state surface
 *     (Normal / Trouble / Alarm / Supervisory).
 *   - CAP-aligned wire fields (severity, urgency, certainty, msgType).
 *   - Audit log of every received frame, chain-hashed and signed.
 *
 * Magic byte: 0xB1 (distinct from Chirp 0xC4).
 *
 * See spec/beacon_channel_v0.md for the full specification and
 * spec/beacon_cap_gateway_v0.md for the deferred CAP interop layer.
 *
 * Status: scaffolding (v0.1). Full integration with the application loop and
 * REST surface is gated behind a separate FEATURE_BEACON_CHANNEL build flag
 * (default OFF). When wired up, the REST endpoints MUST follow the
 * Bearer-token template trampoline pattern used by /api/mesh/* and
 * /api/bluetooth/*.
 */

#ifndef SECURACV_BEACON_CHANNEL_H
#define SECURACV_BEACON_CHANNEL_H

#include <Arduino.h>
#include "build_config.h"
#include "log_level.h"
// Constants, enums and wire structs (spec/beacon_channel_v0.md §5), in this
// same namespace. Arduino-free, so the host tests include the real layouts.
#include "beacon_wire.h"

#ifndef FEATURE_BEACON_CHANNEL
#define FEATURE_BEACON_CHANNEL 0
#endif

namespace beacon_channel {

// ════════════════════════════════════════════════════════════════════════════
// CALLBACKS
// ════════════════════════════════════════════════════════════════════════════

typedef void (*BeaconAlarmCallback)(const BeaconAlertCanonical* alert);
typedef void (*BeaconStateCallback)(BeaconState old_state, BeaconState new_state);
typedef void (*BeaconCosignRequestCallback)(const BeaconAlertCanonical* requested,
                                            const uint8_t* originator_fp);

// ════════════════════════════════════════════════════════════════════════════
// PUBLIC API
// ════════════════════════════════════════════════════════════════════════════

#if FEATURE_BEACON_CHANNEL

// Initialize the Beacon channel (call once at boot).
// device_privkey/pubkey are the device's persistent Ed25519 identity (same
// keys used by Opera and witness records). beacon_channel does NOT generate
// its own keys.
bool init(const uint8_t* device_privkey, const uint8_t* device_pubkey,
          const char* device_name);

// Shutdown.
void deinit();

// Enable / disable.
void set_enabled(bool enabled);
bool is_enabled();

// Main-loop tick (handles state transitions, self-test cadence, trouble
// detection, expired alarms).
void update();

// Status (state, trouble reasons, active alarm).
BeaconStatus get_status();
const char* state_name(BeaconState state);

// ── Beacon set management ──
uint8_t get_beacon_set_size();
const BeaconSetEntry* get_beacon_set_entry(uint8_t index);
bool revoke_beacon_set_entry(const uint8_t* fingerprint);

// v0.5: auto-revoke a Beacon-set member when their device sends a tamper
// alert (or a local tamper sensor fires on a paired neighbor's Opera
// channel). Called from mesh_network.cpp::handle_tamper_alert. The lookup
// computes the Beacon set's 16-byte fingerprint from the device's full
// Ed25519 pubkey and revokes the entry if present.
//
// Returns true if a matching beacon set entry was found and revoked
// (false if the device isn't paired into our Beacon set — no action
// needed). Idempotent; safe to call multiple times.
//
// Threading: same single-task invariant as the rest of beacon_channel.
// Called from the mesh_network message-dispatch path, which runs on the
// main loop task (see mesh_network.cpp::update() and the g_rx_pending
// queue model).
bool on_peer_tampered(const uint8_t* device_pubkey);

// Pairing flow.
bool start_pair_init();
bool start_pair_join();
bool confirm_pair();
void cancel_pair();
bool is_pairing();

// ── Origination flow (two-pubkey co-signing) ──
//
// Step 1: user A holds-to-send → originate_alert() builds the canonical,
// signs with the local device key, and broadcasts a COSIGN_REQ to nearby
// paired devices.
// Returns false if rate-limited, presence requirement not met, no paired
// cosigner candidate available, or time not synced.
bool originate_alert(BeaconTemplate template_id, BeaconUrgency urgency,
                     BeaconSeverity severity, BeaconCertainty certainty,
                     BeaconDetailSlot detail, uint32_t ttl_minutes);

// Step 2 (on the cosigner): respond to a pending COSIGN_REQ. The user has
// confirmed the alarm. The local device signs the canonical and emits a
// COSIGN_RESP back to the originator.
bool cosign_pending_request(bool confirm);

// True when pick_cosign_candidate() would find a paired neighbor able to
// take an encrypted COSIGN_REQ right now: non-revoked, X25519 key known,
// selftest inside COSIGN_FRESHNESS_MS (or not yet observed since boot). The
// two-device path is open exactly when this is true — and the solo path is
// closed (spec §6.2). The REST layer checks it first so it can name the
// reason (`paired_cosigner_available`) instead of a generic refusal.
bool paired_cosigner_available();

// Solo-degraded origination path (spec §6.2). For genuinely single-device
// households that have no paired Beacon-set neighbor — or none able to
// cosign right now. Refused whenever paired_cosigner_available() is true.
//
// Caller MUST:
//   - Have the physical BOOT button held DOWN at the moment of this call
//     (firmware checks the GPIO state in real time).
//   - Have passed the hold-to-send UI interaction (the REST handler
//     enforces this).
//
// Frame produced:
//   - originator_fp == cosigner_fp == this device's fingerprint
//   - flags |= BCN_FLAG_SOLO_ORIGIN
//   - certainty = BCN_CERT_OBSERVED (regardless of the parameter; the
//     spec forbids elevation of solo frames)
//   - single Ed25519 signature, copied into both signature slots
//
// Returns false if:
//   - a fresh paired cosigner is available (use originate_alert instead)
//   - BOOT button is not currently held
//   - device key self-test fails
//   - rate limit exceeded
//   - time is not wall-clock-synced
bool originate_alert_solo(BeaconTemplate template_id, BeaconUrgency urgency,
                          BeaconSeverity severity, BeaconDetailSlot detail,
                          uint32_t ttl_minutes);

// Returns the GPIO state of the BOOT button as a real-time check (no
// debouncing — solo origination is a held-button-while-holding-to-send
// pattern that takes ~2 s, swamping any switch bounce). Implementation
// reads GPIO 0 on ESP32-S3 (the standard BOOT pin); other boards may
// remap via beacon_set_boot_button_gpio(uint8_t).
bool boot_button_held();
void set_boot_button_gpio(uint8_t gpio);

// Step 3 (back on originator, automatic): receive COSIGN_RESP, verify, emit
// the dual-signed frame at hop 0 — with the header msg_type the signed
// canonical carries (ALERT, or CANCEL from originate_cancel) — and adopt it
// locally, since ESP-NOW never delivers a broadcast back to its sender: the
// originator enters ALARM for its own ALERT and leaves it for its own CANCEL.

// ── Network all-clear (spec §10 /api/beacon/cancel) ──
//
// Originate a BEACON_MSG_CANCEL naming the alarm this device holds
// (ref_canceled_nonce = that alarm's frame nonce, spec §5.4). Same
// two-pubkey cosign flow as originate_alert: a paired neighbor confirms and
// signs, and only a neighbor that ALSO holds this alarm will sign it
// (cosigner-side gate). `clr_template` must be an all-clear template
// (BCN_CLR_RESOLVED / _SAFE / _FALSE_ALARM). Charged to this device's 24 h
// origination bucket like an ALERT — receivers charge it there too (spec §8).
// Returns false if there is no active alarm, the clock is unsynced, the
// template is not an all-clear, no paired cosigner is available, or the
// bucket is spent.
bool originate_cancel(BeaconTemplate clr_template, BeaconCertainty certainty,
                      uint32_t ttl_minutes);

// Solo CANCEL (spec §6.2 step 4: msg_type = Cancel on the solo path). Same
// gates as originate_alert_solo: refused while a fresh paired cosigner is
// available, and refused unless the physical BOOT button is held right now.
// The frame carries BCN_FLAG_SOLO_ORIGIN, certainty = Observed whatever was
// asked, and one signature in both slots. Adopted locally at hop 0.
bool originate_cancel_solo(BeaconTemplate clr_template, uint32_t ttl_minutes);

// Silence the active alarm on THIS device only — the local mute. Sends
// nothing: paired devices stay in ALARM until the alarm's own `expires` or a
// network CANCEL (originate_cancel / originate_cancel_solo) reaches them.
// Any surface that calls this must say exactly that; "canceled" would be a
// claim the network never saw.
bool silence_active_alarm();

// ── Active alarm + audit log ──
bool has_active_alarm();
const BeaconAlertCanonical* get_active_alarm();
size_t get_audit_log_count();
const BeaconAuditEntry* get_audit_log_entry(size_t index);

// ── Self-test ──
// Force a self-test emission (mostly for tests / on-demand operator check).
bool emit_selftest();

// ── Callbacks ──
void set_alarm_callback(BeaconAlarmCallback callback);
void set_state_callback(BeaconStateCallback callback);
void set_cosign_request_callback(BeaconCosignRequestCallback callback);

// Dispatch an ESP-NOW frame received by the shared mesh callback.
void dispatch_espnow_message(const uint8_t* mac, const uint8_t* data,
                             int len, int8_t rssi_dbm);

#endif // FEATURE_BEACON_CHANNEL

} // namespace beacon_channel

#endif // SECURACV_BEACON_CHANNEL_H
