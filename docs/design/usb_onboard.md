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
   emits keystrokes only on the `Confirm` edge of the consent state machine — a
   physical **BOOT** press. `step(Off, Confirm)` never types (feature off) and a
   console `Request` alone never types; pinned by `test_disabled_never_types`
   and `test_one_tap_confirm_types`. A dropped device won't press its own
   button, which is what keeps a self-typing keyboard from being a drop attack.
2. **The frictionless open is injection-free.** The default way to "open the
   website on plug-in" is *not* the keyboard at all: the firmware drops a
   `START-HERE.html` (+ Windows `.url` + macOS `.webloc`) at the root of the
   read-only drive. Plug in, open the obvious file, done — no console, no
   keystrokes. The keyboard is the *one-tap* convenience on top (tap BOOT and it
   types the URL), not the primary path.
3. **One fixed destination, validated twice.** The only thing the keyboard — or
   a START-HERE file — can carry is the compile-time `SECURACV_HELP_URL_BASE`
   (`https://securacv.com/canary`). `build_launch_plan()` and every link-file
   builder refuse any URL that fails `is_allowed_help_url()` — a
   positive-character allow-list that admits only `https://<help-origin>…` with
   URL-safe characters and no shell metacharacter — so even a corrupted device
   id can never become a typed command or a booby-trapped shortcut. Pinned by
   `test_allowlist_rejects_everything_else`, `test_plan_refuses_bad_url`, and
   `test_link_files_refuse_bad_url`.
4. **The drive is read-only.** The MSC LUN's write callback always fails; the
   host can copy evidence off but cannot alter or erase it.
5. **Off by default, disableable.** `FEATURE_USB_ONBOARD=0` everywhere except the
   opt-in profile; even there the default launch method is `MANUAL` (type the
   URL as text, the person presses Enter — no OS hotkey). Fully hands-off
   auto-typing exists only behind the separate, default-off
   `USB_ONBOARD_AUTOLAUNCH` compile flag (see below).

## What the person experiences (frictionless by default)

1. Plug the Canary into a computer. It appears as a serial device and (if the
   SD is mounted) a **read-only drive** with a `START-HERE.html` /
   `Open-Canary-Help.url` / `Open-Canary-Help.webloc` at its root. **Nothing
   types.**
2. **Zero-touch:** open `START-HERE` from the drive → the browser goes to
   `securacv.com/canary?d=<device-id>&r=onboard`. No console, no button, no HID.
3. **One-tap:** or just tap the physical **BOOT** button → the keyboard types
   the URL (and, for the OS launch methods, opens the browser). One deliberate
   action, no serial console.
4. **Console (optional preview):** power users can press `u` on the serial
   console for an announced preview + a 15-second armed window, then confirm
   with BOOT — useful when choosing the OS launch method.

Either way the person lands on `securacv.com/canary`, which explains file
browsing, recovery, and unsealing — and opens the relevant section when the URL
carries `&r=recover` / `&r=unseal`.

### Fully hands-off (opt-in): `USB_ONBOARD_AUTOLAUNCH`

For a genuinely zero-interaction auto-open, `-DUSB_ONBOARD_AUTOLAUNCH=1` makes
the device auto-fire the launch once, a few seconds after enumeration, with no
button press. This is real HID auto-typing — the BadUSB shape — so it ships
**off**; the START-HERE file gives the same "it just opens" feel without
injecting into whatever machine the device is plugged into. Enable it only for
devices and hosts you control.

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

## Keeping the demo honest (anti-rot)

The website's `/plugin` emulator illustrates this exact behavior, so the two
must not drift. The shared "contract" — the `?r=onboard` reason tag and the
START-HERE filenames — lives in one place, `usb_onboard_logic.h`
(`kOnboardReason`, `kStartHereHtml`, `kStartHereWinUrl`, `kStartHereMacWebloc`),
used by the glue and pinned by `test_usb_onboard_logic.cpp::test_onboarding_contract`.
The website mirrors the same values in `securacv_website/onboarding-spec.json`
(with `provenance` back to these symbols) and pins them in
`tests/plugin-facts.test.mjs`. Change a value on either side and that side's CI
fails with a message to update the other — neither can rot silently. One block
of that file is not hand-mirrored at all: its `builds` object is stamped from
`firmware/build_matrix.json` by `scripts/carry_to_site.py --site
<website-checkout>` (the website's weekly carry job runs it), so the /checkup
firmware-type selector follows the build matrix without anyone retyping it.

## Files

- `firmware/common/usb/usb_onboard_logic.h` — pure trust model + URL/allow-list + keystroke plan + onboarding contract constants
- `firmware/tests_host/test_usb_onboard_logic.cpp` — host tests (CI: firmware host tests)
- `firmware/canary/lib/securacv_usb_onboard/` — Arduino HID/MSC glue (flagged)
- `firmware/canary/include/canary_config.h` — `FEATURE_USB_ONBOARD`, `SECURACV_HELP_URL_BASE`
- `firmware/canary/platformio.ini` — `[env:usb-onboard]`
- `firmware/canary/src/main.cpp` — setup/loop/console/BOOT wiring (flagged)
- `securacv_website/canary.html` + `js/canary.js` — the help page the device opens
