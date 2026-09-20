# Witness page API v1 — `GET /api/v1/witness`

**Status:** v1.0 — one contract, two chain formats. The canary-wap side is
host-tested (`firmware/projects/canary-wap/tests_host/test_witness_page.cpp`
renders the shared fixture byte-for-byte and verifies every signature with
OpenSSL); the reference device-api side is exercised by its Node tests; the
iPhone decoder and verifier (`ios/Sources/SecuraCV/Model/WitnessChain.swift`,
`ios/Sources/SecuraCV/Security/ChainVerifier.swift`) are **compile-untested**
in this repository — the gated Apple CI is their compiler.

This is the page a phone, a wall, or a script reads to verify a Canary's
witness chain on its own, with no vendor in the loop. It was written because
the two ends had drifted apart: the app fetched `/api/v1/witness` in the shape
the canary-vision reference server serves, while the canary-wap firmware
answered only `/api/witness` — one record, another shape, no signature
(roadmap row 6). Both now serve **this** page. Where they legitimately differ
(how the chain hash is built, what the signature covers) the page says so in
one field, `chain_format`, and a reader picks the matching verifier.

Nothing here invents a signing scheme. Each format below is exactly what its
firmware already signs on disk; the page only puts those bytes on the wire.

## 1. Request

```
GET /api/v1/witness?last=N
```

| Param | Default | Range | Meaning |
|---|---|---|---|
| `last` | 20 | 1–100 | Number of newest records to return. Absent or malformed → 20; out of range → clamped. |

Authentication is the device's own: the reference device-api reads
`X-Canary-Token`, canary-wap reads `Authorization: Bearer <token>` (the
same token the pairing receipt carried). A client that sends both speaks to
both. The route is never public: it is in canary-wap's route-security
allowlist only as a *gated* route (`tests_host/check_route_security.py`).

## 2. Response

```json
{
  "schema": "securacv/witness_page/v1",
  "chain_format": "wap_v1",
  "device_id": "canary-fixture-0001",
  "total": 4,
  "uptime_s": 700,
  "records": [ { …record… }, … ]
}
```

| Field | Type | Required | Meaning |
|---|---|---|---|
| `records` | array | yes | Oldest first. The last element is the chain head as of this page. May be empty (a canary-wap's ring is RAM-only and starts empty at every boot). |
| `chain_format` | string | no | Which chain construction the records use: `reference_v1` or `wap_v1` (§4). **Absent means `reference_v1`** — the reference device-api predates this field. A reader that meets a format it does not know must not badge the page anything stronger than *unverified*. |
| `schema` | string | no | `securacv/witness_page/v1` when present. |
| `device_id` | string | no | The device's own id (the `wap_v1` genesis input, §4.2). |
| `total` | integer | no | The chain length: the newest `seq` this key has issued. May exceed the newest `seq` on the page only if records were issued between the ring read and the header write; it never falls below it. |
| `uptime_s` | integer | no | Seconds since boot at render time — the same value as `/api/status` `uptime_sec`. The anchor for records that carry no `timestamp` (§3.2). |

Unknown top-level fields are ignored.

### 2.1 Record

| Field | Type | Required | Meaning |
|---|---|---|---|
| `seq` | integer | yes | 1-based, contiguous per device key. |
| `hash` | hex string | yes | This record's chain hash (64 hex). |
| `prev_hash` | hex string | yes | The previous record's `hash`, or the genesis value for `seq` 1 (§4). |
| `event_type` | string | yes | A vocabulary word (§5). |
| `zone` | string | no | A local, opaque zone id; the empty string when the device has no zone concept (canary-wap). Absent decodes as `""`. |
| `signature` | hex string | no | Ed25519 over the message §4 names for the format, 128 hex. **Absent or empty means the record is not individually signed.** A reader must not badge such a record *verified*; the strongest honest word is *unsigned*. |
| `timestamp` | string | no | ISO-8601 UTC. **Coarse** — see §3. Absent when the device has no believable wall clock. |
| `time_source` | string | no | `device_clock` or `gps_utc`. Present only with `timestamp`. Absent decodes as `device_clock`. |
| `gps_timestamp` | string | no | `reference_v1` only: the GPS-derived UTC string when `time_source` is `gps_utc`; absent otherwise and hashed as the empty string. |
| `payload_hash` | hex string | `wap_v1`: yes | Domain-separated SHA-256 of the record's payload (§4.2). |
| `time_bucket` | integer | `wap_v1`: yes | The coarse uptime bucket bound into the chain hash (§4.2). |
| `time_bucket_ms` | integer | `wap_v1`: yes | The bucket width in force when the record was made (a runtime setting, so it rides per record). |
| `record_type` | integer | no | The raw firmware record-type enum, for the offline verifier's benefit. |

Unknown record fields are ignored (the reference server also sends
`thumbnail`, `gps_fix_quality`, `gps_satellites`, `gps_fix_age_ms`).

## 3. Time on this wire — Invariant III

`spec/invariants.md` Invariant III: a Canary never publishes a precise
timestamp. On this wire that is two floors, and a reader must know both:

* **Wall-clock time is never finer than a ten-minute bucket.** `timestamp`,
  when present, is a bucket start (§3.1), and a reader presents every record
  as a bucket.
* **The chain binds an *uptime* bucket at its native width.** canary-wap's
  chain hash covers `time_bucket = millis() / time_bucket_ms` (§4.2), where
  `time_bucket_ms` is a runtime setting clamped to the firmware's floor
  (`TIME_BUCKET_MS`, 5 000 ms as shipped; `/api/config` reports it as
  `time_bucket_floor_ms`). The page carries `time_bucket` and
  `time_bucket_ms` because the hash cannot be recomputed without them. This
  is not new exposure: the same value already rides the SD witness line,
  `GET /api/witness`, `POST /api/export` and the MQTT `chain` publish, and
  `uptime_s` is `/api/status`'s `uptime_sec` — every one of them behind the
  same Bearer token. It does mean an authenticated reader can place a record
  to the bucket's width *relative to boot*, and, with any anchor, in wall
  time. A reader MUST still present it coarsened (§3.1, §3.2); widening the
  chain's floor to the ten-minute grid is a firmware decision recorded as
  open in `docs/IMPROVEMENT_ROADMAP.md` (it touches both products' witness
  chains, their config floors and the operator copy that names 5 s), not
  something this contract can do on its own.

### 3.1 `timestamp` is a bucket start

`timestamp`, when present, is the start of the record's ten-minute bucket
(`floor(t / 600 s) × 600 s`), never the second the record was made. A reader
displays it as a bucket ("around 10:20"), never as an instant.

The reference device-api's `timestamp` carries fractional seconds because it
is part of that format's hash pre-image (§4.1); a reader still **presents** it
coarsened. The Swift `TimelineEvent.timeBucket` is exactly that floor.

### 3.2 No clock, no `timestamp`

A canary-wap has no RTC and no SNTP; it learns the date from GPS when it has
one. Until then it emits **no** `timestamp` at all rather than a 1970 date.
`time_bucket` and `time_bucket_ms` still ride the wire because the chain hash
binds them (§4.2), and with `uptime_s` a reader can place the record in time
relative to *its own* fetch:

```
age_s     = uptime_s − time_bucket × time_bucket_ms / 1000
timestamp = floor((fetched_at − age_s) / 600 s) × 600 s
```

That is the same anchoring the WAP events feed uses
(`ios/Sources/SecuraCV/Wire/WapEvents.swift`), and it stays coarse.

## 4. Chain formats

A reader recomputes every record's `hash` from the record's own fields,
checks that each `prev_hash` equals the previous record's `hash`, and then
verifies the **head's** `signature` against the key it pinned for this device
on first sight (TOFU — `ios/Sources/SecuraCV/Security/Keychain.swift`
`PinnedKeyStore`). Older records on the page are covered by the chain to the
head. A page whose head carries no signature is *unsigned* even if older
records are signed.

"Verified" means precisely that check passed against a pinned key
(`AGENTS.md` rule 4). No field on this wire is itself a verification — in
particular canary-wap's own post-sign self-check (`WitnessRecord::verified`)
is deliberately **not** serialized.

### 4.1 `reference_v1` — the canary-vision device-api

Source of truth: `canary-vision/device-api/lib/witness-chain.js` and
`canary-vision/docs/api.md`.

```
preimage  = "${seq}:${prev_hash}:${timestamp}:${event_type}:${zone}:${time_source}:${gps_timestamp}"
hash      = hex( SHA-256( utf8(preimage) ) )
signature = Ed25519( key, utf8(hash) )          // over the 64-char hex STRING
genesis   = "000…0" (64 zeros)
```

`time_source` defaults to `device_clock`; `gps_timestamp` is the empty string
when absent. The Swift pre-image is pinned against device-produced vectors in
`ios/Tests/SecuraCVTests/WitnessChainPreimageTests.swift`.

### 4.2 `wap_v1` — canary-wap firmware

Source of truth: `canary_wap.ino` `compute_chain_hash` /
`create_witness_record`, mirrored offline by `tools/verify_witness_log.py` and
on disk by `witness_store.h` (`/WITNESS/records.jsonl`).

```
sha256_domain(d, m) = SHA-256( ascii(d) || 0x00 || m )

payload_hash = sha256_domain("securacv:fw:payload:v1", payload)      // payload never leaves the device
hash         = sha256_domain("securacv:fw:chain:v1",
                             prev_hash(32) || payload_hash(32) || seq(BE32) || time_bucket(BE32))
signature    = Ed25519( key, hash(32) )                                // over the RAW 32 bytes
genesis      = sha256_domain("securacv:genesis:v1", ascii(device_id))
```

The signature covers only the chain hash, so a reader **must** recompute
`hash` from `prev_hash`, `payload_hash`, `seq` and `time_bucket` before
trusting the signature — otherwise an edited `seq` with a genuine
hash/signature pair would pass (the seq-binding rule `witness_store.h`
documents for the SD tail, applied to the page). A `wap_v1` record missing
any pre-image field cannot be recomputed and reads as a broken link.

The page carries `device_id`, so a reader that sees `seq` 1 MAY check its
`prev_hash` against the genesis value. It is optional: a `last=N` page rarely
reaches back to `seq` 1.

## 5. `event_type` vocabulary

`reference_v1` uses the reference server's names (`person_detected`,
`vehicle_detected`, …, the dictionary ids where they exist).

`wap_v1` records name the firmware **record type**, not the sensing event
inside it — the payload never leaves the device, only its hash does:

| `record_type` | `event_type` | Meaning |
|---|---|---|
| 0 | `boot_attestation` | The boot record; first record of every boot. |
| 1 | `witness_event` | A committed sensing event (its content is in the hashed payload). |
| 2 | `tamper_detected` | A tamper record — the dictionary id, so every reader's tamper severity fires. |
| 3 | `state_change` | A GPS/fix state transition. |
| 4 | `power_shutdown` | Reserved (the offline verifier's name). |

The offline verifier names type 2 `tamper_alert`; the page uses the
dictionary id on purpose (`spec/witness_dictionary.json`). Readers resolve
unknown words through their vocabulary's calm default (`.notice` in the
apps), never as an error.

## 6. Where each side lives

| Side | File | Tested by |
|---|---|---|
| Fixture (both ends decode it) | `spec/fixtures/witness_page_v1.json`, written by `spec/fixtures/gen_witness_page_v1.py` (`--check` in CI) | — |
| canary-wap renderer + ring | `firmware/projects/canary-wap/arduino/canary_wap/witness_page.h`; handler `handle_witness_v1` in `canary_wap.ino` | `tests_host/test_witness_page.cpp` (host; byte-exact + Ed25519) |
| Reference device-api | `canary-vision/device-api/routes/witness.js` | `canary-vision/tests/api/witness.test.js` |
| iPhone decoder + verifier | `ios/Sources/SecuraCV/Model/WitnessChain.swift`, `ios/Sources/SecuraCV/Security/ChainVerifier.swift` | `ios/Tests/SecuraCVTests/WitnessPageFixtureTests.swift` (compile-untested here) |

The fixture's signing seed is a readable ASCII string and is never a device
key; its public key is the constant both tests embed.

## 7. What canary-wap keeps, honestly

The ring behind the page holds the newest 16 records in RAM
(`witness_page::RING_CAP`) and is empty after every reboot; `last` larger than
that returns what is held. The durable history is the SD log
(`/WITNESS/records.jsonl`), verified offline by `tools/verify_witness_log.py`.
The old `GET /api/witness` (one record, the device's own self-check flag)
remains for the device's dashboard and is not this contract.

`payload_hash` is new on an HTTP surface (the old page sent only
`time_bucket` and `chain_hash`). It is domain-separated but unkeyed, so a
holder of the Bearer token can confirm a guess at a low-entropy payload — a
short state-transition string, say — by hashing the guess. That is the same
exposure the SD line (`ph`) and the PWK export already carry, not a new one;
the page adds no plaintext and no key, and the token it sits behind is the
same one that reads the SD log.
