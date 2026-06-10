# Onboarding Multiple Canaries — Setup Wizard

This guide walks you through flashing and onboarding **more than one** SecuraCV
Canary (WAP firmware) onto your home network, telling them apart, and physically
locating any one of them with the **Identify** feature.

If you only have a single device, the short version still applies: flash it,
join its WiFi, open `http://canary.local`, done. The rest of this guide covers
what changes once a second device joins the network.

> **TL;DR**
> - Each Canary is reachable at a **unique** name: `canary-<name>.local`
>   (e.g. `canary-kitchen.local`) — or `canary-aabb.local` if you don't name it.
> - `canary.local` still works as a **catch-all** for your first/primary device.
> - Use the **Identify** button (device dashboard or the Canary Vision app) to
>   blink a specific device's LED + chirp so you can find it — like Philips
>   Hue's "identify this light".

---

## Hardware per device

| Component | Notes |
|-----------|-------|
| **XIAO ESP32-S3 Sense** (or ESP32-C3) | One per Canary |
| **USB-C cable** | For flashing |
| microSD, GPS | Optional |

Each board has a globally-unique hardware identity. The firmware derives a
stable per-device handle from it (`device_id` = `canary-s3-AABB`, AP SSID
`SecuraCV-AABB`), so you never have to configure anything to make two devices
distinct at the hardware level. The four-character suffix is encoded in an
unambiguous alphabet (no `0/O/o` or `1/I/i/l/L`), so it never contains glyphs
you might misread. Because the handle is derived from immutable hardware, it is
**the same every time you flash** — that stability is the point: it's how
`canary.local`, Home Assistant, and MQTT keep tracking the same board.

---

## Step 1 — Flash Canary #1

From the WAP project directory:

```bash
cd firmware/projects/canary-wap

# PlatformIO (recommended)
make upload          # build + flash over USB-C
make monitor         # watch serial output (115200 baud)

# …or Arduino IDE: open arduino/canary_wap/canary_wap.ino
#   Board: "XIAO_ESP32S3"  ·  USB CDC On Boot: Enabled  ·  PSRAM: OPI PSRAM
#   (define HARDWARE_XIAO_ESP32S3 or HARDWARE_XIAO_ESP32C3 for your board)
```

On first boot the device starts a WiFi Access Point and a captive portal.

## Step 2 — Add Canary #1 to your home network

1. On your phone/laptop, join the WiFi network **`SecuraCV-AABB`** (the last 4
   characters are unique to this board). The AP password is device-unique and
   printed on the provisioning receipt / serial log.
2. A "sign in to network" page appears. Open **`http://canary.local`** in a real
   browser (the captive page links to it; `http://192.168.4.1` also works).
3. In the dashboard's **WiFi** section:
   - Select your home network and enter its password.
   - **Device name** (optional but recommended): type something memorable like
     `kitchen`. This becomes the device's hostname → **`canary-kitchen.local`**.
4. Press **Connect**. The device joins your home WiFi and reboots.

After it rejoins, Canary #1 is reachable at:
- **`http://canary-kitchen.local`** (its unique name), **and**
- **`http://canary.local`** (the catch-all — see "How `canary.local` behaves").

## Step 3 — Flash Canary #2 and add it

Repeat Steps 1–2 with the second board. Join its AP (`SecuraCV-CCDD` — a
different suffix), open `http://canary.local`, and name it e.g. `livingroom`.

> While you are connected to Canary #2's *own* AP during setup, `canary.local`
> resolves to **that** device (you're on its isolated AP network), so the wizard
> is never ambiguous about which device you're configuring.

Once it joins your home WiFi, Canary #2 is reachable at
**`http://canary-livingroom.local`**.

---

## How `canary.local` behaves with multiple devices

Previously **every** Canary advertised the hostname `canary`, so two devices
collided on `canary.local` and which one answered was unpredictable. That is
fixed:

- **Every device** now advertises a **unique** hostname:
  `canary-<name>.local`, or `canary-<mac-suffix>.local` if unnamed.
- **`canary.local`** is claimed as an *additional* catch-all by the **first
  device that grabs it** (first-wins). A second device detects it's already
  taken and does **not** fight for it — it relies on its unique name.

What this means in practice:

| Scenario | `canary.local` resolves to | Each device also at |
|----------|---------------------------|---------------------|
| One Canary | that device | `canary-<name>.local` |
| A device still in setup (on its own AP) | that device | — |
| Several Canaries on home WiFi | the primary (first to claim) | `canary-<name>.local` each |

So `canary.local` stays a friendly default, and the unique names remove all
ambiguity once you have more than one.

> **Note:** `.local` resolution relies on mDNS/Bonjour. macOS and iOS have it
> built in; Windows 10+ and most Linux (Avahi) do too. If a `.local` name won't
> resolve on your network, use the device's IP address instead (find it in your
> router's client list — the unique hostname shows up there too).

---

## Pairing from the Canary Vision app (zero typing)

The `canary-vision` companion app pairs devices with the **BOOT-tap** flow —
no tokens to copy:

1. In the app: **My Canaries → + Add Canary**. Canaries already on your WiFi
   appear under *Discovered on your network*; tap one (or enter its address).
2. The app listens for ~60 seconds. **Short-tap the BOOT button** on that
   Canary — the tap opens the device's provisioning gate and the app captures
   the receipt (address + token) itself.
3. Confirm the pairing: tap **Blink to confirm** to make that exact box flash
   its LED, give it a name (becomes `canary-<name>.local`), and pick the room
   it watches.
4. Tap **+ Add another Canary** and repeat — every device after the first is
   one tap in the app plus one tap on the device.

Fallbacks (under *Other ways to pair*): scan the pairing QR from the device
dashboard (**Settings → Device → Show QR** — requires the app to be served
over HTTPS), paste a provisioning receipt JSON, or enter the device address +
API token manually. The QR carries this device's API token, so the dashboard
only renders it for an authenticated session.

Rooms are stored in the app only — devices never hold fleet-wide state.

---

## How do I tell them apart?

1. **By name** — the unique `canary-<name>.local` URL, and the `device_id`
   shown on each dashboard's Status tab.
2. **In a list** — open the `canary-vision` companion app. Its My Canaries
   dashboard lists every paired Canary with its name, room, firmware, and
   health, and surfaces unpaired ones under *Discovered on your network*.
3. **Physically — the Identify feature** (below).

---

## Identify a specific device (like Philips Hue)

When the boxes look identical and you need to know which is which, **Identify**
makes one device announce itself: it **triple-blinks its LED** and plays the
"I'm here" **chirp** for ~15 seconds. Fully non-blocking — the device keeps
serving while it blinks.

**From a device's own dashboard:** open `http://canary-kitchen.local`, go to the
**Canary Chirp** card, and click **Identify**.

**From the Canary Vision app** (best when onboarding several at once):
1. Open the app's **My Canaries** dashboard.
2. Tap a device, then **Blink to find this Canary** (under Identity). The
   pairing wizard also offers **Blink to confirm** right after a device
   joins, so you never lose track of which box is which.
3. Watch your shelf — the matching Canary blinks (and chirps if a buzzer is
   attached).

**From the API directly:**
```bash
curl -X POST "http://canary-kitchen.local/api/identify" \
     -H "Authorization: Bearer cv_<your_token>" \
     -H "Content-Type: application/json" \
     -d '{"duration_ms": 15000}'
# → {"ok":true,"duration_ms":15000,"visual_only":false}
```
`duration_ms` is optional (default 15000, clamped to 1000–60000). `visual_only`
is `true` when no buzzer is attached, in which case Identify is LED-only.

> Connect a passive buzzer to the chirp GPIO for an audible identify; without a
> buzzer the device blinks the on-board LED only.

---

## Renaming a device later

You don't have to re-run setup to rename a Canary. From an authenticated
session:

```bash
curl -X POST "http://canary-kitchen.local/api/device-name" \
     -H "Authorization: Bearer cv_<your_token>" \
     -H "Content-Type: application/json" \
     -d '{"name": "pantry"}'
# → {"ok":true,"device_name":"pantry","mdns_host":"canary-pantry"}
```

The new `canary-pantry.local` name takes effect immediately (mDNS re-announces;
no reboot needed). Names are sanitized to `[a-z0-9-]` per DNS label rules.

---

## Troubleshooting

| Symptom | Fix |
|---------|-----|
| `canary.local` opens the "wrong" Canary | Expected with multiple devices — use the unique `canary-<name>.local` instead. |
| `.local` names don't resolve | Your OS/network may lack mDNS. Use the device IP from your router's client list. |
| Two devices show the same name | One was never named — name it in setup or via `/api/device-name`. |
| Identify returns `401` | Provide the device's API token (Bearer header, a paired Canary Vision app, or an open authenticated session). |
| Identify blinks but is silent | No buzzer attached — that's the LED-only fallback (`visual_only:true`). |

See also: [`getting_started_canary.md`](getting_started_canary.md) for
single-device setup, and
[`onboarding_workflow_evaluation.md`](onboarding_workflow_evaluation.md) for the
engineering rationale behind these changes.
