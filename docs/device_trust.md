# Device Trust (per-device PKI)

How SecuraCV proves an MQTT-published event actually came from your
Canary, why this matters, and the four UI states the Home Assistant
integration surfaces.

## TL;DR

Each Canary has an Ed25519 keypair, generated on first boot, persisted
in NVS. The firmware signs every `chain`, `events`, and `counts`
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

1. Open the Canary's `/enroll` page in any browser on the same LAN
   (e.g. `http://canary-<fp>.local/enroll` or the device's IP). The
   page renders the fingerprint in big monospace text plus the full
   pubkey hex.
2. In HA → **Settings** → **Devices & services** → **SecuraCV** →
   **Configure** → **Pin a device pubkey**.
3. Enter the `device_id` and paste the 64-char pubkey hex.

The manual pin overrides any existing TOFU pin (the previous pubkey
is retained in the audit trail).

## Key rotation

When you legitimately re-flash a Canary (firmware update that wipes
NVS, or moving an SD card to fresh hardware), the new keypair won't
match the pin and HA fires a **persistent notification** plus marks
the entities unverified.

To clear it:

1. **Settings** → **Devices & services** → **SecuraCV** → **Configure**
   → **Rotate a pinned device key**.
2. Enter the device_id and the new pubkey (read from `/enroll` on the
   re-flashed Canary).
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
  pinning if your broker isn't trusted.

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
5. Rotate the pin via **Configure** → **Rotate** → paste the new pubkey
   from `/enroll`. Notification clears, `verified: true` on the next
   publish.

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
