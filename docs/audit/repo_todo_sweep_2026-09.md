# Open work — the backlog (three repos)

Every open item across `kmay89/securaCV`, `kmay89/securacv_website` and
`kmay89/securacv-homeassistant`, from the 2026-09-20 sweep and the passes
since. What gates the v1 tag is in [`v1-roadmap.md`](../v1-roadmap.md); this
file is everything else that is unfinished.

- **Only open items live here.** When you finish one, delete it in the same
  PR as the fix; `CHANGELOG.md` and `git log -p` on this file are the history
  (the full ledger, completed items included, last stood at commit `018ee0e`).
- IDs are stable: never renumber, add new items at the end of a section.
- Keep an item to what is wrong, where, and what remains. No wave diaries.
- Line numbers drift; file paths and quoted phrases are the durable pointers.
- Tags: **[code]** a session can finish it · **[human]** needs hands,
  hardware, an account or money · **[decision]** needs a maintainer call
  first.

---

## 0. The five unlocks (do these and whole sections light up)

These are the choke points the sweep kept hitting. Each one un-gates many
items below.

- [ ] **U1 [human] One bench session on real hardware.** Seven built firmware
  capabilities are "Built · bench-gated" (`docs/hardware/dev_playground_todo.md`,
  "the real gating work" section),
  `docs/audit/hardware_verification_checklist.md` (23 items at the sweep,
  more since) is entirely unrun, and 12 board pin maps carry "NOT yet
  validated on bench". The playground doc calls this "the thing an AI cannot
  do from CI." Un-gates: F-section flags — now also the bench half of most
  firmware items that landed in #1703 and #1704, each named in its item
  below (e.g. runbook Track D for F15, the checklist's WPA3/PMF rows for
  F16; the Beacon and Opera-mesh rows also wait on code: F30, F31, F33) —
  C6, C7, plus the sentinel/sense bench boxes (the sentinel's now include
  its Phase 1a rows, F22).
  *Note (2026-11-04, #1920):* wave 23 adds bench rows this session owns: the
  ESP32-S3's first real die-temperature readings (F438: `/api/thermal`, the
  envsens tamper counter and the camera state over a PEEK session on a XIAO
  ESP32S3 Sense); the hub hand-off's six `magic_pairing.md` §9 rows (F254) and
  the flagship's one-asker tap (F409) in the checklist; whether NimBLE stores
  `authenticated == 0` for a Just Works bond on the WAP (F446); the RS485
  exception on a 4.3B's glass (F317); and the WAP dashboard's bond-store note,
  late bring-up subtitle and new chirp icons on a device (F350@13406, F418).

- [ ] **U2 [human] The release signing-key ceremony.** The pinned key is the
  all-zero placeholder (`desktop/flash-engine/src/release.rs`,
  `canary-local/assets/flash-core.js`), so both flashers accept firmware on
  checksum alone. See `docs/RELEASE_BUTTONS.md`. Un-gates: signed-release
  verification everywhere; the flashers' "no signed release yet" fallbacks
  retire themselves.

- [ ] **U3 [human] Apple Developer account.** The signed release workflows
  (`tvos-release.yml`, `ios-release.yml`) exit green until their enable
  variables and `APPLE_*` signing secrets exist (`tvos/README.md`, "Status").
  `ios-selfheal.yml` is *not* gated on it — PRs always get an unsigned
  simulator build with no Apple credentials. Un-gates: A8, A13, tvOS/iOS
  App Store presence, Critical Alerts entitlement request, signed builds
  that exercise the CloudKit path.

- [ ] **U4 [human] FCC authorization work + responsible-party facts.**
  `store.json` has `part15b_sdoc: false` and blank responsible-party and
  `ship_from` blocks; `docs/strategy/29-fcc-and-product-compliance-diligence.md`
  holds the checklist. Un-gates: W-section store items for radio SKUs.

- [ ] **U5 [human] Stripe + PirateShip one-time setup (~1 hour).**
  `securacv_website/store-README.md` "One-time setup". Un-gates: W1 — the two
  already-FCC-clear parts SKUs could sell today.

Smaller one-time human acts, same flavor:

- [ ] **U6 [human] Set `MIRROR_PAT` in the monorepo secrets.** Until then the
  HACS mirror refresh is inert-but-green
  (`.github/workflows/homeassistant-mirror.yml` warns and files an issue), and
  every change to the carried `custom_components/securacv` files has to be
  resynced into the mirror by hand. The secret: a fine-grained PAT scoped to
  `kmay89/securacv-homeassistant` only (Contents, Pull requests and Issues
  read/write), saved as the repository secret `MIRROR_PAT` here; then
  dispatch Actions → "homeassistant mirror" on `main`, confirm it reports the
  mirror already matching or opens a `bot/mirror-sync` PR, and close #1637.

- [ ] **U7 [human] Open the staged home-assistant/brands submission.**
  `brands/home-assistant/README.md` says "not submitted"; it is the only route
  to an integration icon on HA < 2026.3.

- [ ] **U8 [human] Let the hub-freshness workflow open its PR.**
  `homeassistant-freshness.yml` refreshes the hub's upstream snapshot (since
  the 2026-10 cleanup it also reruns `gen_hub_seed.py`,
  `gen_hub_provision_bundle.py` and `gen_start.py`, in canary-local.yml's
  drift order) and pushes with `secrets.FRESHNESS_PAT || github.token`. The
  default token does not get to open the pull request here, so the newer
  snapshot sits on `bot/homeassistant-freshness` and #1662 stays open. Add the
  `FRESHNESS_PAT` secret (fine-grained, this repo, contents and pull-requests
  write; the workflow's comment says why a PAT also lets the PR's own checks
  run) or enable "Allow GitHub Actions to create and approve pull requests",
  re-run the workflow, merge its PR and close #1662.

- [ ] **U9 [human] Delete the four stale Lab draft releases.** `app-v0.1.1`,
  `app-v0.1.2`, `app-v0.2.0` and `app-v0.2.1` are drafts older than the
  published `app-v0.2.4` (listed by the releases API on 2026-10-10). Since the
  2026-10 cleanup the release planner ignores them (`release-everything.yml`'s
  `drafts` job), but nothing deletes them. Deleting a release is destructive,
  so it is a human's act: remove the four from the Releases page.

- [ ] **U10 [human] Give the HACS mirror a description and topics.**
  `kmay89/securacv-homeassistant` has no repository description and no topics
  (checked 2026-10-10); the HACS store listing reads both, and the
  hacs/default submission checks the topics, which is why the mirror's
  `validate.yml` hacs job carries `ignore: topics`. An admin runs
  `gh repo edit kmay89/securacv-homeassistant --description "…" --add-topic …`
  (the 2026-10 audit's draft: "SecuraCV for Home Assistant (HACS): signed
  witness events from Canary devices and the Privacy Witness Kernel, never
  footage, never identity. Developed in kmay89/securaCV.", topics
  `home-assistant`, `homeassistant`, `hacs`, `hacs-integration`,
  `homeassistant-integration`, `home-automation`, `mqtt`, `privacy`,
  `local-first`), then deletes the `ignore: topics` line and its comment and
  confirms Validate is green on `main`. Two mirror chores fit the same visit:
  issue #13 ("HACS mirror has drifted") should close itself on the first
  `main` push after the 2026-10 cleanup lands (mirror-freshness's new close
  step), so close it by hand if it does not; and Dependabot PR #25 wants a
  rebase so the new `lint` job runs on it.

---

## 1. Firmware (securaCV monorepo)

The canonical defect ledger is `firmware/ESP32S3_OPTIMIZATION_ROADMAP.md`
(30 prioritized items; its own §6 table governs). The sweep spot-checked the
P0 rows by source inspection; status below reflects that inspection, not the
doc's claims. ("Verified" is reserved in this repo for a signature checked
against a pinned key or a claim proven on hardware — nothing below claims it.)

### P0 — confirmed still open by source inspection

- [ ] **F38 [code+decision] Roadmap item 18's hardware key protection is
  still open** (a P1 row, listed here beside its sibling F5). Its two
  host-testable parts landed with F5 (#1704): (a) the PIO canary tree's
  first-boot identity keygen runs inside `bootloader_random_enable()` /
  `bootloader_random_disable()`, the PR #994 pattern the other three trees
  already had (the Scout key and the mesh pairing keys are generated after
  RF is up and are deliberately left bare), and a `regression_check.sh` gate
  now requires the call in all four keygen files; (b) `{seq, chain_head}`
  persist as one atomic 39-byte NVS blob sealed with a CRC-16
  (`firmware/common/witness/chain_state.h`, host-tested; the legacy pair
  stays a read-only fallback, and a legacy seq ahead of the blob's wins, so
  a re-upgrade after a downgrade does not re-sign seqs). Open, recorded in
  roadmap row 18 and not attempted: (c) a DS/HMAC-bound key — the DS route
  is RSA-only and reserved by `hardware_root_of_trust.md` §5.4 / §8 #4, and
  `key_at_rest.h` reserves its `hw-bound` label — and (d) an eFuse/RTC
  rollback anchor (design only); both need the IDF-component toolchain
  (roadmap item 9) and a bench. Also skipped: the canary-wap carry of
  `chain_state.h` (three write sites, a staged copy, a `check_csi_sync.sh`
  entry; since done: F59, #1749, carried it, so the sketch writes one
  `chain_st` blob through a staged `chain_state.h` that `check_csi_sync.sh`
  holds) and an entropy self-check. Bench (U1): K1's fresh-unit step (the
  keygen's `bootloader_random` pair shares the SAR ADC the battery monitor
  has already opened — does the reading survive?) and K2 (a power cut
  between record and persist never leaves a torn seq/head pair).

(Roadmap items 1 and 2 were confirmed **fixed**, and the roadmap doc now says
so — see D2 below.)

### Timeline & time

### Mesh / fleet / beacon

- [ ] **F30 [code+decision] Beacon §3.3 pairing flow.** Nothing can add a
  member to a beacon set: build the `PAIR_OFFER` wire type, an ephemeral
  X25519 exchange with a 6-digit confirmation, the FE-gated beacon-set write
  with `has_x25519_pubkey`, and `spec/beacon_cap_gateway_v0.md` §2.2's cap
  of three non-revoked gateway entries enforced at the add. Design-review
  sized, its own PR. Until it lands no board can pair, so F13's encrypted
  two-device path (ALERT and CANCEL) is unreachable.

- [ ] **F31 [code+decision] The Beacon runtime loop is not wired.** Wire
  `beacon_channel` into `canary_wap` behind `FEATURE_BEACON_CHANNEL`:
  `init()` beside `chirp_channel::init()`, `update()` in the loop,
  `dispatch_espnow_message()` beside the chirp forward in
  `mesh_network.cpp`, the alarm/state callbacks routed to MQTT
  `beacon.state` and `PATTERN_BEACON` audio, and `set_enabled()` behind an
  explicit, persisted user opt-in — it has no surface today, and enabling
  at boot would switch on a life-safety broadcast (plus its daily
  fingerprint self-test broadcast) with no user choice. Prerequisite: a
  COSIGN_REQ frame is 310 B and the shared ESP-NOW receive path drops
  anything over 250 B, so either shrink
  `BeaconCosignRequestPayload.ciphertext[160]` to the 72 B canonical (a wire
  change: a 222 B frame, and the size pin in
  `test_beacon_cancel_origination.cpp` moves) or widen `mesh_network`'s
  buffer. The handler-budget part already landed with F13. Two policy calls
  ride with it: whether a CANCEL is exempt from the originator's 5/24 h
  bucket (spec §8; today an originator on its fifth origination cannot
  cancel its own alarm), and what a two-device CANCEL does when the chosen
  cosigner silenced the alarm locally or missed the ALERT (today it refuses
  silently, the deterministic pick keeps choosing it, and each retry spends
  a bucket slot).
  *Found by F201 (#1762):* wiring `beacon_channel::init()` makes its
  `load_beacon_set()` and `load_audit_log()` open `beacon` read-only before
  anything writes it, on a flash-encrypted board's first boot after an NVS
  erase; route both through `csi_module_settings_nvs::begin_read_only()` in
  the same change. `ensure_x25519_keypair()`'s read cannot come first: every
  caller needs a beacon-set entry, which only `load_beacon_set()` fills from
  NVS.

- [ ] **F32 [decision+human] CAP-gateway attestation milestone.** Deferred
  by decision in F13; `spec/beacon_cap_gateway_v0.md` §6 sets the gates in
  order: the trust-root decision (§5 question 1), a separately named
  firmware build, a per-deployment legal review with an operator identity,
  the gateway pairing UX and §2.2 cap (with F30), then code. Until then a
  gateway-trust entry is a plain cosigner, pinned by
  `test_gateway_trust_confers_no_privilege` and
  `test_source_grants_gateway_trust_nothing`.

- [ ] **F412 [code+decision] canary-wap Chirp: a chirp's `ttl_minutes` is
  signed and carried, but nothing holds it to a range or honors it.** Spec
  §3.3's CDDL says it is "how long to display (5-60)". `POST /api/chirp` takes
  any value (`input["ttl_minutes"] | 15` into a `uint8_t`), and no receiver
  reads it: every row lives `DEFAULT_DISPLAY_MS` (30 minutes). Since F306
  (#1916) a relay carries the byte as its origin signed it, nothing more, and
  `test_chirp_security.cpp`'s `c4_a_relay_keeps_the_signed_ttl` pins today's
  receive-side lifetime, so a change that honors the TTL moves that case on
  purpose. Decide whether to refuse a TTL outside 5-60 at send, honor it at
  receive, or both. From code. Found by F306 (#1916).

- [ ] **F435 [code+decision] canary-wap Chirp: the replay filter and the
  recent table key on the nonce alone, so a signed frame can pre-empt a heard
  chirp.** Since F309 (#1920) only a frame whose signatures verify puts its
  nonce in the filter, but the filter and the recent table hold nonces, not
  signers. An attacker who hears a chirp can sign its own hop-0 witness with
  that nonce under a fresh session key, or send a hop-1 relay that names the
  origin's key and carries no origin signature (soft-accepted on the relayer's
  signature: `handle_witness()`'s v0.2 limitation; an envelope with no key is
  refused). Either is stored first wherever it arrives first, the relay as the
  origin's chirp with any template, and the real chirp is refused as a repeat.
  `test_chirp_security.cpp`'s `c9_a_forged_nonce_does_not_block_the_chirp`
  pins both as known limits; spec §5.3, the Replay cell and the
  Spoofed-origination cell say so. Closing it needs a decision: key the filter
  and the recent table on (nonce, origin key), persist the origin signature
  across relays and refuse a relay without one (the threat model's Outstanding
  item), or both. From code and the host suite. Found by F309's review
  (#1920).

- [ ] **F436 [code] canary-wap Chirp: a stale verified witness counts against
  its origin key's hour once per filter period.** `handle_witness()` runs
  `pubkey_rate_check_and_record()` before the freshness window, so a captured,
  validly signed but stale frame counts against its origin key's 4-an-hour
  budget before it is refused, once per filter period (331 to 662 s): four
  plays 11.1 minutes apart use up the key's hour. Pre-existing; written in
  spec §5.3 by D19 and pinned by
  `c9_a_stale_frame_counts_once_a_filter_period`, which becomes the failing
  test once the order moves. Move the freshness check ahead of the rate check;
  `c9_a_refused_frame_counts_once`'s counts then shift by one. From code.
  Found by F309's review (#1920).

- [ ] **F445 [code+decision] Three canary-wap pairing lines still reach `GET
  /api/logs` cut at 79 bytes.** `HealthLogRingEntry::message` is 80 bytes and
  `health_log()` forwards no detail, so three pre-existing pairing lines in
  `mesh_network.cpp` logged through `health_log()` reach `GET /api/logs`, the
  dashboard and the BLE log cut at 79 bytes, on a character; Serial and
  `/HEALTH` carry them whole. F359's line (#1920) fits (message 78 bytes,
  detail 42) by splitting what this side knows from where to look; splitting
  each of the three into message + detail through `log_health()` changes their
  words, a wording call, one line each. From code. Found by F359's review
  (#1920).

### Network surface & provisioning

- [ ] **F15 [code] No TLS on the canary.local HTTP/peek surface.** The one
  gap `firmware/PARITY_PLAN.md` lists as shared by *both* trees (❌ / ❌ at
  the sweep; the dashboard reads ⚠️ / ⚠️ since #1691). Landed for the
  canary tree's dev builds in #1704 (option (b) — maintainer to confirm):
  self-signed ECDSA P-256 HTTPS on 443 serving the whole route table, the
  certificate generated on the device once and kept under the WAP's NVS
  keys, plus a port-80 server that keeps the six connectivity probes and
  307-redirects the rest (307, not the WAP's 301: a cached permanent
  redirect would strand a factory-reset device's plain-HTTP setup). The
  decisions and the redirect builder live in the pure, host-tested
  `common/network/tls_policy.h`; TLS is skipped during first-boot setup, the
  peek stream runs in-handler over TLS, and `/api/status` reports
  `tls_enabled` / `tls_cert_fp` / `tls_mode_reason` (a core missing a
  capability builds HTTP-only and says why). `FEATURE_HTTPS=1` is on in
  `[env:dev]` (inherited by dev_ha, usb-onboard and full, and restated in
  full) and off in release and the board envs; the compile is CI's (the
  dev and full legs; `dev_ha` inherits the flag but no workflow builds it,
  `firmware/flavors.json` `build_envs`), and FEATURES.md reads ⚠️ for
  canary (PIO).
  Still open: turning it on in release/release_ha, the maintainer's call
  once the size-guard delta is read (option (c), flasher-provisioned
  certificates, is the fallback if the 2.0.17 core lacks x509write);
  canary-wap, whose dashboard cell reads ⚠️ since #1691: a runtime opt-in,
  not the build's posture — compile-gated on `esp_https_server.h`, served
  after setup once the on-device certificate loads, and plain HTTP during
  setup and on a start failure, logged (`firmware/FEATURES.md`, the
  canary-wap HTTPS note; PARITY_PLAN's shared TLS line); and the bench, U1
  runbook Track D, D1–D5.
  *mDNS TLS advertisement done (#1757):* the canary's `_securacv._tcp`
  record now carries a `tls` TXT ("1" when the HTTPS server actually came
  up, "0" otherwise — its absence means firmware predating this, not plain
  by choice) and a `secure_port` TXT when live, so a discovery client (the
  Lab, the Flasher) can tell HTTPS is on without probing. The decision is
  the pure, host-tested `tls_policy::mdns_tls_advert`; `begin()` announces
  before the server exists (tls=0) and `startHttpServer()` re-announces once
  TLS is up, which the STA_GOT_IP re-announce then carries to the home-WiFi
  interface. The plain `http`/`securacv` services keep advertising 80 (it
  307-redirects) for a client that cannot do TLS. Host-tested
  (`test_tls_policy`), canary `[env:full]` compiles. Not bench-verified (U1).
  *Correction (2026-10-19, F337/F344):* the on-device certificate has never
  been generated on either tree. Both generators are gated on
  `__has_include("mbedtls/x509write_crt.h")` (`canary_wap.ino`:88;
  `securacv_network.cpp`:110 through `SECURACV_HAS_TLS_CERTGEN`), a header
  mbedTLS has never shipped: its write API is declared in
  `mbedtls/x509_crt.h`, behind `MBEDTLS_X509_CRT_WRITE_C`. Checked on
  arduino-esp32 2.0.17 and on PlatformIO's framework-arduinoespressif32
  3.3.8 and its libs (both ship `esp_config.h` defining
  `MBEDTLS_X509_CRT_WRITE_C`, and declare `mbedtls_x509write_crt_der` in
  `x509_crt.h`; neither has `x509write_crt.h`). The only writer of the stored certificate
  (`tls_store_to_nvs()` on the WAP, `tls_store_der_to_nvs()` on the
  flagship) is called only inside that block, and no in-tree tool writes
  those NVS keys. So no stock unit of either tree stores or serves a
  certificate or hands out a `tls_cert_fp` pin: the WAP serves plain HTTP
  after setup, and the flagship's `FEATURE_HTTPS` builds serve HTTP only and
  say so in `tls_mode_reason` ("no stored certificate and this core cannot
  generate one"). "Landed for the canary tree's dev builds in #1704" holds
  for the compile, not for a device. The fix is to test `mbedtls/x509_crt.h`
  and `MBEDTLS_X509_CRT_WRITE_C` (or take option (c)). It turns HTTPS on for
  every such unit, changes the pin the apps trust and the mDNS `tls` advert,
  and needs Track D, so it is not done here. F337's cookie name and F344's
  Host rule are written for both transports. When it is done, the HTTPS
  mint can also clear a `Partitioned` `__Host-cv_session` that a server at
  the device's name planted in front of the device's own (THREAT_MODEL's
  HTTP row; F344's ninth review): send
  `__Host-cv_session=x; Secure; Path=/; Partitioned; Max-Age=0` before the
  session's `Set-Cookie` in the same answer. In headless Chromium 141,
  against a stand-in server, that answer left only the new cookie. The
  deletion goes first so that a browser that ignores `Partitioned`, and so
  deletes the device's own cookie, then takes the new one (reasoned, not
  run).
  *Note (2026-11-04, #1919):* three things to settle with this item, from F404
  and its review: what a lone stored TLS entry gets (F420); the flagship's
  `tls_load_der_from_nvs()`, which still takes any failed read as no
  certificate and would store over a pinned one once its generator builds
  (F421); and the WAP's `tls_generate_self_signed_cert()` (inside `#if
  SECURACV_HAS_TLS_CERTGEN`), which writes the RSA private key's DER into a
  2048-byte stack buffer (`key_buf`), copies it out and never zeroes it, on
  success or failure, and whose failure paths free `g_tls_cert_der` directly
  instead of through `tls_free_buffers()`, leaving `g_tls_cert_der_len`
  nonzero. It compiles out on every pinned core, so nothing runs it; when this
  item turns the generator on, zero `key_buf` on every exit, route the failure
  frees through `tls_free_buffers()`, and add a host test over the real
  mbedTLS calls.

- [ ] **F62 [code+decision] Consume the mDNS TLS advert in the desktop
  clients.** Found by the #1757 review: the canary now advertises `tls` /
  `secure_port` over mDNS (F15), but no in-repo discovery client reads them
  — the Flasher's `desktop/src-tauri/src/fleet.rs` serializes neither into
  `FleetSighting`, and `desktop/src/app.js` still builds an `http://` URL
  from the port-80 SRV record (which 307-redirects to a self-signed 443 the
  default reqwest trust policy then rejects). Closing the loop needs: the two
  fields carried through `FleetSighting` in BOTH the Flasher and the Lab's
  twin (`desktop-lab/src-tauri/src/fleet.rs`, held equal by
  `desktop_parity.test.js`); the frontend transport decision to prefer
  `https://<secure_port>` when `tls="1"`; and a client TLS-trust model for
  the self-signed cert — a TOFU pin store or an explicit accept-with-pin
  flow (security-sensitive, a maintainer decision). Pre-existing gap, not a
  regression; deferred from #1757 to keep that PR to the advertisement half.

- [ ] **F57 [decision] canary-wap's receipt route and bearer-gated API do not
  refuse a foreign Host.** F56 gave the canary's receipt the Host-first order
  its `auth_gate` routes already had; on the WAP, neither the receipt nor the
  bearer check (`api_auth.h`) reads the Host. `handle_provisioning_receipt`
  (`canary_wap.ino`) serves on a bearer (`api_auth_check_optional`) or takes
  the BOOT tap (`provisioning_gate_take()`) without reading the Host, and
  nothing under `firmware/projects/canary-wap/` includes
  `firmware/common/network/host_guard.h`. #1722 left the WAP out, recording
  that a port needs a decision on the captive portal. The follow-up proposed
  with F56's fix has the canary's shape: stage `host_guard.h`, refuse a
  foreign Host first with the SoftAP exempt, and extend the WAP's own
  `check_route_security.py`. That decision comes first. The canary exempts
  every request from its own AP subnet (`host_is_foreign`'s `from_ap_subnet`),
  but in standalone mode the WAP's AP is the product (`canary_wap.ino` says so
  where it restarts the captive DNS), so the same exemption would exempt every
  client it has. And the WAP runs its captive DNS for the whole life of the
  AP, pointing every name at itself, so captive-portal assistants arrive under
  foreign Host names; its `/` redirect and `request_host_is_direct` (the
  wizard's `/api/wifi/pair-token`) already tell those apart from the owner's
  browser by `canary.local` / `192.168.4.1`. Decide which interface and which
  names the WAP trusts, then port it with its route checker. Found in the
  wave-7 receipt review (F56).
  *Note (2026-10-21, F344):* F344 answers this item's question for the
  pages and the receipt: the WAP stages `host_guard.h`, takes no SoftAP or
  interface exemption (its captive DNS sends every name to the device, and
  the wizard's two names pass), asks the Host first on the receipt in every
  mode, and after setup on the landing, the `?cv_pair=` exchange, `/admin`,
  `/settings` and `/companion`; the setup branch's names are now exact
  matches, not substrings. F344 rejected a guard on every gate as defense
  in depth only, on the premise that a rebinding page holds no cookie for
  its own name; its third review found that premise false (any host on the
  LAN takes a session at the landing, and a page can set that value for its
  own name with `document.cookie`), so the cookie validator behind every
  cookie gate (`session_validate_cookie()`) now asks the Host too. What
  this item still leaves open is the bearer: outside the receipt, the
  bearer-gated API routes read no Host *(amended 2026-10-26, #1843's sweep:
  those routes are behind `api_auth_check()`, which takes a landing
  session's cookie before the bearer, F346; the cookie path asks the Host,
  the bearer path does not)* *(amended 2026-10-27, the owner-authority PR,
  #1887: not all of them; the routes `api_auth.h` names, the pairing QR,
  the vault key, the two pairing confirms, Disconnect and Forget in every
  mode, Connect and AP-only after setup, and the two reboot routes, take
  `api_auth_check_bearer()`, the bearer alone, which asks no Host
  either)*, so a rebinding page that holds the
  bearer token itself is not refused (and a pair token's value carried off
  the LAN works on `/api/wifi/connect` with no Host question for its ten
  minutes, until F334 removes that road after setup). Left open for the
  maintainer to close or narrow.
  *Note (2026-10-21, F344's fifth review):* two corrections to the note
  above, and one thing no Host question closes. A carried pair token works
  with no Host on `/api/wifi/ap-only` and `/api/wifi/qr-scan` (start, status
  and stop) as well as `/api/wifi/connect`. F334, still open, asks whether
  Wi-Fi changes after setup take a bearer (its entry names connect and
  ap-only, not the scan) *(amended 2026-10-27, the owner-authority PR,
  #1887: F334 is done; after setup a carried pair token no longer works on
  connect, ap-only or the scan's start, since connect and ap-only take the
  bearer alone and the start answers `404`)*; the scan's status and stop
  take the pair token
  through `qr_scan_request_allowed()`, and `qr_auto_scan_tick()` runs
  whenever no Wi-Fi is configured, so a standalone unit with a camera keeps
  scanning after setup and a carried token reads that scan's status and
  stops it *(amended the same day, same PR: since F333 the boot scan runs
  only while no network is stored and the unit is not standalone,
  `keyless_join_open()`, so a standalone unit does not scan, and after
  setup a carried token reads and stops a boot scan only on a unit with no
  network stored that is not standalone)*. And
  `host_guard.h` accepts a name because public DNS cannot mint it, which is
  not the same as only this device answering to it: every WAP claims
  `canary.local` over mDNS, and the SoftAP's captive DNS answers every name
  with the device. So a page another network served under one of those
  names, or at the device's own LAN address (an IP literal passes), on a
  hostile hotspot or with mDNS spoofed on a shared Wi-Fi, left open or kept
  in the browser's HTTP cache until the browser comes home or joins the
  SoftAP, is same-origin with the device and passes every Host check: it
  can take the landing's pair token, a session through `?cv_pair=`, during
  setup the wizard's pair token (from `/`'s redirect to the wizard or from
  the re-issue), and, around a BOOT tap pressed from that browser, a
  receipt it can read. The flagship and the display share
  the rule. Only HTTPS with an identity the browser can check closes it,
  and neither F15 nor F256 gives a browser one: F15 would bring HTTPS with a
  self-signed certificate, whose warning the owner accepts, and only a
  client that pins the certificate checks it (the iPhone app pins the
  receipt's `tls_cert_fp`; F256 asks whether it may learn one over mDNS).
  F15 would still help: once a WAP serves HTTPS after setup, port 80
  answers only the captive probes, `/api/fleet` and a redirect, so a page
  planted over plain HTTP is cross-origin to the device and takes none of
  these (it reads nothing; but `SameSite=Strict` compares sites, so in a
  browser that does not count the scheme in the site, as Chromium does, its
  requests still carry the session cookie and it can write blind with the
  owner's session: a reboot, or any other write a session may make
  *(amended 2026-10-27, the owner-authority PR, #1887: since F361 the two
  reboot routes take the bearer alone, and of a session's writes only the
  firmware install restarts the unit, and only into a newer signed
  image)*); a page
  served over https at that name whose certificate warning
  the owner accepted still does, and so does any page during the first-boot
  setup or on a unit whose HTTPS start failed, which serve plain HTTP.
  From code; the cache half seen in headless Chromium against
  a stand-in server (F344's sixth review), not against a device. No new
  number.

- [ ] **F58 [decision] The canary's identity-key store still reports success
  whatever the write did.** `nvs_store_key()`
  (`firmware/canary/lib/securacv_crypto/src/securacv_crypto.cpp`) ignores
  `putBytes`' result; F55 left it alone on purpose. Its caller,
  `witness_provision_device()`, stops provisioning on false ("provisioning
  cannot continue"), so an honest result there halts a device on a full NVS
  instead of booting it with a key the next boot will not find (a new device
  id and a broken Home Assistant pin). Maintainer to choose: halt, or boot
  with a loud, counted ephemeral identity. Found doing F55.

- [ ] **F251 [code+human] The flagship `firmware/canary` has no Bluetooth
  setup door.** Magic pairing (the Improv-over-BLE door,
  `docs/design/magic_pairing.md`) is built on Sense, Vision and the WAP's FULL
  profile. Six files promise the flagship's door "in a follow-up"
  (`magic_pairing.md`'s header, §5 table and flagship paragraph,
  `docs/GLOSSARY.md`'s "Bluetooth setup door", `docs/ble_protocol.md`
  §13.7, `docs/design/onboarding_shared_module.md`, `firmware/FEATURES.md`'s
  door row, `firmware/common/network/claim_ticket.h`'s header; all but
  `claim_ticket.h` now link here), and `firmware/canary` has no Improv code
  (`git grep -il improv -- firmware/canary` is empty). The iPhone already maps
  a `Canary-XXXX` advert to the WAP family (`ios/Shared/NearbyCanary.swift`:
  "the flagship build pairs like a WAP"), but `ios/Shared/SetupGuide.swift`'s
  `CanaryFamily` has no flagship case. Work: link
  `common/network/improv_core.h`, `improv_ble.{h,cpp}` and
  `provisioning_gate.h` (and `claim_ticket.h`, if it pairs like a WAP as the
  iOS parser assumes) into `firmware/canary/platformio.ini` under
  `FEATURE_IMPROV`, started after the NimBLE stack owner
  `ble_status_stack_begin()`; decide which envs carry it (today only
  `[env:full]` links NimBLE); advertise the `Canary-XXXX` name; decide whether
  the walkthrough gets a flagship family with `hasBluetoothDoor` (and a
  `SetupGuideTests` row) or the WAP page fits; fill the flagship column of
  `magic_pairing.md` §5 and add its rows to §9. The design ties it to the
  flagship's portal migration (`onboarding_shared_module.md`, Phase 4 item 2).
  Bench (U1): the §9 rows on a XIAO ESP32-S3. Found by the 2026-10 cleanup
  audit.

- [ ] **F254 [human] After magic pairing, a Sense or Vision still needs
  its hub (MQTT) settings from a flasher.** The Bluetooth door hands over
  Wi-Fi only: Improv carries an SSID and a password, and Sense and Vision
  answer an empty URL and appear on the LAN (`magic_pairing.md` §5, "After the
  join"). Their broker host, port and login are NVS keys (`mqtt_host` and its
  neighbors) that the flashers' broker block writes, so a phone-only setup
  ends with a device on Wi-Fi that reports to no hub. The 2026-10 cleanup made
  the iPhone say so (`SetupGuide.swift`'s `hubNote`; the walkthrough copies
  the hub login with a two-minute clipboard expiry and points at the Flasher's
  MQTT fields) and did not close it. Work: a companion GATT characteristic
  beside Improv on Sense and Vision that takes the broker host, port and login
  right after a proven join, only while the door is open and only on an
  encrypted link, with the Wi-Fi write's caps and refusals; the phone's
  `MagicPairPlan` sequencing; host tests on both ends; and its wire in
  `magic_pairing.md` §3 and rows in §9. Bench (U1). Found by the 2026-10
  cleanup's iOS package.
  *Done, the [code] half (#1804, 2026-10-16, per GitHub's history; this
  clone's root is #1851's squash), the rest retagged [human] (#1920):*
  `firmware/common/network/hub_handoff.h` (`test_hub_handoff.cpp`);
  `improv_ble.cpp`'s HUB characteristic (`WRITE_ENC` plus the handler's
  `isEncrypted()` / `sec_state.encrypted` check, taken while the session is
  Provisioned or Provisioning); `store_hub` in each `main.cpp` through
  `set_mqtt_credentials`; on the phone `ImprovWire.hubHandoff` /
  `HubHandoffStatus` (`ImprovWireTests`), `ImprovClient.swift` and
  `NearbySetupLogic.hubEnding` (`NearbyCanaryCardLogicTests`). #1920 adds six
  unrun `magic_pairing.md` §9 rows: stored and in Home Assistant, the status
  byte read back, an unencrypted write refused, no join this session refused,
  refused documents (each with a valid host, so the host is not the fault),
  and the hand-over onto a new broker by either of `store_hub`'s two paths (a
  restart or an in-place re-point); the record's wipe is host-tested, the
  mailbox's and the attribute's are not. §3.6 now names the join as the gate,
  not the shut door: `handle_hub()` never reads `door_open_now()`, so a write
  after the door closes is taken, and "no later client reaches HUB" holds for
  a Provisioned session's link only, since mid-join a dropped phone can be
  followed inside the join's 30 s by a client that inherits the session
  (F447). `SetupView.swift`'s comment and its hub-login footer (a string, past
  the brief's comment-only scope; nothing pins it) no longer say the phone
  cannot hand the hub over; the Swift compile is CI's. Remaining: U1 runs the
  §9 rows on a Sense and a Vision.

- [ ] **F256 [code+decision] canary-wap does not advertise its TLS certificate
  fingerprint over mDNS.** Its `_securacv._tcp` TXT set (`MDNS.addServiceTxt`
  in `canary_wap.ino`) carries `device_id`, `fw`, `host`, `name`, `model` and
  the broker keys, not the fingerprint it already computes
  (`g_tls_cert_fp_hex`, served in the receipt as `tls_cert_fp`). So the iPhone
  has no way to pin an https WAP it found on the LAN before it holds a
  receipt, and since the 2026-10 cleanup the app deliberately offers no "tap
  BOOT, then Continue" pairing over the LAN and no copy promises one. Adding a
  TXT key (`tls_fp`, say) is a decision first: an mDNS record is
  unauthenticated, so a fingerprint learned from it is trust on first use, not
  a pin. Decide whether that is acceptable for a pairing the BOOT tap already
  gates; then add the key, the app's LAN BOOT-tap path and their tests.
  Related: F62, the same trust question for the flagship's `tls`/`secure_port`
  advert in the desktop clients. Found by the 2026-10 cleanup's iOS package.
  Note (2026-10-19, F337): `g_tls_cert_fp_hex` stays empty on every stock
  WAP, and the receipt, the pairing QR and the door's claim send no
  `tls_cert_fp` pin (`""` in the receipt and the claim), because no
  certificate is stored, so `g_tls_enabled` and `tls_after_setup()` are
  false. So there is no fingerprint to advertise until F15's certgen works.
  This item waits on F15.

- [ ] **F351 [decision] The flagship's request limit is one per device.**
  `rate_limit_check()` (`firmware/canary/lib/securacv_network/src/securacv_network.cpp`)
  counts every request in one 60 s window for the whole device: 120 requests
  and 30 actions (`canary_config.h`'s `RATE_LIMIT_MAX_REQUESTS` and
  `RATE_LIMIT_MAX_ACTIONS`). It runs on 57 handlers, before `auth_gate()` on
  the 56 that have one. One dashboard tab on the Status panel spends 60 a minute (`securacv_webui.cpp`
  polls `/api/status` and `/api/sensing` every 2 s), so a second tab, or any
  host on the network sending two requests a second, holds every
  rate-limited route at `429 rate_limited` for everyone, the owner's app
  included. The provisioning receipt has no rate limit. The per-address
  table F343 adds for bearer strikes (`firmware/common/network/auth_backoff.h`)
  is the shape a per-source budget could take. Decide whether the budget
  stays per device, becomes per source, or both. From code; not probed.
  Found by the F340 decision's security review, 2026-10-19.

- [ ] **F352 [code] iOS: a pairing for a re-installed Canary keeps polling
  with its old token.** An install or a full erase in either flasher, whose
  factory image starts NVS over and so regenerates the WAP's key (an OTA
  update and the 10 s hold keep the key and the token), changes both its
  token and its `device_id`. *(Amended 2026-10-21, #1843: this item had said
  a factory reset, which keeps both. Amended again 2026-10-22, #1843's sweep,
  third pass: that amendment named a full erase as the only cause and said
  an ordinary re-flash keeps the key, but both flashers write a factory image
  that spans offset 0 to the app's end with the settings region blank
  (`make_factory.py`; `browser_flasher.md`, "A factory flash re-initializes
  NVS"), so an install without the erase changes them too.)* `DeviceStore.add`
  (`ios/Sources/SecuraCV/Security/DeviceStore.swift`) dedupes by id, so the
  old row stays beside the new one, and `FleetStore`'s 20 s refresh loop
  (`ios/Sources/SecuraCV/App/FleetStore.swift`, `startRefreshLoop()`) keeps
  sending the old token to `/api/status`. Since F343 that holds only the
  phone's own address, but that is the address the new pairing polls from,
  so the new row is held at `429` intermittently; a refused `wapStatus()`
  falls back to the reference API's `info()`, and a row that answers
  neither is marked `.lost` (the same file). The fix is to stop
  polling a row that answers `403` and offer re-pair or remove. From code;
  not run. Found by the F343 decision's owner review, 2026-10-19.

- [ ] **F353 [decision] canary-wap: the bearer token cannot be rotated.**
  `derive_api_token()` derives the token from the identity key, and the 10 s
  BOOT hold keeps both (`factory_reset_by_hold()`), so the token a unit was
  set up with is its token for life. After F334, F345, F346 and F361 it is the
  only post-setup grant of a new bond, a vault key or a reboot, and the only
  one of a network move over HTTP (a session can still remove a bond or the
  vault key, rejoin the stored network, and start the firmware install,
  which restarts the unit only into a newer signed image; and a peer already
  bonded through the pairing confirm or a passkey, one a session made
  before F346 among them, still moves the network with no token, over the
  Bluetooth credentials write that
  `ble_provision.cpp` hands to `ble_request_wifi_provisioning()`), and the
  setup door's winner after the owner's own Forget or hold keeps it
  (magic_pairing.md §3.7, §8). Options: a
  rotation counter in the derivation (the iPhone and Home Assistant pairings
  then run again), or keep, documented. Whoever implements it must make
  `nvs_load_token()` answer Loaded, Absent or Refused, as F360 did for the key
  (reuse `stored_key_length()`'s IDF probe shape), and close F365 in the same
  PR, with a test that a rotated token survives a failed read. Filed by the
  owner-authority PR (#1887), allocated by the network-authority decisions
  (2026-10-20).
  *Note (2026-11-04, #1916):* F365 is done: `nvs_load_token()` answers Loaded,
  Absent or Refused and provisioning stores a token only on Absent. What a
  rotation still owes is its own test that a rotated token survives a failed
  read.

- [ ] **F356 [code+decision] The shared Host rule refuses router names some
  households use.** `firmware/common/network/host_guard.h` accepts an IP
  literal, a single label, and `.local`, `.lan`, `.internal` and
  `.home.arpa` names, and refuses everything else: a name with one trailing
  dot (`canary-7916.local.`), the router suffixes `.home` (a TLD ICANN has not
  delegated) and `.localdomain` (no TLD at all), and FRITZ!Box's `.fritz.box`.
  Since F344 the WAP answers those with its fixed `403` page after setup and
  its receipt refuses them; the flagship serves its page under them without
  the token and refuses its API, and the display refuses its writes and its
  CSRF token. Decide whether to widen the canonical header to accept a
  single trailing dot and `.home` and `.localdomain`, and decide on
  `.fritz.box`, for all three products, with rows in
  `firmware/tests_host/test_host_guard.cpp` and the staged copies cp'd. From
  code; not probed. Filed by F344, 2026-10-21.

- [ ] **F357 [decision] canary-wap: any web page can read `/api/wifi`,
  `/api/wifi/scan` and `/api/fleet` at the device's address, and a
  DNS-rebinding page can read the other public routes.** `http_send_json()`
  (`canary_wap.ino`) sends `Access-Control-Allow-Origin: *` on every JSON
  answer, and three routes on the route check's public allowlist answer
  through it with no credential: `GET /api/wifi` (the home network's SSID,
  the station IP, the SoftAP's name and its client count), `GET
  /api/wifi/scan` (nearby networks; a request can also start a scan, which
  stalls the SoftAP) and `GET /api/fleet` (coarse presence, whose wildcard
  is a documented contract: `tvos/discovery/DISCOVERY.md` says a single
  Canary is readable from any page on the LAN). A fourth public route, the
  wizard token's re-issue (`GET /api/wifi/pair-token`, setup only, the
  exact setup names), answered through it too, and its body is a pair
  token that authorizes `/api/wifi/connect` and `/api/wifi/ap-only`; F344's
  second review closed that one (it now sends no CORS header, and
  `check_route_security.py` refuses a mint site that answers through
  `http_send_json()`). So any page a household
  member opens over plain `http` can `fetch('http://<the WAP's IP>/api/wifi')`
  and read the answer, with no rebinding: a simple GET needs no preflight,
  Firefox and Safari do not gate a public page's requests to a private
  address, and current Chrome asks the user for local-network access first.
  The Host question cannot close that read, because the IP literal and the
  `.local` name both name the device. The other public routes send no CORS
  header on their answers (`/api/selftest`, `/api/device-info`,
  `/api/help-qr`, `/enroll`, `/api/device/enroll`, `/admin`, `/settings`,
  `/companion` and their assets, `/sense`'s redirect, the captive probes;
  `/api/device-info`'s `500` and `503` go through `http_send_error()` and so
  carry the wildcard, with an error code only), so only a DNS-rebinding
  page reads them; since F344 the pages among them refuse a foreign Host
  after setup, and the API routes do not ask. Decide: (1) drop the wildcard
  from `http_send_json()` and keep it only where a contract needs it
  (`/api/fleet` and its `OPTIONS`; the wizard and the dashboard read
  `/api/wifi` and `/api/wifi/scan` same-origin, and the apps send no
  `Origin`), or answer a cross-site `Origin` on `/api/fleet` with less, as
  the display does (`glass_web.cpp`); and (2) whether the public API routes
  ask the Host question too, which closes the rebinding read (the wizard
  reaches them at `canary.local` or `192.168.4.1`, which pass). The routes
  behind a credential that also send the wildcard leak nothing to another
  site: a cross-site fetch carries no `SameSite=Strict` cookie and no
  bearer, and a browser refuses a credentialed answer under `*`. The
  exposure predates F344. From code; not run in a browser. Filed by F344,
  2026-10-21; restated the same day by its review, which found the
  wildcard: the first text said only a rebinding page could read these
  routes, and offered the Host question as the remedy.
  *Note (2026-11-04, #1919):* F355 met this item from the pairing side. Since
  F355 the Canary Vision SPA and its docs say a WAP pairs in the iPhone app
  and over MQTT, not from the SPA, but two things still send an owner there.
  The WAP dashboard's only pairing card (`web_ui.h`'s "Pair with the Canary
  Vision app": "In the app: Add Canary → Other ways to pair → Scan pairing
  QR", and Show QR for `/api/pairing-qr`; a comment in `canary_wap.ino` and
  `docs/canary_settings_access_validation.md` say the same) offers a QR the
  SPA cannot complete, since the SPA then calls `GET /api/v1/info` with
  `X-Canary-Token`, which the WAP neither serves nor reads, and the iPhone
  app's scanner reads only a display's `WIFI:` QR. And a canary-wap peer in
  the SPA's "Discovered on your network" is still offered "Pair this Canary"
  into the BOOT-tap step, which ends "The Canary didn't answer" against WAP
  firmware (seen in F355's one-off headless Chromium probe, which nothing in
  the tree reruns). Either this decision makes the WAP
  SPA-pairable (an `Access-Control-Allow-Origin` on the receipt, the SPA's
  token header, `/api/v1/info`), or the card says where a WAP pairs (a
  firmware edit, then `gen_web_assets_gz.py`) and the SPA sends a WAP peer to
  the iPhone app.

- [ ] **F363 [code+decision] The flagship's and the canary-wap's
  `nvs_load_bytes()` discard `getBytes()`' count, and a failed read reads as
  nothing stored, so a boot can overwrite the stored chain state; canary-sense,
  canary-vision and canary-sentinel read a failed chain read the same way.**
  Both trees have the helper: `securacv_crypto.cpp:552` and
  `canary_wap.ino:1790` each answer `false` for a failed read-only open or a
  length other than `len`, then call `getBytes()` and answer `true` whatever
  it returned. The chain load is the caller, in the same shape in both
  (`securacv_witness.cpp:280` / `:283`, `canary_wap.ino:11436` / `:11439`):
  the `{seq, head}` blob into an uninitialized stack array, then the legacy
  head into another. A read that fails after the length check leaves the
  array as the stack held it, or holding the bytes IDF copied from the
  stored entry when the data's CRC failed (IDF copies the data into the
  buffer before it checks it; when only the CRC field was hit, those are
  the stored bytes, and the load goes on as if the read had worked). For
  the blob, `chain_state::decode()`'s version byte and CRC-16 refuse almost
  any other bytes, so it reads as no blob; for the
  legacy head nothing checks, so when `chain_state::choose()` takes the
  legacy pair (no valid blob, or a legacy seq ahead of it) the chain
  continues from a head nothing checked: the stack's bytes, or the bytes
  IDF copied before the CRC refused them. And `false` means both "not
  there" and "could not read": a failed open or length
  lookup on both entries gives `Source::Genesis`, the seq from
  `nvs_load_u32()` (which answers its default on a failed read too) and the
  genesis head, and `if (genesis)` persists that over the stored blob
  (`securacv_witness.cpp:326`, `canary_wap.ino:11482`). The likeliest road
  there is on a unit that never had the legacy pair: a blob read that fails
  after its length check, whose bytes `decode()` refuses, with no legacy
  entry, reaches `Source::Genesis` the same way. The records that follow are
  signed under the stored key at seq numbers it already used, from a
  different head: a branch only the SD reconciliation
  (`witness_store::sd_wins()`) can undo, and only with a mounted card whose
  log is ahead. canary-sense's, canary-vision's and canary-sentinel's
  `load_or_start_chain()` (`src/witness.cpp:108`-`:128`, the same lines in
  all three projects) check the read's count, so they never adopt what a
  refused read left in the buffer, but they take any failed open, length
  lookup or read as no chain: the head starts again at genesis and
  `s_length` at 0, and the next record's `persist_chain()` (sense `:243`,
  vision `:236`, sentinel `:240`) writes them over the stored head and
  length. Chain integrity, not secrecy; medium, since the chain is what
  these devices exist to keep. The fix is the same three-way answer:
  `nvs_load_bytes()` checks the count, zeroes the buffer on a refusal and
  tells `Absent` (IDF's `ESP_ERR_NVS_NOT_FOUND`) from `Refused`, the three
  projects' chain load does the same, and a refused chain read never
  persists a genesis head over the stored one. What the boot does instead
  (halt as on a refused key, or run unpersisted until the chain reads) is
  the decision. Host cases over the chain glue and `test_chain_state.cpp`'s
  decision. Found by F360's review (#1840).

- [ ] **F364 [code] The canary-wap's plain SHA-256 helpers discard mbedTLS's
  return codes.** `sha256_raw()` and `sha256_domain()` (`canary_wap.ino:1580`,
  `:1589`) and `beacon_channel.cpp`'s `ecdh_session_key()` (`:318`),
  `compute_fingerprint()` (`:409`), `load_audit_log()` (`:711`) and
  `chain_audit_entry()` (`:932`) call `mbedtls_sha256_starts()`, `_update()`
  and `_finish()` and check none of them. So do `mesh_crypto.cpp`,
  `mesh_network.cpp`, `chirp_channel.cpp`, `familiar.cpp`,
  `vault_snapshot.cpp` and `securacv_ota.cpp`; `ble_ota.cpp` checks its
  three. Four callers drop one-shot `mbedtls_sha256()`'s answer:
  `usb_evidence_drive.cpp:91`, `vault_snapshot.cpp:109`,
  `device_pseudonym.h:57` (its own `sha256_raw()`, under `device_id_hex()`,
  the salted pseudonym in the device-info answer) and `wifi_presence.h:111`
  (`hash_mac()`, under `FEATURE_WIFI_PRESENCE`); `rf_presence.cpp:184`
  checks its own. That is every call in the sketch directory (`grep -rnE
  '\bmbedtls_sha256(_(starts|update|finish))?\s*\('` over its `.ino`, `.cpp`
  and `.h` files). `mesh_crypto.cpp`'s note that these calls return `void`
  "on arduino-esp32 2.0.x (this build)" is stale: the sketch pins core
  3.3.8, where they return `int`. `sha256_raw()`, `sha256_domain()` and
  the four `beacon_channel.cpp` helpers each hash on a stack context and
  allocate nothing themselves (`securacv_ota.cpp`'s image hash mallocs a
  4096-byte read buffer, which it checks and frees). On the ESP32-S3's
  port (IDF v5.5.1 and v5.5.4 `port/sha/core/esp_sha256.c`, the same file
  in both, and `sha.c`, whose two differences change no answer on either
  chip: an aligned copy for flash-encrypted external memory, which neither
  the S3's nor the C3's `soc_caps.h` enables, and the cache write-back of
  PSRAM buffers, which the S3 builds, moved from `esp_sha_dma()` into
  `esp_sha_dma_process()`, in both versions run before the DMA starts and
  its result ignored) a call answers 0 except on the DMA path. An update takes it when the 64-byte
  multiple it hashes in one call, after topping up any bytes already
  buffered, is more than `SHA_DMA_MODE_THRESHOLD` (`SHA_ALIGN_DOWN()` and
  `sha_operation_mode()`, `esp_sha_internal.h`): 256 on the S3, so 320
  bytes or more of whole blocks, and 128 on the ESP32-C3 the sketch's
  `XIAO_ESP32C3` compile leg builds for (`firmware.yml:553`), so 192 or
  more. There `esp_sha_dma()` answers -1 when the first DMA operation
  cannot claim the shared crypto GDMA channels (`esp_crypto_shared_gdma.c`,
  retried by every later one until it succeeds) or when it cannot allocate
  an internal copy of a buffer that is not DMA-capable. The update then
  returns without hashing those blocks, and `_finish()` still writes all 32
  bytes of a digest that skipped them: a wrong hash, not stack bytes. Of
  `canary_wap.ino`'s callers, a record payload that long
  (`create_witness_record()`, `:2836`) and the TLS certificate's
  fingerprint (`:2149`) can take that path; the key fingerprint, the chain
  link, the genesis head, the pseudonym and the Wi-Fi presence hash are
  short (the last two under 100 bytes) and run in block mode, which has no
  failing path. The OTA image hash (`securacv_ota.cpp`, 4096-byte updates)
  takes it too, and a wrong digest there fails the manifest compare, so the
  image is refused. A wrong payload hash is a record that fails
  verification, and a wrong certificate fingerprint fails a pin, so this is
  low severity: detectable, no secret, and it needs the GDMA claim or the
  allocation to fail. The fix: check each answer, zero the output and have
  callers refuse, as `hmac_sha256()` does since F359, and correct the stale
  note. A host case like `test_wap_hmac_fail.cpp`'s seam. Found by F359's
  review (#1840).
  *Partly done (#1916), the sketch's slice; the item stays open for the module
  files:* `canary_wap.ino`'s `sha256_raw()` and `sha256_domain()` are
  `[[nodiscard]] bool`, check `_starts()`, `_update()` and `_finish()`, free
  the context on every path and zero their 32-byte output on a failure, as
  `hmac_sha256()` does since F359. Each of their five callers refuses what its
  hash was for: a failed key fingerprint or genesis head fails provisioning, a
  failed chain link makes no link, a failed payload hash makes no record, and
  a failed certificate fingerprint leaves the certificate unpinned and HTTPS
  off. A generated TLS certificate is now hashed before it is stored. The
  review found the zeroed record reaching other surfaces, so a refused
  fingerprint or genesis head also keeps BLE discovery off the air and keeps
  `[OK] Boot attestation`, `loop()`'s table rows and the 60 s self-verify off
  it. `test_wap_sha_fail.cpp` (new, 1280 checks) runs the helpers and their
  callers, cut verbatim, over an mbedTLS stand-in that fails one named step
  and lets the later steps run, since mbedTLS does not promise that a step
  after a failed one fails, so a dropped update answer is caught. Host-tested
  under g++, clang++ 18 and ASan/UBSan; the ESP32 compiles are CI's; not
  bench-tested. Next slice: `beacon_channel.cpp`'s four helpers,
  `mesh_crypto.cpp` (and its stale arduino-esp32 2.0.x note),
  `mesh_network.cpp`, `chirp_channel.cpp`, `familiar.cpp`,
  `vault_snapshot.cpp`, `usb_evidence_drive.cpp`, `wifi_presence.h`, both
  `securacv_ota.cpp` copies, and the three `device_pseudonym.h` copies, which
  move the emulator dist. Two connection-bound reads that still report the
  unnamed identity on a refused boot are F403.
  *Partly done (#1919), slice 1a, the WAP-only modules; the item stays open
  for `beacon_channel.cpp`, `mesh_network.cpp`, `chirp_channel.cpp`,
  `securacv_ota.cpp` and `device_pseudonym.h`:* `vault_snapshot.cpp`'s seal
  digest fails the seal as `sha` (the tmp file is removed and nothing is
  chained), `set_pubkey_hex()` refuses, before NVS is written, a key whose id
  did not hash, and at boot `load_config_from()` adopts a stored key only when
  it read back and its id computed. `evidence_update_verify.h`/`.cpp` and
  `usb_evidence_drive.cpp` answer `sha_failed` before any compare. The
  familiar-device filters (`familiar.cpp`, `familiar.h`) learn, match and
  persist nothing on a failed hash, `wifi_presence.h` does not count a capture
  whose MAC did not hash, and `mesh_crypto::sha256_domain()` checks every step
  and zeroes its output on a failure, as the canary library's IDF 5 body does,
  its stale arduino-esp32 2.0.x note corrected. A new seam,
  `test_wap_sha_fail_modules.cpp`, runs the real code (whole files, linked
  files or verbatim cuts) over a SHA-256 stand-in that fails a named step
  once: 408 checks under g++, clang++ 18 `-Werror` and g++ ASan/UBSan,
  valgrind-clean; over the old sources it fails 189 of 408, and valgrind
  reports 126 uses of uninitialized values. `test_evidence_update_verify`
  gains the refused-hash case. The ESP32 compiles are CI's (Arduino-CLI core
  3.x, and PlatformIO's `canary-wap-default` and `canary-wap-usbdrive` envs,
  the second the only build of `usb_evidence_drive.cpp`'s real body); not run
  on a device. Still open in this slice's files: `sha256_domain()` stays
  `void`, because `ble_scan.cpp`, which calls it, is byte-identical with the
  canary tree's, whose API is `void`; so `hash_beacon_id()` and
  `mesh_pair_crypto.h`'s `confirmation_code()` and `confirm_hash()` take a
  zeroed digest as a real one (both sides of a failed pairing would show
  000000). Their 56-72-byte inputs run in the SHA engine's block mode, so this
  is hygiene, not a live path; making it `bool` edits
  `canary/lib/securacv_mesh`, `securacv_ble_scan` and their WAP copies
  together, and no sync script holds the two `mesh_crypto` copies equal. The
  next slices, as wave 22's selection sets them: `beacon_channel.cpp`'s four
  helpers, `mesh_network.cpp`'s `sha256_domain` and its sign and verify
  callers, and `chirp_channel.cpp`'s `session_id_from_pubkey`, whose inputs
  run in block mode except the beacon audit head on the C3 leg, next wave with
  whoever owns those files then (`mesh_network.cpp`'s part with F303 and
  F359); then the three `device_pseudonym.h` copies (they move the emulator
  dist) and the three `securacv_ota.cpp` copies, which need an IDF 4.4
  spelling (`mbedtls_sha256_*_ret`) only CI's core 2.0.17 legs compile, in the
  dist wave with the next version-bump rebuild (the OTA image hash already
  fails safe at the manifest compare). A refused vault key reads as a
  malformed key in its API answer (F426).
  *Note (2026-11-04, #1920):* none of the open slices landed this wave. Slice
  1b (`beacon_channel.cpp`'s four helpers, `mesh_network.cpp`'s
  `sha256_domain` and its sign and verify callers, `chirp_channel.cpp`'s
  `session_id_from_pubkey`) is buildable, but its files were the chirp and
  wap-says-what-happened packages' this wave and its proof file
  `test_wap_sha_fail_modules.cpp` the latter's (F426, which slice 1a filed,
  closed there: a refused key id answers in its own words), so it waits for
  wave 24 as one package; every input runs in the SHA engine's block mode on
  the S3 and the C3, so no failing path exists in practice meanwhile. Slice 2
  (the three `device_pseudonym.h` copies) moves the emulator dist, and the
  `securacv_ota.cpp` copies need the IDF 4.4 spelling only CI compiles: the
  dist wave with the next version bump.

- [ ] **F402 [decision] canary-wap: a stored token it cannot read or use keeps
  a release unit unprovisioned at every boot.** Since F365 (#1916) a stored
  token the WAP cannot read, or one that is not `cv_` and 32 letters or
  digits, fails provisioning (F336's posture), so a release build raises no
  SoftAP and no web server until the entry heals or the unit is erased. The
  CRC case heals by itself: IDF erases an entry whose data CRC fails in the
  read it refuses, so the next boot reads Absent and derives a token (one a
  flasher seeded is then replaced by the derived one). A refusal that persists
  does not heal. Decide whether it should halt, as a refused key does (F360),
  or derive a new token once a person confirms, the same kind of call as F58.
  Neither writer stores another shape today (`provisioning.rs` and
  `flash-core.js` refuse a token that is not `cv_` and base62), but
  `desktop/flash-engine/src/provisioning.rs:228-229`'s comment still says the
  WAP accepts `>= 35 bytes`; correct it with whichever answer is chosen. From
  code and the host suite. Found by F365 (#1916).

- [ ] **F409 [human] The flagship's one-asker tap has no bench row.**
  Since F354 (#1916) the flagship hands its BOOT tap to one asker and its
  dashboard banner asks before the press, but
  `docs/audit/hardware_verification_checklist.md`'s flagship lockout row
  (F340, step 5) still says to "short-tap BOOT and reload A's dashboard tab".
  Add a row and run it (U1): open the locked dashboard on A and wait for "Now
  short-tap BOOT"; tap, and the page unlocks without a reload, with one
  receipt; with B polling `/api/provisioning-receipt` across a tap, A and B
  both get `409 tap_contended` and nothing is handed out; and the same with
  A's dashboard in a background tab. Then reread step 5 of the F340 row,
  written before the banner asked first. Not bench-tested. Found by F354 (#1916).
  *Row landed (#1920), the rest retagged [human]:*
  `hardware_verification_checklist.md` has a flagship section (F354, F409)
  with the row: wait for "Now short-tap BOOT on the Canary..." on A's locked
  dashboard, tap, and the page unlocks with no reload and one receipt; with B
  polling `/api/provisioning-receipt` across a tap, both get `409
  tap_contended` and nothing is handed out; both again with the dashboard in a
  hidden tab. Step 5 of the F340 row waits for the banner's line and taps with
  no reload, and the page-load grant (`page_token_inject()`) keeps its own
  step. Unrun. Remaining: U1 runs it on a XIAO ESP32-S3.

- [ ] **F410 [decision] The flagship's `tap_contended` hint is the WAP's
  without its console clause.** The WAP's hint ends "or read the token from
  the USB serial console". The flagship's console never prints the token
  (`AuthManager::redact` exists and nothing calls it), so since F354 (#1916)
  its hint drops that clause; the keys, their order and the error value are
  the WAP's, and `test_securacv_auth.cpp` holds that relation against
  `canary_wap.ino`. If the apps must see one string from both devices, the
  other road is for the flagship to print the token on its console, a security
  decision. Keep the two hints different, or decide that. Found by F354
  (#1916).

- [ ] **F419 [decision] canary-wap: what a boot with a refused identity serves
  on the surfaces F403 does not gate.** Since F403 (#1919) a refused key
  fingerprint or genesis head keeps BLE discovery, the BLE status service, the
  witness HEAD and the signer off; records and the chain persist refuse on
  `chain_refused` (F364); and a release build raises no SoftAP
  (`provision_device()` fails, so `resolve_ap_password()` refuses and
  `wifi_init_provisioning()` returns before the SoftAP, the web server and the
  home-network join). On a release build these still read the refused
  identity: `bluetooth_channel::set_device_metadata()` and
  `ble_console::set_device_metadata()` (an empty fingerprint); the BLE Improv
  claim and name suffix; `setup()`'s unconditional
  `csi_mqtt::set_identity(g_device.device_id, ...)`, which the camera's boot
  QR join uses (`qr_auto_scan_tick()` checks `keyless_join_open()`, not the
  identity, and the scan saves the Wi-Fi credentials, joins and reinitializes
  MQTT, which then opens with topics and Home Assistant discovery keyed by the
  empty id and publishes unsigned); the `STA_GOT_IP` handler's mDNS TXT
  `device_id ""`; `mesh_network::init(..., g_device.device_id)`, which copies
  the empty name into Opera's pairing DISCOVER and ACCEPT frames; and
  `datamgmt::process()`'s hourly `backup_chain()`, which writes the refused
  head under an HMAC from the key to `/CHAIN/backup.bin` over the last backup
  (DEV and FULL with an SD card only; nothing on the WAP calls
  `restore_chain()`, and under a persistent SHA fault the HMAC fails and
  nothing is written). A debug build, which raises a dev-password SoftAP, adds
  `/api/status`, `/api/chain`, `/api/device-info`, `/api/export`, the witness
  v1 route, `/api/fleet` and its scan, the pairing QR, the provisioning
  receipt, `mdns_reannounce()` and `csi_mqtt::init()`; `/enroll` and
  `/api/device/enroll` answer 200 with an empty `device_id`, `pubkey_hex` and
  `fingerprint_hex`, because `render_enroll_json()` reads the unmounted
  signer's copies without checking `s_ready` (only `?nonce` refuses, 500).
  Gating `csi_mqtt::init()` alone would not hold (`set_identity()` plus the QR
  join, or a later `/api/mqtt/config` save through `open_client()`, opens the
  client), and skipping `datamgmt::process()` would also skip log rotation.
  Decide, per surface, whether a refused boot answers that it is refused or
  serves nothing, then gate each with a host case like
  `test_wap_sha_fail.cpp`'s. From code; not run on a device. Found by F403's
  review (#1919).

- [ ] **F420 [decision] canary-wap: a lone stored TLS entry keeps HTTPS off
  for good. Decide before F15.** F404 (#1919) refuses half a stored pair,
  which protects a pin when one entry's lookup misses only for this boot (IDF
  can set a page it cannot read aside for that boot). Three paths leave a lone
  entry for good: `tls_store_to_nvs()` writes the certificate and then the
  key, so a key put that fails leaves a lone certificate; power lost between
  the two puts does the same; and IDF erases a corrupt entry when it refuses
  its read and leaves its partner (F404's review, and the likely case). The
  factory reset keeps `tls_cert` and `tls_key` (`recovery_kit.test.js`,
  `THREAT_MODEL.md`), so every later boot refuses, and HTTPS does not come
  back without a full flash erase in a flasher; before F404 the next boot made
  a new pair over it. In none of the three does the refusal protect a pin that
  still works: in the store paths no pin was handed out
  (`g_tls_cert_persisted` stays false on a failed store), and in the corrupt
  case the pinned certificate or its key is gone. Options: roll the
  certificate back in `tls_store_to_nvs()` when the key put fails (and the
  reverse); persist a "pin handed out" marker, so that a half pair nobody
  pinned may be replaced; or treat a lone entry as Absent, accepting that a
  lookup that misses for one boot then replaces a pinned pair.
  `test_a_corrupt_entry_leaves_its_partner_and_https_off` and
  `test_half_a_stored_pair_is_refused` pin today's answer and change with the
  decision. Unreachable on stock units until F15. From code and the host
  suite. Found by F404's review (#1919).
  *Note (2026-11-04, #1920):* the flagship has the same answer now (F421):
  `tls_load_der_from_nvs()` refuses a lone `tls_cert` or `tls_key` on every
  boot and `initTls()` stores nothing over it, so HTTPS stays off there too
  until a full erase; `tls_store_der_to_nvs()` also writes the certificate
  before the key, and the boot's `[TLS] WARNING: certificate not stored` line
  names that outcome. A lone entry also arises without a failed store, on both
  trees: IDF erases an entry whose data CRC fails in the read that found it,
  so a pair with one corrupt half is a lone entry on the next boot. The
  decision covers both trees; `test_tls_load_der.cpp`'s
  `test_a_lone_entry_is_refused` pins the flagship's answer beside
  `test_wap_first_boot_nvs.cpp`'s, and both change with it. Still unreachable
  on stock units until F15.

- [ ] **F424 [decision] canary-wap: `GET /api/beacon/set` refuses a full set
  of full-length names.** `handle_set_list()` (`beacon_api.h`) refuses any
  answer over 4096 bytes ("set list too large"). 32 members with 24-byte names
  already exceed it (about 140 bytes each), and since F408 (#1919)
  `shown_text()` can grow a name to 72 bytes. The answer is valid JSON, never
  a cut one, but a full set cannot be listed. Decide a higher cap or a
  streamed answer. Beacon is off in every stock build. From code. Found by
  F408 (#1919).

- [ ] **F428 [decision] The flagship has no SPA card, and the reference
  device-api says it is a canary-wap.** Since F355 (#1919) `discovery.md` §1
  says the flagship (`firmware/canary`, `dt` `canary`) advertises, browses,
  runs an HTTP server and serves `GET /api/v1/peers`, but not `/api/v1/info`.
  The `dt` table and the SPA's `DEVICE_TYPES` still have no `canary` entry, so
  a flagship peer renders as "unknown" and takes the `http` branch; whether
  its receipt route answers CORS was not checked. Separately, the reference
  device-api reports `device_type` `canary-wap` (`device-state.js`), so the
  only pairing the SPA completes ends on "Canary WAP paired", beside a WAP
  card that now says the WAP firmware does not pair in this app. Decide
  whether the flagship gets a card and whether the reference server gets a
  type of its own; either ripples through `tests/api` and `tests/spa`. From
  code. Found by F355 (#1919).

- [ ] **F437 [code] Three canary-wap comments say what the code no longer
  does.** `mesh_network.h`'s `MAX_PUBKEY_RATE_TRACK` still reads "LRU of
  recent originating pubkeys"; since F416 (#1920) the victim is the row whose
  window started longest ago, and a run-out window is dropped at the next loop
  pass (a key used a second ago whose window opened 59 minutes ago goes before
  one whose window opened a minute ago). `check_wap_json_answers.py`'s J11
  `HEX_WRITERS` "why" for `nearby_prefix_from_name` says it copies the four
  bytes after the name prefix, and `ble_nearby.h`'s call-site comment says
  its four bytes are taken only when all four are hex digits; since F425
  (#1920) they are also folded to lowercase (the record's guarantee, hex
  digits only, still holds and the guard passes).
  Comment-only, three one-line edits; the J11 text moves no gate. From code.
  Found by F416's and F425's reviews (#1920).

- [ ] **F439 [code] `/api/status`'s `tls_mode_reason` never says why the
  stored certificate was refused.** `initTls()` sets `m_tls_reason` ("stored
  certificate unreadable: HTTPS off this boot, nothing stored over it"; F421,
  #1920) and the boot's `[WARN] TLS unavailable` line and the health log carry
  it, but `startHttpServer()` (`securacv_network.cpp`, `m_tls_reason = why;`
  after `tls_policy::decide`) overwrites it with `decide()`'s generic "no TLS
  certificate (generation unavailable or failed)", so `/api/status` reports
  that. Pre-existing for every `initTls()` failure (keygen, DRBG, now the
  refused read); the comment above the TLS section claimed otherwise and now
  says what happens. Let `decide()` keep `initTls()`'s reason when the
  certificate is missing; that touches the shared, host-tested
  `common/network/tls_policy.h`. Unreachable on stock units until F15. From
  code. Found by F421 (#1920).

- [ ] **F440 [code] canary-wap's `tls_load_from_nvs()` still reads
  Preferences' two zero lengths as absence.** F404's rule on the WAP
  (`canary_wap.ino`) takes `getBytesLength() == 0` for both entries as Absent,
  and Preferences answers 0 for a failed lookup too, so a lookup fault on both
  entries would generate a new pair over a stored one. Not reachable today
  (IDF's size query answers only OK, NOT_FOUND or INVALID_HANDLE once the open
  succeeded) and certgen is off on stock units (F15). The flagship asks IDF
  with an `nvs_get_blob` size probe since F421 (#1920); give the WAP the same
  probe for parity, with a host case in `test_wap_first_boot_nvs.cpp`. From
  code. Found by F421 (#1920).

- [ ] **F443 [code+decision] Both dashboards still splice device vocabulary
  into `innerHTML` and `onclick` strings.** Outside the rows F407, F422 and
  F423 moved to the escape-and-index pattern, the WAP's `web_ui.h` splices
  `onclick="removePeer('${peer.fingerprint}')"` (16 hex the device formats),
  `confirmChirp`/`dismissChirp('${nonce}')` (16 hex from `%02x`),
  `openAckModal(${log.seq})`, `${c.emoji}` and `${c.category}` (the chirp
  table's and `category_name()`'s vocabulary), `${peer.state}`, `${a.type}`,
  `${r.type_name}`, `d.key_id`, `${log.level_name}` and `${log.category}`, and
  the vault list's `it.name` (only names `vault_logic::filename_parse()`
  accepts); the flagship's `securacv_webui.cpp` the same `removePeer`,
  `confirmChirp`/`dismissChirp`, `openAckModal` and chirp splices (the chirp
  row is unserved, F176), `e.type` (the vision chip's `htypes` table), `FP:
  ${peer.fingerprint}` (`fp_hex`) and `${chirp.urgency}` (`urgency_name()`).
  Each is device vocabulary or device-formatted hex, not radio or stored free
  text, so none was changed. Decide whether to move them all to the index and
  escape pattern for defense in depth; this list is the whole grep, so that
  package starts from it. From code. Found by F422's review (#1920).

- [ ] **F444 [code] The WAP dashboard's Opera rename shows no refusal.**
  `web_ui.h`'s `saveOperaName()` posts `/api/mesh/name` and ignores the
  answer, so a name the F303 rule (#1920) refuses (a byte below 0x20, past 32
  bytes, not whole UTF-8) leaves the old name in place without a word, where
  `startPairing()` alerts `d.error`. Show `d.error` there too, then regenerate
  `web_assets_gz.h`. From code. Found by F303 (#1920).

- [ ] **F446 [code+decision, gated by U1] canary-wap Bluetooth: a Just Works
  bond that escapes F191's delete stays in the store, unlisted, with no road
  back to the retry.** F353's non-destructive half (#1920) leaves such a bond
  out of the rebuilt paired list, but it stays in NimBLE's store until Clear
  all, and on a unit whose list is already saved by identity (any owner
  pairing since F172) the rebuild never runs, so the roads F353 named (a
  reboot after NimBLE stored the bond and before the loop task applied its
  report, a reboot inside a kept bond's retry minute, a dropped link event)
  still leave one unlisted where only the F209 count and lockout line show it.
  Decide whether a kept bond's identity is remembered across a reboot (NVS) so
  the per-minute retry resumes, or whether boot deletes a bond whose stored
  PEER_SEC record says `ltk_present` and `authenticated` 0. Either needs the
  bench row first: that NimBLE-Arduino 2.3.8/2.5.0 really stores
  `authenticated == 0` for a Just Works bond and 1 for a passkey or Numeric
  Comparison bond on the WAP, since a wrong flag would delete the owner's
  bond. From code and the host harness; not bench-tested. Found by F353
  (#1920).

- [ ] **F447 [code+decision] The hub hand-off's HUB write is bound to the
  session, not to the link that sent WIFI_SETTINGS or to a clock.**
  `improv_ble.cpp`'s `handle_hub()` takes a HUB write while the session is
  Provisioned or Provisioning and never reads `door_open_now()` or which link
  wrote; it relies on there being one link. Once the join has succeeded that
  holds (a Provisioned session advertises the beacon non-connectable), but
  mid-join a Provisioning session still advertises connectable (`advert_for` →
  Improv; `tick()` re-puts the advert on air after a drop; `follow_link`
  clears only `link_up`), so a phone that drops before the verdict can be
  followed, inside the join's 30 s (`Timing::provisioning_timeout_ms`), by
  another client that inherits the session and whose HUB write is taken once
  the join lands. HUB also has no time bound of its own beyond the session's.
  `magic_pairing.md` §3.6 and the §9 lead say so since #1920. Options: tie HUB
  to the connection generation that sent WIFI_SETTINGS (`s_conn_gen` is
  already tracked), bound it to the 20 s linger, or both. From code. Found by
  F254's bench rows (#1920).

- [ ] **F448 [code+decision] The canary-vision SPA still offers and accepts IP
  addresses the CSP blocks.** `spa/app.js`'s `isPrivateUrl()` accepts RFC
  1918, link-local and loopback IP literals, the manual-entry placeholders
  suggest `192.168.1.47`, and `peerBaseUrl()` falls back to a peer's `ip`.
  Under the served `connect-src 'self' http://*.local` (F427, #1920) each of
  these fails like an unreachable device (`fetch` rejects with a TypeError)
  unless the IP is the SPA's own origin; the docs say so, `app.js` does not.
  Either have the SPA say "enter its `.local` name" when an IP is not its own
  origin, or widen `connect-src` to the `http:` scheme, the trade F427
  declined (it gives up the bound on where an injected script can send data).
  For whoever weighs the second: CSP3 says an IP host-source never matches
  (127.0.0.1 aside), but Chromium matched a literal `http://192.168.1.47`
  source in the probe, so listing one device's IP would be Chromium-only
  behavior. From code and a Chromium probe. Found by F427 (#1920).

### Parity & sub-projects

- [ ] **F20 [code] Arduino↔PlatformIO parity debt.** Canonical ledgers:
  `firmware/FEATURES.md` (the ❌/⚠️ dashboard) and
  `firmware/canary/CONSOLIDATION.md` (gap inventory, 16 rows since F29 added
  one; phase 3 half done, phases 5–8 unstarted). Gap #11 is done (option D —
  maintainer to confirm), its premise corrected: the canary never had an
  unauthenticated receipt route — it had none at all; the real exposure was
  `GET /` and `GET /setup` handing the bearer token to any home-LAN caller.
  Now a short BOOT tap opens a 30 s gate
  (`common/network/provisioning_gate.h`, host-tested) that admits exactly one
  consumer — one `GET /api/provisioning-receipt` (the WAP's shape) or one page
  load — and the pages carry the token only for first-boot setup, a
  bearer-authenticated request, a SoftAP peer or a spent tap; a bare LAN load
  gets an unlock banner instead. A fail-open found on the way is closed (with
  no credential provisioned, `checkOptional` accepted a bare `Bearer` header
  against the empty token), and
  `firmware/canary/scripts/check_route_security.py` now fails CI on any route
  that reaches no credential gate and is not on its public allowlist. The
  dashboard's canary gate row had read ✅ before any canary code existed; it
  reads ⚠️ until the U1 bench pass (one tap → one receipt, a second GET → 403)
  (#1704). Carrying over the WAP's session-cookie model, so a LAN reload stops
  needing a tap, is a Phase 6 follow-up.
  *Partly done (#1802), gap #10 reconciled (slice a; option "⚠️ with what is
  missing", maintainer to confirm):* `firmware/canary/CONSOLIDATION.md`'s row
  reads ⚠️, as `FEATURES.md`'s Hardware state row has since
  `common/health/boot_guard.h` landed: an NVS count of boots that never
  reached healthy (a power-on reset does not count), started over by a
  different image (an OTA install, a rollback, a re-flash; images are told
  apart by `app_elf_sha256`, so two builds that both leave it zero look like
  one image), and at 4 such boots a serial safe-mode console (never for an
  image still pending OTA confirmation: the bootloader's rollback owns that
  case) that brings nothing else up until the owner clears it (serial `c` then
  `y`, or BOOT held 2 s); host-tested
  (`firmware/tests_host/test_boot_policy.cpp`, `test_boot_guard.cpp`),
  CI-compiled, no bench pass. The row names what is still missing against the
  canary-wap's `hardware_state.h`: per-peripheral state (its GPS state machine
  and camera present/standby record in one `HardwareState`) and its degraded
  safe mode (optional peripherals off while the core witness runs, bounded
  automatic recovery reboots, `POST /api/safe-mode/retry`); the SD three-state
  is already ported (`securacv_storage`'s mount worker, `storage_sd_state()`).
  Docs only. Rejected: ✅ (most of `hardware_state.h` is not ported) and
  leaving ❌ (it contradicted FEATURES.md's Hardware state row and the code).
  Not taken: the rest of that port, the other per-gap ports and Phase 8. Next:
  the two Low-complexity rows still at ❌, #12 (audible chirp) and #14 (system
  monitor), then #15 (log acknowledgment and categories, Medium, already
  partial), then the unstarted phases.
  *Note (2026-11-04, #1916, F354):* the flagship's tap now goes to one asker
  and its banner asks before the press, and `CONSOLIDATION.md` gap #11 and
  `FEATURES.md` say so. The user and security docs, this item's slice, still
  describe first-asker or press-then-reload:
  `docs/getting_started_canary.md:557` ("then reload within 30 seconds"),
  `:564-567` ("whichever asks first") and `:818`;
  `docs/homeassistant_setup.md:448`; `docs/security/SECURITY_MODEL.md:311` and
  `:356`; `docs/security/SECURITY-AUDIT.md:204`'s amendment;
  `docs/PARITY.md:181`; and `docs/design/magic_pairing.md:489` and `:800`.
  `canary-local/tools/gen_wap.py:1255-1263`'s comment says the same, and its
  `must_words` pin keeps the guide's "On a WAP it hands the recovery kit to
  one asking address" paragraph scoped to a WAP, so rewriting that paragraph
  turns `canary-local.yml`'s gen_wap step red unless the pin moves with it;
  `test_docs_claims.py`'s `MUST_PASS_IN` and `RETIRED_IN` rows for
  `getting_started_canary.md` move too. The guide can also say the banner asks
  from a hidden tab and that a phone's screen should stay on.

- [ ] **F21 [human] canary-wap enterprise readiness** —
  `firmware/projects/canary-wap/ENTERPRISE_READINESS_TODO.md`. §1
  (security/privacy) is done. Reconciled in #1704: every tick carries its
  evidence pointer, boxes that something else superseded say so, and the
  code-sized items landed — a Good / Needs attention / Action required
  status strip from a host-tested verdict on `/api/status` (wording: option
  "the checklist's own three labels" — maintainer to confirm),
  `regression_check.sh --strict` (zero warnings in both modes, reviewed
  allowlists that fail when stale; a release refuses on it, which meets the
  security/privacy acceptance criterion), the offline-queue outage
  assertion, and `integrations/ha_frigate_mqtt/up.sh`. The status-strip box
  and its parent, "Simple status language", are ticked (#1704): the strip
  is CI-compiled (the Arduino CLI and PlatformIO canary-wap builds), not yet
  seen on a device, and its label wording is still maintainer to confirm.
  Still open: the three F34 boxes that remain (see F34) and the U1 / human
  rest —
  the under-10-minute unbox and the Docker + Frigate end-to-end run (the two
  acceptance criteria still unmet), the per-env boot-time budget and a live
  broker-down run.
  *Partly done (#1802), the pinning box:* the WAP's release rows
  (`firmware-release.yml`, `flasher-release.yml`, `ram_audit.yml`,
  `csi_module_disable_matrix.yml`) pass `core-version: "3.3.8"`, the
  sketch.yaml pin and `platform_core3`'s core, so a release's Arduino-CLI WAP
  build (a release builds the WAP through Arduino-CLI only) rides the core the
  PR's PlatformIO `canary-wap-*` builds already use. The two release jobs
  install it with `exclusive-core: "true"`: they install the display's 2.0.17
  and 3.3.10 later in the same job, and should a step ever restore another
  core's cache into `~/.arduino15` ahead of the WAP row, the two releases
  would sit side by side and an unversioned FQBN would take the newer
  (RELEASE_LESSONS 2026-07-29 (k)); today the WAP row is each job's first
  install. `flasher-release.yml` reads its pin from the dispatch ref, so a
  refresh of an older tag builds that tag's WAP image on today's pin, not on
  the core its own release used. `firmware.yml`'s PR row keeps floating on the
  action's weekly "latest" as the early warning for the next core, so a cut
  release can differ from its PR build's core. Decision (maintainer to
  confirm), recorded in `firmware/PLATFORMS.md` with what was rejected
  (pinning the PR row too, which loses the warning; leaving every row on
  latest, which builds a release on whatever core the action resolved that
  week). `ENTERPRISE_READINESS_TODO.md`'s pinning box and its parent are
  ticked; `test_gen_firmware_sbom.py` holds each WAP row to its side of the
  split, and the SBOM lists the four rows under arduino-esp32@3.3.8. The
  libraries stay unversioned in rows and profile. Checked here by actionlint
  and the SBOM; the release rows compile only on dispatch or tag, and the
  pinned core resolves only in CI. A cache cost the pin exposes is CI19.
  *Retagged [human] (#1920):* no code is left in this item. Every open box is
  a U1 bench row (the under-10-minute unbox, the Docker + Frigate end-to-end
  run, the per-env boot-time budget, a live broker-down run) or one of F34's
  two decisions.

- [ ] **F22 [code+human] canary-sentinel is Phase 0** — Phase 1a landed
  (#1703), compile-gated in CI and NOT released: canary-sense's
  network/witness path carried and pinned by `check_sentinel_net_sync.sh`
  (whole files but for named product regions), a new signed `sentinel`
  canonical covering every coarse field it publishes (one golden vector
  shared by firmware and HA), MQTT events + retained state, HA discovery,
  signed pull-OTA with one product per preset env (all declared
  unpublished), the setup portal and mDNS; HA dispatches the new event
  dialect and maps the device to modality "other". Decisions — the 1a/1b
  split, copy-and-pin over a `common/net` promotion, the "other" modality,
  the per-preset OTA names — maintainer to confirm. Open: Phase 1b (the
  WiFi-RF / CSI / BLE channels and the fleet-link beacon on the C6 radio) is
  bench-bound and not built, and the README's bench checklist — 10 boxes
  now, with Phase 1a's three — is U1.
  *Note (2026-10-22, device-details PR 7, #1892):* two Phase 1a defects in
  what the Sentinel says about its own sensors, fixed. Its retained health
  never carried the `radar` object: `publish_health_retained()` takes one
  (carried from canary-sense), and `main.cpp` passed none, so a Standard
  build's radar link reached Home Assistant nowhere. Both health publishes
  now pass `radar_link_health()`, judged by the stall rule the radar's
  fusion vote uses (`include/canary/sentinel_links.h`): no object before a
  frame or a whole stall window, and none on Lite. Once the radar has
  spoken, the object reads down exactly when the radar votes `Denied`; a
  radar silent since boot reads down there while its channel never votes,
  as an unwired one does, so the object is the only place it shows. The
  integration gives a Canary the radar-link sensor when its health reports
  the object (`sensor.py` `_wants_radar_link()`). The object's frame age and
  error count are activity in the device-details design's classes, which
  that design keeps off the broker tier; they ride the owner's broker as
  canary-sense's already do, a legacy exception its PR 7 asks for, and Home
  Assistant's diagnostics redaction lacks `last_frame_age_ms` until its
  PR 16. And the BH1750's `begin()` result was ignored, so a unit with no
  light sensor voted `Denied` on every read: a blinded channel for good, and
  on a preset with the radar an `Anomaly` whenever the radar saw a body. A
  sensor that did not answer at boot is now absent: the channel never votes,
  so it adds no suspicion, but the state topic's `channel_denied` is true
  for it (requirement R6, never a silent gap), so Home Assistant's
  Channel-blinded problem sensor stays on, as it did when the sensor voted
  `Denied`, and the boot console's `BH1750` row reads `not found: absent,
  never votes, reported as channel_denied`. One that answered and then fails
  a read is blinded (`Denied`). R2's text in `sentinel_requirements.h` says
  which sensors raise suspicion and which are reported instead.
  `firmware/tests_host/test_sentinel_links.cpp` (new, 55 checks) runs the
  rules alone and the door preset through the real engine, and holds
  `main.cpp`, which no host suite compiles, to calling them: the bodies of
  `radar_link_health()`, `read_light()` and `light_absent_at_boot()`, the
  snapshot's `channel_denied` and both health publishes are compared with
  their source text, comments and spacing aside. The old light rule fails it
  whether restored in the header or in `main.cpp`, as do a radar object
  dropped from either publish and an absent sensor dropped from
  `channel_denied`. Two new Home Assistant tests hold the gate (both fail on
  the old one). Host-tested; not run on hardware. Left: a sensor unplugged
  while the unit is off boots as absent, so it reaches the owner as a denied
  channel but not as suspicion; until the device-details record carries the
  part's status (device-details PR 15), only the anomaly score, and the
  level that score can lift to `Anomaly` (on every preset with the radar,
  it does while the radar sees a body), tell it from a blinded one on the
  wire.

- [ ] **F34 [code+decision] canary-wap enterprise boxes that are their own
  change** (from F21; each names itself in `ENTERPRISE_READINESS_TODO.md`).
  Two wait on a maintainer call: the guided factory reset — whether
  `POST /api/factory-reset` may exist at all (recommended: yes, but only
  behind the Bearer credential AND a BOOT-tap `provisioning_gate_take()`,
  with a two-step confirmation in the companion PWA; a remote credential
  alone must never wipe a witness) — and the setup profile presets — what
  "Frigate bridge mode" means (device-side or hub-side) before the three
  `/api/mqtt/config` presets and their plain-language tradeoff copy can be
  written. Two are plain code: the kernel's backend audit trail (a
  `docs/security/backend_audit.md` row per detector backend and a
  `cargo test` that every non-pure-Rust backend is feature-gated), and a
  firmware-side MQTT contract fixture (the WAP's `csi_mqtt.cpp` discovery /
  retained templates checked against the HA parsers in pytest, with QoS
  written down as the delivery bound).
  *Partly done (#1802), the backend audit trail:*
  `docs/security/backend_audit.md` has one row per backend module in
  `src/detect/backends/` (the motion-only `cpu` and `stub` backends, their
  shared `motion.rs` engine, and `tract` behind `backend-tract`, which
  `detect-eval` also enables), each listing the build crates the module names
  and whether any of them builds native code, from a source reading: nothing
  ran on a camera, and the tract crate tree was read from Cargo.lock only.
  `scripts/tests/test_backend_feature_gates.py` (unittest, in `lint.yml`'s
  `scripts/tests` discover; 37 cases) is driven from `mod.rs`: each `mod
  name;` resolves to `name.rs` or `name/mod.rs`, and every `.rs` under the
  module's directory is read. It fails a backend that names an optional crate,
  or a crate whose `Cargo.lock` tree holds `cc`, `cmake`, `bindgen`,
  `pkg-config` or `vcpkg`, without a `cfg(feature)` on its `mod` item or file
  (and one gated on a feature that does not pull its optional crate in); a
  default feature set that enables a backend's gate or optional crate
  (transitively); a backend with no row, a stale row, or a row whose feature
  cell disagrees, whose "Crates it names" cell does not list exactly the
  crates the module names, or whose "Native code" cell says none for a native
  one; a file no declared module owns, or a declared module with no file; and
  `mod.rs` naming an optional or native crate itself. It refuses what it
  cannot read: an `any(...)` or bare `not(...)` gate, an undeclared feature, a
  `#[path]` or `cfg_attr` on a module, `include!`, an inline module in
  `mod.rs`, a missing `Cargo.lock` or a crate it does not list. Either gate
  site alone (the `mod` item or the file's `#![cfg]`) is accepted, as in Rust,
  so dropping one of tract's two is caught by the tree-facts case rather than
  by the rule. Decision (maintainer to confirm): a static Python test, not the
  `cargo test` this item named; the kernel's crates are not fetchable
  everywhere the check should run, the property lives in the source text and
  the lock, and `rust.yml` keeps compiling the default and `backend-tract`
  sets. Second decision (maintainer to confirm): "non-pure-Rust" is read off
  `Cargo.lock` as above, for optional and required crates alike (a backend
  calling `rusqlite`, which builds bundled SQLCipher, needs a gate); rejected:
  "needs an optional crate" alone, reading each crate's source language (no
  crate source offline), and counting every `-sys` crate (`linux-raw-sys` is
  pure Rust); the lock reading cannot see prebuilt native code, or a build
  script that runs a compiler with none of those tools in its tree. It passes
  on the old tree, so it is proven by mutation: 30 cases plant each break on
  copies of the real inputs, and thirteen mutations of the checker each fail a
  case. `ENTERPRISE_READINESS_TODO.md`'s box is ticked.
  Found here: A102 (the tract model is not hashed at load).
  *Partly done (#1836), the MQTT contract fixture:*
  `firmware/projects/canary-wap/tests_host/test_wap_mqtt_bodies.cpp` (`make
  run-wap-mqtt-bodies`, in `run`; 356 checks) runs the real `csi_mqtt.cpp`
  over stubs/mqtt and a fake esp_mqtt client through a unit's life (the
  connect burst, a healthy loop, a tamper commit sent the way
  `csi_event_egress::pump()` sends one, a backfill replay, a unit in trouble,
  unsigned early-boot bodies, discovery removed). Its signed bodies are signed
  for real (device_signature.cpp compiled in, OpenSSL's Ed25519 on the repo's
  test key). It checks the record (every publish QoS 0, the last will QoS 1
  and retained, the command topics subscribed at QoS 1, state and discovery
  retained and events and tamper not, every body one strict JSON object,
  `replay` true on the backfilled row only, the sketch's update-state and
  acoustic bodies relayed byte for byte), delivers every command word a
  discovery config names back as MQTT_EVENT_DATA and checks it latches (no
  host test delivered a command before), and prints the record with `--json`.
  `custom_components/securacv/tests/test_wap_mqtt_contract.py` (31 cases: 17
  pass, 14 strict xfails) builds it with make, so there is no committed fixture to go
  stale. It replays the record into the integration, set up in MQTT mode
  against an emulated broker (retained on subscribe, an empty retained publish
  clears), and holds: the TOFU pin from health, the signed chain, counts and
  event verifying, Last Event's timestamp, zone, confidence, signed flag and
  `wifi-csi` modality, the health and egress attributes, the tamper, event-id
  and Chirp levels with the tamper body's type and severity, unsigned bodies
  read as unsigned, and the last will taking the device offline; and it holds
  the 33 discovery configs to the record at key and word level.
  `scripts/tests/test_ha_discovery_wap_render.py` (lint.yml, jinja2 at Home
  Assistant's pin, through `_ha_jinja`) renders every template on every body
  on its topic: availability, each sensor's field, Online and Channel Locked,
  Presence over every state core_presence.cpp and core_breathing.cpp emit,
  each acoustic sensor over securacv_audio.cpp's event words, and the device
  triggers firing on live rows of their own state only. The update-state and
  acoustic keys are held to canary_wap.ino. QoS is written down as the
  delivery bound (not 'QoS 0 everywhere': the last will and the command
  subscriptions are QoS 1). python.yml lists every file the harness compiles
  (the test holds the list to every rule of the compile's dependency file) and
  sets `SECURACV_WAP_HARNESS=require` on the pytest step, as lint.yml does on
  its scripts/tests step, so a toolchain that cannot build the harness fails
  there instead of skipping; that wiring has not run in Actions yet. Proven by
  mutation: 33 plantings in the firmware, the integration and the workflows
  each fail it (among them health unretained, QoS 1, the status key, a
  discovery template key, the mic switch's pl_on, the install word, an
  inverted Online, a Presence list without active, the smoke word, the anomaly
  trigger's payload, a trigger without its replay gate, the replay flag
  ignored, zone, device_type, severity and timestamp renamed, a dropped
  python.yml path, the require moved); 20 of them passed the first version.
  The firmware and the integration are not changed: the gaps the record shows
  are pinned as strict expected failures and filed. Host-tested only (the
  templates rendered with jinja2 configured as Home Assistant configures it,
  not in a running Home Assistant); reconnect on a live broker is U1.
  `ENTERPRISE_READINESS_TODO.md`'s box is ticked. The record also reproduces
  F41's open call (the aggregate Tamper sensor clears on the WAP's next health
  while the lid is open), pinned as a strict xfail. Found here: HA44 (the
  first event lost to Last Event), F454 (discovery entities on the
  non-retained events topic), HA45 (the device's firmware version), HA46
  (entities a WAP feeds nothing), HA47 (WAP presence and the HomeKit
  projection), HA48 (Memory Critical on a WAP) and F368 (Signal Strength after
  a connect). Still open in F34: the factory-reset and setup-preset decisions.

- [ ] **F454 [decision] Six of the canary-wap's own MQTT-discovery entities
  read the non-retained events topic.** *(Filed as F367, an id given twice.)* Presence, State, Confidence, Motion
  Score, Breathing Score and Breathing Rate take their state from `events`,
  which csi_mqtt.cpp publishes unretained (so a reconnect replays no committed
  event). After a Home Assistant or broker restart each reads unknown until
  the WAP's next committed event, which can be hours. Options: a retained
  presence snapshot topic for those configs (canary-sense's `state` pattern),
  or document the gap. Pinned by
  `custom_components/securacv/tests/test_wap_mqtt_contract.py::test_every_discovery_entity_finds_its_state_after_a_restart`
  (strict xfail). Found by F34's contract fixture (#1836).

- [ ] **F36 [code+human] Tighten Scout proximity pairing (F27 follow-up).**
  The window pairs the first single qualifying advert. Two tightenings need
  real advert data first (U1): an N-advert confirmation (several qualifying
  adverts from one `hashed_id` before it pairs; N depends on real advert
  rates), and a fleet-link / Chirp payload filter so a neighboring Canary's
  advert cannot win the window (it needs the advert payload passed into
  `ble_scout_on_advert()`, which today takes only MAC, RSSI and time). The
  WAP carries the same pairing module and could adopt the route after.
  *Partly done (#1802), the payload-filter half; option "only the fleet's own
  formats", maintainer to confirm:* the Scout's proximity pairing window no
  longer offers an advert in the fleet's own formats.
  `ble_scout_advert_kind.h` (a pure header, committed byte-identically in
  `firmware/canary/lib/securacv_ble_scan/src/` and the canary-wap sketch
  folder, and in `check_ble_scan_sync.sh`'s BYTE_FILES) classifies an advert's
  manufacturer data as a presence beacon (company 0xFFFF, type 0x10, 11 or 13
  bytes; the version byte is not read, so a newer schema at either length is
  still refused) or a Chirp (company 0xFFFF, 17 bytes, type 0x01..0x05).
  `ble_scout_on_advert()` gains an `offerable` flag and hands
  `pairing::offer()` only offerable adverts from unpaired devices; tracking a
  paired device never reads it. Both trees' scan callbacks read the
  manufacturer data once and pass `advert_kind::offerable()` (the canary's
  fleet roster feed reuses the same read); the canary-wap has no pairing
  route, and its copies match the canary's after the sync script's
  normalization. Not excluded (a choice): any advert without fleet-format
  manufacturer data, a Canary's included. Read from the source, a Canary sends
  one in at least these cases: a canary-wap DEV-profile build's Bluetooth
  channel advert (`bluetooth_channel.cpp`'s `SERVICE_UUID`; that profile has
  no Opera beacon), the legacy service-UUID advert the beacon code falls back
  to when its advert data is refused (`securacv_ble_status.cpp` in the canary;
  `ble_opera.h`, and `ble_chirp.h`'s restore with Opera compiled out, in the
  canary-wap), and a Sense or Vision Improv setup door's flags-and-name advert
  when no beacon was handed to it (`common/network/improv_ble.cpp`). In a FULL
  canary-wap the Opera beacon takes the primary advert and the service UUIDs
  ride the scan response, which the passive Scout (`setActiveScan(false)`)
  never requests. Rejected: a service-UUID rule (it would copy UUID strings
  from canary-wap-only headers into the canary tree with nothing holding the
  copies together, and would still miss the name-only advert), refusing every
  company-0xFFFF advert (0xFFFF is the Bluetooth testing id, which other
  people's hobby tags use too), and classifying with `fleet_beacon_parse()`
  (it refuses a schema version it does not know, says nothing about Chirps,
  and would need the fleet_link include path on firmware.yml's host compile
  lines). Host-tested in plain g++ with firmware.yml's run_test recipe:
  `test_ble_scout_pairing.cpp` holds the constants to `fleet_beacon.h` and to
  the Chirp shape `fleet_roster_feed.cpp` tables and the canary-wap's
  `ble_config.h` sends (both read from disk), refuses every blob
  `fleet_beacon_parse()` accepts, counts refusals over every length 0..40 and
  every type byte, classifies 19 near misses from exact-size heap copies,
  drives `ble_scout_on_advert()` so strong beacon and Chirp adverts from
  unpaired advertisers never pair while the near misses and adverts with no
  manufacturer data still do, and reads both trees' `ble_scout_nimble.cpp`
  (comments stripped) to hold each scan callback to one manufacturer-data
  read, `advert_kind::offerable(mfg_bytes, mfg.size())` and the verdict passed
  by the only `ble_scout_on_advert()` call; `test_ble_scout_broadcast.cpp`
  shows a paired device whose adverts are not offerable arriving, departing
  and broadcasting call for call like one whose adverts are. Both fail on the
  old code (it does not compile against the new call; with the flag ignored,
  the pairing test fails); 18 mutants of the filter, the gate and the pinned
  sources, and 14 mutants of the scan callback written into both trees, each
  fail a test or the sync check; before the callback pin, 10 of those 14
  passed every gate. On the integrated tree the pairing suite passes with that
  recipe and `check_ble_scan_sync.sh` holds. The `[env:full]` and canary-wap
  Arduino compiles are CI's; no bench pass. A phone, a stranger's tag, or a
  Canary advert without fleet-format manufacturer data held close can still
  win the window: that is the documented limit. The Scout limits paragraph of
  `docs/csi_developer_api.md` and the threat note in both copies of
  `ble_scout_pairing.h` still say a neighboring Canary's advert can pair;
  their new text, and a U1 row, are handed to the integrator (landed
  2026-10-19 by the post-#1802 verification: both copies of the threat note,
  the Scout limits paragraph, and the F36 section of
  `hardware_verification_checklist.md`). Still open (U1):
  the N-advert confirmation (several qualifying adverts from one `hashed_id`
  before it pairs), which waits for real advert rates from the bench, and
  whether the Canary adverts above reach a passive Scout at pairing strength.
  Found here: F316.

- [ ] **F316 [code] One header for the BLE Chirp advert's shape.** The 17-byte
  Chirp (company 0xFFFF, type 0x01..0x05, fp2 at [15-16]) is spelled out
  separately by its sender (the canary-wap's `ble_config.h` / `ble_chirp.h`),
  by each roster that reads it (`fleet_roster_feed.cpp` in the canary tree,
  the canary-sense and canary-vision `fleet_roster_scan.cpp` copies, the
  display's `chirp_scan.cpp`) and now by `ble_scout_advert_kind.h`; only
  `test_ble_scout_pairing.cpp` holds two of those together, by reading their
  source as text. A `fleet_link` header beside `fleet_beacon.h` (constants, a
  builder and a parser, host round-tripped) would let every copy include one
  definition, after which the pairing test includes it instead of reading
  files. Wave 18's F272 does not cover it (it is the community Chirp channel's
  C-ABI header, not this advert). Found by F36 (#1802).

- [ ] **F79 [code+decision] Below the backfill's bound, the SD event log is
  trusted input.** Both backfills (the canary's
  `csi_event_backfill::Planner`, and the canary-wap's, which runs the same
  Planner since F78 in #1762; its `iterate_since` is gone) refuse a
  card line at or above the allocator's next id (F46). They sign and send
  whatever a line below that bound says: its content, and its id, including
  an id a reboot skipped or one the allocator had passed by the time the
  walk read it. A forged line written ahead of real rows also moves the
  watermark past them, so the real rows behind it are never delivered
  (review probes on the real planner). Options: a per-line MAC under a
  device key, or replaying only rows the witness chain vouches for. Not
  tracked in the roadmap or the gaps ledger. Found by F46's review (#1761).
  *Since #1762:* both egresses drop a row waiting in their RAM hold at or
  below the watermark (F103's review), so a forged line also drops the held
  rows at or below its id, where they used to be sent and refused by HA's
  replay gate (all but an equal id).

- [ ] **F108 [decision] Both egresses wait 45 s for a card at every boot, even
  with none in.** A canary or canary-wap with an SD slot and no card (or
  another device's card) holds every row committed in the first 45 s after
  boot in RAM, and drops ambient rows from those seconds, because a card might
  still mount with older rows on it (F78, F104). Those rows arrive up to 45 s
  late, or not at all if the device reboots or its broker changes first, so
  such a device in a boot loop shorter than 45 s delivers no events-topic row
  (the tamper topic and the boot power verdict are unaffected). The boot probe
  often knows better: the storage manager's boot mount either found no card or
  is still running (`sd_mount_policy.h`). Decide whether a probe that finished
  and found no card ends the wait at once, at the cost of the rows of a card
  seated in the first 30 s, and weigh the boot-loop case. Found by F104
  (#1762).

- [ ] **F142 [code] A canary-wap loop-task MQTT publish can still wait out a
  network timeout, or longer on a trickling link.** Since F112 a link that
  stops costs a loop pass one `kNetworkTimeoutMs` (2 s) before the connection
  aborts, which is over LESSONS_LEARNED's "anything that can block >1 s does
  not belong on the loop task". A link that makes a little progress just
  inside the timeout restarts it on every partial write (`esp_mqtt_write()`
  loops until the message is out), so a long publish (a discovery config, a
  backfill row) or a pass of several can wait longer, bounded by throughput,
  not by the timeout. The reconnect burst no longer holds the loop (F112's
  review), and the connect is not a wait the loop meets. A publishing worker
  (the loop task hands rows to it and reads results back, keeping F78's
  per-row outcome) would take esp_mqtt off the loop task. From esp-mqtt's
  source at the IDF 5.5.4 pin and a host model; not probed. Found by F112
  (#1762).

- [ ] **F147 [code+decision] canary-wap event rows sent at QoS 0 can be lost
  at a connection abort with the egress watermark already past them.**
  `publish_raw()` reports a row sent when `esp_mqtt_client_publish()` returns
  a message id, which at QoS 0 means the bytes reached lwIP's send buffer, and
  the egress counts it delivered (`csi_event_egress.cpp`'s `to_sent()`: kSent
  is kYes) and moves on. When the link then stalls, the next publish that
  finds the buffer full waits out the network timeout and aborts the
  connection, and the rows still in the buffer are gone (the broker also
  publishes the will). F112 shortened the trigger from 10 s to 2 s; the loss
  path is older. QoS 1 for event rows with the watermark moved on the PUBACK
  (`MQTT_EVENT_PUBLISHED`), or any watermark that moves only on the broker's
  acknowledgment, would close it; either changes F78's per-row delivery
  accounting and the outbox's RAM use, hence the decision. From esp-mqtt's
  source and F112's review; not probed (U1: the stalled-broker row counts
  them). Found by F112's review (#1762).

- [ ] **F148 [decision] canary-wap's 2 s MQTT network timeout is also the
  whole CONNACK wait.** esp-mqtt's `esp_mqtt_connect()` waits
  `network_timeout_ms` in total for the broker's CONNACK. Home Assistant's
  Mosquitto add-on validates a login its Supervisor has not cached through
  Core, which its auth.py says takes 1-2 s, so the first connect after an
  add-on or Supervisor restart (or a password change) can fail once and retry
  10 s later (the retry is a cache hit). `kNetworkTimeoutMs` could be raised
  to 2500 ms and still keep `kNetworkOpsBudget` (3) times it under the 8 s
  watchdog, at the cost of a longer stall on the loop task; or left at 2 s and
  the one retry accepted. Decide, and say so in `csi_mqtt.h`. From source (the
  CONNACK wait at esp-mqtt 6af4446; auth.py only in the review's saved copy,
  not upstream); not probed. Found by F112's review (#1762).

- [ ] **F314 [decision] Whether ANOMALY-category rows follow F90's last-slot
  rule.** Since F90 a module with a ceiling of 2 or more keeps its last slot
  for an opening whose (type, state) differs from its most recent one, so
  `system.integrity`'s tamper (12/hour) and `anomaly.baseline` (10/hour) now
  admit at most the ceiling less one of a repeated kind per counter hour (a
  host probe: 11 of 12), keeping the last slot for a different kind. For
  tamper that can be a gain (a flapping SD card no longer takes the slot
  another tamper kind needs), but it changes life-safety-category rows; decide
  whether `CSI_CATEGORY_ANOMALY` should be exempt from the last-slot rule.
  Found reviewing F90 (#1802).

- [ ] **F91 [decision] Anomaly rows wait in an open bundle.** Every
  non-ambient row with a state goes through the bundler, ANOMALY rows
  included, and commits only when its bundle closes. `anomaly.baseline`'s
  cooldown is 600 s by default, so nothing ever merges into its bundle:
  bundling adds two minutes of RAM-only exposure and nothing else. On the
  canary since F81 an `unusual_motion` row commits two minutes after the
  motion (host, the canary's real bridge: detected at 71 s, committed at 191
  s; before F81, at 71 s), and a restart inside those two minutes loses it
  (F92). The canary has no open-bundle view; the canary-wap's
  `/api/events/today` snapshot shows a live alarm before its bundle closes.
  ANOMALY rows bypass quiet hours because they are urgent (`csi_event.cpp`
  step 4b), and `system.integrity` already seals its own key for exactly this
  reason (`tamper_events_module.cpp` `emit_kind`).
  Options: seal ANOMALY-category rows at commit on both trees, with
  `csi_bundler_flush_key()` after the emit as `system.integrity` does, or by
  having the bundler return COMMIT for them; or keep the bundle and say so.
  The same two-minute-plus wait applies to `core.breathing`,
  `core.multilink_fusion`'s `motion_confirmed`, `meta.empty_room_baseline`
  and, on BLE builds, `ble.scout` arrivals and departures, which are EVENT
  rows. Found reviewing F81 (#1762).
  *Since F93 (#1762):* a cooldown the canary-wap's Tuning Lab stored (30 to
  3600 s) applies from boot too, and one under two minutes lets a repeat merge
  into the open bundle; at the 600 s default nothing merges, as above.
  *Since F92 (#1836):* a planned restart commits the open anomaly row first
  (up to a second); a crash or power cut inside the two minutes still loses
  it, and the restarts F355 lists still do.

- [ ] **F355 [decision] The restarts and sleeps that still flush no open
  bundle.** The canary's `setup_check_timeout()` (`securacv_setup.cpp`) and
  the canary-wap's `setup_wizard::check_timeout()` restart from inside the
  setup code on the loop task without the F92 flush, as do the canary's
  network library's fallback `handle_reboot` (unused while main.cpp registers
  the restart owner), the canary-wap's safe-mode auto-recovery
  (`hardware_state.h` `safe_mode_update()`, on the loop task, with CSI
  running, since the WAP starts CSI in safe mode) and its retry (the httpd
  task, which may not pump the egress), the power policy's low-battery deep
  sleep on both trees (its wake is a reset; on the canary-wap CSI is off from
  battery saver on, so bundles have mostly closed; `power_monitor.h`'s
  critical-battery sleep too), and a factory reset of the credentials scope,
  which keeps the chain and the card. A first-boot device rarely has open
  bundles or a broker; decide whether each takes
  `flush_events_before_restart()` and the chain persist (through a hook for
  the libraries and `hardware_state.h`), or stays as it is and is said so in
  the setup and power docs. `check_event_egress_order.py` rule 10 and
  `check_wap_event_egress.py` rule 13 hold the two sleeps by their gates, so a
  new restart or sleep in either file must choose. Found doing F92 (#1836).

- [ ] **F218 [decision] Quiet Hours hold the daily summary, against the
  chokepoint's own comment.** On the canary-wap a Quiet Hours window spanning
  23:55 (the device default, 23:00 to 07:00, once turned on) holds
  `meta.daily_summary`'s row like any non-anomaly row: no `daily_summary`
  commits, and the morning's `held_summary` counts it as one more held row
  (`test_wap_daily_summary.cpp`'s
  `test_quiet_hours_over_2355_hold_the_summary`). `csi_event.cpp`'s step-4b
  comment (both trees) says every non-anomaly event from a non-meta module is
  suppressed, but the code exempts only `meta.quiet_hours`
  (`is_qh_summary_module`), so the daily summary, and
  `meta.empty_room_baseline`'s `baseline_status` should one emit inside the
  window, are held. Decide between the comment (let every meta module, or the
  summary alone, pass the gate as `meta.quiet_hours`' own summary does) and
  the code (keep them held and correct the comment), or move the summary to
  the window's close. F121 left both as they stand. Found by F121 (#1762).

- [ ] **F219 [code+decision] The daily summary's counts reach no reader.**
  `meta.daily_summary` puts its counts in `note` (`a<A> q<Q> x<X>`), which no
  published surface carries (the MQTT events body, the card line and
  `/api/events/today` leave `note` out), and they count committed ring rows,
  which closed bundles never reach (F77), so on a device they read `a0 q0 x0`.
  The wire shows only `"type":"daily_summary"` and `bundled` (rows walked,
  earlier summaries among them). Decide what the summary reports (periods or
  minutes per state, which needs F77's bundles, or a `duration_sec` sum) and
  where a reader sees it (a wire field, Home Assistant attributes, the Today
  sheet). Found by F121 (#1762).
  *Since F77 (#1836):* closed bundles reach the ring, so the counts are of
  bundles (a state held for three hours is about 18 rows);
  `test_csi_daily_summary.cpp` and `test_wap_daily_summary.cpp` pin `a0 q8 x0`
  for the day that read `a0 q0 x0`, and no published surface carries the note
  yet. What the summary reports and where a reader sees it is still the
  maintainer's call.

- [ ] **F124 [decision] The canary reads CSI module settings that nothing on it
  can write.** Since F93 the canary applies stored `csi` rows at boot by the
  canary-wap's rule, but it has no `/api/settings`, calibration or Tuning Lab
  surface, so its owner cannot set a preset, sensitivity, threshold or pet
  mode; only a canary-wap image on the same board could have stored them.
  Decide whether the canary gets a settings surface (and which keys), or says
  plainly that its CSI modules run on defaults. Found by F93 (#1762).

- [ ] **F126 [decision] The securacv-csi library changed a contract without a
  version.** Since F93, `csi_module_tick_all()` ticks no module until
  `csi_module_init_all()` has run its `init()`, so a third-party sketch that
  registers and ticks modules without that call gets no ticks.
  `library.json` and `library.properties` still say 0.4.0, and the committed
  firmware SBOM pins it. Decide whether to bump the library version and
  regenerate `sbom/sbom-firmware.cdx.json`. Found by F93 (#1762).
  *Note (#1802):* F90 is a second contract change since 0.4.0.
  `csi_event_emit()` no longer refuses at a full ceiling an emit that would
  merge into an open bundle, and with a ceiling of 2 or more keeps a module's
  last slot for an opening whose (type, state) differs from its most recent
  one; the library gains `csi_bundler_peek()` and `csi_bundler_refresh()`.
  `library.json` and `library.properties` still say 0.4.0; the bump, and the
  `sbom/sbom-firmware.cdx.json` regeneration it brings, stay this item's
  decision.

- [ ] **F315 [code] The dashboard does not say when stored thresholds override
  its preset.** Since F166 the calibration status carries `current_source`
  (`stored`, `preset` or `mixed`), but the canary-wap dashboard's preset
  picker and sensitivity slider show nothing when a stored `core.presence`
  threshold (a calibration's apply, a bundle import, a Tuning Lab slider) wins
  over them, so an owner who moves the slider sees no change. F127 made the
  Lab's reset remove the row; this note was F127's third option, deferred
  because `web_assets_gz.h` belonged to wave 18. Found doing F127 (#1802).

- [ ] **F165 [decision] canary-wap's first boot after an NVS erase says its
  event-id floor is unreadable and skips the card log's reload.**
  `apply_event_id_floor_from_nvs()` reports NVS unread when the `csi`
  namespace does not exist yet (the events egress creates it right after), so
  `csi_integration::init()` prints `[EVT-LOG] event-id floor not readable from
  NVS - the log is not reloaded this boot`, a fault's wording on a device that
  is only new, and does not arm the Today ring's reload from the card; the
  next boot reloads it. Decide whether an absent namespace is "nothing stored"
  (arm the reload; `csi_event_inject` refuses every row at or above the
  restored floor, so only rows below the one id space would load) or keep the
  answer and reword the line. F150 kept the answer as it was. From reading.
  Found by F150 (#1762).

- [ ] **F48 [code+decision] canary-wap's mesh crypto and its interop with the
  PIO tree.** Found by F33 (#1718). canary-wap's AUTH exchange still runs
  X25519 over long-term Ed25519 keys, the bug class F33 part 2 fixed for
  pairing, and its rotation encrypts under those session keys. canary-wap
  also HKDFs the pairing key where the PIO tree and spec §5.3 use it raw,
  and its pairing payload structs differ from the PIO tree's, so the two
  trees cannot pair with each other (the outer frame and the type-byte
  numbering they also disagreed on are reconciled by spec §4.5's
  `mesh_wire.h` — host-tested, crypto review pending; the payloads and
  the derivation are what remain). Concurrent removals on canary-wap cannot converge without
  a wire change: `MSG_OPERA_REKEY` names no removed device and has no
  announcement phase. Crypto review and a wire decision first, then code in
  both trees and a cross-tree host test.
  *Found by F95 (#1762):* the AUTH exchange's fix must also change its
  payloads, not only its key agreement. `AUTH_RESPONSE` (160 B) makes a 262 B
  signed frame against ESP-NOW's 250 B, so `send_to_peer` refuses it
  (host-probed, and pinned by `test_mesh_liveness_wap`'s
  `no_session_opens_and_a_removal_splits_the_opera`). Two more for the crypto
  review, from code: `handle_auth_challenge` derives a session for the member
  whose key the payload names, not for the envelope's signer, and
  `handle_auth_response` never checks `challenge_sig`; its `opera_proof` is a
  fixed signature over the `opera_id`, the same in every exchange. Until this
  lands, a canary-wap removal splits the opera (F95, F114).

- [~] **F49 [code] Mesh leftovers from F33.** (1) The canary's health-log
  list passes `millis()` to `formatTimestamp`, the same uptime-as-time-of-day
  rendering F33 part 7 fixed for alerts. (2) The joiner side's
  `CodeReadyCallback` never fires: the code arrives on `SEND_ACCEPT`, and
  dispatch reports only `NOTIFY_CODE_READY`. The web UI reads the code from
  `GET /api/mesh`, so no user sees it today. (3) There is no radio-MAC
  learning from opera frames, so a changed MAC means a re-pair. (4) Some
  PIO residual splits remain: both initiators already handed out, a mutual
  removal, or a lost ACK. A random-loss probe split 3 of 60 runs at 5%
  frame loss (spec §5.6 states it).
  *Done (#1756), parts 1-2; part 3 withdrawn (#1761):*
  (1) `GET /api/logs` now carries `uptime_ms` (handle_logs) and the log list
  renders each entry's `timestamp_ms` as an age against it (`formatLogAge`,
  shared with `formatAlertAge`) instead of `new Date(...)` — the made-up
  time of day is gone. New host test `test_canary_health_logs.test.js`
  (lifted-and-stubbed, Date poisoned, u32-wrap pinned) in the Makefile and
  firmware.yml's node step.
  (2) `dispatch_action` now fires the `CodeReadyCallback` on the joiner's
  `SEND_ACCEPT` too (its code-derivation beat — there is no separate
  `NOTIFY_CODE_READY` on that side), with the same code `pairing_confirmation_code()`
  reports. Pinned by `test_joiner_offer_surfaces_code_with_accept`.
  (3) **Withdrawn (#1761).** #1756 sent an opera envelope from an address
  the transport did not hold through the full verify (signature + opera_id
  + strict counter). On a pass it re-bound the signer's transport binding
  to that address (`bind_peer_mac`) and persisted it
  (`PeerMacLearnedCallback`, then `main.cpp` `save_peer_mac`). That verify
  does not establish an address:
  - the envelope signs no source or destination;
  - the PIO sender spends one outbound counter across every destination;
  - frames go out unencrypted (`encrypt = false`).
  So any genuine member frame the receiver had not heard passed the verify
  when an outsider re-sent it from its own address. That covers a missed
  broadcast, a rekey frame unicast to another member, or, after the
  receiver reboots, a frame heard since its last 5-minute counter save.
  A host probe against main (c104f56) showed the receiver:
  - moving B's binding to the outsider and dropping B's real address;
  - sending its next rotation OFFER to the outsider alone (the 60 s commit
    then forgot B when B stayed silent);
  - answering a replayed `REKEY_OFFER` with its `REKEY_ACCEPT` to the
    outsider.
  #1756's own test replayed only a frame the receiver had already heard,
  which the counter stops anyway.
  Now:
  - the unknown-sender hook takes pairing frames only again, so opera
    frames from unbound addresses drop as `recv_dropped_no_peer` before any
    verify;
  - `on_opera_frame` is void again, with no `via_unknown`;
  - the callback and `main.cpp`'s save of it are gone;
  - a changed radio MAC means a re-pair, which re-binds an already-trusted
    device.
  Kept: `bind_peer_mac`'s add-before-remove order (#1756 review).
  Pinned by four tests, each failing on #1756's code:
  - `test_unheard_broadcast_replayed_from_a_new_address_moves_nothing`
  - `test_unheard_rekey_offer_replayed_from_a_new_address_moves_nothing`
  - `test_bound_peer_new_address_is_dropped_not_learned` (#1756's test,
    rewritten)
  - `test_repair_moves_a_trusted_peers_address`
  Spec §8.3 now says a verified frame MUST NOT bind an address. How a
  changed MAC could be learned
  safely is F68.
  The review of the withdrawal found pre-existing limits, and they are
  documented here, not fixed:
  - the re-pair it points to does not authenticate the long-term key (F69);
  - on the PIO tree a re-pair cannot start with eight members bound,
    because the transport table has no slot for the new address;
  - the address a verified frame is recorded under can be a pairing
    partner's, with no spoofing (F70).
  Spec §11.1 items 4 and 5 are now marked partial. Host-tested only; not
  bench-verified (U1).
  canary-wap's half (#1761). canary-wap's `handle_received_message`
  re-pointed a member's `mac_addr`, and its ESP-NOW registration, at the
  source of any frame that passed `opera_id`, signature and its per-peer
  counter. A host probe ran the real `mesh_network.cpp` on new host stubs
  (`tests_host/stubs/mesh_net`, `mesh_net_sim.h`: several simulated devices,
  frames carried as bytes). It showed:
  - a frame B sent A that A missed, re-sent from an outsider's own address,
    moved B to the outsider. A dropped B's address from its ESP-NOW list and
    sent its frames for B there until B's next frame (its 30 s heartbeat)
    arrived;
  - canary-wap counts per destination and the envelope names none. So a
    frame B sent C did the same whenever B's counter for C ran ahead of A's
    last-seen for B. A also kept that counter, so B's own frames dropped as
    replays and B stayed bound to the outsider until its counter for A
    passed it;
  - after a power cut, a frame A heard since its last 5-minute counter save
    did the same;
  - from C's copied address, B's next real frame deleted C's ESP-NOW
    registration, so A could no longer reach C.
  Now a frame whose source is not the signer's own bound address drops
  before `verify_signature`. It spends no counter, reaches no handler and
  counts in `auth_failures`. No signed frame writes an address. That is
  stricter than the PIO tree (F70).
  The re-pair path could not re-bind either: `add_peer` appended a second
  entry for a key it already held, which no lookup reached, and a full opera
  refused it. It now re-binds the existing entry:
  - the new address is registered first;
  - an address another member holds is refused;
  - counters, name and state are kept;
  - the move is logged as a health WARNING.
  A changed address needs a re-pair with each member that holds it.
  canary-wap keeps its key in NVS, so a swapped module joins as a new
  member; an NVS image moved to another board, or a relay (F69), reaches the
  re-bind. A full canary-wap opera (16) still takes a re-pair (host-tested
  through `add_peer`, not a full pairing). A duplicate entry the old
  `add_peer` saved is folded into one at boot. It takes the later pairing's
  address unless another member holds it, and the fold is logged.
  Since a pairing now binds addresses, the review ran the pairing handlers
  the same way and found two paths that needed no owner:
  - a joiner took COMPLETE before its owner confirmed. Whoever answered its
    DISCOVER first replaced its opera, or, with the opera_secret and a
    member's public key, re-bound that member;
  - an initiator kept a finished pairing's keys until the 2-minute timeout,
    so a radio that overheard its OFFER got the opera_secret sealed to it
    (pre-existing).
  Both are closed as on the PIO state machine. The joiner takes the first
  OFFER, and a COMPLETE only after its owner confirmed. The initiator takes
  one ACCEPT, from where its OFFER went, and wipes the pairing after
  COMPLETE.
  Pinned by `test_mesh_address_wap` (19 tests against the real file; 15 fail
  on 89a4c56) and by `test_mesh_rx_gates_wap`'s
  `no_frame_moves_a_members_address` (fails on 89a4c56). Spec §8.3, §4.5's
  table, §3.3, §11.1 item 5, THREAT_MODEL and LESSONS_LEARNED are updated.
  Still open on canary-wap: a radio copying a member's own address can
  deliver that member's unheard frames, including ones sent to other
  members. They are dispatched and silence the member until its counter
  catches up (host-probed). Host-tested only; the Arduino compile is CI's;
  not bench-verified (U1).
  All 13 mesh C++
  suites + the webui node tests + the full firmware host suite pass; canary
  `[env:full]` compiles. **Part 4 (PIO residual splits) is left open — it
  rides F48's cross-tree wire decision (a mutual-removal convergence needs a
  `MSG_OPERA_REKEY` wire change), not something to land alone.** Not
  bench-verified on hardware (U1).
- [ ] **F68 [decision] How may a mesh peer's changed radio MAC be learned
  safely?** F49 part 3 (#1756) learned it from any verified opera frame,
  and it was withdrawn (#1761): the envelope signs no address and one
  counter serves every destination, so a replayed frame the receiver had
  never heard re-pointed a member at an outsider's radio. Today a changed
  MAC (a swapped module, a new locally-administered address) means a
  re-pair on the PIO tree, and on canary-wap too since #1761. Options:
  (a) **A signed self-asserted MAC.** The sender puts its own radio
  address in the signed bytes (a payload field or a header field under the
  shared `mesh_wire.h` registry). A receiver binds an address only when the
  frame names it and it equals the frame's source. This is a cross-tree
  wire change, so it is F48's territory. It stops a replay from the
  outsider's own address. It does not stop ESP-NOW source spoofing, but a
  spoofer can then only point the member at the member's own named
  address.
  (b) **A challenge the new address must answer.** A frame from a new
  address for a trusted fingerprint triggers a fresh nonce, and the new
  address must return it signed with the peer's key before any re-bind.
  That defeats replay, but costs new message types (also a wire change), a
  round trip, and a rate limit so strangers cannot make the device sign
  and send on demand.
  (c) **Keep re-pairing.** No wire change, but a re-pair is only as strong
  as pairing. Pairing binds whatever long-term key the DISCOVER or OFFER
  carried, and the 6-digit code covers only the ephemeral exchange. So an
  outsider relaying an owner-run pairing from its own address can re-point
  an already-trusted member at its own radio and persist it, or get its
  own key trusted (F69). On the PIO tree a re-pair also cannot start with
  eight members bound. Choosing (c) means F69 lands first.
  canary-wap's side: it dropped its verified-frame re-bind (#1761), and it
  re-pairs like the PIO tree, one member at a time. Whichever option is
  chosen is carried to both trees. Found by the adversarial
  review of #1756.

- [ ] **F69 [code+decision] Pairing does not authenticate the
  long-term keys.** On both trees the 6-digit code and the CONFIRM hash
  are derived from the ephemeral X25519 session key only.
  - PIO initiator: takes `ctx.peer_pubkey` from the plaintext DISCOVER and
    ignores the ACCEPT's `device_pubkey`.
  - PIO joiner: takes the OFFER's `device_pubkey`.
  - The long-term key signs nothing in the exchange.
  - canary-wap derives its code the same way (`mesh_pair_crypto`). Since
    #1761 its re-pair re-binds a member it already holds. So a relayed
    pairing whose codes match, claiming a member's key, re-binds that member
    to the relay's radio there too, until another re-pair (host-probed);
    before, it added an unreachable duplicate entry. On canary-wap the relay
    is the main way to reach that re-bind: its key lives in NVS, so a
    swapped module joins as a new member. The move is logged as a health
    WARNING. canary-wap's joiner needed no relay at all before #1761, since
    it took COMPLETE before its owner confirmed; #1761 closed that.
  The review of #1761 host-probed the PIO tree. An outsider relays an
  owner-run pairing (A and a new device D) from its own address, no
  spoofing, without touching the ephemeral keys:
  - both screens show the same code;
  - claiming B's key made A re-bind trusted member B to the outsider's
    radio, and main.cpp would persist it, so B's own frames then drop
    until another re-pair;
  - claiming its own key got it trusted, and it then signed a REKEY_OFFER
    that removed B.
  The results were identical before #1756, on c104f56 and on #1761. Spec
  §11.1 item 5 ("Man-in-the-Middle: visual confirmation codes") is now
  marked partial.
  Decide:
  - either bind both long-term public keys into the code and the CONFIRM
    hash, or sign the transcript with each side's long-term key. Either is
    a cross-tree wire change with canary-wap (F48's registry) and needs
    maintainer crypto review;
  - whether a pairing that presents an already-trusted fingerprint should
    re-bind silently. Today the only sign is main.cpp's WARN "Peer pubkey
    persisted but mesh_session register failed (table full or already
    registered)".

- [ ] **F72 [decision] canary-wap counts per destination, but its envelope
  names no destination.** A receiver judges a frame its member sent to another
  member against its own last-seen for that member. Such a frame is fresh
  whenever the member's counter for the other member ran ahead. That happens,
  for example, while the receiver sits at `PEER_UNKNOWN` in the member's
  table, which the alerts and the Beacon, channel-lock and hub-election sends
  skip; a fresh pairing leaves the partner there (host-probed with the #1761
  harness). Since F76 (#1762) heartbeats go to every member whatever its
  state, so they no longer skip it. Since F71 (#1762) a boot resumes every
  member one past the highest send-counter reservation stored for any of them,
  so a reboot levels the per-destination counters and the gap restarts from
  nothing. Between boots it grows with the traffic one member gets and another
  does not, as before. Since #1761 only a radio copying the member's own
  address can deliver such a frame (ESP-NOW does not authenticate a source).
  It is dispatched and pushes the receiver's last-seen ahead, which silences
  the member until its counter catches up (host-probed). A host probe (#1762):
  one frame replayed across members cost the member one genuine frame after a
  reboot and two with no reboot, where resuming each member from its own
  reservation (F71's first cut) cost up to a block (1025). Options:
  - put the destination fingerprint in the signed bytes (a wire change under
    spec §4.5's registry, with F48);
  - use one outbound counter per sender, as on the PIO tree. That makes any
    unheard frame fresh at every member instead.
  See THREAT_MODEL "Still open on canary-wap". Found by the canary-wap half
  of the F49 part 3 withdrawal (#1761).
  *Since F99 (#1762):* a canary-wap device starts a new member one past the
  highest counter it can have signed, so a pairing in a steady opera leaves the
  new member level with the busiest member (pinned by `test_mesh_liveness_wap`'s
  `a_new_member_starts_level_with_the_busiest_member`; F99's first version
  started at the highest reservation, a block further ahead, and one replayed
  frame cost a member 984 heartbeats). A boot still levels the counters, but a
  re-pair between boots can start the gap wide: after a busy member is removed
  and re-paired, its counter starts as far ahead of the quieter members' as its
  traffic was, until the next boot. Host-probed on #1762: A
  sent B 3072 frames and C 5, removed B (C re-paired after the F95 split), and
  re-paired B; A's next counters were 3073 to B and 9 to C, so one A-to-B frame
  replayed at C from A's address drops A's next 3065 frames at C, about 25
  hours of heartbeats. One counter per sender or a destination in the signed
  bytes closes it.

- [ ] **F451 [decision] Ship F94's role-bound CONFIRM ahead of pairing v1, or
  fold it into v1.** *(Filed as F356, an id given twice.)* F94 (#1836) is a wire break of its own: pairing firmware
  before it cannot pair with firmware after it, on either tree. Pairing v1 is
  a second break on both trees: F69's long-term keys bound into the code and
  CONFIRM, or a transcript signed with them, and F115's commitment to one
  ephemeral. Shipping F94 now closes the reflection today and costs two
  breaks. Folding it into v1 costs one break and leaves the reflection open
  until v1 ships. Either way v1 can keep F94's two domains
  (`securacv:pair:confirm:init:v1`, `securacv:pair:confirm:join:v1`) and their
  pinned vectors, or take new ones. Crypto review: maintainer to confirm.
  Found by F94 (#1836).

- [ ] **F452 [decision] Fail or drop a joiner's own CONFIRM reflected to it.** *(Filed as F357, an id given twice.)*
  Since F94 (#1836) a joiner that reads a CONFIRM equal to its own role's
  hash, from the initiator's address, ends its pairing (`bad_confirm`), while
  its initiator, holding the genuine CONFIRM, completes and holds a member
  that never joined; a wrong hash sent to the joiner in that slot already
  caused the same one-sided ending. The joiner could instead drop such a
  CONFIRM, so the honest pairing completes through a reflection and any other
  mismatch still ends it. F94 keeps the failure: a reflected CONFIRM shows a
  radio forging the partner's address, and dropping it gains nothing against a
  radio that can send any other wrong hash in the same slot.
  `test_a_joiners_reflected_confirm_ends_only_the_joiners_pairing` (both
  trees' tests) pins the current choice and fails under the other. Found by
  F94's protocol review (#1836).

- [ ] **F360 [decision] Keep or remove the PlatformIO tree's leading CONFIRM
  now that F94 leaves it no firmware to serve.** Every COMPLETE, and every
  F134 copy, carries the initiator's CONFIRM in front of it (F97). It was
  written for a joiner on firmware before F97, which is also before F94 and
  since F94 (#1836) fails closed at the CONFIRM; an updated joiner does not
  need it, and a lost first COMPLETE is recovered by the F134 copies in both
  orders (host-probed in review). The joiner's bounded CONFIRM re-send (F117),
  the other pre-F97 path, is also loss handling between updated devices and
  should stay: when the joiner's owner confirmed first and its CONFIRM was
  lost, the re-send is what reaches the initiator and brings the COMPLETE, and
  without it both sides time out
  (`test_the_joiners_resend_recovers_its_lost_confirm`). Decide whether to
  keep the leading CONFIRM, or remove it, the pre-F97 joiner models in
  `test_mesh_pairing` and `test_mesh_session`, and the frame it adds to every
  COMPLETE and copy; `mesh_pairing.h` ("Firmware before F94") and spec §5.2
  state where both paths stand now. Found by F94 (#1836).

- [ ] **F115 [code+decision] The 6-digit pairing code has no commitment, so a
  relay that swaps both ephemeral keys can grind it.** Nothing commits either
  side's ephemeral before the other side's is sent. The relay answers the
  joiner first with its own OFFER (the joiner shows its code and stops its
  DISCOVER), DISCOVERs the initiator as a joiner, and searches ephemerals
  against the initiator's OFFER until the initiator's code equals the
  joiner's; it then opens the COMPLETE and holds the `opera_secret`.
  Host-probed on canary-wap's real handlers in F98-F100's review and rerun on
  its branch: about 1.5 million X25519 tries, under 3 minutes on one host
  core, and the search splits across cores, against the 2-minute pairing
  timeout; both screens showed 312463, both owners confirmed, and the relay
  read A's `opera_secret`. The relay presented both devices' own long-term
  keys, so this is not F69's key substitution. Fix with a commitment to one
  side's ephemeral before it sees the other's (the joiner's DISCOVER could
  carry a hash of its ephemeral, revealed in its ACCEPT; Bluetooth's numeric
  comparison commits to a nonce that way) or a much longer code, together with
  F69's transcript binding: a wire change on both trees (F48). The PlatformIO
  tree derives its code the same way, with OFFER then ACCEPT in the same order
  (read from code, not probed). The threat model's claim that a swapped
  ephemeral shows different codes "by construction" and spec §11.1 item 5
  were corrected in #1762. Found by F98-F100's review (#1762).

- [~] **F95 [code] canary-wap never opens an AUTH session, so removing a
  member splits the opera.** Nothing sends `MSG_AUTH_CHALLENGE`: the handlers
  exist, but no code sends one. So `session_established` is never true, and
  `remove_peer`'s rotation sends `MSG_OPERA_REKEY` to no member. With no ACK
  pending, `maybe_finalize_rekey` commits the new secret at once, resets every
  member's counters, and marks them AUTHENTICATING or STALE. The survivors
  keep the old `opera_id`, and from then on each side drops the other's
  frames. Host-probed with the #1761 harness: after 10 simulated minutes of a
  working three-member opera, 0 of 6 entries held a session. A removed C, sent
  B no REKEY, and changed its own `opera_id` at once; three minutes later B
  held A as STALE and A held B at AUTHENTICATING. F48 covers how the AUTH key
  is derived; this item is that nothing starts the exchange. Until it is
  fixed, every survivor must re-pair after any removal on canary-wap. Related,
  from code and not probed: a rotation resets a member's last-seen to 0 in RAM
  only, and `replay_ctrs` keeps the old value until the next 5-minute save, so
  a receiver that reboots in between would drop the sender's restarted
  counters until they climb back. Found while fixing F71-F76 (#1762).
  *Partly done (#1762); the main item stays open and needs F48 first.*
  Starting the AUTH exchange does not fix it as the exchange stands
  (host-probed, and pinned by `test_mesh_liveness_wap`'s
  `no_session_opens_and_a_removal_splits_the_opera`, which fails the day it
  stops being true):
  - the `AUTH_RESPONSE` payload is 160 B, so its signed frame is 262 B against
    the 250 B an ESP-NOW frame carries, and `send_to_peer` refuses it. A
    challenged member marks a session the challenger never gets;
  - were it sent, the two keys would still differ: the exchange runs X25519
    over the long-term Ed25519 keys (F48's bug class), so every
    `MSG_OPERA_REKEY` would fail to decrypt and the rotation would commit at
    its 60 s timeout with the same split.
  Both are wire or derivation changes, F48's (added there). Until then a
  removal moves the remover alone to a new `opera_id`; every survivor keeps the
  old one, keeps trusting the removed device, and must re-pair with the
  remover. Spec §3.1 and §5.6 and the threat model now say so (the §5.6 caveat
  does not hold on canary-wap). Whether to rotate at all meanwhile is F114.
  The related counter gap is fixed: a rotation no longer resets any counter
  (tx 1, rx 0, on both sides), as the PIO tree's rotation keeps its own. The
  reset bought nothing (frames are signed with the long-term key, and the
  `opera_id` in the signed bytes kills a frame from before the rotation) and
  cost two things. A survivor the rotation did not reach (every one, today)
  dropped the remover's frames after the re-pair that rejoined it, until they
  climbed back (host-probed). And the last-seen reset was in RAM only, so a
  reboot before the 5-minute `replay_ctrs` save restored the old value with
  the same effect. Pinned by
  `a_survivor_re_paired_after_a_rotation_hears_the_remover_at_once` and
  `a_rotation_that_reaches_a_member_keeps_every_counter` (the test gives both
  sides one session key, standing in for a working AUTH exchange); each of the
  four resets put back alone fails one of them. This supersedes F71's Done
  text on the rekey (corrected there). Host-tested only; the Arduino compile
  is CI's; not bench-verified (U1).
- [ ] **F114 [decision] Should a canary-wap removal rotate while no survivor
  can be told?** Until F48 no member holds an AUTH session (F95), so
  `remove_peer`'s rotation reaches no one: it only moves the remover to a new
  `opera_id` and splits it from every survivor, which must re-pair with it. The
  rotation buys nothing there today: the remover drops the removed device by
  its key either way, and the survivors keep trusting it either way. The
  routine way to hit it is not a hostile device: since F98 a device that comes
  back at its old radio address with a new key (an NVS erase or a reflash) is
  refused by every member that still holds its old entry, and removing that
  entry at each of them moves each to an `opera_id` of its own, so one
  reflashed device can scatter the whole opera. Options: keep rotating (the
  §5.6 flow, ready for F48); or skip the rotation, and log it, when no
  survivor holds a session, which keeps the opera together until F48. Found by
  F95 and F98's review (#1762).

- [ ] **F145 [decision] canary-wap's fresh-device Bluetooth TX power is +3
  dBm, not the +9 its settings comment promises.** `bluetooth_channel.cpp`'s
  `g_settings` initializer sets `.tx_power = 9` with a comment that +9 is the
  power devices always ran at, but `load_settings()` reads
  `nvs->getChar("bt_tx_pwr", 3)`, so on a device with no stored key the boot
  sets +3. Decide which is meant, make both say it, and pin it
  (`test_bluetooth_commands_wap.cpp` can). Found by F111's Bluetooth harness
  (#1762).

- [ ] **F168 [decision] A phone that asks to pair while the owner has not
  started pairing mode is refused on the next pass, by accident.**
  `onConfirmPassKey` (now its event, F143) sets the session to confirming
  without touching `started_ms`, so `update()`'s pairing timeout measures from
  the last owner-started pairing (or boot), and whenever that was 60 s or more
  ago it cancels at once, answering no on the phone's link. Pre-existing, kept
  by F143; `test_bluetooth_commands_wap.cpp`'s full-queue and security tests
  start pairing mode first for that reason. Decide whether a pairing outside
  pairing mode is refused on purpose (then refuse it by name, with a test) or
  gets its own 60 s. Since F171 (#1762, wave 13) the same holds on the FULL
  profile. Found by F143 (#1762).

- [ ] **F177 [decision] canary-wap's Bluetooth status views sit in internal
  DRAM.** The views F138's Bluetooth half publishes (`bluetooth_channel.cpp`'s
  `g_status_view`, `g_scan_view` with up to 16 scanned devices,
  `g_paired_view` with up to 8 paired ones) are static `loop_snapshot::Value`
  copies in internal DRAM, the internal heap the PSRAM diet freed for the BLE
  stack, about 1.5 KB, and `ram_audit.yml` names none of them. Since #1762
  `loop_snapshot.h` has `AttachedValue` (the bytes in a PSRAM block the loop
  task attaches, the lock on-die), which the Chirp tables use. Decide per
  view, from its measured size, whether it moves to a PSRAM block (a paired
  view that carries key material stays on-die, as mesh `g_peers` does), and
  add any that stays static and reaches 256 bytes to the RAM audit's guard.
  Found by F138's review (#1762).

- [ ] **F187 [decision] FULL-profile canary-wap: Opera's advertising and Chirp
  ignore the Bluetooth switch.** `disable()` (POST /disable, or the settings'
  `"enabled": false`) stops the shared NimBLE advertiser, but on FULL Opera's
  onConnect and onDisconnect (`ble_opera.h`) and `ble_chirp.h`'s
  `restoreOperaAdvertising()` after every chirp (the heartbeat chirp comes
  every 5 minutes) start it again, connectable, whatever the pairing channel's
  `enabled` says. Since F173 each link that advertising lets in while
  Bluetooth is off is dropped at once, whatever service it came for (on FULL
  no BLE client can stay connected while Bluetooth is off), so a phone or
  Canary set to reconnect can loop connect-and-drop. Decide whether "Bluetooth
  off" on FULL silences Opera's discovery advertising and the chirps, or the
  web UI says BLE Discovery stays on and which services Bluetooth off refuses.
  From code and the host harness (`full_profile_off_refuses_every_link`); not
  probed. Found by F173 (#1762).

- [ ] **F188 [decision] FULL-profile canary-wap: a second BLE client can
  connect while one is linked, and the pairing channel tracks only the
  newest.** Opera re-advertises (connectable) after every connect, and since
  F171 the channel leaves that advertising on through a link on FULL (stopping
  it would take the fleet-link beacon off the air), so with NimBLE-Arduino's
  default of 3 connections a second phone, Canary or app session connects
  beside the first. The channel records only the newest link, whatever service
  it came for: the status and connection card describe that one; its
  inactivity timeout (5 minutes by default) counts only the channel's own
  characteristics' activity (Opera's, BLE OTA's, provisioning's, the console's
  and the log export's post none), so a recorded OTA, provisioning or Opera
  session idle on the channel's characteristics for that long is dropped; and
  a second phone's Numeric Comparison replaces the first's on show. Since the
  F171 review another link's encryption no longer labels the recorded one,
  only the recorded link's end clears it (F169), and the timeout drops only
  it; the owner's Disconnect drops every link. Decide whether FULL keeps one
  link (a non-connectable beacon while linked, or Opera not re-advertising) or
  the channel tracks every link and counts every service's activity. From code
  and the host harness; not probed. Found by F171 (#1762).

- [ ] **F207 [decision] canary-wap Bluetooth: NimBLE's bond store holds 3
  phones and the paired list 8.** NimBLE-Arduino 2.3.8 and 2.5.0 keep
  `CONFIG_BT_NIMBLE_MAX_BONDS` = 3 (the nimconfig.h default; no build
  overrides it) while `MAX_PAIRED_DEVICES` is 8. Since F189 a fourth phone is
  refused while three bonds are held (it used to evict the oldest), so five of
  the list's slots can never fill and the dashboard promises a list it cannot
  keep. A `#define` in the sketch does not reach the library's compile (the
  Arduino IDE builds libraries separately; a PlatformIO `build_flags` -D
  does), and each bond costs store RAM and NVS. Decide: raise it through build
  flags on the PlatformIO envs (and say what an Arduino IDE build keeps), or
  cap the list and the dashboard at 3. From NimBLE source and the host
  harness; not probed. Found by F189 (#1762).

- [ ] **F208 [decision] canary-wap Bluetooth: NimBLE's CCCD store may be too
  small for three bonded phones' subscriptions.** `CONFIG_BT_NIMBLE_MAX_CCCDS`
  keeps 8 records (the nimconfig.h default in NimBLE-Arduino 2.3.8 and 2.5.0;
  no build overrides it). NimBLE persists one per bonded phone per
  characteristic it subscribes to, and the sketch has 12
  `NIMBLE_PROPERTY::NOTIFY` sites, so three bonded phones subscribing to three
  each make nine. Since F189 a full CCCD store refuses the record: the phone's
  subscription write is answered with an ATT error and the subscription holds
  for that link only (NimBLE's default unpaired another phone's bond instead).
  Decide: raise it with the bond store (F207) through PlatformIO build flags,
  or keep the refusal and say in the dashboard that a phone's subscriptions
  may not survive a reconnect. From NimBLE source and the host harness; not
  probed. Found by F189's review (#1762).

- [ ] **F449 [code+decision] canary-wap Bluetooth: on FULL a late tap's
  `init()` can fill a server `ble_manager` made and started.** *(Filed as F351, an id given twice.)* When the
  worker's `bluetooth_channel::init()` is refused, the worker goes on to
  `ble_manager::init()`, whose own heap guard and `NimBLEDevice::init()` can
  pass where the channel's did not (a heap that recovered in between, a
  retryable NimBLE failure); Opera then registers on
  `NimBLEDevice::createServer()`'s singleton and the finalize stage starts its
  advertising (`operaStart()`). A later tap's `init()` finds NimBLE up, skips
  its heap guard, and creates the channel's service, the OTA, console,
  provisioning, Improv, log and witness exports and the standard profiles on
  that server, which may already have started: F167's case from the other
  side, which F260's report does not cover (it lists only the worker's
  services). Decide whether the channel refuses a late `init()` on a server it
  did not create, or the worker skips ble_manager when the channel's `init()`
  was refused. From code (`ble_manager.h`:106-143, `ble_opera.h`:274); not
  probed. Found by F260 (#1836).

- [ ] **F450 [decision] canary-wap Bluetooth: a refused or canceled pairing's
  link stays up, and Cancel does not cancel a shown passkey.** *(Filed as F352, an id given twice.)* Since #1836 a
  passkey outside the owner's pairing mode is refused by handing NimBLE a
  passkey it cannot use (F191), but `onPassKeyDisplay()` names no link, so the
  stranger's link stays until NimBLE's 30 s pairing timeout and then until the
  inactivity timeout (5 minutes by default); a failed or rejected pairing's
  link, and one whose comparison the channel answered no outside pairing mode,
  stays the same way. On DEV the channel stops advertising while a link is
  recorded, so such a link keeps the WAP from being found for that time, and
  the stranger can connect again. And Cancel or the pairing-mode timeout
  clears the channel's pairing state but not a Passkey Entry whose six digits
  were already shown: NimBLE holds that key until its own timeout, so a phone
  that types them after Cancel still bonds, and `apply_auth_complete()`'s
  authenticated branch lists it with no pairing-mode check (the digits were
  shown to the owner, so not a bond without the owner, but Cancel does not
  cancel it). Decide whether a failed or canceled pairing on the recorded link
  drops it (the owner's own rejected pairing included), the advertiser keeps
  running beside an unauthenticated link, and an authenticated bond reported
  outside pairing mode is listed, logged or refused. From code and
  NimBLE-Arduino 2.3.8 source; not probed. Found by F191 and its review
  (#1836).

- [ ] **F354 [decision] canary-wap Bluetooth: a radio sending a listed phone's
  identity address deletes its bond.** NimBLE-Arduino's server answers
  `BLE_GAP_EVENT_REPEAT_PAIRING` itself (NimBLEServer.cpp, 2.5.0 :608-626;
  2.3.8 the same handler, per the review): when a peer whose identity matches
  a stored bond starts a new pairing, it deletes the old bond
  (`ble_store_util_delete_peer()`) and lets the pairing go on. A phone's
  identity address is usually its public Bluetooth address, which any radio
  can send, so a stranger who has heard it may delete the owner's bond with
  one pairing attempt, in or out of pairing mode; since #1836 the Just Works
  bond that attempt makes is deleted too and the listed entry dropped with it
  (F191's review), so the owner's phone must pair again (no stranger's bond is
  kept). Whether a phone's own controller then refuses the stranger's address,
  and which phones are exposed, is not read here. Decide whether the channel
  answers repeat pairing itself (a GAP event listener ahead of the library's,
  refusing it outside the owner's pairing mode), or the owner is told when a
  listed phone's bond was replaced. From code (NimBLE-Arduino source, scratch
  clone); not probed. Found by the review of F191 (#1836).

- [ ] **F175 [decision] A canary-wap with no GPS fix can never originate on
  Chirp.** The sketch has no SNTP; GPS is its one wall-clock source (F8, F28),
  and a send, a confirmation and a suppress vote all wait for `time()` to pass
  `MIN_UNIX_TIME`, while night mode reads true (the conservative answer).
  Since F146 the owner is told so (`clock_unsynced`), but a board without the
  GPS module, or indoors, stays receive-only. Decide whether Chirp takes a
  time from somewhere else (SNTP over the uplink, the hub, the phone in the
  companion wizard, as F28 takes the zone) or the Chirp page says up front
  that sending needs GPS. Found by F146 (#1762).

- [ ] **F362 [decision] Chirp spec 2.5.4's "4-5 chirps per day" and 2.5.8's
  "~5/day" are not what the cooldown tiers allow.** Spec 2.5.4's design list
  says "Max 4-5 chirps per day per device" and 2.5.8's threat table says
  "Escalating cooldowns (max ~5/day)", but the tiers (5 min, 15 min, 1 h, then
  4 h) let a device send 9 in its first 24 hours and then 6 a day, since F193
  resets them only after 24 quiet hours (before F193, 9 every day). F193's
  paragraph in 2.5.4 states the real count, and
  `test_chirp_commands_wap.cpp`'s
  `the_tiers_let_9_out_on_the_first_day_then_6` counts it from the constants.
  Decide whether the list and the table are corrected or the tiers lengthened
  to meet them (a longer top tier, or a daily cap), which would change the
  canary-wap's COOLDOWN_TIER_* constants,
  `test_chirp_protocol_invariants.cpp`'s pins of them and that test's counts.
  From the spec and code. Found by F193 (#1836).

- [ ] **F453 [decision] canary-wap Chirp's `chirp sent` health line keeps a
  lasting record of the owner's chirps on the SD card.** *(Filed as F364, an id given twice.)* `send_chirp()` logs
  `chirp sent: <template text> (<urgency>, tier N)` at INFO, the default store
  threshold (`g_log_min_level`), and `log_health()` stages every line it keeps
  for `health_store_drain()`, which appends it with its sequence number and
  uptime to that boot's `/HEALTH/boot_<count>.jsonl` whenever a card is
  mounted (FEATURE_SD_STORAGE is 1 in the full profile, the one with Chirp);
  the files outlive reboots until datamgmt's /HEALTH count rotation drops
  them. F193 kept the cooldown tiers out of NVS because storing them would
  record when the owner chirped, and spec 6.4 says "Cannot prove a specific
  device sent a chirp"; a card holding this device's `chirp sent` lines is
  such a proof. Spec 2.5.4 now says the line reaches the card. Decide whether
  it drops the template text, moves below the default threshold, stays out of
  the SD tier, or stays as a deliberate local record that 6.4 then names. From
  code. Found by F193's fix pass (#1836).

- [ ] **F195 [decision] canary-wap Chirp's POST routes answer refusals with
  different statuses.** Since F174 a refused confirm answers 409 or 404, while
  a refused send (F146's `chirp_disabled`, `presence_required`, `cooldown`,
  `clock_unsynced`, `night_restricted`), a send with an unknown template, a
  refused mute (`invalid_duration`) and a failed enable still answer 200 with
  `success:false`. Decide whether every Chirp refusal carries a status (the
  dashboard reads the body either way, and must never get a 403, which it
  takes for a bad token). Found by F174 (#1762).

- [ ] **F228 [decision] canary-wap: the fleet scan lists every
  `_securacv._tcp` advert as a Canary.** Any device on the LAN can advertise
  the service with any TXT values, and adverts are unsigned. GET
  `/api/fleet/scan` keeps the shortest adverts that fit (F211), so a long
  advert can no longer push a Canary out, but short adverts nobody owns win a
  place ahead of a real Canary whose row is longer: eight short ones fill the
  list, and the Fleet sheet cannot tell them apart. Decide whether the list
  prefers adverts it can verify (a device id the fleet knows, a signed TXT
  field) or marks rows as unverified. From code. Found by F211 (#1762).

- [ ] **F268 [decision] canary-wap: the MQTT pages' `maxlength` cuts a longer
  paste without a word.** F241 gave the `/mqtt` page's host, user, password
  and prefix inputs, and the setup wizard's Home Assistant step, a `maxlength`
  at the device's limits. A browser enforces maxlength on inserted text by
  cutting it. Chromium cut a 12-character insert into a `maxlength=8` input to
  8 (probed with Playwright's `keyboard.insertText`). So a 140-character
  broker password pasted into either page is saved as its first 128 and the
  broker refuses the login. Without the attribute, the device answers 400
  "password must be at most 128 bytes", and since F241 both pages show that
  reason. Decide whether the password (and the host) keep `maxlength`, or drop
  it and let the device's refusal speak; the CA textarea carries none for that
  reason. From a Chromium probe; Firefox and Safari not probed. Found by
  F241's review (#1802).

- [ ] **F300 [code] canary-wap: about forty POST handlers still parse what one
  `httpd_req_recv()` returns.** F266 moved POST `/api/mqtt/config` to
  `http_body_reader.h`'s `read_all()`, as the Beacon routes read; 41
  single-read call sites remain (`canary_wap.ino` 19, `bluetooth_api.h` 8,
  `chirp_api.h` 6, `household_api.h` 3, `audible_chirp_api.h` 2,
  `csi_integration.cpp` 2, `rf_presence_api.h` 1), each parsing whatever the
  socket held and most cutting a longer body at its buffer without a word.
  Their bodies are small, so a split is rarer than for a pasted CA, but one
  that crosses a segment boundary is parsed cut. Give them one reader
  (`read_all()` behind a helper that answers its TOO_LARGE and READ_FAILED in
  each route's own error shape). From code; not probed. Found by F266 (#1802).

- [ ] **F232 [code+decision] canary-wap Chirp: unsigned presence beacons fill
  the nearby table and steer two gates.** `handle_presence()` adds a row for
  any session id a beacon claims, up to `MAX_NEARBY_CACHE` (32), and refuses
  new rows while full; a row lives three minutes after its last beacon. The
  table gates confirmations (`nearby_has_pubkey_with_presence()`: a confirmer
  with no row is dropped) and sets the community suppression threshold
  (`g_nearby_count / 2`, at least 3). So one device in range beaconing 32
  made-up session ids keeps real neighbors out of the table (their
  confirmations refused, so no chirp validates) and turns community
  suppression off (F233: from 18 rows on, no chirp can be suppressed). Decide
  what a presence row should cost (a beacon signed with the session key, as
  witnesses are; a per-sender-address cap; evicting the stalest row rather
  than refusing the newest) and whether the threshold should count only rows a
  signed frame confirmed. From code; the suppression effect host-probed, the
  confirmation effect not probed. Found by F213 (#1762).
  *Note (#1802, F233):* the threshold this item names is now
  `suppress_votes_needed()`: more than half of the rows, at least 3 and at
  most 17. The voter set now holds 17, so 32 made-up rows no longer turn
  suppression off. They raise the bar to 17 distinct verified votes, so fewer
  than 17 real neighbors cannot suppress anything while the table is padded.

- [ ] **F234 [code+decision] canary-wap Chirp: a suppress vote counts from any
  session key, with no nearby row.** `handle_suppress_vote()` checks the
  vote's signature and that the header's session id is the voter key's, but
  not that the voter has a presence row (a confirmation must), and a session
  key costs nothing to make. Host-probed (the same scratch test): votes signed
  by fresh keys suppress a witnessed `TPL_EMERG_FIRE_VISIBLE` chirp at a
  receiver with 0 or 6 rows at the third vote and with 17 rows at the eighth;
  a suppressed chirp leaves the recent table at the next prune (30 s), so `GET
  /api/chirp/recent` stops showing it. So one device in range can hide any
  chirp, an emergency template included, at every receiver in range with fewer
  than 18 rows, against spec §2.5.6's "legitimate content won't get
  mass-dismissed". Decide who may vote (a presence row, as for confirmations;
  a row a signed frame confirmed; F232's per-address cost) and whether an
  emergency template can be voted down at all. From code and a host probe.
  Found by F213's review (#1762).
  *Note (#1802, F233):* since F233, fresh-key votes suppress at every table
  size, not only below 18 rows. The number needed is more than half of the
  rows: the 3rd vote at 0 to 5 rows, the 4th at 6, the 9th at 17, the 17th at
  32. F233 now tests that a vote must verify (`a_vote_must_verify`: its
  signature and its session id), not who may cast one.
  *Note (2026-11-04, #1916, F311):* an ACK now counts only from a key whose
  nearby row is ten minutes old, but `handle_suppress_vote()` still counts a
  signed vote from any key, row or not, so more than half the rows and at
  least three votes from keys that never beaconed still remove a chirp. F305
  (#1916) closed the one-key path (a RESOLVED ACK); the vote path is still
  this item's decision. `docs/FLEET_SEMANTICS.md` §6 and §8 and spec §3.4 rule
  4 now say so.

- [ ] **F310 [code+decision] canary-wap Chirp: with no wall clock, a replay is
  stored again once the filter has forgotten it.** A receiver without a synced
  clock applies no freshness window, so once a chirp's row has left the table
  and the filter has forgotten its nonce (more than 331 s and at most 662 s
  after it was heard) the same signed frame is stored as new (spec §5.3 says
  so since F270). Hold the nonces of chirps stored here for as long as a row
  can live (30 min, 8 bytes each), or refuse unsynced witnesses; which is the
  decision. From code. Found by F270 (#1802).

- [ ] **F139 [decision] The PlatformIO tree's last-seen tombstones have no
  release and no opera scope.** `mesh_session.cpp` keeps a dropped peer's
  tombstone by fingerprint alone, re-applies it whenever that key is
  registered again, whatever the opera (after a rotation too, where the peer's
  old frames already fail the `opera_id` check), and parks its counter again
  at the next drop. So a device whose outbound counter went back while it kept
  its key (`mesh_out_ctr` lost, the identity key kept) drops at that tombstone
  after every re-pair until eight newer drops evict it. canary-wap since F116
  keeps a tombstone per fingerprint and `opera_id`, restores it only at a
  re-add into that opera, and releases it at a removal of a member not heard
  above it since its re-add, which reopens the window for that member. Decide
  whether the PIO tree does the same (either or both), or both trees keep
  their tombstones for good. Read from code, not probed. Found by F116
  (#1762).

- [ ] **F140 [code+decision] A removed member's frames signed after its
  removal still end canary-wap's COMPLETE resend at a re-pair into the same
  opera.** A removal is one-sided: the removed device keeps the opera and goes
  on signing heartbeats to its remover, above the last-seen tombstone the
  remover kept (F116), until its own owner removes the remover. One of those,
  recorded and replayed from its address at a re-pair into the same opera (a
  removal of the last member rotates nothing), is fresh and counts as the
  joiner heard, so the initiator stops resending the COMPLETE while the
  joiner, which lost it, times out. Host-probed by F116's review on the real
  `mesh_network.cpp` (a heartbeat at counter 6 above tombstone 5, and the gap
  frame of two owners who removed each other a minute apart, each ended the
  resend; in the second, B failed to `MESH_NO_OPERA` while A held B). Fix
  shape: end the resend only on a frame that proves the joiner holds this
  COMPLETE (an acknowledgement under the pairing session key, a wire change),
  or on a frame signed under the joiner's new pairing; the decision is the
  wire change. Found by F116's review (#1762).

- [ ] **F141 [decision] canary-wap on a board without flash encryption drops
  every member at every boot, keeps no tombstone for them, and does not
  implement spec §5.5 items 1-2.** `load_opera_config()` refuses on an FE-off
  board, so `init()` never calls `load_peers()`: every member leaves the table
  at every boot without passing through `retire_rx()`, and the next 5-minute
  save with no member removes `replay_ctrs`. A re-pair into the same opera (as
  a joiner of a member that kept it) then starts each member at 0, and its
  recorded frames are fresh again (F116's review, probed on the real code).
  Nothing in canary-wap gates pairing on flash encryption
  (`flash_encryption_enabled()` is read only in the opera-config and deny-list
  persist and load), unlike §5.5's items 1 (refuse to create an opera) and 2
  (refuse to consume a `PAIR_COMPLETE`). Decide whether canary-wap implements
  §5.5 items 1-2, retires the stored members' last-seen counters as tombstones
  at an FE-off boot (from `peer_<i>` and `replay_ctrs`, both unencrypted
  already), or both, or whether the spec changes. Read from code; spec §4.2
  and §12.3 and the threat model state the gap since #1762. Found by F116's
  review (#1762).
  *Since F137 (#1762, wave 12):* a canary-wap boot removes the member slots an
  older firmware left only with flash encryption on. On a board without it the
  stored members (`peer_<i>`, plaintext keys and radio addresses, and their
  fingerprints in `tx_ctrs` and, until the next 5-minute save, `replay_ctrs`)
  stay until the board's next membership change, on purpose, because one of
  this item's options reads them to retire each member's last-seen counter as
  a tombstone. Whatever is decided here also decides whether such a boot
  removes them afterwards (`test_mesh_liveness_wap`'s
  `a_board_without_flash_encryption_keeps_its_stored_members` pins today's
  answer).

- [ ] **F318 [code+decision] canary-wap: `pair/join` on a Canary that holds an
  opera still runs and keeps the old members.** `start_pairing_joiner`
  (`mesh_network.cpp`) does not check `g_opera_config.configured`, and the
  joiner's COMPLETE (`handle_pair_complete`) installs the new opera while the
  old `g_peers` stay, as F136 found on the PlatformIO tree. Refusing there
  (and answering 409 `opera_exists` where the sketch's handler now says 400
  `pairing_failed` for any refusal) makes the re-pair paths that 19 of
  `test_mesh_liveness_wap.cpp`'s 73 tests, one in `test_mesh_address_wap.cpp`
  and one in `test_mesh_commands_wap.cpp` drive unreachable: they re-pair a
  device that holds its opera, and canary-wap's leave broadcasts a LEAVE every
  member acts on, so Leave then Join adds a new member where they check a
  re-bind. Decide whether canary-wap follows the PlatformIO answer and those
  tests move to Leave then Join (then add the refusal test to
  `test_mesh_liveness_wap.cpp`), or a join into the opera already held goes
  through. Host-probed: the refusal was built and reverted (#1802).

- [ ] **F319 [decision] PlatformIO: since F136 a Canary pairs into an opera
  once.** A Canary in an opera cannot join a second member, and that member
  cannot join it (each holds an opera), so a PlatformIO Canary holds the
  member it joined through and the Canaries that joined through it; the old
  join paired it with further members of the same opera as a side effect.
  Alerts go to bound members only (F101) and nothing relays (spec section
  2.3), so in an opera built as B and C each joining A, B's alerts reach A and
  not C; and a changed radio address is re-bound at one member. Options: let a
  join into the opera the device already holds go through (the joiner learns
  the opera only from the COMPLETE, so this needs the opera named earlier, a
  wire change: the OFFER carries a name and a member count, no id), or a bind
  step between two members that needs no Leave. From code; not probed (#1802).

- [ ] **F297 [code+decision] canary-wap's Community tab offers BLE Discovery's
  Alert and Heartbeat in builds without FEATURE_BLE.** The DEV profile (the
  canary-wap-mobile env) sets FEATURE_BLE 0 as well as FEATURE_MESH_NETWORK 0
  (`build_config.h`), so since F276 its Community tab opens on BLE Discovery
  with no other view: it reads "Unavailable" and an empty nearby list (GATED's
  FEATURE_BLE entry), while its Alert and Heartbeat buttons post to
  `/api/ble/chirp/send`, which nothing registers there. Decide whether to hide
  the Community nav button when none of its three views is served, or to gate
  BLE Discovery's send card on `GET /api/ble/status` answering. From code; not
  run on a device. Found by F276 (#1802).
  *Note (2026-11-04, #1919):* since F296 reschedules BLE Discovery's refresh
  on every pass, a build without BLE (the DEV profile) asks its two unrouted
  reads, `/api/ble/status` and `/api/nearby`, every 10 s while the view is
  open, two 404s each time where it asked once. Whether that view should stop
  on an unrouted answer belongs with this decision.

- [ ] **F63 [code] canary-sense and canary-sentinel answer no serial `j`, so
  the in-browser flasher's identity card cannot show them.** HA17 (#1727)
  gave both a boot line with the full key, not the `j` self-manifest
  canary-vision answers (`firmware/common/attest/self_manifest.h`:
  `device_id` / `pubkey` / `pubkey_fp`), which is what the identity card
  reads. Sentinel reads no serial input, and Sense's serial input belongs
  to the tuning console, which has no identity command, so `j` needs a new
  input path on both. Which port that input would arrive on is F67's
  question, so settle F67 first. Found doing HA17.

- [ ] **F85 [decision] On the touch169 the Success hop rises into the
  halo's top arc.** The onboarding bird's scene seat stays inside the halo
  (F66, `scene_bird_top`), but the one earned hop puts its head into the
  arc's stroke at the apex on the 240x280 touch169. The hop is 12 px at the
  default Character, at most 14 for a shipped one and 15 under
  canary_mark's clamp of the temperament, plus the overshoot path's brief
  swing past that. The seat is at y 56 and the halo starts at 46. In the
  native harness the bird's top reaches y 43 at the apex, with 30 of its
  pixels on the stroke (18 under Heirloom). Either accept it (the hop
  crosses the halo for a moment) or seat the bird lower on that glass so
  the whole hop stays inside. Found by F66 (#1761).

- [ ] **F86 [decision] On the touch169 and the AMOLED: the halo's chord or
  the whole line.** F66 fits every centered onboarding line inside the
  halo's inner chord on rectangular small glass. That narrows a title from
  the 224 px row it had to 160 px (154 under Heirloom) on the 240x280
  touch169, and from 434 px to 314 (308) on the 450x600 AMOLED. So the
  touch169 now shows "Check your phone" and "No address" at both types,
  and the AMOLED shows "No address" (and "Check your phone" under
  Heirloom). The old rows held "Nice - check your phone" and "No address
  from the router" whole. It is the same truth in fewer words, but a
  450 px panel now drops "from the router" to keep a decorative ring's
  chord. Either keep it (the halo frames the scene's words), or on those
  two glass let the centered lines take the panel's row and size or drop
  the ring to clear them. `test_onboard_layout` pins where each shorter
  form shows, so any change updates it. See also F87. Found by F66
  (#1761).
  *Also (#1762):* the same trade now holds on the 800x480 glass (the dash
  line, dash7, nightstand7). F84 fits the scene lines inside the 300 px halo's
  chord (284 px at a title's latitude and 286 at the body's; 282 and 284 under
  Heirloom) in the small glass's faces (titles 24 px, 28 under Heirloom;
  bodies 16, 20). So "Check your phone" and "No address" show there at both
  types, and a network name wider than the body's chord
  ("Basement-Mesh-Extender-Office-5G", 296 px at 16) shows its head and tail
  around "..." where the panel's old 784 px row read it whole. One more option
  on that glass: sizing its halo with `halo_ring()` from its Join stack, as
  the small glass does, gives a 312 px ring at y 84 (306 at y 82 under
  Heirloom), between the Join title's row (to y 82; 80) and the credentials
  row (from y 398; 390), with a 296 px chord at a title (290 under Heirloom).
  There "Nice - check your phone" (295 px at 24) reads whole at the default
  type; under Heirloom (348 px at 28, 295 in the floor face) it does not, and
  "No address from the router" (331 px at 24) does not at either.
  `test_onboard_layout` pins where each shorter form shows on the wide glass
  too, so any change updates it.

- [ ] **F87 [decision] A title's ladder: the Character's face or the whole
  words.** `fit_line()` tries a line's forms in the Character's face first
  (the whole line, then its shorter form), and only then in the default
  Character's face; F45's rows and F50's coach lines use the same order.
  So under Heirloom the round watch shows "No address", though "No address
  from the router" fits its 222 px line in the default face (221 px), and
  the AMOLED shows "Check your phone", though "Nice - check your phone"
  fits its 308 px in the default face (295). For a title, the whole words
  may matter more than the face. Either keep one ladder for every line, or
  try a title's whole form in the floor face before its shorter form.
  Found by F65 (#1761).

- [ ] **F371 [decision] The round watch's splash tail now reaches into its
  bird.** F185 draws the speech bubble's tail on LVGL 8.4, so the round watch
  shows one for the first time, and F159 could not raise the watch's bird
  clear of it: a seat clear of the tail would lift the bird's hop off the 240
  px disc. In the native LVGL 8.4 render (`glass_splash_test.cpp`, read at
  every tick) the tail's ink stands up to 16 px into the bird's box at every
  first meeting under Heirloom (the watch's tallest bubble is 94 px there
  whatever the pseudonym) and hides as many as 13 of the bird's drawn pixels
  with "2222..." and 30 with the widest-lettered "WWWW..."; at the default
  type 4 and 12 px, hiding 0 and 1. Accept it, let the watch's hop graze the
  disc's top, draw its tail as the shorter triangle F246 renders (7 rows over
  the bubble, not 10), or seat its bubble lower where the disc's chord allows.
  The test prints it; `test_splash_layout` and `glass_splash_test.cpp` pin the
  watch as one of the two canvases not clear of the tail. Rendered natively,
  not on a watch. Found by F185 (#1836).

- [ ] **F372 [decision] The first meeting's raised bird stays raised through
  the wordmark beat.** Since F159 (#1836) the first meeting's bird rises clear
  of its tallest bubble, and `splash.cpp` keeps it there when the bubble bows
  out and the wordmark takes its 900 ms beat. In native LVGL 8.4 renders
  (`glass_splash_test.cpp` prints it on every play) the bird's ink now ends 69
  to 89 px above the wordmark's on the 172 px, 180 px and touch169 glass, 87
  to 125 px on the AMOLED (the most under Heirloom with "WWWW...") and 76 to
  78 on the dash under Heirloom with "WWWW...", where the seats before F159
  kept 60 to 67 on every portrait canvas and 67 to 70 on the dash; the round
  watch and the landscape nightlight, which do not rise, keep theirs. Accept
  it, or settle the bird back to its family's seat as the bubble bows out (an
  animated drop over the beat). Found by F159's review (#1836).

- [ ] **F373 [code] The 172 px glass's splash wordmark reaches both edges
  under Heirloom.** On the 172x320 nightstand the splash's "SecuraCV"
  wordmark, in Heirloom's title face with 2 px letter spacing, is as wide as
  the glass: in the native LVGL 8.4 render of the first meeting's wordmark
  beat (`glass_splash_test.cpp`, the S3 nightstand's config) its ink spans
  every column, 0 to 171, before F159 and after, so its first and last letters
  are cut at the panel's edges, and hello again shows the same label. The
  default type (12 to 161) and every other 8.4 canvas keep it inside, nothing
  holds the wordmark's box, and the C6 nightstand's lean ladder was not
  rendered. Fit it to the canvas (a smaller face, or no letter spacing where
  it would not fit) and hold its ink on the glass. Found by F183 (#1836).

- [ ] **F224 [human] No turned dash glass has been confirmed on real glass.**
  F205 turns LVGL 9.5's flushes and maps touch by arithmetic held on the host
  and in a native LVGL 9.5 run outside CI. On a dash, dash7 or nightstand7,
  confirm three things: that Portrait shows the face with its top along the
  panel's native left edge and Portrait, Flipped along the right; that a tap
  lands under the finger at all four turns (the GT911's axes against the
  panel's); and what a full-screen turned repaint costs (the portable
  `lv_draw_sw_rotate` loops read and write PSRAM column by column, 384,000
  pixels per full frame). If it is slow, the code options are turning straight
  into the RGB framebuffer (the second partial-mode example in LVGL's rotation
  docs, with the cache write-back Arduino_GFX does) or a smaller internal-RAM
  turn buffer. No row in `hardware_verification_checklist.md` names it yet.
  Found by F205 (#1762).

- [ ] **F370 [code] A phone turn under an open dash settings sheet leaves the
  sheet in the old canvas.** `/api/set orientation` only stores the choice;
  `main.cpp`'s `render()` turns the glass with `lvgl_port_set_rotation()` on
  its next pass whether or not the settings sheet is open, but nothing calls
  `settings_ui.cpp`'s `layout_host()` or rebuilds the open page
  (`settings_ui_tick()` rebuilds only Firmware and Network), so the sheet
  keeps the 800x480 layout on a 480x800 canvas (or the reverse) and an open
  Orientation page keeps its old check until the next navigation. The reset
  path in `settings_ui.cpp` already re-measures the canvas for exactly this
  reason (the review comment on its re-measure). From code, not run. Found by
  F245 (#1836).

- [ ] **F250 [code] `io/orientation.h` describes `Orient::R90` two ways.** Its
  classifier picks R90 when gravity pulls toward the glass's right edge, the
  device turned clockwise, and R90's own comment says "turned clockwise", but
  the same comment says "onto its left edge", and the enum's header says the
  value is how far the face must rotate clockwise; both describe the opposite
  turn. `display_1in47.cpp`'s landscape MADCTL bytes are also not yet
  bench-verified. The Lab's 3D case (A56) turns by the classifier's reading.
  Correct the comments, and check the bytes on a bench. Found by A56 (#1762).
  *Partly done (#1802):* the comments say one turn. `io/orientation.h`'s enum
  header and values now name each value by the turn the device made from
  upright, the way `step()` reads gravity in the native-portrait frame. R90 is
  a quarter turn clockwise onto its right edge (USB to the left), R270 is onto
  its left edge (USB to the right), R180 is USB up, and the face is drawn the
  opposite quarter turn on the panel. The classifier's own comment and
  `test_orientation`'s say the same. The header also names `pins.h`'s real
  macros (`IMU_AXES_SWAP_XY`, `IMU_X_SIGN`, `IMU_Y_SIGN`) and says the model
  takes the negated accelerometer reading. The emulator HAL (turn 1 is the
  device turned clockwise) and A56's scene (the case turned that many quarter
  turns clockwise) agree with it. Against `display_1in47.cpp` the item's
  premise held one level deeper. By that file's own model of MADCTL, rotation
  1's 0x28 draws the face upright for a device turned counter-clockwise, and
  rotation 3's 0xE8 for one turned clockwise: the old enum header and "onto
  its left edge" described what the bytes draw, and the classifier names the
  other turn. What the glass shows also rests on the IMU's signs, which nobody
  has measured. The classifier wants the gravity vector, but an accelerometer
  at rest reads +1 g along the axis that points up, and `imu_read_accel()`
  passes that on unnegated. So if `pins.h`'s identity seed has the chip's
  mounting right, standing upright commits R180, and a clockwise turn commits
  R270, which 0xE8 draws upright. With that seed, only flipping both IMU signs
  and swapping the r1/r3 bytes makes the glass, the classifier, the emulator
  HAL and the Lab's 3D case agree. Flipping `IMU_Y_SIGN` alone reads upright
  in every pose but makes R90 the counter-clockwise turn.
  `display_1in47.cpp`'s note now says so, and
  `hardware_verification_checklist.md` gains the bench row. An IMU commit logs
  nothing to serial (F285), so the row reads `"orientation"` from `GET
  /api/settings` after each pose: 0 upright, 1 clockwise, 3 counter-clockwise,
  2 upside down. Fix `pins.h` until each pose commits its value, then judge
  the bytes (F282). Open: the IMU mapping and the MADCTL bytes need the bench
  (U1); no byte changed. Not bench-tested.

- [ ] **F282 [code+human] The nightlight's IMU sign convention and its MADCTL
  table must be settled together.** By `display_1in47.cpp`'s own model of
  MADCTL, rotation 1's 0x28 draws the face upright for a device turned
  counter-clockwise onto its left edge, and rotation 3's 0xE8 for one turned
  clockwise. `io/orientation.h`'s classifier asks for rotation 1 when gravity
  pulls along +x, the device turned clockwise. The classifier wants the
  gravity vector, but an accelerometer at rest reads +1 g along the axis that
  points up, and `imu_read_accel()` and `main.cpp` pass the reading on
  unnegated. So if `pins.h`'s identity seed has the chip's mounting right,
  both in-plane signs come out reversed. Standing upright commits R180, drawn
  upside down. A clockwise turn commits R270, which 0xE8 draws upright. With
  that seed, only flipping both IMU signs (in `pins.h`, or one negation in the
  glue) and swapping the r1/r3 bytes makes the glass, the classifier, the
  emulator HAL and the Lab's 3D case agree. Flipping `IMU_Y_SIGN` alone reads
  upright in every pose but makes R90 the counter-clockwise turn, the opposite
  of the emulator and the Lab, and then a side turn's tumble enters from the
  edge that was down. Read from source; not seen on a device. U1's new
  checklist row reads `"orientation"` from `GET /api/settings` after each
  pose. Fix the IMU mapping until each pose commits its value, then the bytes;
  this is F250's open half. Found by F250 (#1802).
  *Note (#1802, F285):* each orientation commit now writes `[orient] <who> ->
  rotation R (was P)` to serial, so U1's orientation row reads that line from
  a serial monitor, or `"orientation"` from `GET /api/settings`.

- [ ] **F283 [decision] Pay the nightlight tumble's Startle at the landing, or
  keep it with the handling?** Since F242, `nightlight_ui_update()` pays the
  Startle on the face's first render after the turn. So the hop (240 ms up,
  peaking at 195 ms) runs while the 700 ms slide is still bringing the bird
  in. Natively on LVGL 8.4 (`runtime_turn.sh` prints it per turn), the whole
  bird is on the glass in only 8 of the hop's 115 frames when it enters over
  the landscape glass's right edge, 56 when it falls onto the landscape glass,
  and 62 when it enters from either side of the portrait glass. Today the bird
  arrives eye wide and a little high and settles onto its perch. The
  alternative is a one-shot timer about 300 ms after `nightlight_ui_tumble()`:
  still after the first mood, still refused asleep, hidden or rationed, and
  inside the 1.4 s window. Decide which reads as the honest reaction. If it is
  the landing, hold it in `test_nightlight_tumble` and in `runtime_turn.sh`
  with the apex inside the glass. Found by F242's review (#1802).
  *Note (#1802, F279):* `runtime_turn.sh` now turns 7 s after power-on, since
  F279 lets the first Startle through, and its hop reads differently there:
  the apex at 185 ms, and the whole bird on the glass in 0 of the hop's 113
  frames when it enters over the landscape glass's right edge, 54 when it
  falls onto the landscape glass, 60 from either side of the portrait glass,
  and all 113 otherwise. The counts move with where the hop falls against the
  render tick; the question is unchanged.

- [ ] **F298 [decision] The first meeting's scripted Tilt and Startle beats
  share the mark's 8 s ration.** `splash.cpp` sends kHello's Tilt beats and
  its Preen beat (a Startle) through `canary_mark_react()`, which lets one
  Tilt or Startle through per 8 s. Since F279 a unit's first meeting at
  power-on draws the opening Tilt at 940 ms, refuses the "Nothing."
  punchline's Tilt 7.4 s later, and plays the Preen beat's Startle at 9.72 s
  and the "Dark means all is well" Tilt at 20 s (`test_splash_scenes` prints
  it); before F279 it drew the punchline's Tilt (8.32 s) and the 20 s one
  only. Decide whether a scripted story beat should pass the ration (it exists
  to keep organic reactions rare, and the script is authored), or the script
  should space its reactions 8 s apart, and hold the chosen schedule in
  `test_splash_scenes`. Found by F279 (#1802).

- [ ] **F299 [decision] `story::kHelloAgain` is authored but nothing plays
  it.** The returning hello (Enter, "Hello again." with a Tilt, Settle) lives
  in `firmware/common/story/story_scripts.h` and `test_story.cpp` lists it,
  but `splash.cpp`'s returning boot hops (Happy) and shows the Character's
  `hello_again` in the tagline, with no bubble and no Tilt, and no Lab page
  plays it either. F279's item counted its Tilt among the reactions the ration
  refused; none was ever sent. Decide whether the returning splash should play
  the scene (it would need the bubble, against "a returning boot must never
  feel slower than home") or the scene should go. Found by F279 (#1802).

- [ ] **F67 [code] The C6 builds most likely send `Serial` to UART0 on the
  radar's pins, not to USB.** `firmware/envs/platformio/canary-sense.ini`
  :78 and `canary-sentinel.ini` :67 add `-UARDUINO_USB_CDC_ON_BOOT` (so do
  `canary-display.ini` :700, for the C6 nightstand, and `common.ini` :76,
  for `common_esp32c3`). PlatformIO's `ProcessFlags` appends every `-U`
  after the `-D` list, and its `CPPDEFINES` removal compares the bare name
  with a `(name, value)` tuple. So the flag cancels the board's
  `-DARDUINO_USB_CDC_ON_BOOT=1` (`seeed_xiao_esp32c6.json` in pioarduino
  55.03.38) instead of being skipped. Arduino-ESP32 3.3.8's
  `HardwareSerial.h` (:426-440) then defines the macro as 0 and maps
  `Serial` to `Serial0`, which is UART0, on RX GPIO17 / TX GPIO16 on the C6
  (:160, :180). `HWCDC.h` (:110-115) declares `HWCDCSerial`, the USB-C
  port, only when CDC-on-boot is 1. Both XIAO C6 `pins.h` files put the
  radar's UART1 on TX16/RX17 ('UART0 stays on USB-CDC console'), and
  Sense's `main.cpp` calls `Serial.begin()` (:677) before
  `RadarSerial.begin()` (:735). So the Sense boot log, its tuning console
  and the `Ed25519 pubkey` line HA17 added most likely go to header pins
  D6/D7, and UART0 and UART1 contend for the radar's pins.
  `scripts/lint_usb_console.py` and RELEASE_LESSONS (q) state the opposite
  rule for C3/C6 ('they provide `Serial` on their own, and the S3 flag
  prevents it'), so no gate notices. This is read from the installed core
  headers and PlatformIO's source, not seen on a bench. The C3 envs on core
  2.0.x were not checked. Fix, after a bench read on a XIAO C6 (U1): settle
  which flag the C6 (and C3) builds need, then change the envs, the release
  FQBNs and `lint_usb_console.py`'s rule together, and drop the port caveat
  from `docs/device_trust.md` and Step 6 of `docs/homeassistant_setup.md`.
  Found reviewing HA17 (#1727), which filed it as F62; renumbered when
  `main`'s F62 (the desktop clients' mDNS TLS advert, #1757) merged first.

- [ ] **F153 [decision] Should canary-vision's dwell and visit lengths end at
  the last sighting?** `dwell_ended`'s `dwell_ms` (`now_ms - dwell_start_ms_`)
  and `visit_ms` (`now_ms - presence_start_ms_`) are taken on the frame that
  declares the person gone, so both include the lost timeout: 1.5 s by
  default, 4 s in the `litter_box` preset, up to 60 s. Taking them from
  `last_seen_ms_` would report how long the person was seen; today they report
  how long the device held them present, and the README says so since F130. A
  change moves the device rows, the Lab core (the emulator dist),
  `test_vision_presence_fsm.cpp`, `test_vision_core_bindings.cpp` and the HA
  recipes that print `visit_ms`. Found by F130's review (#1762).

- [ ] **F312 [decision] Whether the Sense's held occupancy count should decay
  while Present.** Since F131 a frame that names no count keeps the bucket
  while Present, so a room that keeps reporting a target whose count frames
  stop arriving (or arrive as 0) holds its last bucket, 2+ included, until the
  clear timeout or a stall; and Present entered on a target frame whose count
  is still 0 reads 0 occupants until a count frame lands (the 2026-07 audit's
  unverified `TARGET_COUNT`). Options: keep holding (today), decay a held 2+
  to 1 after a window, or read One whenever Present names no count. A bench
  capture of the MR60's real frame order settles which matters. Found by F131
  (#1802).

- [ ] **F252 [code+decision] canary-wap's rf-sensing wizard is unreachable:
  the Phase 12 routes were never built.** The Phase 10 setup wizard
  (`wizard.h` / `wizard.cpp` in
  `firmware/projects/canary-wap/arduino/canary_wap/`) compiles into the
  sketch, but nine of its public functions have no caller outside
  `wizard.cpp`: `set_zone_name`, `get_zone_name`, `start_pairing`,
  `finish_pairing`, `restart_training`, `always_ignore_last_decision`,
  `set_context`, `get_status` and `get_status_for_export`. Only
  `init`/`deinit`/`tick` (from `rf_presence.cpp`) and the Phase 11 conformance
  self-test, which restores the state it changes, call into it, so on a device
  the wizard never leaves `WIZ_UNCONFIGURED`, and the six modules' noised
  `*_for_export` paths (`dp.h`) are reachable only through the uncalled
  `get_status_for_export`. `rf_presence_api.h` registers only
  `/api/rf/{status,settings,conformance,enable,disable,rotate}`; the
  `/api/rf/wizard/*`, `/api/rf/notify/*` and `/api/rf/self-test` routes of
  `docs/rf_sensing_phase12_http_routes.md` §4 (Draft v0.1, 2026-04-25:
  "Implementation is a follow-up PR") do not exist. `wizard.h`, `dp.h` and
  that doc's status line say so since the 2026-10 cleanup. Decide, then do
  one: (a) build the spec, a `rf_wizard_api.h` beside `rf_presence_api.h`
  registering the §4 routes behind `auth_gated<>`, the handler-count constant
  in `canary_wap.ino` raised, the SPA wiring and the gzip asset regen, keeping
  the spec's §8 acceptance list; or (b) delete the nine functions and the
  `*_for_export` functions of the notify, baseline, household, familiar and
  federated modules, keep `dp.h` only if something else draws through it, and
  mark the Phase 12 doc withdrawn. Checks:
  `firmware/scripts/regression_check.sh`, the canary-wap builds,
  `make -C firmware/tests_host` and `test_dashboard_route_match.test.js`.
  Found by the 2026-10 cleanup audit.

- [ ] **F292 [decision] `CAN_BITRATE_DEFAULT` is read by no code.** It is
  500000 in `firmware/boards/waveshare-esp32s3-lcd43b/pins/pins.h`, but the
  playground's CAN station asks for 500 kbit/s itself on both transports
  (`can::BITRATE_500K`, `TWAI_TIMING_CONFIG_500KBITS()`), and the activation
  checklist used to tell benches to change the define for another rate. Either
  have both station paths take the rate from it (the inline path needs
  `can_bus.cpp`'s rate-to-preset map) or delete it. Found by F253 (#1802).

- [ ] **F293 [decision] Three dev-mode display envs poll for the plain dash's
  OTA image.** dash-b, dash-rs485 and dash-can carry `FEATURE_DEVMODE` and the
  4.3B pin map but pass `SECURACV_OTA_PRODUCT="securacv-canary-display-dash"`,
  the plain 4.3 image's product, which `include/canary/config.h`'s own rule
  says a gear-carrying image must never do (its derived identity for such a
  build is `securacv-canary-display-dash-modes`). None of the three is in
  `release_envs`, but an OTA poll from one would accept the plain-dash image
  and drop both the bench and the 4.3B pins. Decide whether these envs take
  the modes product (or a 4.3B-specific one) or stop polling. Found by F253
  (#1802).

- [ ] **F366 [code+human] A Dash or Dash-modes unit installed from a flasher
  can never update over the air.** Both release workflows build those two
  products with arduino-cli under one `DASH_FQBN` whose partition scheme is
  `huge_app` (`firmware-release.yml` ~line 454, `flasher-release.yml` ~line
  397): a single app slot, no second slot to write an update into. The
  factory image the browser flasher installs carries that table (bootloader,
  partition table and app in one file), so every such unit polls a signed
  `manifest-canary-display-dash.json` / `-dash-modes.json` it can never
  install. The PlatformIO env builds the same board on `default_16MB.csv`
  (A/B `0x640000` slots), but `flavors.json` declares no `size_guards`
  budget for `canary-display-dash` or `-dash-modes`, so the release's
  `check_slot_budget.py` measures neither image against any slot.
  Pre-existing since fw-v2.4.1, the first release whose workflow carries
  `huge_app`. Work: build both with an A/B scheme that matches the
  PlatformIO env, in both workflows at once (the two `DASH_FQBN` lines must
  stay identical); declare both budgets in `size_guards`; and put a notice at
  the top of the next CHANGELOG section, as 2.4.16 did for the C3 Visions:
  a partition table changes only over USB, so every Dash and Dash-modes unit
  installed from a flasher needs one USB reflash. Bench (U1): one OTA on a
  reflashed unit. Found by the 2.4.16 release audit (2026-10-10).

- [ ] **F367 [code+decision] Three OTA images are close to filling their
  slots.** Measured on the fw-v2.4.16 release assets against `flavors.json`'s
  `size_guards`: `canary-display-nightstand-c6` is 2,028,784 B in a
  `0x1F0000` (2,031,616 B) slot, 2,832 B of headroom after growing 29,856 B
  since 2.4.15. A release that grows it by more than those 2,832 B fails
  `check_slot_budget.py`, and the display loop in `firmware-release.yml`
  then publishes no OTA manifest for it, so C6 nightstands stop updating
  until a release fits. `canary-sense` (1,821,632 B, 92.65%) and
  `canary-sense-wellbeing` (1,825,472 B, 92.85%) sit in `0x1E0000`
  (1,966,080 B) slots, each about 373 KB larger than in 2.4.15 (2.4.16 added
  MQTT over TLS and the Bluetooth setup door to them). A larger table is
  no answer for units already in the field (a partition table moves only over
  USB; see the 2.4.16 Vision notice in `CHANGELOG.md`), so the room has to
  come from the images: decide what the C6 nightstand build drops or puts
  behind a flag (largest contributors first, from its linker map), and make
  the slot check warn below a headroom floor (5% free, say) so the next
  squeeze shows up before a product is skipped. Found by the 2.4.16 release
  audit (2026-10-10).

- [ ] **F413 [code] canary-display: the watch's boot log offers a lantern it
  does not build, and the factory reset's comment keeps a witness identity a
  display does not hold.** `src/main.cpp:1394-1401`'s two `boot_kv("Button",
  ...)` strings say "double = lantern" inside `#if BOOT_BUTTON_PIN >= 0`, with
  no `FEATURE_LANTERN` gate, so the round watch, which builds no lantern,
  prints a gesture it does not have (D14, #1916, corrected its pin map). And
  the 10 s hold's comment (`:2073-2088`) says the hold leaves "the witness
  identity ... this unit's birth certificate"; a display holds no witness key
  (`mqtt_mgr.cpp:635`, `glass_web.cpp:897`, `:927`), and what survives is the
  device id, the pseudonym salt and the `cvtrust` pins, since
  `forget_wifi_credentials()` removes only the credentials, the ring and
  `ap_pass`. Gate the lantern words on `FEATURE_LANTERN` and say what stays.
  `main.cpp` is an emulator input, so the change is a dist rebuild (pinned
  emsdk) for every display flavor. From code. Found by D14 (#1916).

- [ ] **F430 [code+decision] No item holds the general identifier lint
  (device-details PR 4).** F384 (#1892, partly done there and in #1916) left the identifier
  lint to device-details PR 4: a lint over every product's sources that denies
  reading or publishing a per-unit identifier (the Grove Vision AI module's id
  through the SSCMA library's `ID()`, and the classes the design's privacy
  review names). `desktop_parity.test.js`'s Vision-only `AI.ID(` deny
  (`sscmaIdCalls()`, F384, F414) is the one case that exists. The design,
  `docs/design/device_details.md` (F384 cites its privacy review, finding
  P11), is not on main, so the lint's scope (which identifiers, which
  products, where it runs) is written down nowhere a reviewer can read. Land
  the design or have a maintainer state the scope here (the decision), then
  build it as an unfiltered `scripts/lint_*.py` in `lint.yml` with the F384
  deny folded in. From the ledger. Found by F414 (#1919), whose title named
  the gap.

- [ ] **F433 [decision] F323's race test: should a pass also see a full
  history read between live stores?** Since F323 (#1919) the writer waits
  until the reader has read the history full, but the read it waits for may be
  taken while the writer is parked in that wait. On one core each of the
  reader's timeslices sees one frozen history, so a passing run can hold no
  read of a full history between two live stores: in a phase-tagged scratch
  copy pinned to one core beside 7 busy loops at -O1, one of 12 passing runs
  had 0 of 17,292 full reads after the wait, and a reviewer saw 0 of 33,808.
  The old assert, when it passed, did prove such a read, and failed on
  scheduling alone when none came. On CI's multi-core runners the reads run
  beside the stores, and on one core neither version guarantees a read that
  overlaps a store mid-copy. If a pass should require one, bound the run by
  phase-tagged full reads after the wait, under the same deadline (or have the
  writer yield once per 40-step window); either changes the test's character
  beyond F323's brief. From host runs. Found by F323 (#1919).

- [ ] **F434 [code] The canary-wap host suite is clean only under CI's serial
  g++ -O2.** Built with `CXX=clang++-18`, about 20 `tests_host` targets fail
  to build: the GCC-only `-Wstringop-truncation` pragma and flag
  (`chirp_host_harness.h` and others) and `-Wnested-anon-types` in
  `stubs/bt/NimBLEDevice.h`. Built with g++ `-O1
  -fsanitize=address,undefined`, libstdc++'s `<regex>` trips
  `-Wmaybe-uninitialized` in five suites, UBSan reports misaligned member
  access at two places in `mesh_network.cpp`, and LeakSanitizer a leak in
  `test_mqtt_reinit`; all of these were so before #1919. One `make -j8` run on
  a loaded machine also failed `test_loop_command_ring`'s
  `test_withdrawals_racing_the_drain_never_run` (`!late.load()` and
  `unfinished == 0`) where the serial build passed it, so it may be
  timing-sensitive under load. Make the suite build under clang++ and the
  sanitizers, fix or explain the two UBSan sites, and look at the ring test
  under load. From host runs. Found by F404 (#1919).
  *Note (2026-11-04, #1920):* wave 23's packages met the same three faults and
  built around them; none is fixed. The GCC-only `#pragma GCC diagnostic
  ignored "-Wstringop-truncation"` sits at `chirp_host_harness.h:82`,
  `test_chirp_commands_wap.cpp:152`, `test_chirp_suppress_wap.cpp:121` and
  twice in `test_bluetooth_commands_wap.cpp` (clang++ 18 `-Werror` rejects an
  unknown warning group; a `#if !defined(__clang__)` guard closes it), and
  `-Wnested-anon-types` fires on the anonymous structs inside
  `stubs/bt/NimBLEDevice.h`'s `struct ble_store_status_event` (name them, or
  guard them as the real header does); the packages ran clang with
  `-Wno-unknown-warning-option -Wno-nested-anon-types`. The two UBSan sites
  are `mesh_network.cpp:1343` (`TamperAlertPayload`, `test_mesh_commands_wap`)
  and `:1162` (the rekey payload, `test_mesh_liveness_wap`), misaligned member
  access on a payload read in place from the receive buffer; `memcpy` it out.
  The flagship's `firmware/tests_host` has its own clang list (F441).

- [ ] **F438 [human] The ESP32-S3 reads its die sensor for the first time:
  bench-validate every consumer.** Since F432 (#1920) `securacv_thermal.cpp`
  includes `sdkconfig.h` before it tests the chip target, so its TSENS branch
  compiles on the S3 (dev and release on IDF 4.4.7 through
  `driver/temp_sensor.h`; `[env:full]` on IDF 5.5.4 through
  `driver/temperature_sensor.h`) and `thermal_read_die_c()` answers on stock
  units for the first time. Everything downstream changes behavior at once and
  none of it has met a real die: the camera step throttles on real 70/80 C
  readings instead of fail-safing at boot; the thermal watchdog
  (`securacv_thermal_watchdog.cpp`) stops logging "Thermal sensor not
  responding" and starts counting throttle and critical episodes; the envsens
  temperature-drift tamper detector arms (`FEATURE_TEMP_TAMPER=1` on dev and
  release) and feeds `sensing_feed_temp_drift_event`; the self-test's
  `test_temp()` passes and the health score rises; the MQTT health body
  carries `temp_c` (Home Assistant shows it as an attribute) and the fleet
  self-report its `temp_c`; the dashboard's thermal card reads "running"
  rather than "conservatively, not blind". The host fakes keep the pinned
  headers' shapes and prove nothing about the hardware. On a XIAO ESP32S3
  Sense (U1): watch `/api/thermal`, the envsens tamper counter and the camera
  state over a PEEK session, and read the health `temp_c` against the room,
  before this reaches a release train; the S3 PlatformIO legs on #1920 are the
  compile proof. From code; not bench-tested. Found by F432's review (#1920).

- [ ] **F441 [code] `make -C firmware/tests_host CXX=clang++-18 run` does not
  complete.** The whole-tree clang++ 18 run stops on
  `-Wno-stringop-truncation` (a GCC-only flag, unknown to clang under
  `-Werror`) and, with `-Wno-unknown-warning-option` added, on six suites:
  `test_csi_daily_summary`, `test_csi_module_boot` and
  `test_csi_modules_integration` (`-Wunused-const-variable` in `common/csi`'s
  `core_presence.cpp` and `anomaly_baseline.cpp`),
  `test_vision_detection_pipeline_ubsan` (no clang UBSan runtime on the host)
  and `test_vision_presence_fsm{,_grace}`. Wave 23's new suites pass under
  clang one by one, so "again with `CXX=clang++-18`" means a per-suite build
  today. Make the gate per suite or fix those six, so the clang run means the
  whole suite; the canary-wap suite's list is F434. From host runs on
  `653073f7`. Found by F421's package (#1920).

- [ ] **F442 [code] A lint for a chip-target macro tested before `sdkconfig.h`
  is included.** `securacv_thermal.cpp` was the one firmware file that tested
  `CONFIG_IDF_TARGET_*` or `ESP_IDF_VERSION` before any include could have
  reached `sdkconfig.h` (a scan of `firmware/` finds no other; canary-wap's
  `mesh_crypto.cpp:36` is a comment), and that made the sensorless stub the
  S3's provider for every release so far (F432, #1920; `LESSONS_LEARNED.md`'s
  entry). `test_camera_thermal.cpp` pins this one file's include order. A
  small `scripts/tests` check over every `.cpp`, `.h` and `.ino` under
  `firmware/`: the first `#if` on `CONFIG_IDF_TARGET_*` or `ESP_IDF_VERSION`
  must follow an include of `sdkconfig.h`, `Arduino.h` or an `esp_*`,
  `driver/` or `freertos/` header, with a self-test that plants the old
  provider. A new gate, not a review fix. From a scan. Found by F432's review
  (#1920).

---

## 2. Apps (desktop Flasher, Lab, iOS, tvOS)

- [ ] **A5 [code] Lab mDNS + BLE discovery** (HTTP-poll only today) and the
  menubar companion with the signed timeline — `desktop-lab/README.md`
  Roadmap items 2–3. *Partial — mDNS and the companion landed (#1704):*
  `desktop-lab/src-tauri/src/fleet.rs` is a desktop-only twin of the
  Flasher's `fleet_scan` (browse only; `desktop_parity` holds
  `SERVICE_TYPE`, `FleetSighting`, `fleet_scan` and `scan_blocking` equal),
  and both apps' Witness Walls now feed browsed boards to the `/api/fleet`
  poll after the kernel addresses (a board answers with a one-board
  self-report, so trying it first would replace the kernel's fleet),
  saying "N Canaries announced on this network — none serves the fleet
  document yet" when boards announce but nothing serves it; `MOBILE.md` no
  longer claims iOS mDNS/BLE discovery. The companion (`companion.rs`) is a
  Rust-driven tray ("N of M online") with native notifications on fleet
  changes — coarse words from the fleet's own report only, never
  "verified", never sealed-log or wellbeing content, and silence never
  notifies — so the `notifications` capability is now true on desktop. On
  macOS it keeps running after the window closes; on Linux it runs while
  the Lab is open (the .deb now depends on libayatana-appindicator3-1, and
  a missing tray library no longer crashes the launch). A browse that
  finds a real Canary, the tray on a real Linux desktop and macOS menu bar,
  and notifications from an unsigned macOS bundle are human passes.
  *Still open:* BLE discovery (skipped: it needs a Mac, for the Info.plist
  Bluetooth entry, and a BlueZ desktop to validate; the shared beacon
  vector fixture and btleplug seam are their own PR) and the companion's
  signed timeline (skipped: it needs a viewer token and a pinned key, A2's
  pairing — without them the only honest render is the unverified fleet
  report the companion already gives).

- [ ] **A8 [human-gated by U3] Exercise the CloudKit household/away path.**
  `HouseholdShare.swift` degrades to a no-op in unsigned builds, so CI has
  never touched iCloud. Needs a signed build (U3), then a test pass.

- [ ] **A9 [human] Windows enablement.** The hub-io raw-disk write backend is
  complete but staged off pending a VM/hardware pass
  (`desktop/hub-io/src/write.rs`, "STAGED — NOT YET ENABLED"), and
  `secret_store.rs`'s Windows branch has never been compiled by CI.

- [ ] **A13 [human-gated by U3/certs] macOS signing/notarization** — both Mac
  apps ship unsigned until `ENABLE_MACOS_SIGNING` + certs exist
  (`desktop-lab/README.md`, `desktop/INSTALL.md`).

- [ ] **A15 [human] On-device pass for the iPhone snapshot flow (A6, A7).**
  CI builds unsigned and runs the simulator, which has no Secure Enclave,
  so neither item has run on a phone. On a physical iPhone with a passcode:
  create a key and see "Secure Enclave" custody; one Face ID or passcode
  prompt per unseal, and canceling it leaves no frame; a file sealed to
  another key raises no prompt; forgetting the key deletes the Enclave
  key; removing the passcode disables the key, as documented. Then a real
  canary-wap seal followed by an unseal on the phone, a `.svlt` opened from
  Files or Mail, and the app-switcher snapshot blank while a frame is up.

- [ ] **A18 [code+decision] The Wall cannot walk an add-on or root-image
  install.** The Home Assistant add-on honors `/config/viewer_tokens.json`
  but has no control that mints a viewer token (running `witness_api`
  inside the add-on needs its config and seed, which the Supervisor offers
  no path to), and the root `witnessd` container image ships no
  `witness_api` binary. Both are documented limits: those installs show
  the Wall the roll-call, never a walk. Decide where minting lives, then
  build it.
  *Partly done (#1802), the root-image half; the add-on half is open:* the
  root `witnessd` image now ships `witness_api` beside the seven operator
  tools (the release build already compiled it; its `[[bin]]` has no
  required-features), and `container-images.yml`'s smoke step checks it like
  every shipped binary and runs `witness_api help`, which must exit 0 and name
  `mint-viewer-token`. `docs/container.md` gives the mint path, `docker exec
  <container> witness_api mint-viewer-token --label <s>`: exec inherits the
  container's `WITNESS_API_*`, `WITNESS_CONFIG` and `DEVICE_KEY_SEED`, its
  working directory and its user (UID 1001 unless `docker run --user` named
  another, the user `witnessd` writes `/data` as), so it finds the seed beside
  the database, and `witnessd` re-reads the viewer file per request, so
  neither a mint nor a revoke (`revoke-viewer-token <id>`) needs a restart,
  provided the container started with `WITNESS_API_TOKEN_PATH` (or the viewer
  path); without one `witness_api` refuses to mint. The page names three ways
  to break it: `exec -u root` (a root-owned `0600` file that `witnessd` and
  later mints cannot open; the page gives the `chown` that undoes it), `exec
  -e` pointing a path elsewhere, and a container started with a
  `WITNESS_CONFIG` written for `witnessd` (`witness_api` refuses its unknown
  keys, A103). It also says a bare `witness_api` is the API daemon, and that
  an image pulled before this change has no `witness_api`. Decision (a default
  for a maintainer): minting lives in `docker exec`, with no new port and no
  new runtime dependency. Not built: the add-on panel's mint button, which
  waits for a maintainer's yes (HA34's `serve_wizard.py` change, which it
  would sit beside, is now on #1802). Both jobs of `container-images.yml` are
  pinned to `ubuntu-24.04` (CI11). The image build and smoke run are proven
  only by `container-images.yml` in CI; the mint flow on a running container
  is not exercised; a stand-in run of the smoke script fails on the old
  Dockerfile and passes on the new one. The kernel claims come from reading
  the code; cargo was not run. Found here: A103, A104, A105.

- [ ] **A19 [decision] The kernel's auth lockout also closes `/api/fleet`
  and `/health`.** The per-address lockout is checked before routing, so a
  tokenless poller that trips it locks itself out of the roll-call too. The
  Wall now asks a refusing source once per session and stays clear of it;
  other tokenless pollers still can lock themselves out. Decide whether the
  lockout should cover the unauthenticated routes.

- [ ] **A33 [decision] `docs/homeassistant_setup.md`'s MQTT Topic Reference
  reads as one product's contract, and it is not the WAP's.** It names
  `securacv/{device_id}/availability` (an online/offline LWT), which
  `firmware/canary/lib/securacv_mqtt` publishes and canary-wap does not; the
  WAP's LWT and online flag ride `status`. It describes status as "Device
  state, GPS, chain sequence". The Hub page lists that table as the topic
  contract (`gen_homeassistant.py` parses it into `topics`). Step 2 of the
  same guide says each Canary always advertises its unique hostname, but
  firmware/canary's `securacv_network.cpp` advertises the constant `canary`
  and relies on RFC 6762 renaming. Decide whether the table is per product or
  the common subset, and say which; A36 is the same question for the Hub
  page's demo. Found by A27 (#1762).
  *Since A36 (#1762):* the Hub demo shows the WAP's own discovery entities,
  so the demo is per product. The guide's table is still this item's call.

- [ ] **A34 [decision] The Sense Lab page's fingerprint is not the repo test
  key's.** The WAP page, the Hub page and the Vision pane show the seed-0x42
  key (fp `7916ca487912fa1b`). The Sense page shows `b7e2c49a11f03d5c`, which
  no derivation produces, though canary-sense derives its fp the same way.
  Decide whether every Lab key example is the test key's. If so, move
  `gen_sense.py`'s `EX_FP` (and an elided health key) to it and add the Sense
  page to `fingerprint_examples.test.js`'s test-key check. Found by A26
  (#1762).

- [ ] **A38 [decision] The WAP Lab page shows sensing cards and a pill
  vocabulary the canary-wap does not have.** `gen_wap.py`'s dashboard cards
  Touch (silent panic, enclosure tamper), Appliance activity (IR) and Thermal
  drift, and its pills (Quiet / Presence / Motion / Active / Offline), come
  from `docs/getting_started_canary.md` §3 and §6-8. The canary-wap sketch has
  no touch pad, IR receiver or die-temperature drift code, and its dashboard
  names its states Empty, Subtle motion, Quiet, Active and Together
  (`csi_dashboard_html.h`). The two vocabularies collide: the firmware's
  `quiet` is a still, breathing person, the guide's Quiet an empty room. The
  sandbox's presence scenes now say the firmware's word in their console line
  and event, keep the guide's pill as an explicit per-scene field, and name
  both in each blurb (A30). The boot log's runtime table (`WEVT` rows, one in
  state `motion`, which `core.presence` never emits) is not a table the sketch
  prints. Decide whether that guide describes the WAP or firmware/canary, then
  trim the page's cards, pills and table to the WAP's. See A33. Found by A30
  (#1762).

- [ ] **A43 [decision] The display flavors have no stand-in built from the
  tree's sources.** A display firmware change that a Chromium probe checks
  (`boot_probe.mjs`, `onboard_probe.mjs`, `bench_probe.mjs`, `csp_probe.mjs`)
  is proven only after CI rebuilds `dist/canary-display-*.js`, which links the
  firmware, LVGL and the browser shims into one page under Asyncify; A40's
  pipe cannot stand in for a page. Decide whether a host build of the display
  firmware's page-checked logic is worth having, or whether CI's rebuild stays
  the only path. Found by A40 (#1762).
  *Merged here (#1762, wave 13), from F184's review:* a native stand-in is
  cheap. F184's review built `build.sh`'s whole dash translation-unit list
  natively (the real `main.cpp`, the emulator's `src/` and `shim/`, LVGL 8.4,
  Ed25519) with g++ in seconds; a driver standing in for the page (the EM_JS
  calls as C functions, a virtual clock) walked the first-boot portal in about
  2 s per run and evaluated the probe's own holds on dumped framebuffers and
  bindings, catching each of nine mutations. Committed and run in the page
  logic tests job beside `LAB_CORES=native`, it would prove the display
  sources on every PR, stale dist or not. Until then F184's turned walk
  (`dash@portrait`) fails on the committed dist until CI's rebuild, and its
  reads ran only in that scratch harness.

- [ ] **A53 [decision] Whether the Lab's source scans should parse
  JavaScript.** Three static rules over the probes (A44's string predicates
  and A45's options in `csp.test.js`, A46's request paths in
  `probe_server.test.js`) read source through `canary-local/tests/js_scan.js`,
  a bracket-and-comma reader, and each refuses by name what it cannot follow
  or lists it in its header. A46's scan grew to about 530 lines of
  reader-level data flow (and 220 lines of fixtures) after review. A vendored
  parser (acorn is one file of about 100 KB, no build step) would let them
  follow values exactly and shrink that; the cost is a vendored dependency in
  a no-build tree and rewriting three scans with their fixtures and mutation
  sets. Found by A46 (#1762).

- [ ] **A57 [decision] May the iOS timeline date a WAP row from its bucket
  when the device's clock is GPS-set?** `GET /api/events/today` rows carry
  only `time_bucket`; on a WAP with a GPS fix that is the household's
  10-minute bucket of the day (F28, A48), on one without it counts from boot,
  and nothing on the wire says which, so `WapEvents.swift`'s `anchoredDates`
  reads deltas only. The deltas also break across an offset change inside one
  boot's ring (the first fix, a DST change, a household zone set or changed, a
  drift correction), and the per-pass recompute can put any row one bucket out
  at a 10-minute boundary. Options: (a) keep deltas only and say so in the
  timeline; (b) have the firmware say per row (or per boot, with an offset
  epoch) whether the offset was applied, so the app can date synced rows by
  their bucket and split a page where the offset moved, a flag that adds no
  finer time than Invariant III's bucket; (c) anchor only on rows from a
  synced device. Found in A48 (#1762).

- [ ] **A84 [code+human] Capture the ROM banners the Lab's bench cannot
  quote.** Since A59 the bench prints only banners quoted from an Espressif
  source, except the ESP32-S3's two download-mode lines (the bench's pre-A59
  text, flagged `unsourced` in `ROM_VOICES` and said so on the bench's
  ribbon). No esp-idf v5.4 doc or example quotes a production ESP32-C3's
  software-reset or download-mode banner (the Nightlight's bench prints a note
  instead), and docs.espressif.com was not reachable from the machine that did
  A59. A serial capture from a C3-LCD-1.47 (power-on, `ESP.restart()`, BOOT
  held through RST) and from an S3 board (BOOT held through RESET), or a
  fetched Espressif page that prints them, would fill `ROM_VOICES` with a
  cited source each and clear the `unsourced` flag. Found by A59 (#1802).

- [ ] **A60 [decision] Whether a bench draws a slide switch and a battery
  fast-forward for a board without a battery path.**
  `canary-local/assets/app.js`'s bench view always makes an ON/OFF switch chip
  from `power.switch` and a "battery time x60" knob, so the Nightlight's bench
  (USB-C its only power) and the Nightstand stick's show a switch labeled "(no
  battery path)" or "(not fitted)" that toggles and gates nothing, and a
  fast-forward with no cell to speed. The battery chip itself is inert there
  since A54. Either a bench block may say it has no switch and the view draws
  neither, or the labeled no-op controls stay. Found by A54 (#1762).

- [ ] **A100 [code] The AMOLED 2.41 twin does not step its night floor as the
  board does.** `display_amoled241.cpp`'s `backlight_night_set()` writes the
  13-bit night duty as one panel brightness byte (`duty13 >> 5`, raised to 1
  while a glow is asked for), as the Nightlight's expander does, but since A79
  `emu_hal_display.cpp`'s `capped_night()` steps the duty only under
  `TFT_USE_EXIO`, so the amoled241 twin passes every duty through and its
  dimmest floors read finer, and a duty under 32 dimmer, than the panel.
  Stepping it on the AMOLED build too, held by `backlight_cap_test.cpp` in an
  amoled241 config, would close it; it moves that flavor's dist. Found by A79
  (#1802).

- [ ] **A98 [code+decision] The iPhone's Nightlight orientation picker names
  the side turns the other way round from the firmware.**
  `ios/Sources/SecuraCV/Views/Components/NightlightSection.swift` labels tag 1
  "On its left side" and tag 3 "On its right side", where `io/orientation.h`
  says `R90 = 1` is "turned a quarter clockwise, onto its right edge" and
  `R270 = 3` "onto its left edge"; the Lab's Try it chips (A80) follow the
  header. One of the two wordings is wrong, or they mean different things by
  "side" (the edge it rests on, or the way its top points). A person with a
  Nightlight on a desk should settle which edge each value stands it on; then
  one wording goes into both, with a test that holds the Swift labels to the
  header. Related: F282 (which way R90 turns on the glass), to be settled on
  the same device. Found by A80 (#1802).

- [ ] **A110 [code] The emulator's onboarding probe still prints the wide Join
  card's corner reach without holding it.**
  `canary-local/tests/onboard_probe.mjs` (`cardInHalo` in `onboard_glass.mjs`)
  prints the corners' reach past the 800 px glass's ring, in both
  orientations, with comments that call F155 open; `test_onboard_layout` now
  holds 159.9 px (163.5 under Heirloom) and 12.9 (16.5) px past the stroke's
  inner edge. Hold the same ceilings on the drawn glass and drop the "F155,
  open" wording. Found by F155 (#1802).

- [ ] **A112 [code] No browser probe reads the splash's tail.** Since F185
  (#1836) every display flavor draws the speech bubble's tail, and the native
  renders read it off the frame (`glass_splash_test.cpp` on LVGL 8.4, in every
  8.4 build's config and the emulator's dash; `glass_splash_lvgl9_test.cpp` on
  9.5), but the emulator's probes (`onboard_probe.mjs`'s splash reads,
  `boot_probe.mjs`) read only the bubble's lines and the bird, so a rebuilt
  dist that drew no tail, or drew it off the bubble's center line or into the
  bird, would pass them. A read of the tail's ink around its box, the way the
  native tests take it (the frame with and without the tail), would hold the
  dist to it. Found by F185 (#1836).

- [ ] **A65 [code+human] The iPhone's Fleet Wi-Fi Bluetooth rescue lane cannot
  reach a WAP.** The Fleet Wi-Fi rollout offers a "Bluetooth rescue" for a
  Canary the router change stranded (`FleetWiFiRollout.swift`'s did-not-return
  copy, `FleetWiFiSheet.swift`, `BLEConsole.swift`'s "Wi-Fi provisioning over
  BLE (the rescue path)"), but `BLEConsole` connects only to a peripheral
  advertising the console service `8fc1cee0-b162-4401-9607-c8ac21383e90`,
  which a WAP does not advertise, and the WAP's snapshot reads need a PIN
  confirmation. So the lane never fills. Either connect on the person's tap to
  the advertised SCV service (`a1b2c3d4-e5f6-7890-abcd-ef0123456001`,
  `ble_config.h`) and discover the console service by UUID once connected, or
  remove the lane and every sentence that offers it. Needs a WAP on a bench to
  settle (U1). Found by the 2026-10 cleanup's iOS package.
  *(2026-10-21, #1843's third review:)* F343's posture waits on this: until
  it is settled, the WAP's way out from under a host striking from nine or
  more addresses is the owner's browser (the landing page's session and
  `POST /api/wifi/connect`), not this lane, and the docs say so. The
  companion page (`companion_pwa.h`) writes the same CREDS over Web
  Bluetooth, but its chooser filters on the name prefix `SecuraCV` alone,
  while a WAP advertises `SCV-` and four characters in its scan response,
  or `WAP-` and four while the setup door is open (`ble_opera.h`,
  `ble_improv.cpp`), and its GAP name defaults to `SecuraCV-Canary`
  (`bluetooth_channel.cpp`); whether a WAP is listed there is for the same
  bench.
  *(2026-10-21, #1843's fourth review:)* the browser's move, and the lane's
  once this is settled, lasts only while the new network stays up: the WAP
  keeps the network it left saved and goes back to it when the new one is
  not found twice running (F343's sixth amendment).
  *(2026-10-22, #1843's sweep, third pass:)* the Fleet Wi-Fi sheet's footer,
  its not-reachable rows and the rollout's cleartext disclosure and declined
  verdict no longer offer the lane; they name the https re-pair and the
  WAP's own setup network. The lane's code (`FleetWiFiRollout.Path.ble`)
  stays until this is settled.
  *(2026-10-27, the owner-authority PR's merge of #1884, #1887:)* the
  browser's move named above now takes the API token after setup (F334),
  and the overflow holds that bearer with every untracked address, so it
  waits until the striking host stops (F343's seventh amendment). The lane,
  once this is settled, would write CREDS with no HTTP lockout in its way,
  with the caveats F343's amendments give it.

- [ ] **A66 [code+human] AccessorySetupKit spike for the nearby card.** On iOS
  18, AccessorySetupKit shows a system picker for a matched accessory and
  grants Bluetooth access to that device alone, in place of the app-wide
  Bluetooth prompt the nearby card needs today. Spike it behind
  `#available(iOS 18, *)`: one `ASPickerDisplayItem` per family with the
  Improv service UUID and the `Sense-` / `Vision-` / `WAP-` name prefixes, the
  Info.plist keys it requires, and `scripts/lint_apple_plists.py` support for
  them. Bench first whether the app's unfiltered fleet-beacon scan still hears
  devices the picker never approved, since that scan is how the Fleet tab
  hears every Canary. Found by the 2026-10 cleanup's iOS package.

- [ ] **A94 [decision] The WAP's `w` monitor chip prints its SoftAP
  password.** `canary_wap.ino`'s `w` answers with the AP SSID, the AP password
  in clear, the IP and a redacted API token. It was one of the eight shared
  chips before A86 and stays a WAP chip after it (labeled Wi-Fi AP). Decide
  whether a one-tap chip on a screen that may be shared or recorded should
  print a secret, or whether the firmware should redact the password the way
  it redacts the token (and the chip stay). This is the USB console, a
  separate road from F289's network one. Found by A86 (#1802).

- [ ] **A69 [code+human] Writing a hub card on Linux needs a disk-group
  account or root.** `desktop/hub-io/src/write.rs`'s `open_target` opens the
  raw device directly, so a normal desktop account gets `PermissionDenied`;
  since the 2026-10 cleanup the Flasher warns before the download and names
  the per-card grant (`sudo chown "$USER" <path>`). Linux's counterpart of
  macOS's `authopen` is UDisks2's `Block.OpenDevice` (or `OpenForRestore`)
  over the system bus, which raises a polkit prompt; the `dbus` crate is
  already in the Flasher's `Cargo.lock` through `keyring`. Add it as the
  fallback when the direct open is refused, then change the preflight copy.
  Validate on a real GNOME and a real KDE session first (L effort).

- [ ] **A70 [decision, its bench gated by U3] One away-alert subscription
  serves the whole iCloud account.** The away path
  (`ios/Sources/SecuraCV/Native/AwayPush.swift`) saves one
  `CKQuerySubscription` with the fixed id `securacv-witness-wake-v1` in the
  private database, and since the 2026-10 cleanup turning the last Anywhere
  rule off deletes it, so opt-out is real. The code treats that subscription
  as the account's, not the device's (`AwayArming.decide`'s comment): one
  iPhone's opt-out pauses away alerts on the account's other devices until one
  with an Anywhere rule next opens the app and saves it again, and a device
  that never armed the path leaves it alone to limit that. Separately,
  `publishWake` is gated on this device's own `subscribed`, so a resident
  iPhone whose rules are all "On Wi-Fi only" stops posting wakes for the
  household's other devices. Decide whether the trade-off stands (it is now
  stated in `docs/design/cloudkit_backend.md`'s opt-out line) or the design
  moves to per-device subscription ids and a publish switch of its own. Bench
  first, on a signed build (A8) with two iPhones on one Apple ID: whether the
  id is in fact one per account, and what the second phone receives after the
  first opts out. Found by the 2026-10 cleanup's Apple package.

- [ ] **A113 [code] The iPhone reads a birth day the Wall drops, and shows it
  a day early west of UTC.** Since A72 (#1832) the Wall and the web readers
  keep `born_day` only inside the window a Canary can stamp (20089 to 49710)
  and format it in UTC. The iPhone's `FleetSelfReport`
  (`ios/Sources/SecuraCV/Wire/FleetSelfReport.swift`) accepts any `born_day`
  above 0, with no window, so a day from 1 to 20088 or above 49710, which no
  Canary can stamp, shows on the phone but nowhere else; and its
  `BirthCertificateCard` formats `bornOn` (midnight UTC) in the local time
  zone, so west of UTC it shows the day before (day 20673 reads Aug 7 instead
  of Aug 8 in US time zones). It also shows its 'met a clock late' note for a
  day with no flag, where the Wall shows it only beside `born_exact: false`.
  Bring the phone in line: the same window, a UTC `FormatStyle`, and a
  west-of-UTC test zone, as the Wall's DeviceParityTests use. Swift builds are
  CI's. Found by A72 (#1832).

- [ ] **A114 [code+decision] No hub or display relays a device's birth day.**
  The kernel's `/api/fleet` (`fleet_document`, `FleetRow` in
  `src/fleet_peers.rs`) and a display's peer rows (canary-display
  `src/net/glass_web.cpp`, whose `FleetSelfDevice peer{}` leaves `born_day` 0)
  never relay `born_day` / `born_exact`, so a fleet read through a hub or a
  display shows no birth day on any reader. Only a Canary that answers
  `/api/fleet` for itself (canary-wap, canary, node_http) sends it; since A72
  (#1832) DISCOVERY.md, tvos/README.md and the website's /witness-wall fold
  say so. Relaying it is a contract change, made in DISCOVERY.md first, then
  in the kernel's two-row vector. Found by A72 (#1832).

- [ ] **A73 [decision] Build or drop the watch's per-witness day strip and
  reason log.** `docs/design/apple_watch_and_notifications.md` §3.3 specifies
  a 24-bucket day strip and a reason log per witness on the wrist. The 2026-10
  cleanup built the rest of the wrist's parity with the iPhone and listed
  these two as open in the RFC's header. Decide whether they ship or leave the
  RFC.

- [ ] **A74 [human] Try the 2026-10 watch surfaces on a real Apple Watch.**
  Each was built and hand-reviewed in the 2026-10 cleanup, and CI's macOS job
  is what compiles it; none has been on a wrist: the custom long-look for
  `SECURACV_WITNESS` notifications mirrored from the iPhone, with Acknowledge
  and Mute still underneath; a storm-summary alert (`AlertStorm.threadID`)
  falling back to the plain words; the Alerts page's leading Acknowledge swipe
  on rows that are `NavigationLink`s; Check the Fleet in Siri, Shortcuts and
  the Ultra's Action button picker. (`apple_watch_and_notifications.md` §8 W1
  says the long-look is untried.)

- [ ] **A111 [code] The phone's Orientation picker shows only the saved
  turn.** The dash's `GET /api/settings` now serves `orientation_worn` and
  `orientation_refused` beside `orientation` (F245, #1836), but
  `ios/Sources/SecuraCV/Transport/GlassSettings.swift` decodes only
  `orientation`, so a glass that refused a turn still reads as turned on the
  phone. Reading `orientation_refused` (absent from older firmware: treat as
  0), the sheet could say the glass stayed landscape for want of memory, in
  the glass's own words. It must read the port's word, not `orientation_worn`
  differing from `orientation`: `GlassSettingsSheet.swift` posts one key and
  re-reads at once, which usually lands in the second before the render loop
  wears the turn, when the two differ on a healthy glass. Swift builds are
  CI's. Found by F245 (#1836).

- [ ] **A91 [code+decision] A recovery kit of another device, opened from a
  Ready-to-pair row, is paired under that row's name.** `PairView.pair(from:)`
  keeps another device's kit at the host it names (A88) but still builds the
  `PairedDeviceRef` from the row (`name: canary.name`, `deviceType:
  canary.deviceType`), so WAP A's kit opened from WAP B's row adds A under B's
  name and type. And "the same device" is a 16-bit `device_id` suffix of the
  key fingerprint (`canary_wap.ino` `generate_device_id`), so two WAPs in one
  home share an id with probability about n²/131072, and then A's kit is moved
  onto B: an https kit fails on A's pin, and a plain-http kit sends A's token
  to B. Decide whether a kit whose `device_id` is not the row's is refused
  with a sentence that names both devices, and whether a plain-http kit should
  be moved at all when it names a host other than the setup address; then test
  it in `DeviceAPITests` and pin the view in
  `scripts/tests/test_ios_setup_glue.py`. Found by the A88 review (#1802).

- [ ] **A92 [code] The nearby card's stand-down is one flag for every iPad
  window.** `FleetStore.nearbyOfferStandsDown` (A75) is a single `@Published`
  Bool and the app supports multiple scenes
  (`UIApplicationSupportsMultipleScenes`), so a "+" sheet in one window hides
  the card in every window, and with a "+" sheet up in two windows, closing
  one clears the flag while the other is still up, and the card shows dimmed
  behind it. Keep the stand-down per scene (an environment value from the
  scene that presents the sheet), or count presenters with a guard against an
  unpaired onAppear, and turn the `magic_pairing.md` section 9 iPad note into
  a check. Found by the A75 review (#1802).

- [ ] **A90 [decision] The Lab's canary-wap topic sample shows a Chirp state the
  device never enters.** `gen_wap.py` writes the `chirp` topic's sample
  payload as `{"state":"listening"}` (`wap.json`), but the canary-wap goes
  from `disabled` through `initializing` straight to `active` (`enable()` in
  `chirp_channel.cpp`) and nothing sets `CHIRP_LISTENING`;
  `chirp_channel::is_active()`, which counts it, has no caller. Show a state
  the device publishes (`active`), and decide whether LISTENING stays in the
  enum and in spec §7.1. Found by HA33 (#1802).
  *Code half done (#1920); the rest retagged [decision]:* the page's chirp
  topic sample is `{"state":"active"}`, guarded by `state_name()`'s `case
  CHIRP_ACTIVE: return "active";` and a code line setting `CHIRP_ACTIVE` in
  `chirp_channel.cpp`, so `gen_wap.py` refuses another example word;
  `devices/wap.json` moved one line, and a `wap.test.js` pin holds the word to
  a state the channel can show (it fails on the old `listening`). Left for the
  decision: whether `CHIRP_LISTENING` stays in the enum (`mesh_network.h`) and
  in spec §7.1. The canary-wap never enters it (`enable()` goes INITIALIZING
  to ACTIVE; `is_active()`, which counts it, has no caller), §7.2 still
  describes INITIALIZING → LISTENING → ACTIVE, Home Assistant's
  `binary_sensor.py` keeps `'listening': True` and `test_chirp_active.py`
  holds that table to `state_name()`'s words, so dropping the word ripples
  into the integration and the HACS mirror. The default that needs no
  decision: keep the enum and add one §7.2 sentence that the canary-wap takes
  INITIALIZING → ACTIVE and never LISTENING, which stays for other
  implementers (listed with D20); removing the word is the cleanup a human
  picks. Host-tested.

- [ ] **A77 [human] Run one real `.deb` self-update on Ubuntu 22.04.** Since
  the 2026-10 cleanup the desktop apps' install docs and release bodies say
  the `.deb` updates itself and asks for a password: tauri-plugin-updater
  2.13.1's `install_deb` runs `pkexec dpkg -i` (falling back to zenity or
  kdialog, then sudo), and the published manifest carries a `linux-x86_64-deb`
  entry. That is read from the plugin's source, not seen. Install an older
  Flasher `.deb` (and an older Lab `.deb`) on Ubuntu 22.04, let it update, and
  confirm the prompt and the result.

- [ ] **A78 [decision] Serve the enclosure generator scripts from the Lab site
  and app, or prune them.** `pages.yml` copies `docs/hardware/enclosure` into
  the published Lab site unpruned (`cp -rL`), so its 12 `.py` files and
  `render.sh` are served, and the Lab app's bundle carries them too
  (`desktop-lab/frontend-stage.json`). To drop them: a
  `find _site/docs/hardware/enclosure … -delete` line in `pages.yml`'s
  "Assemble site" step and the matching `pruneExt` entry
  (`{"docs/hardware/enclosure": [".py", ".sh"]}`); `lab_bundle.test.js`
  already parses every `find` line there and will demand the pair. A
  maintainer's call.

- [ ] **A111 [human] Confirm `PairedDevice.tlsCertFP` in Production CloudKit,
  then send iOS 0.7.0 and tvOS 0.3.0 to App Review.** `ios-v0.6.0` was the
  first release to write `PairedDevice.tlsCertFP` (#1675), and nothing
  records its promotion to Production, the environment every TestFlight and
  App Store build talks to (`CHANGELOG.md`: the 2.4.16 section's iOS 0.7.0
  bullet, and the correction in the 0.6.0 entry). In order, on a Mac with a
  CloudKit management token: (1) `ios/scripts/cloudkit_schema.sh check`,
  which reads Development and Production; if Production lacks the field,
  `promote`, then `check` again. (2) In App Store Connect, confirm iOS 0.7.0
  build 38.1 and tvOS 0.3.0 build 19.1 have finished processing in
  TestFlight (tvOS only after A112). (3) Submit both for App Review. The
  release workflows stop at the upload, so neither version reaches anyone
  until (3). Found by the 2.4.16 release audit (2026-10-10).

- [ ] **A112 [human, then code] Confirm the tvOS store build carries its
  CloudKit and app-group entitlements.** Since 2026-08-13
  (`.github/RELEASE_LESSONS.md`, the tvOS provisioning entry)
  `tvos/scripts/release-tvos.sh` archives store builds with
  `CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO` and leaves all signing
  to `-exportArchive`. An unsigned archive has no entitlements in its
  signature, and whether the export puts back
  `com.apple.developer.icloud-services`,
  `com.apple.developer.icloud-container-identifiers` and
  `com.apple.security.application-groups`
  (`tvos/WitnessWall/Support/WitnessWall.entitlements`; the Top Shelf
  extension's app group in `TopShelf.entitlements`) is unconfirmed. Without
  them the Wall's away wake (ResidentWatch) and the Top Shelf cache would not
  work. Check build 19.1's entitlements in App Store Connect (the build's
  metadata lists them) or with `codesign -d --entitlements :-` on the
  exported app and its extension. If any is missing, the archive has to keep
  its entitlements (a distribution-signed archive, per the same lesson's
  constraints) before 0.3.0 goes to review. Either way, add a step after the
  export in `release-tvos.sh` that prints both bundles' entitlements and
  fails when one is absent, so the next build answers this itself. Found by
  the 2.4.16 release audit (2026-10-10).

- [ ] **A113 [code+human, bench gated by U3] `CloudSync.push` probably fails
  every update to an existing `PairedDevice`.** It builds a fresh
  `CKRecord(recordType:recordID:)` per device and calls `db.save(record)`
  with the error dropped (`ios/Sources/SecuraCV/Cloud/CloudSync.swift`). A
  save that carries no server change tag for a record id the server already
  holds is expected to fail with `serverRecordChanged`, so a rename, a new
  `baseURL` or a new `tlsCertFP` would reach iCloud on a device's first push
  and never after. Bench first, on a signed build (A8): pair a device, rename
  it on one iPhone, and read the record in the CloudKit Console or on a
  second iPhone after a pull. If it is stale, save through
  `CKModifyRecordsOperation` with `savePolicy = .changedKeys` (or fetch,
  change and save), and log each record's error instead of discarding it.
  Found by the 2.4.16 release audit (2026-10-10).

- [ ] **A119 [human] Look at the Wall's Verification footer on tvOS.** A105
  (#1916) put all three mint forms in `WallSettingsView.swift`'s footer, a
  literal that SwiftUI reads as Markdown, and spelled the root form's
  container `CONTAINER` instead of `<container>`, which a Markdown reader can
  take for HTML. `test_witnessd_unit.py` refuses the inline openers it lists,
  `%` and control characters there, but no test renders the view: what it
  draws, whether `<container>` would be safe, and whether the footer fits on
  screen are unproven. Look at it in the tvOS simulator or on an Apple TV; if
  `<container>` draws as text, its `MARKDOWN_ACTIVE` row or the `ROOT_FORM`
  placeholder assertion is the one line to change. Needs a Mac with Xcode.
  Found by A105 (#1916).

- [ ] **A120 [decision] A flavorless display name resolves to the Watch
  Station, chip unseen, and four surfaces say it as fact.** Since A96 (#1916)
  `productForProject()` resolves `canary_display` / `canary-display` to the
  displays family's first entry, the Watch Station, which was the role line's
  hedged guess ("It looks like the ..."). The smart pick ("This board already
  runs Canary Watch Station, so it is preselected"), the update headline, the
  other cards' "Switch from Canary Watch Station" verdicts and the desktop
  passport's head (pinned by the A96 test) now state it, and it ignores the
  chip: on an ESP32-C6 or C3 board the card for the display that chip runs
  (Nightstand C6, Nightlight C3) reads "Switch from Canary Watch Station".
  Better than main, which named the flagship. Options: resolve such a name to
  "a display, flavor unknown" (no preselect, no headline, silent verdicts, a
  hedged passport), or prefer the display `productsForChip(chip)` names before
  the family's first. Change both flashers together. Releases after 2.4.17
  stamp the asset stem, so this matters only for names a board check has not
  shown to exist (A122). Found by A96 (#1916).

- [ ] **A121 [decision] Both flashers offer the self-check ask to firmware the
  catalog does not name.** To keep the browser's ask for boards on 2.4.17 and
  earlier (their descriptor names the Arduino core, and flagship and Vision
  boards among them answer `j`), A97 (#1916) asks any readable, non-blank
  firmware unless the catalog names a product whose `serial_receipt` is false.
  The browser always did; the desktop used to require a recognized product ("a
  foreign image has no j to answer"). A truly foreign image (MicroPython, a
  vendor demo) is now offered the ask on both and answers nothing. Keep it, or
  narrow it (recognized products plus Arduino-core descriptors, say) in both
  flashers together, with `desktop_parity.test.js`'s A97 rows. Found by A97
  (#1916).
  *Note (2026-11-04, #1919):* A123 is this rule on the done card: after a
  restore or a local-file write the browser monitor gets no product, and
  `requiresLiveReceipt(null)` asks. Decide the two together; A95's render
  (#1919) pins the two asks and changes with the decision.

- [ ] **A122 [human] Read what real boards report to the flashers.**
  Everything A96 and A97 (#1916) decide is host-tested against the names in
  `flash.json` and synthetic ones. With boards in hand: which `project_name` a
  display board's descriptor reports (a stamped stem, `canary_display`, or
  `arduino-lib-builder`), and whether "tap the board's RESET button" then
  shows the boot log in each flasher's monitor, for the WAP, the Sense and a
  display. Not bench-tested. Found by A96 and A97 (#1916).

- [ ] **A123 [decision] The browser monitor asks `j` of whatever firmware a
  backup or a local file left on the board.** After a restore or a local-file
  write, `phaseDone` hands the monitor no product, and
  `core.requiresLiveReceipt(null)` is true, so the browser sends `j` twice to
  whatever that image is (a WAP, Sense or display image answers "unknown
  command" twice). It is a read and harmless, and A95 (#1919) pins today's two
  asks. It is A121's decision met after a restore: the same
  `requiresLiveReceipt(null)` rule asks firmware the catalog does not name,
  so decide the two together, and A95's render rows for the two asks change
  with them. From the A95 render. Found by A95 (#1919).

- [ ] **A126 [decision] The bench's BOOT press is not the strap.** A99 (#1919)
  drives only the twin's button input with the press; the strap toggle alone
  picks download mode. On a real board a finger holding BOOT through RESET is
  also the strap, so a press held through RESET on the bench boots the app
  where real silicon parks the ROM (`bench.test.js` pins "RESET while pressed
  boots the app: the strap toggle, not the press, picks download mode", and
  the bench lets the press go before the reset). Decide whether the press
  should count as the strap too. Found by A99 (#1919).

- [ ] **A127 [code] The bench's restart undoes an erase the twin's firmware
  makes.** `emu_support.cpp`'s `Preferences::remove` and `::clear` never call
  `js_nvs_write`, so an erase never reaches the page's copy of the flash, and
  the bench's reboot (`onReboot`, "booting again with its memory intact")
  restores that copy and boots provisioned while `emu-shell.js` seeds the home
  network again. So a firmware factory reset (the 10 s BOOT hold A99, #1919,
  made reachable) or a forgotten network comes back after the restart; the
  press's title says so, and `bench_probe.mjs` and `bench.test.js` hold that
  true. Report removals from `emu_support.cpp` (a dist rebuild) and give the
  reboot a path that drops the keys and boots unprovisioned. Found by A99
  (#1919).

- [ ] **A129 [code] Three fidelity gaps in the Sense radar lab's data path.**
  Since A124 (#1920) the lab drives the firmware's two FSMs frame by frame,
  and three small things still differ from the radar and the parser. (1) The
  lab sends a vitals frame on every animation pass while someone is still,
  near and Present; the MR60BHA2 reports vitals about once a second
  (`senselab.json`'s `hardware.vitals_period_ms` is 1000) and `sense.json`
  carries no cadence, so the lock confirms and drops up to one vitals period
  earlier than on a real radar; carry the cadence into `sense.json`
  (`gen_sense.py`) and have the lab model it. (2) `radarFrame` hands
  `makePresenceFSM` `Math.min(...)*100` cm as a float where the firmware's
  parser rounds distance to `uint16`, so a range band could differ within 0.5
  cm of a gate; round in `radarFrame`. (3) `gen_sense.py` writes the `second`
  sandbox scene's serial as `[vitals] breathing lost` where `drive_fsms`
  prints `[vitals] breathing lost (stall)` as the deadline drops the lock,
  which the lab now prints; the field is unused for lab-driven scenes but says
  otherwise (a `gen_sense.py` edit and a `sense.json` regeneration). From
  code. Found by A124 and its review (#1920).

- [ ] **A130 [decision] Two rules for the Sense radar lab: what a moving
  in-range target's lock reads, and whether the wellbeing toggle restarts the
  FSMs.** (1) `makeRadarLabLoop`'s `VitalsFSM` sees only presence frames until
  `world.still`, so on the wellbeing build the Vitals row says "no lock
  (unknown)" until the first vitals frame, where `mr60_vitals.cpp` moves
  Unknown → Lost on any vitals frame, plausible or not; if the MR60BHA2
  reports vitals frames while a target moves in range, the device reads "lost"
  where the lab reads "unknown". Decide whether the lab's radar emits
  implausible vitals frames for a moving in-range target (with A129's cadence)
  or the Vitals row states the lab's rule. (2) `senseStateDue`'s comment calls
  the lab's wellbeing toggle a build switch (a reflash), but the lab restarts
  neither FSM: the `VitalsFSM` runs from the lab's boot and only receives
  vitals frames on the wellbeing build, so a lock held when the build was
  switched off is still held if it is switched back on within `lock_lost_ms`.
  Decide whether toggling should `reset()` the loop (a `presence_cleared` from
  the fresh boot would then be signed). From code. Found by A124's review
  (#1920).

- [ ] **A131 [code] `gen_wap.py` still types the `_securacv._tcp` TXT list.**
  `ap.mdns_services[1].txt` (`device_id`, `fw`, `host`, `name`, `model`,
  `dt=canary-wap`, `role=witness`) is typed in the generator, not read from
  `canary_wap.ino`'s `addServiceTxt()` calls, and the `broker` and `bport`
  records `mdns_sync_broker_txt()` adds while the MQTT link is up are not
  listed. F366@17612 (#1920) read the ports only. Read the records from the
  sketch at each start as the ports are read, refusing a spelling the reader
  cannot take, with scratch-copy cases in `test_gen_wap_ap_facts.py`. From
  code. Found by F366's package (#1920).

---

## 3. Home Assistant (monorepo `custom_components/` + HACS mirror)

- [ ] **HA8 [human-gated by U1] Ship a working example camera** for the
  Frigate config once a real camera is on the bench (today: one placeholder
  camera, `demo`, on an RTSP path nothing serves — Frigate starts it and
  logs ffmpeg errors for it, so the config parses but never detects).

- [ ] **HA12 [decision] The kernel device's double "SecuraCV" prefix.** HA2
  kept "SecuraCV Last Event" and "SecuraCV Adapter Host" (keys
  `kernel_last_event`, `adapter_stats`) byte-identical. Dropping the prefix
  changes their friendly names, new-install entity ids,
  `docs/homeassistant_setup.md` and the data `gen_homeassistant.py`
  generates from it (`canary-local/devices/homeassistant.json`). Decide,
  then rename in `strings.json` and `translations/en.json` together (the
  names test pins them).

- [ ] **HA13 [human] Record a live bring-up in the Frigate runbook.**
  `integrations/ha_frigate_mqtt/RUNBOOK.md`'s header says its expected
  outputs are derived, not observed (HA7 was done with no Docker daemon),
  and its "Settings → Devices & services → MQTT → Configure" path comes
  from Home Assistant's documentation, not a click-through. Run the stack
  once and replace the derived samples with a dated live run; the header
  keeps saying "derived" until then. The verify script's first check needs
  a live detection, so this pairs with HA8.

- [ ] **HA23 [decision] Home Assistant accepts an event whose id equals its
  mark.** `_replay_gate` (`sensor.py`) refuses only an id below the last
  verified one. An equal counter passes on purpose: a broker re-delivers
  the retained last event, and the chain and counts republish unchanged
  while idle. So when a boot's NVS writes all fail and the device reissues
  an id, HA accepts the second, different body under it (a review history
  run through the real gate). This was true before F46, which does not
  make it worse. Decide whether an equal id should pass only with an
  identical body. Found by F46's review (#1761).
  *Note (2026-11-04, #1916, HA44):* the same equal counter re-shows a live
  re-send of an identical signed event on Last Event and records it again, and
  the projected Motion and Occupancy sensors run no gate at all (they read
  only the trust view), so they re-arm their hold window on it. HA44 removed
  the one double delivery it would otherwise have added, the creating
  publish's retained echo, and a stale counter is still refused right after
  it. Decide with this item whether the projected sensors skip an event Last
  Event marks as a replay, or every retained event.

- [ ] **HA27 [decision] Home Assistant records the Health sensor's counters on
  every change.** The recorder stores each Health sensor state write with its
  attributes. `uptime_seconds` already makes every health publish a new
  attributes row, and since HA24 a canary-wap's retained `egress` topic writes
  the sensor a second time each health cycle, with its thirteen counters.
  Decide whether the counters (and the uptime) go in the entity's
  `_unrecorded_attributes`, which drops their history. Found by HA24 (#1762).

- [ ] **HA40 [decision] The activity ribbon's quarter-hourly row turns a
  Canary's Last event and the voice brief to `unknown`.**
  `core.activity_ribbon` commits `ribbon_bucket_advanced` (no state, P0,
  category event) every 15 minutes on both trees, both egress paths publish it
  on the events topic, and `build_event_body` writes `unknown` as its
  `event_type`; so on a quiet device the canary Last event sensor reads
  `unknown` most of the day, and the voice brief says "the last thing
  witnessed was unknown" (casual), "One thing in the last hour: unknown"
  (night) and "unknown, within the last ten minutes" (last event). This was so
  before HA28 and still is: HA28 names only the summaries, which therefore
  show for at most 15 minutes. Decide whether bookkeeping rows move Last event
  and the voice brief at all: the sensor could keep its previous value for a
  placeholder row that is not a summary, or the firmware could keep the ribbon
  row off the events topic. Found by HA28's review (#1802).

- [ ] **HA43 [decision] A Canary's Last event shows fields its signature does
  not cover beside a verified badge.** The signed canonical carries `state`
  (`signature.py` `build_event_canonical`), not `event_type` or `type`, so a
  validly signed body whose `event_type` (before HA28) or a summary's `type`
  (since HA28) was rewritten on the broker still shows the rewritten label
  with `verified: true`. Decide whether Last event shows `state`, which the
  signature covers, or marks the label as unsigned. Found by HA28's review
  (#1802).

- [ ] **HA46 [code+decision] Integration entities a canary-wap gets and feeds
  nothing.** Mesh Connected (binary_sensor.py) reads `peer_count`, `peers`,
  `sent`, `received` and `relayed`, which no firmware publishes: the WAP's
  `mesh` topic carries its airtime governor's counters (airtime_pct_x100,
  channel, locked_to_sta/ap, fallback, routine_allowed/denied, urgent_sends),
  so it reads off on every WAP whatever its mesh does, against
  docs/homeassistant_setup.md's 'Opera mesh peers present'. GPS Fix
  (sensor.py) reads the health's `gps` object and reads `no_fix` forever. SD
  Wear (sensor.py) and SD Replace (binary_sensor.py) read the health's `sd`
  object (wear_pct, replace_recommended); a WAP's health carries only
  `sd_mounted`. Tamper GPIO, Tamper GPS Jamming and Tamper Motion
  (binary_sensor.py's tamper-type sensor) read tamper.gpio / tamper_gpio,
  gps_fix_lost / gps_jamming and tamper.motion / unexpected_motion; a WAP's
  lid switch is `enclosure_open`, and it has no GPS and no motion sensor.
  Decide per entity: not created for a canary-wap (device_type), or read the
  WAP's fields. Pinned by
  `test_wap_mqtt_contract.py::test_an_entity_a_wap_gets_moves_with_what_it_sends`
  (seven strict xfails; not creating the entity for a WAP resolves its case).
  Found by F34's contract fixture (#1836).

- [ ] **HA47 [decision] A canary-wap's presence never asserts the HomeKit
  Motion and Occupancy sensors.** Both are created for a WAP from its events
  topic. But `HOMEKIT_EVENT_SIGNALS` (const.py, generated from
  spec/witness_dictionary.json's homekit_projection) maps none of the WAP's
  presence vocabulary (event_type active, subtle, quiet, together, empty, from
  core_presence.cpp), so both stay off. Whether CSI presence projects to
  occupancy is a maintainer's call (it is a privacy promise in the Apple Home
  quickstart). The dictionary, gen_apple_home_docs.py and the kernel's
  homekit.rs move together. Pinned by
  `test_wap_mqtt_contract.py::test_a_wap_presence_event_asserts_motion_and_occupancy`
  (two strict xfails; not creating them for a WAP also resolves it). Found by
  F34's contract fixture (#1836).

- [ ] **HA36 [decision] What an unreachable mirror means for the HACS link
  job.** `lint.yml`'s `hacs-store-page-links` fetches four files from
  raw.githubusercontent.com on every PR and exits 2 (red) when it cannot after
  three tries, so a network blip turns any PR red. Decide whether an
  unreachable mirror is a failure (today: nothing is checked, so nothing
  passes silently) or a warning annotation. Found by HA35 (#1802).

- [ ] **HA49 [decision+human] Check app 0.9.0 and its fix on Home Assistant
  OS, and decide on a patch release.** 0.9.0's `config.yaml` declares
  `frigate.cameras` and `frigate.labels` as `list(str)?`, which is the
  Supervisor's select syntax: a choice whose only allowed value is the word
  `str`. By the Supervisor's own option code (`supervisor/apps/options.py` at
  home-assistant/supervisor `bde7fb7b`, `_validate_typed`'s
  `vol.In(match.group("list").split("|"))(str(value))`), the app's own
  defaults (`cameras: []`, `labels: ["person", ...]`) fail that check, which
  `write_options` runs on the stored options before every start, and `POST
  /addons/self/options` refuses the wizard's save the same way; so every
  install of 0.9.0 should fail to start with "value must be one of ['str']".
  Behind it sits the bashio parse HA38 fixed, which by bashio 0.15.0's code
  stopped every Frigate-mode setup with labels set. This was read from the
  Supervisor's source and checked against a pure-Python copy of its rules in
  `test_addon_store.py`; running the Supervisor's code was refused here, and
  it was not seen on a real install. #1916 fixes both (`- str?`, `opt_list`).
  On HA OS: install 0.9.0 and see whether it starts and whether the wizard
  saves; then this branch's build the same way. Then decide on a patch
  release. The app's CHANGELOG entry sits under `## Unreleased`, which
  `scripts/bump_version.sh` does not rename, so the release must rename it.
  Release blocker. Found by HA38's review (#1916).

- [ ] **HA50 [human, then code] Confirm HA16's entity ids on a live Home
  Assistant, then put HA15's lines back.** Follow
  `docs/audit/hardware_verification_checklist.md`'s HA16 row: Home Assistant
  2025.10 or later that has never seen either device; a kernel bridge
  publishing discovery with no SecuraCV integration claiming it; a canary-wap
  DEV or FULL build on a XIAO ESP32-S3 Sense (`FEATURE_ACOUSTIC_EVENTS`). On
  the Entities tab under Settings → Devices & Services, filter by each device
  and read the Entity ID column: expect `binary_sensor.pwk_<zone>_motion`,
  `binary_sensor.pwk_chain_problem`, `sensor.pwk_last_event`,
  `binary_sensor.<id>_smoke_alarm` and `binary_sensor.<id>_co_alarm`. On a
  pass, tick the row, put HA15's three lines back in
  `docs/integrations/apple-home-homekit-bridge.md` §4 and say which release
  showed them (`test_docs_claims.py` keeps them out only while the row is
  unticked). Not run. Found by HA16 (#1916).

- [ ] **HA52 [decision] The key-mismatch repair no longer shows the kernel's
  address.** Since F399 (#1916) the `tofu_mismatch` confirm text names the two
  fingerprints only. The core's plain `ConfirmRepairFlow`, which an orphaned
  issue id gets, fills that text from the issue's placeholders alone, and the
  issue no longer holds the address, so a `{url}` slot there would render
  unfilled. In a home with two kernels, the owner tells them apart by the
  pinned fingerprint, not the address. The flow could pass
  `entry.data[CONF_URL]` (a flow is never in a download), with the
  hassfest-subset slot test changed beside it. Keep it as it is, or bring the
  address back in the integration's own flow. Found by F399 (#1916).

- [ ] **HA54 [decision] A wizard re-run on a standalone install drops its
  configured cameras.** `savePayload()` always sends mode `frigate`, and the
  server's Frigate branch writes `options.cameras = []`, which drops a
  standalone install's hand-configured cameras and their zone ids; the same on
  main. A test pins (HA39, #1916) that a Frigate save stores no zone id,
  without deciding this, and the reconfigure note says the wizard overwrites
  the cameras it collects, so it may be intended. Decide whether a re-run
  keeps a standalone install standalone. Found by HA38 and HA39 (#1916).

- [ ] **HA55 [decision] A discovered camera can outrank a configured one.**
  HA38 asked to decide, once list options read whole (#1916), whether a
  configured camera comes before a discovered one. `build_camera_config` still
  merges with `jq -s 'add | unique_by(.name)'` and takes `.[0]`, the first row
  by name, so with go2rtc discovery on the primary camera can be a discovered
  one while a camera is configured. Decide the precedence, then hold it with a
  `test_serve_wizard.py` case. From code. Found by HA38 (#1916).

- [ ] **HA56 [code] Derive the diagnostics redaction set from the generated
  device-details vocabulary.** F391 named `TO_REDACT`'s identifier keys
  literally (#1891) and its health keys (#1916), and left its last clause:
  derive the set from the generated vocabulary, so a key the vocabulary
  gains is redacted with no edit to `diagnostics.py` (device-details PR 3).
  No other item tracked that clause, and neither the design it comes from
  (`docs/design/device_details.md`, §11, P18) nor a generated vocabulary is
  on main. Land the vocabulary or say where it lives, then read `TO_REDACT`
  from it and hold the literal keys to it in
  `test_the_redaction_set_names_every_identifier_class`. Found by the review
  of #1916's ledger (F391).

- [ ] **HA60 [code+decision] Health's `battery_percent` attribute reads 100
  when a present battery sends no number.** `sensor.py` shows
  `battery_percent` 100 when `battery_percent()` is None. That is right on
  mains (the canary-wap sends 100 with `battery_present` false), but on a unit
  with `battery_present` true and a bool, non-finite, negative, over-wide or
  separator-spelled charge (each no reading since HA57, #1920) it shows a 100
  nobody sent. HA53 made `memory_free_bytes` None for no reading; doing the
  same here changes an attribute contract dashboards may read
  (`test_health_and_egress_read_as_the_wap_sent_them` pins 100 on mains), so
  it is a decision first. From code. Found by HA57 (#1920).

- [ ] **HA61 [decision] One spoken vocabulary for the lowercase wire ids.**
  HA58 (#1920) keeps an unlisted id's own case, so the lowercase ids are said
  in their own words: `sd_remove` "sd remove", `sd_error` "sd error",
  `gps_jamming` "gps jamming", `memory_critical`, and `acoustic_co_alarm`
  "acoustic co alarm" (a TTS reads "co" as a syllable on a safety alarm);
  `held_summary` is said "held summary" (HA42 pinned that wording) while the
  card and the Last event entity now say "Quiet Hours summary" (HA41). The
  card already narrates the tamper kinds with calm sentences ("Storage card
  removed"), and `strings.json`'s `tamper_events` has a third wording ("SD
  Removed"). Choose one spoken vocabulary for these ids (the card's,
  `strings.json`'s or the id's own), and whether `lint_dictionary_sync.py`
  holds it; `test_voice.py`'s docstring says the lowercase case is deliberate
  until then. From code. Found by HA58 (#1920).

- [ ] *(Mirror repo itself: no code work. It was byte-identical again as of
  securacv-homeassistant#17 (2026-09-24), which resynced the 33 carried
  files #1703, #1704 and #1718 had moved. The same PR brought the store
  page's watch-actions, key-pinning, broker-TLS and Apple Home sentences,
  and a `lint_readme.py` overclaim check that reads a hard-wrapped claim as
  one and refuses "encrypted by default". PR #1725's carried files
  followed in securacv-homeassistant#19 and PR #1727's in #20 (2026-10-01),
  byte-identical again (U6). PR #1761 added one carried test (F46's
  `tests/test_replay_one_id_space.py`), carried by hand in
  securacv-homeassistant#21 (2026-10-02). #1762's carried files (waves
  9-15, 12 files) followed by hand in securacv-homeassistant#24 (`034089c`,
  2026-10-07; `check_mirror_sync.py`: 66 files match). Its health
  items are U6, U7 and U10 above, plus the three monorepo-fixture tests its CI
  deselects, which is by design. A few more tests skip themselves there
  because they read firmware sources the mirror does not carry.)*

---

## 4. Website (`kmay89/securacv_website`)

- [ ] **W1 [code, gated by U5] Wire buy links for the two FCC-clear SKUs**
  (`wap-parts`, `vision-parts` in `store.json`) once Stripe exists. Everything
  else stays waitlist-only until U4.

- [ ] **W6 [code, paired with U4/U5 work] Supplier quotes are placeholders** —
  5 rows in `suppliers.json` marked `"status": "placeholder"`. When real RFQs
  land, also invert `tests/quote-compare.edge.test.mjs` ("honesty flag" test
  currently asserts placeholders exist — it will fail the moment they don't).
  *Note (2026-11-04, #1920):* the test half closed in website #230
  (`a80397a`): `tests/quote-compare.edge.test.mjs`'s honesty flag derives each
  route's status from `suppliers.json` instead of asserting that placeholder
  rows exist, so the day real quotes land does not turn CI red, and a shape
  test requires a status on every routed leg. The five placeholder rows stay
  until real RFQs exist (U4/U5).

- [ ] **W10 [human] Tizen TV package has `REPLACEME` app IDs**
  (`tv/config.xml`; it was `tv/tizen/config.xml` before the manifests moved)
  — needs Samsung Seller Office issuance before any submission. The same
  human, with the vendor SDKs: run `tizen build-web -- tv/` and
  `ares-package tv/` once to confirm the moved manifests still package (both
  will also pick up `tv/README.md` and `tv/cast/` unless excluded), and settle
  W24's store wording first.

- [ ] **W11 [human] Set `ANTHROPIC_API_KEY` so the advisory review runs.**
  Each repo's `claude-review.yml` skips with a warning when the secret is
  unset (seen on real runs, 2026-10-13), so a green "Canary Reviewer" check
  means nothing was reviewed. Set it under Settings → Secrets and variables →
  Actions in the monorepo and the website (and under Dependabot secrets if
  bot PRs should be reviewed).

- [ ] **W13 [code, needs a GPU session] Render quality — W9's GPU half.** (1)
  Cinematic mode: the route is decided in `docs/render-roadmap.md` (a WebGL2
  path tracer, `three-gpu-pathtracer` on `three-mesh-bvh`, lazily imported on
  opt-in, not a WebGPU rewrite); building it needs a GPU session and a
  bundle-size review. (2) The micro-wear roughness map spans G 241–254 (14
  levels) and is "not yet judged on a GPU": choose its amplitude, and whether
  to normalize the peak to G = 255, in the Render Lab — a textures test pins
  the range the roadmap states, so move the map and the roadmap together.

- [ ] **W14 [code+decision] Homepage reduction, the editorial half** (W8's
  remainder; founder items 1 and 3 in `docs/ia-homepage-2026-09.md` "Open —
  human items"). Relocate the timeline (`.timeline-section`, "Same camera.
  Same question. Different futures.") to `witness.html` or cut it, then
  rewrite the hero description and `#witness-pitch` to the review's order:
  hero → three-part product → what leaves the device → four paths → proof.
  Before the timeline can be cut, "rules apply forward only" needs an owner
  off the homepage: only the timeline's Day-180 row says it today, and its
  natural home (`how-it-works#constraints`) is headed "Three intentional
  constraints", so a fourth is a copy decision. Fifteen tests read
  `index.html`; the manifest lists them. Then prune the CSS families the
  mechanical cut left unused.
  *Note (2026-11-04, #1920):* the forward-only premise closed in the website's
  `98ef90f` ("Cleanup audit, part 1"): `how-it-works.html`'s constraints
  ladder carries "Rules apply forward only" as invariant VI, its home off the
  homepage, and `index.html`'s timeline row points at it, so the timeline can
  be cut without losing the rule. The editorial half (relocating or cutting
  the timeline, the hero and `#witness-pitch` rewrite) is the founder's.

- [ ] **W15 [decision] The CANARY trademark exposure, and "Witness" as the
  hero noun.** The claims audit's one structural item, still open after W7:
  CANARY is a live U.S. registered trademark for home-security cameras, the
  options are a clearance opinion, repositioning "canary" as a metaphor, or a
  rename, and the audit says not to scale store sales until it is settled
  (`docs/claims-audit-2026-07.md`), which bears on U5/W1. The IA
  review's hedge, W8's item (d), is to build the public hierarchy around
  *witness* and keep *Canary* as the affectionate product name; renaming
  surfaces is a founder decision, and the dormant hero B variant
  (`data-hero="b"`, "Three things. One witness.", `js/exp-hero.js`
  `ROLLOUT = 0`) is the cheapest place to try it (open item 2 in
  `docs/ia-homepage-2026-09.md`).

- [ ] **W16 [code+human] The claims-audit rows W7 did not cover**, listed in
  `docs/claims-audit-2026-07.md`'s "Status 2026-09-22" block: "architecturally
  incapable" (`compare.html`); the "nothing phones home / anywhere" family
  (M2: `download.html`, `lab.html`, `compare.html` and `watch-over.html`'s
  hero fine print — W8's trim removed only the second of watch-over's two);
  the recording-consent notice (H3); and a contact mailing address and role
  email plus the privacy policy's date (M4 / L3), which need facts only a
  human has. Give each fix a guard in `tests/legal-claims.test.mjs`, as W7
  did.
  *Code half done (website #203):*
  - "Architecturally incapable" names what is missing.
  - The phone-home family and its siblings ("LAN-only", "local-only",
    "never online") are gone from the site. The FAQ names each routine
    trip online and the iPhone companion's iCloud default.
  - A recording-consent notice, pinned verbatim, is on the store and on
    First Flights.
  - `tests/legal-claims.test.mjs` scans each surface as flowing text, so an
    aside, a tag, a line break or a no-break-space entity cannot split a
    phrase.

  Still open (human): the contact mailing address, a role email and the
  privacy policy's date (M4/L3). Recorded for the same founder pass: the
  privacy policy says the site fetches nothing external, while every page
  loads Google Fonts. The audit's monorepo follow-up list (the Lab's
  "nothing here phones anywhere" footers, `hatch.json`'s motto,
  `docs/FAQ.md`, `docs/getting_started_canary.md`) and Compare's unsourced
  "auto-deletes in 24 h" cell are the wave-4 claims-followup package.
  `docs/FAQ.md`'s phone-home answer now names the Canaries' disclosed
  outbound paths and the desktop apps' update fetch (#1722): the broker, the
  signed update check, the display's SNTP and its opt-in forecast, linked
  to `SECURITY_MODEL.md`'s list. It still opens with "No.", which website
  #203 ruled out for the site's FAQ, so it stays on that follow-up list
  with `docs/getting_started_canary.md`, the Lab's footers and
  `hatch.json`'s motto.
  *Note (2026-11-04, #1920):* two halves closed on the website since. #230
  (`a80397a`) took Compare's unsourced "auto-deletes in 24 h" cell: the
  footage cell says the clips stay in Frigate and the kernel records no video,
  the deletion cell scopes "in code" to the kernel's signed event log (pruned
  after 7 days by default), a note gives each retention with its mechanism and
  source line, and `tests/legal-claims.test.mjs` refuses a deletion word near
  "24 hours" and an unsourced duration in a footage clause. #245 (`2b77fc9`)
  gave `privacy.html` the iPhone, iPad and Apple Watch app's section and moved
  its date to October 29, 2026. Still human: the contact mailing address, a
  role email and the Google Fonts call.

- [ ] **W37 [decision] The night contrast pass holds normal-size text to 3:1,
  not 4.5:1.** It fails only under 3:1 (WCAG's large-text and UI-component
  floor), so normal-size text between 3:1 and 4.5:1 passes (lab.html's stage 5
  and 6 numerals read 3.7:1, white on teal and green, the same by day). Decide
  whether normal-size text is held to 4.5:1, which would make the pass a
  per-element size check (the light-ink guard's state-fill check already uses
  4.5:1). Found by W34 (the website PR, securacv_website#228).

- [ ] **W38 [decision] Six inactive states fade under 3:1 in both themes.**
  checkup.html's unreached BLE rungs (`.rung.dim`, opacity .45), off firmware
  pills (`.fw-pill.off`) and off tiles (`.tile.off`), explainer.html's gone
  rows (`.row.gone`), linux.html's locked capability tags (`.cap.locked`) and
  modes.html's unplayed beats (`.beat` not yet `.fired`) read 1.5 to 2.8:1 by
  day and by night; the night pass excuses them through its DIMMED list only
  because the same text reads 3:1 undimmed. Decide whether an inactive state
  keeps a readable fade (3:1 or better, by opacity or a muted ink); if so,
  restyle the six and delete DIMMED. Found by W34 (the website PR,
  securacv_website#228).

- [ ] **W24 [decision] The TV app's store text says "verified record".**
  `tv/appinfo.json`'s `appDescription`, `tv/config.xml`'s `<description>` and
  `tv/README.md`'s opening line call the Wall "a calm, always-on view of your
  Canaries' verified record", and `tv/app.js` and the README say the emulator
  simulates one. `verify_core.js` checks an envelope against its own
  `device_public_key` unless a trusted key is supplied, and on real hubs the
  Wall is presence-only, so under the house rule ("verified" only for an
  Ed25519 signature checked against a pinned key) the wording needs a human
  call before any store submission (W10): keep the word only where a pinned
  key is in fact used, or say "sealed", as the staged emulator labels in
  `js/tv-emulator.js` do since the 2026-10 cleanup.

- [ ] **W25 [code+decision] The kit cards live on both Compare and Store.**
  Both pages carry the "2 of 4 designed Canaries shipped" count, and
  `compare.html` keeps a `#kits` grid that the store repeats. Make `/store`
  the single owner of the kit cards and the count, and cut the `#kits` grid to
  one line that links there. It touches `tests/coming-soon.test.mjs` and
  `tests/availability.test.mjs` (both key on `compare.html`'s badges), the
  `/paths` "Get notified" door (`/compare#kits`) and `index.html`'s "2 of 4"
  line, which links `/compare#kits`. An editorial call; the current text is
  true. Found by the 2026-10 cleanup's site review.

- [ ] **W26 [human, before U5's payment links] `privacy.html` names no
  retention period for order records.** The page says what a store order
  shares (Stripe takes the payment; name and shipping address go on the USPS
  label), but its Data Retention section covers only contact messages
  ("typically no longer than 2 years"). Before any `store.json` payment link
  goes live: (1) state how long order records (name, shipping address, email)
  are kept; (2) review the GDPR paragraph, whose only stated legal basis is
  consent, since order data is usually processed to perform a contract; (3)
  decide whether Stripe belongs under "Service providers … under strict
  confidentiality agreements" or only under the "Store orders" bullet. Facts
  and a legal call only the owner has.

- [ ] **W27 [decision] The warrant canary has no stated refresh cadence.**
  `transparency.html` reads "Statement current as of: July 24, 2026" and names
  no schedule, so a reader cannot tell a lapsed statement from a current one.
  Pick a cadence (the 2026-10 review's example: the first business day of each
  quarter, and a statement older than 100 days is to be read as lapsed), say
  it on the page, then re-affirm each statement and move the date. Only the
  owner can re-affirm them.

- [ ] **W28 [human] `terms-of-sale.html` describes a store that sells kits and
  devices.** §1, §4, §6, §9 and the short version say "Kits are built and
  tested in small weekly batches" and "We only sell devices whose firmware has
  been released", and the short version promises "Anything dead on arrival is
  replaced" while the §6 table's row reads "A board is dead on arrival". The
  store sells printed enclosure sets only, behind the compliance gate (U4),
  and the website's AGENTS.md rule 2 says to call them "enclosure sets," never
  "kits." Rewrite the terms for what is sold, decide which dead-on-arrival
  scope is meant and make both places say it, and consider stating the
  compliance gate in §1. Legal text: the owner's call (with D12).

- [ ] **W29 [decision] A one-trustee break-glass quorum is legal, and the site
  copy assumes two or more.** `QuorumPolicy::validate`
  (`src/break_glass/core.rs`), and so `break_glass policy set`, accepts a
  threshold of 1 (it refuses only 0 and a threshold above the trustee count),
  so a 1-of-N vault is a valid configuration. Much of the site assumes at
  least two: `vault.html`'s deck and its "One approval doesn't." pillar, How
  It Works' "requires approval from multiple trustees", and the glossary's
  break-glass entry. Decide: the kernel refuses (or warns on) a threshold
  below 2, or the copy says "the quorum you set". A product call across the
  kernel and the site. Found by the 2026-10 cleanup's site review.

- [ ] **W30 [human] The website's agent brief undercounts what a new page
  needs.** `tests/site-map.test.mjs` now also requires a `PAGE_WORDS` line in
  `js/canary-search.js` for every `SITE_MAP` route, but the website's
  `AGENTS.md` rule 6 says "Adding a page means five things" and its
  `CLAUDE.md` says "the five things adding a page requires". Update both
  together (the `CLAUDE.md` edit is a human's, which is why the 2026-10
  integrator left both), then run `node scripts/make-agent-entrypoints.mjs`.

---

## 5. Hardware, CAD, print files (monorepo)

The canonical ledger is `docs/hardware/enclosure/AUDIT_2026_09.md` ("Open —
major, by theme") — work its themes, then tick here.

- [ ] **C4 [decision] The C6 `brass_h` disagreement** — registry says 5.0, the
  file prints the C3's measured 3.0; `devices/README.md` says owning either
  number would bless it. A maintainer measures and decides.

- [ ] **C6 [human] Caliper passes for the board registry** — 2 of 9
  `BRD_REGISTRY` rows are measured; `ws147`/`ws169` (which drive three
  display cases) are vendor-drawing rungs; ~2,460 `MEASURE` tags across 26
  `.scad` files. Pairs with U1.

- [ ] **C7 [human] Print-validate the in-development enclosures** — only 4 of
  ~35 printable designs have committed STLs
  (`docs/hardware/enclosure/README.md` "In development" lists 22). Print,
  fit-check, then commit per the regen order in CLAUDE.md
  (`scripts/regen_cad.py`). Every environmental rating in `field_ratings.md`
  is also still "untested" — that protocol needs real units too.

- [ ] **C10 [code] C1's remaining waves.** Wave 1 (#1703) moved the help that
  was already written; the rest is writing it. Help for the 536 knobs that
  have none, the released four first (their help carries to the website
  builder's Advanced accordion); `[min:step:max]` ranges where a range is
  known; one group name for the two-stud interface; presets for the doorbell
  and the Sense, modeled on `canary_wap_enclosure.scad`'s `preset` with
  defaults unchanged so no STL moves. Also the 7" case
  (`canary_s3_lcd7.scad`): its four shared-help lines (9 knobs) and its
  above-line comments were left out of wave 1 by instruction and sit in
  `lint_design_lang.py`'s `HELP_LINE_DEBT`; `gen_stamp`'s digest ignores
  comments and whitespace, so splitting them needs no STAMP_REV bump — delete
  the debt entries with the split. The Dash's and the J-box's `usb_w` /
  `usb_h` still share a line (no help to lose). Mind the Sense's `usb_h`: its
  help is only a `[4:0.5:8]` range, and `gen_enclosures.py` reads a range only
  at the start of a comment, so wave 1 left it alone rather than risk its
  slider. `DESIGN_RULES.md` §11 "Customizer help text" names the first four.
  *Wave 2 done (#1718, website #203):*
  - The 7" case's shared-help lines are split and `HELP_LINE_DEBT` is
    empty. The Dash's and the J-box's `usb_w`/`usb_h` are split.
  - Every knob in the released four (WAP, Vision, doorbell, Sense) has help
    read off the geometry that uses it, except the option-list selectors.
  - Stated ranges sit on their knob's line, and `gen_enclosures.py` reads a
    range where the builder does (43 knobs had been a slider in one and raw
    text in the other).
  - One group name, "Stud/keyhole interface", across 16 files, held by a
    new `lint_design_lang.py` rule.
  - Presets for the doorbell (`doorbell_weather`) and the Sense
    (`sense_wall`, `sense_ceiling`), with defaults unchanged (preset
    contents and the group name: maintainer to confirm). A new check
    refuses a preset that overrides a control the builder does not gray
    out. It caught the WAP's and the Vision's presets leaving `opt_weep`
    live.
  - Knobs without help: 535 → 347 of 1743. The CSG of every touched part
    is byte-identical to the base.

  *Wave 3 (#1802):*
  - The Lab's group headers (C10a): `canary-local/tools/gen_enclosures.py`
    imports the web builder's own `GROUP_RE`, so the 52 multi-line `/* [Group]
    ... */` headers in 10 case files group their knobs in the Lab too. A
    header's continuation lines become the group's note, with only the comment
    gutter stripped, and are never read as knobs. 335 Lab knobs changed group,
    333 of them knobs the builder also reads; the WAP's `tol_slide` now sits
    in "Print tolerances", not "Shell". One group drops out:
    `canary_s3_touch169.scad`'s empty [Side cluster] header (C22).
    `scripts/tests/test_enclosure_parsers.py` holds both parsers to one group
    for all 1726 knobs they share, holds the committed `enclosures.json` to
    the builder's groups (1723 knobs), and holds `gen_enclosures.py` to that
    one import as `GROUP_RE`'s only binding. Only `enclosures.json` moved.
    Decision (maintainer to confirm): one group rule, the builder's, imported
    rather than copied. Rejected: a Lab-only multi-line matcher, which would
    be a second rule to drift. The default "stop at the first module" was not
    taken: it drops 317 Lab entries (230 names), none read by name by any
    consumer, but it moves `catalog.json`'s `engineering_param_count` for 13
    products, which this wave kept byte-identical (C21).
  - The J-box's pass-through (C10b): `canary_jbox.scad`'s `usb_w`/`usb_h`, the
    slot a pigtail cable runs out through, are now `pigtail_w`/`pigtail_h`
    with help, and the file's comments call it the pigtail slot.
    `KNOB_MEANINGS` refuses a pigtail help under `usb_*` and boot clearance
    under `pigtail_*`. `opt_weep`'s help, which is also its catalog label,
    still says "USB slot" and moves with the catalog decision below. Decision
    (maintainer to confirm): a name of its own. Rejected: keeping `usb_w` with
    a `deviates:` note, since a name with two meanings is what C2 removed. The
    `DESIGN_RULES.md` §10 row for the new name is handed to the integrator.
  - Help for the three monorepo-only dev cases (C10c-1): in
    `canary_field_case.scad`, `canary_jbox.scad` and
    `canary_bench_fixture.scad`, every knob alone on its line has help read
    off the geometry that uses it (27), except the field case's `bez_on` (a
    catalog option whose help would become its label) and the bare `part`
    selectors. The bench fixture's `mag_h` says no geometry reads it and where
    the slider's pocket sits. Writing it corrected one comment: the bench
    fixture's reed channel axis sits `reed_d/2 - 0.3` below the face (the
    comment said 0.3). The field case's [Screw lobes] header now says 8x
    inserts, as `lobes()` places (two docs still say six, C23).
    `scripts/tests/test_lint_design_lang.py`'s `HELP_DEBT` holds the rest
    shrink-only at 23/12/7: 41 knobs that share a line, and `bez_on`. Decision
    (maintainer to confirm): these three cases first, held by a shrink-only
    table. Rejected: invented ranges (the geometry states none).
  - No geometry moved: the CSG of all 12 part options of the three cases is
    byte-identical to the base under OpenSCAD 2021.01, and their 24 PNG
    previews show no change. The package left `catalog.json`, `build.json`,
    `workshop.json` and `figures.json` byte-identical. On the integrated tree
    `test_lint_design_lang.py` and `test_enclosure_parsers.py` pass (33).
    Host-tested; nothing printed.

  Still open: the remaining development-case knobs without help, the three dev
  cases' 42 among them, whose shared lines must be split, which moves
  `catalog.json`'s `engineering_param_count` (the package's patch for that
  split, handed to the integrator with #1802 and not applied, also gives
  `bez_on` and `opt_weep` their help, and those helps become their catalog
  labels; decision pending); the Lab stopping at the first module like the
  builder (C21, which also moves `engineering_param_count`); and the
  maintainer call on the OpenSCAD 2021.01 desktop Customizer, which shows
  plain boxes for the house `help // [range]` form (`DESIGN_RULES.md` §11).
  *Correction to the wave 3 brief:* stopping the Lab parser at the first
  module drops 317 entries (230 distinct names), not about 1015. The 1015
  counts every name the old Lab parser read that the builder does not: 317
  past the first module, 603 expression-valued (derived) assignments before
  it, and 95 in [Hidden] groups. The C10c-1 counts (33/21/18) include each
  file's `part` selector, which the house leaves bare; without the selectors
  the three files had 32/20/17 knobs without help.

  *Wave 4 (#1836):*
  - Help for the next monorepo-only cases (C10c-2): in 19 case files (the C3
    pocket case, the solar relay, the sign, the vehicle mount, the Hammond
    plate, the wear clip, the Sense bedside stand and gang plate, the inserts,
    the Pi hub, the mount adapters, the Vision Pro plate, the shop tools, the
    dock, the outlet cradle, the 7-inch frame, the S3 stick, the 1.69 case and
    the C6 display), every knob has help read off the geometry and asserts
    that use it. That is 133 knobs: 76 alone on their lines, 36 on eleven
    shared lines now split one knob per line, and 21 boolean options, whose
    help is also their catalog label. Seven knobs that no geometry reads say
    so (C29): the gang plate's `board_clear`, the mount adapters' `tol_slide`
    and `tol_hole`, the vehicle mount's two tolerances, the S3 stick's
    `tol_hole` and the C6's `snap_n`. Writing it found three things the help
    now states: the solar relay's cavity, 17.7 deep at the defaults, clears
    `bh_h` + 1.5 while an 18650 is 18 mm across (C28); the cable gland's
    overfill assert compares `cg_fill` with itself (C34); and the 7-inch
    frame's brand-word width estimate reads about 25 % short of the cut words
    (C33). It also found the builder parser giving the 1.69 case's
    `skirt_wall` the help of its computed neighbor `skirt_dep`, which the lint
    and `HELP_DEBT` counted as help (no surface showed it: the Lab skipped the
    line and the builder does not offer the case); that line is split too, a
    twelfth, and a test on each parser holds it (the lint rule that let it
    pass is C32). `scripts/tests/test_lint_design_lang.py`'s `HELP_DEBT` holds
    all 19 at zero. Knobs without help: 246 -> 113 of 1807 (the 81 option
    selectors are not counted). No geometry moved: the CSG of 104 part options
    and 27 other selector sets is byte-identical to the base under OpenSCAD
    2021.01, the comment-stripped token streams are identical, and 207 of the
    208 PNG previews are byte-identical to base renders (the 7-inch frame's
    underside view varies from run to run in both trees; 14 parts, 28 of the
    208 PNGs, render blank at the defaults on the base too, nine of them gates
    whose pass is an empty render). The wave leaves `build.json`, `workshop.json`,
    `figures.json` and `builder_manifest.json` as they were; only
    `enclosures.json` and `catalog.json` regenerate. Host-tested; nothing
    printed.
  - Two maintainer decisions ride on it, each in commits of its own so they
    can be dropped alone (C27). The shared-line splits (the package's e29b9282
    and 33433c38, cherry-picked here as 222f6827 and efb87fbd; the second's
    cherry-pick conflicted in `catalog.json`, and the resolved counts are the
    package's): the Lab lists the 36 knobs it skipped, three of them read by
    nothing (the C6's `snap_n`, the vehicle mount's `tol_slide` and
    `tol_press`), and the 1.69 case's `skirt_wall` and its computed
    `skirt_dep`: 38 entries, 10 of them in three groups it never showed (the
    C3 case's and the vehicle mount's Print tolerances, the Hammond plate's
    Board snap clips) and 28 in groups it already showed, and `catalog.json`'s `engineering_param_count` moves for seven products
    (c3_lcd147 176 -> 182, c6_display 88 -> 92, hammond_chassis 26 -> 31,
    relay_solar 100 -> 107, s3_touch169 94 -> 100, vehicle_mount 19 -> 26,
    wear_clip 15 -> 18), as it reads at this head. And the 21 boolean helps
    (08e44df1, here c5abc963, with two labels reworded by the review fix, here
    1c9f871d), which replace bare knob names such as `opt_btn` as Lab catalog
    labels. Rejected: waiting for C21, which moves the same counts and leaves
    the knobs without help. Two commit bodies overstate and cannot be amended:
    efb87fbd says the builder and the Lab showed `skirt_dep`'s help on
    `skirt_wall` (neither did), and b28df079 calls 0.392 x `edge_text_size`
    the per-letter width (it is the half-width; the help says so since
    1c9f871d).

  Still open after wave 4: 113 knobs without help, namely the carried four's
  55 (combo 33, fit_coupon 10, dash_display 7, watch_station 5; each edit
  moves a website sha256 pin, and watch_station needs a `gen_print_files.py`
  re-render), C10c-1's 42 (its handed-up shared-line patch for the field case,
  the J-box and the bench fixture is C27's decision too, and is still not
  applied), `canary_templates_2d.scad`'s 12 (C30) and 4 in the released
  doorbell and Vision (C31); the Lab stopping at the first module like the
  builder (C21); and the maintainer call on the OpenSCAD 2021.01 desktop
  Customizer.

- [ ] **C21 [code+decision] The Lab's knob list is not the builder's.** After
  C10a's group fix, `canary-local/tools/gen_enclosures.py`'s `parse_scad`
  still differs from `gen_builder_manifest.parse_scad` in three ways. First,
  it reads one `name = value;` per line, so 136 knobs the builder reads are
  missing from `enclosures.json`, all on shared lines (`lip_h = 3.0;  lip_t =
  1.2;`). Second, it lists the Customizer's [Hidden] groups (95 entries) and
  expression-valued derived assignments (603 before the first module), which
  neither the OpenSCAD Customizer nor the builder offers. Third, it reads past
  the first `module` (317 entries). Each fix moves `catalog.json`'s
  `engineering_param_count`, which is the decision.
  `scripts/tests/test_enclosure_parsers.py` would grow a knob-set comparison
  once the parsers agree. Found by C10a and C10c-1 (#1802).
  *Since C10c-2 (#1836):* 37 of those shared-line knobs (C10's wave 4: eleven
  lines in seven cases and the 1.69 case's `skirt_wall`) sit on their own
  lines, so the Lab lists them and the builder's and the Lab's lists agree on
  them; the rest of the first difference, and the other two, are unchanged.
  Whether those splits stand is C27.

- [ ] **C27 [decision] Confirm or revert C10c-2's two decision commits.** Wave
  4 of C10 (#1836) carries two commits the package marked as maintainer
  decisions, each droppable alone. (1) The shared-line splits: e29b9282, here
  222f6827 (eleven lines in seven cases, 36 knobs, three read by nothing),
  with 33433c38, here efb87fbd (the 1.69 case's `skirt_wall` / `skirt_dep`
  line): the Lab gains 38 entries (37 skipped knobs and the computed
  `skirt_dep`), 10 of them in three groups it never showed (the Hammond
  plate's Board snap clips, the C3 case's and the vehicle mount's Print
  tolerances), and `catalog.json`'s `engineering_param_count` moves for seven products (c3_lcd147 +6, c6_display
  +4, hammond_chassis +5, relay_solar +7, s3_touch169 +6, vehicle_mount +7,
  wear_clip +3). (2) 08e44df1, here c5abc963: help for 21 boolean options,
  which becomes their Lab catalog label in place of the bare knob name (two
  labels reworded by 1c9f871d). Confirm both, or revert either commit and
  regenerate `enclosures.json` and `catalog.json` (`gen_enclosures.py`).
  C10c-1's handed-up patch for the field case, the J-box and the bench fixture
  (help for 42 knobs, `bez_on` among them, and the J-box `opt_weep` label) is
  the same decision and waits on this one. Found by C10c-2 (#1836).

- [ ] **C28 [code+decision] The solar relay's cavity is shallower than a
  fitted 18650.** `canary_relay_solar.scad` sizes the cavity as `cav_d =
  max(standoff_h + pcb_t + lb_stack, bh_h) + 1.5`, which is 17.7 at the
  defaults (the LoRa stack sets it; an echo probe confirms). `bh_h = 15.0` is
  the only battery height it reads, and the holder lies flat on the plate (the
  bay's pocket does not go into the floor). Its help reads `bh_h` as the
  holder's height with its cell fitted, which is what the cavity must clear,
  and an 18650 is 18 mm across, so a fitted holder stands at least 18 mm off
  the plate, into the face's underside. Measure a holder with its cell, then
  grow the default or the cavity formula, and re-check the posts, the roof
  screws' pilots and the fit-check. Since #1836 (C10c-2) the help asks for the
  measurement; the geometry is unchanged. Found by C10c-2 (#1836).

- [ ] **C29 [decision] Seven knobs no geometry reads are still offered.**
  `canary_sense_gang.scad`'s `board_clear` (the carrier is located by rails
  and clips), `canary_mount_adapters.scad`'s `tol_slide` and `tol_hole` (its
  wall-screw holes are `screw_d` as drawn), `canary_vehicle_mount.scad`'s
  `tol_slide` and `tol_press`, `canary_s3_lcd147.scad`'s `tol_hole` (the case
  has no screws) and `canary_c6_display.scad`'s `snap_n` (the nubs are always
  two per side wall) are literal knobs, so the OpenSCAD Customizer and the Lab
  list them, and turning them does nothing. Since #1836 (C10c-2) their help
  says so. For each, either remove it, which moves `catalog.json`'s
  `engineering_param_count`, or wire it where a fit needs it, which is a
  geometry change with previews. Found by C10c-2 (#1836).

- [ ] **C30 [code] The paper templates retype other cases' numbers.**
  `canary_templates_2d.scad`'s [Bracket] and [Doorbell] knobs are hand copies
  ('mirror canary_vision_enclosure bracket defaults', 'mirror
  canary_vision_doorbell plate defaults'): 12 knobs on four shared lines with
  no help, and the builder's parser drops two trailing notes on those lines.
  Today `br_x`/`br_y`/`br_screw_d` (46/34/4.2) match the Vision bracket, and
  `db_out_x`/`db_out_y` (51.5/169.2) and `db_exit_dx` (2.2) match the
  doorbell's echo. The screw insets and `db_exit_cy` were not checked. Read
  the values through `use <>` (the outlet cradle's `target_gap()` pattern), or
  hold them to their sources with a check, then split and help the lines. The
  file is in `lint_design_lang.py`'s SKIP list as not a case, so C10c-2 left
  it out. Found by C10c-2 (#1836).

- [ ] **C31 [code] The released doorbell and Vision have knobs without help
  again.** C10 gave every knob in the released four help except the option
  selectors. At #1836's head, `canary_vision_doorbell.scad` has
  `svc_pip_depth` (:390), `ant_rib_d` (:472) and `spk_post_dy` (:669) without
  help (added by the doorbell v0.8/v0.9 work), and
  `canary_vision_enclosure.scad` has `span_max` (:436). Both files are
  carried, so the fix rides the carried batch: the `gen_builder_manifest.py
  --site` carry, the website's builder-facts pins and `builder_manifest.json`.
  Add the released four to `test_lint_design_lang.py`'s `HELP_DEBT` at zero in
  the same change, so a new released knob cannot land without help. Found by
  C10c-2 (#1836).

- [ ] **C33 [code] The 7-inch frame's brand-word width estimate reads about 25
  % short.** `canary_s3_lcd7.scad` sizes the bottom-wall words with
  `vword_half = 0.392 x edge_text_size x letters` (:2137), from '0.70 em x
  1.12 tracking' (:2133), but OpenSCAD 2021.01's text size is not the em:
  SECURACV at size 7, spacing 1.12, in Liberation Sans Bold inks 58.7 mm (x
  -29.56..29.16 in a DXF export) against the 43.9 mm estimate. The asserts
  that keep the leash anchor, the peel scoop, the port label and the dock keys
  off the words (:2557, :2574, :2587, :2785) therefore guard a shorter word
  than the one cut. At the defaults the cut word reaches x = 13.74, inside the
  anchor's and the scoop's 1 mm keep-outs (bounds 15.2 and 15.44), though the
  C10c-2 review's CGAL intersection with the scoop was empty, and `port_lbl_a`
  ships empty. Calibrate the factor against a measured render (a DXF of the
  default words) or carry measured widths, and fix the :1968 comment, which
  calls 0.392 the advance where `_lbl_w` doubles it. Since #1836 (C10c-2) the
  `edge_text_size` help says the estimate reads short. Found by the C10c-2
  review (#1836).

- [ ] **C11 [decision] Raise the lid ribs, or leave them** (C3's rib half; the
  audit's "Ribs proportioned backwards"). The evidence is recorded and no rib
  changed: `DESIGN_RULES.md` §11's "Lid rib proportions" table gives the
  headroom each committed preset leaves over its tallest component, read off
  the CAD by `gen_hardware.py` and held there by its `--check`. Four presets
  are pinned at zero slack (doorbell, Sense, Vision DevKit indoor, WAP
  battery_full) and four have 0.35–3.58 mm, so no per-file `lid_rib_h` can
  rise without growing `cav_extra` (which moves the released envelopes and
  every figure and AR model after them) or becoming preset-derived, and the
  WAP's rib doubles as the battery hold-down (`batt_hold`). The headroom is
  bounded by each preset's tallest component, which is not a geometric
  clearance proof; a true rib-versus-component probe needs per-case
  component-envelope modules in the released case files. The table's
  candidate is a bool `lid_rib_fill`: the rib grows to the preset's own slack,
  the WAP's battery sets excluded, the four pinned presets stay
  byte-identical, only the slack presets' lids and fronts move (their
  envelopes do not), shipped through `scripts/regen_cad.py --previews`. A
  maintainer decides, per case.

- [ ] **C12 [human] The BOM disagreements `gen_hardware.py` found (12 at its
  first run, 8 today).** Its
  first run joined each committed preset's fasteners to the BOM CSVs and
  listed what disagrees in `docs/hardware/enclosure/hardware.json`
  (`bom_drift`): two short quantities in `bom_canary_vision.csv` (the doorbell
  echoes 10 M2 screws against SCR5's 8, and 7 inserts against INS1's 5) and
  ten fasteners no row bills (mostly #6 wall screws; the Sense's M2 pan
  self-taps and its hinge bolt; the WAP weather base's M3 wall screws and its
  shield's replacement lid screws). The generator never edits a CSV: someone
  who knows the build decides which side is right, fixes the CSV (or the CAD),
  and shrinks `KNOWN_DRIFT`, whose `--check` fails when a listed entry stops
  happening. Same pass: `bom_canary_display.csv`'s D-ENC1 still describes the
  Dash back's retired keyholes and M4 pair.
  *Update (#1836):* `hardware.json` `bom_drift` lists 8 today. SCR5 is no
  longer short (18, the doorbell's 12+4+2 echoed screws); INS1 is the one
  short quantity left (5 against the 13 inserts set `doorbell+inserts`
  echoes); the other seven are unbilled: the #6 bracket wall screws of the
  Sense and the three Vision presets, the Vision weather preset's #6 keyhole
  screws, the Sense's M5 hinge bolt and nut, and the WAP weather base's M3
  wall screws.
  *Note (2026-11-04, #1916, C26):* `assembly.test.js` now holds every counted
  Assemble part to its BOM row and lists `canary-vision/screws` in a
  shrink-only `NOT_COMPARED`, because SCR5 bills the Vision case and the
  doorbell in one quantity. Splitting that row per product is this item's
  call; the entry fails once it is stale, so the split removes it in the same
  change and gives the Vision's screws their `ref`.

- [ ] **C25 [code+decision] The Assemble tab's two fastener gaps.**
  `canary-local/tests/assembly.test.js` lists them in its shrink-only
  `FASTENER_GAPS` (#1836, beside C12). (1) The Vision tab seats the OV5647 on
  the front's posts but draws none of the four M2 pan x 6 self-tap screws
  `canary_vision_enclosure.scad` echoes for it (set `vision.xiao_indoor`): add
  a camera-screw part posed from the camera posts in
  `docs/hardware/enclosure/gen_assembly_poses.py`, or decide the tab leaves
  them out and say so in its step. (2) `canary_dash_display.scad` prints no
  `HARDWARE` line, so no `gen_hardware.py` set counts the four D-SCR1 screws
  the Dash tab drives: give the case an `hw_echo()` and `gen_hardware.py` a
  set (with a JOIN against `bom_canary_display.csv`'s D- rows), or decide the
  in-development case stays uncounted. Each fix shrinks `FASTENER_GAPS`, and
  the test fails until it does. Found by C3 (#1836).

- [ ] **C16 [decision] No printed part holds the Dash portrait.**
  `glass_settings.h` offers the dash glass turned portrait (`ROT_PORTRAIT`,
  "for a wall column or a tall bedside face"), but
  `canary_dash_display.scad`'s desk stand is cut for the landscape case (the
  90-degree plug's channel under the USB-C in the case's bottom wall, the
  screw lobes standing on the pedestal outside the lip) and its wall cradle's
  four dock features sit at ±38 x ±16, a landscape rectangle; turned a
  quarter, neither takes the case. The Lab draws a turned dash standing alone
  for that reason (A47). Decide: a portrait mount (a cradle with a second dock
  pattern, or a stand cut for the case on its side), or the portrait setting's
  copy saying it needs one. Found by A47's review (#1762).

- [ ] **C17 [decision] Which lights the C3-LCD-1.47 has.**
  `docs/hardware/enclosure/canary_c3_lcd147.scad` cuts its light band for an
  onboard RGB LED and keeps the USB end lit for a charge LED, while
  `firmware/boards/waveshare-esp32c3-lcd147/pins/pins.h` (`HAS_RGBLED 0`, no
  WS2812) and the board README (no WS2812, no battery charger IC) describe a
  board with neither. The Lab's Nightlight bench lists no lights until a look
  at a board settles which file is wrong, and that file needs correcting.
  Found by A54 (#1762).

- [ ] **C18 [decision] Which way the Touch 1.69's case holds its glass, and
  what its figure draws.** `docs/hardware/enclosure/canary_s3_touch169.scad`
  holds the 240x280 panel landscape: its active-area window is 32.634 x 27.972
  (exactly 280:240) with the 280 px axis along X, and `glass_w` comes from the
  registry's `brd_ws169_glass_h()`. The firmware draws the panel at its native
  portrait 240x280 (the pin map's `LCD_WIDTH` 240, `LCD_HEIGHT` 280). A unit
  cut to the CAD as it stands therefore shows the face sideways. The Lab's
  touch169 figure (`canary-local/tools/figures/massing.mjs`) is a sketch while
  the case STL is gitignored. Its lit face, 36.13 x 28.13, is the panel
  record's glass less a hand-typed 2.5 mm inset, not the case's active area.
  Since A55 the Lab sets the canvas whole into that face, upright, with dark
  bands beside it. That is a drawing choice that matches neither the CAD-cut
  unit nor a portrait one. Options: (a) the case holds the glass portrait,
  through a `cad.params`/registry change in CLAUDE.md's CAD regen chain, with
  previews; (b) the firmware wears a quarter turn on this board, a rotation
  plus a 280x240 layout pass, held by `glass_turn.sh`-style tests; (c) either
  way, source the figure's lit face from the case's `aa_w` x `aa_h` in
  `massing.mjs`, then run CLAUDE.md's figure chain, so the figure stops being
  a sketch inset. Either (a) or (b) turns the Lab's fit off for this card,
  since the canvas and its face would then be the same way round and sampled
  edge to edge. Against today's sketch face, though, the canvas would then
  stretch about 10%: 28.13 x 36.13 is 0.778 against the canvas's 0.857 for
  (a), and 36.13 x 28.13 is 1.284 against 280x240's 1.167 for (b). So (c)
  belongs with whichever is chosen. Found by A55 (#1802).

- [ ] **C37 [code+decision] Map or drop the six UNMAPPED build-matrix
  claims.** Since C36 (#1920) `scripts/lint_build_matrix.py` lists the claims
  whose tree `#define`s no catalog flag and has no alias yet: canary-wap's
  `selftest` (`FEATURE_DIAGNOSTICS`), `csi` (`FEATURE_CSI`) and `gps`
  (`FEATURE_GNSS`), and canary-vision's, canary-sense's and canary-display's
  `selftest` (`FEATURE_DIAGNOSTICS`). They are checked in the negative
  direction only (no env sets the flag to 0). Each needs someone to read the
  tree and decide: alias the spelling that really carries the feature (the WAP
  compiles its own CSI copy with no `FEATURE_CSI` flag), or drop the claim
  from `firmware/build_matrix.json`. A per-claim fact-finding and copy
  decision. From the lint's `UNMAPPED` set (`scripts/lint_build_matrix.py`).
  Found by C36 (#1920).

---

## 6. Documentation hygiene (small, do alongside other work)

- [ ] **D10 [decision] The docs call a WAP's four-character suffix unique to
  the board.** `getting_started_canary.md` says "The four characters at the
  end are unique to your Canary", and `onboarding_multiple_canaries.md` says
  the last four characters "are unique to this board". The suffix encodes 16
  bits of the key fingerprint, and its fourth character is always `2` (A29),
  so only three vary. Two Canaries share a suffix with a 1-in-65,536 chance
  per pair; in a fleet of about 300 the odds that two share one are about
  even. Decide whether the copy should say the suffix tells your Canaries
  apart in practice, and that the full device id and fingerprint are what is
  unique. Found by A29's review (#1762).

- [ ] **D12 [human] Name the governing state in the terms.** The website's
  live `terms.html` §9 says the terms are governed by "the state in which
  Errer Labs operates", and the monorepo's `TERMS.md` (a services-terms
  template whose bracketed items are "decisions for you and your lawyer")
  still reads `[your state]` in §10, among seven such placeholders. Name the
  state in `terms.html` and bump its "Last updated"; fill `TERMS.md`'s
  placeholders when the template is finished; and decide whether
  `terms-of-sale.html` needs the same clause (W28). A legal fact only the
  owner has.

- [ ] **D18 [decision] The Chirp spec's night rule describes neither the code
  nor §2.5.3.** `spec/chirp_channel_v0.md` §2.5.7's row says "10pm-6am |
  Safety templates only, requires 2 confirmations". The code does neither.
  `handle_ack()` uses `CONFIRMATIONS_SAFETY` (1) for emergency and weather
  templates at any hour; `TEMPLATE_TABLE` allows 19 of 24 templates at night
  (all authority; power, water and gas; all emergency, weather and all-clear);
  `is_night_mode()` is read only by `read_status()` and by `send_chirp()`,
  which refuses its own night send of the five templates that are not
  `night_allowed`; and `handle_witness()` has no night check, so receivers do
  not enforce the hours (the rule is the sender's own, as the Sybil cost D16
  corrected was). §2.5.3's threshold sentence since D16 (#1919), pinned by
  `ChirpSpecSybilCost`, has no time term, so the two sections disagree. Either
  have §2.5.7 say what holds (a canary-wap refuses its own night send of the
  five, the threshold does not change at night, and receivers do not enforce
  the hours), or decide on a night threshold. From code. Found by D16's review
  (#1919).

- [ ] **D20 [code] More Chirp passages describe a v0.1 flow.** Outside D19's
  scope (#1920), `spec/chirp_channel_v0.md` §1.2 rule 6 and §10.1 item 1 say
  "one chirp per 5 minutes" where the code uses escalating cooldown tiers
  (§2.5.4); §1.3 keeps the "someone checking car doors" use case; §4.1's send
  flow and mockup offer a free-text "Brief message (64 chars max)" and four
  categories that do not exist (Chirp sends templates only, §2.5.3); §4.3's
  per-category and relay limits are not the code's; §7.2 describes
  INITIALIZING → LISTENING → ACTIVE where the canary-wap takes INITIALIZING →
  ACTIVE (A90's default: one sentence saying so, LISTENING left for other
  implementers); and `THREAT_MODEL.md`'s Chirp section says origination is
  refused "when SNTP unsynced", where the canary-wap has no SNTP (GPS sets its
  clock). Bring each to the code; `ChirpSpecApiExamples`' readers could extend
  to §4.1. From the spec. Found by D19's package (#1920).

---

## 7. CI hygiene (monorepo workflows)

Workflow debt no section above owns: path filters, interpreter pins and the
host-test list. The rules these items apply are `.github/CI.md`'s.

- [ ] **CI4 [decision] The path-filter read gate hears only
  canary-local.yml's logic tests.** CI2 (#1725) records what that job's three
  test steps read. The job's drift-step generators run unarmed: measured
  on CI2's branch, they read 194 paths outside the filter, 187 of them
  `gen_flash.py`'s (all but two under `firmware/canary/**`; the two are
  `.github/workflows/firmware-release.yml` and
  `firmware/provisioning/platformio_secure.ini`), plus `gen_figures.mjs`'s
  6 existence probes and one `gen_operator.py` read. Arming them is a policy
  call: `firmware/canary/**` in the filter would run the workflow's wasm
  build-and-boot job (a 45-minute timeout) on every flagship firmware PR. No
  other filtered workflow is recorded either, for example
  `ios-selfheal.yml`'s XCTests, whose `#filePath` reads CI1 listed by hand
  (the recorders hear node and python3 only). Decide which drift steps to
  arm, at what cost, and how the other filtered workflows' readers are
  heard. Found doing CI2.

- [ ] **CI14 [code+decision] One home for the spelling families across both
  repos.** The families now live twice: as JavaScript lists in the website's
  `tests/copy-honesty.test.mjs` and as Python lists in
  `scripts/lint_spelling.py`. Since CI9 (#1802) a digest test in
  `scripts/tests/test_lint_spelling.py` makes a change on the monorepo side a
  visible edit, but nothing on the website side notices, so the next stem
  added there passes silently here. Decide between two options. One carries
  the lint's lists to the website as data: `gen_builder_manifest.py --site`
  already carries and pins files, and the website test would read the carried
  JSON and fail on a hand edit. The other keeps two copies and adds a parity
  check to the weekly carry job that runs both regexes over the generated
  pairs. Found by CI9 (#1802).

- [ ] **CI17 [code] Trial the jobs on `ubuntu-26.04` before 24.04 retires.**
  CI11 (#1802) pinned every job that floated on `ubuntu-latest` to
  `ubuntu-24.04` in all three repos, ahead of the 2026-10-19 image move, and
  deferred the trial its own text asked for: one throwaway PR that runs a copy
  of each Linux job on `ubuntu-26.04`, then a fix, or a reviewed move of the
  pin, for what breaks. The jobs most likely to notice are the ones CI11
  named: `rust.yml`'s GStreamer, ffmpeg and mosquitto apt jobs,
  `desktop-hub-core.yml`, `desktop-lab-check.yml`, `ram_audit.yml`, and every
  job that relies on the image's `jq`, `gh` or `xvfb`. Each pin's reason
  comment says to revisit before 24.04 retires. Found by CI11 (#1802).
  *Since A58 (#1836):* `canary-local.yml`'s page logic job also installs a
  WASI sysroot for the image's clang through
  `.github/actions/setup-wasi-sysroot` (`clang-major: "18"`) and pins
  `WASM32_CLANGXX=clang++-18` for `cores_wasm32.test.js`: the input, the pin
  and the job's `runs-on` move together, and that file's CI test fails by name
  on a `runs-on` change until they do.

- [ ] **CI22 [code] Run `check_wap_json_answers.py`'s self-test mutations in
  parallel.** The guard's proof grew to 230 mutations with J11 (F405, #1919):
  13m07s of CPU locally with nothing else running, against 9m20s before the
  wave. Each mutation reruns the whole check (about 3.3 s each, across the
  230 mutations and 10 correct edits); J11's own rule adds about 0.13 s to
  each run, so the growth is the count. On CI it took
  9:51 on #1916's PR run but 12:49 on main after #1916 merged, at the
  smaller count (`firmware.yml` run 41770593222), so a slow
  runner would have landed at about 18 of the `wap-guard-selftests` job's 20
  minutes; #1919 raised that job's timeout to 30 minutes as a stopgap. Run the
  mutations in parallel inside the guard (a process pool), or shard them (a
  `--shard K/N` flag and one matrix leg per shard), then bring the timeout
  back down; `check_wap_loop_commands.py`, the job's other leg, may want the
  same. Found by F405 (#1919).

- [ ] **CI25 [decision] The AppImage runtime and plugin now ship from dated
  releases, and `pins.txt` is outside the release watches.** Two calls CI21
  (#1920) left to a maintainer. (1) The brief named three bundler tools; the
  action also pins the AppImage runtime (type2-runtime 20251108, Build ID
  `a87aaf5d…`), because the appimagetool inside linuxdeploy's plugin otherwise
  downloads type2-runtime's `continuous` into the front of every shipped
  AppImage, and the plugin moves from `continuous` to `1-alpha-20250213-1`.
  Confirm that shipping the dated runtime and plugin instead of "whatever
  `continuous` is on build day" is wanted; dropping the runtime row means
  removing it, the `LDAI_RUNTIME_FILE` export and verify's Build ID check. (2)
  RELEASE_LESSONS 2026-09-23 (b) says a pins file a release reads belongs in
  that target's watch (`.github/espflash-pins.env` is in both);
  `.github/actions/pin-appimage-tools/pins.txt` changes the Linux AppImage's
  bytes the same way and is in neither. Adding it to `release-targets.yml`
  means, per `release_plan.py`, a NEEDS_BUMP line in the "Update everything"
  summary whenever `pins.txt` changes at an unbumped version (a report a human
  acts on, never a cut); `test_release_plan.py`'s pins-file test scans only
  `.github/*.env`, so it does not flag it. From code. Found by CI21 (#1920).

- [ ] **CI26 [code+decision] `sbom.yml`'s own tools are unpinned and its two
  Rust documents differ in scope.** Untouched by CI20 (#1920): the workflow
  installs cdxgen unpinned (`npm install -g @cyclonedx/cdxgen`), rebuilds
  cargo-cyclonedx weekly from crates.io latest, and runs on Node 20 (`CI.md`
  names Node 20 as the known exception); the kernel's `sbom-rust.cdx.json`
  step is still the host-target resolution where the app steps use `--target
  all` (the kernel is a Linux service, so that scope may be right; say so or
  align them); and cargo-cyclonedx writes spec 1.3 unless told `--spec-version
  1.5`, cdxgen 1.7, so the documents are on three spec versions
  (`sbom/README.md` says which). Pin both tools (the code half), decide the
  kernel step's scope, and unify the spec version if wanted (one flag per
  step; the decision half). From the workflow. Found by CI20 (#1920).

---

## Canonical ledgers (do not duplicate — point here)

| Ledger | Governs |
|---|---|
| [`IMPROVEMENT_ROADMAP.md`](../IMPROVEMENT_ROADMAP.md) | The 2026-09-02 audit's open residuals (device package, TLS bench passes, platform pins) |
| `firmware/ESP32S3_OPTIMIZATION_ROADMAP.md` | The 30 prioritized firmware defects (8 P0) |
| `firmware/FEATURES.md` | Arduino↔PIO parity dashboard |
| `firmware/canary/CONSOLIDATION.md` | Consolidation phases + gap inventory |
| `firmware/projects/canary-wap/ENTERPRISE_READINESS_TODO.md` | WAP enterprise checklist |
| `docs/hardware/dev_playground_todo.md` | Bench-gated capability list (U1) |
| `docs/audit/hardware_verification_checklist.md` | The hardware verification cases (U1) |
| `docs/hardware/enclosure/AUDIT_2026_09.md` | Enclosure open themes (C1–C3, C10–C12) |
| `docs/RELEASE_BUTTONS.md` | Release/ship operations (U2) |
| `securacv_website/docs/render-roadmap.md` | 3D/AR quality roadmap (W3, W4, W5, W9, W12, W13) |
| `securacv_website/docs/roadmap.md` | Website roadmap, coming-soon surfaces (refreshed by W2, website PR #200) |

**Provenance.** Findings confirmed by source inspection during the
2026-09-20 sweep (five parallel
audits over the three repos; classic TODO-marker counts were near zero — this
project records debt as honest-status prose and checklists, which is what this
file indexes). Items listed here were spot-checked against source at sweep
time; re-read the source before building on a line.
