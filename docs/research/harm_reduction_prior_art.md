# Harm-reduction messaging — prior art and design anchors

Status: Working brief, 2026-05-11
Purpose: Ground the Beacon channel and the hardened Chirp channel in established public-service messaging conventions so that (a) the system is genuinely useful, (b) it does not impersonate official alerts, (c) it is interoperable with existing infrastructure if a properly-authorized operator wants to feed it in the future.

This is a *design brief*, not a literature review. Citations are to canonical authoritative sources by name; specific paragraph references are noted where they shape a design decision.

## 1. OASIS Common Alerting Protocol (CAP)

**Source:** OASIS Standard, *Common Alerting Protocol Version 1.2* (2010). Also ITU-T Recommendation X.1303.

CAP is the data-format lingua franca of every serious public-alert system on the planet — IPAWS (US), Public Alerts (Canada), Public Warning System (EU), Google Public Alerts, NWS, Amber Alert America, Australian EWS, Chile SAE. Adopt its vocabulary verbatim wherever possible.

**Fields the Beacon and Chirp wire formats should be log-translatable to:**

```
identifier   — globally unique message ID
sender       — originator's identity (string)
sent         — origination timestamp (ISO 8601)
status       — Actual | Exercise | System | Test | Draft
msgType      — Alert | Update | Cancel | Ack | Error
scope        — Public | Restricted | Private
info[]       — one or more language-specific alert blocks
  category   — Geo | Met | Safety | Security | Rescue | Fire | Health |
               Env | Transport | Infra | CBRNE | Other
  responseType — Shelter | Evacuate | Prepare | Execute | Avoid | Monitor |
                 Assess | AllClear | None
  urgency    — Immediate | Expected | Future | Past | Unknown
  severity   — Extreme | Severe | Moderate | Minor | Unknown
  certainty  — Observed | Likely | Possible | Unlikely | Unknown
  effective  — when alert becomes effective
  onset      — when expected event begins
  expires    — when alert expires
  headline   — short (≤160 char) summary
  description — longer prose
  instruction — recommended actions
  area[]     — geographic scope (polygon, circle, geocode)
```

**Implications for our design:**
- The Severity/Urgency/Certainty triple ("SUC") is the universal handle. Adopt it verbatim — Beacon wire format carries `severity`, `urgency`, `certainty` as enum fields, not derived from template.
- `msgType` distinguishes a real alert from a drill (`Exercise`). We must wire this through end-to-end; drills must not enter the same statistics or trigger the same urgency UI.
- `scope` for Chirp and Beacon is always `Private`. **Never** `Public` (which has specific IPAWS meaning) and **never** `Restricted` (also IPAWS-specific, denoting recipients with privileged access). Document and lint this.
- `status = Test` is a separate signal from `msgType = Exercise`. Test is "the system is being verified, do nothing." Exercise is "we are doing a real drill, follow procedures." Beacon supports both.
- Digital signing in CAP uses XML-DSig. We do not use XML; we use Ed25519 over canonical CBOR. The semantic intent is the same and the CAP-export gateway translates between them.

## 2. IPAWS, WEA, EAS — what civilians may not originate

**Sources:** FEMA, *IPAWS Architecture and Operations* (2019); 47 CFR Part 10 (Wireless Emergency Alerts); 47 CFR Part 11 (Emergency Alert System).

**Hard rules our system must observe:**

1. **No civilian device may originate WEA, EAS, or any IPAWS-conformant alert.** Originator authorization is gated by FEMA's IPAWS Alerting Authority program. Our device is not one. Beacon is **not WEA**, must never represent itself as WEA, must never be received by a phone as a WEA.
2. **No impersonation of the WEA attention signal.** The two-tone (853 Hz + 960 Hz, ~8 s) attention signal at 47 CFR §10.520(d) is reserved. Playing it from a buzzer is an FCC violation. `audible_chirp.h::PATTERN_BEACON` must not use these frequencies in combination.
3. **No use of protected phrasing.** "Wireless Emergency Alert", "Presidential Alert", "AMBER Alert" (NCMEC trademark), "Silver Alert", "Civil Emergency Message", "This is a test of the Emergency Alert System" — all reserved. Our UI and audio strings must never use these.
4. **Receive-only future.** A properly-authorized operator (NWS, building manager with FEMA designation, etc.) could legally translate a CAP feed into a Beacon broadcast on their own devices via a designated gateway pubkey. Beacon spec supports this (`spec/beacon_cap_gateway_v0.md`) but the implementation is deferred — we don't ship a default gateway in v0.

**Why this matters operationally:** false-alarm civil liability for impersonating an emergency alert is real. The Hawaii 2018 false missile alert (§4 below) is the canonical case; the operator who pushed the button was sanctioned by the FCC and the state of Hawaii. We absolutely must not be in a position where a misconfigured Beacon could be confused with an official alert by either a recipient or a regulator.

## 3. NFPA 72 — supervised circuits, trouble vs alarm vs supervisory

**Source:** NFPA 72: *National Fire Alarm and Signaling Code* (2022 edition). The relevant chapters are §10 (Fundamentals) and §23 (Protected Premises Fire Alarm Systems).

The smoke-detector-grade reliability bar the repo owner is asking for has a name in fire-alarm engineering: **supervised signaling**. The core ideas:

- **Three operational states, distinct from each other and visually distinguishable**: `Normal`, `Trouble`, `Alarm`, plus `Supervisory` for non-fire conditions like a valve closed or a low-pressure switch.
- **Trouble != Alarm.** A trouble signal is "I cannot do my job right now" — battery low, wire broken, sensor disconnected, peer missing. It must be reported but must not be visually or audibly confused with an alarm.
- **Daily / weekly self-test.** §14 requires periodic self-test. A device that hasn't self-tested within its required interval enters `Trouble`. This is the discipline that makes a 30-year-old fire alarm panel still trustworthy.
- **Fail noisy, not silent.** A trouble or alarm condition must produce a distinguishable audible signal. A system that silently fails is worse than no system.
- **Anti-tamper.** Tamper switches on every enclosure; any tamper is a supervisory signal, not a fail-silent.
- **Single-point-of-failure rules.** No single wire, fuse, or device, when failed, may render the system inoperative. Mesh is a natural fit here — Beacon's two-device co-signing is the network-protocol analog.

**Apply to Beacon:**
- Beacon state surface in the UI and as an MQTT sensor is the NFPA four-state enum: `Normal | Trouble | Alarm | Supervisory`. Document the trigger conditions:
  - `Trouble`: time unsynced, OR airtime governor sustained >80% of cap, OR a paired neighbor's `BEACON_SELFTEST_OK` heartbeat has been missing for >36 h, OR the device's own Ed25519 key fails its self-check.
  - `Alarm`: an unsuppressed, dual-signature-verified Beacon frame is active locally with `urgency = Immediate` or `severity ≥ Severe`.
  - `Supervisory`: muted, urgency filter raised, opera-rotate-in-progress — informational, not fail-silent.
  - `Normal`: none of the above.
- Daily `BEACON_SELFTEST_OK` heartbeat signed by the device key; carries `(uptime, free_heap, last_alarm_time, key_self_test_ok)`.
- Receivers maintain a per-pubkey "last self-test received" timestamp; >36 h absence → Trouble.
- A device whose own self-test fails (key check, NVS read, RTC sanity) refuses to originate Beacons until repaired.

## 4. Hawaii 2018 false missile alert — what the world learned

**Source:** FCC, *Report on the False Missile Alert in Hawaii on January 13, 2018* (2018); Hawaii state legislative report (2018).

Mitigations adopted industry-wide after this event, in roughly the order they're commonly cited:

1. **Two-person rule for high-severity origination.** A single operator pressing a single button is no longer sufficient for actual emergency-broadcast origination at any IPAWS-authorized agency for the highest urgency categories.
2. **Mandatory cancellation channel.** Every alert system must have an instant cancellation path that is visually obvious and rehearsed. Hawaii took 38 minutes to push a correction in 2018 because the cancellation path was a tertiary menu.
3. **Separate test/drill semantics.** Drills must be distinguishable from real alerts at every layer. The Hawaii operator selected the wrong menu item; CAP `msgType=Exercise` and `status=Test` exist precisely to make this kind of confusion structurally impossible.
4. **Audit log of who originated, signed in real time.** Reduces deniability and accountability gaps.

**Apply to Beacon:**
- Two-pubkey cryptographic co-signing on origination — the technical analog of the two-person rule. Hardwired, not optional.
- `TPL_CLR_*` (all-clear) templates are already first-class in Chirp; carry the same into Beacon as `msgType=Cancel`. UI gives them the same prominence as origination.
- `msgType=Exercise` and `status=Test` are wire-format-distinct from `msgType=Alert`/`status=Actual`. Drills don't accumulate in alarm history, don't trigger `Alarm` state, don't count against the per-pubkey rate limit (separate bucket).
- Beacon audit log is append-only, chain-hashed (same primitive as witness records), signed.

## 5. AMBER and Silver Alert message conventions

**Sources:** US DOJ, *AMBER Alert: America's Missing: Broadcast Emergency Response*; National Center for Missing & Exploited Children, *AMBER Alert Best Practices* (2019); FHWA MUTCD §2L.

**Useful for our design (not for impersonation):**

- AMBER and Silver are tightly scoped: child abduction / endangered person, three-frame DMS sign, short verb-led phrasing, no opinion or speculation, specific data fields (vehicle make/model/plate, abductor description, direction of travel). They are *not* used for general-purpose advisories.
- The legitimate need they fill is community help in time-critical search. Our Chirp and Beacon channels deliberately do *not* compete with this — we have no template for missing persons because we cannot solve the identifying-information problem without violating our no-PII invariant.
- Their message-design discipline (three frames, short verbs, no editorial) is excellent for our DMS-style on-device display.

**Apply:**
- Beacon and Chirp templates use short verb-led phrasing (already the case: "fire or smoke visible", "evacuation in progress", "shelter in place advisory").
- Beacon receives ≤3-frame template + detail rendering, modeled after MUTCD DMS guidance.
- We never originate a missing-person template. Period.

## 6. FHWA MUTCD §2L — Dynamic Message Signs

**Source:** US DOT FHWA, *Manual on Uniform Traffic Control Devices* (2009 ed. with 2012 revisions), Chapter 2L.

The display-design body of knowledge for "limited bandwidth, high-stakes public communication." Key rules adopted:

- Three frames maximum per message; each frame self-contained enough to act on alone.
- Short verbs, no punctuation that fails legibility at distance.
- Standard abbreviations (`I-95`, `EB`, `S/B`, `RT`, `LT`).
- Avoid words that don't add information.
- Severity of phrasing must match severity of event.

These map to our on-device UI guidelines, not the wire format.

## 7. Harm-reduction movement — public health framing

**Sources:** DanceSafe (`dancesafe.org`), DrugsData (`drugsdata.org`), Energy Control (Spain), Harm Reduction International (`hri.global`).

The harm-reduction movement's hard rules — earned the hard way:

- **Never identify users.** Information that helps a person make safer choices is shared; information that identifies them is not.
- **Never tip law enforcement.** Anonymity is the precondition for the information flow.
- **Default to information that helps people stay alive.** Bad-batch warnings, naloxone availability, shelter locations, contaminated-water warnings, severe-weather advisories. "How do we keep people alive *today*" is the question.
- **Reject content that targets individuals.** Anything that names a person, describes a person, or could be used to target a person is off-limits.

These map cleanly onto Chirp's existing "no suspicious person" rule (`spec/chirp_channel_v0.md` §2.5.3 and our `chirp_channel.cpp:TEMPLATE_TABLE`) and onto Beacon's narrow life-safety template set.

**Apply:**
- No template in either channel may describe a person.
- The Chirp privacy firewall (ephemeral identity, no link to opera_id) is non-negotiable.
- The Beacon persistent-identity model is justified *only* by the higher trust requirement, and even then it carries only device pubkey — never household identity, never opera_id, never PII.

## 8. Nextdoor / Citizen / Ring Neighbors anti-patterns

**Sources:** Citizen lab and ACLU reporting on these platforms (2019–2023); FTC consent decree with Ring (2023).

The community-alert genre at scale converges on three failure modes:

1. **Racial profiling.** Free-text "suspicious person" reports become a racial-profiling engine. Anonymous broadcast plus free text plus no accountability is sufficient.
2. **Stalking and targeting.** Identifying details (descriptions, license plates, vehicle photos) become a substrate for harassment.
3. **Fear-of-the-day amplification.** Algorithmic ranking by engagement pushes the most alarming content; the steady state is "your neighborhood is dangerous, watch closely."

**Mitigations the genre has converged on (and we already adopt):**
- Structured templates only, no free text.
- No identifying details — no descriptions, no vehicles, no plates, no photos.
- Geographic scoping (3-hop, not city-wide).
- No engagement-ranked feed — chronological only, no boosts.
- Calm visual language, not red alarm colors.

**One mitigation the genre has *not* converged on but which our model adds:**
- Cryptographic origination authentication. Anonymity ≠ unaccountability; ephemeral session pubkeys mean a single bad actor can be rate-limited and suppress-voted without ever being identified.

## 9. Meshtastic / GoTenna / Disaster Radio

**Sources:** Meshtastic (`meshtastic.org`) protocol docs, GoTenna Mesh (sunset 2024, schema preserved on Internet Archive), Disaster Radio (`disaster.radio`).

Concrete prior art for ESP32-class mesh public-service messaging. What they got right that we adopt:

- **Strict frame discipline.** Meshtastic uses 237-byte LoRa payloads; ESP-NOW gives us 250 B. Our `MAX_MESSAGE_SIZE = 250` is correctly conservative.
- **Channel separation by magic byte.** Meshtastic's `magic[2]` discriminates app-level message types; we do the same with `0xC4` (Chirp) and `0xB1` (Beacon).
- **Airtime budgeting per channel.** Meshtastic limits to fractions of a percent of LoRa duty cycle per device. Our `airtime_governor` 2% cap is in this spirit but on the much higher-bandwidth 2.4 GHz radio.
- **Published open schema.** Meshtastic's protobuf definitions are public; independent receivers can verify. Our specs (`spec/chirp_channel_v0.md`, `spec/beacon_channel_v0.md`) are public; this is the right baseline.

**What they did better that we should follow:**

- Meshtastic's "store-and-forward" with hash-based dedup is more sophisticated than our 100-entry array. We're moving Chirp to Bloom-filter dedup (C9) in this branch — same intent.
- GoTenna's "shouts" required a hardware button press at every hop — analog of our "hold-to-send" pattern. We carry this into Beacon's co-sign UX.

## 10. FCC Part 15 and unlicensed device rules

**Source:** 47 CFR Part 15 (Unlicensed Operations).

Our hardware operates under Part 15 §15.247 (2.4 GHz ISM, ESP-NOW and WiFi). Two relevant rules:

- We may not cause harmful interference to licensed services and must accept interference from them. The airtime governor (2% cap) is a deliberate good-neighbor margin well below any regulatory threshold.
- We may not represent ourselves as broadcasting on behalf of a licensed emergency service. Naming the channel "Beacon" (or similar non-EAS terminology) and refusing to use protected phrasing or tones keeps us cleanly outside this trap.

## 11. Summary — design rules we adopt

These are the binding rules the Chirp and Beacon implementations must observe:

1. **CAP vocabulary.** Severity, urgency, certainty as enums on the wire. `msgType` (Alert/Update/Cancel/Exercise). `scope = Private` always.
2. **No impersonation.** No WEA tones, no IPAWS phrasing, no AMBER/Silver/Presidential terminology, no red color.
3. **Two-person rule for Beacon.** Multi-pubkey cryptographic co-signing on origination, not optional, not bypassable.
4. **NFPA-72 supervised state.** `Normal | Trouble | Alarm | Supervisory` as the publicly visible state surface for Beacon. Daily self-test heartbeat. Fail noisy.
5. **No PII, ever.** No person descriptions, no vehicles, no plates. Templates only.
6. **Ephemeral identity for Chirp; persistent device pubkey for Beacon.** No cross-channel correlation.
7. **CAP gateway interop is spec only.** Inbound and outbound mappings documented; no v0 implementation; gateway alerts get a clear "from gateway X" badge, not elevated urgency.
8. **Cancellation is first-class.** Every alert has an all-clear; every alert has a fast cancellation path.
9. **Drills are first-class but distinct.** `msgType=Exercise`, separate cooldown bucket, separate audit trail.
10. **Hold-to-send.** No accidental origination. Two-second hold on the originator, additional hold on the cosigner.

These are the rules that take us from "yet another community-alert app" to "smoke detector for a neighborhood."
