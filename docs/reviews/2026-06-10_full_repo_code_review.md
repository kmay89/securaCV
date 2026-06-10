# Full Repository Code Review — Claims vs Code, Security Posture, and the Path to v1

**Date:** 2026-06-10
**Scope:** entire repository at commit `88dd6f6` — Rust kernel, firmware trees, Home Assistant
integration, install/setup path, fleet/mesh protocols, CI, and documentation.
**Method:** documentation claims were treated as untrusted and verified against source. Every
finding below cites the file (and line where stable) that backs it.

---

## 1. Executive summary

**The core is real, and the documentation is unusually honest — with specific exceptions.**
The Rust kernel's cryptographic claims all verify against code: Ed25519 domain-separated
signing over a hash chain, SQLCipher database encryption with an independently derived key,
quorum break-glass with constant-time rotating capability tokens, and a mandatory seccomp
sandbox for detection modules. CI gates run real end-to-end pipelines (live MQTT broker, real
RTSP server, real `log_verify` binary), not mocks.

The gaps cluster in three places:

1. **One real bug:** the Home Assistant integration's "Kernel HTTP API" mode stops working
   within 10 minutes of setup, because the kernel rotates its capability token every 10-minute
   bucket while HA holds the token statically (§5.3).
2. **Firmware asymmetry:** `canary-wap` is a genuine signed-witness device, but
   `canary-vision` — listed in the README as a first-class Canary — performs **zero
   cryptography** (§4.2). Three overlapping firmware trees carry stale internal docs that
   contradict the code in *both* directions (§4.4).
3. **The multi-canary fleet is code-complete but unproven:** the Opera mesh and Chirp channels
   have real crypto implementations that have never run on two or more physical devices;
   gossip replication and co-signing are spec-only (§6).

Status per the project's own framing (`v1-rc`, on-device validation pending) is **accurate**.
The ordered gap list to a v1 worth shipping is in §8.

---

## 2. What was audited

| Area | Key paths |
|---|---|
| Rust kernel | `src/` (crypto, log, vault, break_glass, detect, ingest, adapter, module_runtime, api, bin), `tests/`, `Cargo.toml` |
| Firmware | `firmware/canary/` (ACTIVE), `firmware/projects/{canary-wap,canary-vision,canary-ota}/`, `firmware/common/`, `firmware/scripts/` |
| HA + setup | `custom_components/securacv/`, `privacy_witness_kernel/`, `scripts/install.sh`, `integrations/ha_frigate_mqtt/`, `homeassistant/`, `viewer/` |
| Fleet specs | `spec/{canary_mesh_network,gossip_replication,chirp_channel,beacon_channel,co_signing}*.md` vs implementing code |
| CI | `.github/workflows/{rust,python,validate}.yml` |

---

## 3. Rust kernel — claims verified ✅

| Claim (README / v1-roadmap) | Verdict | Evidence |
|---|---|---|
| Ed25519 hash-chained signed log | **Real** | Domain-separated signing in `src/crypto/signatures.rs` (`sign_with_domain`); chain + per-entry signature verify in `src/log/mod.rs` |
| Tamper-evidence provable | **Real** | `tamper_demo` and `log_verify` binaries; `tests/frigate_mqtt_e2e.rs` runs the real `log_verify` against an encrypted DB |
| SQLCipher encrypted DB, key decoupled from identity | **Real** | `rusqlite` `bundled-sqlcipher` (`Cargo.toml`); `resolve_db_encryption_key` (`src/lib.rs:138`) honors independent `SECURACV_DB_KEY_SEED` via HKDF-SHA256; in-place rotation via `rekey_database_file` (`src/lib.rs:162`) |
| Break-glass multi-party quorum | **Real** | `QuorumPolicy` n-of-m in `src/break_glass/core.rs`; `unseal` refuses before quorum (`src/break_glass/http.rs`) |
| Break-glass web UI (new, PRs #739–741) | **Real, sound design, young** | `src/break_glass/server.rs`: loopback-default with defense-in-depth peer check (line 185), refuses non-loopback bind without TLS (line 72), rejects tokens in query params (line 199), rate-limiting `Lockout` (line 436). The console page itself is unauthenticated but contains no secrets; all API calls require the rotating Bearer token. Shipped days ago — lowest-mileage component in the security surface. |
| Seccomp sandbox for detection | **Real and mandatory** | `execute_sandboxed` is the only module path (`src/module_runtime/mod.rs:57`); libseccomp denylist in `src/module_runtime/sandbox.rs:123-134`. Adapter parse sandboxing is **opt-in** (`adapter-sandbox` feature, `src/adapter/sandbox.rs`) — docs say so honestly. |
| Privacy invariants enforced in code | **Real** | `RawFrame` non-`Serialize`/non-`Clone` with compile-fail tests; `RawMediaBoundary::deny_export` gates raw export on break-glass token (`src/lib.rs`); `ClaimKind` allowlist has no face/plate variants (`src/adapter/contract.rs`); 10-minute `TimeBucket` coarsening |
| RTSP / file / V4L2 / ESP32 ingest | **Real, feature-gated** | `src/ingest/{rtsp,rtsp_ffmpeg,file,v4l2,esp32}.rs`; ffmpeg RTSP path has a true e2e CI job (MediaMTX + `tests/rtsp_e2e.rs`); GStreamer is compile-gated only |
| Detection backends | **Real, motion-only by default** | Stub/CPU/Tract in `src/detect/backends/`; default build is frame-diff motion; `witnessd` logs a startup WARN when motion-only — matches the roadmap's caveat exactly |
| Vault sealing Classical/PQ/Hybrid | **Real, opt-in** | `VaultCryptoMode` in `src/vault/crypto.rs` (ChaCha20-Poly1305; ML-KEM-768 for PQ); gated on `BREAK_GLASS_SEAL_TOKEN` in `src/bin/witnessd.rs`; silently disabled when unset |
| API auth | **Real** | 32-byte OS-random capability token, constant-time compare via `subtle` (`src/api/mod.rs:125-138`), loopback default `127.0.0.1:8799`, token file mode 0600 |

No hardcoded secrets in production paths; no command injection or path traversal found in the
break-glass or API handlers; `unwrap`/`panic` usage (~258 hits) concentrates in tests and
poisoned-lock fatals, not untrusted-input parsing.

---

## 4. Firmware — one real witness device, one unsigned PoC, three trees

### 4.1 `canary-wap` (ESP32-S3, Arduino tree) — the real device ✅

- Ed25519 device key in NVS; every witness record signed and hash-chained
  (`firmware/projects/canary-wap/arduino/canary_wap/device_signature.{h,cpp}`).
- **Signed MQTT to Home Assistant is implemented and wired** — contrary to the project's own
  `PARITY_PLAN.md` (see §4.4): `csi_mqtt.cpp` signs each event publish via
  `device_signature::sign_event` with the `securacv-canary-sig` canonical format that
  `custom_components/securacv/signature.py` verifies; initialized and pumped in
  `canary_wap.ino:5710` and `canary_wap.ino:7747`, with offline backfill replay on reconnect.
- SD append-only storage, GPS coarsened to 3 decimal places (`firmware/common/gnss/`),
  salted `device_pseudonym` instead of raw MAC (`firmware/common/identity/`), BLE
  provisioning, device-unique AP password, embedded web dashboard.
- Setup-wizard code exists in two modules: `setup_wizard.h` (first-run captive portal,
  included by the sketch at `canary_wap.ino:202`) and `wizard.h` ("Phase 10" orchestration —
  pairing, zone tagging, training progress — used by `rf_presence.cpp` and `tests.cpp`).

### 4.2 `canary-vision` (ESP32-C3 + Grove Vision AI V2) — unsigned PoC ⚠️

- **No cryptography at all.** The only "sign" matches in its source are `(unsigned long)`
  casts (`firmware/projects/canary-vision/src/main.cpp:78-105`). Events are plain JSON over
  MQTT with a sequence number.
- No `device_pseudonym`, no SD/offline path; HA MQTT discovery works
  (`src/ha/ha_discovery.cpp`) but the HA integration will correctly mark it "unsigned."
- The README lists it alongside canary-wap as a first-class Canary. As shipped it is an
  optical presence sensor, not a witness device.

### 4.3 `canary-ota` — functional engine, disconnected ⚠️

- Manifest fetch, SHA256 verification, A/B partitions with rollback, self-test framework: real.
- **No Ed25519 manifest/firmware signing** (its own README lists it as Phase 3) and **no
  integration** into canary-wap/canary-vision — nothing in the shipping trees calls it.

### 4.4 Variant sprawl and stale internal docs ⚠️

Three overlapping trees (`firmware/canary` "ACTIVE" PlatformIO, `firmware/projects/canary-wap`
Arduino + legacy PIO, `firmware/projects/canary-vision`) plus `firmware/{PARITY_PLAN,FEATURES,
FIRMWARE_VARIANT_AUDIT,VARIANT_POLICY}.md`. The parity docs are stale **in the conservative
direction** (PARITY_PLAN lists "port MQTT to Arduino canary-wap" as open work although the code
ships it; `ENTERPRISE_READINESS_TODO.md` calls onboarding-wizard work missing although
`setup_wizard.h` and `wizard.h` exist). Stale-pessimistic is better than stale-optimistic, but it misleads
contributors and reviewers in both directions and inflates the apparent gap list.

`firmware/scripts/regression_check.sh` is a genuine privacy gate: hard-fails on raw MAC
addresses, un-coarsened lat/lon, and API-token leakage into the witness chain, across all trees.

---

## 5. Home Assistant integration and setup path

### 5.1 The integration is real and multi-device ✅

`custom_components/securacv/`: HACS-valid (validate.yml runs HACS + Hassfest), three config
modes (MQTT-only recommended / Kernel HTTP / both), MQTT auto-discovery of any number of
Canaries by topic prefix, **per-device Ed25519 TOFU trust store** with manual rotate/unpin
(`device_trust.py`), real payload signature verification (`signature.py`), per-tamper-type
binary sensors, and a dependency-free Lovelace timeline card whose verification badges are
honest by label (✓ "Signature verified" only on actual Ed25519 pass). Pytest suite covers
trust pinning and signature parity.

### 5.2 `install.sh` is real ✅ — but the README's "3 steps" hide two hard prerequisites ⚠️

`scripts/install.sh` (511 lines) genuinely installs Mosquitto, Frigate, the integration, the
`privacy_witness_kernel` add-on (which exists: `privacy_witness_kernel/config.yaml`, image
built by `addon-image.yml`), and generates + warns about the device key. What the README does
not say:

- **Frigate camera configuration is a hard blocker** — the deployed `frigate.yml` contains
  `PLACEHOLDER_CAMERA_IP`; until the user edits RTSP URLs, Frigate doesn't start and the
  kernel has nothing to witness.
- **Where the API token comes from** (add-on logs / token file) is undocumented in the README
  flow, yet the Kernel-HTTP config mode asks for it.

### 5.3 Bug: Kernel-HTTP mode dies within 10 minutes ❌

- The kernel rotates the capability token every 10-minute `TimeBucket` and rewrites the token
  file (`src/api/mod.rs:368-371`).
- The HA client captures the token **once** at config-entry setup
  (`custom_components/securacv/__init__.py:120`) and never re-reads it; a 401 raises
  `SecuraCVApiAuthError` → `UpdateFailed` (`__init__.py:129-130,197`) with no reauth flow and
  no token-file re-read.

Net effect: every Kernel-HTTP-mode install shows working sensors for ≤10 minutes, then goes
permanently unavailable until reconfigured — and then breaks again. MQTT mode (the
recommended default) is unaffected, which is probably why this hasn't been caught. Fix
options: HA re-reads the token file per poll (works when HA and the add-on share `/config`),
a proper reauth flow, or an opt-in long-lived token in the kernel.

### 5.4 Other setup-surface notes

- Compose stack (`integrations/ha_frigate_mqtt/docker-compose.yml`): Mosquitto runs
  `allow_anonymous true` bound to loopback only — acceptable for the single-host stack,
  must not be copy-pasted to a LAN deployment.
- Browser evidence viewer (`viewer/verify_core.js`): real WebCrypto Ed25519 with
  domain-separated canonical-JSON parity to the Rust verifier, and an honest "ML-DSA not
  verified offline" caveat. Tested (`verify_core.test.js`).

---

## 6. Multi-canary fleet — code-complete crypto, zero hardware proof

| Protocol | Spec | Implementation | Status |
|---|---|---|---|
| Opera mesh (ESP-NOW) | `spec/canary_mesh_network_v0.md` | `mesh_network.cpp` (Arduino tree) + `firmware/canary/lib/securacv_mesh/` (pairing, sessions, envelopes, beacons, host tests) | **Code-complete, bench-gated.** Ed25519 challenge-response, X25519 ECDH, ChaCha20-Poly1305, opera-secret rotation on peer removal, flash-encryption requirement enforced. Never validated on ≥2 physical devices. WiFi-AP bridge relay and BLE fallback are spec-only. |
| Chirp channel | `spec/chirp_channel_v0.md` | `chirp_channel.cpp` | **Code-complete, hardened (audits C1–C14 closed in code), bench-gated.** No two-board test yet. |
| Beacon (life-safety) | `spec/beacon_channel_v0.md` | `beacon_channel.{h,cpp}` | **Scaffold**, `FEATURE_BEACON_CHANNEL` default OFF. Not v1 material. *Post-review corrections:* the item-12 concern was a false positive — the **audible** self-test chirp is already monthly/waking-hours/SNTP-gated (`canary_wap.ino:7649-7706`); the 24 h `SELFTEST_INTERVAL_MS` is the **radio** supervision heartbeat that the 36 h neighbor-gap trouble detection requires. The item-9 gap (64-entry overwrite ring vs append-only mandate) was real and is now **fixed**: the log of record is an append-only SD JSONL file with the NVS ring demoted to a recent-view cache (`beacon_channel.cpp::sd_append_audit_entry`), and a `FEATURE_BEACON_CHANNEL=1` CI compile gate was added. |
| Gossip replication | `spec/gossip_replication_v0.md` | none found | **Spec-only** (spec itself says optional, off by default). |
| Co-signing | `spec/co_signing.md` | none found | **Spec-only** (kernel-side design doc). |

There is also no direct canary→kernel pairing ceremony: the kernel's `ingest-esp32` path
(`src/ingest/esp32.rs`) pulls frames over HTTP without device-signature verification, and the
architecture instead routes canary events to HA over signed MQTT (verified by the HA
integration's TOFU store, not by the kernel's sealed log). That is a coherent design, but it
means **canary events do not land in the kernel's hash-chained log today** — the sealed log
covers the Frigate pipeline; canary-wap keeps its own on-device signed chain. Worth stating
explicitly in the docs.

---

## 7. Security: settled vs pending

**Settled (verified in code):**
log/chain crypto and domain separation; SQLCipher with independent, rotatable DB key;
quorum break-glass with rate-limited, constant-time, never-logged tokens; loopback-default +
TLS-or-refuse exposure policy on both API servers; mandatory module seccomp sandbox;
no raw MAC / un-coarsened GPS in firmware output (CI-enforced); minimum seed entropy with
placeholder-seed rejection (`src/lib.rs:2271-2289`); zeroized key buffers.

**Pending (known, mostly documented):**

| Item | Where | Risk note |
|---|---|---|
| Device signing key is seed-derived, not hardware-backed | `signing_key_from_seed`, `src/lib.rs:2273` | Roadmap B2 / v1.1; honest in docs |
| TLS off by default on kernel API | `src/api/mod.rs:154-162` (warns) | Loopback-mitigated; LAN exposure requires operator action |
| Vault sealing opt-in via env token, no setup UX | `src/bin/witnessd.rs` | Silently disabled when unset — consider a startup notice |
| OTA updates unsigned (SHA256 only) | `firmware/projects/canary-ota/` | Must be Ed25519-signed before any auto-update story ships |
| Mesh/chirp crypto unproven on hardware | §6 | Code review ≠ RF reality; bench validation is the gate |
| Plaintext device key on host disk | `install.sh`, `/config/.securacv/device_key` | Acknowledged in `docs/root_paradox.md` |
| Anonymous loopback MQTT in compose stack | `integrations/ha_frigate_mqtt/` | Fine single-host; document the LAN caveat |
| Break-glass web UI is days old | PRs #739–741 | Design is sound; needs soak time / adversarial review |

No critical vulnerabilities were found **in default builds**. *(Post-review update:
the Beacon conformance gaps originally noted here are resolved — the audit log is now
SD append-only per `AGENTS.md` item 9, and the item-12 self-test concern was a false
positive; see the corrected Beacon row in §6.)*

---

## 8. What's needed to get to v1

v1 target: **working canary firmware + HA + multi-canary fleet that works together, simple
setup, ready for people to try.** Ordered by blocking weight:

1. **On-device hardware validation** — the project's own stated blocker, and the right one.
   Bench-validate canary-wap on real ESP32-S3 (boot → provision → signed MQTT → HA verified-✓),
   then a 2–3 device Opera mesh + chirp exchange. This single step converts the fleet story
   from "code-complete" to "works." *(Hardware + days, not code-weeks.)*
2. **Fix the HA kernel-mode token bug (§5.3)** — re-read the token file per poll, add a reauth
   flow, or add an opt-in long-lived token. Without it, one of the three advertised config
   modes is broken-by-design. *(Small, well-bounded.)*
3. **Decide canary-vision's identity** — either port Ed25519 signing + `device_pseudonym` from
   canary-wap (the signing module is reusable), or relabel it in README/docs as an unsigned
   optical sensor. Shipping it as a "Canary" without crypto undermines the project's central
   claim. *(Port: ~days. Relabel: hours.)*
4. **Consolidate firmware variants and fix stale parity docs** — pick the canonical tree per
   product, archive the rest, and regenerate `PARITY_PLAN.md` / `ENTERPRISE_READINESS_TODO.md`
   from code reality (both currently understate what's done). *(Docs + tree hygiene.)*
5. **Setup polish for "ready to try"** — add the two missing README steps (edit `frigate.yml`
   cameras; where the API token lives), and exercise the canary-wap `wizard` flow end-to-end
   on hardware as part of item 1. *(Hours.)*
6. **OTA: integrate or de-scope** — wiring canary-ota into the trees without signed manifests
   would be a security regression; either add Ed25519 manifest signing and integrate, or
   explicitly mark OTA as post-v1. *(Recommend: de-scope from v1, sign in v1.1.)*
7. **Say what v1 fleet means** — gossip, co-signing, and beacon are spec-only/off; keep them
   out of v1 and state in the README that today's "fleet" = N independently-signed canaries
   converging in HA plus local mesh presence, with the kernel's sealed log covering the
   Frigate pipeline (§6's canary-vs-kernel-log distinction).

Items 2, 3 (relabel option), 4, and 5 are achievable quickly; item 1 is the calendar gate.

---

## 9. Stale-docs fix list

- `firmware/PARITY_PLAN.md` — Group-B "port MQTT to Arduino canary-wap" is done in code.
- `firmware/projects/canary-wap/ENTERPRISE_READINESS_TODO.md` — onboarding-wizard item ignores `setup_wizard.h`/`wizard.h`.
- `README.md` — Firmware section presents canary-vision as a peer of canary-wap (§4.2);
  install section omits Frigate camera config + token discovery (§5.2).
- `custom_components/securacv/signature.py` docstring cites the canary-wap signer as the
  source of truth — correct, but worth adding canary-vision's unsigned status to
  `docs/device_trust.md` so "unverified" badges aren't read as a bug.

---

*Review produced from a full-tree audit; all file:line citations checked against the working
tree at `88dd6f6`.*
