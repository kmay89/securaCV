# USB Onboarding — "plug me in" (consented HID + read-only drive + guided recovery)

Design document. Status: **draft — Phase 1 (logic + glue + website) implemented;
Phase 2 (on-device HID/MSC validation) pending.** The feature flag ships **off**;
it exists only in the opt-in `[env:usb-onboard]` build.

Companion to [`usb_evidence_drive.md`](usb_evidence_drive.md) — that doc covers
the read-only SD-over-USB drive and drop-file updates; this one adds the *human*
layer: what a person sees the moment they plug a Canary into a computer.

## Why

When someone plugs a Canary into a laptop, the most useful thing that can happen
is that they land on a page that explains this exact device — how to browse its
files, how to recover it, how to unseal its evidence. So the device does that:
on a USB-OTG build it enumerates as a composite device and, on a deliberate
confirmation, opens the owner's help page in a browser.

A device that *types on its own* is the BadUSB attack pattern. For a **privacy
and safety** product that is unacceptable unless the trust model is airtight and
visible. So the trust model is the feature. This is the anti-BadUSB:

```
                 ┌─────────────────────────────────────────────┐
   USB-C ──────► │ TinyUSB composite device (ESP32-S3 OTG)     │
                 │  ├─ CDC : the serial console (unchanged)    │
                 │  ├─ HID : a keyboard that types ONE url,    │
                 │  │        only after a physical confirm     │
                 │  └─ MSC : the SD card, READ-ONLY            │
                 └─────────────────────────────────────────────┘
```

## The five safeguards (why this is not a BadUSB)

These are enforced in host-tested pure logic
(`firmware/common/usb/usb_onboard_logic.h`,
`firmware/tests_host/test_usb_onboard_logic.cpp`), not just asserted in prose:

1. **It never types on its own.** Enumerating does nothing. The HID keyboard
   emits keystrokes only on the `CONFIRM` edge of the consent state machine,
   which is reachable only from `ARMED`, which is reachable only from an
   explicit owner `REQUEST` (the console `u` key). `step(IDLE, CONFIRM)` does
   not type — pinned by `test_confirm_only_types_after_arming`.
2. **It announces first.** `REQUEST` prints, in plain text on the console, the
   exact URL it will type and opens a **15-second** arming window. Any other
   key, a timeout, or an unplug re-locks it. The physical **BOOT** button is the
   only confirmation.
3. **One fixed destination, validated twice.** The only thing the keyboard can
   type is the compile-time `SECURACV_HELP_URL_BASE`
   (`https://securacv.com/canary`). `build_launch_plan()` refuses any URL that
   fails `is_allowed_help_url()` — a positive-character allow-list that admits
   only `https://<help-origin>…` with URL-safe characters and no shell
   metacharacter — so even a corrupted device id can never become a typed
   command. Pinned by `test_allowlist_rejects_everything_else` and
   `test_plan_refuses_bad_url`.
4. **The drive is read-only.** The MSC LUN's write callback always fails; the
   host can copy evidence off but cannot alter or erase it.
5. **Off by default, disableable.** `FEATURE_USB_ONBOARD=0` everywhere except the
   opt-in profile; even there the default launch method is `MANUAL` (type the
   URL as text, the person presses Enter — no OS hotkey, no automation).

## What the person experiences

1. Plug the Canary into a computer. It appears as a serial device and (if the
   SD is mounted) a read-only drive named `CANARY-EVIDENCE`. **Nothing types.**
2. Open the serial console (115200 baud) and press `u`. The console prints:

   ```
   === Open help page? ===
     Method : manual (type URL, you press Enter)
     Will type: https://securacv.com/canary?d=<device-id>&r=onboard
     Nothing is typed until you press the physical BOOT button.
     Press any other key to cancel. Auto-cancels in 15s.
   ```
3. Press **BOOT** (short press). The keyboard types the URL (and, for the OS
   launch methods, opens the browser). The person lands on
   `securacv.com/canary`, which explains file browsing, recovery, and unsealing
   — and opens the relevant section when the URL carries `&r=recover` / `&r=unseal`.

## Console commands (added)

| Key | Action |
|-----|--------|
| `u` | Ask to open the help page — announces the URL and arms the confirm window |
| `o` | Show onboarding status; cycle launch method (manual → macOS → Windows → Linux) |
| `v` | Print the recovery guide (SD-wins chain reconciliation) |
| `k` | Print the unseal guide (off-device, with the operator's private key) |
| BOOT short press | **Confirm** — the only input that lets the keyboard type |

## Recovery & unsealing (surfaced, not reinvented)

The `v`/`k` guides and the website describe behavior that already exists:

- **Recovery** is the witness chain's existing *SD-wins* reconciliation
  (`witness_recover_from_sd()`): a factory reset erases NVS settings but never
  the SD evidence; on boot the device adopts the SD tail only when it is
  strictly ahead and its signature verifies. Re-seat the card, reboot, done.
- **Unsealing** happens **off-device**. Sealed `.svlt` snapshots are encrypted
  to the operator's public key (X25519 + HKDF + ChaCha20-Poly1305 write-only
  escrow); the Canary cannot decrypt them by design. The operator copies them
  off the read-only drive and runs `tools/unseal_snapshot.py` with their private
  key.

## Build & enablement

`ARDUINO_USB_MODE` must be `0` (USB-OTG/TinyUSB) for HID + MSC; the stock canary
profile builds `1` (hwcdc). So this is a separate profile:

```bash
cd firmware/canary
pio run -e usb-onboard -t upload
```

The default `dev`/`release` images are unchanged — the `securacv_usb_onboard`
library is only `#include`d under `FEATURE_USB_ONBOARD`, so it is not even
compiled into other profiles. Flashing the OTG build uses the native-USB DFU
path (the same enumeration change the WAP evidence-drive build already documents).

## Phase status

- **Phase 1 (this work):** pure logic + host tests; Arduino HID/MSC glue
  (`canary/lib/securacv_usb_onboard/`); `[env:usb-onboard]`; main-loop wiring;
  the `securacv.com/canary` help page. Flag stays off.
- **Phase 2 (pending):** enumerate the composite device on real macOS / Windows /
  Linux / iPadOS hosts; confirm HID timing (prelude settle, per-key delay) and
  read-only MSC of the SPI SD; only then consider enabling in any release profile.

## Files

- `firmware/common/usb/usb_onboard_logic.h` — pure trust model + URL/allow-list + keystroke plan
- `firmware/tests_host/test_usb_onboard_logic.cpp` — host tests (CI: firmware host tests)
- `firmware/canary/lib/securacv_usb_onboard/` — Arduino HID/MSC glue (flagged)
- `firmware/canary/include/canary_config.h` — `FEATURE_USB_ONBOARD`, `SECURACV_HELP_URL_BASE`
- `firmware/canary/platformio.ini` — `[env:usb-onboard]`
- `firmware/canary/src/main.cpp` — setup/loop/console/BOOT wiring (flagged)
- `securacv_website/canary.html` + `js/canary.js` — the help page the device opens
