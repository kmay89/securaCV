# Canary QR Onboarding — point it at the glass

The display becomes the commissioner: it mints a provisioning QR carrying
its own Wi-Fi, the hub address, and a short-lived fast-track token; a
camera canary is powered on, pointed at the glass, and joins. Research
base: Wyze/Tuya device-scans-screen flows (including Wyze's CVE'd shell
injection through the SSID field of a provisioning QR — the reason our
parser treats every field as hostile), Nest/Ring distance coaching,
Improv Wi-Fi's state machine, ESPHome/Matter LED conventions.

## The grammar (`firmware/common/provision_qr/provision_qr.h`)

Shared, header-only, host-tested. Two payloads, tried in order:

**Primary — `SCV1` compact pipe-KV (what the display mints):**

```
SCV1|s=MyHomeWiFi|p=hunter2pass|h=192.168.4.20|o=1884|t=<22-char>|x=1789700000|n=Porch
```

`s` SSID (required) · `p` passphrase · `h` hub host (absent = ask the
fleet) · `o` hub port (absent = 1883) · `t` single-use fast-track token
(128-bit, base64url) · `x` unix expiry (mint + 10 min) · `n` suggested
name. `\|` and `\\` escape inside values; unknown keys are skipped
(`g=` is reserved for a future factory fleet key).

**Secondary — the universal phone format:** `WIFI:T:WPA;S:…;P:…;;`
(ZXing escaping, both the Android and iOS dialects accepted). Any phone
can mint one from its share-Wi-Fi screen — the break-glass path when no
display is handy. It carries Wi-Fi only, so a canary joining this way
finds the hub by asking the fleet (mDNS gossip) and waits for a human
blessing on the display.

**Input hardening (the Wyze lesson):** every field is length-capped,
numerics and the token charset are validated, over-cap or malformed
fields reject the whole payload, and parsed values are never handed to a
shell, format string, or allocator. Payloads over 512 bytes are ignored
outright.

**Why not encrypt the QR:** the Wi-Fi password is already on the user's
own screen at hand distance; secrecy comes from physical proximity plus
the 10-minute expiring token, not payload crypto. Chain trust stays
TOFU + on-glass blessing, same as every other join path.

## The display side (this wave)

- **Entry points:** watch — "add a canary" in the settings tree, or
  long-press the hero page when the fleet is empty ("no canaries yet ·
  hold to add"); dash — "Add a canary" on the transparency sheet.
- **Rendering:** dark-on-white card with a real quiet zone, never
  inverted (the iOS Smart-Invert failure class). Watch: 148 px code
  inside the round glass's inscribed square, payload capped at 84 bytes
  (QR v4/v5 — big modules beat error correction for a fixed-focus lens);
  the cap sheds hub host then expiry, and if the Wi-Fi credentials alone
  don't fit, the watch says to use the wall panel instead of rendering
  an unscannable code. Dash: 360 px code plus coaching copy (distance,
  glare, hold steady).
- **Freshness:** countdown on the glass; expired codes silently re-mint
  with a fresh token — no error state to squint at. Backlight pins to
  full while the code is up.
- **The celebration is transport-agnostic:** the moment any new witness
  appears while the surface is open, the glass celebrates and closes.
  QR, phone Wi-Fi code, or captive portal — all paths end the same way.
- A live unacked alert closes the surface instantly; onboarding never
  outranks an alarm.

## The canary side (canary-wap: shipped)

The WAP already carried a quirc scanner (vendored, ISC — an attestable
QR input path) behind `FEATURE_QR_PROVISION`; the onboarding wave taught
it the shared grammar and made it scan on its own:

- **Shared grammar first:** the scan task parses `SCV1` and the modern
  `WIFI:` dialects via the sync-guarded copy of
  `firmware/common/provision_qr/provision_qr.h` (the legacy `SECURACV:`
  wizard format still works). Expired `SCV1` codes fail fast — before
  any join attempt — when the clock is valid.
- **Boot scan-to-join:** an unprovisioned canary with a usable camera
  runs 60 s scan windows with a short breather, forever — power it on,
  point it at the display's code, done. No phone, no session. A phone
  captive-portal session takes the camera over cleanly at any time.
- **Session vs. proximity auth:** the display-minted `SCV1` path carries
  its own expiring token, so the phone-session pair-token gates don't
  apply to it; a plain `WIFI:` code in boot-scan mode joins Wi-Fi and
  leaves trust to the display's blessing.
- **Hub handoff:** `h=`/`o=` from the code point the canary's MQTT
  bridge at the hub immediately (config saved + re-init), so the fleet —
  and the display's celebration — sees it the moment Wi-Fi comes up.
- **It answers out loud:** credentials accepted = ascending chirp (LED
  blink on silent hardware); stale code = error buzz. Garbled or foreign
  codes never end the scan — it says so and keeps watching.
- **Still to come:** the full count-coded LED grammar (below) and the
  token echo in the first hello; the display matches it against
  `commission_ui_token()` for automatic blessing. Without a token
  (plain `WIFI:` join), the display asks for one tap.
- **LED grammar** (single-color cadence carries the meaning; color is
  reinforcement; count-coded errors so a user can *say* what they see):

| State | Cadence |
|---|---|
| Waiting for a code | 100 ms on / 900 ms off |
| Code read | 3 quick blinks, then 500 ms solid |
| Joining Wi-Fi | rapid even 100/100 ms |
| Wi-Fi up, finding hub | double-blink + 600 ms off |
| Enrolled | solid 3 s, then dark (a witness shouldn't glow) |
| Wrong Wi-Fi password | groups of 2 red blinks |
| No IP / AP unreachable | groups of 3 |
| Hub unreachable | groups of 4 |
| Code expired/reused | groups of 5 |
| Unreadable code | 2 long blinks |

  After 30 s of any error group the canary returns to scanning on its
  own — every retry re-announces its state (the anti-Wyze rule: never a
  silent forever-loop).
- **Camera-less siblings (ESP32-C6):** still to come — Improv Wi-Fi over
  BLE with the display as commissioner: the display spots the
  advertisement, asks "new sensor found — add it?", one tap writes the
  credentials, and the same enrollment path finishes the job. The SoftAP
  fallback it layers over is no longer hypothetical: an unprovisioned (or
  recovery-stuck) Sense or Vision already raises the shared setup
  portal's `SecuraCV-XXXX` network on its own
  (`firmware/common/network/setup_portal`) — no long-press needed — so
  BLE commissioning arrives as convenience on top of a shipped
  break-glass path.
