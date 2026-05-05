# Setting up your Canary

> A SecuraCV Canary is a small RF-sensing witness device. It watches the
> shape of motion in a room — without a camera, without a microphone,
> without ever learning who you are. This guide gets one going in five
> minutes.

---

## What's in the box

- One SecuraCV Canary (a XIAO ESP32-S3 in a small enclosure)
- A USB-C cable
- A printed card with your device's **AP password** and **Device ID**

If your Canary did not come with a card, the password is also burned into
the device and shown on the serial console at first boot — see *Recovering
the password* at the bottom of this page.

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

> **SecuraCV-A1B2**

The four characters at the end are unique to your Canary. Tap the
network and enter the **AP password** from your card. (The password is
*not* `password` and *not* the device ID — it's the eight-character
string printed under the QR code.)

Once joined, your phone will automatically open the Canary's dashboard.
If it doesn't, open Safari (or any browser) and visit:

> **http://canary.local**

…or, if your network is unusual, the IP address shown in your Wi-Fi
settings (typically `192.168.4.1`).

That's it. You're now talking to the device.

---

## 3 · See what it's sensing

The dashboard opens on the **Status** tab. Tap **Sensing** in the top nav
to see what the radio is feeling, in plain English.

You'll see a coloured pill at the top of the page:

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

When the Canary recognises the pattern, the card turns red and the
status pill reads **🔥 Smoke alarm pattern** or **⚠ CO alarm pattern**.
That same event flows through the witness chain so a Home Assistant
automation can react — for example, send everyone in the house a push
notification *because* the kitchen Canary heard the upstairs smoke
alarm before anyone in the basement noticed.

What this card **does not** do:

- It does not record audio. Ever. The microphone's output is reduced
  to a single loudness number every 20 ms, then the sample buffer is
  zeroed *in place* before the next 20 ms arrives.
- It does not recognise voices, words, or specific sounds beyond the
  two regulatory cadences. Speech is structurally impossible to
  recover from a binary on/off envelope.
- It does not phone home. The match is local.

If your smoke / CO detector uses a non-standard cadence (rare, mostly
older European models), the card will stay quiet — the Canary deliberately
avoids fuzzy matching.

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
  a hand or body within a few centimetres triggers `approach` once
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

## 7 · Power & wake

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

## 8 · What it never does

Three things are **structurally impossible** with this device — not
*hard*, not *configurable off*, but unable to:

1. Identify a person. The radio signal that touches your phone or watch
   never lands in storage. The MAC and BSSID are scrubbed in the same
   callback that receives them; only a 32-byte numeric summary of the
   room's RF shape is kept, and that summary is bucketed to integers
   so device-fingerprinting is information-theoretically impossible.
2. Recognise a face or record audio. There is no camera frame storage
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

If you have Home Assistant on your network, the Canary advertises a
`_securacv._tcp` mDNS service. Add it via **Devices & Services → Add
Integration → SecuraCV**. The Sensing entities — *motion*,
*breathing*, *activity_label*, *channel*, *frames_in_window* — appear
automatically. No cloud, no account.

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
| Phone can't see `SecuraCV-XXXX` | Power LED off → check USB cable; or the device was put into safe-mode by repeated tamper events |
| `canary.local` doesn't load | Some Android versions don't resolve `.local`; use the IP address from your Wi-Fi settings |
| Sensing pill stays **Offline** | This build was compiled without CSI; check **Settings → About → Build features** |
| **Drop: rate-limit** climbing fast | Strong nearby 2.4 GHz interferer; move the Canary or switch your home Wi-Fi to channel 6 or 11 |
| Gauges look noisy at low signal | Move the Canary closer to other Wi-Fi devices, or away from a metal wall behind it |
| **Acoustic alarms** card says **Mic offline** | The PDM driver failed to start. Check serial output for an `Audio: I2S` error. Usually a hardware issue with the on-board mic. |
| Smoke alarm beeping but no event fires | Most US/EU alarms use the standard T3 cadence; UK and some older alarms use T4. Check your alarm's manual for cadence type — only T3 (smoke) and T4 (CO) are matched today. |
| **Touch** card stuck at **Calibrating** | The pad never produced a stable reading. Check that nothing is touching the pad during the first 2 s after boot (the baseline is sampled then), and that the GPIO is actually connected to a touch-capable pin (1, 3, 4, 5, or 6). |
| Touch panic fires randomly | Your enclosure or mounting is letting the pad float. Either ground the pad better, raise the press threshold (`TOUCH_RELATIVE_THRESHOLD_PCT` in the lib), or move to a different channel via `-DTOUCH_PIN_NUM=N`. |
| **Power & wake** card always reads **cold_boot** | Normal — this build doesn't actually deep-sleep. Battery / always-off behavior arrives in a follow-up build. |

For anything else, **Settings → Diagnostics → Send to installer** packages
the health log into a signed bundle you can share without leaking
contents of your home.
