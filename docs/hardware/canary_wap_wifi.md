# How the Canary WAP's WiFi works — link, radio, and sensing

> Orientation doc for the Canary **WAP** (Wireless Access Point witness,
> `firmware/projects/canary-wap/`). "WiFi" means two very different things on
> this device, and keeping them straight is the key to understanding it:
> WiFi is both **how the device connects** and **how it senses the room**.
> The one 2.4 GHz radio does both jobs (and shares itself with BLE), which is
> why the firmware spends so much care on coexistence.
>
> This is the map; the deep dives are linked at the bottom.

---

## The two meanings of "WiFi" here

| | **WiFi as a link** | **WiFi as a sensor** |
|---|---|---|
| Purpose | Get the device on your network + serve its dashboard | Detect that a person is *in the room* — no camera, no mic |
| Mechanism | SoftAP for setup → station (STA) on home WiFi | CSI (RF-field perturbation) + probe-request counting |
| Needs | A router + credentials (or runs AP-only) | Only that WiFi is *up* at all — it measures the radio itself |

The product name — "Canary WAP: feels presence through the WiFi field itself"
— is about the **second** column. The first column is just how you get to it.

---

## Part 1 — WiFi as a link (setup + connectivity)

### The AP → STA state machine
The device tracks a small provisioning state machine
(`WIFI_PROV_*` in `../../firmware/projects/canary-wap/arduino/canary_wap/canary_wap.ino`:
`IDLE → SCANNING → CONNECTING → CONNECTED / FAILED / AP_ONLY`):

1. **First boot — the device *is* an access point.** It broadcasts its own
   network **`SecuraCV-XXXX`** (`generate_ap_ssid()`) protected by a
   **device-unique WPA2 password** — `cv-` + 12 characters **derived from the
   device's private key** (not a shared default; it's printed on the serial
   console at first boot). Join it and the device serves a **captive portal**
   at `192.168.4.1` / `canary.local` — your phone's "Sign in to network" sheet
   opens it automatically.
2. **You hand it your home WiFi.** Scan the on-page QR (or tap the manual
   link), pick your SSID, type the password — one screen each. **The
   credentials go straight to the device and never leave your home** (no app,
   no account, no cloud round-trip).
3. **It joins as a station (STA).** Once the STA link has held for
   **2 minutes** (`AP_DROP_GRACE_MS = 120000`), the device **tears down its own
   SoftAP** so the single radio runs **STA + BLE** — the coexistence combo
   Espressif rates stable — instead of **AP + STA + BLE**, which is rated
   unstable (C1) once a client is joined to the AP. See `wifi_drop_ap()` /
   `wifi_raise_ap()`.
4. **It stays reachable.** mDNS advertises `canary.local` and a per-device
   `canary-<name>.local`, **re-announced when the STA gets its home-network IP**
   (without that, the hostname is only resolvable on the AP interface).

### When there's no home WiFi
If credentials are missing or wrong, the device settles into **`WIFI_PROV_AP_ONLY`**
and **runs permanently on its own SoftAP** — that's a valid product state, not
an error. If a previously-healthy STA link later drops, the SoftAP is
**auto-re-raised** as a re-pairing fallback.

### Why the choreography — one radio
There is a **single 2.4 GHz radio** shared by WiFi (AP and/or STA) *and* BLE.
That constraint explains most of the seemingly-fussy logic:
- The SoftAP is pinned to `AP_CHANNEL`; **joining a home AP on a different
  channel drags the SoftAP to that channel** (single radio), which briefly
  kicks the provisioning phone — the 2-minute grace exists so a *successful*
  join doesn't look like "couldn't connect" on the phone while it
  re-associates + re-DHCPs.
- A full WiFi **scan hops the radio across every channel**, stalling the
  SoftAP — hence scans are bounded/scheduled.
- **STA + BLE is the stable steady state**; AP + STA + BLE is only tolerated
  transiently during provisioning.

> This is also why the direct **BLE fleet link** (see
> [`display_discovery_and_resilience.md`](./display_discovery_and_resilience.md)
> §3.1) uses BLE rather than ESP-NOW: BLE shares this radio but is **not tied to
> the WiFi channel**, so it keeps working no matter what the STA is doing.

---

## Part 2 — WiFi as a sensor (the reason it's called a *WAP*)

Two independent WiFi-based sensing paths, both privacy-first:

### CSI — Channel State Information (presence / motion)
The device reads the **fine-grained radio channel response** of WiFi packets
(`esp_wifi_set_csi_rx_cb`, wired up in `csi_integration.h`; modules under
`csi_*`). A human body moving through the room perturbs the 2.4 GHz RF field,
and those perturbations show up in the CSI. The device:
- **learns an "empty-room baseline"** — that's the "step out of the room for one
  minute" step in setup — then
- flags **presence / motion** when the live signal deviates from that baseline.

No camera, no microphone: it senses the **field**, not who you are. This is what
makes the dashboard's orb pulse when someone is in the room.

### Probe-request presence (device counting)
In promiscuous mode the device passively overhears the **probe requests** phones
constantly broadcast (`wifi_presence.h`). Privacy invariants are strict:
- each **MAC is hashed immediately** (in `process_queue()`, out of ISR context)
  with a **per-time-bucket salt**, so hashes can't be correlated across buckets;
- **raw MACs and SSIDs are never stored** — only **unique device *counts*** are.

So it can say "roughly this many devices are around" without tracking anyone.

Both sensing paths need WiFi to be *up* (AP or STA) to have packets to measure —
the radio is doing double duty as link and sensor.

---

## Part 3 — how it ties together

Presence and motion from CSI / probe sensing become **signed, hash-chained
witness records** (Ed25519, tamper-evident). From there:
- the **local dashboard** at `canary.local` renders live state;
- the CSI bridge can publish **Home Assistant MQTT auto-discovery** so the fleet
  shows up in HA;
- and the same presence/status also **beacons directly to nearby displays over
  BLE** — no broker, no WiFi required — via the fleet link.

So the WAP's WiFi is a loop: it connects over WiFi, senses over WiFi, signs what
it sensed, and then surfaces it locally, to Home Assistant, and to your displays.

---

## Deep dives

| Topic | Doc |
|---|---|
| Radio-level BLE + WiFi-AP coexistence audit | [`../esp32s3_ble_wap_audit.md`](../esp32s3_ble_wap_audit.md) |
| How the mesh (Opera ESP-NOW + Chirp) shares the radio | [`../network_coexistence.md`](../network_coexistence.md) |
| CSI setup (grandma path) + developer path | [`../csi_quickstart.md`](../csi_quickstart.md) |
| Writing a CSI sensing module | [`../csi_modules.md`](../csi_modules.md) |
| Secure hardware provisioning / device identity | [`../secure_provisioning.md`](../secure_provisioning.md) |
| Displays finding the WAP (mDNS + direct BLE) | [`display_discovery_and_resilience.md`](./display_discovery_and_resilience.md) |
