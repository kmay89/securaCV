# Power-event resilience + the outage log

Harden the Canaries against **brownouts, power flickers, and outages**, and
keep an honest, correctly-named log of *when the power went out*. This is the
design and the integration recipe; the decision logic is a pure, host-tested
core so the behavior is proven, not reviewed.

- **Core (this wave, host-tested):**
  [`firmware/common/power/power_events.h`](../../firmware/common/power/power_events.h)
  ← [`firmware/tests_host/test_power_events.cpp`](../../firmware/tests_host/test_power_events.cpp)
- **Boot-path wiring (lands separately, hardware-validated):** per firmware —
  see the recipe below. This split is the same one
  [`boot_policy.h`](../../firmware/common/health/boot_policy.h) uses: the pure
  decision core lands first with CI proof; the glue that feeds it real signals
  lands and is validated on hardware.

## The honest premise

A device that just lost power **cannot record the instant it died** — it is
unpowered. Any log claiming an exact "power lost at HH:MM:SS" is guessing. So we
do the one thing a device truthfully can:

1. While running, it persists a **liveness heartbeat** — the latest wall-clock
   time it was known alive (from an RTC or SNTP; a monotonic boot index when
   there is no clock).
2. On the **next boot**, it classifies how the *previous* session ended from the
   reset cause and a couple of cheap flags, and appends one correctly-named
   event: *"power restored; last seen alive at T → outage ≥ (now − T)"*. The
   duration is an explicit **lower bound**, never a fabricated instant.

## The vocabulary (said the correct way)

| Term | What it means | Is it a "power went out" event? |
|---|---|---|
| **Cold boot** | First-ever power-up; nothing prior to compare to. | No |
| **Clean reboot** | The device stopped *on purpose* — OTA update, user reset, intended power-off, deep-sleep wake. | No |
| **Brownout reset** | The supply **sagged** below the chip's brownout detector and reset it. A dip, not necessarily a full blackout. | Yes (a dip) |
| **Power restored (outage)** | A power-on reset whose predecessor did **not** stop cleanly: mains was lost while running and has returned. | Yes (a blackout) |
| **Voltage sag / flicker (rode through)** | A dip the device survived without resetting — on a battery-backed board it ran on the cell. Logged as a mains-present transition, not a reset. | Yes (survived) |
| **Fault reset** | A watchdog or panic reset — a crash, surfaced in the log but counted on its own axis, **not** as a power loss. | No |

The distinction between a **brownout** (supply dipped) and an **outage** (mains
gone) is the point of "said the correct way": they are different events with
different causes, and the log names each for what it is.

## The classification (the pure core)

`powerevents::classify(Signals)` is a pure function of signals the glue
collects. The full table is pinned by the host test:

| Reset cause | clean-shutdown flag | RTC marker | → lineage |
|---|---|---|---|
| *(no prior session)* | — | — | **Cold boot** |
| Brownout | any | any | **Brownout reset** |
| Fault (WDT/panic) | any | any | **Fault reset** |
| Deep-sleep wake | any | any | **Clean reboot** |
| Power-on | set | — | **Clean reboot** (user powered off, then on) |
| Power-on | clear | — | **Power restored (outage)** |
| Software (esp_restart) | any | — | **Clean reboot** (OTA / user reboot / config apply — always intentional) |
| Unknown | set | — | **Clean reboot** |
| Unknown | clear | present | **Unknown** (not asserted) |
| Unknown | clear | absent | **Power restored (outage)** (corroborated) |

A Software reset is *always* our own firmware calling `esp_restart()`; a real
power loss can only surface as a power-on or brownout reset, never as Software.
So an OTA update or a user reboot is never mislabeled an outage — the reset
cause alone settles it, no clean-shutdown flag required.

The signals:

- **reset** — `esp_reset_reason()` mapped onto `powerevents::ResetKind`.
- **clean_shutdown** — an NVS flag the running device *clears at start* and
  *sets only before an intended stop* (graceful shutdown / reboot). Still set on
  the next boot ⇒ the last stop was deliberate.
- **rtc_marker_present** — a magic value kept in **RTC-domain `RTC_NOINIT`
  memory**. It survives a soft/brownout reset but **not** a true power loss, so
  its absence corroborates a real outage. A secondary hint, used only to
  disambiguate an unflagged software reset — never to override an explicit
  reset cause.
- **have_prior_session** — `boot_count > 0` (or any persisted heartbeat).

## The log

`powerevents::Log` is a trivially-copyable POD persisted as raw NVS bytes (like
`power_history_t`): a fixed ring of the last `kRingCap` (16) events plus
**monotonic aggregate counters** that outlive the ring —
`total_outages`, `total_brownouts`, `total_faults`, `longest_outage_s`,
`last_incident_epoch`. `log_valid()` rejects an uninitialized or foreign blob so
a first read on a fresh device falls back to `log_init()`.

## Hardening, alongside the log

- **Detect + name brownouts** (already counted today in
  `securacv_power`/`power_monitor` as `brownout_count`; this core folds that into
  the same event stream with the correct label).
- **Enable the brownout detector on every image.** It is on in the production
  hardening overlay —
  [`sdkconfig.defaults.secure`](../../firmware/provisioning/sdkconfig.defaults.secure)
  (`CONFIG_ESP_BROWNOUT_DET=y`, `LVL_SEL_7`). Promote it into each build's base
  sdkconfig so dev/release images share the same floor. *(Note: for
  Arduino-framework PlatformIO builds this is a precompiled-IDF setting; verify
  the level actually takes on the target before relying on it — hence it is
  called out here rather than silently flipped.)*
- **Clean-shutdown discipline** — clear the flag at the start of every normal
  run; set it only in `power_graceful_shutdown()` and just before an intended
  `esp_restart()` (OTA, user reboot). This is what lets an outage be told apart
  from a deliberate power-cycle.
- **Eager persistence** — commit the heartbeat and critical state to NVS
  promptly (the witness store already tolerates a torn final line from a
  power-cut; the heartbeat should be flushed on a cadence, e.g. 30–60 s, not
  buffered).
- **Flicker ride-through** — on battery-backed boards (the XIAO's LiPo, the
  4.3C's CS8501 cell) a flicker is survived on the cell; log the mains-present
  transition via the existing power source detection rather than a reset.

## Emitting it as a *witnessed* event (tamper-evident)

A power incident can be signed and hash-chained like any other witness record,
so "the power went out at T" is verifiable, not just a local note. Reuse the
existing kernel (`firmware/canary/lib/securacv_witness`):

- Lowest-friction: a `RECORD_STATE_CHANGE` with a CBOR payload
  `{"type": boot_power_wire(k), "outage_s": …, "boot": …}` — the exact shape the
  power-mode-change hook and `RECORD_POWER_SHUTDOWN` already use.
- Or add a dedicated `RECORD_POWER_EVENT` to `RecordType` (a wire-format change —
  bump the schema/catalog if you take this path).

`boot_power_wire()` gives the stable token (`cold_boot`, `clean_reboot`,
`power_restored`, `brownout`, `fault_reset`).

## Per-firmware integration recipe (the boot-path glue)

Each firmware adopts the shared core the same way; the header is already on
every PlatformIO tree's include path (`-I firmware/common`), and the Arduino
sketch trees stage a byte-identical copy behind a sync check (the contract
`power_logic.h` documents).

Early in `setup()`, before risky init:

1. `powerevents::ResetKind rk = map_reset(esp_reset_reason());`
2. Load from NVS (`"securacv"` namespace): the `Log` blob, the `clean_shutdown`
   flag, `last_alive_epoch`, and `boot_count`.
3. Read the `RTC_NOINIT` marker (present ⇒ power held) and re-seed it.
4. `auto k = powerevents::classify({rk, clean_flag, marker_present, boot_count>0});`
5. `uint32_t outage = powerevents::outage_bound_s(last_alive_epoch, now_epoch);`
6. `powerevents::log_note(L, powerevents::make_event(k, now_epoch, boot_count, outage));`
   then persist `L`.
7. If `is_power_incident(k)`, emit the witnessed event and print a console line
   (`boot_power_name(k)`).
8. **Clear** the `clean_shutdown` flag for the run now beginning.

In the main loop: persist the liveness heartbeat (`last_alive_epoch = now`) on a
cadence (5 min is a good wear/resolution trade-off), and only when the wall
clock is actually set — so a board with no clock never writes a meaningless
heartbeat. **Set** the `clean_shutdown` flag only in `power_graceful_shutdown()`
(a deliberate power-off); a normal reboot needs nothing, because `esp_restart()`
already surfaces as a Software reset the classifier treats as clean.

Surface it per device: a console/diagnostic line and telemetry everywhere; the
**on-glass "power" line** on the display boards (the 4.3B/4.3C, which have the
PCF85063 RTC for real wall-clock times — the ideal home for a human-readable
outage history, and the UX the 4.3C README scopes as *"cut the power, the Canary
keeps witnessing"*).

## Status

- **Done:** the pure decision core + outage-log ring, host-tested in CI
  (`test_power_events.cpp`, 66 checks, `-Wall -Wextra -Werror`).
- **Done — canary base reference wiring:**
  [`firmware/canary/include/canary_power_events.h`](../../firmware/canary/include/canary_power_events.h)
  is the boot-path glue (the exact recipe above), wired into
  `firmware/canary/src/main.cpp` at three sites — `on_boot()` (classify + log +
  console), `witness_incident()` (sign a restored-outage/brownout record), and
  `heartbeat()` (the loop liveness persist). It is the copy-me reference for the
  other trees. *(Compiled on CI/hardware; the pure core it calls is the
  host-tested part.)*
- **Done — Home Assistant egress:** the base tree also publishes the boot
  classification on `securacv/<id>/tamper` as the payload the integration's
  per-type sensors parse — `{"type":"power_loss"}` for a restored outage or
  brownout (with the honest lower-bound duration in `detail` when one is
  known), `{"type":"unexpected_reboot"}` for a fault reset, nothing for a
  benign boot. The JSON builder is `powerevents::ha_tamper_json()` in the pure
  core, host-tested exactly; the publish is non-retained on purpose (a
  retained copy would re-trigger HA's edge-latching sensors on every HA
  restart). This closes the "`power_loss` — none found" row in
  [`homeassistant_setup.md`](../homeassistant_setup.md) for the base tree.
- **Done — canary-display wiring:**
  [`power_events_glue.h`](../../firmware/projects/canary-display/include/canary/power_events_glue.h)
  applies the recipe to the display tree — `on_boot()` and the loop heartbeat
  both sit **above** the mode latch so a bench/demo boot still records its
  lineage, and the health publish carries the held
  `power_loss_detected`/`unexpected_reboot` flags. Two honest differences
  from the base tree: no witness record (this tree carries no signing
  kernel — verify-only by design), and `pe_clean` rides `putUChar` (the
  display's Preferences shim documents that the bool accessors may be
  absent). The 4.3C's RTC stays off pending its I2C census, so outage
  durations there read unknown until `FEATURE_RTC` is hardware-validated —
  the classifier itself needs no clock. *(Compiled on CI; the on-glass
  "power" line is the hardware-validated follow-up.)*
- **Next (per firmware, hardware-validated):** wap / vision / sense /
  sentinel, same recipe; and the 4.3C's RTC + on-glass outage history once
  the I2C census confirms the PCF85063. Each is a small, isolated change
  against the same proven core; the base helper is the template.
