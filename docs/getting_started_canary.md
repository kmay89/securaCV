# Setting up your Canary

> A SecuraCV Canary is a small RF-sensing witness device. It watches the
> shape of motion in a room — without a camera, without recording a single
> second of audio, without ever learning who you are. (Where a Canary has a
> microphone, it is honest work: in stock builds it listens only for the
> standard smoke and CO alarm cadences as an on/off loudness envelope,
> never as sound it keeps — see
> [section 5](#5--listening-for-alarms).) This guide gets one going in
> five minutes.

**The journey, by surface** (each is optional; every one is free): rehearse
in [the Lab](https://kmay89.github.io/securaCV/canary-local/) — the real
firmware, compiled to WebAssembly, in your browser — then flash a real board
in [the browser flasher](browser_flasher.md) or the desktop
**SecuraCV Flasher** app, then live with the fleet in
[Home Assistant](homeassistant_setup.md), the iPhone / Apple Watch
[companion app](../ios/README.md), or the Apple TV
[Witness Wall](tvos/README.md).

---

## What's in the box

- One SecuraCV Canary (a XIAO ESP32-S3 in a small enclosure)
- A USB-C cable
- A printed card with your device's **AP password** and **Device ID**

If your Canary did not come with a card, the password is also burned into
the device and shown on the serial console at first boot — see *Recovering
the password* at the bottom of this page.

Building your own from parts? See the
[Canary Peripheral Build Plan & BOM](hardware/canary_peripheral_build_plan.md)
for what to buy (buzzer, status LED, button/tamper/touch, battery, enclosure)
and how it wires up.

**Already have an ESP32 board in a drawer?** It may well run this firmware —
the supported-board table in
[`firmware/boards/README.md`](../firmware/boards/README.md) includes the
AI-Thinker ESP32-CAM, generic ESP32-WROOM-32 DevKits, and the Freenove
ESP32-S3 camera kit alongside the XIAO, each with an honest note on what it
can and can't sense. Two things before you flash a board you didn't buy new
from us-adjacent channels:

- Read [Unflashed boards — what protects you](unflashed_board_intake.md)
  first and bring the board up cold (hold BOOT while plugging in). A
  secondhand board arrives carrying somebody else's flash contents; that
  page is the honest account of what the flashers can and can't check.
- If your board **isn't** in the table, that's not a dead end: a new board
  is a data contribution — pin map plus build config, no core code. The
  one-PR recipe is [`firmware/PORTING.md`](../firmware/PORTING.md), and a
  registered board gets CI-built on every PR from then on.

---

## 1 · Plug in

Plug the USB-C cable into your Canary and into any 5 V power source — a
phone charger, a laptop, a USB battery. Within ten seconds the indicator
LED stops blinking and the device is ready.

That's the entire hardware setup.

---

## 2 · Connect with your phone

On your phone, open Wi-Fi settings. You'll see a new network named
something like:

> **SecuraCV-AB7K**

The four characters at the end are unique to your Canary. Tap the
network and enter the **AP password** from your card. (The password is
*not* `password` and *not* the device ID — it's the eight-character
string printed under the QR code.)

Your phone will **stay** connected — the Canary answers your phone's
"is there internet here?" check so it won't drop the network or warn you
that there's no internet. (There isn't, by design: the Canary never
phones home. It just keeps the connection so you can reach it.)

Now open a browser and visit:

> **http://canary.local**

- **iPhone / iPad / Mac:** a "Sign in" panel may pop up first with the
  same instruction — you can read it and then open Safari, or just tap
  *Cancel → Use Without Internet* and go to `canary.local`. `canary.local`
  works reliably here.
- **Android:** no pop-up appears (that's normal — it's what keeps you
  connected). Open Chrome and type `canary.local`. Some Android browsers
  don't resolve `.local` names; if the page doesn't load, use the numeric
  address instead:

> **http://192.168.4.1**

`192.168.4.1` always works, on every phone. It's the safe fallback any
time `canary.local` doesn't load.

That's it. You're now talking to the device.

---

## 3 · See what it's sensing

The dashboard opens on the **Status** tab. Tap **Sensing** in the top nav
to see what the radio is feeling, in plain English.

You'll see a colored pill at the top of the page:

| Pill | What it means |
|---|---|
| **Quiet** | The room looks empty or perfectly still. |
| **Presence** | Steady micro-motion — the kind of signal a person sitting and breathing makes. |
| **Motion** | Clear room-scale movement. Someone is walking or moving objects. |
| **Active** | Sustained, vigorous activity. |
| **Offline** | The radio hasn't started yet, or this build doesn't include CSI sensing. |

Underneath are three gauges (Motion, Breathing-band, Signal), an
8-band spectrum showing where the activity is happening, four direction
arrows showing whether motion is approaching or receding, and a
breathing-band spectrum that lights up when someone in the room is at
rest.

> **Try this:** open the Sensing tab on your phone, then put the phone
> down and stand a few meters from the Canary. Wave your arms once.
> Within a second the **Motion** gauge will jump and the spectrum bars
> will dance. Stop moving. Watch the pill settle to **Presence** as
> long as you stay in the room, then to **Quiet** after you leave.

---

## 4 · Place it well

The Canary uses Wi-Fi radio reflections. A few rules of thumb:

- **Mount it on a wall, ~1.5 m off the floor.** Same height as a
  thermostat. This is the sweet spot for picking up walking adults
  without being confused by furniture.
- **Aim it across the room, not at a wall.** The radio sees a cone-ish
  region in front of itself.
- **Avoid microwaves, baby monitors, and dense metal.** They flood the
  2.4 GHz band with their own signals. If your **Drop: rate-limit**
  counter on the Sensing tab is climbing fast, something nearby is
  drowning the Canary out.
- **Keep the device powered.** It runs continuously; battery operation
  is a future option (the *Garage / Workshop* profile in the roadmap).

A single Canary covers a typical room well. **Two Canaries on either end
of a hallway** unlock cross-device direction sensing — see the Mesh tab
once your second device is paired.

> Adding a second (or third) Canary? See
> [Onboarding Multiple Canaries](onboarding_multiple_canaries.md) — it covers
> naming each device (`canary-kitchen.local`), how `canary.local` behaves with
> several devices, and the **Identify** button that blinks a specific device so
> you can tell them apart.

---

## 5 · Listening for alarms

Below the main sensing tiles you'll see a card titled **Acoustic alarms**.
The Canary's microphone is *always* listening, but only for two
specific patterns — the standard cadences every code-compliant smoke
and CO alarm in your home already emits:

| Cadence | Standard | What it sounds like |
|---|---|---|
| **T3** | NFPA 72 / ISO 8201 (smoke) | Three half-second beeps, half-second gaps, then 1.5 s silence — repeating. |
| **T4** | UL 2034 (carbon monoxide) | Four short beeps, half-second silence, then five-second silence — repeating. |

When the Canary recognizes the pattern, the card turns red and the
status pill reads **🔥 Smoke alarm pattern** or **⚠ CO alarm pattern**.
That same event flows through the witness chain so a Home Assistant
automation can react — for example, send everyone in the house a push
notification *because* the kitchen Canary heard the upstairs smoke
alarm before anyone in the basement noticed.

What this card **does not** do:

- It does not record audio. Ever. The microphone's output is reduced
  to a single loudness number every 20 ms, then the sample buffer is
  zeroed *in place* before the next 20 ms arrives.
- It does not recognize voices, words, or specific sounds beyond the
  two regulatory cadences. Speech is structurally impossible to
  recover from a binary on/off envelope.
- It does not phone home. The match is local.

If your smoke / CO detector uses a non-standard cadence (rare, mostly
older European models), the card will stay quiet — the Canary deliberately
avoids fuzzy matching.

### 5.1 · Supplement, not a substitute

The Canary is **not a UL-listed life-safety device**, and the firmware
will tell you so on the Acoustic Alarms card. It cannot detect smoke,
fire, or CO **directly** — it only hears your existing alarms. Keep
your code-compliant smoke and CO detectors. The Canary's job is to help
a UL-listed alarm get *noticed* (sent to your phone via Home Assistant,
written into the witness chain, shared across a mesh of Canaries) — not
to replace it.

### 5.2 · What it can and can't hear

These are the realistic limits of the current implementation. They are
honest because lying about safety equipment gets people hurt.

| Scenario | Likely outcome |
|---|---|
| UL-listed alarm sounding within ~3 m, line of sight, quiet room | Reliable detection within a few alarm cycles (~10–15 s). |
| Same alarm through a closed bedroom door | Often works but slower; detection rate drops sharply. |
| Alarm one room over or more | **Unreliable.** Treat as a bonus, not a guarantee. |
| Running dishwasher / TV / loud conversation in the same room as the alarm | The noise can pin the envelope into the "ON" state between beeps and break the cadence match. Move the Canary closer to the alarm if you can. |
| Alarm with a non-standard cadence (proprietary, older European, voice annunciator) | Won't match. The Canary only recognizes NFPA 72 (T3, smoke) and UL 2034 (T4, CO). |
| Alarm's low-battery "chirp" every 30–60 s | Won't match — that's not the alarm cadence. |
| You have no smoke or CO alarm | The Canary cannot warn you. It is not a primary detector. |

The default RMS thresholds (ON = 800 / OFF = 400) are tuned for a UL
alarm at ~85 dB SPL at 3 m in a typical room. They are compile-time
constants in `firmware/canary/lib/securacv_audio/src/securacv_audio.h`.

### 5.3 · Verifying the mic isn't recording you

Privacy here is enforced structurally — the int16 PDM buffer is wiped
inside the same call that produced it, before any other code can touch
it (see `securacv_audio.cpp::process()`). But you shouldn't have to
trust us; you should be able to check.

- **Mute it.** The Acoustic Alarms card has a `Mute microphone` button.
  Clicking it calls `POST /api/audio/mute`, which physically
  uninstalls the I2S driver and releases GPIO 41/42. The card pill
  turns gray, the level meter goes to zero, and the `enabled` field
  in `/api/status` flips to `false`. The mute persists across reboots
  (stored as the `mic_muted` bool in the `securacv` NVS namespace).
- **Inspect.** Hit `GET /api/audio/level` — when muted, `running` is
  `false` and `rms` is `0`. No audio path is open; the GPIOs are
  tri-stated until you unmute.
- **Reproduce.** The full audio stack is open-source. The privacy
  contract is asserted in the doc-comment at the top of
  `securacv_audio.h`, and the `secure_wipe()` of every PDM buffer
  lives at `securacv_audio.cpp::process()`.
- **Audit it.** Every mute or unmute is signed into the Ed25519
  witness chain with the source (dashboard / Home Assistant / boot)
  and a 10-min time bucket. Export the chain (**Settings → Export →
  Witness chain**) to see when the mic was on or off — useful if you
  ever need to prove "the device was *not* listening at time X" or
  "the device was un-muted before the incident."
- **Toggle from Home Assistant.** If MQTT is configured, the
  device exposes a `Microphone` switch entity. Flipping it from HA
  goes through the same I2S teardown, the same NVS persistence, and
  the same witness signature as the dashboard toggle — with the
  source tagged `MQTT` so an auditor can tell.

### 5.4 · Testing the mic

Two tests, both built into the **Acoustic alarms** card under the
collapsible **Test the microphone** section.

**Step 1 — Is the mic alive?** Open the test panel. You'll see a small
horizontal bar with two notches (the OFF and ON thresholds). Clap or
speak near the Canary. The bar should jump well above the ON notch and
fall back when the room is quiet. If the bar never moves, the I2S
driver isn't running — check the device serial log for `Audio: I2S`
errors, or check the `i2s_read_errors` counter in the stats grid.

**Step 2 — Does the cadence detector work?** Put the Canary within ~3 m
of your smoke or CO alarm, then click **Listen for 30 s**. Press the
physical TEST button on your alarm. Within a few cycles the panel
should report `Matched smoke_alarm_t3` or `Matched co_alarm_t4`. While
self-test is active the matcher uses slightly relaxed timing and the
event callback is **suppressed** — no Home Assistant automation fires
during a test press.

If nothing matches in 30 s, the panel will tell you whether it heard
*any* sound transitions (so you can tell "mic broken" from "alarm too
far away" from "alarm uses a non-standard cadence").

**Step 3 — No alarm? Test against a synthetic tone.** Below the *Listen
for 30 s* button there are **Play T3 (smoke)** and **Play T4 (CO)**
buttons. They use your phone's or laptop's speakers to emit the
standard 3.2 kHz alarm cadence — sample-accurately scheduled in the
browser via Web Audio API. Hold the device within ~30 cm of the Canary;
the test panel should match within a couple of cycles. **This is a
synthetic test pattern.** Speakers and real alarms differ in frequency
response and reverberation, so it's a fast sanity check that the
detector works at all — not a replacement for the real-alarm test
above. Always verify against an actual UL-listed alarm before relying
on the detector.

### 5.5 · Transient detection (knock / doorbell / glass break)

Builds with `FEATURE_ACOUSTIC_TRANSIENTS=1` (the `dev` and `dev_ha`
profiles in `platformio.ini`; off by default in `release`) add three
opt-in heuristic detectors that ride the same 50 Hz envelope as the
T3 / T4 cadence matcher, plus one extra scalar per frame: the
high-passed RMS (corner ≈ 4 kHz). The privacy story does NOT change:
the raw int16 PDM samples are still wiped inside the same call that
produced them, no spectrogram is stored, and the only data that
crosses the module boundary is the usual `{event_type, confidence,
time_bucket, cycle_count}` event record.

| Detector | Fires on | Conf floor | Likely false positives |
|---|---|---|---|
| **Knock** (`AUDIO_EVENT_KNOCK`) | 3 short impulses (30–180 ms each, 60–400 ms apart) with low-band-dominant character. | 50 | Drumming, repeated table thumps, hand claps. |
| **Doorbell** (`AUDIO_EVENT_DOORBELL`) | 2 mid-band tones (250–900 ms each, 50–400 ms gap), trailing silence. | 60 | Two-word commands, double-beep appliance ready tones. Modern wireless / melodic doorbells with longer melodies will NOT match. |
| **Glass break** (`AUDIO_EVENT_GLASS_BREAK`) | Single sustained ON (0.8–3 s) with high-band-dominant character (HPF/full RMS > 1.3×). | 70 | Hair-dryer, vacuum, certain HVAC fans, prolonged hissing leaks. |

These are **heuristic** detectors. They are *not* replacements for a
UL-listed glass-break sensor, a doorbell switch, or a security alarm.
They are a "did something noisy in this category just happen?" signal
with a low false-positive rate by virtue of the conservative confidence
floors and trailing-silence gates, but they will miss real events that
sit outside the spec windows above. Every match is signed into the
witness chain just like the T3 / T4 records, so an after-the-fact
auditor can see "at 02:31 the device flagged a glass-break event with
73% confidence." Trust the chain, not the binary sensor.

### 5.6 · Home Assistant diagnostic sensors

The HA discovery payload also exposes 7 diagnostic counters under the
device's *Diagnostic* card so you can plot rates over time:

| Entity | Tracks |
|---|---|
| **T3 Cycles Total** | Confirmed smoke-alarm cadences since boot |
| **T4 Cycles Total** | Confirmed CO-alarm cadences since boot |
| **Knock Count** | Phase 2b knock matches since boot |
| **Doorbell Count** | Phase 2b doorbell matches since boot |
| **Glass Break Count** | Phase 2b glass-break matches since boot |
| **Audio Frames** | Total 20 ms RMS windows processed (~50 / s while running) |
| **I2S Read Errors** | DMA underflows / driver errors — alert if climbing |

Plus a **Run Audio Self-Test** button entity that triggers the same
30 s relaxed-tolerance window as the dashboard's *Listen for 30 s*
button. The button can be wired into an HA automation that fires
every Monday at 9 am to verify the detector still matches against
your alarm's regular weekly test press.

---

## 6 · The silent panic pad

Below the **Acoustic alarms** card you'll see a card titled **Touch**.
Out of the box it's connected to **GPIO 4** (pin **D3** on the XIAO
header), waiting for one of three things:

- **Long-press → silent panic.** Hold a finger on the pad for 1.5 s
  (or anything you've wired the pad to — a bedside sticker electrode,
  a hidden contact under a desk). The device fires a `silent_panic`
  event into the witness chain *without* flashing an LED, beeping,
  or otherwise announcing the press to anyone in the room.
- **Sustained drop below baseline → enclosure tamper.** If somebody
  opens the case or pries the device off its mount, the touch pad
  loses its connection to the electrode and the reading drops to
  less than half of its calibrated baseline; held there for more
  than 5 s, `enclosure_tamper` fires.
- **Brief approach (optional) → presence courtesy.** Off by default;
  a hand or body within a few centimeters triggers `approach` once
  per 1.5 s.

The first ~2 s after boot are spent calibrating the baseline — during
that window the card reads "Calibrating" and the panic / tamper
detectors are off. After that the card stays at "Idle" until something
happens.

To wire your own panic surface, route a wire from **D3** to a small
metal plate, sticker electrode, or a piece of conductive copper tape
hidden under furniture. Anything bigger than a coin works.

If you want to use a different pin, override at compile time with
`-DTOUCH_PIN_NUM=N` where N is one of `1, 3, 4, 5, 6` (the available
non-conflicting touch channels on the XIAO Sense — the SD-card and
camera pins are excluded).

---

## 7 · Appliance activity

The **Appliance activity** card listens — *passively, without storing
anything* — for the IR pulses that every TV remote, AC remote, and
set-top-box remote in your home emits. The standard cadences (NEC,
RC5, Sony SIRC) cover well over 90 % of consumer remotes.

When somebody hits a button, the card lights up:

- **Last protocol** tells you what family of remote it was (NEC =
  most TVs/AC; RC5 = Philips; Sony = its own thing).
- **Hash bucket** is a small number 0–15. The same button on the
  same remote produces the same bucket *within one session*. Across
  reboots the buckets shuffle — a per-session salt is mixed into
  the hash, so the bucket can't be used to track a remote across
  days.

What this gives you:

- A "household active" baseline. If the kitchen canary normally sees
  TV-remote activity from 7–10 pm but goes silent for three days,
  Home Assistant has a clear signal something is off — without the
  canary ever recording *what was watched*.
- A coarse "which appliance" hint without identification: bucket #3
  pressed twice in 5 s = "they're using the same thing twice"; bucket
  #7 in another room = "different appliance / different remote." No
  more than that.

What you'd need to wire this up: a generic 38 kHz IR receiver module
(VS1838B, TSOP4838, anything in that family). Three pins: VCC →
3.3 V, GND → GND, OUT → **GPIO 3 (D2)**. Override at compile time
with `-DIR_RX_PIN_NUM=N` if you've already wired that pin to
something else.

If your board has no IR receiver attached, the card stays at **No
activity** forever — that's fine.

---

## 8 · Thermal drift

The **Thermal drift** card watches the chip's *own* die temperature
once a minute. The internal sensor on the ESP32-S3 is rough — about
±2 °C absolute accuracy — but very repeatable, which is what we
need.

Once the device has run for ~5 minutes the baseline is locked.
After that, if the temperature suddenly steps **±5 °C** — say
because someone opened the case and let the warm air out, or
unmounted the device and carried it from a heated living room to a
cold garage — the card flips to **⚠ Thermal drift**.

This is *defense-in-depth* on top of the touch-pad tamper detector
from §6. The touch pad catches the common "case opened" scenario;
the temp sensor catches the slower "device moved" scenario the
touch pad can't see.

The card never displays a precise temperature — only whole-degree
readings. That's deliberate; an attacker with access to the
dashboard learns nothing about the room beyond "warmer than 20 °C
or cooler than 20 °C", because the calibration is also rounded.

Don't confuse this card with **Adaptive performance**, which sits
right below it: Thermal drift is a *tamper* detector (sudden steps),
while Adaptive performance shows how the device paces heavy work
like camera streaming to stay inside its thermal envelope, plus its
lifetime heat/cold history. Placement, heat-sink, and hot/cold
weather guidance lives in the
[Thermal Guide](./thermal_guide.md).

---

## 9 · Power & wake

The **Power & wake** card surfaces what the ESP32-S3's RTC peripheral
sees when the chip wakes up from deep sleep. On a normal boot it reads
**cold_boot**; on a touch-pad wake it reads **touch** with the firing
pad number; on an EXT0 GPIO wake it reads **ext0_gpio**, and so on.

Today the canary runs always-on (constant Wi-Fi AP, HTTP server,
sensing). A future battery profile (the *Garage / Workshop* deployment)
will deep-sleep between events and wake on touch / timer. The
plumbing for that is already in place — the **Caps** field on this
card lists which native wake sources are wired up: `timer`, `touch`,
`ext0`, `ext1`, `ulp-riscv`. When you flash a battery build, those
become live.

---

## 10 · What it never does

Three things are **structurally impossible** with this device — not
*hard*, not *configurable off*, but unable to:

1. Identify a person. The radio signal that touches your phone or watch
   never lands in storage. The MAC and BSSID are scrubbed in the same
   callback that receives them; only a 32-byte numeric summary of the
   room's RF shape is kept, and that summary is bucketed to integers
   so device-fingerprinting is information-theoretically impossible.
2. Recognize a face or record audio. There is no camera frame storage
   in the firmware (the optional Peek tab is a live MJPEG passthrough,
   never written to SD). The microphone is not enabled in this build.
3. Tell anyone outside your home what it sees. The Canary is on its own
   Wi-Fi network. It optionally connects to your home Wi-Fi for Home
   Assistant integration; it never reaches the internet on its own.

If you want the long version, read `spec/canary_free_signals_v0.md`
(Invariants A–F) and `kernel/rf_presence_architecture.md`.

---

## Optional: connect it to your home Wi-Fi

The Canary works completely offline as an island device — your phone
connects directly to it. If you'd rather have it appear on your home
network (so it's reachable while your phone is on home Wi-Fi, and so
Home Assistant can discover it):

1. On the dashboard, tap **Settings → Wi-Fi**.
2. Tap **Scan**, choose your home SSID, enter the password.
3. The Canary stays on its own network *and* joins yours (dual-mode).

The dashboard URL becomes `http://canary-<your-id>.local` once it joins
your home network.

---

## Optional: Home Assistant

If you have Home Assistant on your network and have configured an
MQTT broker (Mosquitto add-on works fine), point the Canary at it
once and ~20 entities appear automatically under a single device
named after your Canary's ID. **No cloud, no account, no integration
to install** — Home Assistant's MQTT discovery does the work.

The sensing entities you'll see:

| Entity | Type | Source |
|---|---|---|
| **Activity** | sensor | quiet / presence / motion / active |
| **Motion Score** | sensor (%) | CSI motion bands |
| **Breathing Score** | sensor (%) | CSI 0.1–0.5 Hz Goertzel |
| **Sensing RSSI** | sensor (dBm) | CSI window mean |
| **Pattern: Smoke Alarm Cadence** | binary_sensor (smoke) | T3 cadence detected — your existing smoke alarm sounding |
| **Pattern: CO Alarm Cadence** | binary_sensor (CO) | T4 cadence detected — your existing CO alarm sounding |
| **Silent Panic** | binary_sensor (safety) | Touch long-press |
| **Enclosure Tamper** | binary_sensor (tamper) | Touch tamper OR thermal drift |
| **Last IR Protocol** | sensor (diagnostic) | NEC / RC5 / Sony / none |
| **Last IR Bucket** | sensor (diagnostic) | 0..15 per-session salted hash |
| **Last Wake** | sensor (diagnostic) | cold_boot / timer / touch / ext0 / ext1 / ulp |
| **Microphone** | switch (icon `mdi:microphone-off`) | Toggle the mic mute from HA. Every flip is signed into the witness chain (audit trail). |

Plus the existing 11 system entities (witness count, chain seq,
uptime, free heap, GPS, online, etc.).

Use these in HA automations:

- *"If kitchen Canary's `Pattern: Smoke Alarm Cadence` goes ON, push
  notification to every phone in the house and flash bedroom lights."*
- *"If any Canary's Enclosure Tamper goes ON, send Slack alert and
  start camera recording on adjacent Frigate instance."*
- *"If bedroom Canary's Activity stays at 'quiet' from 7 am to 11 am
  on a weekday and the elder-care scenario is active, send a wellness
  ping to the family group chat."*

To configure: dashboard → **Settings → MQTT** → enter broker host
and credentials. The Canary publishes one retained snapshot per
30 s to `securacv/{device_id}/sensing` so HA gets the latest state
even after a restart.

---

## The other Canaries: Vision and Sense

Everything above describes the classic Wi-Fi-sensing Canary (the "WAP").
Two sibling devices share the same witness DNA but sense differently and
set up differently. Both are **MQTT-native**: they join your home Wi-Fi,
talk to your MQTT broker, and live inside Home Assistant — no everyday
dashboard of their own. (A unit with no Wi-Fi saved — or one that keeps
failing to join for a reason you can fix, like a changed password — raises
its own `SecuraCV-XXXX` setup network so a phone can point it at your
router; it keeps sensing the whole time.)

### Canary Vision

A person-detection witness. A Grove Vision AI V2 module runs the vision
model on its own NPU and hands the ESP32 host nothing but boxes and
scores over I2C — **no pixels ever cross the wire, and no video is ever
stored**. What you get in Home Assistant: presence, dwelling, confidence,
and a coarse voxel location.

**Bring-up, in short** (full walkthrough:
[`firmware/projects/canary-vision/`](../firmware/projects/canary-vision/README.md)):

```bash
cd firmware/projects/canary-vision
make secrets          # copies secrets.example.h → secrets/secrets.h
# edit secrets/secrets.h: your Wi-Fi SSID/password + MQTT broker IP
pio run -e canary-vision-xiao-c3 -t upload   # pick the env for your board
```

Wi-Fi and the broker address come from `secrets/secrets.h` at build time,
or from the flasher's fields (seeded into the chip's settings at flash
time). If neither gave the board a network, it raises its own
`SecuraCV-XXXX` setup network — join it from a phone and pick your Wi-Fi
there. Flash once over USB; after that, updates arrive over the air.

### Canary Sense

A radar witness. A 60 GHz mmWave module (Seeed MR60BHA2, XIAO ESP32-C6
host) senses presence through the air — no camera, no microphone, no
MAC addresses. It reports presence, a 0/1/2+ occupant bucket, and a
near/mid/far range band. The **wellbeing build** adds a breathing lock;
heart-rate entities exist **only** in that opt-in vitals build and are
compiled out of the default image entirely.

**Bring-up, in short** (full details:
[`firmware/projects/canary-sense/`](../firmware/projects/canary-sense/README.md)):

```bash
cd firmware/projects/canary-sense
make secrets          # copies secrets.example.h → secrets/secrets.h
# edit secrets/secrets.h: your Wi-Fi SSID/password + MQTT broker IP
make upload           # default env; `make upload-wellbeing` for vitals
```

Same deal as Vision: credentials from `secrets.h` or the flasher, the
same `SecuraCV-XXXX` setup network if it has none (or can't join for a
fixable reason), OTA after the first USB flash.

### How they show up

Once on your network, both variants:

- **Announce themselves over mDNS.** Each advertises a `_securacv._tcp`
  service carrying its device ID, name, hostname, firmware version,
  model, and device type — the same advert every Canary (and the
  fleet display) uses, so other SecuraCV devices and the companion app
  can see them on the LAN. They advertise only; there's no `canary.local`
  dashboard to open for these two — the only web page they ever serve is
  the setup wizard, and only while the setup network is up.
- **Appear in Home Assistant automatically.** Point them at the same
  MQTT broker HA uses and the entities register themselves via MQTT
  discovery — no integration to install, same as the WAP.

### Which device is which?

Every Vision and Sense exposes an **Identify** button in Home Assistant
(on the device page, next to its entities). Press it and that device
blinks its LED for ten seconds — the fastest way to tell three
identical white boxes apart while you're labeling rooms.

### What's different from the WAP flow

| | Canary (WAP) | Vision / Sense |
|---|---|---|
| First contact | Join its `SecuraCV-XXXX` network, open `canary.local` | Flash with your credentials over USB (or join its `SecuraCV-XXXX` setup network) |
| Own dashboard | Yes (full web UI) | No — Home Assistant is the UI (setup wizard only, while unprovisioned) |
| Wi-Fi setup | Captive portal / Settings → Wi-Fi | Flasher fields or `secrets/secrets.h`; setup network as the fallback |
| Broker address | Set on the dashboard | Flasher fields or `secrets/secrets.h` |
| Identify | Companion app (blink + chirp) | **Identify** button in Home Assistant |

A unified onboarding wizard that closes this gap — pairing MQTT-only
devices from the companion app without editing `secrets.h` — is on the
roadmap: see
[`docs/onboarding_unified_wizard.md`](onboarding_unified_wizard.md).

> **Honesty note:** the mDNS announcement and Identify button are new in
> this firmware and verified in CI builds; hardware bench validation is
> still in progress.

---

## Keeping it up to date

You never need to take the Canary down off the shelf — or touch it at
all — to update its software.

The device checks for new releases once a day. When one is available:

- **In Home Assistant:** the Canary's **Firmware** entity shows "Update
  available" with the release notes. Press **Install** and watch the
  progress bar. The device restarts on its own and is back within a
  minute.
- **On the device dashboard:** Settings → Device → **Software Update**
  shows the same thing, with the same one-button install.
- **Hands-free:** turn on the **Auto Update** switch (per device) and new
  releases install themselves within a day. It's off by default — your
  Canary never restarts unattended unless you choose that.

Safety, in plain terms:

- Every update file is checked against a cryptographic signature before
  it's installed. A tampered or corrupted file is refused.
- The new software is written **next to** the old one, never over it. If
  the update fails — even from a power cut mid-install — the Canary
  starts right back up on its previous software by itself.
- After installing, the device runs a health check on itself. If anything
  is wrong — including new software that crashes or hangs — it switches
  back automatically and tells you why. One bad start is all it takes to
  recover; you never need to touch the device.
- If power is cut during the first minute after an update, before the
  device finishes checking itself, it simply returns to its previous
  software. Nothing is lost — the update is offered again.
- Every update (and any rollback) is signed into the witness chain, so
  the record shows exactly when the software changed.

There is no way to break the device through an update. In the absolute
worst case a Canary can always be re-flashed over its USB port — updates
never touch the part of the device that makes that possible.

No internet? Updates can also come from a server on your own network —
see `docs/firmware_ota.md` for the air-gapped setup.

---

## Recovering the password

If you've lost the printed card:

1. Plug the Canary into a computer with a USB-C cable.
2. Open a serial terminal at **115 200 baud** on the new USB-CDC port.
3. Press the **BOOT** button on the device for ~1 second. The terminal
   will print the AP SSID and password.

If you've also lost the BOOT button (it's the small one next to the USB
port), press and hold for **5 seconds** during power-up. The device will
factory-reset, generate a fresh keypair, and print the new credentials
on serial.

---

## A note on what gets recorded

The sensing cards on the dashboard show *live* state — they're for
you, in the moment. A subset of those events also get permanently
recorded into the device's **witness chain**: an Ed25519-signed,
SHA-256 hash-chained log on the SD card that's structurally tamper-
evident.

Five kinds of event are signed:

- **Smoke alarm pattern** (T3 cadence)
- **CO alarm pattern** (T4 cadence)
- **Silent panic** (touch pad long-press)
- **Enclosure tamper** (touch pad disconnect)
- **Thermal drift** (sustained ±5 °C temperature step)

That's it. Five high-stakes things the homeowner would actually want
court-defensible records of. Everything else — CSI motion, IR remote
activity, approach detection — stays as live dashboard state and is
never written to disk.

The signed records contain the same scalars the dashboard already
shows (kind, confidence, time bucket, category). No extra information
crosses the privacy barrier just because the chain is involved.

---

## What you can break

You can't brick the device through the dashboard. You *can* lose your
witness chain by factory-resetting it (the Ed25519 device key is
re-derived from a fresh hardware-RNG seed and the previous chain is no
longer signable). If you've been using the Canary as a witness device
for evidence, **export the chain before you factory-reset** —
**Settings → Export → Witness chain**.

---

## When it doesn't work

| Symptom | Likely cause |
|---|---|
| Phone can't see `SecuraCV-XXXX` | Power LED off → check USB cable. (Safe mode does *not* hide the network — the Wi-Fi AP keeps running.) |
| Dashboard shows a yellow **Safe mode active** bar | The Canary *crashed* (firmware panic / watchdog / brownout) several times in a row and disabled optional peripherals (camera, SD, mesh, BLE, presence, GPS) to protect the core witness functions. Ordinary power cycling, unplugging, or pressing reset does **not** trigger this — only genuine crashes do. It auto-reboots once it has run stably for ~60 s. If a persistent fault keeps crashing it back into safe mode it stops retrying and stays put — fix the underlying issue (reseat the SD card, check power), then click **Retry full boot** on the bar. |
| `canary.local` doesn't load | Some Android browsers don't resolve `.local` names. Use **http://192.168.4.1** instead — it works on every phone. |
| Phone drops the Wi-Fi or warns "no internet" | This shouldn't happen on current firmware (the Canary answers the connectivity check to stay connected). If it does, update the firmware, then reconnect. |
| Sensing pill stays **Offline** | This build was compiled without CSI; check **Settings → About → Build features** |
| **Drop: rate-limit** climbing fast | Strong nearby 2.4 GHz interferer; move the Canary or switch your home Wi-Fi to channel 6 or 11 |
| Gauges look noisy at low signal | Move the Canary closer to other Wi-Fi devices, or away from a metal wall behind it |
| **Acoustic alarms** card says **Mic offline** | The PDM driver failed to start. Check serial output for an `Audio: I2S` error. Usually a hardware issue with the on-board mic. |
| **Acoustic alarms** card says **Mic muted** | You (or someone with dashboard access) muted the mic. Click `Unmute microphone` on the card. The mute persists across reboots — it's stored in NVS as `mic_muted`. |
| **Test the microphone** bar never moves | I2S isn't running. Check the **I2S errors** counter on the same card; check the device serial log for `Audio:` errors. If muted, the bar is intentionally zero. |
| **Listen for 30 s** ends with "No transitions seen" | The mic is reading but no sound crossed the ON threshold. Either the alarm is too far away (move within ~3 m), the alarm is too quiet, or the mic is genuinely failing. |
| **Listen for 30 s** sees transitions but matches nothing | Likely a non-standard alarm cadence, or the room is noisy enough that the inter-beep gaps aren't clean. The Canary only matches T3 (NFPA 72 smoke) and T4 (UL 2034 CO). |
| Smoke alarm beeping but no event fires | Most US/EU alarms use the standard T3 cadence; UK and some older alarms use T4. Check your alarm's manual for cadence type — only T3 (smoke) and T4 (CO) are matched today. |
| **Touch** card stuck at **Calibrating** | The pad never produced a stable reading. Check that nothing is touching the pad during the first 2 s after boot (the baseline is sampled then), and that the GPIO is actually connected to a touch-capable pin (1, 3, 4, 5, or 6). |
| Touch panic fires randomly | Your enclosure or mounting is letting the pad float. Either ground the pad better, raise the press threshold (`TOUCH_RELATIVE_THRESHOLD_PCT` in the lib), or move to a different channel via `-DTOUCH_PIN_NUM=N`. |
| **Power & wake** card always reads **cold_boot** | Normal — this build doesn't actually deep-sleep. Battery / always-off behavior arrives in a follow-up build. |
| **Appliance activity** card stays at **No activity** | Either no IR receiver is wired to GPIO 3 (D2), or no IR remotes are being used in the room. Check the **Frames received** counter on the same card — if it stays at 0 across several remote button presses, the receiver isn't seeing pulses (loose wire, wrong pin, wrong polarity on VCC/GND). |
| **Appliance activity** decodes few frames | Many cheap IR remotes deviate from the ISO timing standards. The lib decodes NEC, RC5, and Sony SIRC — it deliberately rejects ambiguous frames so the dashboard isn't noisy with garbage. |
| **Thermal drift** card never leaves **Calibrating** | The internal temp sensor needs five clean samples (5 minutes by default). If the device just booted, just wait. If it persists past 10 minutes, the sensor may have failed to start — check serial. |
| **Thermal drift** firing constantly | Your room's HVAC is cycling aggressively (5+ °C swings at the device). Tune `drift_threshold_tenths_c` upward in the build, or move the device away from a cold-air register. |
| **Adaptive performance** card often shows **Adaptive** or **Protective pause** | The spot runs warm during streaming — not a fault, but the device would have more headroom with the heat sink fitted, some shade/ventilation, or a lower peek resolution. See the [Thermal Guide](./thermal_guide.md), especially the symptom→action table. |

For anything else, **Settings → Diagnostics → Send to installer** packages
the health log into a signed bundle you can share without leaking
contents of your home.
