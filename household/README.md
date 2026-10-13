# household-keys

The hub side of SecuraCV's **household keys**. A household member presents a
key they carry and the hub turns that deliberate, signed act into a
**household posture**: `away`, `home` or `night`. The key can be a phone or
watch key behind that device's own user verification, a FIDO2 key, a fob or an
NFC card. The hub publishes the posture as a fixed 112-byte statement signed
with its Ed25519 posture key. Nothing here is a biometric, and nothing here
learns who anyone is. The design record is
[`docs/research/face_id_without_faces.md`](../docs/research/face_id_without_faces.md)
§6.

- Package `household-keys`, library `household_keys`, binary `household_checker`.
- A standalone crate: an empty `[workspace]` and its own `Cargo.lock`. It is
  **not** a feature of `witness-kernel`, and neither crate depends on the
  other. The split means kernel code cannot call the checker. Keeping
  witnessd's Unix user away from the checker's files is a separate,
  deployment-time measure (a *won't*, not a *can't*).
- CI: [`.github/workflows/household.yml`](../.github/workflows/household.yml)
  runs fmt, clippy with `-D warnings`, rustdoc with `-D warnings`, the tests
  and an MSRV 1.89 check. [`.github/workflows/audit.yml`](../.github/workflows/audit.yml)
  runs `cargo audit` on the lockfile and `cargo deny` with the repo's
  `deny.toml`.

## Status

What is built and tested on a host:

- the roster, with its change rules and rollback refusal;
- single-use challenges;
- the `es256` and `fido2-es256` verifiers;
- the NTAG 424 DNA `AuthenticateEV2First` reader state machine, checked
  against NXP AN12196 transcripts;
- the sealed card-key store;
- the posture statement and every witness-side check;
- the LAN API;
- a reference software credential.

What is **not built**:

- **Key-satellite firmware.** This is the door device: it tunnels APDUs to
  cards and FIDO2 keys and emulates a card for phones. The NTAG and FIDO2
  code here runs against a simulator and test assertions. No satellite drives
  it yet.
- **Phone and watch apps.** `household_checker client` is a reference
  software credential, not a secure one: its key is a plain file and it asks
  nobody anything before it signs.
- **MQTT.** v0 publishes the statement only to the output seam file and
  `GET /v1/posture`. The draft's retained broker topic does not exist.
- **Witness consumption.** No Canary verifies posture statements yet.
  `posture::verify` is the witness-side contract.
- **TLS on the LAN API.** See the limitations below.

## The threat model, in brief

**What a key does.** A key changes the posture and nothing else. The posture
applies to every event equally. A credential never rewords, demotes, quiets or
delays a particular event, and no output identifies a member or a credential.

**Intent lowers, possession only raises.** Only an *intent* credential may set
`home`, the posture that lowers indoor sensing. A *possession* credential may
set only `away` or `night`. Cards and fobs are always possession, and so is a
FIDO2 key whose assertion lacks the UV flag. The hub cannot prove that a phone
key requires user presence. The admin quorum declares the class at enrollment.

**Freshness and replay.** Freshness comes from a 32-byte nonce drawn from the
OS RNG. A challenge is good for one use, for 10 seconds, on the session that
asked for it. The hub checks membership against the *current* roster epoch,
and an epoch change discards every outstanding challenge. No signature
counter, usage tally or arrival history is kept for any credential. FIDO2
clone detection is traded away for that.

**`home` lapses.** A `home` statement lives at most 24 hours (12 by
default). A witness checks the lapse against its own clock. The hub replaces a
lapsed `home` with `away`. The hub also starts in `away` after every restart.

**The roster has no names.** Members and admins are random 16-byte handles.
Unknown JSON fields are refused, so no field can hold a name.

**Roster changes take approvals.** Each change needs the approvals the change
rules require, counted by distinct admin handle:

- **Adding a member** needs `threshold` approvals. When the threshold is 1, it
  also needs a confirm at the hub.
- **Revoking a member** needs one admin, or the member's own credential.
- **Changing the admin set or the threshold** needs `threshold` approvals from
  the *current* admins, after 24 hours pending.

Every change is `epoch + 1` and links to the previous roster's digest. Every
accepted roster is countersigned by the hub's Ed25519 roster key. The chain is
re-checked on every load. The checker refuses any roster below the highest
epoch it has accepted, and any different roster at that epoch.

**Card keys are sealed.** Card keys are the only secrets in the roster. They
are sealed with XChaCha20-Poly1305 under a key-encryption key (KEK) held in a
file outside the data directory. A hub compromise still means re-keying every
card.

**What the network sees.** Every rejection on the LAN API is the same `401`
with the same body, so no response says whether a handle exists. Five
rejections at one door within ten minutes start a 60-second cooldown for that
door. The count is kept in RAM, per door, and never per credential.

**Logs.** The server logs startup, shutdown and storage failures, and nothing
per request. Sentinel tests plant recognizable handles. They assert that none
of them appears in any HTTP response body, statement, seam write or log line.

## Running it

```sh
cd household
cargo build --release

# 1. A household: id, posture key, roster key. The KEK file goes OUTSIDE the
#    data directory and outside its backups.
household_checker init --data-dir /var/lib/securacv-household --kek /etc/securacv/household.kek
#    → prints the household id and the posture public key witnesses pin.

# 2. The first admin, then genesis (a confirm at the hub).
household_checker admin-keygen --out ~/hk-admin.json
household_checker roster genesis --data-dir D --admin-key ~/hk-admin.json --physical-confirm

# 3. Enroll a credential. `client keygen` makes a reference software key.
household_checker client keygen --out ~/phone.key           # → public_key
household_checker roster propose add-member --data-dir D \
    --kind phone --class possession --public-key <hex>       # → proposal, new_member_handle
household_checker roster approve --data-dir D --proposal <digest> --admin-key ~/hk-admin.json
household_checker roster apply --data-dir D --proposal <digest> --physical-confirm <first 8 hex of digest>

# 4. Pair a LAN client (prints a bearer token once) and serve on loopback.
household_checker pair --data-dir D > ~/hk-token
household_checker serve --data-dir D --kek /etc/securacv/household.kek   # 127.0.0.1:8787

# 5. Present the credential.
household_checker client command --key ~/phone.key --handle <handle> \
    --token-file ~/hk-token --purpose night
household_checker client posture --token-file ~/hk-token
```

More subcommands:

- `roster propose revoke | add-admin | remove-admin | set-threshold`
- `roster pending` lists proposals, their approvals and when an admin-set
  change becomes ready.
- `roster approve --approval-file F` imports an ES256 enclave or FIDO2 admin
  approval made elsewhere.
- `roster approve --holder-proof-file F` imports a member's own revocation
  proof.
- `card provision --handle H [--key-hex K]` puts a card key into the sealed
  store. Software only: writing the key onto a physical card is not part of
  v0.
- `serve --bind ADDR --allow-lan-plaintext` binds a LAN address. Without that
  flag, any address that is not loopback is refused.

### The output seam

The checker writes the current statement to
`<data_dir>/household_posture.bin`, atomically (temp file, then rename) and
with mode 0600. `GET /v1/posture` returns the same 112 bytes base64-encoded,
plus the decoded posture. A reader must run every check in `posture::verify`
and behave as `away` on any failure.

## Wire summary

The doc comments are the normative detail. These are the essentials:

- **Statement** (`posture.rs`): `SCVP`, version 1, posture byte, 16-byte
  household, `seq`, `issued_at` (floored to the minute) and `not_after`, then
  an Ed25519 signature over `"securacv-posture-v1\0" ‖ bytes[0..48]`.
  `seq` never drops below `minute(issued_at) << 10`, so a restored `seq.json`
  cannot make the hub reuse a sequence number.
- **Challenge message** (`wire.rs`):
  `"securacv-hk-v1\0" ‖ purpose ‖ door_id(16) ‖ be64(epoch) ‖ n_h(32)`.
  **That is 72 bytes. The v0 draft says 88, which does not match its own
  field list.** This crate follows the fields, and a test pins the length.
- **Roster** (`roster.rs`): `securacv-cjson-v1`, with signing bytes
  `"securacv-roster-v1\0" ‖ cjson`. The encoder lives in this crate. A test
  pins it to bytes the root crate's `canonical_json.rs` produced.
- **Admin approvals** (`admin.rs`) are over
  `"securacv-hk-admin-v1\0" ‖ digest`.
- **Two domains this crate adds**, which the draft leaves open:
  - holder revocation: `"securacv-hk-revoke-v1\0" ‖ digest`;
  - the hub's roster countersignature:
    `"securacv-hk-hub-roster-v1\0" ‖ digest`.
- **Cards** (`ntag424.rs`): `RndA = SHA-256(msg)[0..16]`. The hub
  trial-authenticates the enrolled card keys, at most 16 per tap. Real keys
  come first, in a fresh random order, then random dummy keys fill the
  remaining slots. Anything that is not a household card sees all 16
  attempts, so it learns nothing about how many cards are enrolled.

**AN12196 vectors.** The two `AuthenticateEV2First` worked examples (key
0x00, a full APDU transcript; key 0x03) were taken from the transcription in
the `ntag424` crate 0.1.1 (MIT OR Apache-2.0, codeberg.org/jannschu/ntag424),
which cites AN12196 for each. The application note itself was not fetched.

## Known limitations

- **The hub confirm is a shell, not a button.** "Physical confirm on the hub"
  is the operator typing `--physical-confirm` in a shell on the hub host.
  There is no button.
- **No admin announcement.** The 24-hour pending period for admin-set changes
  is enforced, but nothing announces a pending change to every admin.
  `roster pending` shows it.
- **Rollback protection lives on the hub alone.** Rollback refusal compares
  against the high-water mark in the data directory. If the whole directory
  is restored from a backup, the mark rolls back with it. v0 has no satellite
  to keep the mark elsewhere (`change::check_not_rollback` takes that value
  when one exists).
- **The LAN API is plaintext HTTP.** On a LAN bind
  (`--allow-lan-plaintext`), the bearer token and the posture cross the
  network in cleartext. A command still needs a signature from an enrolled
  key, and the reference client refuses a challenge whose purpose or door was
  swapped. On loopback this does not arise.
- **Any LAN host can trigger the cooldown.** Requests with a bad bearer count
  as rejections at the LAN door. Anyone on the LAN can therefore hold the LAN
  API in cooldown. This fails safe (the posture does not change), but it is a
  denial of service.
- **One connection at a time.** The server is single-threaded. A slow client
  can hold it for up to the 5-second connection budget.
- **Card failure counters.** Trial authentication makes failed
  `AuthenticateEV2First` attempts on a household card: fewer per tap than the
  number of enrolled cards. Check the NTAG 424 DNA failed-authentication
  counter configuration before enrolling many cards.
- **Relayed cards leak a count.** Someone who relays a genuine household card
  over many taps can learn how many cards are enrolled from the attempt
  counts.
- **Cards revoke through an admin only.** The library checks holder
  revocation proofs for `es256` and `fido2-es256` members, but a card cannot
  revoke itself in v0.
- **Card keys are not checked at apply.** `roster apply` does not check that
  every enrolled card has a key in the sealed store. A card without a key
  simply never authenticates.
- **The CLI signs only Ed25519 admin approvals.** ES256 enclave and FIDO2
  admin approvals are checked by the library, but they must be made
  elsewhere and imported with `--approval-file`.
- **Tests pin the encoding, not the scheme.** The AN12196 vectors and the
  canonical-JSON vector pin the encodings. Nothing here has had an external
  security review.
