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
  `VerifyingKey::from_bytes` / `.verify(` site.
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

### Stated, not fixed

- `reference_v1`'s witness-page pre-image joins fields with `:` and is not
  injective; the spec now says so and what a verifier must reject
  (`spec/witness_api_v1.md` §4.1). The next `chain_format` length-prefixes.
- The break-glass single-use token ledger (`consumed_break_glass_tokens`) is
  an unsigned table; a database writer can delete a row and replay a token
  once more inside its bucket. A device-signed `TokenRedeemed` receipt row is
  the fix and is tracked.

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
