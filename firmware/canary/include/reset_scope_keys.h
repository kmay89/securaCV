/*
 * SecuraCV Canary — what each factory-reset stage clears.
 *
 * The BOOT hold (common/io/reset_hold.h, wired in main.cpp) has two acting
 * stages. The 10 s release is ResetScope::CredentialsAndSettings: the node
 * goes back to the box state ON THE NETWORK — saved networks, broker,
 * setup flags and household settings gone — and KEEPS its witness identity,
 * because the Ed25519 key is the device's birth certificate and every hub
 * and display that pinned it would otherwise see a stranger
 * (docs/TRUST_MODEL.md; onboarding_fsm.h decision 3). The 30 s hold is
 * ResetScope::Everything, today's whole-namespace clear, identity too.
 *
 * The first list is what the 10 s stage removes, key by key; the second is
 * the identity and chain state it must never touch. Both are data so
 * firmware/tests_host/test_canary_onboarding_glue.cpp can hold them apart
 * and hold every spelling here to the source that owns it. The keys this
 * tree spells in canary_config.h are referenced by macro; the ones other
 * files keep private (securacv_mqtt.cpp's NVS_KEY_MQTT_*, the shared broker
 * transport's TLS trio, securacv_setup.cpp's zone pair) are spelled once
 * more here, and the host test reads those files to prove the spelling.
 *
 * Pure: no Arduino.
 */

#pragma once

#include <stddef.h>

#include "canary_config.h"
#include "network/wifi_store.h"

namespace canary {
namespace reset_scope {

/// ResetScope::CredentialsAndSettings — the 10 s release.
inline constexpr const char* const kCredentialsAndSettings[] = {
  // Wi-Fi: the legacy current pick and the saved-networks ring.
  NVS_KEY_WIFI_SSID,
  NVS_KEY_WIFI_PASS,
  NVS_KEY_WIFI_EN,
  canary::net::wifi_store::NVS_KEY,
  // Broker: securacv_mqtt.cpp NVS_KEY_MQTT_* ...
  "mqtt_host",
  "mqtt_port",
  "mqtt_user",
  "mqtt_pass",
  "mqtt_en",
  // ... and common/network/mqtt_transport.h's TLS trio (mode, CA, pin).
  "mqtt_tls",
  "mqtt_ca",
  "mqtt_fp",
  // Setup state and settings.
  NVS_KEY_SETUP_OK,
  NVS_KEY_DEV_NAME,
  NVS_KEY_TOKEN,   // re-derived from the kept key on the next boot (securacv_auth.cpp)
  "tz",            // securacv_setup.cpp: the household zone ...
  "tz_iana",       // ... and the IANA name it was mapped from
};
inline constexpr size_t kCredentialsAndSettingsCount =
    sizeof(kCredentialsAndSettings) / sizeof(kCredentialsAndSettings[0]);

/// What that stage keeps: the identity, the chain, the device's history.
/// (ResetScope::Everything clears the namespace; these go with it.)
inline constexpr const char* const kIdentityKept[] = {
  NVS_KEY_PRIV,
  NVS_KEY_CHAINST,
  NVS_KEY_SEQ,
  NVS_KEY_CHAIN,
  NVS_KEY_BOOTS,
  NVS_KEY_TAMPER,
  NVS_KEY_LOGSEQ,
  NVS_KEY_BORN,
  NVS_KEY_BORN_EX,
  NVS_KEY_TLS_CERT,   // the pin the provisioning receipt handed the phone
  NVS_KEY_TLS_KEY,
};
inline constexpr size_t kIdentityKeptCount =
    sizeof(kIdentityKept) / sizeof(kIdentityKept[0]);

}  // namespace reset_scope
}  // namespace canary
