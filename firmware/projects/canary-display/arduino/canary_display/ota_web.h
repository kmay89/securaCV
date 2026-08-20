#pragma once
#include <stdint.h>

// The display's /api/ota/* web surface — the same one-click contract the
// flagship canary serves (status / check / install), so the desktop
// Flasher's "Update over the air" button works on a display too. Every
// call routes through ota_mgr, never straight into the raw engine: the
// HTTP trigger and HA's MQTT Install button take the same code path, and
// the engine's own in-flight guard is the single arbiter of "busy".
//
// Net-local ON PURPOSE — only glass_web.cpp and ota_mgr.cpp include this.
// ota_mgr.h is included by main.cpp and ui/settings_ui.cpp, which the WASM
// emulator compiles (canary-local/emulator/build.sh); web-only additions
// live here so those TUs' bytes never move for a surface the emulator
// doesn't build.

namespace canary::net {

// The engine's answer to a check/install kick — the web layer's whole
// vocabulary for it, mapped 1:1 onto the HTTP statuses the canary's
// /api/ota routes answer (200 / 409 / 500).
enum class OtaKick : uint8_t {
  Started,  // the async engine task is off and running → 200
  Busy,     // a check or install is already in flight  → 409
  Failed,   // the engine refused (init failed, no task) → 500
};

// Everything GET /api/ota/status serves. Static engine strings except
// `latest`, which is parsed from the remote manifest — the web handler
// escapes that one before it rides a JSON document.
struct OtaWebStatus {
  const char* installed;    // running firmware version (CANARY_FW_VERSION)
  const char* latest;       // manifest's version (nullptr until a check ran)
  const char* state;        // technical state ("Idle", "Downloading", ...)
  const char* state_text;   // friendly one-liner ("" when idle)
  const char* error;        // technical last error ("No error" when none)
  const char* error_text;   // friendly error ("" when none)
  bool update_available;    // a newer, verified image is offered
  bool auto_update;         // nightly auto-install is on
  uint8_t progress;         // 0..100 during download, 0 otherwise
};

// Snapshot the engine for GET /api/ota/status. Cheap; call per request.
OtaWebStatus ota_web_status();

// POST /api/ota/check — fetch the manifest and compare versions, without
// installing. Non-blocking; the result lands in ota_web_status().
OtaKick ota_web_check();

// POST /api/ota/install — securacv_ota_check_and_install() semantics
// (check INCLUDED — a separate /api/ota/check first would make this 409).
// Non-blocking: a worker task downloads, verifies, flashes and reboots,
// so the HTTP response is on the wire long before any restart.
OtaKick ota_web_install();

}  // namespace canary::net
