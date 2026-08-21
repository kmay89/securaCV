// SD deep-archive tier for the time-machine journal — contract, flavor truth,
// and the bench gate are documented in canary/fleet/sd_archive.h.
//
// Bus choice, stated once: the dash TF slot's data lines are native GPIOs but
// its CS/DAT3 lives on the write-only CH422G expander — no native chip-select.
// SPI mode would need a per-transaction CS the SD library cannot delegate, so
// the archive uses the S3's SDMMC host in 1-BIT mode instead: CLK/CMD/D0 are
// GPIO-matrix routed to the slot's pins and no CS is needed at all — a card
// falls back to SD mode when DAT3 rides high, which is the expander latch's
// default state (and re-asserted via the HAL at init, belt and braces). This
// is the vendor's own demo path for this panel family.
//
// Gated like journal_store: FEATURE_SD_STORAGE is off in default builds and
// compile-verified by the dedicated `canary-display-dash-sd` env, so shipping
// images stay byte-identical until the bench flip. The !__EMSCRIPTEN__ clause
// keeps the wasm emulator inert the same way (it compiles src/fleet/*.cpp but
// has no SDMMC peripheral).
#include <config.h>

#include "canary/fleet/sd_archive.h"

#if defined(FEATURE_SD_STORAGE) && FEATURE_SD_STORAGE && !defined(__EMSCRIPTEN__)

#if !defined(CD_FLAVOR_DASH) && !defined(CD_AMOLED_GLASS)
#error "FEATURE_SD_STORAGE on the watch isn't supported yet: the slot shares the panel's SPI pins and the GC9A01 runs on Arduino_GFX's private bus handle (no shared arbiter — see sd_archive.h). Build a dash or AMOLED-2.41 flavor, or leave the flag 0."
#endif

#include <Arduino.h>
#include <ArduinoJson.h>
#include <FS.h>
#include <SD_MMC.h>

#include "pins.h"
#include "canary/hal/display.h"
#include "canary/log.h"

#if !defined(SD_PIN_SCK) || !defined(SD_PIN_MOSI) || !defined(SD_PIN_MISO)
#error "FEATURE_SD_STORAGE=1 but this board's pin map declares no SD data pins (SD_PIN_SCK/MOSI/MISO). Add them to firmware/boards/<board>/pins/pins.h once verified against the vendor schematic (the 7\" panel's slot is still unverified — see its pins.h)."
#endif

namespace canary::fleet {

namespace {

constexpr const char* DIR_PATH = "/SECURACV";
constexpr const char* PATH = "/SECURACV/journal.jsonl";
constexpr const char* ROTATED = "/SECURACV/journal.1.jsonl";

// Deep tier, orders of magnitude beyond CD_JOURNAL_MAX_BYTES: 64 MB holds
// years of 10-minute-bucket records yet never crowds even the smallest card
// people actually own. Rotation keeps at most one prior generation, so the
// archive is bounded at ~128 MB — a card can never silently fill to the brim.
constexpr uint64_t ARCHIVE_MAX_BYTES = 64ULL * 1024 * 1024;

// A failed mount is retried no faster than this. Mount attempts on an empty
// slot cost tens of milliseconds — fine at event cadence, hostile at loop
// cadence, which is why retry rides append (events) rather than loop().
constexpr uint32_t REMOUNT_BACKOFF_MS = 10000;

bool s_mounted = false;
uint32_t s_next_try_ms = 0;      // 0 = may try immediately
bool s_absent_noted = false;     // one calm log line per absence, not a spam

// One record -> one JSONL line. KEPT IN EXACT LOCKSTEP with journal_store.cpp's
// line_of(): the two tiers must stay byte-compatible so a card and the
// internal flash tell one story in one schema. (Write side only — the archive
// is read by laptops, not by the device.)
size_t line_of(const JournalRecord& r, char* out, size_t cap) {
  JsonDocument doc;
  doc["ts"] = r.epoch;
  doc["id"] = r.id;
  doc["nm"] = r.name;
  doc["sv"] = r.sev;
  doc["bd"] = r.badge;
  doc["ev"] = r.ev;
  doc["fp"] = r.fp;
  doc["ln"] = r.chain_len;
  doc["ch"] = r.chain_raw;
  return serializeJson(doc, out, cap);
}

// Rotate PATH -> ROTATED once past the byte budget. Delete-then-rename is
// crash-ordered: the worst a power cut leaves is a missing older generation,
// never a truncated or doubled current file.
void rotate_if_needed(uint64_t projected) {
  if (projected <= ARCHIVE_MAX_BYTES) return;
  SD_MMC.remove(ROTATED);
  SD_MMC.rename(PATH, ROTATED);
}

bool try_mount(uint32_t now_ms) {
  if (s_mounted) return true;
  if (s_next_try_ms != 0 && (int32_t)(now_ms - s_next_try_ms) < 0) return false;
  s_next_try_ms = now_ms + REMOUNT_BACKOFF_MS;

  // DAT3 high = SD mode. The expander's default latch already releases the
  // line; re-assert in case some earlier owner drove it low. A HAL that
  // can't reach the expander is a mount that would fail anyway — bail calmly.
  if (!canary::hal::sd_dat3_release()) {
    if (!s_absent_noted) {
      canary::log_line("SDAR", "expander unreachable - archive unavailable");
      s_absent_noted = true;
    }
    return false;
  }

  // 1-bit SDMMC on matrix-routed pins. Macro names are the pin map's SPI-era
  // names; the electrical mapping is CLK=SCK, CMD=MOSI, D0=MISO. Default
  // (20 MHz) class frequency — this slot's routing is conservative.
  if (!SD_MMC.setPins(SD_PIN_SCK, SD_PIN_MOSI, SD_PIN_MISO)) {
    if (!s_absent_noted) {
      canary::log_line("SDAR", "SDMMC pin routing refused - archive unavailable");
      s_absent_noted = true;
    }
    return false;
  }
  if (!SD_MMC.begin("/sd", /*mode1bit=*/true, /*format_if_mount_failed=*/false,
                    SDMMC_FREQ_DEFAULT)) {
    if (!s_absent_noted) {
      canary::log_line("SDAR", "no card (or unreadable) - insert one and it archives on the next event");
      s_absent_noted = true;
    }
    return false;
  }
  if (SD_MMC.cardType() == CARD_NONE) {
    SD_MMC.end();
    return false;
  }
  if (!SD_MMC.exists(DIR_PATH) && !SD_MMC.mkdir(DIR_PATH)) {
    // Mounted but unwritable (locked or worn card): treat as absent.
    canary::log_line("SDAR", "card mounted but not writable - archive off");
    SD_MMC.end();
    return false;
  }
  s_mounted = true;
  s_absent_noted = false;
  char msg[96];
  snprintf(msg, sizeof(msg), "deep archive up - %llu MB card",
           (unsigned long long)(SD_MMC.cardSize() / (1024ULL * 1024ULL)));
  canary::log_line("SDAR", msg);
  return true;
}

// A write that fails after a good mount usually means the card was pulled.
// Unmount so the next event goes through the lazy re-mount (and its logging)
// instead of erroring forever against a phantom filesystem.
void demote_after_failure(const char* why) {
  char msg[96];
  snprintf(msg, sizeof(msg), "append failed (%s) - card pulled? archive off until re-seen", why);
  canary::log_line("SDAR", msg);
  SD_MMC.end();
  s_mounted = false;
  s_next_try_ms = 0;  // a re-inserted card may mount on the very next event
}

}  // namespace

bool sd_archive_init() { return try_mount(millis()); }

bool sd_archive_mounted() { return s_mounted; }

void sd_archive_append(const JournalRecord& r) {
  if (!try_mount(millis())) return;
  // Same buffer arithmetic as journal_store: 1200 clears worst-case escaped
  // chain_raw + envelope; truncation is skipped, never half-written.
  char buf[1200];
  const size_t n = line_of(r, buf, sizeof(buf));
  if (n == 0 || n >= sizeof(buf) - 1) return;
  File f = SD_MMC.open(PATH, FILE_APPEND);
  if (!f) {
    demote_after_failure("open");
    return;
  }
  const uint64_t projected = (uint64_t)f.size() + n + 1;
  const size_t wrote = f.write(reinterpret_cast<const uint8_t*>(buf), n);
  f.write('\n');
  f.close();
  if (wrote != n) {
    demote_after_failure("short write");
    return;
  }
  rotate_if_needed(projected);
}

void sd_archive_wipe() {
  if (!s_mounted) return;
  SD_MMC.remove(PATH);
  SD_MMC.remove(ROTATED);
}

uint64_t sd_archive_bytes() {
  if (!s_mounted) return 0;
  uint64_t total = 0;
  for (const char* p : {PATH, ROTATED}) {
    File f = SD_MMC.open(p, FILE_READ);
    if (f) {
      total += f.size();
      f.close();
    }
  }
  return total;
}

uint64_t sd_archive_card_bytes() { return s_mounted ? SD_MMC.cardSize() : 0; }

}  // namespace canary::fleet

#else  // FEATURE_SD_STORAGE off (or emulator) — no-op stubs so callers link.

namespace canary::fleet {
bool sd_archive_init() { return false; }
bool sd_archive_mounted() { return false; }
void sd_archive_append(const JournalRecord&) {}
void sd_archive_wipe() {}
uint64_t sd_archive_bytes() { return 0; }
uint64_t sd_archive_card_bytes() { return 0; }
}  // namespace canary::fleet

#endif  // FEATURE_SD_STORAGE
