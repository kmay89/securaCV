# Household keys and household posture — v0

Status: Draft v0.1
Intended Status: Normative
Last Updated: 2026-10-14

This is the wire contract for the design recorded in
[`docs/research/face_id_without_faces.md`](../docs/research/face_id_without_faces.md)
§6. A household member presents a **household key** (a phone or watch key
behind that device's own biometric, a FIDO2 key, a fob, an NFC card). The hub
turns that deliberate, signed act into a **household posture**: `away`, `home`
or `night`. Witnesses never learn who anyone is, a key never joins to a
witnessed event, and nothing here is a biometric.

It also specifies the **attention rules** (§7): how loudly the hub tells the
household about one event, without identity.

**Implementation status.** The hub side is built and host-tested, and nothing
here has been on a bench or had an external security review:

- the household-key checker, [`household/`](../household/) (package
  `household-keys`, binary `household_checker`): the roster and its change
  rules, single-use challenges, the `es256` and `fido2-es256` checks, the
  NTAG 424 DNA `AuthenticateEV2First` reader state machine (checked against
  NXP AN12196's worked examples), the sealed card-key and hub-key stores
  (§5.4), the posture statement with every witness-side check (§3.2), the
  output seam (§6.2) and the LAN API (§6.1);
- the attention engine, [`src/relay/attention.rs`](../src/relay/attention.rs),
  with its vocabulary in `spec/witness_dictionary.json` and its conformance
  vectors in [`fixtures/attention_v1.json`](fixtures/attention_v1.json) (§7).

**Not built:** the key satellite (§6.3) and the phone and watch clients, so no
real credential reaches the checker except through its reference software
client; any witness that honors a household posture (§6.4); and the relay
wiring that would deliver an attention decision. Nothing calls the attention
engine yet.

Terms: **intent** and **possession** are defined in §2. "Verify" in this
document means checking an Ed25519 signature against a pinned key, as the
project uses the word. The P-256, FIDO2 and AES checks are called *checks*.

---

## 1. Principles (normative)

1. **The boundary is the hardware.** Credentials enter only through a key
   satellite or the checker's LAN API, never through a Canary.
2. **Presentation, never observation.** Every input is a deliberate act: an
   NFC tap, a button press, or a confirm on a phone or watch. Nothing listens
   for passing devices.
3. **Commands, not classification.** A key's only effect is a household
   posture change, which applies to every event equally. No credential ever
   rewords, demotes, quiets, collapses or delays a specific witnessed event.
4. **Intent lowers, possession only raises.** Only an *intent* credential may
   set `home`, the posture that lowers sensing. A *possession* credential may
   set only `away` or `night`.
5. **Home lapses.** Every `home` statement carries a `not_after` at most
   `HOME_MAX_SECS` after it was issued, and a witness counts it for at most
   `HOME_MAX_SECS + MAX_SKEW_SECS` of its own clock, whatever the hub's clock
   said (§3.3). A witness that cannot verify a current statement senses
   fully.
6. **No who, in any output.** Rosters carry random handles, never names.
   Nothing in a posture statement, a log line, an HTTP response or a
   notification identifies a credential or a member.
7. **Local only.** No cloud, no Apple or Google service, no passkey hybrid
   tunnel, no URL on any tag.

## 2. Vocabulary

The household posture is dictionary vocabulary: the `household_posture` block
of [`witness_dictionary.json`](witness_dictionary.json). It is always called
the *household* posture, never bare "posture", which already means body
posture in Vision events.

| id | wire byte | Effect on witnesses | May be set by |
|---|---|---|---|
| `away` | `0x00` | Full sensing everywhere. The default and the fail-safe. | intent or possession |
| `home` | `0x01` | Witnesses whose zone kind is `indoor` sense tamper only. Outdoor, door and unclassified witnesses are unchanged. Lapses (§3.3). | intent only |
| `night` | `0x02` | Full sensing everywhere. Reserved for a future after-hours rule set; in v0 it behaves as `away`. | intent or possession |

`unset` is not a posture anyone can set. It is the value a consumer uses when
no statement is available or none passes §3.2, it has no wire byte, it is
never signed, and it means exactly today's behavior.

Credential classes:

| class | Credentials | May set |
|---|---|---|
| `intent` | a phone or watch key created to require its holder's user presence (iOS `.userPresence`; Android `userAuthenticationRequired` behind a Class 3 biometric); a FIDO2 key whose every assertion carries the UV (user verification) flag | `home`, `away`, `night` |
| `possession` | an NTAG 424 DNA card or sticker; a button fob; a FIDO2 key with user presence only | `away`, `night` |

- Cards and fobs are always `possession`.
- A FIDO2 member declared `intent` MUST present UV on every assertion. An
  assertion from it without UV is refused for every purpose, not treated as
  possession. Over NFC through a satellite with no keypad, UV is possible only
  on a key with its own fingerprint sensor; such a key is registered with
  `credProtect` 3, so a harvested credential ID cannot probe it silently
  elsewhere. Any other FIDO2 key is registered with `credProtect` 2 and is
  `possession`. Disclose to its holder that its relying-party ID and
  credential ID are not secrets: whoever harvests them from an NFC exchange
  can later recognize the key with a silent `up=false` probe.
- The hub cannot prove that a phone key requires user presence (iOS offers no
  offline attestation of it). The admin quorum (§5.3) declares the class at
  enrollment, and the roster records it. Android key attestation, where
  present, MAY be checked at enrollment.

## 3. The posture statement (hub → witnesses)

### 3.1 Layout

A fixed 112-byte record, all integers big-endian:

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 4 | `magic` | ASCII `SCVP` |
| 4 | 1 | `version` | `0x01` |
| 5 | 1 | `posture` | §2 wire byte |
| 6 | 2 | `reserved` | `0x0000`; reject otherwise |
| 8 | 16 | `household` | random household id |
| 24 | 8 | `seq` | strictly increasing per household |
| 32 | 8 | `issued_at` | Unix seconds, floored to the minute |
| 40 | 8 | `not_after` | Unix seconds; `0xFFFF_FFFF_FFFF_FFFF` (never) is allowed only for `away` and `night` |
| 48 | 64 | `sig` | Ed25519 by the hub posture key over `"securacv-posture-v1\0" ‖ bytes[0..48]` |

A statement carries no credential, member, door or zone. It does carry a
time: `issued_at` is the minute of the last posture change (a command, a
lapse or a restart), and whoever reads the statement learns that minute
(§6.2). The checker keeps `seq` at or above `minute(issued_at) << 10`, so
restoring its sequence file from a backup cannot make it reuse a sequence
number. That floor states nothing `issued_at` does not already state.

### 3.2 Verification (witness side)

A witness accepts a statement only if every check passes. Otherwise it MUST
behave as `away`:

1. the length is exactly 112, `magic` and `version` match, `posture` is a §2
   wire byte, `reserved` is zero, and `issued_at` is a whole minute;
2. `household` equals the household the witness was paired into;
3. the signature verifies against the **pinned** hub posture key;
4. `seq` is strictly greater than the last accepted `seq`, which the witness
   persists;
5. the witness has a trusted wall clock, and `now < not_after`;
6. for `home`: `not_after - issued_at <= HOME_MAX_SECS`;
7. `issued_at <= now + MAX_SKEW_SECS` (300 s in v0): a statement issued
   further ahead of the witness's clock (a hub clock running fast, or a
   statement signed for later) is refused.

A statement byte-identical to the one already accepted is not new: the witness
keeps it and does not run rule 4 against it again (it would fail). For as long
as it holds the statement it re-evaluates rules 5 and 7 on its own trusted
clock, treating no clock as a failure, and for `home` also requires
`not_after <= now + HOME_MAX_SECS + MAX_SKEW_SECS`, the bound rules 6 and 7
imply. So a stale `home` cannot keep a house in `home` past its lapse even if
the hub dies, and a held statement stops counting at once if the witness's
clock steps back far enough to put it in the future.

### 3.3 Lapse

`HOME_MAX_SECS` is 86 400 (24 h) and `MAX_SKEW_SECS` is 300 (5 min) in v0.
The hub issues `home` with `not_after = issued_at + home_ttl`, where
`home_ttl <= HOME_MAX_SECS` is an operator setting (default 43 200, 12 h).
Renewal is another intent act. When a `home` statement lapses, the hub issues
a fresh `away` statement. Witnesses that miss it fall to `away` on their own
clock anyway. The hub also starts in `away` after every restart.

Rules 6 and 7 bound a `home` statement on each witness's own clock: it counts
for at most `HOME_MAX_SECS + MAX_SKEW_SECS` (24 h 5 min) of that clock,
whatever the hub's clock said when it signed. The hub's lapse check applies
the same held-statement test (§3.2) on the hub's own clock, so a `home` also
lapses at once if the hub clock steps back more than `MAX_SKEW_SECS` behind
its `issued_at`. A hub clock more than 5 minutes ahead of a witness (give or
take the minute floor on `issued_at`) makes that witness refuse its
statements (it behaves as `away`, the fail-safe); a hub clock behind makes
`home` lapse early. Plain NTP is unauthenticated, so the hub SHOULD take its
time from an authenticated source (for example chrony with NTS).

## 4. Challenges and credential bindings

### 4.1 Challenge

The hub draws every challenge from its OS RNG:

```
n_h      32 random bytes, single use, valid 10 s
purpose  0x01 = set away, 0x02 = set home, 0x03 = set night
door_id  16 bytes: the satellite's random door id, or 16 zero bytes for the LAN API
epoch    u64, the roster epoch the challenge was issued under

msg = "securacv-hk-v1\0" ‖ purpose ‖ door_id ‖ be64(epoch) ‖ n_h        (72 bytes)
```

That is 15 + 1 + 16 + 8 + 32 = 72 bytes. (An earlier draft said 88, which
does not match its own field list. The fields are normative.)

A challenge is bound to the session that requested it: a satellite's
authenticated session, or one LAN API session (§6.1). A response presented on
any other session is rejected. Membership is checked against the **current**
roster epoch when the response arrives, and every epoch change discards all
outstanding challenges. The checker holds at most 64 outstanding challenges,
and at most 4 per session.

### 4.2 Bindings

| `alg` | Credential | Binding |
|---|---|---|
| `es256` | phone, watch or fob key | ECDSA P-256 with SHA-256 over `msg`; the signature is ASN.1 DER. |
| `fido2-es256` | FIDO2 key | `authenticatorGetAssertion` with `clientDataHash = SHA-256(msg)` and the roster's `rp_id`. The hub checks `rpIdHash = SHA-256(rp_id)`, the UP flag, the UV flag when the member is `intent`, and the DER ES256 signature over `authenticatorData ‖ clientDataHash`. The signature counter is **not** stored (§8). |
| `aes128-ev2` | NTAG 424 DNA card or sticker | `AuthenticateEV2First`, run by the hub through the satellite's APDU tunnel, with the reader random `RndA = SHA-256(msg)[0..16]`. |

For cards:

- **Random ID is on**, so the hub cannot tell which card it holds before
  authenticating. It **trial-authenticates the enrolled card keys**, bounded
  by the roster: v0 allows at most 16 card members, and every tap runs exactly
  16 attempts, the real keys first in a fresh random order and random dummy
  keys after them. A card that is not a household card therefore learns
  nothing about how many cards are enrolled. Someone who relays a genuine
  household card over many taps can still learn the count from where the
  attempts stop.
- Trial authentication makes failed `AuthenticateEV2First` attempts on
  household cards. Check the card's failed-authentication counter
  configuration before enrolling many cards.
- Each card has its own random AES key. Keys are never diversified from the
  UID, and there is no household-wide class key.
- An NTAG 424 DNA card has only its fixed NDEF application, so it is the
  first authentication, not an application select, that separates a household
  card from a stranger's. The satellite forwards no UID and reads no file.
- SUN/SDM messages (Secure Unique NFC: a MAC over the UID and a read counter)
  are never used for authority. They are replay-safe only if whoever checks
  them keeps a per-card counter, which §8 forbids.

### 4.3 Rate limits

Rejections are counted per door and per lane. A door has two lanes:
**signature** (`es256` and `fido2-es256`: a phone, watch, fob or FIDO2 key,
over the LAN API or relayed by a satellite) and **card** (`aes128-ev2`). Five
rejected responses on one lane of one door within ten minutes start a
60-second cooldown for that lane, so junk on one lane never cools the other.
The LAN API is one door (`door_id` = 16 zero bytes).

- Only a response to a live challenge counts: the challenge take and every
  check after it. A request without a paired token, or whose body does not
  parse, never reaches the limiter (§6.1), so a client with no token cannot
  start a cooldown. A refusal caused by a cooldown, or by a stale roster
  (§6.1), is not counted either.
- A cooldown refuses only `home`, both the challenge and the command. `away`
  and `night` still go through on a cooling signature lane, so a denial of
  service cannot hold a house in `home` through the cooldown.
- The card lane carries `away` and `night` only, and it stays closed while it
  cools: every trial `AuthenticateEV2First` (§4.2) touches a real card's
  failed-authentication counter.
- A cooldown that has ended is forgotten, and its count starts over.
- Counts are kept in RAM only, per door and lane, never per credential or
  handle.

## 5. The roster

### 5.1 Format

The roster is a JSON object serialized with `securacv-cjson-v1`, the rules of
[`src/canonical_json.rs`](../src/canonical_json.rs). The checker carries its
own encoder and a test pins it to bytes the root crate produced.

```json
{
  "v": 1,
  "household": "<16-byte hex>",
  "epoch": 7,
  "prev": "<SHA-256 hex of the previous roster's signing bytes, or 64 zeros>",
  "threshold": 2,
  "admins": [
    { "handle": "<16-byte hex>", "alg": "ed25519", "kind": "cli", "public_key": "<hex>" }
  ],
  "members": [
    { "handle": "<16-byte hex>", "kind": "phone", "class": "intent",
      "alg": "es256", "public_key": "<SEC1 uncompressed hex>", "not_after": null }
  ]
}
```

- Admin `alg`/`kind` pairs: `ed25519`/`cli`, `es256`/`enclave`,
  `es256`/`fido2`. FIDO2 admins add `rp_id` and `credential_id`.
- Member `alg`/`kind` pairs: `es256` with `phone`, `watch` or `fob`;
  `fido2-es256` with `fido2` (adds `rp_id` and `credential_id`); `aes128-ev2`
  with `card`. A card carries no key in the roster: its AES key lives in the
  hub's sealed store (§5.4), under the member handle.
- `not_after` lets a member expire on its own; `null` means it does not.
- Handles are random and never derived from a key, a UID or a name. Unknown
  fields are refused, so **no field can hold a name or label**.
- Both lists are sorted by handle, so a roster has exactly one encoding.
- v0 limits: 16 admins, 64 members, 16 of them cards.

The **signing bytes** are `"securacv-roster-v1\0" ‖ cjson(roster)`, and the
roster **digest** is `SHA-256(signing bytes)`. A roster is valid only with:

- the approvals the change rules (§5.3) require, over the digest, from admins
  of the **previous** roster, counted by distinct admin handle; and
- the hub's Ed25519 countersignature over
  `"securacv-hk-hub-roster-v1\0" ‖ digest`.

The chain of rosters is re-checked on every load.

The roster is a stored set of long-lived, per-household identifiers: random
handles and credential public keys. It is not a biometric and holds no name,
but it is still a new identifier store, and the governance review in §9 has to
argue for it as one.

### 5.2 Admin and holder signatures

| Signer | Signed message |
|---|---|
| admin, `ed25519` / `cli` | Ed25519 over `"securacv-hk-admin-v1\0" ‖ digest` |
| admin, `es256` / `enclave` | ECDSA P-256 SHA-256 over `"securacv-hk-admin-v1\0" ‖ digest`, DER |
| admin, `es256` / `fido2` | an assertion with `clientDataHash = SHA-256("securacv-hk-admin-v1\0" ‖ digest)`, UV required |
| member revoking itself, `es256` | ECDSA P-256 SHA-256 over `"securacv-hk-revoke-v1\0" ‖ digest`, DER |
| member revoking itself, `fido2-es256` | an assertion with `clientDataHash = SHA-256("securacv-hk-revoke-v1\0" ‖ digest)`, UP required |

One admin is one person, decided at an enrollment ceremony. The hub never
infers distinct people from distinct key bytes, and refuses one key under two
handles. A card cannot sign, so in v0 a card is revoked only by an admin.

One credential (the same public key, or the same FIDO2 credential id) is
never both an admin and a member: the roster is refused. A FIDO2
authenticator signs whatever `clientDataHash` the relaying device hands it,
so a credential in both lists would let a compromised relay turn an ordinary
door touch into an admin approval. Distinct credentials on one authenticator
are still only as safe as the device relaying them, so an admin credential
belongs on an authenticator kept for admin approvals, approved through a
device the admin trusts.

### 5.3 Change rules

| Change | Requires |
|---|---|
| Add a member | `threshold` admin approvals. With `threshold` 1, also a confirm at the hub. |
| Revoke a member | any one admin, or the member's own credential |
| Change the admin set or the threshold | `threshold` approvals from the **current** admins, after a 24-hour pending period announced to every admin |
| Every change | `epoch + 1`, and `prev` = the digest of the previous roster |

In v0, "a confirm at the hub" is an operator command run on the hub host;
there is no button yet. The 24-hour pending period is enforced, but nothing
announces a pending change to every admin yet: `household_checker roster
pending` lists it.

`roster genesis` defaults to `threshold` 1. With `threshold` 1, one admin's
approval is all any change needs (an add also needs the confirm at the hub),
including removing every other admin once the 24-hour pending period has
passed, and nothing announces that pending change beyond `roster pending`.
So "one stolen admin key cannot take over" holds only with `threshold` 2 or
more. The trade-off: with exactly two admins and `threshold` 2, losing one
admin key leaves no way to remove it, because removing an admin needs two
current admins. So `threshold` 2 wants at least three admins.

### 5.4 Secrets and backups

Two files in the checker's data directory are secret. Both are sealed with
XChaCha20-Poly1305 under keys derived (HKDF-SHA256, salt = the household id)
from one key-encryption key (KEK):

| File | Magic | HKDF info | Holds |
|---|---|---|---|
| `cards.sealed` | `SCVK` | `securacv-hk-cards-v1` | the card keys, which are clone-capable |
| `hub_keys.sealed` | `SCVH` | `securacv-hk-hub-keys-v1` | the hub posture key and the hub roster key |

The posture key is the key every witness pins (§3.2 rule 3), and it alone
decides whether indoor witnesses sense. The roster key countersigns every
accepted roster (§5.1). Their public halves are in plaintext
`hub_keys.json`. No roster field is secret.

The KEK is a file of 64 hex characters. The checker refuses a KEK path
inside the data directory, and (on Unix) a KEK file readable by group or
others. There is no passphrase option. Only the commands that open sealed
material take `--kek`: `init`, `roster genesis`, `roster apply`,
`card provision` and `serve`; every other command reads public material only.
`card provision` opens the hub keys first, which proves the KEK belongs to
this household, so a card key is never sealed under a KEK the server cannot
open.

Sealing protects a copy of the data directory (a backup, a tarball). It does
not protect a disk image that also holds the KEK file. Excluding the data
directory from backups, and keeping the KEK out of any backup that holds the
directory, is the operator's job: v0 does not, and cannot, enforce it.

A leaked card key means re-keying that card. A leaked posture key lets anyone
sign `home`, skipping principle 4 entirely, and a forged statement with a huge
`seq` would lock out every genuine one (§3.2 rule 4), so recovery is a new
posture key and re-pairing every witness. A hub compromise means both:
re-keying every card, and a new posture key with every witness re-paired.

Applying a roster change (`roster apply`) removes every sealed card key whose
handle is neither a card member of the applied roster nor a card member of a
pending proposal, and reports how many it removed (`card_keys_removed`), so
the hub does not keep a revoked card's key, the only thing that can recognize
or clone that card. `card provision` reads a card key from a file
(`--key-file`), never from the command line, where it would land in shell
history and in `/proc` (`--key-hex` is refused); without one it generates a
random key and prints it once.

### 5.5 Rollback

The checker refuses a roster whose epoch is lower than the highest epoch it
has accepted, any different roster at that epoch, and any roster below the
epoch high-water mark a paired satellite reports. v0 has no satellite, so the
high-water mark lives in the data directory alone, and restoring the whole
directory from a backup rolls it back with the roster.

## 6. Interfaces

### 6.1 LAN API

Served by `household_checker serve`. It binds loopback (`127.0.0.1:8787`) by
default, and a LAN bind is explicit: the checker refuses any non-loopback
address unless the operator passes `--allow-lan-plaintext`.

**On any bind, it answers only peers on the home network.** A connection from
any other peer address is closed at accept, without a response. The home
network is loopback; IPv4 private (RFC 1918) and link-local; IPv6
unique-local (`fc00::/7`) and link-local (`fe80::/10`); and the IPv4-mapped
IPv6 forms of those. Everything else is refused, including `100.64.0.0/10`
(the shared address space Tailscale and carrier NAT use) and public IPv6
addresses, so on a home LAN that uses public IPv6 addresses, bind an IPv4 LAN
address. A proxy, VPN or ingress that re-originates traffic from a private or
loopback address defeats the filter, so the LAN API MUST NOT sit behind one
that accepts peers from elsewhere. "A key does nothing from outside the home
network" is therefore enforced by peer address: a strong default, not a
cryptographic guarantee.

v0 has no in-process TLS, so a LAN bind SHOULD sit behind TLS (a
TLS-terminating proxy on the hub, which must itself accept home-network peers
only, since every request it forwards arrives from its own address). Without
it, every request crosses the LAN in cleartext, including each client's bearer
token (stable per client) and, in a `/v1/command` body, the member handle. A
passive listener on the LAN can therefore link acts to a device and to a
member, and can replay a sniffed token to read the posture. A signature still
has to come from an enrolled key.

| Method | Path | Body | Result |
|---|---|---|---|
| `POST` | `/v1/challenge` | `{"purpose": "home" \| "away" \| "night"}` | `{"challenge_id", "msg_b64", "expires_in": 10}` |
| `POST` | `/v1/command` | `{"challenge_id", "handle", "signature_b64"}` (FIDO2 adds `authenticator_data_b64`) | `{"posture", "not_after"}` |
| `GET` | `/v1/posture` | — | `{"statement_b64", "posture", "not_after"}` |

`not_after` is `null` for a statement that never lapses.

- **The authority is the credential signature.** No route changes the
  household posture without a response from an enrolled credential to a live
  challenge. Nothing else (a token, an Apple Home write, a Home Assistant
  call, an MQTT word) can change it.
- **A session token binds challenges and reads the posture; it cannot change
  it.** v0 names a LAN session by a client token that `household_checker
  pair` mints for one client (`Authorization: Bearer …`), at most 32 paired
  clients. The token's SHA-256 is the session a challenge is bound to (§4.1).
  A token cannot change the household posture, but it does read it:
  `GET /v1/posture` gives its holder the posture word, `not_after` and the
  signed statement (`issued_at` to the minute, and `seq`). `household_checker
  pair --reset` unpairs every client, and takes effect while the server runs.
  There is no per-token unpair: tokens are deliberately not linked to
  members, so revoking a member does not unpair its device, and after a
  revoke `roster apply` tells the operator to run `pair --reset` and pair the
  clients they keep again. The token belongs to the checker; it is not the
  kernel API's paired bearer.
- **Every rejection is the same `401` with the same body**, whether the token
  was missing or wrong, the request cap was reached, the body did not parse,
  `home` was refused during a cooldown (§4.3) or while the roster is stale
  (below), the challenge was used, the handle was unknown, the signature was
  bad or the class was not allowed. No response says whether a handle exists,
  and neither does the time a refusal takes: every command that reaches the
  credential check runs exactly one P-256 check, against a dummy key when the
  handle is unknown, expired, a card, or does not match the binding the
  request used (an `es256` response for a FIDO2 member or the other way round,
  a wrong `rp_id`, no UV from an `intent` FIDO2 member). A test asserts this by
  counting the checks, not by timing them.
- **A request without a paired token is refused before anything is
  counted**: it is charged neither to the request cap nor to the door limiter,
  so a client with no token cannot crowd out paired clients or start a
  cooldown. A body that does not parse never reaches the door limiter either.
- The server is bounded like the kernel API: 8 KiB per request, a 4 KiB
  body, one request per connection, no chunked bodies, and a cap of 600
  requests a minute for all paired clients together, charged only after the
  token matches. Each connection is served on its own thread: at most 16 at
  once, and at most 4 from one peer address; a connection over either cap is
  closed at accept. Every connection has an absolute 5-second budget, and
  each read and write timeout is cut to what is left of it (at most 3 s each).
  A LAN host with many addresses can still take every slot. Any network
  denial of service against the LAN API holds the current posture; if that
  is `home`, it stays until it lapses at `home_ttl`, so a short `home_ttl`
  limits it.
- **It reloads while it runs.** The roster chain, the sealed card store and
  the pairings reload independently. A part that fails to load keeps its
  previous state, is retried on every poll, and is logged once. If the roster
  chain changed but does not load or pass its checks (§5.1, §5.5), the roster
  is **stale**: `home` is refused until a roster passes them, and `away` and
  `night` are still checked against the last roster that did, because raising
  sensing is safe whoever asks.
- It logs startup, shutdown, reload trouble and storage failures, and nothing
  per request: no handle, credential id or peer address.

### 6.2 Output seam

The checker writes the current statement to `<data_dir>/household_posture.bin`
atomically (a temp file, then a rename) with mode 0600. `GET /v1/posture`
serves the same bytes. A reader MUST read at most 112 bytes, run every check
in §3.2, and behave as `away` on any failure. v0 publishes nothing over MQTT.
An unsigned broker word would let any broker writer quiet a house, and a
retained topic would tell every subscriber the exact time of each change.

An accepted `away` or `night` that does not change the current posture
publishes nothing: no new `seq`, and the seam is not rewritten, so the seam
does not reveal a tap that changed nothing. `home` is always reissued,
because that renews its lapse.

The seam holds the current statement only, not a history, but the data
directory states the time of the last posture change: the seam's `issued_at`
and `not_after`, and `seq.json`'s `last_seq` (whose value `>> 10` is that
minute), give the minute, and both files' modification times give the
second. That change was
a command, a lapse or a restart. Any live reader of the seam or of
`GET /v1/posture` learns each change time to its own polling precision.
Whether that is acceptable is for the posture RFC (§9) to decide, beside
whether Home Assistant may show the household posture at all.

### 6.3 Key satellite (not built)

The satellite is its own device class, never a Canary: the reference design
is a XIAO ESP32-C3 and an ST25R3916 NFC front end, with the MCU inside the
wall and only the antenna outside. Its contract:

- **Pairing.** It pairs with the hub by a real PAKE (password-authenticated
  key exchange) over a short code, SRP-6a or SPAKE2+, with an attempt cap and
  both long-term keys bound into the exchange. It then runs Station-to-Station
  sessions (X25519 → HKDF-SHA256 → ChaCha20-Poly1305). A code-comparison
  exchange like Opera's is not a PAKE and does not qualify
  ([`canary_mesh_network_v0.md`](canary_mesh_network_v0.md) §11.1).
- **Storage.** It stores only its own Ed25519 identity, the pinned hub keys
  and a hub-signed roster-epoch high-water mark: no member material. The
  identity is drawn at first boot between `bootloader_random_enable()` and
  `bootloader_random_disable()`, because the radio is not up yet.
- **Secure by default.** ESP-IDF, not the Arduino 2.0.17 core: NVS
  encryption via the HMAC scheme, Secure Boot v2 (RSA-3072), flash encryption
  in release mode, secure download mode, a per-station Wi-Fi PSK and an
  enclosure tamper switch. Like any ESP32-C3 it stays open to the AR2023-007
  side-channel and fault-injection chain given days of physical access.
- **Phones.** It emulates an ISO 7816-4 card on the household AID. An iPhone
  reads it in a foreground Core NFC `NFCTagReaderSession` as an
  `NFCISO7816Tag`, with the AID listed under
  `com.apple.developer.nfc.readersession.iso7816.select-identifiers`. That
  needs the self-service NFC Tag Reading capability, not the NFC & SE Platform
  entitlement. Android uses reader-mode `IsoDep`, with no HCE. The challenge
  exists only in the RF field.
- **Cards and FIDO2 keys.** It tunnels APDUs between them and the hub. It
  forwards nothing about a card or key, no UID and no file, beyond the
  authentication exchange of §4.2.
- **Feedback.** The same neutral feedback for every tap. The result goes only
  to the presenter's own phone.
- **Traffic.** Its uplink runs on fixed-size, padded frames on a fixed
  cadence, so its own traffic does not time taps. A posture change is a
  separate signal: indoor witnesses going quiet changes their traffic, and §9
  must pace that too.
- **Fobs.** A fob stays silent until its button is pressed: no BLE
  advertising.

### 6.4 Witnesses (not built)

A witness that honors household posture reads the statement, verifies it per
§3.2, and, if its zone kind is `indoor`, senses tamper only while a valid
`home` statement is current. How a witness receives the statement is for the
posture RFC (§9) to settle; v0 offers only §6.1 and §6.2. Until a witness does
this itself, the hub's attention engine applies the same rule to that
witness's notifications (§7.2 rule 2), and the witness log is unchanged either
way.

## 7. Attention rules (the hub's notification policy)

### 7.1 Inputs and outputs

For one event, the engine takes:

- the event's coarse class, mapped from either a kernel `EventType` id or a
  device wire word (an unknown word maps to `other`, which is at most
  `digest`);
- the zone slot and its zone kind (`indoor`, `outdoor`, `door` or `unknown`),
  which the operator configures;
- the household posture (`unset`, `away`, `home`, `night`), the same value for
  every event at that moment;
- whether the reporting witness applies household posture itself;
- the reporting channel, and how many physically independent modality classes
  agreed (channels that fail together count once: Wi-Fi and BLE are one
  carried-radio class, a light sensor and a camera one optical class);
- the dwell band (`none`, `brief`, `sustained`; sustained is at least 90 s);
- the device's own novelty hint (`unknown`, `familiar`, `unfamiliar`), read
  only for an arrival seen on carried radio alone;
- the hub's local hour, which v0 reads nowhere; and
- whether the household's quiet hours are on.

No input is a credential, a timestamp, a device id or free text.

It returns a tier (`none`, `digest`, `important`, `critical`), one closed
phrase, an episode key (event family and zone slot: no device, no time, no
text) and one closed reason code for the "why did or didn't I get this" trace.
Tiers map to the system-wide ladder in
[`apple_watch_and_notifications.md`](../docs/design/apple_watch_and_notifications.md)
§1.1. `none` exists only for rule 2.

### 7.2 Rules

In the order the engine applies them:

1. **Critical is never lowered.** Tamper, a log-integrity failure, a witness
   declared dark, and a heard smoke or CO alarm cadence are `critical`,
   whatever the posture, zone, channel, corroboration, novelty, dwell, quiet
   hours or episode.
2. **Household posture acts on sensing, not on wording.** While a valid `home`
   is current, every other event from an `indoor` witness is `none`, the
   outcome of that witness sensing tamper only. Outdoor, door and unclassified
   zones are never affected. The phrase never changes. `unset`, `away` and
   `night` give the same decision in v0.
3. **Some classes never interrupt.** An animal, or an event with no word, is
   at most `digest`.
4. **Novelty is for radio-only arrivals, and moves nothing else.** An arrival
   seen on carried radio alone stays `digest` unless the device's hint is
   `unfamiliar` and the dwell is sustained, when it may be `important`. A
   `familiar` hint never drops an arrival below `digest`. Novelty never makes
   anything `critical` and never touches a camera, radar, contact or tamper
   event. The phrase and reason are the same whatever the hint was.
5. **Corroboration earns interruption.** Two or more independent modality
   classes agreeing, or a camera or radar person, may be `important`.
6. **Otherwise, `digest`.** A single weak channel, or a single strong one
   with nothing agreeing, is `digest`.

Then:

- **Quiet hours hold digests.** During quiet hours a `digest` waits for the
  morning summary. `important` and `critical` are delivered.
- **One episode, one notification.** The first decision of an episode
  notifies. A repeat at the same or a lower tier notifies again only after its
  earned rest: 120 s, doubling per notification to a cap of 1800 s, with an
  1800 s calm gap starting a new episode. A higher tier always gets through,
  and `critical` is never governed. These are the Apple apps'
  `RepeatGovernor` numbers, so hub and phone rest a burst the same way.
- **No identity, ever.** Credentials are not an input. The phrase table is
  closed, its only blank is the operator's own zone name, and no phrase or
  reason says a name, "known", "unknown", "stranger", "intruder",
  "recognized", "verified", "familiar", "authorized" or "household".
- **No new remote class.** A decision maps to a remote poke only when it is
  `critical`, and then only to one of the relay's four existing classes
  (tamper, integrity, offline, pattern). Household posture and everyday
  activity never create a new remote poke class.

### 7.3 Conformance vectors

[`fixtures/attention_v1.json`](fixtures/attention_v1.json) holds hand-written
input → output cases, word-mapping cases for both event dialects, and timed
episode sequences. The Rust test in `src/relay/attention.rs` replays every one
(`cargo test --features alert-relay relay::`, run by `rust.yml`), and
`scripts/lint_dictionary_sync.py` pins the engine's vocabularies to the
dictionary's `household_posture` and `attention` blocks. Any surface that
decides tiers (the iPhone app, the Dash, the Home Assistant integration)
replays the same file before it claims to follow these rules.

## 8. What v0 deliberately does not do

- **No signature counters, usage tallies or arrival history** for any
  credential. Freshness comes from the single-use challenge, and FIDO2 clone
  detection is traded away for it.
- **No guest passes.** A guest pass's use record is attendance tracking of a
  cleaner or caregiver.
- **No passive presence** (Bluetooth address resolution, Wi-Fi probes) as a
  credential or a quieting signal.
- **No MQTT, no in-process TLS** (§6.1, §6.2).
- **No door locks.** Locks and Aliro wallets are a separate device class.
- **No journal of posture changes.** The checker records none (the data
  directory states the time of the last change only, §6.2), and logs no
  per-request line.
- **Nothing on a public chain.**

## 9. What needs an RFC before anything on a Canary uses this

The hub side exists so the contract can be tested. Letting a witness change
what it senses on a household posture is a separate step. It goes through a
formal RFC under
[`governance_and_invariants.md`](../docs/governance_and_invariants.md) §3.2,
because the roster is a new store of identifiers (governance §2.3, §4).
The decision record
[`household_presence_and_posture.md`](../docs/design/household_presence_and_posture.md)
lists what that RFC must settle:

- the roster, argued as a stored set of long-lived, per-household
  identifiers;
- whether a household posture change is journaled at all, and if so as a new
  sealed-log record type that starts in the dictionary;
- a credential check driving Canary sensing, read against Free Signals
  Invariant D ("no integration with access control");
- how a witness receives and pins the statement, and how posture-driven
  traffic changes are paced (Invariant III); and
- whether Home Assistant may show the household posture at all, since its
  recorder would keep an exact-time diary of every change.
