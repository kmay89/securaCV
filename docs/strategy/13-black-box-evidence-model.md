# The Black-Box Evidence Model — What Flight Recorders Teach Our Chain

**Scope:** how the world's believed recorders — aviation FDR/CVR, the EU smart
tachograph, automotive EDRs, maritime VDRs, rail event recorders, police
body-cam evidence systems, CTBTO treaty monitoring, and the digital-forensics
chain-of-custody canon — earn belief in what they collect; a mapping of every
one of their mechanisms onto our sealed log; and the model for **what SecuraCV
collects, what it promises, and how it proves the promises are kept**.

**Questions this doc answers:**

> How does a black box make strangers believe its data is real? Which of those
> mechanisms do we already have, which are we missing, and what should our
> chain formally collect and promise so the record holds up — to an insurer, a
> court, or a skeptic — the way a flight recorder's does?

Short answer: recorders are believed not because any one mechanism is
unbreakable but because **dictionary, format, survivability, ritual, power,
custody, adversarial readout, disclosure limits, and corroboration form one
mutually reinforcing system — and every historical failure was converted into
a new mandatory mechanism**. Measured against that system, SecuraCV is in an
unusual position: we already have the *cryptographic core* that aviation, EDR,
VDR, and rail recorders lack entirely (signed-at-source, hash-chained,
externally anchored, offline-verifiable — company we share only with the EU
tachograph and CTBTO treaty stations), but we are missing most of their
*institutional* machinery: a machine-readable parameter dictionary whose
schema travels with the data, a correlation ritual that periodically proves
the whole truth-chain end-to-end, a preservation-hold and erasure-certificate
regime, non-equivocation witnessing, and a published custody/readout ceremony
aimed at FRE 902(13)/(14) self-authentication. Almost all of it is software
and documentation, not hardware — and two quotable laws of the recorder world
should govern the work: aviation's **"capture maximally, disclose minimally"**
(the asymmetry that made a microphone over every pilot's shoulder socially
acceptable) and the tachograph world's hard-won **"cryptography is not
enough"** (process, calibration, and custody carry the other half).

---

## 1. How recorders earn belief — the eight mechanisms

Distilled from the two research passes (aviation; everything else), the
recorder industries converge on the same mechanisms, each purchasable
separately, compounding together:

1. **A regulated, versioned dictionary.** An FDR records exactly the 88
   parameters enumerated in 14 CFR 121.344, each with range, sampling rate,
   accuracy, and resolution fixed in Appendix M — and the *decode key* is
   itself a legal deliverable: §121.344(j) obliges the operator to maintain
   "documentation sufficient to convert recorded data into engineering
   units", standardized as machine-readable XML (ARINC 647A "FRED"). An
   investigator in 2035 can decode a 2015 frame because the schema was a
   maintained artifact, not tribal knowledge.
2. **Self-announcing structure; corruption is local.** ARINC 717 frames carry
   distinct sync words every second; damage costs the damaged seconds, and
   readout re-locks at the next sync. Labs transplant memory chips from
   wrecked recorders and still read them.
3. **Survive — and be findable after — the event you exist to document.**
   3,400 g impact, 1,100 °C × 60 min fire, 6,000 m × 30 days immersion,
   90-day locator beacons, and (A350) a deployable second recorder that
   ejects, floats, and carries its own distress beacon.
4. **The correlation ritual.** EASA AMC1 CAT.GEN.MPA.195(b): annually,
   download a *real* recording, decode it with the production dictionary,
   verify every mandatory parameter for plausible values and signal quality,
   archive the report. The tachograph's version: approved-workshop inspection
   and calibration at least every two years, seals checked, workshops
   themselves audited. A recorder that hasn't recently proven it can tell the
   truth isn't presumed to.
5. **Interruption is itself evidence; erasure is interlocked.** Post-SilkAir
   185 (recorders stopped minutes before the dive; no breaker "click" on
   tape), CVRs got 10-minute independent power (RIPS) and the un-clicked stop
   became a clue. The erase button works only on the ground with the parking
   brake set, and post-incident erasure is a crime. The tachograph logs
   power interruptions and **security-breach attempts** as first-class
   events.
6. **Custody ceremony + adversarial readout.** Annex 13: photograph in situ,
   ship sealed, water-recovered recorders travel *in water*, only the
   investigating authority opens the box, and parties with opposing
   interests (operator, manufacturer, unions, regulator) jointly produce the
   transcript. The adversarial composition *is* the authentication.
7. **Two-tier data, two-tier disclosure.** The same acquisition unit feeds
   the armored FDR (sealed, evidentiary, rarely touched) and the QAR
   (rich, routine, blame-protected for safety analytics). CVR audio is never
   made public — only pertinent transcript excerpts (49 U.S.C. §1114(c);
   Annex 13 §5.12). Maximal capture was made politically survivable by
   minimal disclosure.
8. **Corroboration and external anchors.** FDR data is believed because it
   locks onto radar, ADS-B, the QAR copy, and CVR acoustics. Tachograph v2's
   single best upgrade: GNSS-vs-odometry consistency checks, so an adversary must
   falsify two independent physics coherently — and the *conflict itself* is
   a logged event ("motion-data conflict"). CTBTO stations sign waveforms at
   the sensor with tamper-detecting enclosures so adversary states can trust
   each other's data.

Two cross-cutting legal facts frame everything: courts treat custody gaps as
going to **weight, not admissibility** (a complete machine-generated custody
trail is a strategic asset, and machine records are not hearsay), and since
2017, **FRE 902(13)/(14)** lets a hash-verified machine record
self-authenticate via a qualified-person certificate — no live testimony.
The EDR proves process can substitute for crypto; the tachograph proves
crypto without process gets attacked at the sensor and the workshop. The
strongest position is both.

---

## 2. Where SecuraCV already stands (the mapping)

Measured mechanism-by-mechanism against the reference systems:

| Mechanism (exemplar) | Our implementation today | Status |
|---|---|---|
| Signed at source (CTBTO stations, OSNMA, tachograph) | Ed25519 per record, on-device for canaries, in-kernel for the pipeline; device-key TOFU pinning in HA; domain-separated signatures | ✅ ahead of aviation/EDR/VDR/rail |
| Sequential integrity (tachograph .DDD, CT logs) | Three hash-chained ledgers — events, break-glass receipts, export receipts — with signed retention checkpoints and key-rotation lineage | ✅ |
| Structure that localizes damage (ARINC 717 sync) | Per-record chain with checkpoints; `log_verify` diagnoses *where* and *what kind* on failure | ✅ partial (no segment/re-sync concept below a checkpoint) |
| Frame counter / continuity (717 subframe cycle) | Heartbeat records: one per 10-minute bucket, carrying per-bucket counter deltas; timeline audit flags missing buckets and stale tails | ✅ |
| Power/lifecycle discretes (RIPS, SilkAir lesson) | Lifecycle `start`/`shutdown_clean` records; unclean shutdown seals `PowerLoss`; firmware brownout counters persist immediately; battery units seal a graceful-shutdown record | ✅ partial (see G3) |
| Fault/conflict events (tachograph "motion-data conflict", "security-breach attempt") | Latched `FailureType` records (StorageFull, StorageWriteFailed, CryptoFailure, ClockSkew, PowerLoss, GapMissingData) + tamper events; `SensorDisagreement` defined but **not emitted** | ✅ partial (G7) |
| External time anchor (RFC 3161, eIDAS) | `log_anchor`: chain head hash to a TSA — content never leaves the device | ✅ (opt-in, operator-initiated) |
| Offline, tool-validated readout (CDR tool, NTSB lab) | Standalone browser verifier byte-parity-locked to the Rust verifier by shared fixtures; `log_verify` CLI with plain-language diagnosis | ✅ (formalize as tool validation — G9) |
| Export custody records (Axon audit trail, custody forms) | Every export mints a signed, hash-chained export receipt; bundles are self-verifying with the device public key | ✅ |
| Two-tier data (QAR vs FDR) | Frigate keeps the pixels (operational tier); the witness chain keeps semantic claims (evidentiary tier); break-glass by N-of-M quorum for raw access | ✅ by construction |
| Disclosure asymmetry (CVR transcript rules, DSRC pre-screen, RFC 3161 hash-only) | Coarse buckets, no identity substrate, export jitter, `replication_status`/`offline_intervals` markers, TSA sees only a hash | ✅ |
| Attribution honesty | Three declared attestation tiers (device / adapter / ha-bridged); adapters can never claim up | ✅ |
| Versioned machine-readable dictionary (Appendix M + FRED) | `spec/event_contract.md` is normative **prose**; vocabularies duplicated as constants in Rust/C++/Python/JS | ❌ G1 |
| Correlation ritual (annual readout; workshop calibration) | Scheduled chain verify (24 h) + Verify Now button — verifies the *ledger*, not the *sensing truth-chain* | ❌ G2 |
| Preservation hold (91.609's 60-day duty; deployment lockout) | Retention deletes on schedule regardless of significance | ❌ G4 |
| Erasure ceremony (interlocked, certificated) | Retention pruning is automatic + checkpointed; no owner-erasure certificate concept | ❌ G4 |
| Non-equivocation (CT witnesses, cosigning) | A lone signed chain can present different histories to different readers | ❌ G5 |
| Survivable/findable core (deployable recorder, 90-day beacon) | Kernel: WAL + FULL sync + atomic writes; firmware: SD-first crash-safe chain. Nothing survives the house | ❌ G8 |
| Published custody/readout ceremony (Annex 13, ISO 27037) | Evidence lifecycle documented; no step-by-step extraction ceremony or court-facing certificate templates | ❌ G6 |

The asymmetry is striking and useful: **we lead the field on cryptographic
mechanisms and trail it on institutional ones — and institutions are mostly
software and documents.**

---

## 3. The gaps, as the recorder industries would file them

**G1 — The dictionary is prose; it must become an artifact.**
(Appendix M + §121.344(j) + FRED.) Define one machine-readable schema — the
**Witness Dictionary** — enumerating: every record type (`event`, `failure`,
`heartbeat`, `lifecycle`, `key_rotation`, receipts), every event/failure/
tamper vocabulary entry, and for each *channel* its declared semantics:
cadence (per-bucket / latched-per-excursion / on-transition), range or value
set, coarseness guarantees (bucket floor, jitter), confidence semantics, and
attestation tier. Version it; **embed the dictionary hash in every retention
checkpoint** (and export bundle) so any future reader can prove which schema
governed which segment. This is doc 12's FR-13 ("one dictionary") extended
with the aviation insight: *the decode key is a maintained deliverable with
the same integrity guarantees as the data*. It also dissolves the known
four-copy vocabulary drift (Rust/C++/Python/JS) via codegen from the one
source.

**G2 — We verify the ledger; nobody verifies the witness.**
(EASA annual readout; tachograph biennial calibration.) `Verify Now` proves
the chain wasn't altered — it cannot prove the system still *senses truly*.
Add the **Correlation Check**: a scheduled (monthly/annual) operator ritual
that exercises the full truth-chain — a physically induced known stimulus
(walk the boundary zone; press the canary's tamper test; radar presence
check) → detection → seal → export → offline-verify — and seals a signed
`correlation_check` attestation record (new system-trace type in the
dictionary: pass/fail per channel, tool versions, dictionary hash). Wizard
and HA guide the ritual; the report archives like an EASA readout. Crucially
this is an *operator-induced real stimulus*, never synthetic injected data —
the kernel must not fabricate detections (that would violate the honesty the
ritual exists to prove).

**G3 — Finish "interruption is evidence."**
(RIPS; SilkAir.) We seal `PowerLoss` retroactively on reopen and firmware
counts brownouts. Missing: a kernel **final-gasp path** (UPS/battery-aware
terminal record on power events where hardware allows), and per-channel
**silence accounting** — a camera or adapter going quiet already seals
`GapMissingData` on the ingest path, but adapter-path drops are currently
logged and lost (doc 12 K7): every silenced channel must leave a signed gap
record, because the un-clicked breaker is a clue.

**G4 — Preservation holds and erasure certificates.**
(91.609's 60-day post-event preservation duty; EDR deployment lockout; CVR
erase interlock; break-glass as our quorum precedent.) Two mechanisms:
- **Hold**: flagging an event/window (owner action, or automatic on tamper/
  chain-fail/break-glass) suspends retention pruning for the covering
  segments until released; holds and releases are sealed records. This is
  the litigation-hold analog — "retention sized to the reporting latency of
  real events" (the 25-hour-CVR lesson: Air Canada 759 was overwritten
  because nobody realized in time).
- **Erasure certificate**: owner-initiated deletion beyond policy (their
  right — Invariant IV) executes only through a multi-condition, confirmed
  ceremony that seals a signed certificate recording *that* a window was
  erased (never *what* it contained). Deletion stays possible; **silent**
  deletion stops existing. Both slot naturally beside break-glass in the
  quorum/ceremony family.

**G5 — Non-equivocation: get the chain witnessed.**
(Certificate Transparency; witness cosigning; corroboration.) A signed chain
proves internal consistency, not that everyone sees the *same* chain: a
device (or thief with the key) could maintain forked histories. Remedies in
ascending strength, all privacy-safe because only 32-byte heads move:
RFC 3161 anchoring (have — proves existence-by-time, not uniqueness);
**mesh witness cosigning** — canaries and the kernel already gossip; let each
device periodically countersign the others' chain heads and seal the
countersignature, making a household of N devices its own witness network
(the fork must now fool every sibling simultaneously); optional publication
of heads to a public transparency log (Rekor-style) for maximal-assurance
users. This turns our existing mesh from a connectivity feature into an
evidentiary one — our deployable recorder and witness network in one.

**G6 — Publish the custody ceremony and aim it at FRE 902(13)/(14).**
(Annex 13; ISO 27037; ACPO; SWGDE; Axon's export package.) A short normative
doc + evidence-pack additions: the step-by-step **extraction ceremony**
(export via `/export/bundle`, hash on acquisition, verify with the offline
tool, custody form per transfer with re-hash — templates included); a
generated **verification certificate** in every evidence pack (what was
verified, tool versions, dictionary hash, results) structured to support a
902(13)/(14) qualified-person certification so the record can
self-authenticate without live testimony; and the FOQA-style statement of
the **two-tier split** (Frigate media = operational tier under the owner's
ordinary control; witness chain = evidentiary tier with ceremonies). Gaps go
to weight — so we generate the complete custody record by default.

**G7 — Conflicts are first-class evidence.**
(Tachograph motion-data conflict — the single most effective anti-fraud
upgrade.) We already run multiple physics (camera, radar, CSI, contact).
Implement `SensorDisagreement` (currently specified but honestly "not
emitted"): when co-located modalities materially disagree within a bucket,
seal the conflict as its own signed record. An adversary must now fool every
physics coherently — and failing to is itself recorded. (Prerequisite
infrastructure — co-location registry, corroboration windows — makes this a
v2 item; the dictionary entry lands in G1 now.)

**G8 — The record must outlive the house.**
(ED-112A survivability; the A350 deployable; the 90-day beacon.) Local
crash-safety is done; site-loss (fire, theft, seizure) is not. The analog is
not cloud custody of footage (never) but **replication of the sealed chain**
— small, semantic, already privacy-filtered: scheduled encrypted export of
chain segments + heads to owner-chosen destinations (second on-site device,
USB ritual, owner's off-site storage), recorded by export receipts;
mesh-replicated heads (G5) already guarantee the *integrity* of whatever
survives. "Surviving data you can't find is no data" — the export scheduler
plus the existing `scheduled_exports` design is the delivery vehicle.

**G9 — Treat the verifier as an accredited tool.**
(Bosch CDR validation; NIST CFTT; Daubert.) The byte-parity fixtures already
make our two verifiers mutually attesting; formalize: a versioned **golden
corpus** (valid chains, every tamper class, every failure kind) that both
verifiers must classify identically in CI; a published tool-validation note
(error characteristics, versions) shipped in the evidence pack. When the
tool is challenged — it will be — the validation record answers.

**G10 — Say the promises out loud.**
Every recorder regime states its guarantees in a form outsiders can test
(Appendix M tolerances; APT certificates; CC Protection Profiles). Ours are
scattered across specs. Consolidate into the Promise Card (§4) and surface
it in the README/trust page — with the honest caveats attached, because
overclaiming is the one unforgivable sin in this genre ("cryptography is not
enough": relay/replay, sensor-side physical attacks, and pre-TOFU spoofing
remain, and we say so).

---

## 4. The Promise Card — what SecuraCV collects and what it promises

The product's evidence contract, stated the way a recorder regime would.
"Mechanism" is what enforces it; "check" is how anyone can test it; "limits"
is what we refuse to overclaim.

**What it collects (by the Witness Dictionary, G1):**
semantic claims only — the 8-kind event vocabulary (boundary crossings,
presence, acoustic impulse, vehicle-after-hours, contact change, object
removal, tamper) with coarse time buckets (10-minute default; 5-minute conformance-critical floor that cannot be narrowed without a ruleset change; jittered exports), local
zone labels, confidence, attestation tier; system-trace records (heartbeats
with per-bucket deltas, lifecycle, latched failures, key rotations,
correlation checks); custody records (export + break-glass receipts, holds,
erasure certificates). Never: frames, faces, plates, precise timestamps,
coordinates, identities, free text.

| # | Promise | Mechanism | Anyone can check by | Limits (stated, not hidden) |
|---|---|---|---|---|
| P1 | **Complete** — every covered bucket is accounted for: an event, a heartbeat, or a sealed gap/failure | heartbeat cadence + latched failures + offline intervals | timeline audit in `log_verify`/viewer | coverage ends where sensing ends; adapter-path gap sealing is G3 |
| P2 | **Intact** — nothing altered, reordered, or deleted inside the record | three hash chains + signed checkpoints | offline verifier, one click | tail truncation detection relies on heartbeat cadence (warning class) |
| P3 | **Authentic** — every record signed by the device that witnessed it | Ed25519 at source; domain separation; key lineage | signature check against pinned/published key | pre-TOFU broker spoofing window; physical key extraction (see P10) |
| P4 | **Time-honest** — coarse by design, anchored externally | TimeBucket floor + clock-skew latching + RFC 3161 anchors | compare anchors vs claimed buckets | buckets are deliberately imprecise (feature); anchoring is opt-in today |
| P5 | **Attribution-honest** — every claim declares its evidence tier | attestation field (device/adapter/ha-bridged), one-way | read the field; verify tier-appropriate signature | ha-bridged claims are only as strong as HA's own record |
| P6 | **Failure-honest** — interruptions, faults, and tampering are themselves sealed records | FailureType ladder, latched; tamper events; lifecycle pairing | look for the sealed record where the silence is | a perfectly clean power cut before first write leaves only the reopen record |
| P7 | **Un-forkable** — one history, the same for every reader | (roadmap G5) mesh cosigned heads; optional public log | compare heads across witnesses | single-device installs keep P2-P4 but not P7; we say so |
| P8 | **Stranger-verifiable** — no trust in us or the runtime required | offline viewer + CLI, byte-parity locked; golden corpus (G9) | run the tool; reproduce the fixtures | verifier trust rests on published validation, like any forensic tool |
| P9 | **Private by construction** — proof without exposure | invariants I–VII enforced in code — I/II at the type level (compile-fail tested), the rest by runtime gates and fail-closed policy; hash-only anchoring; two-tier split | read the export: nothing identifying is in it | privacy limits evidentiary richness — deliberately (the QAR/FDR trade) |
| P10 | **A known instrument** — you can know what code witnessed | signed firmware/OTA, versioned dictionary hash in checkpoints, (doc 12) release provenance | verify firmware signatures + dictionary hash | plaintext-NVS key on current canaries until flash encryption ships (doc 12 F2) — the honest asterisk |

The card's rule, borrowed from aviation: **capture maximally (within the
privacy floor), disclose minimally, promise only what a stranger can check.**

---

## 5. Roadmap

**Phase A — artifacts (docs/spec, no kernel changes):**
Witness Dictionary v1 as machine-readable schema + codegen for the four
vocabulary copies (G1) · custody/extraction ceremony doc with custody-form
and 902(13)/(14) certificate templates (G6) · Promise Card onto the trust
page/README (G10) · dictionary hash into checkpoints and export bundles (G1,
small kernel change riding along).

**Phase B — rituals and ceremonies (kernel/add-on/HA):**
Correlation Check flow — wizard/HA-guided stimulus ritual, `correlation_check`
record type, archived report (G2) · Hold + release records wired to tamper/
chain-fail/break-glass triggers and an owner action (G4) · erasure-certificate
ceremony (G4) · final-gasp/UPS terminal records + adapter-path gap sealing
(G3) · golden corpus + tool-validation note in CI and the evidence pack (G9).

**Phase C — the witness network (mesh/fleet):**
Mesh witness cosigning of chain heads sealed as records (G5) · scheduled
encrypted chain replication to owner destinations (G8) · optional public
transparency-log publication for maximal-assurance deployments (G5) ·
`SensorDisagreement` corroboration engine (G7).

Dependencies and fit: Phase A's dictionary is doc 12's FR-13 made concrete
and unblocks the cross-language golden vectors (doc 12 A3); the Correlation
Check extends doc 11's Verify Now / repairs surface; holds and erasure join
the break-glass ceremony family; G5/G8 give the Canary mesh its evidentiary
purpose; and the whole card feeds doc 11's Evidence Pack (§7.1) — which is
where a court first meets us, certificate included.

---

## 6. Sources

Aviation: 14 CFR 121.344 + Appendix M (eCFR; 88-parameter list, per-channel
range/rate/accuracy/resolution; §121.344(j) conversion-documentation duty),
14 CFR 25.1457 (CVR channels, RIPS, erase interlock), 91.609, ARINC
647A/FRED, ARINC 717/767, EUROCAE ED-112A/B, TSO-C123/124, EASA AMC1
CAT.GEN.MPA.195(b) (annual recording inspection), ICAO Annex 13 (custody,
§5.12 disclosure) + Annex 6 App. 8, 49 U.S.C. §1114/§1154, NTSB party/CVR/FDR
handbooks and Vehicle Recorder Division practice, 73 FR 12541 (2008 rule),
91 FR 4447 (2026 25-hour CVR final rule), GADSS; mishaps: United 585/USAir
427, SilkAir 185, Swissair 111, AF447, MH370, Air Canada 759 (ASR-18/04).

Other recorders: Reg. (EU) 165/2014 + 2016/799 Annex 1C (smart tachograph:
ERCA/MSCA PKI, ECDSA/AES/SHA-2, motion-sensor pairing ISO 16844-3, signed
.DDD, motion-data-conflict + security-breach events, biennial workshop
calibration, Galileo OSNMA/TESLA, Common Criteria PPs; Anderson 1998 and the
magnet/pulse/relay attack literature), 49 CFR Part 563 + Driver Privacy Act
2015 + Matos/Bachman/Christmann (EDR), IMO SOLAS V/20 + MSC.333(90) + annual
performance test (VDR), 49 CFR 229.135 + App. D + IEEE 1482.1 (rail), Axon
Evidence architecture (SHA-2 ingest, Merkle fingerprints, immutable audit,
original-preserving redaction) + CJIS, CTBTO IMS station-level signing +
tamper-detecting enclosures + CD-1.1/SSI.

Forensics & anchoring: ISO/IEC 27037/27041-3, SWGDE, ACPO principles, NIST
SP 800-86/800-101 + CFTT, Melendez-Diaz (custody gaps → weight), FRE
901/902(13)/(14) and advisory notes, UK PACE s.69 repeal + Post Office
Horizon lesson; RFC 3161 + eIDAS qualified timestamps, ANSI X9.95, RFC
6962/9162 Certificate Transparency, Sigstore Rekor, CoSi witness cosigning /
Armored Witness.

Repo grounding: `spec/invariants.md`, `spec/event_contract.md` (§2 event
structure, §2.1 replication/offline markers, §3 conformance-critical buckets,
§12 system-trace records), `docs/log_verify.md` (three ledgers, timeline
audit, failure diagnosis), `docs/timestamping.md` (log_anchor), `docs/
evidence_lifecycle.md`, `docs/device_trust.md`, doc 11 §7.1 (Evidence Pack),
doc 12 (Flight Rules FR-13, findings A3/F2/K7).
