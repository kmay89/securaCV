# Canary ↔ AC Infinity UIS — research dossier

Can a Canary switch an **AC Infinity UIS Control Plug (AC-ADA3)** and know
whether the load actually came on? And more broadly — can a Canary stand in for
a WiFi UIS controller (Controller 69 Pro / AI+) and drive the whole AC Infinity
ecosystem?

**Status:** research only. Nothing here is implemented in firmware. No board
change, no build flag, no BOM line has been committed on the strength of this
document.

*Research date: 2026-07-26. Method note: AC Infinity publishes no UIS protocol
specification, and `rollitup.org`, `eevblog.com` and `acinfinity.com`'s manual
PDFs all returned HTTP 403 to direct fetches from the research environment, so
the port-electrical section is built from **search excerpts of those threads**
and is marked by confidence throughout. The cloud-API section is different in
kind: it is read directly from the source of a working, maintained integration
([`dalinicus/homeassistant-acinfinity`](https://github.com/dalinicus/homeassistant-acinfinity))
and the field names are quoted verbatim from its `const.py` / `client.py`.
Treat §4 as **verified**, §3 as **plausible and unbench-tested**.*

> **Safety disclaimer.** This document discusses a device that switches **125 V
> AC at up to 15 A**. It is informational only and provided **"as is" with no
> warranty of any kind**. Mains wiring and mains-adjacent electronics should be
> designed and reviewed by a qualified person and comply with all applicable
> electrical codes. SecuraCV and the authors accept no liability for damage,
> injury, or loss arising from use of this information; **you assume all risk.**

---

## 1 · The short version

Three findings, in the order that matters:

1. **Do not plug the ADA3 into a Canary's USB-C port.** It is a USB-C *shell*
   carrying a non-USB, ~10 V proprietary bus. See §2 — this is the one item in
   this document that can destroy hardware today.
2. **There is no command set to discover at the plug.** The UIS port is not a
   packet protocol. It is a power rail, a presence line, and an analog/PWM
   0–10 V dim line. For the ADA3 specifically — a relay — "the command" is a
   voltage level, and the interesting question is one threshold, not a protocol.
   The named, structured *commands* live one layer up, in the controller's cloud
   API (§4).
3. **The port carries no state back.** Nothing on the wire tells you the relay
   closed or the load drew current. If a Canary is going to *claim* an outlet
   is on, it has to measure that independently (§6) — which is, conveniently,
   exactly the posture the rest of this project takes toward claims.

For "act like an AI+ controller," there are two real paths and they are not
close in cost: reimplement the port electrically (§3, §5) or drive a real
controller through its cloud API (§4). §7 compares them.

---

## 2 · ⚠️ Do not connect the ADA3 to a Canary with a USB-C cable

This is the thing to read before anything else.

UIS uses the USB-C **connector** and none of the USB **specification**. Community
teardowns consistently report the cable carries roughly 4 conductors, with about
**10 V** on the VBUS pins and **~10 V presented on a CC pin** as the presence
signal that tells a controller a device is attached.

A XIAO ESP32-S3's USB-C port is a real USB port. Its VBUS pin feeds the board's
5 V-nominal charger/regulator input, and its CC pins sit behind the standard
5.1 kΩ pulldowns referenced to 3.3 V logic. Presenting ~10 V to either is well
outside what those nets are built for. The plausible outcomes run from a dead
charger IC to a dead board.

The same warning applies in reverse, and is worth stating because it is the more
tempting mistake: **do not plug a normal USB-C device into a UIS port.** A UIS
port sourcing 10 V into a device expecting 5 V is the same failure with the
roles swapped.

**Consequence for any build:** a Canary never terminates a UIS cable at its own
USB-C jack. The interface is a **USB-C breakout board** wired to GPIO through
the isolation described in §5. The Canary's own USB-C port stays what it is —
power and flashing.

---

## 3 · What UIS actually is  ⚠️ *unverified, bench-test before trusting*

Every claim in this section is community-sourced, and the sources contradict
each other on details. Confidence is marked per row. **Nothing here should be
wired up before the measurements in §5.1 are run on your own unit.**

| Element | Best current understanding | Confidence |
|---|---|---|
| Connector | USB-C receptacle, **not** USB signaling | **High** — universally reported |
| Conductors | ~4 used (reports vary 3–4) | Medium |
| Rail | ~**10 V DC** on VBUS | **High** — multiple independent reports |
| Who sources the rail | The **device**, not the controller (a Cloudline fan powers the original Controller 67/69 through the cable) | Medium — verified for fans; **assumed** for the ADA3, which is mains-powered and has its own supply |
| Presence detect | Device ties a **CC pin to ~10 V**; controller enables the port on seeing it | Medium |
| Control | **0–10 V** analog level, generated as **PWM**, on a D± conductor | **High** on "0–10 V dim line"; medium on which conductor |
| PWM carrier | ~**5 kHz** | **Low — single-source.** Measure it. |
| Levels | Discrete **0–10**, matching the controller UI | **High** |
| Feedback | **None.** No return path from device to controller | Medium-high — no source describes one |

Two independent corroborations that the control line really is just a dimmer:

- AC Infinity sells a **"UIS Lighting Adapter Type-A for PWM or 0-10V Dimmers"**
  — a first-party product whose entire job is to expose the UIS control line as
  the industry-standard 0–10 V / PWM dimming signal.
- The `Controller 69` documentation describes the control type across all UIS
  devices as **"PWM 10 Levels."**

**Wire colors are contested and must not be trusted.** One source reports red
= +10 V, black = GND, green (D−) = PWM, yellow (D+) = ground; another reports
red = +10 V, black = GND, white = NC, yellow = 10 V PWM @ 5 kHz. They disagree
on which color is the control line. Color-coding is a manufacturing detail,
not a spec. **Measure, don't assume** (§5.1).

### 3.1 What this means for the ADA3 specifically

The ADA3 is a mains relay in a wall-plug body. It almost certainly threshold-
detects the dim line rather than interpolating it: level 0 → relay open,
level ≥ 1 → relay closed. There is no dimming to do — you cannot half-open a
relay.

So the reverse-engineering target for the ADA3 collapses from "a protocol" to
**one number: the voltage at which the relay latches, and its hysteresis.**
That is a ten-minute bench measurement, not a research project.

### 3.2 What the sticker says

The user-reported label — **50/60 Hz, 1800 W, 15 A** — matches the published
**AC-ADA3** North-American specification exactly (Type B plug, 125 V rated,
100–240 V input, 50/60 Hz, 1800 W, 15 A). `G2317` in orange is a
manufacturing lot/date marking, not a model number; it does not appear in any
AC Infinity catalog and should not be used to identify the part.

### 3.3 Naming — two different products, two different projects

"Outlet Control Plus" is close enough to two real AC Infinity products to be
worth disambiguating up front, because they need opposite approaches:

| If you have | What it is | Right approach |
|---|---|---|
| **AC-ADA3 UIS Control Plug** | A dumb relay on a UIS cable. No radio, no intelligence. | §5 — wire it. |
| **UIS Outlet Controller AI / AI+** | A **WiFi** smart outlet, a controller in its own right (`devType` **21** / **22** in the cloud API) | §4 — no wiring at all; it is already an API endpoint. |

The 1800 W / 15 A / 50–60 Hz sticker says AC-ADA3. Confirm before buying parts.

---

## 4 · The layer where commands actually exist ✅ *verified from source*

The WiFi controllers (Controller 69 Pro, AI+, Outlet AI+) do not expose a richer
protocol at the UIS port — their WiFi is for the app, and the port stays the
same 0–10 V line. The structured, named commands live in **AC Infinity's cloud
API**, and it is fully mapped by a maintained open-source integration.

**Host:** `http://www.acinfinityserver.com` — note **plain HTTP**.

| Endpoint | Method | Purpose |
|---|---|---|
| `/api/user/appUserLogin` | POST | Login. Body `appEmail`, `appPasswordl` *(sic — their typo, and it is load-bearing)*. Password is truncated to 25 chars. Returns `appId`. |
| `/api/user/devInfoListAll` | POST | All controllers + ports + sensors for `userId`. |
| `/api/dev/getdevModeSettingList` | POST | Current mode/settings for `devId` + `port`. |
| `/api/dev/addDevMode` | POST | **Write port mode/level** (params URL-encoded onto the query string). |
| `/api/dev/modeAndSetting` | PUT | Write path for **AI-family** controllers. |
| `/api/dev/getDevSetting` / `/api/dev/updateAdvSetting` | POST | Advanced per-port settings. |

**Headers:** `User-Agent: okhttp/4.12.0` on everything; `token: <appId>` when
authenticated; AI controllers additionally require `minversion: 3.5`.

**Read-modify-write is mandatory.** The write endpoints take the *entire*
settings object, not a delta — the integration GETs the current settings, mutates
the fields it cares about, and POSTs the whole thing back. Sending a partial
payload will clobber state.

### 4.1 The fields that matter

Per-port state (`devInfoListAll` → `ports[]`):

| Field | Meaning |
|---|---|
| `port` | Port index |
| `speak` | **The level, 0–10.** (Their spelling of "speed".) |
| `loadState` | Whether the load is on |
| `online` | Whether a device is detected on the port |
| `remainTime` | Time left in the current timer/cycle |

Mode selection — `atType`:

| Value | Mode | Value | Mode |
|---|---|---|---|
| 1 | Off | 5 | Timer to off |
| 2 | On | 6 | Cycle |
| 3 | Auto | 7 | Schedule |
| 4 | Timer to on | 8 | VPD |

So **"toggle the outlet"** is: `atType=2`, `speak=10` → on; `atType=1` → off.
Related fields: `onSpead` / `offSpead` *(sic)* set the level used in each state,
`activeCycleOn` / `activeCycleOff` drive cycling, `schedStartTime` /
`schedEndtTime` *(sic)* drive scheduling.

Controller types (`devType`): `11` = 69 Pro, `18` = 69 Pro+, `20` = 89 AI+,
`21` = Outlet AI, `22` = Outlet AI+.

### 4.2 `loadState` is not the load state

This one deserves a flag, because it is the field you would reach for to answer
"is it on?"

`loadState` is reported by the *controller*, about a port whose only outbound
wire is a dim line with no return path (§3). The controller cannot see the
relay. Whatever `loadState` reflects — most likely the commanded state, possibly
a port-current measurement on newer hardware — **it is not confirmation that the
appliance is running.** Treat it as an echo of the command until someone proves
otherwise on a bench by unplugging the load and watching whether the field
moves.

That gap is the whole reason §6 exists.

---

## 5 · Path A — a Canary drives the ADA3 directly

### 5.1 First: characterize the port (do this before wiring anything)

You need an ADA3, a multimeter, and ideally a scope. A known-good UIS controller
makes step 4 much easier but is not required for 1–3.

1. **Continuity-map the receptacle.** With the ADA3 **unplugged from mains**,
   buzz out a USB-C breakout against the ADA3's captive cable / receptacle.
   Record which USB-C pin numbers (A1–A12 / B1–B12) go to which conductors, and
   whether the A and B rows are shorted together — if they are, the connector is
   flip-agnostic, which matters for cable orientation.
2. **Power it and measure statically.** Plug the ADA3 into mains with nothing in
   its outlet. Measure every conductor to GND. Confirm the ~10 V rail, confirm
   which pin is held near 10 V as presence, and confirm the ADA3 *sources* the
   rail rather than expecting it. **Log the actual numbers** — §3 is assumption
   until this step replaces it.
3. **Find the latch threshold.** Drive the candidate control line from a bench
   supply through a series resistor, sweep 0 → 10 V slowly, and record the
   voltage where the relay clicks on and the (different) voltage where it clicks
   off. The gap is the hysteresis. **These two numbers are the entire ADA3
   "protocol"** and everything in §5.2 depends on them.
4. **Scope a real controller, if you have one.** Capture the control line at
   each UI level 0–10. This is what settles the ~5 kHz carrier claim, tells you
   whether it is true PWM or a filtered DC level, and reveals any handshake at
   plug-in that the static measurements miss.

Record the results in this document. A dossier with measured numbers in it is
worth more than this one.

### 5.2 The interface

Three constraints set the design:

- The Canary's GPIO is 3.3 V; the control line needs ~10 V.
- The rail belongs to the ADA3, so the Canary sources no power to it.
- The far end of the ADA3 is **mains**. Everything else follows from that.

**Use an optocoupler, not a bare MOSFET.** A logic-level MOSFET switching the
dim line to the ADA3's 10 V rail works electrically and is what the fan-control
community uses. It also bonds the Canary's ground to the ground of a
mains-referenced switch, which puts the mains barrier's integrity in series with
your witness device. An optocoupler is a few cents and removes the Canary from
that conversation entirely. On a device whose entire pitch is trustworthiness,
this is not a place to save a part.

**The output stage must fail to OFF.** This is the part to get right, and it is
easy to get backwards. The Canary is not the only thing that can stop working:
it boots, it resets, it can lose power or hang with the GPIO floating, and an
ESP32 GPIO is a high-impedance input at reset. Every one of those states leaves
the optocoupler dark. **Dark must mean relay open**, or a crashed or unpowered
witness device silently energizes a 15 A load.

So the opto transistor **sources** the ADA3's 10 V onto the control line when
lit, and a pull-down resistor holds the line at GND when it isn't. Do *not*
idle the control line pulled up to the 10 V rail — that inverts the failure
mode into "everything broken = load on."

```
                              ┊ isolation barrier
                              ┊         ADA3's own 10 V rail
                              ┊                 │
  Canary GPIO ─[R 220Ω]─▶ LED ┊  ┌── opto transistor ──┐
        GND ────────────────  ┊  │                     ├──▶ UIS control line
                              ┊  └─────────────────────┘    │
                              ┊                          [R_pd]  pull-DOWN
                              ┊                             │
                              ┊                          ADA3 GND

  GPIO high → opto lit → line pulled to ~10 V → relay closed
  GPIO low / floating / Canary unpowered → opto dark → R_pd holds line at 0 V
                                                     → relay OPEN  ← fail-safe
```

Two corollaries worth checking on the bench: size `R_pd` low enough that it
actually holds the line below the *release* threshold measured in §5.1 step 3
against the opto's dark leakage, and pick a control GPIO that is **not** a
strapping pin and has no boot-time pull-up (D5/GPIO6 satisfies both on the
ESP32-S3).

Because the ADA3 is binary (§3.1), **no PWM is required** — hold the line above
the latch threshold for on, at 0 V for off. Keep the LEDC/PWM option in reserve
for UIS *fans and lights*, which do use the full 0–10 V range.

**Suggested pins on the XIAO ESP32-S3 Sense** (from
[`canary_peripheral_build_plan.md`](./canary_peripheral_build_plan.md) §5.1):

| Function | XIAO pin | GPIO | Note |
|---|---|---|---|
| UIS control out | D5 | 6 | Free today |
| CT sense in (§6) | D4 | 5 | ADC1_CH4. **Contended** — D4/GPIO5 is the documented relocation target for the cap-touch pad when tamper occupies D3/GPIO4. A build using touch + tamper + this needs a fourth pin. |

Neither is reserved, and neither collides with the camera, PDM mic, microSD, or
GNSS blocks.

### 5.3 What this does *not* get you

Driving the line makes the ADA3 switch. It does not make the Canary a UIS
controller. Fans, lights and pumps need the full 0–10 V range and correct
presence handling; the AC Infinity app, sensors and modes are not in scope at
all. Native UIS *host* emulation across the ecosystem is a much larger project
than switching one relay, and §5.1 step 4 is where you would find out how much
larger.

---

## 6 · Monitoring state — the part the wire cannot do

The UIS port returns nothing (§3), and `loadState` is a controller-side echo
(§4.2). A Canary that reports "outlet on" because it *asked* for the outlet to
be on is reporting its own intent, dressed as an observation. A relay that welds
shut, a tripped breaker, an unplugged appliance, a lost UIS cable — every one of
those reads as "on."

This project already has a name for the difference between what a system claims
and what it can show. Applying it here: **measure the load.**

**Recommended: a split-core current transformer** (e.g. SCT-013 class) into an
ADC pin through the usual burden resistor and mid-rail bias network. It never
touches mains — it clamps around insulated cable — and it is the only option
here that distinguishes "relay closed" from "load running."

> **⚠️ A CT clamped around the appliance cord reads zero.** This is the standard
> way to get this wrong, and it fails in the direction that matters: a jacketed
> cord carries line and neutral together, their fields oppose and cancel, and
> the CT reports ~0 A **while the appliance is happily running** — a verification
> path that lies in exactly the "it's off" direction you built it to catch.
>
> A CT must enclose **one conductor only**. Get that with a commercial
> **line-splitter / current-splitter adapter** rated for the circuit (the AC-ADA3
> is a 15 A device) — a plug-through accessory that separates the conductors and
> presents one of them for clamping, often with a ×10 turn to help at low
> currents. **Do not slit, split, or otherwise modify a mains cord's jacket to
> reach a conductor.** Buying the adapter is the whole fix; it is the difference
> between a non-contact measurement and a modified mains cable in someone's
> home.

That distinction is worth more than binary on/off. A CT sees the *shape* of
consumption, which means a Canary could witness "the equipment on this outlet is
drawing what it normally draws" — a genuine claim about the world — instead of
"I sent a command." It also fits the existing sensing vocabulary: this is a
shape, not an identity.

Cheaper and coarser alternatives, for completeness: an opto-isolated AC
presence sensor across the outlet (binary, tells you mains is present at the
socket but not that the load is drawing), or an off-the-shelf inline energy
meter (accurate, but adds a second network device and its own trust question).

**Any claim emitted from this should be worded as what was measured.** "Load
current present" is true and checkable. "Outlet on" is an inference. The
distinction is the point.

---

## 7 · Path B — drive a real controller through the cloud, and why it is not free

If the goal is "use the whole AC Infinity ecosystem," §4 is a complete,
already-mapped answer requiring zero hardware: buy a Controller 69 Pro or AI+,
let it own the UIS ports, and have a Canary or the HA integration call the API.
Every fan, light, pump, and outlet in their catalog comes along for free, with
their sensors and modes intact.

It also has costs that this project in particular should not wave through:

- **It is not local.** `http://www.acinfinityserver.com` is the authority. No
  documented LAN API exists.
- **It is plain HTTP.** Credentials and commands cross the network unencrypted.
- **It leaks event timing.** This is the sharp one. A Canary that switches an
  outlet in response to a detection, over a cloud API, emits a
  timestamped, account-linked request to a third party **every time something
  happens in front of the device**. That is a network side-channel proportional
  to event occurrence — precisely what
  [`spec/invariants.md`](../../spec/invariants.md) Invariant III
  ("Metadata Minimization Is Structural") says the system must not do:
  *"Cannot vary network behavior in proportion to event occurrence unless
  explicitly configured for cover traffic."*

None of the invariants forbid **actuation** — they govern raw media, identity,
metadata, custody, and queryability, and an outlet is none of those. A Canary
switching a light is invariant-clean. A Canary switching a light *by telling a
vendor's cloud, in real time, that it saw something* is not.

**So: the wire path (§5) is the one that fits this project.** It is more work
and it stays inside the device boundary. The cloud path is the fast way to
explore the ecosystem and a reasonable bench tool; it should not sit between a
detection and a response in a shipped Canary.

### 7.1 Path C — BLE

The UIS controllers pair over Bluetooth for local setup, which implies a local
control path that would sidestep both the cloud dependency and the timing leak.
**No public reverse-engineering of it was found** — searches for the service and
characteristic UUIDs turned up nothing usable. This is an open gap, and if
anyone wants a genuinely local way to drive the full ecosystem, sniffing the
app's BLE session is where the value is.

---

## 8 · Open questions

Ordered by how much they block progress.

1. **The ADA3 latch threshold and hysteresis** (§5.1 step 3). Blocks the §5.2
   circuit. Cheapest to answer.
2. **Does the ADA3 source the 10 V rail?** Assumed from the fan case. Blocks the
   circuit's power assumptions.
3. **Actual pin map on the ADA3's connector** (§5.1 step 1), including whether
   the A/B rows are shorted. Blocks the breakout wiring.
4. **Is the carrier really ~5 kHz PWM, or a filtered DC level?** Single-source.
   Doesn't block the ADA3; blocks UIS fans and lights.
5. **What does `loadState` actually reflect?** Unplug the load, watch the field.
   Determines whether §6 is mandatory or merely better.
6. **The BLE protocol** (§7.1). The only known route to local ecosystem-wide
   control.

## 9 · If you want to run §5.1 this week

A USB-C breakout board (one that brings out all pins to headers), a multimeter,
and the ADA3 you already have. That is enough for steps 1–3, which answer
questions 1–3 above and turn the speculative half of this document into a
measured one. The optocoupler, the CT clamp **and its line-splitter adapter**
(§6 — the CT is useless without it), and a scope only matter after that.

---

## 10 · Sources

Community/electrical (§2, §3) — treat as unverified:

- [Has anybody reversed the AC Infinity UIS protocol? — Rollitup](https://www.rollitup.org/t/has-anybody-reversed-the-ac-infinity-uis-protocol.1091506/)
- [Help reversing the AC Infinity UIS protocol? — EEVblog](https://www.eevblog.com/forum/beginners/help-reversing-the-ac-infinity-uis-protocol/)
- [Idiots guide to getting an esp32 to control AC Infinity Cloudline EC fans — Rollitup](https://www.rollitup.org/t/idiots-guide-to-getting-an-esp32-to-control-ac-infinity-cloudline-ec-fans-esphome-ha.1053910/)
- [DIY EC Fan controller? — Rollitup](https://www.rollitup.org/t/diy-ec-fan-controller.1032169/)
- [ex-nerd/esphome-shelly-cloudline](https://github.com/ex-nerd/esphome-shelly-cloudline)
- [Duct Fan Controller with ESP32 and ESPHome — Matt Clark](https://scattym.github.io/esp32-wifi-pwm-ducturbo-fan-controller/v1/)

First-party product/spec (§3, §3.2):

- [UIS Control Plug, for Outlet-Powered Equipment (AC-ADA3) — AC Infinity](https://acinfinity.com/uis-control-plug-for-outlet-powered-equipment/)
- [UIS Control Plug user manual (PDF)](https://acinfinity.com/content/ADA2207X1_220727_UIS%20Control%20Plug%20Manual.pdf)
- [Smart UIS Lighting Adapter for PWM & 0-10V Dimmers — AC Infinity](https://acinfinity.com/uis-lighting-adapter-type-a-for-rj11-12-connector-lights-with-pwm-or-0-10v-dimmers/)
- [Controller 69 Overview — AC Infinity](https://acinfinity.com/pages/controller-programming/controller-69-overview.html)
- [Controller AI+ Overview — AC Infinity](https://acinfinity.com/pages/controller-programming/controller-ai-overview.html)
- [Ecosystem Compatibility — AC Infinity](https://acinfinity.com/pages/ac-infinity-ecosystem/ecosystem-compatibility.html)

Cloud API (§4) — read from source:

- [dalinicus/homeassistant-acinfinity](https://github.com/dalinicus/homeassistant-acinfinity)
  — `custom_components/ac_infinity/const.py` and `client.py`
- [keithah/homebridge-acinfinity](https://github.com/keithah/homebridge-acinfinity)
