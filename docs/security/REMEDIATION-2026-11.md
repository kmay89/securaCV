# SecuraCV — Security Remediation Pass (2026-11)

A findings-level review of every tree against a well-resourced adversary —
one who reads the database at rest, sits on the LAN, controls the broker or
an upstream host, ships a hostile model library, or opens a pull request —
followed by the code-level fixes. The rule of the earlier passes holds: no
finding is closed by wording alone without saying so, and where a fix is
structural work for later, this page says what still stands.

The audit was run as eight independent reviews (backoffice door, backoffice
public surface, kernel network code, kernel cryptography, firmware, website
and Lab, Home Assistant integration and CI, desktop and Apple apps). Each
finding below names the file it lives in; each fix carries a regression test
unless the line says otherwise.

## Kernel (`src/`)

### Evidence integrity

- **Retention could be used to launder a deletion.** `SqliteSealedLogStore::
  enforce_retention_with_checkpoint` aged rows by the bare `created_at`
  column, which neither the entry hash nor the signature covers. An actor
  with a database handle but no signing key could stamp live rows as ancient
  and have the device itself sign a checkpoint over their deletion; every
  verifier then started from that checkpoint and reported `valid`. Age now
  comes from the **signed** time bucket in each row's payload, and the pass
  re-derives every hash link through the first kept row before it prunes: a
  rewritten prefix refuses the pass (`CONFORMANCE_RETENTION_CHAIN_BREAK`,
  sealed as a storage failure by `witnessd`) instead of being checkpointed
  over. The in-memory store ages the same way. Five tests that aged rows by
  rewriting the column — the attack, verbatim — now seal old buckets instead.
  `src/storage.rs`; tests in `src/lib.rs`
  (`retention_ages_rows_by_the_signed_bucket_not_the_created_at_column`,
  `retention_refuses_a_prefix_whose_payload_was_rewritten`,
  `retention_prunes_by_the_signed_bucket_when_created_at_is_newer`).
- **Hybrid vault mode was an OR, not a hybrid.** `VaultCryptoMode::Hybrid`
  wrapped the DEK itself under the master key beside the ML-KEM path, so
  either secret alone opened the envelope — no stronger than its weaker
  half. New hybrid envelopes (`kem_alg: hybrid-ml-kem-768`) derive the DEK
  from the ML-KEM shared secret **and** a master-key-wrapped classical
  secret, with no fallback to either; the earlier shape still decrypts and
  is never written again. `decrypt_v2` also refuses any `aead_alg` or
  `kem_alg` spelling it does not implement before touching a byte, so a
  future second scheme is not selectable by an at-rest edit of the
  unauthenticated header. `src/vault/crypto.rs`, `spec/break_glass.md`.
- **Weak Ed25519 keys and non-strict verification.** Every verifying key the
  kernel parses — from storage, the wire, a quorum policy, a rotation record,
  a bundle — now goes through `crypto::signatures::verifying_key_strict`,
  which refuses a small-order point (under such a key `(R = identity, s = 0)`
  verifies for every message), and every signature check is
  `verify_strict`. `src/crypto/signatures.rs` and every former
  `VerifyingKey::from_bytes` / `.verify(` site, the fleet `state` publish
  verifier that landed alongside included.
- **Vault nonces are born random.** The three AEAD nonces in
  `src/vault/crypto.rs` were zero-filled buffers that the OS RNG overwrote a
  line later; `fresh_nonce()` now draws every byte as the array is built, so
  no constant nonce ever exists for a dropped fill call (or a static
  analyzer) to find.
- **`court_export` pinned the key it was verifying.** The kit's custody
  record described a pinned-key verification while the key came from the
  database under review. `--public-key` / `--public-key-file` now anchor the
  lineage at a key supplied out of band (the database's own identity must
  sit in that lineage); without one the kit, its `MANIFEST.json`
  (`identity_verified: false`) and the console say *self-consistent under the
  key the database names; identity not independently confirmed*, the wording
  `log_verify` already used. `src/bin/court_export.rs`.
- **Seeds in plain strings.** `ResolvedSeed.seed` and `generate_device_seed`
  are `Zeroizing<String>`; `KernelConfig` zeroizes its seed on drop and
  redacts it from `Debug`. `src/crypto/mod.rs`, `src/lib.rs`.

### The detector sandbox

- **The child held the signing key and had a free-text channel out.** The
  sandbox child is a fork of a parent that already holds the device seed, so
  the `ptrace`/`process_vm_*` denials protected nothing it needed: a hostile
  model library could read the key from its own heap and walk it out through
  `CandidateEvent.zone_id` (any string, sealed verbatim, republished by the
  bridges) or `correlation_token` (any 32 bytes). The parent now rebuilds
  every candidate from what it already knows — the module's declared zone
  ids (`Module::declared_zone_ids`, fail closed), its allowed event types,
  the frame's bucket, a token the parent mints from the view — and the
  child's say is reduced to *which* declared zone, *which* allowed type,
  whether a token applies and a two-decimal confidence, sixteen candidates a
  frame at most. `src/module_runtime/mod.rs` (`admit_candidates`).
- **Still open, structural:** the child should never hold the key at all. The
  worker should be spawned before `Kernel::open` and fed views over a pipe.
  Tracked here; the admission layer above bounds the damage until then.
- **A hung or hostile module wedged ingest.** No reply deadline, no resource
  limit, `fork` allowed. The child now runs under `RLIMIT_CPU`, dies with the
  thread that forked it (`PR_SET_PDEATHSIG`), has its standard streams
  pointed at `/dev/null` (the daemon's log sink was a second channel out),
  and may create threads but not processes (`clone` filtered by
  `CLONE_THREAD`, `clone3` answered `ENOSYS` so glibc falls back to the
  filtered `clone`, `fork`/`vfork` denied). The parent reads the reply with
  a `poll(2)` deadline and kills a child that misses it, so a frame that
  never returns costs one inference error, not the witness.
  `src/module_runtime/sandbox.rs`.
- **Denylist gaps.** `rt_sigqueueinfo` / `rt_tgsigqueueinfo` /
  `pidfd_send_signal` (a `SIGKILL` to the daemon from the same uid), SysV
  `shm*`/`msg*`/`sem*`, `inotify`/`fanotify` (a path-existence oracle with no
  `open`), `unshare`/`setns`/`mount`/`umount2`/`pivot_root`/`chroot`,
  `sendmmsg`/`recvmmsg`, `uselib` are denied. The allowlist inversion
  tracked in the 2026-08 pass remains the proper end state.

### Network services

- **`/api/fleet` served who-is-home words any broker client could forge, to
  anyone on the LAN, by default.** The default roll-call is now `self` (the
  kernel's own row). `full` — every peer's row including the wellbeing words
  from the unsigned `state` topic — is the Witness Wall's explicit opt-in
  (`api.fleet_rollcall` / `WITNESS_API_FLEET_ROLLCALL`, and an add-on
  option). The route's comment that it "says nothing an observer could not
  already see" is true again. Signing the `state` publish on the firmware
  side so `full` can be served safely is follow-up work. `src/api/mod.rs`,
  `src/config.rs`.
- **Webhook adapter was slowloris-able.** Per-operation timeouts only; a
  sender dripping a byte every few seconds held a worker forever, sixteen of
  them the pool. Every connection now has the API's absolute 30-second
  budget (`DeadlineStream`), TLS handshake included. `src/adapter/webhook.rs`.
- **Break-glass approvals replayed within a bucket.** The request hash
  carries no nonce, so after a granted unseal the same request re-opened to
  the same hash inside its 10-minute bucket and the approvals already
  collected verified again. A redeemed hash no longer reopens
  (`BreakGlassSession::redeem` / `409 request_already_redeemed`).
  `src/break_glass/session.rs`, `src/break_glass/http.rs`.
- **Break-glass lockout table could be filled and disabled.** One failure
  from an address pinned its entry forever; 4096 addresses (one IPv6 /64)
  filled the table and every later address was never locked out. Settled
  streaks expire, and a full table evicts the soonest-expiring entry rather
  than leaving a new address untracked. `src/break_glass/server.rs`.
- **HAP pair-setup attempts reset on restart.** The accessory-wide counter
  lives only in memory, so a supervisor that restarts the bridge handed a
  code-guesser a fresh hundred tries each time. It is persisted with the
  pairings. `src/bridge/hap/store.rs`, `src/bin/hap_bridge.rs`.
- **`/api/sign-announce` signed any extra field.** The kernel signs the bytes
  it is handed; a capability-token holder could obtain the device's
  signature over any JSON that also named the kernel. The payload's
  top-level and `api` keys are now limited to the announce contract.
  `src/api/announce.rs`.
- **Sensor adapters let the payload rename the zone.** A broker client could
  write sixty-four characters of its choosing into the signed log as an
  adapter-attested zone. The operator's configured label stands unless the
  route sets `allow_zone_override = true`. `src/adapter/mqtt_sensor.rs`,
  `src/bin/adapter_host.rs`.
- **`adapter_host` spoke plaintext MQTT to any broker.** It now resolves the
  broker the way the bridges do: loopback unless `allow_remote_mqtt = true`,
  `mqtts://` with webpki roots, a configured CA and optional client
  certificate, never an insecure-verify mode. `src/bin/adapter_host.rs`.
- `adapter_host`'s SocketCAN frame read is `ptr::read_unaligned` (the
  byte buffer carried no alignment promise).
- **Break-glass token redemption lived only in an unsigned table.** A
  redeemed token's nonce went into `consumed_break_glass_tokens`, which
  nothing signs; a database writer could delete the row and present the
  token again inside its bucket (the in-memory `consumed` flag is per
  process). Redemption is now recorded twice, burn first, in one
  transaction: the nonce is burned, then a device-signed, hash-chained
  `TokenRedeemed { nonce, receipt_entry_hash }` row is appended to
  `break_glass_receipts`, restating the `Granted` receipt it spends. The
  burn refuses any nonce the chain already shows redeemed, so a deleted row
  buys no replay; the token is validated (signature, live bucket, grant
  quorum) before anything is written, so a tampered token file cannot
  poison the chain. Every chain walk (`log_verify`, `break_glass receipts`,
  the boot verification, the evidence-envelope verifier in Rust and JS)
  fails on a nonce redeemed twice, a grant redeemed by two nonces, a
  redemption naming no grant or restating it wrongly, a burned nonce with no
  signed row, or a signed row with no burn row (`FailureKind::
  TokenRedemption`). `break_glass unseal` therefore needs the signing seed,
  not only the database key. What the walk cannot see — a redemption row
  dropped from the chain tail with its burn row — is what anchoring the
  receipt head is for; `spec/break_glass.md` says so. `src/lib.rs`,
  `src/break_glass/core.rs`, `src/verify.rs`, `src/envelope.rs`,
  `src/break_glass/cli.rs`, `viewer/verify_core.js`.

### Stated, not fixed

- `reference_v1`'s witness-page pre-image joins fields with `:` and is not
  injective; the spec now says so and what a verifier must reject
  (`spec/witness_api_v1.md` §4.1). The next `chain_format` length-prefixes.

## Backoffice — the door (`backoffice/`)

`backoffice/SECURITY.md` states the guarantees; each of these is a place the
code did not deliver them. Operator-facing wording there was corrected in the
same change.

- **The client chose its own address.** `clientIp()` took the *first*
  `X-Forwarded-For` hop; the policy promised the last one the trusted proxy
  appended. Behind any proxy that appends (nginx, Fly, a two-hop chain) a
  request header chose the address every per-address control used: the
  login limiter and lockout, `ADMIN_ALLOW_CIDRS`, `SESSION_BIND_IP`, the
  tripwire, the audit log. The header is now walked from the right, trusted
  hops skipped, the first untrusted hop is the client; an untrusted peer's
  header is ignored. `src/http.mjs`.
- **The second factor could be swapped with a cookie alone.** Rotating the
  authenticator and "log out everywhere else" needed nothing but a live
  session, so a stolen cookie became durable control. Rotation needs the
  current password and a code from the *current* authenticator beside the
  new one; logging other sessions out needs the password; both are audited
  with the address. `src/auth.mjs`, `src/admin_users.mjs`.
- **Per-address controls were per-string, and reset wholesale.** Limiter,
  lockout and tripwire keyed on the raw address (an IPv6 client rotating
  inside its /64 got a fresh budget each time) and `clear()`ed every
  counter past ten thousand keys (one botnet, everyone's lockout gone). They
  key on the /64 for IPv6 and shed the oldest entries instead. `src/guard.mjs`,
  `src/http.mjs`.
- **The audit chain did not look below id 1 and did not cover `id`.** Rows
  inserted with a zero or negative id were never walked; `id` was outside the
  MAC; fields were joined with a separator a field could contain. The MAC
  now covers `id` over a JSON-encoded tuple, the walk starts at the lowest
  id and names any row below 1, old marks still verify. `src/db.mjs`,
  `src/integrity.mjs`.
- Invitation tokens and session ids are stored hashed (migration 016; live
  sessions end once, open invitations are re-issued); the invite link is
  shown once in the response, not carried in a `GET` query. Login POSTs are
  under the same cross-site refusal as every other form. The audited email
  on a failed login is capped and scrubbed. Role drift: `api/users`,
  `health/external` and `POST api/backups` are owner-only; `api/exceptions`
  matches `/exceptions`. The sign-in-link request does the same database
  work for an unknown address as for a known one.
- **Still open:** `scrypt` runs synchronously on the event loop for every
  login and API-token check; a worker or `crypto.scrypt` is the fix.

## Backoffice — the inbox and the public surface (`backoffice/`)

- **Two quadratic parsers in inbound mail froze the process.** A bounce
  scan ran an address regex over the whole body (300 KB took 44 s of CPU;
  the cap is 8 MB) and `htmlToText` lower-cased the whole document once per
  tag. One mail to the support inbox stalled the Stripe webhook, checkout,
  the health probe and the admin. Both are linear and run on the capped
  text. `src/modules/support.mjs`.
- **One crafted mail lost the rest of its batch.** Every fetched message
  was flagged seen before parsing, MIME nesting was unbounded (3,800 levels
  overflowed the stack), and one exception ended the loop. Nesting is
  capped at ten; each message is ingested on its own, and a failure files
  an exception naming it. `src/jobs.mjs`, `src/modules/support.mjs`.
- **Anyone could bounce any customer.** A mail whose `From` said
  `postmaster` marked every address in its body bounced, suppressing all
  campaign, automation and recovery mail to them. A DSN now counts only
  when it references one of our own outbox ids, and only for that address.
- **Third-party inboxes could be bombed** through the consent request
  (every call sent a mail and rotated the token) and cart recovery (one
  mail per expired Checkout session, no per-address cap) — SECURITY.md said
  they could not. Consent mail is one per address per hour; recovery mail
  one per address per day and only for a known contact; checkout is
  limited per email. `src/modules/campaigns.mjs`, `recovery.mjs`,
  `server.mjs`.
- **Status probes were an SSRF oracle from the ops role.** The hostname was
  checked once at save time as a string; IPv6-mapped literals passed,
  redirects were followed, bodies were unbounded. Every run resolves the
  host and refuses private, loopback, link-local, mapped, NAT64 and ULA
  answers; redirects are the result, not followed; reads are capped at
  2 MiB, as is every other outbound read. `src/modules/status.mjs`,
  `src/net.mjs`.
- **"Forget this person" left the address behind** in checkout sessions,
  invoices, warranty claims, shipped/canceled timeline lines, exception
  bodies and audit detail. The transaction blanks them all, the export
  shows them, and addresses are no longer written into the immutable
  audit chain or timeline text at the source. `src/modules/privacy.mjs`.
- `/store/session/:id` no longer hands out the signed order-status link;
  confirm and unsubscribe act on `POST`, not on a link scanner's `GET`; the
  waitlist quantity is an integer; the form-origin acknowledgement carries a
  fixed subject and no unverified name; mail threading matches only our
  own message ids and files a stranger's reply as a flagged note; Stripe's
  timestamp must be finite.
- **Stated, not fixed:** the IMAP client still flags a message seen while
  fetching; the per-message exception row is what makes a loss visible. A
  first-time shopper with no contact row no longer receives cart-recovery
  mail — a product decision, stated in SECURITY.md.

## Website, evidence viewer and Lab (`viewer/`, `canary-local/`, the site)

- **"Verified" meant "signed by the key the record names."** The evidence
  viewer verified an envelope against `provenance.device_public_key` from
  the envelope itself; the Witness Wall page did the same over plain http
  and printed "Record verified here on this device". `verifyEnvelope` takes
  a trusted key the way the bundle path already did; without one the
  headline is *self-consistent (identity not confirmed)*. The Wall pins the
  hub's key on first use, shows its fingerprint, verifies against the pin
  after, and alarms on a change. `viewer/verify_core.js`,
  `viewer/template.html`, the site's `tv/app.js`.
- **The viewer had no Content-Security-Policy** — the one page whose job is
  to render hostile input had no second line of defense. The build emits
  hashed script and style sources, refuses inline style attributes, and a
  behavioral test feeds every string field a hostile element.
  `viewer/build.mjs`.
- **`figures.html` rendered fork-authored SVG as `innerHTML`** for a
  `?ref=` the API could not resolve. The ref must resolve to a branch or
  tag, and the SVG goes through an element and attribute allow-list.
- The emulator badged real fleets "chain ✓" without checking anything;
  live tiles say *presence only*. The Lab flasher no longer drops silently
  to checksum-only for a loopback manifest override; it asks, naming the
  origin. `?src=` and `?kernel=` are honored only for local origins.
- **Stated, not fixed:** the site's `style-src 'unsafe-inline'` is what
  turns a text spoof into a full-page overlay; hashing every page's style
  block is a separate project.

## Desktop Flasher and Lab app (`desktop/`, `desktop-lab/`)

- **A LAN host could collect a device's bearer by announcing its
  `device_id`.** The fleet book re-pointed a device at whatever address an
  mDNS announcement named, and the next Identify or Update sent the stored
  token there over http. The bearer never travels in the clear now: before
  a token goes to a `(device_id, address)` pair the app agrees a session key
  with the device over X25519, the device signs the whole transcript with
  the Ed25519 identity key the book pinned at flash time, and the token
  rides only sealed under that key (`Authorization: Sealed`, ChaCha20-
  Poly1305, bound to the method, the path and a counter). A host that
  announces the device's name gets a ciphertext it cannot open; a relay to
  the reachable genuine board gets one request it cannot read, replay or
  redirect. There is no plain-bearer fallback: firmware without the
  handshake is told to update once over USB. The identity badge stays
  shown-only, and a device that turns up at a different address than the
  one it was first proven at is still confirmed through a native dialog.
  Both flashers' token cards say how the key travels, in the same words.
  `desktop/src-tauri/src/seal.rs`, `fleet.rs`, `desktop/src/app.js`,
  `canary-local/assets/flash-core.js`; the firmware side below. **Still
  open:** the request body and the answer are not sealed, and the phone
  app, the dashboard and Home Assistant still send the plain bearer.
- **`host_is_local` was a name check.** Single-label names and
  `.lan`/`.internal`/`.home.arpa` resolved through unicast DNS and the
  answer was never checked, so a hostile resolver could send a bearer off
  the LAN. Every resolved address must be private, loopback or link-local
  and the client is pinned to those addresses; the Lab app and the hub
  onboarding path use the same gate, and onboarding shows the resolved
  address and warns when the hub answered elsewhere before.
- **Three commands took any absolute path from the webview.** They take an
  opaque handle from a Rust-side picker that refuses non-regular files and
  anything under the app's own data directory. The raw-disk write raises a
  native confirm beside the JS one. Without an OS secret store, passwords
  and tokens are no longer written to `localStorage`.

## Home Assistant integration, add-on and CI (`custom_components/`, `privacy_witness_kernel/`, `.github/`)

- **Supervisor discovery could read a file and move an entry.** The
  discovery step took `host`, `port` and `token_file` verbatim from the
  payload, read the first bytes of that path and sent them as a bearer to
  that host before any confirmation, and re-pointed an existing entry with
  no identity check. The token file must be the add-on's own; nothing is
  read or sent before the confirm step; an existing entry moves only when
  `/api/identity` at the discovered URL, asked with the entry's own
  credential, answers with the pinned key and device id. A signed challenge
  for discovery moves is the stronger follow-up. `config_flow.py`.
- Every JSON answer from the hub is read under a 4 MiB cap (only the bundle
  download was). Hub-supplied zone ids are validated and capped at 64
  entities per entry. The pairing forms say when the URL is plain http to a
  host that is not the add-on.
- The add-on's `device_key_seed` is a password option; it gains
  `fleet_rollcall` for the kernel's new default. The seed's location in
  `/config` (inside every backup and every file-sharing add-on) is a data
  migration and stays open.
- **Supply chain.** The add-on image pins its bases by digest and verifies
  `rustup-init` and `cargo-chef` by sha256 (no more `curl | sh`);
  `packages: write` is scoped to publish jobs; the sidecar and backoffice
  images carry provenance and an SBOM (the add-on image's shape is pulled
  per-arch by the Supervisor and is release-tested first); emsdk is checked
  out at a pinned commit, PlatformIO at a pinned version; CODEOWNERS covers
  `.github`, `.claude` and `scripts/hooks`; dependency review fails on
  moderate; `deny.toml` declares its advisory policy. The release packages a
  source tarball under the existing checksum and attestation globs so
  `install.sh` can require its line in `sha256sums.txt` (a release cut
  before this change falls back with a loud warning). `cloud-init`
  installs Docker from the apt repository with the key fingerprint checked.
  The website's `social-record` workflow validates the issue-derived post id
  and passes it through `env`.

## Firmware (`firmware/`)

- **The local API's bearer could only be sent in the clear.** canary-wap
  gains the sealed bearer: `GET /api/device/seal?nonce=&client_pub=` (no
  token; the enroll card plus an ephemeral X25519 key the identity key
  signs over the transcript), a four-slot session table with a 15-minute
  TTL and a per-session counter, and an `Authorization: Sealed
  <sid>.<seq>.<hex>` form that both bearer gates in `api_auth.h` open
  through one reader before the usual constant-time compare, under the
  same per-address lockout; a seal that does not open answers 401 with a
  word (`no_session`, `expired`, `replay`, `bad_tag`, `bad_format`,
  `unsupported`) so a client knows whether a new handshake helps. Host-
  tested over real X25519 / HKDF / ChaCha20-Poly1305 against reference
  vectors the Flasher's Rust client pins too (`tests_host/test_api_seal.cpp`).
  `api_seal.h`, `seal_api.h`, `api_auth.h`, `device_signature.{h,cpp}`
  (the `seal` canonical and `sign_seal`, in both copies).
- **The peer protocol's reboot branch re-opened the replay window.** Any
  captured frame with a lower uptime than the peer's current one passed as
  "the peer rebooted", reset the sequence counter, and took the frame's
  source as the peer's address — so a LAN host could re-inject old
  events, stale heartbeats and election inputs, and redirect the
  coordinator-bound unicast to itself. Frames now carry a per-boot random
  `boot_id` (the formerly reserved header byte, under the HMAC tag); a new
  boot is taken only inside a small sequence and uptime window, once per
  30 s per peer, and provisionally until a second in-order frame confirms
  it; the address is taken only from a frame that advanced a counter the
  row already held. `docs/TRUST_MODEL.md` states exactly what holds and the
  residuals (a captured pair of a boot's first frames the receiver never
  saw; a sender with no `boot_id`). `firmware/common/network/peer_protocol.h`,
  `peer_link.cpp`.
- **Beacon writes were gated by the landing-page session any LAN host can
  mint.** With two WAPs an attacker could drive `originate` on one and
  `cosign` on the other and obtain a genuinely dual-signed life-safety
  alert with no human (latent: the channel defaults off). All twelve
  mutating Beacon routes take the bearer; a null token refuses instead of
  passing. `canary-wap/.../beacon_api.h`, `docs/security/THREAT_MODEL.md`.
- `/api/ota/config` reads its body through the shared bounded reader and
  applies all-or-nothing. A v1 OTA manifest no longer bypasses the board
  check, and a persisted manifest-version floor refuses v1 once a signed v2
  has been accepted (`SECURACV_OTA_ERR_MANIFEST_DOWNGRADE`); the USB
  drop-file and BLE OTA paths get the stricter board check and keep their
  own policy for the floor. `SECURACV_OTA_SKIP_CERT_VERIFY` is renamed
  `SECURACV_OTA_SKIP_CN_CHECK`, which is what it does.

## Still standing, across trees

- The detector sandbox child still holds the signing key (admission bounds
  the channel; a key-less worker is the fix).
- `/api/fleet` in `full` mode still serves broker-forgeable wellbeing words;
  signing the firmware's `state` publish would let `full` be served safely.
- The desktop challenge gate is not channel binding; a relay to a reachable
  genuine board still works.
- The add-on's device seed lives in `/config`; moving it is a data migration.
- The backoffice runs `scrypt` on the event loop; the IMAP client flags a
  message seen while fetching.
- The site's inline styles keep `style-src 'unsafe-inline'`.
- `reference_v1`'s witness pre-image is not injective; the break-glass token
  ledger is an unsigned table.
