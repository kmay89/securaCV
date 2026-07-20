/*
 * SecuraCV Canary — USB evidence drive glue. See the header for the
 * contract and docs/design/usb_evidence_drive.md for the design.
 *
 * Build shape: the whole implementation sits behind
 * FEATURE_USB_EVIDENCE_DRIVE && !ARDUINO_USB_MODE so that the stock WAP
 * build (hwcdc, flag off) compiles this file to an honest no-op stub —
 * `u` on the console answers "not in this build" — while the opt-in
 * usbdrive build gets the real thing. PHASE 2 (on-hardware validation)
 * PENDING; see the design doc's checklist.
 */

#include "build_config.h"
#include "usb_evidence_drive.h"

#if FEATURE_USB_EVIDENCE_DRIVE && defined(ARDUINO_USB_MODE) && (ARDUINO_USB_MODE == 0)

#include <Arduino.h>
#include <USB.h>
#include <USBMSC.h>
#include <SD.h>
#include <ArduinoJson.h>
#include <Ed25519.h>
#include <Preferences.h>
#include "mbedtls/sha256.h"
#include "esp_ota_ops.h"

#include "evidence_drive_logic.h"
#include "evidence_update_verify.h"
#include "securacv_ota.h"
#include "ota_release_key.h"

namespace usb_evidence_drive {

using namespace evidence_drive;

static Config s_cfg = {};
static Mode s_mode = Mode::OFF;
static USBMSC s_msc;
static bool s_usb_started = false;
static uint8_t* s_staging = nullptr;          // PSRAM FAT16 volume
static ShareStatus s_share;
static char s_status[96] = "USB drive off";

// Deferred-event flags — USB callbacks run in the TinyUSB task; anything
// heavy waits for poll() on the main loop.
static volatile bool s_pending_eject = false;
static volatile bool s_pending_unplug = false;

// ── MSC callbacks ───────────────────────────────────────────────────────────
static int32_t on_read(uint32_t lba, uint32_t offset, void* buffer, uint32_t bufsize) {
  if (s_mode == Mode::UPDATE && s_staging) {
    const uint64_t off = (uint64_t)lba * FAT_SECTOR_SIZE + offset;
    if (off + bufsize > FAT_VOLUME_BYTES) return -1;
    memcpy(buffer, s_staging + off, bufsize);
    return (int32_t)bufsize;
  }
  if (s_mode == Mode::EVIDENCE) {
    // Whole-sector reads straight off the SD (SPI). offset is always 0 for
    // sector-aligned MSC reads with 512-byte sectors.
    if (offset != 0 || (bufsize % 512) != 0) return -1;
    uint8_t* out = (uint8_t*)buffer;
    for (uint32_t i = 0; i < bufsize / 512; i++) {
      if (!SD.readRAW(out + i * 512, lba + i)) return -1;
    }
    return (int32_t)bufsize;
  }
  return -1;
}

static int32_t on_write(uint32_t lba, uint32_t offset, uint8_t* buffer, uint32_t bufsize) {
  // EVIDENCE is read-only at the protocol level; only UPDATE accepts writes.
  if (s_mode != Mode::UPDATE || !s_staging) return -1;
  const uint64_t off = (uint64_t)lba * FAT_SECTOR_SIZE + offset;
  if (off + bufsize > FAT_VOLUME_BYTES) return -1;
  memcpy(s_staging + off, buffer, bufsize);
  return (int32_t)bufsize;
}

static bool on_start_stop(uint8_t /*power*/, bool start, bool load_eject) {
  if (!start && load_eject) s_pending_eject = true;
  return true;
}

// ── crypto deps for the verifier ────────────────────────────────────────────
static bool dep_ed25519(const uint8_t sig[64], const uint8_t pub[32],
                        const uint8_t* msg, size_t len) {
  return Ed25519::verify(sig, pub, msg, len);
}
static void dep_sha256(const uint8_t* data, size_t len, uint8_t out[32]) {
  mbedtls_sha256(data, len, out, 0 /* SHA-256, not 224 */);
}

// ── update pipeline (runs on the main loop via poll) ────────────────────────
static void run_update_check() {
  FatFile bin, man;
  char reason[96];
  if (!fat16_find_update(s_staging, &bin, &man, reason, sizeof reason)) {
    snprintf(s_status, sizeof s_status, "update drop: %s", reason);
    fat16_write_result(s_staging, reason);
    return;
  }

  // Manifest: read + parse (ArduinoJson) into the engine's struct.
  char man_json[FAT_MAX_MANIFEST_BYTES + 1] = {0};
  if (fat16_read_file(s_staging, man, (uint8_t*)man_json, FAT_MAX_MANIFEST_BYTES) != man.size) {
    fat16_write_result(s_staging, "could not read the manifest back - copy both files again");
    return;
  }
  JsonDocument doc;
  if (deserializeJson(doc, man_json, man.size) != DeserializationError::Ok) {
    fat16_write_result(s_staging, "the manifest is not valid JSON - re-download it from the release");
    return;
  }
  securacv_ota_manifest_t m;
  memset(&m, 0, sizeof m);
  snprintf(m.product, sizeof m.product, "%s", (const char*)(doc["product"] | ""));
  snprintf(m.version, sizeof m.version, "%s", (const char*)(doc["version"] | ""));
  snprintf(m.min_version, sizeof m.min_version, "%s", (const char*)(doc["min_version"] | ""));
  snprintf(m.url, sizeof m.url, "%s", (const char*)(doc["url"] | ""));
  snprintf(m.sha256, sizeof m.sha256, "%s", (const char*)(doc["sha256"] | ""));
  m.size = doc["size"] | 0u;
  snprintf(m.signature, sizeof m.signature, "%s", (const char*)(doc["signature"] | ""));
  snprintf(m.manifest_signature, sizeof m.manifest_signature, "%s",
           (const char*)(doc["manifest_signature"] | ""));
  snprintf(m.signing_key_id, sizeof m.signing_key_id, "%s",
           (const char*)(doc["signing_key_id"] | ""));
  snprintf(m.release_notes, sizeof m.release_notes, "%s",
           (const char*)(doc["release_notes"] | ""));
  snprintf(m.release_url, sizeof m.release_url, "%s",
           (const char*)(doc["release_url"] | ""));

  // Image: stage into its own PSRAM buffer for hashing + writing.
  uint8_t* image = (uint8_t*)ps_malloc(bin.size);
  if (!image) {
    fat16_write_result(s_staging, "out of memory staging the image");
    return;
  }
  if (fat16_read_file(s_staging, bin, image, bin.size) != bin.size) {
    free(image);
    fat16_write_result(s_staging, "could not read the image back - copy both files again");
    return;
  }

  // The exact network-OTA acceptance pipeline (host-tested end to end).
  char floor_buf[16] = {0};
  {
    Preferences p;
    if (p.begin("securacv_ota", /*readOnly=*/true)) {
      p.getString("min_ver", floor_buf, sizeof floor_buf);
      p.end();
    }
  }
  evidence_update::VerifyDeps deps;
  deps.ed25519_verify = dep_ed25519;
  deps.sha256 = dep_sha256;
  deps.release_pubkey = SECURACV_OTA_RELEASE_PUBKEY;
  deps.product = s_cfg.product;
  deps.running_version = s_cfg.running_version;
  deps.nvs_floor = floor_buf;
  const evidence_update::Verdict v =
      evidence_update::verify(m, image, bin.size, deps);
  fat16_write_result(s_staging, v.msg);
  snprintf(s_status, sizeof s_status, "update drop: %s", v.msg);
  if (!v.ok) { free(image); return; }

  // Write to the inactive slot; pending-verify + boot self-test semantics
  // are the engine's, unchanged.
  const esp_partition_t* part = esp_ota_get_next_update_partition(NULL);
  esp_ota_handle_t handle = 0;
  if (!part || esp_ota_begin(part, bin.size, &handle) != ESP_OK) {
    free(image);
    fat16_write_result(s_staging, "could not open the update slot - reboot and try again");
    return;
  }
  esp_err_t err = esp_ota_write(handle, image, bin.size);
  free(image);
  if (err != ESP_OK || esp_ota_end(handle) != ESP_OK ||
      esp_ota_set_boot_partition(part) != ESP_OK) {
    fat16_write_result(s_staging, "writing the update slot failed - nothing was changed");
    return;
  }
  securacv_ota_mark_pending_install(m.version);
  snprintf(s_status, sizeof s_status, "verified %s - rebooting to install", m.version);
  delay(750);      // let the host read RESULT.TXT's cache flush settle
  ESP.restart();
}

// ── mode transitions ────────────────────────────────────────────────────────
static void msc_present(uint32_t block_count, bool writable) {
  s_msc.vendorID("SecuraCV");
  s_msc.productID(s_mode == Mode::UPDATE ? "CANARY-UPDATE" : "CANARY-EVIDENCE");
  s_msc.productRevision("1.0");
  s_msc.onRead(on_read);
  s_msc.onWrite(on_write);
  s_msc.onStartStop(on_start_stop);
  s_msc.mediaPresent(true);
  s_msc.isWritable(writable);
  s_msc.begin(block_count, FAT_SECTOR_SIZE);
  if (!s_usb_started) { USB.begin(); s_usb_started = true; }
}

static void enter_off() {
  s_msc.mediaPresent(false);
  if (s_mode == Mode::EVIDENCE) {
    share_apply(s_share, ShareEvent::SHARE_RELEASE, true);
    if (s_cfg.sd_resume) s_cfg.sd_resume();
  }
  s_mode = Mode::OFF;
  snprintf(s_status, sizeof s_status, "USB drive off");
}

static void enter_evidence() {
  if (!s_cfg.sd_quiesce || !s_cfg.sd_quiesce()) {
    snprintf(s_status, sizeof s_status, "SD not ready - evidence drive unavailable");
    return;
  }
  share_apply(s_share, ShareEvent::SHARE_REQUEST, true);
  s_mode = Mode::EVIDENCE;
  msc_present((uint32_t)SD.numSectors(), /*writable=*/false);
  snprintf(s_status, sizeof s_status,
           "EVIDENCE drive shared read-only - eject on the computer when done");
}

static void enter_update() {
  fat16_format(s_staging); // always a fresh, empty drop-zone
  s_mode = Mode::UPDATE;
  msc_present(FAT_TOTAL_SECTORS, /*writable=*/true);
  snprintf(s_status, sizeof s_status,
           "UPDATE drop-zone shared - copy the release .bin + manifest, then eject");
}

// ── public API ──────────────────────────────────────────────────────────────
bool begin(const Config& cfg) {
  s_cfg = cfg;
  if (!s_staging) s_staging = (uint8_t*)ps_malloc(FAT_VOLUME_BYTES);
  if (!s_staging) {
    snprintf(s_status, sizeof s_status, "no PSRAM for the update drop-zone");
    return false;
  }
  return true;
}

void poll() {
  if (s_pending_eject) {
    s_pending_eject = false;
    if (s_mode == Mode::UPDATE) run_update_check();
    else if (s_mode == Mode::EVIDENCE) {
      share_apply(s_share, ShareEvent::HOST_EJECT, true);
      if (s_cfg.sd_resume) s_cfg.sd_resume();
      s_mode = Mode::OFF;
      s_msc.mediaPresent(false);
      snprintf(s_status, sizeof s_status, "evidence drive ejected - SD back with the firmware");
    }
  }
  if (s_pending_unplug) {
    s_pending_unplug = false;
    if (s_mode != Mode::OFF) enter_off();
  }
}

void cycle_mode() {
  switch (s_mode) {
    case Mode::OFF:      enter_evidence(); break;
    case Mode::EVIDENCE: enter_off(); enter_update(); break;
    case Mode::UPDATE:   enter_off(); break;
  }
}

Mode mode() { return s_mode; }
const char* status_line() { return s_status; }

} // namespace usb_evidence_drive

#else // ── stub build: flag off or no TinyUSB — honest no-ops ────────────────

namespace usb_evidence_drive {
bool begin(const Config&) { return false; }
void poll() {}
void cycle_mode() {}
Mode mode() { return Mode::OFF; }
const char* status_line() {
  return "USB drive mode is not in this build (needs FEATURE_USB_EVIDENCE_DRIVE + USB-OTG)";
}
} // namespace usb_evidence_drive

#endif
