# Fleet Link — hardware bench smoke-test checklist

> The **direct BLE fleet link** (`FEATURE_FLEET_LINK`, presence+status beacon +
> on-demand GATT pull — see [`display_discovery_and_resilience.md`](./display_discovery_and_resilience.md)
> §3.1) is implemented, host-tested, and compiles in CI, but its **on-air radio
> behavior has not been validated on hardware**. This checklist is the gate to
> clear before it ships in a signed release. Until every ⬜ below is ✅ on real
> boards, treat the feature as experimental (`FEATURE_FLEET_LINK=0` ships
> presence-only).

## What you need

| Qty | Item | Role |
|-----|------|------|
| 1+ | XIAO ESP32-S3 running **canary-wap** (`FULL` profile, PSRAM=opi) | the beacon/GATT server |
| 1 | XIAO ESP32-S3 running **canary-display** (`watch` or `dash`, `FEATURE_CHIRP_SCAN=1`, `FEATURE_FLEET_LINK=1`) | the scanner/central |
| — | A phone with **nRF Connect** (or LightBlue) | ground-truth BLE sniffer |
| — | Serial consoles on both (115200) | watch the logs |
| — | A way to **turn off home WiFi** (or a WAP that never joins one) | the whole point |

Flash both from the browser flasher or `arduino-cli`/PlatformIO. Note each
device's `SCV-XXXX` name (last 4 hex of its fingerprint) from the boot serial —
that suffix is the correlator used everywhere below.

## A. Beacon is actually on air (the passive layer)

- ⬜ **A1 — advert exists.** With nRF Connect, the WAP appears advertising
  **manufacturer data, company `0xFFFF`, 11 bytes**, first payload byte `0x10`
  (beacon type), second `0x01` (schema). This is the single most important
  check — the advert restructure (beacon in the primary advert, `SCV_SERVICE_UUID`
  + name moved to the **scan response**) is the part most likely to misbehave.
- ⬜ **A2 — scan response intact.** In nRF Connect the WAP's **name `SCV-XXXX`**
  and the 128-bit `SCV_SERVICE_UUID` (`a1b2c3d4-…-456001`) still show up (they
  now live in the scan response). Confirms backward-compat: other WAPs
  (active scanners) and GATT clients can still find it.
- ⬜ **A3 — payload decodes.** The beacon bytes match the wire contract
  (`fleet_beacon.h` / `beacon_parse.h`): flags, battery %, health %, chain-height
  low-16 (LE), and the last 2 fingerprint bytes = the device's `SCV-XXXX` suffix.
- ⬜ **A4 — it's continuous, not a 2 s blip.** The beacon is present on
  essentially every scan (advert interval ~500 ms), unlike the old 2 s-per-5 min
  chirp. A chirp event (alert/tamper) briefly overrides it, then it **restores**
  to the beacon (verify by triggering a chirp and confirming the beacon comes
  back within ~2–3 s — this exercises `ble_chirp`'s restore path).
- ⬜ **A5 — no fallback banner.** WAP serial does **not** print
  `Opera beacon adv setup failed — legacy UUID advert active`. If it does, the
  `NimBLEAdvertisementData` path failed and A1 will be a plain UUID advert.

## B. Display sees it — no broker, no WiFi (the core promise)

- ⬜ **B1 — turn WiFi OFF** (or never provision it). No MQTT broker anywhere on
  the bench.
- ⬜ **B2 — display lists the Canary.** Within one scan window the display's
  fleet view shows the WAP by its `SCV-XXXX` name, `seen_via_ble`, link Online.
  Display serial (`CHIRP`) shows the listener up and beacons parsed
  (`chirp_scan_count()` climbing).
- ⬜ **B3 — status populates.** The card shows battery / health / chain height
  from the beacon (not just "present"). Values track the WAP's real state.
- ⬜ **B4 — tamper punches through.** Trigger a tamper on the WAP; the display
  raises a `tamper (ble)` event at Tamper severity, and the 60 s edge-dedupe
  means a repeat within a minute does **not** spam the log.
- ⬜ **B5 — never "Verified".** The BLE-fed card must **not** show the Verified
  trust badge — this channel is unsigned by design. Badge stays Unknown/Signed
  as appropriate, never green-Verified from BLE alone.
- ⬜ **B6 — continuous-scan mode.** With WiFi down the display scans
  continuously (not 4 s/20 s bursts). Discovery latency should be seconds, not
  minutes. Confirm it still respects the heap gate (no `BLE stack init failed`
  boot-loop — see C1).

## C. Stability / the known-risk items

- ⬜ **C1 — no boot-loop, no heap panic.** Neither board reboots on BLE bring-up.
  Watch for the historical `BLE_INIT: Malloc failed` / `assert emi.c` panic — the
  heap gate (`ble_gate.h`, 48 KB block / 96 KB free) should skip-and-retry, not
  crash. Let both run 30+ min.
- ⬜ **C2 — WiFi + BLE coexistence.** Turn WiFi back **on** (broker present):
  the display should prefer MQTT and the direct BLE listener should back off
  (bursts, not continuous) — MQTT data and BLE presence coexist without the
  dashboard timing out.

## D. GATT pull — the on-demand rich detail (the highest-risk path)

- ⬜ **D1 — tap connects.** With WiFi off, tap a Canary on the display; it opens
  a NimBLE **central** connection to the WAP's status service (`5e63a1b0-…`),
  reads fw / chain seq / health / degrade / SD% / mic-muted / battery, then
  disconnects. Display serial (`FLEETLINK`) shows the pull; `fleet_link_count()`
  increments.
- ⬜ **D2 — ⚠️ ADDRESS TYPE (the flagged bug).** This is the one most likely to
  fail. ESP32 peripherals often advertise a **random** static address. The code
  now carries the `NimBLEAddress` (with its type) from scan → connect instead of
  rebuilding it as `BLE_ADDR_PUBLIC`. **Verify the connect actually succeeds**
  against a WAP using a random address (check the WAP's address type in nRF
  Connect). If D1 fails with a connect timeout, this is the first suspect.
- ⬜ **D3 — TOFU pin.** Reconnect to the same Canary: it binds/reuses the
  address↔fingerprint pin. Then (optional) spoof a second device advertising the
  same `SCV-XXXX` fp from a different address — the display must log
  `ble identity mismatch` and **refuse** its status.
- ⬜ **D4 — radio hand-off.** After the GATT pull disconnects, the passive
  beacon listener resumes (B2 still works). No stuck scan, no lingering
  connection, no heap leak over repeated tap→read→disconnect cycles.
- ⬜ **D5 — dual core lines.** Repeat D1 on both display library lines the CI
  builds: **core 2.0.17 / NimBLE 1.4.x** and **core 3.x / NimBLE 2.x** (the
  `getDevice`-by-value vs by-pointer and client-callback differences live here).

## E. Fleet scale (if you have ≥2 WAPs)

- ⬜ **E1 — multiple Canaries.** Two+ WAPs beaconing at once all appear on the
  display, each correctly keyed by its own `SCV-XXXX` suffix (no cross-talk /
  merged witnesses).
- ⬜ **E2 — RSSI/nearest sane.** If opportunistic nearest-RSSI GATT is enabled,
  it targets the closest WAP, not a random one.

## Sign-off

When A–D are all ✅ on both core lines, flip `FEATURE_FLEET_LINK` on by default
with confidence and drop the "hardware-validation pending" caveat from
`display_discovery_and_resilience.md` §3.1/§4 and this repo's release notes.
Record the boards, core/NimBLE versions, and firmware `@ <sha>` you validated
against here:

| Date | Boards | core / NimBLE | fw sha | Result | By |
|------|--------|---------------|--------|--------|----|
|      |        |               |        |        |    |
