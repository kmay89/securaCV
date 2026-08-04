# Apple Home quickstart — from nothing to a sensor in the Home app

Two lanes, both ending with your fleet in the Home app. Pick one:

| You have… | Go to | Time |
|---|---|---|
| Home Assistant already running | [the HomeKit Bridge recipe](apple-home-homekit-bridge.md) | ~5 min, no new code |
| An Apple TV or HomePod and nothing else | [§1 below](#1-the-native-lane-hap_bridge) | ~10 min, one build |

Either way you need **one Apple TV or HomePod** signed into your Apple
Account. That is what makes it a Home Hub — without one, HomeKit accessories
work only while your phone is on the same Wi-Fi, and automations don't run at
all.

---

## 1) The native lane (`hap_bridge`)

`witnessd` speaks the HomeKit Accessory Protocol directly. No Home Assistant,
no cloud, no account.

### 1.1 Build it

The HAP stack is behind a default-off feature, so a normal build contains
none of it:

```sh
cargo build --release --features bridge-homekit-server --bin hap_bridge
```

### 1.2 Run it

```sh
./target/release/hap_bridge \
  --canary porch-canary="Porch Canary" \
  --canary garage-canary="Garage Canary" \
  --state ~/.securacv/hap_state.json \
  --mqtt-host 192.168.1.10
```

`--canary` takes `<mqtt-device-id>=<display name>`. The device id is what the
Canary publishes under (`securacv/<device-id>/…`); the name is what you will
see in the Home app.

> **Order matters.** The order of `--canary` flags fixes the HAP accessory
> ids. Reordering them after pairing makes the Home app read the wrong
> device. Add new Canaries to the **end** of the list.

It prints:

```
SecuraCV → Apple Home
  Setup code : 137-22-258
  Pairing URI: X-HM://0023P3O9E13I9
  Device ID  : 3A:BA:50:3E:86:C4
```

### 1.3 Pair

1. iPhone → **Home app → `+` → Add Accessory**
2. **More options…** — the bridge is not an Apple-certified accessory, so it
   won't appear on the scan screen.
3. Pick **SecuraCV**, enter the setup code.
4. iOS says *"uncertified accessory"* → **Add Anyway**. Normal; every
   Homebridge and HomeSpan user sees it.
5. Assign each Canary to a room.

### 1.4 No MQTT yet? Prove the pairing first

```sh
hap_bridge --canary test="Test Canary" --no-mqtt
```

The accessory pairs and publishes on the metronome; every signal simply
stays clear. Useful for separating "pairing is broken" from "events aren't
arriving," which otherwise look identical.

---

## 2) What you'll see

One accessory per Canary, each with a tile per signal:

| Signal | Home app | On by default |
|---|---|---|
| Motion | Motion sensor | yes |
| Occupancy | Occupancy sensor | yes |
| Contact | Contact sensor | yes |
| Tamper | ⚠️ on each sensor | **always** — cannot be turned off |
| Active | Status on each sensor | yes |
| Low battery | Status on each sensor | yes |
| Person / Vehicle / Animal / Package | Motion sensors | **no** — see §4 |

Tamper is deliberately not disableable. A witness reporting that it was
interfered with must not be able to do so invisibly to a home you already
chose to publish into.

---

## 3) The tick is a privacy dial, not a performance knob

`--tick-ms` (default 1000) sets how often the bridge publishes. It publishes
on that metronome **whether or not anything happened** — constant rate, so
the fact that a packet went out tells an observer nothing.

- **Coarsen it** (`--tick-ms 5000`) and external timing blurs further:
  nothing downstream can place an event more precisely than five seconds.
- **Shorten it** (`--tick-ms 500`) and automations feel snappier, at the cost
  of a finer timing bound.

Out-of-range values are **refused, not clamped** — a pacing constant is a
privacy parameter, so being quietly overruled would be worse than an error.

---

## 4) Turning on the class-scoped signals

Off by default. They carry one extra word — the coarse object class — which
is the single sanctioned step past what a hardware PIR sensor would publish:

```sh
hap_bridge --canary porch="Porch" --enable-class motion_person
```

Valid: `motion_person`, `motion_vehicle`, `motion_animal`, `motion_package`.

This is still not identity. There is no face, no plate, no name, no
re-identification, and no field in the vocabulary for any of them. It is
"a person-shaped thing moved," not "who."

Enabling one is logged, because it should never happen silently.

---

## 5) Keep the state file

```
~/.securacv/hap_state.json   (mode 0600)
```

It holds the accessory's private key, its setup code, and every pairing.

- **Back it up.** Losing it means every controller in the house must pair
  again from scratch.
- **Keep it private.** The bridge *refuses to start* if the file is readable
  by group or other, and tells you to `chmod 600` — it will not quietly fix
  the mode, because a key that has already been exposed should be treated as
  exposed.

---

## 6) When it doesn't work

| Symptom | Try |
|---|---|
| Bridge never appears in "Add Accessory" | Phone and bridge must be on the same subnet — mDNS does not cross VLANs. Check the host firewall allows UDP 5353 and TCP 51826. |
| "Accessory not found" after entering the code | Something else may be paired to it already. An accessory that is already paired refuses new pairings by design; delete it from the Home app first, or start from a fresh `--state` file. |
| Paired, but every sensor is blank | Events are not arriving. Check with `mosquitto_sub -h <broker> -t 'securacv/+/events' -v`. Confirm `--mqtt-host` and that `--canary` device ids match the topics exactly. |
| Occupancy never turns on | Presence rides in the retained `state` snapshot (`securacv/<id>/state`, field `presence`), not a `presence` topic. Confirm your firmware publishes it. |
| Worked, then stopped after a restart | The `--state` path changed, so the bridge minted a new identity. Point it back at the original file. |
| Automations don't run when you're out | No Home Hub. An Apple TV or HomePod must be signed in and set as a hub. |

Run with `RUST_LOG=debug` for more.

---

## 7) What this never sends

- **No video.** Not here, not through HomeKit Secure Video. That decision and
  the triggers that would revisit it are recorded in
  [the design doc](../design/apple_home_integration.md) §2.
- **No identity, no zone, no timestamp, no count, no confidence.** The
  vocabulary has no field for any of them, so there is nothing to leak and no
  setting to get wrong.
- **Nothing inbound.** Signals are read-only by construction: Apple Home is
  an audience for witness state, never a control surface for it.

---

## 8) Honest status

The pairing transcript follows the HAP specification and is exercised
end-to-end by tests that drive it with an independently written controller,
plus a live check where an external process completed pair-setup M1→M2 over a
real socket. **That proves the wire format, not that Apple's controller
accepts it** — only pairing against a real Apple TV or HomePod settles that,
and this document will say so plainly until it has.

If you hit something this table doesn't cover, that is worth an issue: it is
the first time this lane has met real hardware.

---

## Trademarks

Apple, Apple Home, HomeKit, HomeKit Secure Video, HomePod, Apple TV and Siri
are trademarks of Apple Inc., registered in the U.S. and other countries and
regions. SecuraCV is an independent project by Errer Labs and is **not
affiliated with, endorsed, sponsored, or certified by Apple Inc.** References
are nominative — for identification and interoperability only.
