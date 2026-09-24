# Canary Doorbell — the doorbell that cannot become a surveillance camera (research & design dossier)

**Status:** research complete (market, silicon, sensors, power — September 2026, sources in §12);
design. What exists today is one tier of three: the **Vision Doorbell** case
([`enclosure/canary_vision_doorbell.scad`](./enclosure/canary_vision_doorbell.scad), v0.4) is a
released catalog variant wrapping the shipping Vision stack. Everything else here — the Pro
carrier board, its firmware image, the sensor set, the case that holds them — is **a design: no
schematic, no image, no bench unit.** Ladder verdicts are derived from evidence on disk, and this
document adds none — its glossary row carries the non-ladder label *design*.

**The one-sentence version:** a wired, open-hardware, open-firmware doorbell that senses better
than Ring's flagship (a starlight square sensor behind a 150° head-to-toe lens, invisible 940 nm
IR, a tracking radar, and a depth sensor aimed at the doormat), tells the household *what
happened* within a second over the LAN with no cloud and no subscription, proves a package was
placed and later taken in a signed, hash-chained record that names no one — and **cannot** show
anyone a picture, because the code that would was never written.

**The one thing to know first:** SecuraCV will not ship a live view, recorded clips, or a
visitor's voice. That is Invariant I ([`spec/invariants.md`](../../spec/invariants.md)), the FAQ's
["can someone watch a live feed?" — *there isn't one*](../FAQ.md), and the Vision's
["boxes, never pixels"](./canary_vision_getting_started.md) aiming rule. §3 argues that refusal
once, with revisit triggers. §4 is the product that refusal makes possible: the doorbell wins the
jobs a doorbell actually does when the surveillance is taken away, and it wins them *because* it
is a witness.

---

## 1 · The market, September 2026 — what they sell, what they gate, where they broke

The category's best sellers, from the [market brief](#12--sources) (every figure cited there):

| Model (year) | Price | Video | Night | Package | Second sensor | Local, no fee? |
|---|---|---|---|---|---|---|
| Ring Battery Doorbell 4K Pro (2026) / Wired Pro 3rd gen (2025) | $249.99 | 2880² 1:1, 140°×140° | color "Low-Light Sight"; reviewers: 4K "noticeably softer" at night | **paid** (Basic $4.99/mo) | radar "3D Motion / Bird's Eye" (history behind a plan) | no — no plan, no recording; cloud outages Oct 2025, Feb 2026 |
| Eufy E340 (2023) | $179.99 (street $120) | 2K + **1080p downward package cam** | spotlight + IR "dual light" | free, on-device | PIR | local eMMC, cloud-authenticated app; 2022 "local-only" streams were not encrypted |
| Wyze Duo Cam (2024) | $89.99 | 2K + 1080p downward | color | **paid** (Cam Plus, annual +50 % in 2026) | PIR | microSD; 2024 breach showed 13,000 users strangers' thumbnails |
| Google Nest Doorbell wired 3rd gen (2025) | $179.99 | "2K" 3:4, 166° | color | free, on-device, 3 h history | PIR | no — $10–20/mo for history; HA needs a Google Cloud project |
| Reolink Video Doorbell PoE / WiFi | ~$110 | 5MP 4:3, 134°×97° | IR | free | PIR | **yes** — RTSP/ONVIF; but 97° vertical misses the doormat inside ~1 m, and ~8 s to a motion notification |
| TP-Link Tapo D260 Pro (ships Sept 30 2026) | $219.99 | **4K**, 180° diag | color | free + face | **radar + PIR** | microSD, no fee; closed firmware, RTSP unconfirmed |
| UniFi G6 Pro Entry (late 2025) | ~$400 | 12MP 1/1.6" + 8MP package cam | IR 5 m | free | — | local, but a UniFi console, IPX5, PoE+ |
| Aqara G410 (2025) | $129.99 | 3MP 4:3, 176° | ten **940 nm** IR | free + face | **mmWave** | mostly; RTSP only when wired |
| Lorex B862 4K | ~$220 | 8MP **portrait 9:16**, 150° | color + IR | person/vehicle | — | microSD, no fee |

Three things the table says. **First, the spec race is over.** 4K, 1:1 or 3:4 head-to-toe,
color night, radar, a package camera and free on-device detection are each available from someone
under $250 — TP-Link's D260 has most of them in one box. Beating the number on the label is not a
product. **Second, every incumbent monetizes the recording**, and the ones that do not (Reolink,
Tapo, Lorex, UniFi) sell the *footage* as the product — RTSP, microSD, an NVR. **Third, the trust
record is the opening.** Ring's Familiar Faces (a 50-person catalog that scans every passer-by)
drew an EFF legal analysis and a federal suit; Search Party scans a whole neighborhood's devices;
police "Community Requests" returned in 2025 through Axon, alongside a Flock Safety deal. Wyze's
breach was itself triggered by an AWS outage; Eufy's "local" cameras uploaded thumbnails; Ring's
live view fell with `us-east-1`. Nobody sells a doorbell whose no-upload claim is *checkable*,
and nobody sells one that is honest to the person standing in front of it.

**The gaps, as the brief ranks them** (§5 of the market brief): a verifiable no-upload
guarantee; privacy toward visitors and passers-by; evidence-grade package-theft proof that is
not a 1080p clip; cloud-outage immunity for the *chime and the notification*, not just the
recording; a doorbell press that is a first-class local event (Tapo has none over its API, Nest
needs a Google Cloud project); and cold-climate honesty. Every one of those is a witness's home
turf.

**Open hardware:** none. No consumer doorbell ships open firmware; the "open" field is ESP32-CAM
hobby builds (MJPEG, poor night), one Crowd Supply campaign on the same ESP32-CAM ceiling, and
Wyze doorbells re-flashed with thingino on their Ingenic T31 — hackable, not designed. The only
open-*hardware* AI camera core in the world is Seeed's reCamera (SG2002, Apache-2.0, KiCad
sources); its 4K Pro sibling's sources are "pending". That is the field we are entering.

---

## 2 · The bar — feature by feature, and where a witness can clear it

| Feature | Best in class today | What the Doorbell does | Verdict |
|---|---|---|---|
| Pixels on a package on the doormat | Ring 4K Pro: 2880² at 140° ≈ 20.6 px/° → a shoebox at 0.5 m ≈ 270×165 px; Eufy's dedicated 1080p down-cam is worse | 5MP 4:3 sensor cropped **1944² at 150°** = 13 px/° → ≈ 170×105 px; the 4K tier (2160²) = 14.4 px/° | **beats every dual-cam and 2K unit; Ring's 4K keeps the pixel crown, at night it does not** (§5.1) |
| Night | Eufy dual light (spotlight + IR); Aqara 940 nm invisible IR; Ring sensor-only color | 2.0 µm STARVIS class (2.9 µm option), F1.6 f-theta, **940 nm** invisible IR *and* a warm porch LED on approach | **matches or beats**; the porch light is a courtesy, not a deterrent floodlight |
| Approach sensing | Ring radar 3D Motion (history paid); Tapo D260 radar; Aqara mmWave | 24 GHz FMCW tracker (x/y, 3 targets, 8 m): approach vector, dwell, sidewalk rejection — **the map is never stored, only the events** | **beats** (free, local) |
| Package left / taken | Eufy "Delivery Guard" flow, the most-praised implementation | `Package` class + a **depth blob on the doormat** (8×8 ToF) → placed / present / removed, radar-corroborated, **signed and chained** (§4.1) | **beats** — it works in the dark and it is evidence |
| Latency | Nest "loads almost instantly"; Reolink 7.9 s to a motion push | button → local MQTT / fleet beacon → Dash, nightstand, phone: **sub-second on the LAN, WAN unplugged** | **beats** |
| Live view / two-way talk | everyone | **none inbound** — an outbound speaker for quick replies rendered on the hub (§4.3) | **refused, by design** (§3) |
| Face recognition | Ring, Eufy, Aqara, Tapo | never — `ObjectClass` has no `Face` | **refused** (Invariant II) |
| Power | Reolink PoE: 802.3af *or* 12–24 VAC *or* 24 VDC; Ring 4K wants 30–40 VA | 8–24 VAC (chime relay + built-in bypass), 24 VDC, 802.3af option, USB-C for the bench; ~2.5 W typical | **matches Reolink**, runs on a 10 VA transformer the 4K Ring cannot |
| Cold | all battery units stop charging below 0 °C | wired, no battery, −20 … 50 °C parts ([cold-weather envelope](./cold_weather_envelope.md)) | **honest** |
| Weather | Tapo D260 IP66; most IP65; UniFi IPX4–5 | CER-2 sealed build (~IP54 as printed; [field ratings](./enclosure/field_ratings.md)); the button IP65–67 | behind on the sticker, honest on the test |
| Subscription | $30–200 / yr | $0, structurally — there is nothing to subscribe to | **beats** |
| Openness | none | case (OpenSCAD), carrier (KiCad), firmware image, evidence format, disclosure text | **beats** |
| Price | $90 (Wyze Duo) – $250 (Ring 4K) + fees; Tapo D260 $220 | Lite ≈ $60 parts today; Pro ≈ $150 at qty-1, ≈ $70 at 1 000 (§10) | qty-1 parity, volume win, and $0/yr forever |

---

## 3 · The refusals — what this doorbell will never do, and why that is the product

Argued once, in the style of the [Apple Home RFC's HKSV verdict](../design/apple_home_integration.md):
each refusal names the invariant, what replaces it, and the trigger that would reopen the question.

| Ring feature | Verdict | Why (invariant) | What we do instead | Revisit trigger |
|---|---|---|---|---|
| **Live view** | never, on a Canary | Invariant I: the kernel exposes no API that streams raw media; the Vision's firmware *cannot* export a frame and the aim card ships boxes | the ring, the approach, the dwell and the package arrive as events in under a second; the Dash shows *what*, never *who* | none for a Canary. An "answer the door" intercom would be a **different device class**, not a witness, and would need its own RFC arguing why a visitor's face on a phone is not surveillance — this dossier does not make that argument |
| **Two-way talk (inbound)** | never | Invariant I (audio waveforms are raw media) and II (nothing witnessed becomes text); the Canary mic, where fitted, hears only a loudness envelope | **outbound only**: the household answers with quick replies the hub renders locally (§4.3) | same as above |
| **Recorded clips, 24/7, pre-roll** | never | Invariant I | the sealed log; break-glass to the vault with n-of-m trustees for the frames (§4.2) | none |
| **Familiar Faces / any identity** | never | Invariant II — a rejected PR, not a setting | `Person`, `Vehicle`, `Animal`, `Package` | none |
| **Neighborhood footage sharing, police portal** | never | Invariants I, IV; the Chirp channel's contract ([`spec/chirp_channel_v0.md`](../../spec/chirp_channel_v0.md)) | Chirp: human-triggered, text-only, ephemeral identity, no history | none |
| **Cloud account, telemetry** | never | Invariant IV | local MQTT, the [alert relay](../design/alert_relay.md) for away pokes, the owner's own iCloud for the iPhone wake | none |

The refusals are not a cost we absorb. They are why the visitor can trust the light on the
button (§4.4), why a package report can be handed to a carrier without exposing the street
(§4.1), and why a court has a reason to believe the record (§4.2). The market brief's own
conclusion: the incumbents' margin lives in the subscription, and the subscription lives in the
recording. Refuse the recording and the whole business model falls off the device.

---

## 4 · The jobs a doorbell does — and how a witness does each better

### 4.1 Package custody without footage (the killer feature)

Porch piracy is the reason people buy a doorbell camera, and the incumbents answer it with a
1080p clip that shows a hooded stranger. A witness answers it with a **custody chain**:

```
14:10  SmallObjectBoundaryCrossing(Package)   camera: package class at the doormat crop
14:10  ContactStateChange(doorbell)           the carrier rang
14:10  doormat depth: OCCUPIED (blob 0.31 m²)  ToF: something is on the mat
  …    doormat depth: OCCUPIED                 (heartbeat, coarsened to 10-min buckets)
16:20  LargeObjectBoundaryCrossing(Person)     radar approach from the sidewalk, no ring
16:20  ObjectRemovedFromZone(Package)          ToF: doormat EMPTY; camera: package class gone
```

Every line is an existing claim kind — **no new vocabulary**
([`spec/witness_dictionary.json`](../../spec/witness_dictionary.json): `object_removed_from_zone`
already exists, with the package icon). Each is signed and hash-chained; the whole sequence
exports as a self-verifying evidence envelope ([evidence lifecycle](../evidence_lifecycle.md))
that a carrier, an insurer or the police can check without our tools and that **names no one and
shows no street**. Time is bucketed to 10 minutes (Invariant III) — coarse enough to protect the
household's routine, fine enough to bracket a theft.

Why the depth sensor: a package is a *small* object on a *known* plane. An 8×8 time-of-flight
array aimed down at the doormat sees a step change in the floor plane and needs no light, no
model and no pixels — it works in the dark and in rain, and it is a physically independent
channel from the camera, which is exactly the corroboration idiom Sentinel and the
[Combo case](./enclosure/canary_combo.scad) are built on. Its honest limit: ST rates the VL53L8CX
to ~2.8 m at 5 klux and much less in direct sun, so the mat must sit within ~1 m and preferably in
the doorbell's own shadow; the camera's doormat crop is the fallback channel when the sun is on
the mat (§11, item 3).

### 4.2 Evidence when it matters — 4K starlight frames that only a quorum can open

"Verified" in this project means an Ed25519 signature checked against a pinned key, and a
break-glass export needs n-of-m trustees ([`spec/break_glass.md`](../../spec/break_glass.md)).
The Doorbell's one *new* mechanism is to give that vault something worth opening: on a
qualifying event (a removal after a placement, a tamper, a dwell past the threshold), the device
seals the current full-resolution frame **encrypted at capture** — a per-object DEK wrapped to
the trustee quorum, the v2 vault envelope the kernel already defines — onto its own eMMC. The
device holds no unwrap key; neither does the household. Nothing on the device can display it. The
pixels the incumbents sell as a live feed exist here only as ciphertext, and only for the frames
around the events that earned them.

This is the honest reason to want a good sensor in a witness: not to look, but so that the frame
your trustees can unseal after a break-in is starlight-clean and not the "noticeably softer"
night image Ring's 4K reviewers describe. It is also a spec change: today sealing happens on the
kernel, and the sensor adapter contract has no image field *by construction*. The on-device vault
needs the spec's blessing before a line of it is written (§11, item 1). Until then the Doorbell is
a coarse-claim source like every other Canary, and that alone delivers §4.1.

### 4.3 Answering without a feed

What a household actually needs when the bell rings and nobody is home: to know it rang, to know
whether a package is there, and to say one thing to the visitor. The first two are events. The
third is **outbound audio only**: the hub renders a quick reply with its local Piper voice (the
[voice recipe](../voice_control.md) already runs it) and the Doorbell plays it through an IP67
speaker — "leave it by the bench", "we'll be right there", "no soliciting". The device stores the
rendered clips; the hub sends `say {clip}` over the same local MQTT command idiom Herald's
`message/set` uses. No microphone path exists in the Pro's default build (§5.6). Ring calls this
"Quick Replies" and sells it in a plan; ours is the only kind of talk a witness can do, and it
costs nothing.

### 4.4 The glow ring — honest to the visitor

Every doorbell lights its button. Ours makes the light *mean* something, and never lie:

- **breathing** — the witness is on; presence sensing in use, nothing recorded;
- **a single swell** on the press — "the house heard you" (the event has been sealed, not
  merely sent);
- **steady, dimmer** while a reply plays;
- **off** when the household has muted the witness — and the IR emitters and the porch LED are
  on the same rail, so a dark ring is a dark sensor, not a decoration.

The palette is the house colorway (warm white, canary yellow); the ring never flashes and is
never red or blue — a doorbell must not look like an emergency vehicle, and the
[no-impersonation contract](../research/harm_reduction_prior_art.md) applies to lights as well as
tones. The face carries the [witness signage](./enclosure/canary_sign.scad) lines — *presence
sensing in use · no video is recorded or stored* — and, unlike a Ring sticker, the sentence is
true by construction. The ring is also the one place the ambient-display rule applies at the door:
**silence is never rendered as safety** — a device that has lost the hub breathes differently,
so an empty log is visibly "unsure", not "quiet".

### 4.5 A ring that arrives in under a second, with the internet unplugged

The press is a `ContactStateChange` on the wire name `doorbell` (the Dash already renders it at
an honest severity — see the [competitor landscape](../research/competitor_app_landscape.md)).
It rides local MQTT to the hub and the BLE fleet beacon to the phone in the same room; the Dash,
the nightstand and the Nightlight react; the mechanical chime strikes through the on-board relay;
the alert relay pokes an away phone with one coarse word. None of it crosses the WAN. The two
Ring outages of the last year took down live view *and* the ring notification; ours cannot,
because there is no server in the path to fall over.

### 4.6 Approach, dwell and the visitor who did not ring

The radar sees a person at 8 m, before the camera has enough pixels to say anything, and tracks
the approach vector: from the sidewalk, from the driveway, straight to the door. That gives three
events the camera alone gets wrong: **approach** (`LargeObjectBoundaryCrossing` at a virtual
line across the walk — sidewalk traffic never crosses it), **dwell** (`PresenceInRestrictedZone`
after N seconds at the door with no press — the "someone stood at my door and left" event the
incumbents hide behind a plan), and the **departure** that closes a visit. The radar also wakes
the camera and the IR: at night the sensor pipeline idles until something approaches, which is
the power story in §8.

---

## 5 · Sensing architecture

### 5.1 The camera — the 4K question, answered honestly

The brief's arithmetic (§5 of the landscape brief): at Ring's 48 in mounting height, seeing a
tall visitor's head at 0.5 m *and* the doormat at 0.4 m needs about **±72° vertical**. That is why
Ring went 1:1 at 150°/140° and why Reolink's 4:3 at 97° vertical cannot see a package closer than
a meter. No rectilinear lens does 150°; these are **f-theta fisheyes**, and on an f-theta lens the
pixel density is uniform in degrees, so the only number that matters for the doormat is
**pixels per degree** = crop height ÷ field. 

| Sensor | Format / pixel | Square crop | px/° at 150° | Lanes | Night | Notes |
|---|---|---|---|---|---|---|
| **Sony IMX335** (5MP 4:3) | 1/2.8", 2.0 µm | **1944²** | **13.0** | 2 OK at ≤30 fps | STARVIS, ~0.2 lx class | native 4:3 wastes no pixels; $15–40 as an M12 module |
| SmartSens **SC450AI** (4MP) | 1/1.8", **2.9 µm** | 1440² | 9.6 | 2 | "black light" full-color night | the darkest-porch option, fewer pixels |
| Sony IMX415 (4K) | 1/2.8", **1.45 µm** | 2160² | 14.4 | **4** | ~0.23 lx — *worse* than the IMX335 per pixel | the cheap 4K: more pixels, each worth less |
| Sony **IMX678** (4K, STARVIS 2) | 1/1.8", 2.0 µm | 2160² | 14.4 | **4** | ~0.13 lx, half the light of an IMX415 | the real "4K starlight"; needs a 4-lane SoC |
| Ring 4K Pro (reference) | ? | 2880² at 140° | 20.6 | — | reviewers: soft at night | — |

So: **"4K" on a 1/2.8" sensor is a night downgrade**, and "4K starlight" costs a 4-lane MIPI
port, which means the RV1126B class of SoC (§6.1) — the reCamera Pro's silicon, $300 today. The
design therefore has two camera tiers on **one lens mount and one face**:

- **Pro (the build): IMX335, 1944² square crop, 2-lane.** 13 px/° beats every dual-cam and 2K
  incumbent on the doormat (a shoebox at 0.5 m ≈ 170×105 px; a classifier is comfortable above
  64 px) and sits within 10 % of a 4K square crop, with a night floor the 4K IMX415 cannot reach.
  The 1944² crop from a 4:3 sensor throws away 25 % of the width and nothing of the height; a 16:9
  4K sensor cropped square wastes 44 %.
- **Pro 4K (the option): IMX678 or the reCamera Pro's SC850SL, 2160², 4-lane**, on the RV1126B
  compute island, when an open board for it exists. Same case, same lens family, same face.

**Lens.** f-theta, f ≈ r/θ. For the IMX335's 1944² crop (r = 1.94 mm) and 150° across the square,
f ≈ 1.5 mm; the commodity M12 fisheye catalogs run 1.4–1.7 mm (185° on 1/1.8"), $18–46. Pick for
the **image circle** (it must cover the 1944² crop's 5.5 mm diagonal without vignetting) and
for MTF at 70° off-axis, because that is where the package sits (§11, item 4). Aperture F1.6 or
faster. An **IR-cut switcher** (ICR) on the M12 thread, $5–15, gives true color by day and full
IR sensitivity at night — the Lorex/Eufy approach; sensor-only color night (Ring's "Low-Light
Sight") is what the SC450AI option buys.

**Detection on the fisheye.** Run the detector on the raw f-theta image (uniform px/° is a
feature, not a bug) at a 640² downscale for the whole field, plus the **doormat crop at native
resolution** for the package classifier — the "digital second camera" that replaces Eufy's
hardware one. Dewarp nothing: there is no viewer to dewarp for. The SG2002 and RV1106 both have
hardware lens-distortion correction if a future model wants it.

### 5.2 Illumination

- **Invisible IR: 4× ams-OSRAM SFH 4725AS (940 nm, 80°, $3–4 each)**, one pair flanking the lens
  and one pair aimed down at the mat, driven pulsed in sync with the exposure at ~300 mA total
  (~1 W average; four of them at their 1 A rating would be 13 W — never continuously). 940 nm
  shows no red glow at all; a STARVIS-class sensor gives up some reach against 850 nm, which the
  aimed pair and the short doormat distance pay back. The Aqara G410 ships ten 940 nm emitters
  for the same reason.
- **Porch LED: 2× warm-white 3 W-class**, radar-triggered on approach, on a dimming curve. It is
  a courtesy light for the visitor and the reason night detections are in color when someone is
  actually at the door. It is not a floodlight and never strobes.
- **The glow ring: a 12-pixel SK6812 ring** (5 V, −20 °C rated part to be confirmed, §11 item 8)
  behind a 2 mm frosted PMMA diffuser around the button.

### 5.3 Radar — 24 GHz FMCW tracking

**Hi-Link HLK-LD2450** ($12): 24 GHz, 1T2R, 8 m, ±60° azimuth, up to three targets with x/y and
speed. It is the cheapest reproduction of Ring's radar path-tracking, and the Ranger and Paw
dossiers already argue FMCW over Doppler for the same reason: a person standing still at the door
is the event. Alternate: the Sense's **MR60BHA2** (60 GHz, $25) — already in the fleet, worse
angular coverage for this job. Buy a module carrying its own FCC grant (a radar is an intentional
radiator under its own rule part); the doorbell's Wi-Fi module must be a modular-approved part for
the same reason (§11, item 7). The radome window follows the Sense's rules: a flat, thin,
unpainted section of the face, no metal within the beam.

### 5.4 Depth — the doormat sensor

**ST VL53L8CX** ($9): 8×8 zones, 65° diagonal, SPI, aimed ~35° down-and-out through a small
window on the body's bottom chamfer so the mat fills the array at 0.4–0.8 m. It reports a
per-zone range; the firmware keeps a calibrated **empty-mat plane** and calls OCCUPIED on a
sustained multi-zone step of ≥ 5 cm. That is the whole package sensor: no pixels, no model,
works in the dark. The VL53L5CX is the same part with a slower interface and a lower budget.

### 5.5 Button and tamper

A **16 mm anti-vandal momentary, IP67, with no LED of its own** (the ring does the lighting) —
the Langir / DAIER / CDOE class at $3–6; piezo or capacitive variants have no moving seal, which
matters for icing, and mechanical gives gloved tactile feedback. Both are `ContactStateChange`.
Tamper: an **LIS3DH** (the Gatekeeper part, $2) for pry-off and hammering, plus the plate's
security-screw continuity on the existing blind boss — `TamperDetected`, no new vocabulary.

### 5.6 Audio

**Out:** MAX98357A I²S amplifier ($3–6) into an IP67 driver — Same Sky CMS-15113-078SP-67
(0.7 W, $5) for the quick replies and a chime tone, or a Visaton K64WP if the doorbell *is* the
chime. **In: none, by default.** The Pro carrier has a footprint for the WAP's envelope-only
PDM microphone, **do-not-populate**, for owners who want the `knock` and `glass_break` acoustic
events; the firmware path behind it is the same loudness-envelope code the WAP and Vision run,
structurally unable to carry speech. A doorbell with a button and a radar does not need a
microphone to know someone is there.

---

## 6 · Compute — the platform, the `can't` image, the detector

### 6.1 The platform choice

| SoC | NPU | Max sensor | MIPI | Open? | Price | Verdict |
|---|---|---|---|---|---|---|
| **Rockchip RV1106G3** (Luckfox Core1106 SoM, castellated, Wi-Fi 6 + BT on-module) | 1 TOPS (one listing says 0.5 — §11 item 2) | 5MP@30 | 2-lane | schematics public; Rockchip BSP + RKNN; an OpenIPC image exists | SoM $27–49 | **the Pro's reference core**: RKNN is the best-trodden NPU toolchain (Frigate runs on it), the SoM solders to a carrier, and a community image already boots it |
| **Sophgo SG2002** (Seeed reCamera core) | 1 TOPS | 5MP@30 | 2-lane | **Apache-2.0 hardware, KiCad**; reCamera-OS; partial mainline RISC-V | core $35–55; Milk-V Duo 256M $8 | **the alternate core, and the openness benchmark**: the only open-hardware AI camera; the carrier is designed so either core can be the compute island |
| **Rockchip RV1126B** (reCamera Pro) | 3 TOPS | 4K30, AI-ISP | 4-lane | BSP; reCamera Pro sources "pending" | $300 as a product; SoMs from Firefly/Boardcon, price unpublished | **the Pro 4K island**, when an open board exists |
| SigmaStar SSC378QE | 1 TOPS | 4K25 | 2×2-lane | vendor SDK under NDA-style terms; OpenIPC supports the family | ~$10 chip | 4K cheaply, but a `can't` needs an auditable image; declined until the SDK is open |
| Ingenic T41 | 1.2 TOPS | 4K30 | — | thingino "experimental" (2026) | modules $9–19 | watch; the Wyze-reflash community lives here |
| ESP32-P4 | none | 1080p, linear ISP | 2-lane | fully open IDF | $15–40 | not a doorbell camera chip; fine for the Lite's *host* role |
| Himax HX6538 (Grove Vision AI V2) | tiny | low-res always-on | — | shipping in the Vision | $16 | **the Lite tier** — what the released case holds today |

### 6.2 The image — a `can't`, not a `won't`

The design rule behind everything is that the surveillance code was never written. On a Linux
camera SoC that has to be *made* true, and it is a build-time property, auditable from the
Buildroot config:

- **no video encoder**: the H.264/H.265 VENC driver is not built into the kernel image — a
  stream cannot exist because nothing on the device can produce one;
- **no RTSP, no HTTP media endpoint, no snapshot URL**: the userland has one network client,
  the MQTT/witness client, and no server that answers for pixels;
- **ISP → NPU only**: frames land in a RAM ring the detector reads and the kernel zeroizes on
  eviction (Invariant I's pre-event buffer rules, verbatim);
- **the vault is write-only**: the one path from the ring to persistent storage is the sealed
  envelope of §4.2, ciphertext the device cannot read back;
- **no shell over the network** in the released image; the console is the USB port behind the
  security screw.

The reCamera Pro is treated today as an *external* sensor at the trust boundary precisely
because its OS has a media plane ([Vision Pro guide](./canary_vision_pro_recamera.md)). The
Doorbell Pro is a Canary because its image has none. That difference is the whole reason to
build our own image rather than point a reCamera at the webhook adapter.

### 6.3 The detector

A YOLO-nano-class model with exactly four classes — `Person`, `Vehicle`, `Animal`, `Package` —
plus `Unknown`, compiled with RKNN (or TPU-MLIR on the SG2002), on a 640² downscale of the
square field at ~10 fps, plus the doormat crop at native resolution when the ToF or the radar
asks. Model weights ship signed like firmware. The `ReprocessGuard` rule holds: a new model
cannot be applied to old data, because there is no old data.

---

## 7 · Events, transport, integrations — nothing new to invent

| Doorbell moment | Claim kind (existing) | Modality | Corroboration |
|---|---|---|---|
| button press | `ContactStateChange` (wire name `doorbell`) | contact | — |
| approach across the walk line | `LargeObjectBoundaryCrossing` | radar, camera | both, or radar alone at night before the camera wakes |
| standing at the door, no press | `PresenceInRestrictedZone` (dwell ≥ N s) | radar + camera | required from both |
| package placed | `SmallObjectBoundaryCrossing(Package)` | camera + ToF | both |
| package removed | `ObjectRemovedFromZone` | ToF + camera | both |
| pry / hammer / plate screw | `TamperDetected` | other (IMU, contact) | — |
| knock (opt-in mic) | `AcousticImpulseInZone` | other | envelope only |

Transport is the fleet's: local MQTT to the hub and Home Assistant, the BLE fleet beacon to the
phone, the alert relay's one-word poke when away. **Apple Home** gets the doorbell as the sensor
accessories the [Apple Home RFC](../design/apple_home_integration.md) already projects
(motion / occupancy / contact / tamper) plus — the one addition worth a dictionary row — HAP's
*Doorbell* service, which is a programmable-switch **event**, not a camera; that lets a HomePod
chime and the house lights answer the door with no video lane, the exact split the RFC argued
(§11, item 6). **Chirp** carries the neighborhood half: a human-triggered, text-only soft alert,
never an image.

---

## 8 · Power

**Wired first.** The Pro is a wired doorbell on purpose: every battery incumbent stops charging
below 0 °C and loses a third of its capacity by −20 °C ([cold-weather envelope](./cold_weather_envelope.md)),
and a radar plus a starlight sensor is not a six-month-battery load. Inputs, all on one carrier:

- **8–24 VAC doorbell transformer** (the installed base): bridge rectifier → 100 V bulk → buck.
  The doorbell sits in series with the chime, so the carrier includes the **chime relay** (the
  Frenck pattern: a relay across the chime terminals strikes it on demand) and a **bypass
  jumper** that does what Ring's "Pro Power Kit" does for electronic chimes that will not pass
  standby current. A **16 V / 30 VA** transformer is the recommended upgrade; the Pro's ~2.5 W
  typical draw runs on the 10 VA units the 4K Ring (30–40 VA) cannot.
- **24 VDC**, same input.
- **802.3af PoE** as an optional PD daughter (12.95 W at the PD, plenty) for the PoE wall the
  UniFi and Reolink users already have.
- **USB-C 5 V** for the bench and the flasher.
- **Supercapacitor ride-through** (2× 10 F) so a chime strike or a brown-out on a shared
  transformer never resets the SoC mid-seal.

Budget (the brief's estimates, §8 of the landscape brief): core 1–1.5 W, sensor 0.4 W, Wi-Fi
0.5–1 W, radar 0.5 W, ToF 0.3 W, IR 1 W average when lit, ring 0.3 W → **~2.5 W typical, ~4.5 W
peak**. The radar-wakes-camera fusion is the power story as much as the detection story: at
night the sensor, ISP and IR idle until the radar sees an approach.

**A battery flavor** is a later decision, with the honest answer already written: LiFePO4 with a
temperature-gated charger and the cold-weather envelope's autonomy math, or none. The Lite tier's
XIAO host can already run from the Vision BOM's protected pack for a doorbell that has no wire
at all.

---

## 9 · Enclosure — evolving the v0.4 case, not replacing it

The released [Vision Doorbell case](./enclosure/canary_vision_doorbell.scad) already solves the
doorbell's mounting problems the incumbents solved: a thin **wall plate** with two T-studs and a
printable 5–15° **wedge**, a body that drops on and locks with a hidden **security screw** driven
up through the plate's foot, the **rear cable oval** into the wall, a TPU gasket with a drip-edge
face, a GORE vent and a weep, the 12 mm pill radius the [design language](../design/DESIGN_LANGUAGE.md)
records as deliberate. The Pro body keeps every one of those and changes the face:

- a **lens boss** for an M12 holder with ICR (the OV5647's square Pi-cam holder goes), a 14 mm
  optical-PMMA disc in the same recessed seat (PMMA does not yellow; PC does in a year or two
  uncoated);
- an **IR window ring** of IR-pass PMMA around the lens for the two flanking emitters, and a
  second pair behind the bottom chamfer with the **ToF window**, both aimed at the mat;
- a **radome** section beside the lens on the same face, flat and unpainted, by the Sense's
  rules — the Combo case's "lens on one column, radome on the other" layout, narrowed;
- the **glow-ring diffuser** as a frosted PMMA annulus around a 16 mm button seat (the 12 mm
  seat remains a preset for the Lite);
- a **speaker grille** under the drip edge, the port sealed with a hydrophobic mesh;
- the plate gains **transformer terminals** (two screw terminals in the plate's foot, wired
  into the body through the existing cable well) beside the USB-C exit.

Envelope target: about **128 × 46 × 26 mm** — between Ring's Pro 2 (114 × 49 × 22) and the Aqara
G410 (141 × 65 × 30); a number for the CAD step to *measure*, not a claim. The case stays
parametric on the manifest's knobs (`cad.params`, [`devices/README.md`](../../devices/README.md)),
prints in PETG/ASA, and rates honestly: **CER-2** sealed as printed, with the field-ratings home
tests to prove it per unit, against the incumbents' IP65 stickers. The Pro is a new manifest
(`devices/canary-doorbell-pro/device.json`) owning its own case file; the Lite stays owned by the
Vision through `cad.also`, as it is today.

---

## 10 · Tiers, BOM and price

Indicative qty-1 hobby prices from the two briefs (September 2026); the [BOM pipeline](./bom_pipeline.md)
is where these become distributor-verified rows.

| Tier | What it is | Parts (qty-1) | At 1 000 (est.) |
|---|---|---|---|
| **Lite — the Vision Doorbell, today** | the released case + XIAO S3 + Grove Vision AI V2 + OV5647 (NoIR variant) + 2× 850 nm emitter + 12 mm lit button; **add** an LD2410 ($3) for approach, a VL53L8CX ($9) for the mat, and a chime-relay power module ($6); night is IR grayscale on the module's small NPU | **≈ $60** | ≈ $45 |
| **Pro — this design** | Core1106 SoM $35 · IMX335 M12 module $25 · 1.5 mm f-theta lens + ICR $12 · LD2450 $12 · VL53L8CX $9 · 4× SFH 4725AS + 2× white LED $15 · SK6812 ring + 16 mm IP67 button $7 · MAX98357A + IP67 speaker $8 · LIS3DH $2 · power block (bridge, bulk, buck, relay, supercaps) $8 · 4-layer carrier + passives + connectors $12 · case, gasket, vent, PMMA, screws $6 | **≈ $150** | **≈ $70** |
| **Pro 4K — the option** | the Pro with an RV1126B compute island and an IMX678 / SC850SL 4-lane module | ≈ $250 (SoM price unpublished; the reCamera Pro is $300 as a product) | ≈ $120 |
| PoE daughter | 802.3af PD module | +$8 | +$5 |

**Against the shelf:** Ring's 4K is $249.99 plus $50–200 a year; Tapo's D260 is $219.99; Eufy's
dual-cam E340 is $180; Wyze's Duo Cam is $90 plus a plan for the detection that makes it a
doorbell. The Lite undercuts all of them today. The Pro at qty-1 is price parity with the
flagships and $0 forever after; at volume it undercuts them by half, which is the brief's own
finding — the silicon in a 4K dual-cam radar doorbell is $45–70, and the rest of the $250 is the
subscription business. **The honest claim is not "cheaper"; it is "the same money, once, for a
device that cannot be turned against you."**

---

## 11 · Bench plan & open items

Numbered, so the follow-ups can cite them.

1. **The on-device vault is a spec decision, not a firmware feature.** Sealing raw frames on a
   Canary, encrypted to the quorum, extends [`spec/break_glass.md`](../../spec/break_glass.md)
   and the sensor adapter contract's "no image field" rule. It needs an RFC and the maintainer's
   yes before a line of it exists; the Doorbell ships §4.1 without it.
2. **RV1106G3 NPU rating** — 1 TOPS (Luckfox, CNX) vs 0.5 TOPS (one Amazon listing). Pull the
   datasheet revision for the exact die before choosing over the SG2002.
3. **ToF on a sunlit mat** — bench the VL53L8CX at 0.5 m against a 30 × 20 cm box at noon; if it
   fails in direct sun, the camera's doormat crop is the sole daytime channel and the doc says so.
4. **Lens image circle and off-axis MTF** — three candidate 1.4–1.7 mm f-theta M12 lenses on the
   IMX335 crop; the package sits at 70° off-axis, where cheap fisheyes fall apart.
5. **940 nm reach** on the IMX335 vs the SC450AI at the mat and at 3 m; decide whether the
   flanking pair needs 850 nm (a faint red glow) to reach the walk.
6. **HAP Doorbell service** as a `homekit_projection` row — an event, not a camera; the Apple
   Home RFC's phasing table decides when.
7. **FCC grants** — the LD2450 module's own grant, the Core1106's Wi-Fi modular approval, and
   Part 15B host testing; UL/IEC 62368-1 for the listing; the transformer circuit is Class 2.
8. **The SK6812 ring at −20 °C** — confirm the part's rated range, or pick a discrete-LED ring.
9. **Chime compatibility matrix** — mechanical, electronic, none; standby current with and
   without the bypass; the 10 VA case shared with a chime.
10. **Buck EMI into the radar** — layout the switcher away from the radome, bench with a
    spectrum analyzer at 24 GHz harmonics.
11. **The `can't` audit** — a script that diffs the released image's Buildroot config against
    the §6.2 list (no VENC, no RTSP, one network client) and fails CI on drift, the way
    `lint_dictionary_sync.py` fails on vocabulary drift.
12. **Naming and the figure** — the Pro needs its own `device.json`, a figure in the ledger and a
    Lab card; its confidence stays *idea* (a dashed ghost) until STLs are committed, by
    construction.

**Bench sequence:** (a) the Lite's three add-ons on the released case — radar, ToF and the chime
relay on the XIAO's spare pins — proves §4.1, §4.5 and §4.6 with shipping firmware and zero new
silicon; (b) a Core1106 on a Luckfox carrier with an IMX335 and the §6.2 image, boxes-only, on the
bench; (c) the Pro carrier.

---

## 12 · Sources

Two research briefs gathered for this dossier (September 24, 2026; ~400 searches; many vendor
domains were unreachable from the research sandbox, so figures are cited from indexed excerpts
and flagged where sources conflict):

- **Market:** Ring's 2024–2026 lineup, plans and the Familiar Faces / Search Party / Axon record
  (Tom's Guide, security.org, EFF, CNBC, ring.com support pages); Wyze v2 / Duo Cam / Battery
  Doorbell and the 2024 breach (TechHive, PCWorld, Washington Post); Eufy E340 / S4 and the 2022
  encryption episode (eufy.com, 9to5google, The Verge via gHacks); Nest 3rd gen and Google Home
  Premium (9to5google, Google Store); Reolink PoE/WiFi spec sheets and the NT98566 teardown
  (reolink.com, MBReviews); UniFi G4 / G6 Pro Entry / Doorbell Lite (techspecs.ui.com,
  LazyAdmin); Aqara G410 (9to5toys, MacRumors); Tapo D235 datasheet and D260 launch (TP-Link,
  Forbes, Gizmodo); Amcrest AD410; Blink gen 2; Lorex B862 spec PDF; Matter 1.5 camera status
  (Matter Alpha, Samsung). Ambarella CV25M in Ring Pro 2 (element14, TechInsights).
- **Silicon and sensors:** Rockchip RV1106/RV1103B/RV1126B datasheets and Luckfox wiki; Sophgo
  SG2002 datasheet and the Seeed OSHW reCamera repository; SigmaStar SSC33x/SSC37x briefs and
  OpenIPC; Ingenic T31/T41 and thingino releases; ESP32-P4 techpedia and the open ESPHome PRs;
  Sony IMX335/IMX415/IMX678/IMX585/IMX662 (e-con Systems, FRAMOS, Commonlands); SmartSens
  SC450AI/SC850SL; ams-OSRAM SFH 4715AS / SFH 4725AS datasheets; ST VL53L5CX/VL53L8CX and the
  ST community outdoor thread; Hi-Link LD2410/LD2450; Infineon BGT60TR13C; Langir/DAIER/CDOE
  button catalogs; MAX98357A; Same Sky and Visaton IP67 speakers; GORE PolyVent; FCC modular
  approval guidance (KDB 996369 D04); UL/IEC 62368-1 and UL 5085 Class 2.
- **In-repo:** [`spec/invariants.md`](../../spec/invariants.md), [`spec/break_glass.md`](../../spec/break_glass.md),
  [`spec/witness_dictionary.json`](../../spec/witness_dictionary.json),
  [`spec/sensor_adapter_contract_v0.md`](../../spec/sensor_adapter_contract_v0.md), the
  [FAQ](../FAQ.md), the [Apple Home RFC](../design/apple_home_integration.md), the
  [competitor app landscape](../research/competitor_app_landscape.md), the
  [Vision Pro guide](./canary_vision_pro_recamera.md), the [Ranger](./canary_ranger_research.md),
  [Paw](./canary_paw_research.md), [Gatekeeper](./canary_gatekeeper_research.md) and
  [Herald](./canary_herald_research.md) dossiers (the radar, tamper and outdoor-placard bricks),
  the [Vision BOM](./bom_canary_vision.csv), the [doorbell case](./enclosure/canary_vision_doorbell.scad),
  the [Combo case](./enclosure/canary_combo.scad), [field ratings](./enclosure/field_ratings.md)
  and the [cold-weather envelope](./cold_weather_envelope.md).
