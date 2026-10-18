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
- the sealed store for card keys and the hub's two signing keys;
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
asked for it. The hub holds at most 64 outstanding challenges, 4 per session.
A session that asks for a fifth loses its oldest instead of being refused, so
whoever holds a session's token cannot starve that session by holding its
slots. The hub checks membership against the *current* roster epoch,
and an epoch change discards every outstanding challenge. No signature
counter, usage tally or arrival history is kept for any credential. FIDO2
clone detection is traded away for that.

**`home` lapses.** A `home` statement lives at most 24 hours (12 by
default). A witness checks the lapse against its own clock, and keeps checking
the statement it holds on that clock (`Statement::is_current`). It refuses a
statement issued more than 5 minutes ahead of its clock, and refusing means
behaving as `away`. The witness must still record that statement's `seq` as
the last one accepted (`posture::verify` returns it with the refusal), so the
statement can never count later, once its `issued_at` arrives. So a `home`
counts on a witness for at most 24 hours and 5 minutes of the witness's own
clock, whatever the hub's clock said. The hub replaces a lapsed `home` with
`away` on its own clock, and that `home` also lapses at once if the hub's
clock steps back more than 5 minutes behind its `issued_at`. The hub times
each `home` on its monotonic clock as well, from the command, so a wall
clock held back (by a spoofed time source, say) cannot keep it alive past
the home TTL. A roster change that drops any member, or a roster that goes stale, ends
a current `home` too (see the limitations). The hub also starts in `away`
after every restart. Plain NTP is unauthenticated, so the hub should take its
time from an authenticated source (chrony with NTS, for example).

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
epoch it has accepted, and any different roster at that epoch. The running
server reloads the chain when the name of its newest chain file changes (a
roster was applied; a file in `roster/chain` not named like a chain file, 20
digits and `.json`, is ignored), and verifies it against the roster public
key it opened under the KEK at startup, never re-read from the plaintext
`hub_keys.json`.
If the changed chain does not load or verify, or the checker refuses the
roster it yields, the server refuses `home` until a roster verifies, ends a
current `home`, and still checks `away` and `night` against the last roster
that verified: raising sensing is safe whoever asks.

**Admins and members never share a credential.** The roster refuses one
public key, or one FIDO2 credential id, as both an admin and a member. A FIDO2
authenticator signs whatever `clientDataHash` the relaying device hands it, so
a compromised relay could otherwise turn an ordinary door touch into an admin
approval. Distinct credentials on one authenticator are still only as safe as
the device relaying them. An admin credential belongs on an authenticator kept
for admin approvals, approved through a device you trust.

**Secrets are sealed.** Three kinds of key are secret. Card keys are
clone-capable. The hub's posture key is pinned by every witness and alone
decides whether indoor witnesses sense. The hub's roster key countersigns
every accepted roster. Card keys are sealed in `cards.sealed` and the two
hub keys in `hub_keys.sealed`, with XChaCha20-Poly1305 under keys derived
from one key-encryption key (KEK); the public halves are in plaintext
`hub_keys.json`.
The KEK is a file of 64 hex characters outside the data directory. The
checker refuses one inside it, and on Unix one with any permission for group
or others. There is no passphrase option. Sealing protects a copy of the data
directory, such as a backup or a tarball. It does not protect a disk image
that also holds the KEK file, and a compromise of the running hub, which
holds the KEK, exposes every one of them.
A leaked card key means re-keying that card. A leaked posture key lets anyone
sign `home`, skipping "intent lowers, possession only raises", and a forged
statement with a huge `seq` would lock out every genuine one: recovery is a
new posture key and re-pairing every witness. Keeping the KEK out of every
backup that holds the data directory is the operator's job: v0 does not, and
cannot, enforce it. The KEK still needs a backup of its own, somewhere safe:
losing it loses the household (see the limitations).

**What the network sees.** Every rejection on the LAN API's three routes is
the same `401` with the same body, whether the token was missing or wrong, the
request cap was reached, the body did not parse, `home` was refused, the
challenge was used, the handle was unknown, the signature was bad or the class
was not allowed. (Only a request that is too large, is not well-formed HTTP,
or names an unknown path or the wrong method gets another status, `413`,
`400`, `404` or `405`, before its token is read.) Every command that reaches
the credential check with a DER signature runs exactly one P-256 signature
check (one whose signature is not DER runs none, whatever the handle),
against a dummy key when the handle is unknown,
expired, a card, or does not match the binding the request used (an `es256`
response for a FIDO2 member or the other way round, the wrong `rp_id`, no UP,
or no UV from an `intent` member). So neither the response nor the time it
takes says whether a handle exists. A test asserts this by counting the
checks, not by timing them. The dummy key is drawn from the OS RNG each time
the checker starts, and its private half is dropped at once (a fixed seed
would have made it public). Its verdict is never read: only a member's own
public key can authorize, and a member without one never does.

Rejections are counted per door and per lane. The signature lane carries
`es256` and `fido2-es256` responses: a phone, watch, fob or FIDO2 key, over
the LAN API or relayed. The card lane carries NTAG 424 DNA authentications.
Five rejected responses on one lane of one door within ten minutes cool that
lane for 60 seconds. Counting starts at the challenge lookup: a command whose
challenge is unknown, used, expired, or bound to another session, door or
epoch counts, and so does every refusal after that. A missing or wrong bearer
token, or a body that does not parse, never counts toward a cooldown, so a
client with no token cannot start one. Refusals caused by a cooldown or a
stale roster are not counted either, nor is a storage failure after a
credential was accepted, and an ended cooldown is forgotten: the
count starts over. On the signature lane a cooldown refuses only `home`, at
the challenge and at the command: `away` and `night` still go through, so a
denial of service cannot hold a house in `home` through a cooldown. The card
lane carries only `away` and `night` and stays shut for its cooldown, because
each trial `AuthenticateEV2First` touches a real card's failed-authentication
counter. Counts live in RAM, per door and lane, never per credential or
handle. The LAN API is one door. The cap of 600 requests a minute is shared by
all paired clients and charged only after the token matches.

On any bind, the server closes a connection from a peer that is not on the
home network, without a response. It answers loopback, private IPv4 (RFC
1918), IPv4 link-local, IPv6 unique-local (`fc00::/7`) and link-local
(`fe80::/10`) peers, and the IPv4-mapped forms of those. It refuses
everything else, including `100.64.0.0/10` (the shared address space
Tailscale and carrier NAT use), Tailscale's IPv6 range `fd7a:115c:a1e0::/48`
(unique-local in form, but a tailnet reaches it from anywhere) and public
IPv6 addresses. So "a key does nothing from outside the home network" rests
on the peer address: a strong default, not a cryptographic guarantee (see the
limitations).

**Logs.** The server logs startup, shutdown, reload trouble and storage
failures, and nothing per request. Sentinel tests plant recognizable handles.
They assert that none of them appears in any HTTP response body, statement,
seam write or log line.

## Running it

```sh
cd household
cargo build --release

# 1. A household: id, posture key, roster key. The KEK file goes OUTSIDE the
#    data directory, and out of every backup that holds the data directory:
#    v0 cannot exclude it for you. Ordinary system backups usually include
#    /etc, so exclude this file from yours, or keep the KEK somewhere they do
#    not reach. Then back the KEK up on its own, somewhere safe: losing it
#    loses the household (see the limitations). Its directory must exist;
#    init writes a new KEK there if the file does not, and refuses a data
#    directory that already holds a household before it writes a KEK.
install -d -m 0700 /etc/securacv
household_checker init --data-dir /var/lib/securacv-household --kek /etc/securacv/household.kek
#    → writes hub_keys.sealed (the two signing keys, sealed under the KEK)
#      and hub_keys.json (their public halves), and prints the household id,
#      the posture public key witnesses pin and the roster public key.
#    Below, D is the data directory and K the KEK file.

# 2. The first admin, then genesis (a confirm at the hub). The threshold is 1
#    unless you pass --threshold N: see the limitations.
household_checker admin-keygen --out ~/hk-admin.json
household_checker roster genesis --data-dir D --kek K --admin-key ~/hk-admin.json --physical-confirm

# 3. Enroll a credential. `client keygen` makes a reference software key.
household_checker client keygen --out ~/phone.key           # → public_key
household_checker roster propose add-member --data-dir D \
    --kind phone --class possession --public-key <hex>       # → proposal, new_member_handle
household_checker roster approve --data-dir D --proposal <digest> --admin-key ~/hk-admin.json
household_checker roster apply --data-dir D --kek K --proposal <digest> \
    --physical-confirm <first 8 hex of digest>               # → epoch, card_keys_removed

# 4. Pair a LAN client (prints a bearer token once) and serve on loopback.
#    At most 32 clients; `pair --reset` unpairs every one of them, and takes
#    effect while the server runs.
household_checker pair --data-dir D > ~/hk-token
household_checker serve --data-dir D --kek K                 # 127.0.0.1:8787

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
- `roster apply` discards every other pending proposal (each was built on
  the old epoch and can never apply), then removes every sealed card key
  whose handle is neither a card member of the roster it applied nor a card
  member of a proposal still pending, and prints
  `{"epoch", "card_keys_removed"}`. So a key provisioned for a card in a
  discarded proposal goes with it. A pending file that does not load cannot
  apply either, so it keeps no card key: it is skipped with a warning that
  names the file. If removing the keys fails, the roster is still applied:
  it prints `{"epoch"}` alone, exits 1 and names `card prune` as the retry.
  After a revoke it reminds you that LAN pairings are separate from the
  roster (see the limitations).
- `card provision --handle H [--key-file F] [--replace]` puts a card key into
  the sealed store, for a handle that is a card member of the roster or of a
  pending proposal. It reads the key as hex from a file. `--key-hex` is
  refused: a key on the command line lands in shell history and in `/proc`.
  The key file gets the KEK's rule: it may not be inside the data directory,
  nor (on Unix) carry any permission for group or others. A pipe or FIFO,
  such as `--key-file /dev/stdin`, is accepted. Delete a key file once the
  card is provisioned. Without `--key-file` it draws a random key and prints
  it once. A card that already has a key is re-keyed only with `--replace`.
  It opens the hub keys first, which proves the KEK belongs to this
  household, so a card is never sealed under a KEK the server cannot open.
  Software only: writing the key onto a physical card is not part of v0.
- `card prune` removes every sealed card key that no card member of the
  current roster or of a pending proposal names, and prints
  `{"card_keys_removed"}`: the retry when `roster apply` could not remove
  them. It skips a pending file that does not load, as `roster apply` does.
- `pair --reset` rewrites `bearers.json` empty even when that file is
  damaged, so it also repairs a damaged pairing file (it then reports that
  the file did not read, instead of how many clients it unpaired).
- `serve --bind ADDR --allow-lan-plaintext` binds a LAN address. Without that
  flag, any address that is not loopback is refused. On any bind, a peer off
  the home network is closed without a response.
- `serve --home-ttl SECS` sets how long a `home` lives: 1 to 86 400 seconds,
  43 200 by default. A short one limits what a denial of service can hold
  (see the limitations).
- `serve` reloads the roster chain, the sealed card store and the pairings
  while it runs. Each part reloads on its own. Nothing counts as seen at
  start, so the first poll loads every part, and a change applied while the
  server was starting is never missed. A part that fails to load keeps the
  previous state, is retried on every poll until it loads, and is logged
  once; a roster that fails also refuses `home` until one verifies (above).
  A roster that loads but that the checker refuses is not retried until the
  chain changes again: it stays stale for `home`.
- Only `init`, `roster genesis`, `roster apply`, `card provision`,
  `card prune` and `serve` take `--kek`. Every other command opens no sealed
  material (some read or write the caller's own key file: an admin key, a
  client key, a bearer token).
- Every command accepts only its own options. An unknown or misspelled one
  (`--key-fle`, say) is refused with exit status 2, never ignored, and so is
  an option written as `--name=value`. No error message repeats an option's
  value or a stray argument: either may be a key typed in the wrong place.

### The output seam

The checker writes the current statement to
`<data_dir>/household_posture.bin`, atomically (temp file, then rename, then
on Unix a sync of the directory) and with mode 0600. `GET /v1/posture`
returns the same 112 bytes base64-encoded, plus the decoded posture. A reader
must run every check in `posture::verify` and behave as `away` on any
failure.

An accepted `away` or `night` that does not change the current posture
publishes nothing (no new `seq`, no rewrite), so the seam does not reveal a
tap that changed nothing. `home` is always reissued, because that renews its
lapse.

A publish that fails reports an error and marks the seam dirty, because the
seam may no longer match what the checker holds: a failed directory sync, for
one, comes after the rename, so the new bytes may already be in the seam.
Until a publish succeeds, no `away` or `night` counts as changing nothing,
and the server's tick (every second) republishes the current statement,
never extending a `home`.

Nothing records which credential was used: no counters, no tap log, no
per-member timestamps, no history. The current statement still states the
minute it was issued, which is the minute of the last posture change, of
the last `home` renewal, or of a republish (`issued_at`, `not_after`, and
`seq.json`'s `last_seq >> 10`), and the modification times of the seam and
`seq.json` state its second. That was a command, a lapse, a roster change
that ended `home`, a republish (after a failed publish, or once a hub clock
that ran ahead is corrected) or a restart. Any live reader of the seam or of
`GET /v1/posture` learns each change's time to its own polling precision.
Whether that is acceptable is for the posture RFC
([`household_presence_and_posture.md`](../docs/design/household_presence_and_posture.md)
§8), next to the question of Home Assistant's exact-time diary.

## Wire summary

The wire contract is [`spec/household_keys_v0.md`](../spec/household_keys_v0.md),
and the doc comments give the detail next to the code. These are the
essentials:

- **Statement** (`posture.rs`): `SCVP`, version 1, posture byte, 16-byte
  household, `seq`, `issued_at` (floored to the minute) and `not_after`, then
  an Ed25519 signature over `"securacv-posture-v1\0" ‖ bytes[0..48]`.
  `seq` never drops below `minute(issued_at) << 10`, so a restored `seq.json`
  cannot make the hub reuse a sequence number.
- **Challenge message** (`wire.rs`):
  `"securacv-hk-v1\0" ‖ purpose ‖ door_id(16) ‖ be64(epoch) ‖ n_h(32)`.
  **That is 72 bytes**, as the spec (§4.1) states. An earlier draft said 88,
  which did not match its own field list. A test pins the length.
- **Roster** (`roster.rs`): `securacv-cjson-v1`, with signing bytes
  `"securacv-roster-v1\0" ‖ cjson`. The encoder lives in this crate. A test
  pins it to bytes the root crate's `canonical_json.rs` produced.
- **Admin approvals** (`admin.rs`) are over
  `"securacv-hk-admin-v1\0" ‖ digest`.
- **Holder revocation** (`admin.rs`, spec §5.2) is over
  `"securacv-hk-revoke-v1\0" ‖ digest`.
- **The hub's roster countersignature** (`admin.rs`, spec §5.1) is over
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
- **Losing the KEK loses the household.** The hub's posture and roster keys
  are sealed under the KEK, and so are the card keys: nothing opens them
  without it. Losing the KEK file means a new `init` (it refuses a data
  directory that already holds a household), a new posture key with every
  witness re-paired, and every admin and member enrolled again. Back the KEK
  up somewhere safe, apart from the data directory's backups.
- **The LAN API is plaintext HTTP.** v0 has no TLS. On a LAN bind
  (`--allow-lan-plaintext`), every request crosses the network in cleartext,
  including each client's bearer token (stable per client), the posture and,
  in a `/v1/command` body, the member handle. A passive listener on the LAN can
  therefore link acts to a device and to a member, and can replay a sniffed
  token to read the posture. A command still needs a signature from an
  enrolled key, and the reference client refuses a challenge whose purpose or
  door was swapped. On loopback this does not arise.
- **The home-network filter is a peer-address check.** A proxy, VPN or
  ingress that re-originates traffic from a private or loopback address
  defeats it. So the LAN API must not sit behind one that accepts peers from
  elsewhere, and a TLS-terminating proxy in front of it must itself accept
  home-network peers only. On an IPv6-only home LAN with public addresses,
  bind an IPv4 LAN address.
- **A denial of service holds the current posture.** Each connection is
  served on its own thread: at most 16 at once and 4 from one peer, where a
  peer is one IPv4 address or one IPv6 /64 (an IPv4-mapped IPv6 address
  counts as its IPv4 address). On the default loopback bind, or behind a
  proxy on the hub, every client is the same peer. When a newcomer finds a
  cap reached, the server shuts down the oldest connection that has not
  finished sending its request (in the newcomer's peer group when the
  per-peer cap is the one reached, otherwise overall) and admits the
  newcomer. Only when every counted connection is already serving a whole
  request is the newcomer closed without a response. So idle or trickling
  sockets never keep out a client that sends its request at once, as a paired
  client does. Reading a request has an absolute 5-second budget; once the
  request is routed, its response gets at least 1 second to write, because
  the request may already have changed the posture. A host that opens connections as fast as the
  accept loop takes them can still contend with a client for a slot. Any
  network denial of service against the LAN API holds the current posture,
  and if that is `home` it stays until it lapses at the home TTL, so keep
  `--home-ttl` short. A client without a token cannot start a cooldown, and a
  cooldown does not block `away` or `night` from a phone, watch, fob or FIDO2
  key. A paired client, or anyone holding a token sniffed on a plaintext LAN
  bind, can still start a cooldown (which refuses `home`) or spend the
  request cap all paired clients share (which refuses everything until the
  minute passes). Whoever holds another client's token cannot starve that
  client's session of challenges (a session at its cap of 4 loses its
  oldest), but can still displace one before its owner answers it, at a
  rate the request cap bounds.
- **The hub's clock matters.** A hub clock more than 5 minutes ahead of a
  witness's (give or take the minute floor on `issued_at`) makes that witness,
  running `posture::verify`, refuse its statements (it behaves as `away`, the
  fail-safe, and must record the refused `seq`, so that statement never
  counts later). Once the hub's clock is corrected, its tick ends a future-dated
  `home` and reissues a future-dated `away` or `night` under the corrected
  clock, so witnesses with correct clocks stop refusing it. A hub clock
  behind makes `home` lapse early on witnesses. A wall clock held back on the
  hub (by a spoofed time source, say) cannot keep a `home` alive there past
  the home TTL: the hub also times each `home` on its monotonic clock, from
  the command. Plain NTP is unauthenticated: take the hub's time from an
  authenticated source, such as chrony with NTS.
- **Threshold 1 means one admin decides.** `roster genesis` defaults to
  threshold 1. With threshold 1, one admin's approval is all any change needs
  (an add also needs the confirm at the hub), including removing every other
  admin once the 24-hour pending period has passed, and nothing announces the
  pending change beyond `roster pending`. So "one stolen admin key cannot
  take over the roster" holds only with threshold 2 or more. With exactly two
  admins and threshold 2, losing one admin key leaves no way to remove it
  (removal needs two current admins), so threshold 2 wants at least three
  admins.
- **LAN pairings are separate from the roster.** A bearer token cannot change
  the posture, but it does read it: `GET /v1/posture` returns the posture
  word, `not_after` and the signed statement (`issued_at` to the minute, and
  `seq`). Tokens are deliberately not linked to members, so revoking a member
  leaves its device paired, and there is no per-token unpair. `pair --reset`
  unpairs every client, and takes effect while the server runs; pair again
  the clients you keep. `roster apply` says so after a revoke.
- **A revocation ends everyone's `home`.** A roster that drops any member
  ends a current `home` at the server's next tick, and so does a roster that
  goes stale; if that `away` fails to publish, every tick tries it again,
  and the ended `home` is never republished. Nothing records who set
  `home`, so a revoked member's `home` cannot be told from anyone else's:
  ending it is the fail-safe, and members still enrolled set it again. A
  roster that only adds members does not end it.
- **The time of the last change is readable.** The seam, `seq.json` and
  `GET /v1/posture` state when the posture last changed, or `home` was last
  renewed, or the statement was last republished, to the minute from the
  statement and to the second from the files' modification times (see
  [The output seam](#the-output-seam)).
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
  simply never authenticates. It does remove every key that neither the new
  roster nor a proposal still pending names as a card member (it discards the
  proposals the new epoch made stale first). If that removal fails, the
  roster is applied anyway and `card prune` retries it.
- **The CLI signs only Ed25519 admin approvals.** ES256 enclave and FIDO2
  admin approvals are checked by the library, but they must be made
  elsewhere and imported with `--approval-file`.
- **Tests pin the encoding, not the scheme.** The AN12196 vectors and the
  canonical-JSON vector pin the encodings. Nothing here has had an external
  security review.
