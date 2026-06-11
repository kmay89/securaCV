# SecuraCV — Independent Roadmap, Reconciliation & Hardware Plan

> Companion to [`00-requirements-spec.md`](00-requirements-spec.md) and
> [`01-flag-report.md`](01-flag-report.md). This roadmap is built from a first-principles reading
> of the code and a current (2026) market scan, then **reconciled** against the repo's own
> `v1-roadmap.md` and `docs/strategy/` — flagging where those overclaim or are stale. Stance
> requested by the owner: *independent + reconcile.* Every item is checked against the seven
> invariants; anything needing an invariant weakened is **cut, not deferred**.

> **Re-baseline 2026-06-04.** Written 2026-06-01; kept as-is with status overlaid. Since then the
> 2026-06 fix wave (#660–#680) executed **all of P0 and P1, and the add-on-install half of P2**. The
> §2 reconciliation actions are largely done (see the note under that table); §3 phase headings now
> carry status. The HA timeline/verified-✓ UI shipped (#689); what remains for the productization arc
> is the **break-glass/trustee setup UI** (and the deferred README screenshot, F-12) plus the
> hardware/key work in **P3–P4**.

## 1. Strategic verdict: **focus, then productize — do not expand**

SecuraCV's moat is a *combination competitors cannot toggle on*: **tamper-evident perception +
privacy-by-construction**. The biggest risk is not too few features — it's **breadth outrunning
proof**. The repo already carries CSI sensing, BLE mesh (Opera), chirp/beacon channels, gossip
replication specs, post-quantum crypto, multi-transport resilience, and adapters — much of it
`_v0` or partially built (see flag report F-07, F-08, F-13). Meanwhile the *payoff a buyer
actually experiences* — a verified ✓ timeline, a one-click install, a court-grade export — is the
least finished. **Recommendation: FOCUS.** Finish the proof-and-payoff core; freeze new transports
and mesh legs until they have a concrete user need.

This agrees with `docs/strategy/05`'s "what it needs to become great" list but goes further: the
strategy docs still enumerate expansion (chirp neighbor alerts, co-signing, mobile app) as
near-term "nice-to-have." Treat all of those as **post-focus**.

## 2. Reconciliation: roadmap claims vs. verified code

| `v1-roadmap.md` claim | Reality (this review) | Action |
|---|---|---|
| Stream A detection "Done" (A1–A5 ✅), "produces correct boxes", "registry routing" | Trait+registry real; **default is StubBackend** (frame-hash motion). Tract is feature-gated, off by default, threshold hardcoded 0.5 (F-01) | Re-open: bundle a model, enable `backend-tract` on the witnessd path, or relabel default as motion-only |
| B1/B3/B4 signing/verify "Done" ✅ | True and tested (REQ-KRNL-001/004) | Keep |
| B2 device key "deferred" | Correct; also DB key coupled to signing key (F-04) | Schedule in P3 with hardware keys |
| Vault "Placeholder, not wired" | **Stale** — vault sealing is actually wired into `witnessd` (opt-in via `BREAK_GLASS_SEAL_TOKEN`), real crypto modes (F-05, corrected) | Update roadmap to "wired, opt-in"; build seal/trustee UX (P2) |
| C1–C4 ingestion "Done" / RTSP "feature-gated" | File roundtrip real; **two RTSP impls** (F-11); RTSP still listed unchecked in acceptance | Pick one RTSP path; update acceptance boxes |
| (omitted) | Post-quantum `pqc-*`, full **Sensor Adapter** framework, webhook TLS/mTLS/Prometheus all shipped but **absent from roadmap** (F-13) | Rewrite roadmap to reflect shipped scope |
| "9 CLI binaries" (CHANGELOG) | 15 exist (F-10) | ✅ Count reconciled to 15 |
| v1 = "every feature works end-to-end" (CHANGELOG) vs "minimally credible" (roadmap) vs "pre-v1" (README) | Three definitions (F-02) | Adopt one v1 definition |

**Single source of truth going forward:** replace `v1-roadmap.md`'s status board with the
[requirements spec](00-requirements-spec.md) Status tags; keep the roadmap for *sequencing only*.

> **Status 2026-06-04 — the actions in this table are largely executed.** Detection relabeled +
> threshold configurable + model fetch (F-01, #660/#665/#667); B-stream signing kept; B2 DB-key
> decouple done, hardware keys still P3 (F-04, #674); vault relabeled "wired, opt-in" in
> `v1-roadmap.md` (F-05); RTSP ffmpeg path designated canonical and CI-verified, acceptance boxes
> updated (F-11/#666); `v1-roadmap.md` rewritten to cite code and reflect shipped scope incl. PQC +
> adapters (F-13, #673); single v1 definition adopted (F-02, #673). The CLI-binary count is now
> reconciled (F-10: CHANGELOG says 15, matching `src/bin/`), and the unbuilt LoRa/SCQCS-audio
> transports + `audio_anomaly` tamper are gated out of the `ALL_*` lists into `FUTURE_*` lists
> (F-07). **Nothing from this table remains pending.**

## 3. Phased plan (invariant-checked, sequenced)

Each phase lists **deliverable → user-facing acceptance → invariant guardrail**.

### P0 — Truth & release hygiene (days, not weeks) — unblocks everything · ✅ DONE (2026-06)
<!-- v1 definition unified + audit-boundary doc + partition reconciliation + honest detection
     labeling all shipped (#673/#668/#670/#660). Residual cleanup: CLI count F-10. -->

- Adopt one **v1 definition** (recommend the roadmap's minimal one); make CHANGELOG match verified
  reality; fix the binary count (F-02, F-10). *Acceptance:* README badge, CHANGELOG, roadmap agree.
- **Honest detection labeling** + close the "audit vs security boundary" doc box that v1 acceptance
  still leaves unchecked. *Acceptance:* a reader can tell what a default build detects (F-01).
- Reconcile firmware **partition tables**; document the canonical scheme per flash size (F-06).
  *Guardrail:* none weakened — pure honesty/cleanup.

### P1 — Make the proof usable (the moat → a feature) · 🟡 MOSTLY DONE (one sub-goal descoped)
<!-- Shipped: configurable confidence (#665); one-command verified model fetch + default model path
     (#667); court-grade verifier verdicts (#664); Frigate→HA gate automated in CI (#672, the live
     full-stack gate stays a manual smoke check since ML on a fixture is non-deterministic).
     NOT shipped — deliberately descoped, not a silent miss: this phase's acceptance ("fresh install
     detects objects with no manual model step") assumed bundling a model + enabling `backend-tract`
     by default. That is intentionally NOT done — in the primary Frigate-bridge deployment object
     detection is Frigate's job, so the witnessd direct-ingest path instead ships a one-command fetch
     (#667) + a motion-only startup WARN (#660, src/bin/witnessd.rs:152-165). So the zero-step
     default-detection acceptance is reframed (Frigate owns detection), not met as originally written. -->

- **Bundled detection model** + `backend-tract` enabled on the witnessd path (kill the ONNX
  hand-download), with **configurable confidence** (replace hardcoded 0.5). *Acceptance:* fresh
  install detects objects with no manual model step. *Guardrail:* model is an *audited* backend;
  keep the seccomp option; never emit identity (Inv. I/II).
- **Court-grade signed export + standalone verifier UX**: build on the existing dual-verifier
  (`src/envelope.rs` ↔ `viewer/verify_core.js`) — ship a polished `viewer/evidence_viewer.html`
  a non-dev can open offline to get a verified ✓. *Acceptance:* a lawyer/journalist verifies an
  export with no toolchain. *Guardrail:* signatures/hashes only, never raw footage (Inv. I/IV).
- **Wire the Frigate→HA release gate**: `verify_pipeline.sh` exits 0 in CI against a live stack
  (the README gate). *Acceptance:* CI green on a real stack.

### P2 — Kill the terminal & deliver the daily payoff · 🟡 IN PROGRESS
<!-- One-click HA add-on install: ✅ DONE — pre-built multi-arch image + publish workflow (#671),
     publicly-installable release gate wired into CI (#677/#680). HA verified-✓ timeline + chain-status
     Lovelace card: ✅ DONE (custom_components/securacv/www/securacv-timeline-card.js + YAML fallback;
     helper logic unit-tested in the viewer CI job). Remaining P2: the README screenshot (F-12,
     deferred — needs an HA+browser capture) and the break-glass/trustee setup UI (F-05). -->

- **One-click HA add-on install** (no `curl | bash`), pre-built Docker images. *Acceptance:*
  install from the HA add-on store. *Guardrail:* local-only; no cloud custody (Inv. IV).
- **Timeline / verification UI in HA** (events + verified ✓ + chain status) — ✅ DONE (#689:
  bundled `securacv-timeline-card` + pure-YAML fallback). **Mobile push out-of-the-box** (daily
  digest + pattern alerts) is already partly built. *Acceptance:* the persuasive payoff is a product
  experience, not raw sensors; the missing README screenshot (F-12) is the one residual gap.
  *Guardrail:* coarse buckets, zone IDs only (Inv. III).
- **Break-glass / trustee setup UI** (no CLI) — the evidence flow must be usable under stress by a
  non-dev. *Guardrail:* N-of-M quorum, immutable receipts (Inv. V).

### P3 — Productize hardware & harden keys
- **Pre-flashed Canary kit** (primary revenue line; serves at-risk + mainstream personas).
- **Hardware-backed keys**: decouple DB key from signing key (F-04 prerequisite), then Secure
  Element / ESP32-S3 **eFuse + flash encryption + secure boot** (the secure partition table already
  anticipates `nvs_keys` from eFuse). *Acceptance:* device key not recoverable from a config seed.
- **Firmware privacy fixes** (F-03): salted/rotating presence tokens instead of raw MAC; GPS
  coarsening; grep guardrails. *Guardrail:* Inv. II/III — these are conformance bugs, fix before
  selling hardware.

### P4 — Optional paid tier (margin, never required) · 🟡 RFC-3161 SHIPPED
<!-- RFC-3161 anchoring shipped as `log_anchor` + src/tsa.rs (online + air-gapped
     offline flows; openssl as the independent countersignature verifier) — see
     docs/timestamping.md. C2PA / Content-Credentials interop remains open. -->

- **Third-party cryptographic timestamping (RFC-3161) + C2PA / Content-Credentials interop** so a
  sealed export self-authenticates against deepfake challenges. *Guardrail:* operates on
  signatures/hashes only — **never** raw footage off-device (Inv. I/IV). This is the
  least-contested moat in the market (see §5).

### Frozen until a concrete need (do NOT build now)
LoRa / SCQCS-audio transports (F-07), unbuilt mesh fallbacks (F-08), chirp neighbor-alert network,
cross-device co-signing, consumer mobile app. Keep the `_v0` channel specs as design intent only.

### Permanent "no" (invariant violations — never build)
Facial recognition / re-ID / plates (Inv. II), searchable/bulk-query archive (Inv. VII),
optional "privacy-off"/longer-retention toggles (coercible), retroactive reprocessing (Inv. VI),
cloud custody of footage (Inv. IV). A metadata-only push relay (tokens, never footage) is the only
acceptable cloud touchpoint.

## 4. Hardware plan (grounded in ESP32-S3 reality)

**The binding constraint on the S3 is flash, not RAM.** PSRAM is 8 MB OPI and ample; the FULL
Arduino build sits near the ~3.0–3.3 MB app-slot ceiling, and recent fixes were about **DRAM**
overflow (moving the 90 KB CSI ring + scratch buffers to PSRAM) and **flash** (gzipping web assets
saved ~336 KB). Recommendations:

1. **Production flash headroom — move FULL builds to a 16 MB-flash S3.** The XIAO ESP32-S3
   ("Plus") and other S3 modules ship 16 MB; this is the cleanest fix for the near-ceiling FULL
   profile and leaves room for real OTA A/B + a `witness_log` partition simultaneously. Keep
   MINIMAL/DEV on 8 MB for iteration. *(Reconciles F-06: pin one scheme per flash size.)*
2. **Keep CV off the S3.** Division of labor is already right: **Grove Vision AI V2 (canary-vision /
   ESP32-C3)** does on-sensor inference; the **S3's job is CSI/BLE/mesh sensing + Ed25519 signing**,
   not running a detection model. Don't try to run ONNX object detection on the S3.
3. **If on-device vision ever becomes a hard requirement**, evaluate **ESP32-P4** (much stronger
   for vision/AI but **no radio** — pair with an ESP32-C6/C5 for WiFi/BLE) rather than overloading
   the S3. Treat as research, not roadmap.
4. **Kernel host:** Raspberry Pi 4 (4 GB+) / Pi 5 / x86; add a **Coral USB TPU ($25–60)** for
   multi-camera detection throughput. This matches the README BOM and `docs/strategy/05`.
5. **Hardware-root-of-trust for the sold kit:** enable S3 **eFuse-derived NVS key + flash
   encryption + secure boot v2**; the `provisioning/partitions_secure.csv` already reserves
   `nvs_keys` for this (but fix its 4 MB assumption — F-06).
6. **Always keep big buffers in PSRAM** (CSI rings, web scratch). Codify as a build guardrail so the
   DRAM-overflow class of bug doesn't recur (REQ-FW-032).

## 5. Market comparison (independent, 2026)

| Product | Model | Recurring fee | Footage location | Tamper-evidence / provenance |
|---|---|---|---|---|
| Ring / Nest | Cloud | $5–20/mo | Cloud | None marketed |
| Eufy | Local-first (HomeBase) | optional | Local + optional cloud | None; took 2022 heat for non-E2EE cloud frames |
| Reolink | Local-first (microSD/NVR) | optional | Local | None; high-FPS face-grade footage |
| UniFi Protect | NVR-first | none | Local (closed ecosystem; pricey hubs) | None |
| Frigate | Self-host OSS | optional Frigate+ | Local | None (detection NVR only) |
| **SecuraCV** | Self-host, **auto-deleting** | **none** | **Local** | **Hash-chain + Ed25519 + standalone verifier** |

**Read of the field (2026):** the no-subscription/local segment is crowded and competent (Eufy,
Reolink, UniFi, Frigate) on *recording*. **Nobody in the consumer/prosumer space sells
cryptographic tamper-evidence / content provenance.** Independent of SecuraCV's own docs, the
external tailwind is strong: tracked deepfake incidents jumped from ~500K (2023) to ~8M (2025);
**C2PA** (Adobe/Arm/BBC/Intel/Microsoft) is the de-facto provenance standard using X.509 + hashing,
tamper-evident by design; the **NY Stop Deepfakes Act (2025)** mandates C2PA-conformant provenance
for synthetic content; and FRE 901 / 902(13)–(14) make cryptographic hash verification courtroom-
relevant. SecuraCV's envelope + dual-verifier is already 80% of a C2PA-interoperable story.

**Implication for positioning:** compete on **proof, not pixels**. Don't out-feature Reolink on FPS
or out-cheap Eufy on hardware — own "the only home camera whose record you can prove wasn't
altered, that can't be turned into surveillance." The privacy constraints (coarse time, no faces,
24-h memory) are the *product*, not an apology (consistent with `docs/strategy/05`, which this
review endorses).

### Caveats to carry forward
- C2PA credentials are **stripped** by most social/messaging platforms today — the value is in the
  *sealed export + standalone verifier*, not in surviving a re-upload. Scope the P4 claim honestly.
- The repo's existing market table (`docs/strategy/05`) is broadly accurate for 2026; this review
  adds the provenance/deepfake quantification and the competitor tamper-evidence column.

---

### Sources (market & hardware scan, May 2026)
- [5 smart security cameras without subscriptions (HowToGeek)](https://www.howtogeek.com/smart-security-cameras-without-subscriptions-or-cloud-storage/)
- [Best no-subscription cameras: eufy vs Reolink vs UniFi (Zomg The Handyman)](https://zomgthehandyman.com/blog/best-no-subscription-security-cameras)
- [Reolink vs Eufy 2026 (Sipko Security)](https://sipkosecurity.com/reolink-vs-eufy-security-camera-comparison-2026/)
- [What is C2PA? Content Provenance (c2paviewer)](https://c2paviewer.com/articles/what-is-c2pa)
- [C2PA Standard 2026: limitations (truescreen.io)](https://truescreen.io/articles/c2pa-standard-history-limitations/)
- [C2PA deepfake detection 2026 (EyeSift)](https://www.eyesift.com/ai-image-detection-2026-c2pa-content-credentials-synthid-watermarks-diffusion-fingerprints-deepfake/)
- [Seeed XIAO ESP32-S3 Sense (Seeed Wiki)](https://wiki.seeedstudio.com/xiao_esp32s3_getting_started/)
- [XIAO ESP32-S3 Sense specs — 8MB PSRAM/8MB Flash (Amazon listing)](https://www.amazon.com/Seeed-Studio-XIAO-ESP32-Sense/dp/B0C69FFVHH)
- [ESP-CSI DIY WiFi presence detection (Hackster)](https://www.hackster.io/limengdu0117/esp-csi-diy-wifi-human-presence-detection-f80508)
