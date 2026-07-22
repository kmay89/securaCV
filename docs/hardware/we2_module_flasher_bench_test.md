# WE2 Module Flasher — Bench Smoke-Test

The one validation the software can't do for itself. Everything about the
SecuraCV module flasher (`canary-local/flash.html` → "Load the Vision's
brain") is proven in CI against a *scripted fake bootloader* — the XMODEM
framing, the CRC, the burn-address preamble, the AT frame parser
(`canary-local/tests/we2.test.js`) — and the pinned model is fetched, hashed
and shape-checked weekly (`.github/workflows/vision-model-verify.yml`). None
of that has touched real silicon. **This runbook is the first-hardware gate:
run it once on a physical Grove Vision AI V2 before we tell anyone the module
flasher is production-ready.**

**Time:** ~15 minutes. **Prereq:** you can plug in a USB-C cable and read a
serial log.

**Companion docs:**
[device guide](grove_vision_ai_v2_guide.md) (ports, recovery, protocol) ·
[getting started](canary_vision_getting_started.md) (the full path) ·
the engine + its tests: `canary-local/assets/we2-core.js`,
`canary-local/tests/we2.test.js`.

---

## 0. What you need

| Item | Notes |
|---|---|
| Grove Vision AI V2 module | Seeed SKU 101021112, with an OV5647 camera attached |
| USB-C **data** cable | charge-only cables are the #1 "no port" cause |
| A Chromium browser | Chrome/Edge — WebSerial only exists there |
| The Lab's flash page | `canary-local/flash.html` (served locally, or the published Lab) |

You do **not** need the XIAO host, WiFi, or a broker for this test — it
exercises the module in isolation, over the module's own USB-C port.

---

## 1. Connect the module port (2 min)

1. Plug the cable into the **module's** USB-C — the big carrier-PCB port next
   to the Grove connector, **not** the XIAO's. (Device guide §2 — the two
   ports go to two different chips; the XIAO port cannot reach the Himax
   flash, and the flow will say so if you pick wrong.)
2. Open `flash.html` → scroll to **"Building a Canary Vision? Load the camera
   module's brain here"** → click it.
3. Click **Connect the module**. Pick **USB Single Serial (CH343)** in the
   browser's port chooser.

- ✅ **PASS:** the flow advances to "Module connected" and the AT probe line
  reports either `SSCMA firmware <version>` **or** the honest "no AT answer —
  a model-less module still flashes through its ROM bootloader."
- ❌ **FAIL — no port listed:** wrong port (the XIAO's), a charge-only cable,
  or the CH343 driver is missing (device guide §7; Linux needs only the udev
  rule).

---

## 2. Burn the pinned model (3 min)

1. Choose **The pinned model (recommended)**.
2. Watch the note line: it fetches the release manifest, downloads the model,
   and **verifies SHA-256 before writing a byte**. Confirm it does *not* error
   with a hash mismatch (if it does — stop; that's a supply-chain red flag,
   not a bench problem, and the freshness workflow should already have caught
   it).
3. The burn screen shows the XMODEM progress bar and a log: reset →
   bootloader menu caught → `aiming the burn at 0x400000` → bytes over XMODEM
   → reboot.

- ✅ **PASS:** progress reaches 100% and the flow moves to "Model burned —
  now make it prove it."
- ❌ **FAIL — "no bootloader menu":** the module didn't drop into its ROM
  loader. Try again (the `'1'`-drip window is tight); if it persists, hold the
  module's **BOOT** button, tap **RESET**, release BOOT, and retry (device
  guide §7). You **cannot brick it here** — the burn menu is in mask ROM.
- ❌ **FAIL — transfer errors / CAN:** cable or power; reseat and retry.

> This is the make-or-break step for the protocol extraction. If the burn
> completes against real silicon, the XMODEM engine (`we2-core.js`) is proven
> — everything the fake bootloader asserted holds on hardware.

---

## 3. Make it prove it — the post-flash handshake (2 min)

The flow reboots the module and handshakes over AT. Watch the proof line.

- ✅ **PASS (full):** "The module answered, carries our model card, and ran
  one inference." → the module took `AT+INFO` (our model card is now stored
  on-device), replied to `AT+VER?`, and returned a result to `AT+INVOKE`.
  **This is the everything-works signal.**
- ⚠️ **PARTIAL:** "answers AT but the test inference didn't reply" →
  power-cycle the module once (unplug/replug) and re-open the flow's bench
  check. If it still won't invoke, go to §5 — this is the Vela-compat risk.
- ⚠️ **PARTIAL:** "no AT answer after reboot" → the burn itself completed and
  verified; power-cycle and confirm it comes up. A module running non-SSCMA
  firmware won't speak AT (device guide §4).

---

## 4. The live preview — see it detect a person (5 min)

1. In the flow's **Bench check** panel, click **Start live preview**.
2. Stand back in frame. You should see camera frames render on the canvas with
   a **bounding box drawn around you** and a class/score label.
3. Drag **Confidence (TSCORE)** up — low-scoring boxes drop out. Drag **IoU
   (TIOU)** — duplicate boxes merge. (These write the module's real
   `AT+TSCORE` / `AT+TIOU`.)

- ✅ **PASS:** a box tracks a real person with a plausible score (≥ ~60 at a
  normal distance), and the sliders visibly change what's shown.
- ❌ **FAIL — frames render but no person box ever appears:** the model runs
  but isn't detecting → §5 (likely our Vela artifact vs. the model). Note the
  scores you *do* see.
- ❌ **FAIL — green-tinted / no frames:** camera issue, not the flasher —
  unsupported CSI camera or a backwards ribbon (device guide §3), out of scope
  for this test.
- Click **Stop** / **Disconnect** when done.

---

## 5. If the model runs but won't detect — the Vela fallback

This is the one risk we can't retire without this bench: our release compiles
the MIT person model for the Ethos-U55 ourselves
(`.github/workflows/vision-model-release.yml`). If the burn + AT handshake
succeed (§2–3) but detection is wrong or empty (§4), the **module is fine and
the flasher is fine** — the suspect is our Vela artifact's memory config vs.
what the HX6538 expects.

Confirm and isolate, in order:

1. **Prove the module + camera are good:** load the *same* model the vendor
   way — SenseCraft (device guide §4). If SenseCraft's model detects people
   and ours doesn't, it's confirmed our compile.
2. **File it** against `vision-model-release.yml` with the scores/behavior you
   saw. The fix is a config change to the Vela step, then re-run "Vision Model
   Asset" for the tag — the flasher picks up the new pinned model with no code
   change (it's SHA-verified from the manifest).
3. Meanwhile the page's SenseCraft path stays the documented fallback, so
   users are never stuck.

If §2–3 succeed **and** §4 detects people, the whole path is validated on real
hardware — record the module's firmware version, the model version from the
manifest, and mark the module flasher production-ready.

---

## 6. Recovery (only if something looks bricked)

You can't brick the module from the flasher — the burn menu lives in the
HX6538's ROM. If the module stops responding entirely:

- **Bootloader mode by hand:** hold **BOOT**, tap **RESET**, release BOOT,
  then retry the flow (device guide §7).
- **Bricked module bootloader:** recover it *through the host MCU* over I2C
  (`we2_iic_bootloader_recover`, device guide §7) — 3–10 attempts is normal.
- **Factory module firmware:** Seeed's factory flasher bundle (device guide
  §7).

---

## Pass criteria (the gate)

| # | Check | Must |
|---|---|---|
| 1 | Module port connects, AT probe answers or degrades honestly | PASS |
| 2 | Pinned model burns to 0x400000, SHA-256 verified, reaches 100% | PASS |
| 3 | Post-flash AT handshake + `AT+INFO` card + one `AT+INVOKE` reply | PASS (full) |
| 4 | Live preview draws a person box; TSCORE/TIOU sliders take effect | PASS |
| 5 | If §4 fails, isolated to our Vela artifact (SenseCraft model works) | recorded |

All four green → the module flasher is validated on hardware. Anything red in
§1–3 is a protocol/engine finding (high priority — the CI fake bootloader
missed it); a red §4 that §5 pins to the Vela compile is a packaging finding,
not an engine one.
