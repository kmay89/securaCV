# Canary Onboarding — Workflow Evaluation & Fixes

An audit of the Canary WAP onboarding flow, the multi-device gaps it had, and
the changes made to support a clean wizard-style setup of several Canaries.
File references are to
`firmware/projects/canary-wap/arduino/canary_wap/` unless noted.

## The onboarding flow (as traced)

1. **Boot / identity** — `provision_device()` loads/creates the Ed25519
   keypair, then derives a stable `device_id` (`canary-s3-AB7K`) and AP SSID
   (`SecuraCV-AB7K`) from its public-key fingerprint (never the MAC —
   event_contract §10), and derives the API token. (`canary_wap.ino`)
2. **AP + captive portal** — `wifi_init_provisioning()` brings up
   `WIFI_AP_STA`, starts the SoftAP with a device-unique password, and (on first
   boot) a captive-portal DNS that redirects to `192.168.4.1`. The static
   captive page tells the user to open `canary.local`. (`setup_wizard.h`,
   `setup_page_html.h`)
3. **mDNS** — `MDNS.begin(...)` advertises `_http._tcp` and `_securacv._tcp`
   (TXT: `device_id`, `fw`, `model`).
4. **Credential capture** — the dashboard posts to `/api/wifi/connect`
   (`handle_wifi_connect`), gated by a short-lived pairing token; SSID/password
   are validated and saved to NVS (`wifi_save_credentials`), then
   `wifi_connect_to_home()` runs.
5. **Connect + reboot** — `wifi_check_connection()` (driven from `loop()`)
   tracks the STA state; on success the wizard marks setup complete and reboots.
6. **Steady state** — AP stays up (never torn down); a `STA_GOT_IP` handler
   re-announces mDNS on the home-WiFi interface.

This is solid for **one** device. The gaps were all about **more than one**.

## Findings

### F1 — Every device claimed the same hostname (the core bug)

`wifi_init_provisioning()` hardcoded the hostname for *all* devices:

```cpp
WiFi.setHostname("canary");
MDNS.begin("canary");
```

A code comment asserted that *"RFC 6762 §9 conflict resolution renames a second
device to canary-2.local"* — but the Arduino **ESPmDNS** wrapper does **not**
perform reliable hostname-conflict renaming. So two Canaries both claimed
`canary.local`, and which one answered was non-deterministic. There was no
stable per-device name to fall back to, and the router's DHCP client list showed
two identical `canary` entries.

**Fix:** every device now advertises a **unique** hostname,
`canary-<name>.local` (friendly name) or `canary-<mac-suffix>.local`
(`generate_mdns_hostname()`, reusing the RFC-1123 sanitizer from
`firmware/canary/lib/securacv_network`). `canary.local` is preserved as a
**first-wins catch-all** via the IDF delegated-hostname API
(`claim_catch_all_hostname()` → `mdns_delegate_hostname_add`), version-guarded
for ESP-IDF ≥ 4.4. New `host`/`name` TXT records expose the unique hostname and
friendly name to the SPA and fleet manager.

### F2 — Friendly-name plumbing existed but was never wired up

`setup_wizard.h` already had `set_device_name()` / `get_device_name()` and the
`dev_name` NVS key — but **nothing ever called `set_device_name()`**, and the
name never affected the hostname.

**Fix:** `handle_wifi_connect` now accepts an optional `device_name` during
setup; a dedicated `POST /api/device-name` allows renaming afterward. Both
persist via the existing `set_device_name()` and regenerate the mDNS hostname.
The SPA setup form gained a "Device name" field.

### F3 — No way to physically locate a device

There was no "identify" affordance. The only LED behaviour was the
`LED_BUILTIN` blink fallback inside `audible_chirp.h`'s pattern player.

**Fix:** a new non-blocking `PATTERN_IDENTIFY` (triple LED blink + "I'm here"
chirp) plus a `POST /api/identify` endpoint driven by a millis-based scheduler
(`identify_tick()` in `loop()`). Surfaced as an **Identify** button on the
device dashboard and per-device in `fleet-manager.html` (Philips-Hue-style).

### F4 — Fleet manager had no per-device auth for actuators

`fleet-manager.html` called device endpoints without credentials. Identify is an
authenticated action, so the fleet manager needed a way to authorize it.

**Fix:** an optional per-device **API token** field (stored in the device
model, sent as `?token=`); `/api/identify` authenticates via
`api_auth_check_or_query` (Bearer header **or** `?token=` **or** an existing
session cookie), matching the existing camera-peek pattern.

## Files changed

| File | Change |
|------|--------|
| `…/canary_wap.ino` | unique+catch-all hostname, `device_name` capture, `/api/identify`, `/api/device-name`, identify scheduler, `device-info` JSON |
| `…/audible_chirp.h` | `PATTERN_IDENTIFY` |
| `…/web_ui.h` | Identify button + JS, Device-name setup field (regenerate `web_assets_gz.h`) |
| `firmware/fleet-manager.html` | per-device + detail Identify buttons, optional token field |
| `docs/onboarding_multiple_canaries.md` | new user-facing wizard guide |

## Verification

- **Build:** `cd firmware/projects/canary-wap && make upload` (or Arduino IDE).
- **Hostnames (2 boards):** `ping canary-aabb.local` and `ping canary-ccdd.local`
  resolve to distinct IPs; `ping canary.local` resolves to exactly one;
  `avahi-browse -rt _securacv._tcp` shows distinct `host`/`name`/`device_id` TXT.
- **Identify:** `curl -X POST http://<host>.local/api/identify -H "Authorization:
  Bearer <token>"` → LED triple-blinks + chirps ~15s; other endpoints stay
  responsive (confirms non-blocking).
- **Rename:** `POST /api/device-name {"name":"pantry"}` → reachable at
  `canary-pantry.local` without reboot.

> The delegated-hostname catch-all (`canary.local`) is the one path that should
> be confirmed on real hardware, since `mdns_delegate_hostname_add` behaviour
> can't be exercised off-device. If a core lacks it, the unique hostname still
> works and the build degrades gracefully (see the `ESP_IDF_VERSION` guard).
