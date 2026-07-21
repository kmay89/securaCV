# Serial test console — run tests without opening a hole

Design document. Status: **Phase 1 implemented** — the read-only `t` "run all
tests" command + the host-tested policy/BLE-ladder logic ship now; the
demo/mutating tiers are designed and gated behind `FEATURE_TEST_CONSOLE` (off in
every shipping image) for a hardware-validated follow-up.

## Why + the trap

Being able to run tests and demos over the USB serial console is genuinely
useful — self-test, exercise the camera, poke Bluetooth. But a rich command
interface over serial is also an **attack surface**, so it has to be built so
that it can't become a vulnerability.

The reframe that makes it safe: **a serial console means physical access.**
Whoever is at the port is holding the device — and might not be its owner (a
dropped device, an evil-maid moment). So the one rule is:

> Physical access **alone** must never escalate to breaking the device's
> guarantees. No command may leak secret material, forge or disable the witness
> chain or tamper detection, or silently mutate state.

## Three tiers (auditable, host-tested)

Commands are declared with a tier + flags, and one pure function decides what may
run. The whole table is checked by `table_is_safe()`, and the policy by
`command_allowed()` — both in `firmware/common/health/test_console.h`, both
proven in `firmware/tests_host/test_test_console.cpp` so CI enforces the model
rather than trusting review.

| Tier | Where it exists | Rules |
|------|-----------------|-------|
| **Diag** (read-only) | **every build, incl. production** | reads state only; prints public info (public keys OK) but **never** private keys / tokens / Wi-Fi passwords; mutates nothing. |
| **Demo** (benign self-tests) | dev/test only (`FEATURE_TEST_CONSOLE`) | camera PEEK, mic beep, BLE advertise-and-wait; in-memory, auto-revert, suppressed from Home Assistant so a test can't trip a real automation. |
| **Mutate** (changes state) | dev only **and** a physical confirm | pairing, config, factory reset — reuse the announce → press-BOOT confirm gate from USB onboarding. |

The two properties that make this "production-level, not vulnerable":

1. **The powerful suite isn't in the shipped binary.** `FEATURE_TEST_CONSOLE`
   is off in `dev`/`release`/… ; production images carry only Tier-Diag. That's
   a security win *and* a flash win — the exact gating the firmware already uses.
2. **Invariants are enforced, not hoped for.** `table_is_safe()` fails if any
   command leaks a secret, if a Diag command mutates, or if a mutating command
   lacks a confirm. `command_allowed(cmd, production, confirmed)` gates every
   dispatch. Host-tested.

## Phase 1: the `t` command (read-only, always available)

`t` runs `diag_run_selftest()` (the 10-probe boot self-test) and prints a
read-only health table: self-test score, SD, GPS, battery, MQTT, and the
**Bluetooth ladder** (below). It's Tier-Diag — safe on any build, mutates
nothing, leaks nothing.

## Bluetooth: say *which* rung failed

"Bluetooth doesn't work" is several different failures. `ble_stage()` collapses a
few observations into the first rung that failed and `ble_hint()` says what to do
— the useful part when BLE is finicky:

```
not compiled in → BLE ships only in [env:full] (the NimBLE image ~2.7 MB doesn't
                  fit the dev/release OTA A/B slots).
stack down      → NimBLE init failed. Needs ~96 KB free internal RAM (a ~30 KB
                  contiguous block PSRAM can't host) — free RAM or the build is
                  too loaded to bring the radio up.
no GATT service → stack up but ble_status_init() didn't register the service.
advertising     → connect a phone (nRF Connect / any BLE scanner) to confirm.
connected       → read the health/battery characteristic to finish.
verified        → advertised + connected + a characteristic exchanged.
```

### The honest BLE caveats (why it's hard)

- **Flash.** The NimBLE-inclusive FULL image is ~2.7 MB and only fits the
  `default_8MB` table; it can't live in the 1.9 MB OTA A/B slots the shipping
  `dev`/`release` images use. That's why BLE is `[env:full]`-only
  (`PARTITIONS.md`).
- **RAM.** The controller needs a ~30 KB **contiguous internal** DMA block and
  ~96 KB total free internal RAM at the init gate; on a loaded S3 that's often
  not there, and with PSRAM off `NimBLEDevice::init()` can panic
  (`LESSONS_LEARNED.md`). A real self-test therefore surfaces the *degraded /
  init-failed* case rather than assuming the radio came up.
- **Base-canary gaps to be aware of:** the Scout scan loop stays dormant because
  `ble_scout_allow_radio()` is never called on the base canary, and
  `ble_status_stack_begin()` calls `NimBLEDevice::init()` from `setup()` without
  the `bt_defaults` heap guard the WAP sketch has. Both are worth wiring before
  relying on BLE in production.

## Phase 2 (gated, follow-up): Demo + Mutate tiers

Behind `FEATURE_TEST_CONSOLE`, add benign demos that call the existing feature
tests — camera `captureFrame()`, `audio_selftest_start()`, and a small new
`ble_status_selftest_start()` (mirror the audio self-test: ensure advertising,
arm a window, count `onConnect` in a new static → "a phone connected = pass").
Mutating commands stay dev-only behind the physical confirm. None of it links
into a production image.

## Files

- `firmware/common/health/test_console.h` — the tier policy + BLE ladder (pure, host-testable)
- `firmware/tests_host/test_test_console.cpp` — host tests proving the invariants (CI)
- `firmware/canary/src/main.cpp` — the read-only `t` command
- `firmware/canary/include/canary_config.h` — `FEATURE_TEST_CONSOLE` (off by default)
