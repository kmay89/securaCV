/*
 * SecuraCV Canary — SD Storage Manager Implementation
 *
 * Mount recovery mirrors the canary-wap hardware_state machinery (where the
 * lessons were paid for on hardware): SD.begin() is a chain of yield-free
 * CPU spin loops in the SPI SD driver with no overall deadline, so ALL
 * blocking mount work runs on a dedicated worker task at idle priority and
 * the loop task only polls a state byte —
 *   IDLE → REQUESTED (loop posts) → RUNNING (worker) → DONE (worker; result
 *   written first) → IDLE (loop adopts).
 * Nothing ever cancels a running mount (no SD.end() under the worker — that
 * would free driver state out from under it); a result that outlives the
 * boot wait budget is adopted by a later periodicCheck() pass instead of
 * being discarded. The branchy decisions live in the pure, host-tested
 * common/storage/sd_mount_policy.h.
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#include "securacv_storage.h"
#include "securacv_witness.h"
#include "storage/sd_mount_policy.h"

#if FEATURE_WATCHDOG
#include <esp_task_wdt.h>
#endif

#if FEATURE_SD_STORAGE

// ════════════════════════════════════════════════════════════════════════════
// MOUNT WORKER — bounded, watchdog-safe mounting
// ════════════════════════════════════════════════════════════════════════════

namespace {

// Boot wait budget for the mount worker. Must sit well under the 8 s task
// watchdog (WATCHDOG_TIMEOUT_SEC) the loop task is subscribed to; the wait
// loop also feeds the watchdog every slice. Matches canary-wap's
// SD_MOUNT_TIMEOUT_MS.
constexpr uint32_t kMountWaitMs = 4000;
constexpr uint32_t kMountWaitSliceMs = 25;

// Re-probe cadence for an absent card / presence check for a mounted one.
// Matches canary-wap's SD_RECHECK_INTERVAL_MS.
constexpr uint32_t kRecheckIntervalMs = 30000;

// Consecutive write failures before the card is marked lost and handed to
// the remount path. Matches canary-wap's SD_MAX_RETRIES.
constexpr uint32_t kConsecutiveErrorLimit = 2;

enum : uint8_t { MW_IDLE = 0, MW_REQUESTED = 1, MW_RUNNING = 2, MW_DONE = 3 };

TaskHandle_t s_mount_worker = nullptr;
volatile uint8_t s_mount_state = MW_IDLE;
bool s_mount_result_ok = false;  // written by the worker BEFORE the DONE store
SPIClass* s_worker_spi = nullptr;

inline uint8_t mount_state() {
  return __atomic_load_n(&s_mount_state, __ATOMIC_ACQUIRE);
}

void mount_worker_task(void*) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if (mount_state() != MW_REQUESTED) continue;
    __atomic_store_n(&s_mount_state, MW_RUNNING, __ATOMIC_RELEASE);
    bool ok = SD.begin(SD_CS_PIN, *s_worker_spi, SD_SPI_FAST);
    if (!ok) ok = SD.begin(SD_CS_PIN, *s_worker_spi, SD_SPI_SLOW);
    s_mount_result_ok = ok;  // written BEFORE the release-store below
    __atomic_store_n(&s_mount_state, MW_DONE, __ATOMIC_RELEASE);
  }
}

}  // namespace

// ════════════════════════════════════════════════════════════════════════════
// GLOBAL INSTANCE
// ════════════════════════════════════════════════════════════════════════════

static StorageManager s_storage;
static SPIClass s_sd_spi(FSPI);

StorageManager& storage_get_instance() {
  return s_storage;
}

// ════════════════════════════════════════════════════════════════════════════
// STORAGE MANAGER IMPLEMENTATION
// ════════════════════════════════════════════════════════════════════════════

StorageManager::StorageManager()
  : m_spi(nullptr), m_mounted(false), m_needs_teardown(false),
    m_mount_generation(0), m_consecutive_errors(0), m_last_check_ms(0),
    m_write_errors(0), m_read_errors(0), m_last_write_ms(0) {}

bool StorageManager::mountInFlight() const {
  const uint8_t st = mount_state();
  return st == MW_REQUESTED || st == MW_RUNNING;
}

bool StorageManager::requestMount() {
  if (mount_state() != MW_IDLE) return false;
  if (!s_mount_worker) {
    // Idle priority on purpose: both cores' IDLE tasks feed the panic
    // watchdog and the SD driver's waits never yield — at any higher
    // priority a stuck mount would starve an IDLE task and move the panic
    // there. At priority 0 the tick time-slicer keeps the IDLE tasks (and
    // their watchdog feeds) running.
    if (xTaskCreate(mount_worker_task, "sd_mount", 4096, nullptr,
                    tskIDLE_PRIORITY, &s_mount_worker) != pdPASS) {
      s_mount_worker = nullptr;
      return false;
    }
  }
  s_worker_spi = m_spi;
  __atomic_store_n(&s_mount_state, MW_REQUESTED, __ATOMIC_RELEASE);
  xTaskNotifyGive(s_mount_worker);
  return true;
}

bool StorageManager::tryAdoptMount() {
  if (mount_state() != MW_DONE) return false;
  const bool ok = s_mount_result_ok;
  __atomic_store_n(&s_mount_state, MW_IDLE, __ATOMIC_RELEASE);
  if (ok) {
    m_mounted = true;
    m_needs_teardown = false;
    m_mount_generation++;
    m_consecutive_errors = 0;
    ensureDirectories();
    Serial.println("[SD] Card mounted");
  } else {
    m_mounted = false;
  }
  // sd_healthy drives the HA binary sensor and the status APIs; keep it
  // tracking the live mount state through every transition, not just boot.
  witness_get_health().sd_healthy = m_mounted;
  return ok;
}

bool StorageManager::begin(SPIClass* spi) {
  if (spi) {
    m_spi = spi;
  } else {
    m_spi = &s_sd_spi;
    m_spi->begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
  }

  if (!requestMount()) return m_mounted;

  // Bounded wait: poll the worker in short slices, feeding the task
  // watchdog each slice. Past the budget, report the card absent and keep
  // booting — the worker keeps running until the blocking call returns and
  // the result lands via periodicCheck()'s adopt-first pass.
  const uint32_t started = millis();
  while (!sd_mount_policy::mount_wait_expired(millis(), started, kMountWaitMs)) {
    if (mount_state() == MW_DONE) return tryAdoptMount();
    delay(kMountWaitSliceMs);
#if FEATURE_WATCHDOG
    esp_task_wdt_reset();
#endif
  }
  Serial.println("[SD] still probing - continuing boot; result adopted when ready");
  return false;
}

void StorageManager::end() {
  // Never free driver state under a running mount attempt (use-after-free
  // in the worker). Callers of this path can retry after it concludes.
  if (mountInFlight()) {
    Serial.println("[SD] unmount deferred: mount attempt in flight");
    return;
  }
  SD.end();
  m_mounted = false;
  m_needs_teardown = false;
  witness_get_health().sd_healthy = false;
}

void StorageManager::periodicCheck(bool msc_holds_card) {
  // Adopt any completed background mount first — one atomic read when
  // nothing is pending, and running it every pass means a result that
  // outlived a caller's wait budget (boot or a previous tick) lands
  // promptly instead of waiting out the recheck interval.
  tryAdoptMount();

  // A card marked lost by noteWriteFailure() still owes an SD.end() so the
  // next mount starts from clean driver state. Policy-gated: never under a
  // running mount, never while USB MSC holds the card.
  if (m_needs_teardown &&
      sd_mount_policy::may_teardown(mountInFlight(), msc_holds_card)) {
    SD.end();
    m_needs_teardown = false;
  }

  const uint32_t now = millis();
  const sd_mount_policy::PeriodicAction action = sd_mount_policy::periodic_action(
      m_mounted, mountInFlight(), msc_holds_card,
      now, m_last_check_ms, kRecheckIntervalMs);
  if (action == sd_mount_policy::PeriodicAction::NONE) return;
  m_last_check_ms = now;

  if (action == sd_mount_policy::PeriodicAction::VERIFY) {
    // Cheap presence check: stat the root. Fails fast if the card left.
    File root = SD.open("/");
    if (root) {
      root.close();
      return;
    }
    m_read_errors++;
    m_mounted = false;
    witness_get_health().sd_healthy = false;
    Serial.println("[SD] Card removed or failed - will retry");
    if (sd_mount_policy::may_teardown(mountInFlight(), msc_holds_card)) {
      SD.end();
      m_needs_teardown = false;
    } else {
      m_needs_teardown = true;  // MSC holds the card; teardown deferred
    }
  } else {  // REMOUNT — card may have been (re-)inserted
    // Fire-and-forget: the worker does the blocking work; the result is
    // adopted by the try-adopt pass above on a later loop() pass. The loop
    // task never blocks here, so a wedged card can't starve the watchdog.
    if (m_needs_teardown) return;  // teardown still owed (MSC held the card)
    Serial.println("[SD] Periodic check - background remount attempt...");
    requestMount();
  }
}

void StorageManager::noteWriteFailure() {
  m_write_errors++;
  m_consecutive_errors++;
  if (m_mounted && !mountInFlight() &&
      sd_mount_policy::declare_lost(m_consecutive_errors,
                                    kConsecutiveErrorLimit)) {
    // Stop hammering a dead card now; SD.end() and the remount run on the
    // next periodicCheck() pass, where the MSC gate is known.
    m_mounted = false;
    m_needs_teardown = true;
    witness_get_health().sd_healthy = false;
    Serial.println("[SD] Consecutive write failures - card marked lost, remount will retry");
  }
}

void StorageManager::noteWriteSuccess() {
  m_last_write_ms = millis();
  m_consecutive_errors = 0;
}

bool StorageManager::ensureDirectories() {
  if (!m_mounted) return false;

  if (!SD.exists("/WITNESS")) SD.mkdir("/WITNESS");
  if (!SD.exists("/HEALTH")) SD.mkdir("/HEALTH");
  if (!SD.exists("/CHAIN")) SD.mkdir("/CHAIN");
  if (!SD.exists("/EXPORT")) SD.mkdir("/EXPORT");

  return true;
}

SDStatus StorageManager::getStatus() {
  SDStatus status;
  memset(&status, 0, sizeof(status));

  status.mounted = m_mounted;
  status.healthy = m_mounted;
  status.write_errors = m_write_errors;
  status.read_errors = m_read_errors;
  status.last_write_ms = m_last_write_ms;

  if (m_mounted) {
    status.total_bytes = SD.totalBytes();
    status.used_bytes = SD.usedBytes();
    status.free_bytes = status.total_bytes - status.used_bytes;
    status.witness_count = countFilesInDir("/WITNESS");
    status.health_count  = countFilesInDir("/HEALTH");
  }

  // Unacked count is owned by the witness/health-log subsystem; mirror it
  // here so the storage status API is a single source of truth for dashboards.
  status.unacked_count = witness_get_health().logs_unacked;

  return status;
}

uint32_t StorageManager::countFilesInDir(const char* dir_path) {
  if (!m_mounted || !dir_path) return 0;

  File dir = SD.open(dir_path);
  if (!dir) return 0;
  if (!dir.isDirectory()) {
    dir.close();
    return 0;
  }

  uint32_t count = 0;
  for (File entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
    if (!entry.isDirectory()) {
      count++;
    }
    entry.close();
  }
  dir.close();
  return count;
}

bool StorageManager::fileExists(const char* path) {
  if (!m_mounted) return false;
  return SD.exists(path);
}

size_t StorageManager::fileSize(const char* path) {
  if (!m_mounted) return 0;
  File f = SD.open(path, FILE_READ);
  if (!f) return 0;
  size_t sz = f.size();
  f.close();
  return sz;
}

// ════════════════════════════════════════════════════════════════════════════
// CONVENIENCE FUNCTIONS
// ════════════════════════════════════════════════════════════════════════════

bool storage_init(SPIClass* spi) {
  return storage_get_instance().begin(spi);
}

bool storage_is_mounted() {
  return storage_get_instance().isMounted();
}

void storage_periodic_check(bool msc_holds_card) {
  storage_get_instance().periodicCheck(msc_holds_card);
}

void storage_note_write_failure() {
  storage_get_instance().noteWriteFailure();
}

void storage_note_write_success() {
  storage_get_instance().noteWriteSuccess();
}

bool storage_mount_in_flight() {
  return storage_get_instance().mountInFlight();
}

uint32_t storage_mount_generation() {
  return storage_get_instance().mountGeneration();
}

#endif // FEATURE_SD_STORAGE
