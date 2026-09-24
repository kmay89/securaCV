# Device Trust (per-device PKI)

How SecuraCV proves an MQTT-published event actually came from your
Canary, why this matters, and the four UI states the Home Assistant
integration surfaces.

## TL;DR

Each Canary that witnesses has an Ed25519 keypair, generated on first
boot, persisted in NVS (the Canary Display line has none and signs
nothing). The firmware signs every `chain`, `events`, and `counts`
publish with that key. Home Assistant pins each device's public key
the first time it appears (TOFU) and **verifies every subsequent
publish** before letting it update entity state. A mismatch fires a
persistent notification but doesn't drop the payload — your entities
still update, but they're marked as **unverified** in their attributes.

If you only care about the upshot: **a hostile MQTT broker cannot
spoof a Canary**, and **a re-flashed Canary will surface a clear
"key changed" notification** instead of silently going unverified.

## What's signed

| Topic | Signed | Verified by |
|-------|--------|-------------|
| `{prefix}/{device}/chain` | Yes | `sensor.canary_<id>_chain_length` + `binary_sensor.canary_<id>_chain_valid` |
| `{prefix}/{device}/events` | Yes | `sensor.canary_<id>_last_event` |
| `{prefix}/{device}/counts` | Yes | `sensor.canary_<id>_witness_count` |
| `{prefix}/{device}/health` | No (carries pubkey for TOFU) | — |
| `{prefix}/{device}/status` | No (used for availability only) | — |
| `{prefix}/{device}/mesh` | No (operational telemetry) | — |

The signature is over a fixed canonical message per topic. For chain:

```
securacv-canary-sig|v1|chain|<device_id>|<length>|<latest_hash_hex>
```

For events:

```
securacv-canary-sig|v1|event|<device_id>|<event_id>|<state>|<category>|<privacy>|<motion>|<breath>|<bpm>
```

For counts:

```
securacv-canary-sig|v1|counts|<device_id>|<total>
```

Ed25519 over the raw UTF-8 bytes. The 64-byte signature is base64url-
encoded (no padding) and shipped as `sig` alongside the existing
fields. The publish also carries `fp` (the 16-char fingerprint),
`alg=ed25519`, and `v=1` so HA can validate without inferring.

Each topic also includes a `v` schema version so the canonical format
can evolve without breaking deployed Canaries — a `v=2` payload on a
`v=1`-verifying HA falls through to mismatch (refuses to silently
upgrade).

## Trust states

The PKI sig outcome is stamped onto each signed entity's
`extra_state_attributes` so dashboard cards can surface it directly:

| `trust_reason` | What it means | When it appears |
|----------------|---------------|-----------------|
| `ok` | Sig verifies against the pinned pubkey | Steady state on a healthy deployment |
| `tofu_pin` | First sight; we just auto-pinned this device | First publish from a freshly-added Canary |
| `unsigned` | Payload missing `sig`/`fp`/`alg` | Older firmware that doesn't sign yet |
| `no_pubkey` | We have a sig but no pinned pubkey yet | Briefly, before the health topic delivers `public_key` for TOFU |
| `mismatch` | Sig doesn't match the pin, OR fingerprint changed | Re-flashed Canary, MITM, broker compromise |

`verified` is a boolean derived from the above: only `ok` and
`tofu_pin` count as verified.

## TOFU enrollment (Trust On First Use)

The default flow is zero-touch:

1. Canary boots and starts publishing health every 60 s. The health
   payload includes `"public_key":"<hex>"`.
2. HA's MQTT subscriber sees the health publish, reads `public_key`,
   and **TOFU-pins it** as the trusted identity for that device_id.
3. Subsequent `chain`/`events`/`counts` publishes are verified against
   the pin.

TOFU is sticky: the same `device_id` showing up later with a different
pubkey hits the **mismatch** path. It does NOT auto-replace.

## Manual pinning (out-of-band enrollment)

If you don't trust your MQTT broker enough for TOFU — for example,
you're sharing a broker with a tenant or running on a shared LAN —
you can pin the pubkey before the device ever publishes. This is the
"strict" mode the PKI design supports.

1. Read the device's `device_id` and its full 64-character public key
   hex off the device itself, from the source the table below names
   for its product. Not every product has one.
2. In HA → **Settings** → **Devices & services** → **SecuraCV** →
   **Configure** → **Pin a device pubkey**.
3. Enter the `device_id` and paste the 64-char pubkey hex.

The manual pin overrides any existing TOFU pin (the previous pubkey
is retained in the audit trail).

### Where each product shows its key

Out of band means not over MQTT: the broker is what you are declining
to trust, so the key has to come off the device by another path. The
pin form needs the full public key. The 16-character fingerprint is
enough to *check* a pin ([below](#checking-a-tofu-pin-against-the-device))
but not to set one. Only canary-wap serves an `/enroll` page.

| Product | Full public key, out of band | Fingerprint only |
|---|---|---|
| **canary-wap** | The `/enroll` page (and `/api/device/enroll`, the same card as JSON), no login: `device_id`, fingerprint and full key. Open it at `canary-<name>.local/enroll`, or `canary-<first four hex of the fingerprint>.local/enroll` on an unnamed device, or at its IP. Once setup is done the WAP serves HTTPS with a certificate it made itself, so `http://` redirects and the browser warns about the certificate; during first-boot setup it is plain HTTP on the setup network. On USB serial, `i` prints the identity block with the full key (the line ends in a literal `...` after the 64 characters: don't paste the dots). | The provisioning receipt it prints on USB serial at every boot carries `pubkey_fp`. |
| **`firmware/canary` build** (and the ESP32-CAM, Freenove S3 and WROOM builds of it) | No `/enroll`. On USB serial, `i` prints the device ID and full key, and `j` prints the self-manifest (`device_id`, `pubkey`, `pubkey_fp`). `GET /api/status` also returns `pubkey` and `fingerprint`, but only with the device's bearer token. | On USB serial, `f` prints the fingerprint. The dashboard's Device Identity card shows the fingerprint and only the first 16 characters of the key, and only on a page that was handed the token (during setup, over the SoftAP, with the bearer, or after one BOOT tap). The provisioning receipt carries `pubkey_fp` only. |
| **canary-vision** | No web server. On USB serial, `j` prints the self-manifest (`device_id`, `pubkey`, `pubkey_fp`). | The boot log prints `Ed25519 ready  fp=<fingerprint>`. |
| **canary-sense** | **None.** No web server, and its serial console is the tuning console, which has no identity command. You can check its TOFU pin but not set one by hand. | The boot log prints `Ed25519 ready  fp=<fingerprint>`, once, at boot. |
| **canary-sentinel** (not released; has not run on hardware) | **None**, in source: no web server and no serial commands. | The boot log's `Ed25519 ready  fp=<fingerprint>` line. |
| **canary-display** line | Nothing to pin: a display has no signing key, and its health publish carries no `public_key`. The proof QR on its screen carries the key *the display* pinned from the same broker, which is a second TOFU, not an out-of-band read. | — |

The apps and flashers read the same sources:

- **The in-browser flasher's** serial monitor has `j` and `i` buttons
  and shows the raw lines, so a key the board prints is there to copy.
  When the board answers `j` (the `firmware/canary` build,
  canary-vision), its identity card shows the key fingerprint, grouped
  `aa:bb:…`.
- **The desktop Flasher's** serial monitor has a **Print receipt (j)**
  button and shows the raw lines. Its receipts panel does not print the
  fingerprint.
- **The iPhone app** pins the key of a Canary paired to it (canary-wap
  or the `firmware/canary` build, the two that serve a provisioning
  receipt) from `/api/status` on first sight, over the LAN rather than
  the broker, and shows the fingerprint under **Keys** → **Pinned
  trust**. It shows no full key, so it can check a pin, not set one.

This table is read from the firmware and app sources (the canary-wap
route in `canary_wap.ino`'s `register_api_routes()`, the handlers in
`firmware/common/identity/device_signature.cpp`, each product's serial
command handler and `witness.cpp`); it has not been walked on a bench
for every product.

### Checking a TOFU pin against the device

Every product that signs shows its fingerprint somewhere in the table
above, even where it can't show its full key. After the first health
publish, compare the `pinned_fingerprint` attribute on the device's
entities (the chain-length sensor, for one) with the fingerprint read
off the device, ignoring case: HA shows lowercase, and some device
surfaces print capitals (everything a canary-wap prints except the
public key on `/enroll`, and the `firmware/canary` build's
`/api/status` and receipt). A match is strong evidence that the key HA
pinned on first sight is the one the device holds. A difference means
something else is pinned: **Unpin a device**, fix whatever let the
other key in (broker ACLs), and compare again after the next TOFU pin,
or pin by hand where the product shows its full key. The fingerprint is
the first 8 bytes of a hash of the key
(`device_trust.fingerprint_from_pubkey_hex`), so it can check a pin but
cannot set one.

## Key rotation

When you legitimately re-flash a Canary (firmware update that wipes
NVS, or moving an SD card to fresh hardware), the new keypair won't
match the pin and HA fires a **persistent notification** plus marks
the entities unverified.

To clear it:

1. **Settings** → **Devices & services** → **SecuraCV** → **Configure**
   → **Rotate a pinned device key**.
2. Enter the device_id and the new pubkey, read off the re-flashed
   Canary from its source in
   [the table above](#where-each-product-shows-its-key). A canary-sense
   or canary-sentinel shows no full key: unpin it instead, then
   [check the new TOFU pin](#checking-a-tofu-pin-against-the-device).
3. The previous pubkey is moved into the device's `previous` audit
   trail; the mismatch notification clears; entities verify cleanly
   on the next publish.

If you can't recover the new pubkey (e.g. you lost the device), use
**Unpin a device** instead. The next publish TOFU-pins to whatever
key arrives. **Use this with care** — it dissolves the previous
identity.

## What the persistent notification looks like

```
SecuraCV: device <id> key mismatch

Canary `<id>` published with fingerprint `<new_fp>` but the pinned
fingerprint is `<old_fp>`. Entities are still updating, marked as
unverified. If you intentionally re-flashed this device, rotate the
pin from the integration's options menu.
```

We dedupe by `(device_id, received_fingerprint)`, so a steady stream
of mismatched publishes only fires the notification once until the
operator either rotates or unpins.

## Why pin, rotate and unpin are not actions

The integration registers Home Assistant actions for watches
(`securacv.start_watch`, `securacv.end_watch`, `securacv.list_watches`
— see [Home Assistant setup](homeassistant_setup.md#actions)) and
deliberately none for trust. Pinning, rotating and unpinning a device
key happen only in the options flow, by a person.

The reason is the mismatch path above. When a Canary shows up with a
key that doesn't match its pin, the notification asks *you* to rotate,
because only you know whether you re-flashed it. A `securacv.rotate_key`
action would let an automation answer that question instead: "on a key
mismatch notification, rotate to the received key" is one YAML rule,
and it turns a re-flashed or impersonating device into a trusted one
without anyone deciding it was theirs. That is exactly what pinning
exists to catch. Actions are authenticated, but an automation reacting
to the attacker's own publish is not a person checking the key on the
device itself.

Watches are the opposite case. Starting one only adds attention, and
ending one removes only attention, never trust, and the early end is
announced like an expiry. So those are actions, and the key decisions
stay with a person. Bulk import of pubkeys (below) is a separate
decision: if it ever becomes callable from an automation it needs a
written rule that it never overwrites an existing pin.

## Threat model

What this defends against:

- **Hostile MQTT broker.** A broker operator can publish arbitrary
  data on any topic, but can't produce a valid signature without the
  device's private key.
- **MITM on the MQTT link.** Same as above — unsigned/forged payloads
  are caught.
- **Replay across devices.** The `device_id` is in the signed
  canonical, so a sig from device A can't be replayed as device B.
- **Replay across topics.** The topic kind (`chain`/`event`/`counts`)
  is in the canonical, so a chain sig can't be replayed as an event sig.

What this does NOT defend against:

- **Compromised firmware** that leaks the privkey or signs lies. You
  need the existing tamper-evident witness chain + SD persistence for
  that — PKI is identity, not integrity of the device itself.
- **Replay of an exact past publish.** An attacker with broker access
  who captures a real `(device_id, event_id=42, sig)` triple can
  re-publish it. Mitigation: HA's per-device-monotonic `event_id`
  tracking (already in `s_last_published_event_id`) can detect
  duplicates — surfacing that as a separate "replay observed" sensor
  is a follow-up.
- **Pre-TOFU broker spoofing.** If the very first time HA sees a
  device is on a hostile broker, TOFU pins the wrong key. Use manual
  pinning if your broker isn't trusted, on a product that shows its
  full key out of band; on one that doesn't (canary-sense,
  canary-sentinel), check the TOFU pin's fingerprint against the
  device instead
  ([where each product shows its key](#where-each-product-shows-its-key)).

## How to verify

Host-side tests (no ESP32 + no HA needed):

```bash
make -C firmware/projects/canary-wap/tests_host       # firmware: canonical builders + b64url
python3 -m pytest custom_components/securacv/tests/   # HA: trust store + verify roundtrip
```

In the field, with a freshly-flashed Canary on the same network as a
freshly-installed HA:

1. Add the SecuraCV integration in HA, MQTT-only mode.
2. Wait 60 s for the first health publish — check HA's log for
   `TOFU-pinning Canary <id> with pubkey <hex>…`.
3. Open the chain sensor's attributes — `verified: true`, `trust_reason: ok`,
   `pinned_fingerprint` matches `received_fingerprint`.
4. Flash a different firmware build to the same Canary (or wipe NVS to
   regenerate the keypair). On the next publish, expect:
   - A `SecuraCV: device <id> key mismatch` persistent notification.
   - `verified: false`, `trust_reason: mismatch` on the sensor.
   - Entities continue updating (warn-loudly-accept policy).
5. Rotate the pin via **Configure** → **Rotate** → paste the new pubkey,
   read off the device
   ([where each product shows its key](#where-each-product-shows-its-key)).
   Notification clears, `verified: true` on the next publish.

## What's NOT solved yet

Stream C v1.1 plan, after this PR:

- **HA dashboard card** that surfaces the pinned-fingerprint state for
  every Canary at a glance (right now you have to drill into entity
  attributes).
- **Replay-detection sensor** based on `event_id` monotonicity.
- **Bulk import** of pubkeys from a YAML file for installs with many
  devices (current options flow is one-at-a-time).
- **CRL / revocation list** for "this fingerprint is known bad" so
  multi-tenant installs can blocklist captured keys.
