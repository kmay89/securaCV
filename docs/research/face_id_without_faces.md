# Research — Face ID without a face at the camera: household keys, not familiar faces (2026-10)

Can a household get what HomeKit Secure Video's face recognition sells —
"Mom is at the front door," no alert when the family comes home, a real alert
when a stranger does — on cheap hardware, with no Apple device in the house
and nothing leaving it? **Partly — and only by recognizing a key, not a
face.** The camera keeps witnessing *what* happened (`Person` at the front
door) and never learns *who*. A household member presents a key they carry —
a phone, a watch, an NFC sticker that costs under a dollar or a card that
costs a few, a fob — and that signed act, not the camera, sets the household
posture (Home, Away, Night), which is where "don't alert me about my own
family" honestly comes from. A key proves possession, not identity (a borrowed
card reads the same), so the system never names anyone: "Mom is at the front
door" is the part this design refuses. If a biometric is wanted, it is the
Face ID or fingerprint sensor already on the person's own phone, unlocking that
key — which is how Face ID itself works: the face unlocks a key inside a
secure element, and the other side only ever sees a signature.

The second half of the answer is the line. A camera that recognizes faces has
to compute a face template for **everyone** who walks into frame — the courier,
the neighbor's kid, the person on the sidewalk — in order to decide that they
are not Mom. That is world → system identification. It is what Invariant II
forbids, what the voice contract already refused for voices, what the 2026
bystander class actions against Ring and Google are about, and — the finding
that settles it for cheap hardware — something a sub-$60 board cannot do to
Face ID's standard anyway.

> **Scope and status.** The hub side of this design is built and host-tested:
> the household-key checker ([`household/`](../../household/)) and the
> attention engine ([`src/relay/attention.rs`](../../src/relay/attention.rs),
> vectors in [`spec/fixtures/attention_v1.json`](../../spec/fixtures/attention_v1.json)),
> to the contract in [`spec/household_keys_v0.md`](../../spec/household_keys_v0.md).
> **Not built:** the key-satellite firmware, the phone and watch apps, any
> Canary acting on a household posture, and the relay wiring (nothing calls
> the engine). None of it has been on a bench or had an external security
> review. The canary-wap household decisions are in a
> [decision record](../design/household_presence_and_posture.md). Sources are
> at the end (swept 2026-10-13); repo claims cite the file that proves them;
> prices are October 2026 snapshots, up this year (DRAM-driven increases hit
> every single-board-computer tier).

---

## 1. The invariant question comes first

Three different things get called "Face ID for the house." They are not the
same request, and the constitution treats them differently:

| Direction | What it is | Example | Verdict |
|---|---|---|---|
| **World → system** | The camera identifies whoever walks into view (1:N) | HKSV Face Recognition, Ring Familiar Faces, Nest familiar faces, Frigate face recognition | **Forbidden. Invariant II, forever.** No revisit trigger exists, and none is proposed here. |
| **Owner → system, by biometric on our hardware** | A device of ours enrolls a household member's face and verifies it 1:1 when they present it | A "face key" door verifier — Face ID rebuilt on an ESP32 | **Not allowed.** It would need two RFCs, the first amending governance itself (§11), and the voice contract's rule 5 is precedent *against* it. |
| **Owner → system, by possession** | A household member proves membership with a key they carry; any biometric happens on *their own* device | Phone or watch key, NFC card, FIDO2 key, fob — Face ID on the phone gating the key | **Recommended, design stage — inside the contract in §6.4, and through the posture/inbound RFC (§13 step 3) before any Canary acts on it.** That RFC names the roster as a stored set of long-lived per-household identifiers (governance §2.3, §4), the posture journal as a new record type, and a credential check driving Canary sensing against Free Signals D. The hub side is built. |

The constitution, for the record:

- [`spec/invariants.md`](../../spec/invariants.md) Invariant II: the kernel and
  its modules MUST NOT "extract or emit license plate numbers, facial
  embeddings, biometric identifiers, or stable object identifiers," nor "emit
  hashes or tokens that remain comparable across devices, locations, or long
  time horizons." Invariant I adds that embeddings "MUST be treated as raw
  media." Conformance requires enforcement "in code, not configuration" — so
  "opt-in face recognition" is not a lighter version of the request; it is the
  same request plus a flag.
- [`AGENTS.md`](../../AGENTS.md) non-negotiable 1, the
  [FAQ](../FAQ.md#does-it-do-face-recognition) ("No — and it can't"), HKSV
  familiar faces' "Refuse, permanently … Not deferred: absent"
  ([`apple_home_integration.md`](../design/apple_home_integration.md) §1.3),
  and the doorbell's revisit trigger "none"
  ([`canary_doorbell_research.md`](../hardware/canary_doorbell_research.md)).
- [`whisper_local_voice.md`](whisper_local_voice.md) §3.1 drew the project's
  only carve-out — owner → system voice *commands* — by **hardware** ("which
  device can feed the pipeline at all"), not by recognizing who is speaking;
  its rule 5 refuses "only respond to my voice" enrollment "regardless of how
  convenient it would be." The face version of that sentence is Face ID.
- [`docs/governance_and_invariants.md`](../governance_and_invariants.md) §3.2:
  a weakening change needs a formal RFC that must "demonstrate why a
  strengthening alternative is not viable." §6 *is* that alternative.

The code already acts on the line: Frigate's recognized names (`sub_label`)
and plate strings are parsed as absent when its events are read in, and the
tests plant marker values to prove none get through
([`src/transport/frigate.rs`](../../src/transport/frigate.rs),
[`tests/frigate_integration.rs`](../../tests/frigate_integration.rs)).

## 2. What Apple actually does — and what is worth copying

HomeKit Secure Video is two very different designs under one name.

**The custody model is genuinely good, and worth copying — all but its
any-household-device-opens-any-clip half (§8 item 9).** Per Apple Platform
Security (May 2024 edition, read in full; current web excerpts match):

- The camera sends a clip to the **home hub**, which decrypts and analyzes it
  **locally**; off the LAN, the hub relays live view **without decrypting it**.
- A kept clip is encrypted with **AES-256-GCM under a fresh random key**:
  ciphertext to iCloud, the key to CloudKit under end-to-end encryption. Apple
  stores ciphertext; the household's devices hold the keys.
- Identities are **Ed25519**; accessories pair with SRP-6a (3072-bit, an
  8-digit code, refusal after 100 failures) and sessions are Station-to-Station
  over X25519, HKDF-SHA512 and ChaCha20-Poly1305. HomeKit routers' "Restrict to
  Home" blocks a camera's internet egress at the network layer.

**Face recognition is the opposite design.** The same guide says HomeKit
stores "information about each person, such as name, as well as images
representing that person's face" in CloudKit, sourced from the owner's Photos
(opt-in) **or "previously analyzed IP camera video"**, and the hub identifies
faces in the stream: 1:N identification of anyone in view, with a synced
gallery that grows from footage of visitors. It needs an iCloud+ plan and a
home hub, which under the new Home architecture can only be a HomePod or an
Apple TV.

**Face ID is the design to learn from.** An attention check; IR and depth
captures from a randomized sequence, signed and matched inside the Secure
Enclave and never exported; anti-spoofing networks; the passcode after five
failures or 48 hours; and a match **releases a key** (it unwraps the Data
Protection class keys) rather than returning a yes/no. Apple states a
random-person false-match probability under 1 in 1,000,000 (1 in 500,000 with
two enrolled appearances; Touch ID 1 in 50,000), higher for twins, look-alike
siblings and children under 13.

The lesson: **the biometric never leaves the person's own secure hardware;
what leaves is a key operation.** §6 builds that, and the repo already does it
once: [`EnclaveCustody.swift`](../../ios/Sources/SecuraCV/Security/EnclaveCustody.swift)
wraps the sealed-snapshot key under a Secure Enclave key usable only after
Face ID, Touch ID or the passcode — an unlock gate, never an identity in a
witness record.

**A correction.** Apple's **HKSV Open Source Compatibility Guide** (Developer
Preview, 2026-06-03) never says the CMAF recording publishing point is on the
home hub, as [`apple_home_integration.md`](../design/apple_home_integration.md)
§1.2 did. The characteristic carrying the publishing-point URL and server CA
certificates is Paired Write, so a paired controller presumably writes it, and
implementers report the camera uploading to an Apple endpoint. That sentence
was corrected. The guide grants no license and needs HAP R17 (MFi licensees
only), and HKSV still needs an Apple home hub, so the verdict stands.

## 3. What everyone else does — who processes where, and what leaks

| Product | Where faces are matched | What leaves the house | Notes |
|---|---|---|---|
| **Apple HKSV** | Home hub (HomePod/Apple TV) | Encrypted clips + an E2EE gallery of names and face images (iCloud) | Needs iCloud+ and an Apple hub |
| **Ring Familiar Faces** (Dec 2025) | Ring's cloud ("encrypted and stored in the cloud, not on your Ring devices") | Faces of everyone the camera sees; unnamed faces kept 30 days | Incompatible with Ring E2EE; geofenced out of Illinois, Texas and Portland, OR (§5); putative bystander class action *Sigwalt v. Amazon.com* (W.D. Wash., filed 1 June 2026) |
| **Google Nest familiar faces** | Not stated by Google | Google Home Premium subscription | Off in Illinois "as a precaution"; three N.D. Cal. bystander suits filed Jun–Jul 2026 |
| **Eufy** (HomeBase 3 "BionicMind") | On the HomeBase | Marketed as local — in 2022 thumbnails and face data were found going to AWS with cloud storage off | NY AG settlement, $450,000 (Jan 2025) |
| **Frigate** 0.16+ (Aug 2025) | Your own server (YuNet; FaceNet on CPU or ArcFace on GPU/NPU) | Nothing for recognition itself — but configured GenAI descriptions can send frames (and the name) to a cloud LLM | Keeps up to 200 recent face crops **including unrecognized people**, and publishes every attempt over MQTT |
| **UniFi Protect** | On-premises (camera or AI Port/Key) | Nothing by design | $210–$1,155 of accessories |
| **Reolink** | — | — | Says its cameras do not currently offer face recognition, citing privacy |

Three patterns matter here:

1. **"AI features" and end-to-end encryption are mutually exclusive in the
   cloud products.** Ring Familiar Faces cannot be used with Ring E2EE, and
   Ring's 2026 "TAKE" default exists so its cloud can still decrypt frames for
   up to 24 hours for AI features.
2. **Every product that sells familiar faces computes a template of every face
   in view**, including the "unknown" ones — Frigate locally, Ring in the
   cloud, both to people who never agreed.
3. **The documented third-party failures are about custody, not
   cryptography**: Ring's 2023 FTC case ($5.8M; employee access to private
   videos), Wyze's February 2024 cross-account thumbnail exposure (about 13,000
   users), Eufy's 2022 "local" uploads, and Ring's police pipelines (Axon
   Community Requests; the Flock Safety tie-up announced and then canceled in
   February 2026 before launch). The surest way not to leak a face is never to
   compute one.

**Matter 1.5** (November 2025) standardized cameras — WebRTC live view and
CMAF clip upload over HTTPS — but no analytics and, as far as this research
could confirm, no end-to-end encryption for recordings; as of October 2026 only
SmartThings ships it. It is a transport to interoperate with later, not a
recognition design (verdict in §12).

## 4. Can cheap hardware do Face ID? The honest numbers

Compute is mostly not the problem: ESP-DL publishes about 250 ms per 1:1 match
on an ESP32-S3 and 82–148 ms of model time on an ESP32-P4, and the $7 Luckfox
Pico (RV1103) ships a face-recognition demo, though it publishes no latency
figure. What cheap hardware cannot do is the part that makes Face ID Face ID:
**liveness, accuracy at a one-in-a-million false-accept rate, a key that
physical access cannot extract, and a model you are allowed to ship.**

| Tier (Oct 2026 price) | What it can honestly do | Accuracy published (true-accept rate, TAR, at a false-accept rate, FAR) | Liveness (presentation-attack detection, PAD) |
|---|---|---|---|
| ESP32-S3 (ESP32-S3-EYE kit, $47.50) — Espressif ESP-DL / ESP-WHO, MIT | RGB 1:1; published model time ≈247 ms (`mfn_s8_v1`) plus ≈4 ms preprocessing | 90.03% TAR at FAR 1e-4 on IJB-C, a public face benchmark (`mfn_s8_v1`) | None shipped. Assume a printed photo works. |
| ESP32-P4 (Function EV board, ~$55 at launch) | RGB 1:1; published model time 82–148 ms | 93.94% TAR at 1e-4 (`mbf_s8_v1`) | None shipped |
| Luckfox Pico RV1103/RV1106 ($7–27), MaixCAM ($40–76) | RetinaFace + MobileFaceNet / InsightFace R50 demos | Model-dependent | None |
| Kendryte K230 (CanMV, ~$50) | The only sub-$60 SoC found whose SDK documents a hardware depth engine for "3D face recognition" (structured-light speckle) | — | Architecturally possible; projector/IR module cost not found; nothing certified |
| Raspberry Pi 5 + AI HAT+ (Hailo) — Pi 5 2 GB is $77.50 since 2026-10-01 | Massive headroom (arcface_mobilefacenet at 1,666 fps on Hailo-8L, LFW 99.4%) | Ships InsightFace-derived weights | None (RGB) |
| Commercial 3D face locks — SwitchBot Lock Vision ($170–230), Eufy E40 ($300), Lockly Visage ($349) | Structured-light/IR face unlock, templates kept on the lock | Vendor claims only (e.g., "below 0.0001%") | No public iBeta PAD letter found for any consumer lock |

What the table means:

- **Accuracy.** Apple claims under 1e-6 FAR; the best small open model
  publishes 90–94% true-accept *at 1e-4* — about a hundred times more false
  accepts, while rejecting one honest attempt in ten. LFW scores (Labeled
  Faces in the Wild, another public benchmark; 99.4–99.8% above) cannot tell
  a door-grade verifier from a Face ID-grade one, and NIST IR 8280 found 1:1
  false positives 10–100× higher across demographic groups.
- **Liveness.** No RGB-only open stack ships presentation-attack detection
  (the best open model calls itself "limited in robustness"); consumer locks
  have no public iBeta letter; and "DepthFake" (2023) spoofed commercial
  depth-based liveness with a 2D photo.
- **The template is the face.** Inversion attacks rebuild faces from
  embeddings that unlock the very system they came from: more than 78% through
  a fuzzy commitment at a 0.1% FAR (Keller et al.; against a different system,
  only 50–120 times its FAR); about 96% against ArcFace on LFW at a 1e-3
  false-match rate (Otroshi Shahreza et al., 2025, from an excerpt — ArcFace is
  the easiest victim, as the attack's generator is conditioned on ArcFace
  embeddings); 84–93% against inversion-resistant systems (DiffMI, 2025). Every
  cheap protection scheme has a published break for faces (fuzzy commitment,
  IronMask, fuzzy extractors under PIPE, BioHashing once its token leaks).
  Invariant I's "embeddings are raw media" is the literature.
- **The key comes out.** The ESP32 family's secure boot and flash encryption
  have a long record of physical attacks — CVE-2019-17391, Ledger Donjon's 2022
  key recovery on ESP32-V3, CVE-2023-35818, the C3/C6 side-channel and
  fault-injection chain (AR2023-007), and in 2026 AR2026-005 (voltage-glitch key
  recovery from the S3's general-purpose AES accelerator, not its
  flash-encryption block; rated low by Espressif) and AR2026-006 (ROM ECDSA
  secure boot accepting invalid signatures). Assume a template on such a device
  is extractable in a few days — and **a face cannot be revoked.** (The repo's
  own boards are weaker by default: NVS, the on-chip key–value store, is
  plaintext at rest on every current build — §6.8.)
- **Licensing.** Code is usually permissive; **weights** mostly are not.
  InsightFace's models (and the Hailo and MaixPy defaults derived from them)
  are "for non-commercial research purposes only," most others trace to
  withdrawn or scraped sets (MS-Celeb-1M, WebFace260M, VGGFace2), and ESP-DL
  does not disclose its training data.
- **Distance.** All of this is the *easy* case, a cooperative person at
  0.3–1 m. A doorbell camera at 3–10 m, at an angle, at night under IR, is much
  weaker — and it is the world → system case §1 forbids anyway.

**Verdict for cheap hardware:** face *convenience* is buildable for under $60;
Face ID-class *security* is not, and no design in this price range should claim
it. The cheapest honest factor that clears "a stranger can't get in" is not a
better face model — it is a key.

## 5. The law draws the same line

The legal research converged on the distinction §1 draws:

- **EU AI Act.** *Biometric verification* is **one-to-one** (Art. 3(36)), and
  verification for "unlocking a device or having security access to premises"
  is excluded from remote biometric identification (Recital 17, Annex III
  point 1(a)), which is high-risk — the Commission's own example is identifying
  a burglary suspect from **a CCTV camera on private property.** Art. 2(10)
  exempts only a natural person's personal *deployer* obligations, not a
  provider's. Regulation (EU) 2026/1744 moved the Annex III date to
  2 December 2027 — a delay, not a rewrite.
- **GDPR.** The household exemption fails once a home camera covers public
  space (CJEU *Ryneš*, C-212/13, 2014). EDPB Guidelines 3/2019's hotel example
  is the bystander problem in one sentence: recognizing enrolled VIPs is
  unlawful "unless all other guests monitored (in order to identify the VIPs)
  have consented." **Rejecting a face is still processing it.**
- **Illinois BIPA.** "Private entity" includes **"any individual"** (740 ILCS
  14/10), so a homeowner is covered on the statute's face: written release
  before collection, $1,000 per negligent and $5,000 per intentional or reckless
  violation, a private right of action (P.A. 103-0769, 2024, made repeated
  collection from one person one violation), and no verification or security
  exemption.
- **Elsewhere:** Texas CUBI (commercial purpose; AG enforcement up to $25,000
  per violation, behind the $1.4B Meta and $1.375B Google settlements);
  Colorado HB24-1130 (any amount of biometric data, since 1 July 2025);
  Washington; California (biometrics are sensitive personal information); and
  the FTC (the 2023 Rite Aid order, a five-year ban plus deletion of derived
  models; its May 2023 policy statement makes unsubstantiated biometric
  accuracy claims a Section 5 problem).
- **The vendors' answer is a geofence.** Amazon says Ring's Familiar Faces is
  not offered in Illinois, Texas or Portland, Oregon; Google and Wyze turn
  theirs off in Illinois; Arlo in Illinois, Texas and Quebec. A local-first,
  account-free, open-source system **cannot** geofence — it would need location
  telemetry it refuses to collect, and a fork can delete the check — so
  compliance has to come from the design. A design that never computes a
  template stays outside every biometric statute surveyed here, the point
  [`legal-audit-2026-07.md`](../legal-audit-2026-07.md) already makes ("keep
  that line bright; it's a genuine differentiator"). The ordinary duties of a
  camera that sees public space (*Ryneš*) apply with or without faces.

## 6. The design: household keys — Face ID's architecture, with the face left on your own phone

### 6.1 The shape

```
 tap / hold at the door                               nothing here ever sees a face
   │
   ▼
 key satellite (new device class, never a Canary)  ── NFC field only, ~10 cm   [not built]
   │  serves a fresh hub challenge over NFC
   ▼
 the person's credential:
   phone / watch ── its OWN Face ID / Class 3 fingerprint unlocks a P-256 key → ES256 signature
   FIDO2 key     ── an assertion over SHA-256(challenge)
   card          ── NTAG 424 DNA mutual authentication, RndA = SHA-256(challenge)[0..16]
   fob           ── ES256 signature after a button press
   ▼
 hub checker (own crate and process)                                [built, host-tested]
   │  checks the response against the roster (public keys; card AES keys sealed
   │  on the hub, §8 item 1); only an intent credential may set Home
   ▼
 one household COMMAND ── set the household posture (Home / Away / Night),
   │                      as an Ed25519-signed 112-byte statement
   ▼
 the household posture changes what indoor witnesses sense — for everyone,
 equally; it never rewords, demotes or delays any event that is sensed
```

Canaries are untouched. A door Canary keeps reporting `Person` at the front
door in a coarse time window, exactly as today, whether or not a key was used.
The household sees both facts — "Person at front door" and, on its own line,
"Posture: Home" (no door, no credential, a coarse time — a display not built
yet; the statement behind it states the minute it was issued, §6.4 rule 4) —
and the system never puts them side by side; any inference is the human's.
That is Free Signals Invariant D as written: "Canary presents signals; humans
interpret meaning."

No Canary applies a household posture yet, so the hub's attention engine
stands in: while a valid Home is current, an indoor witness's non-critical
events reach no one — the outcome tamper-only sensing would give — and the
witness log is unchanged. The rule is the same for every event from that
witness, and no credential is an input
([`spec/household_keys_v0.md`](../../spec/household_keys_v0.md) §7).

### 6.2 Why commands, and not "recognition by key"

Two shortcuts look tempting and both fail: letting a key presented at the door
*reword* the Canary's alert for that door ("a household key was used" instead
of "Person at front door"), and letting a nearby household phone quiet it.

- **It is the hard stop.** [`spec/canary_free_signals_v0.md`](../../spec/canary_free_signals_v0.md)
  §5 says a Canary "MUST NOT be used to … distinguish 'authorized' from
  'unauthorized'." Joining a credential to a Canary's `Person` event to decide
  its wording uses the Canary's output to make exactly that split, wherever the
  reader lives. Invariant F adds "no 'known device' vs 'unknown device'
  distinction in event output."
- **Passive phone presence is not evidence of anything.** A phone's Bluetooth
  Low Energy (BLE) resolvable private address is 22 random bits plus a 24-bit
  hash keyed by its identity resolving key (IRK), and resolution takes **no
  time input** — so one sniffed advertisement keeps resolving indefinitely, not
  just for the ~15-minute rotation. It relays live with about 8 ms added (NCC
  Group, 2022) and carries through walls, so "a household phone is nearby" is
  true whenever anyone is home — and a phone hands the same IRK to every peer
  it bonds with, so it is effectively a global identifier.
- **The repo already learned this lesson once.**
  [`HomePresence.swift`](../../ios/Sources/SecuraCV/Alerts/HomePresence.swift):
  "An inference meant to remove noise had quietly become a mute switch on the
  loudest alarm … a false 'away' is silence during a fire, a false 'home' is a
  notification you didn't need. We take the notification."

What survives is **a deliberate, signed act by a household member, presented
at a door: a command** — like a voice command, but carrying a signature, which
is why it may do what voice may not (voice contract rule 4: voice never arms or
disarms, because "a voice command carries no signature").

### 6.3 What a key does, and what it never does

| A household key may | A household key may never |
|---|---|
| Set the **household posture** (Home / Away / Night) — the vocabulary [`apple_home_integration.md`](../design/apple_home_integration.md) open decision #3 says must exist first, dictionary-first (now the dictionary's `household_posture` block) | Reword, demote, quiet, collapse or delay any Canary event or its notification |
| Act as **the inbound trust surface** decision #4 asks for. A tap supplies possession and presence at the door — not the inside, on-device press the repo's physical-confirm idiom means (announce → press-BOOT; [`test_console.md`](../design/test_console.md), [`usb_onboard.md`](../design/usb_onboard.md)), which a single-approval enrollment still requires (§6.6) — and needs no Apple Home | Touch the chain, Beacon, break-glass, retention or configuration |
| Drive "quiet when home" (indoor witnesses drop to tamper-only while Home) — less witnessing of the family at home — but only from a credential that proves intent (a phone or watch key with user presence, a FIDO2 key with user verification); a card or fob is possession, not intent (§6.5), so it may raise sensing (Away, Night) and never lower it | Mute tamper, integrity, offline or pattern pokes, or stop an outdoor witness |
| Be revoked in one step, by epoch | Name anyone, anywhere — no label, handle or per-person count in any output |
| | Unlock a door or actuate anything on a Canary (Free Signals D) |
| | Work passively, or from outside the home network — the checker closes any connection from a peer that is not on it, a peer-address check that a proxy or VPN re-originating traffic from a private or loopback address defeats (§6.4 rule 7) |

### 6.4 The household-key contract (the load-bearing part)

Modeled on the voice contract in
[`whisper_local_voice.md`](whisper_local_voice.md) §3.1:

1. **The boundary is the hardware.** Credentials enter only through a **key
   satellite** (or the hub itself), never a Canary. Canary RF stays symmetric
   and identity-free. The satellite has no camera, no microphone and no sensor
   that witnesses.
2. **Presentation, never observation.** Every input is a deliberate act inside
   a ~10 cm NFC field, a fob's button, or a confirm on a phone at the
   satellite; nothing listens for passing phones. The satellite forwards no UID
   and reads no card data: a household credential is told from a stranger's
   only by selecting the household application (phones) or by a first
   authentication with an enrolled key (NTAG 424 DNA, which has only its fixed
   NDEF application), so a bystander's transit or payment card is never read.
3. **Commands, not classification.** A key's only effect in v0 is a posture
   change, which applies to every event equally. It never joins to a witnessed
   event.
4. **Transient by default.** Challenges are single-use, live ≤ 10 s, in RAM;
   no per-credential counter, usage tally or arrival history exists (the rate
   limiter counts rejections per door and lane, in RAM, never per credential).
   If posture changes are journaled at all, it is as a new sealed-log record
   type — a ruleset change ([`spec/event_contract.md`](../../spec/event_contract.md)
   §9) that starts in the dictionary (AGENTS.md rule 5) — with only a coarse
   time bucket, *without* the credential, the door or a precise time. Whether
   even that is too much (a daily posture change is a coarse arrival diary) is
   for the posture RFC; the checker as built journals nothing. It does not
   blur the latest statement, though: the current statement states the minute
   it was issued (`issued_at`), the statement file's modification time states
   its second, and any live reader of that file or of `GET /v1/posture` learns
   each new statement — a command (a renewed Home included), a lapse, a
   roster change that ends Home, a restart, or a republish after a failed
   write or a hub clock set back — to its own polling precision. That too is
   for the posture RFC, next to Home Assistant's exact-time diary
   ([decision record](../design/household_presence_and_posture.md) §8).
5. **No who, in any output** — journal, alert ledger, digests, Watch
   snapshots, MQTT, Home Assistant, logs. The roster holds random handles and
   refuses any field that could hold a name. Every tap gets the same neutral
   "read" feedback at the door; acceptance shows only on the presenter's phone,
   so the street never learns who is "authorized."
6. **Fail toward today, and Home lapses.** Jamming, a dead satellite, a
   flooded LAN API or a missing key never lower sensing — they hold the
   current posture, and a held Home still lapses; a failed tap changes
   nothing. Home is the one state below today's — left set on an empty house,
   it is less witnessing than today — so it lapses to full sensing on its own
   (12 h by default, at most 24 h after it was issued), the hub restarts in
   Away, and a statement a witness cannot verify reads as Away: a forgotten
   Home is bounded, not removed. A witness refuses a statement issued more
   than 5 minutes ahead of its own clock — and still records its sequence
   number as the last accepted, so it can never count later, once its time
   arrives — so a Home counts for at most 24 h 5 min of each witness's own
   clock, whatever the hub's clock said; a hub
   clock more than 5 minutes ahead (give or take the minute floor on
   `issued_at`) is refused (witnesses behave as Away), and one running behind
   lapses Home early. Home silences nothing the relay carries and adds no
   remote poke class (widening the relay's vocabulary "is a privacy decision,
   not a tweak" — [`src/relay/mod.rs`](../../src/relay/mod.rs)).
7. **Local only, forever.** No cloud, no Apple or Google service, no passkey
   "hybrid" tunnel, no URL on any tag (a background-NDEF universal link would
   bring in Apple's associated-domains CDN and a public hostname). Off the home
   network, a key does nothing: on any bind the checker closes, without a
   response, every connection from a peer that is not loopback, private or
   link-local (the 100.64.0.0/10 range Tailscale and carrier NAT use, and
   Tailscale's IPv6 range `fd7a:115c:a1e0::/48`, unique-local in form, are
   refused too). That is a peer-address check — a strong default, not a
   cryptographic guarantee. A proxy, VPN or ingress that re-originates
   traffic from a private or loopback address defeats it, so the hub's LAN
   API must not sit behind one that accepts peers from elsewhere.

### 6.5 Components

| Piece | What it is | Notes |
|---|---|---|
| **Key satellite** (not built) | XIAO ESP32-C3 (already in the repo) + ST25R3916 NFC front end; MCU inside the wall, antenna outside | Its own device class, like the voice satellite, secure by default on ESP-IDF (spec §6.3): NVS encryption, Secure Boot v2 RSA-3072, flash encryption in release mode, secure download mode — still open, like any C3, to AR2023-007 given days of access — a tamper switch, and a first-boot identity drawn inside `bootloader_random_enable()`/`disable()` as `firmware/projects/canary-vision/src/witness.cpp` does. No member material. A padded uplink metronome keeps its own traffic from timing taps (Invariant III; the [`src/bridge/homekit.rs`](../../src/bridge/homekit.rs) pacer); indoor witnesses going quiet after a tap is a separate signal the posture RFC must pace. |
| **Hub checker** (built) | [`household/`](../../household/): its own crate and process, not `witnessd` | Roster in a private data directory; card keys and the hub's own signing keys (the posture key witnesses pin, the roster key that countersigns every accepted roster) sealed under a key-encryption key held outside it. Sealing protects a copy of the directory, not a disk image or a backup that also holds that key, and keeping that key out of any backup that holds the directory is the operator's job: v0 does not, and cannot, enforce it. That key still needs a backup of its own, somewhere safe: losing it loses the household (a new household, a new posture key with every witness paired again, every admin and member enrolled again). Paired clients reach it over a LAN API with no TLS of its own in v0: without a TLS proxy on the hub, a listener on the LAN sees each client's bearer token and, in a command, the member handle. The crate split stops kernel code calling it; keeping witnessd's Unix user out of its files, and giving it no internet route, are `won't`s the deployment configures, not `can't`s. |
| **Phone and watch keys** (apps not built) | Android Keystore/StrongBox P-256 behind a Class 3 biometric; iOS Secure Enclave P-256 behind `.userPresence` — never "express" keys | A foreground NFC reader session against the satellite's emulated ISO 7816-4 card — iPhone: Core NFC `NFCISO7816Tag` on a custom AID under `com.apple.developer.nfc.readersession.iso7816.select-identifiers` (the self-service Tag Reading capability, not the NFC & SE Platform entitlement); Android: reader-mode `IsoDep`, no HCE. The challenge lives only in the RF field, so it cannot be stockpiled. |
| **Cards** | NTAG 424 DNA (AES-128, ~$0.45–1 a sticker, ~$2.50–5 a PVC card) or DESFire EV3 (EAL5+, relay-resistant proximity check) | *Possession, not intent* — a card answers any reader field — so Away or Night only; offer shielding sleeves. Random ID on, so the hub trial-authenticates each enrolled card's own key, bounded by the roster (16 in v0, always 16 attempts); check the failed-authentication counter first. Never diversified from the UID; no class key. |
| **FIDO2 keys** | Any NFC key speaking FIDO2's CTAP2 (Client to Authenticator Protocol), $15–29 | With no keypad on the satellite, user verification (UV) needs a key with its own fingerprint sensor, registered `credProtect` 3 as *intent*. A plain key is `credProtect` 2, *possession*, and its holder is told a harvested credential ID and relying-party ID let anyone recognize it later with a silent `up=false` probe. No signature counter is stored (clone detection traded for no usage tally). |
| **Fob** | nRF52840, one button (~$10–12 DIY) | Possession. Signs after a button press, and is silent until pressed: no BLE advertising. |

**Two curves.** Member keys are P-256 (phones' secure elements, passkeys,
FIDO2, Aliro); the household's own signatures stay Ed25519. Admin entries are
algorithm-tagged (`ed25519 | es256`, `cli | enclave | fido2`) with
per-algorithm checks and domain separation, and one admin is one person,
decided at an enrollment ceremony, not by de-duplicating key bytes
([`src/break_glass/core.rs`](../../src/break_glass/core.rs)'s Ed25519-only
`QuorumPolicy` was a pattern, not a structure to reuse).

### 6.6 Protocol and roster, in brief

The wire contract is [`spec/household_keys_v0.md`](../../spec/household_keys_v0.md).
The challenge binds purpose, door, roster epoch and a single-use 32-byte nonce
(72 bytes, valid 10 s, bound to the asking session). The hub accepts a response
only at the **current** epoch and only if the credential's class allows the
purpose, then signs a new statement.

- **The roster** — member public keys and card-key references, random
  handles, no field that can hold a name, a strictly increasing epoch, admin
  approvals and a hub countersignature. A satellite will keep a hub-signed
  epoch high-water mark so a restored backup cannot revive a revoked key; until
  then a restore of the whole data directory rolls it back (a v0 limit).
- **Changes.** Adding a member takes m-of-n admins and, when m is 1, a
  physical confirm on the *inside* (v0: an operator command on the hub host).
  Revoking is cheap on purpose — any one admin, or the holder. A roster that
  drops any member ends a current Home once the hub loads it: nothing records
  who set Home, so one a revoked member set cannot be told from anyone
  else's, and ending it is the fail-safe; members still enrolled set it
  again. Changing the admin set or threshold takes m-of-n current admins and
  24 hours pending, so with a threshold of 2 or more one stolen admin phone
  cannot take over.
  Genesis defaults to threshold 1, where one admin key's approval is all any
  change needs (an add also needs that confirm) — removing every other admin
  included, once the 24 hours pass — and nothing announces the pending change
  beyond `roster pending` (the notice to every admin is not built). With
  exactly two admins, threshold 2 leaves no way to remove a lost admin key
  (removal needs two current admins), so threshold 2 wants at least three
  admins. Every epoch bump discards outstanding challenges.
- **No guest passes in v0.** A guest pass's use log is attendance tracking of a
  cleaner, caregiver or dog walker; if ever added, no per-use record.
- **Never on a chain** (§9). Nothing anchors the roster today: the RFC 3161
  run takes only its four ledger heads (`src/anchor_policy.rs`), and the roster
  is not in witnessd's database. A fifth subject would be a code change that
  alters the per-run request pattern once.

### 6.7 Door locks and Aliro: interop only, as a separate device class

CSA's **Aliro 1.0** (26 February 2026) is the standard phone-and-watch door
key (NFC tap, BLE, BLE + UWB hands-free; offline, asymmetric). It is right for
a *lock* to speak, but not the core of a no-Apple design: every wallet
provisioning path observed runs through a platform home app and hub (Apple
Home Key needs an Apple Matter hub; Samsung's Digital Home Key, announced
2 March 2026, comes with SmartThings onboarding; Google Wallet had not shipped
Aliro home keys as of the latest evidence). Locks stay outside the fleet (Free
Signals D; [`kernel/rf_presence_architecture.md`](../../kernel/rf_presence_architecture.md):
"No actuator interfaces (no door locks, no alarms)"). A household that owns an
Aliro lock keeps it; the project does not integrate it or ingest its identity
output.

### 6.8 The existing canary-wap household layer: drop

[`household.h`](../../firmware/projects/canary-wap/arduino/canary_wap/household.h)
stores up to eight bonded phones' IRKs to quiet their own arrivals, and
`presence_context.h` flips HOME/AWAY from owner sightings. The July 2026 audit
flags the IRK store as HIGH-severity privacy debt against Free Signals
Invariants A and F, notes that enrollment is unwired, and asks for a decision
record ([`esp32s3_firmware_full_audit_2026-07.md`](../audit/esp32s3_firmware_full_audit_2026-07.md)).
The [decision record](../design/household_presence_and_posture.md) answers
**drop** — above all, the IRK store lets a passive inference change event
generation (§6.2) — with the firmware removal, including a purge of the
plaintext `hh_slots` and `hh_count` NVS keys, as follow-up. It keeps
`familiar` only as RAM-only, notification-only novelty, and retires the
household / non-household tier split in
[`apple_watch_and_notifications.md`](../design/apple_watch_and_notifications.md).
Until then, `household.h`'s claim that IRKs are "flash-encrypted at rest on
ESP32 with secure boot" is wrong: flash encryption does not cover NVS, NVS
encryption cannot be built under the Arduino 2.0.17 core, and secure boot is
opt-in, so the IRKs sit in plaintext
([`key_at_rest.h`](../../firmware/common/identity/key_at_rest.h)).

### 6.9 Bill of materials (October 2026 snapshots)

| Household (4 people, 2 doors) | Parts | Approx. cost |
|---|---|---|
| Phones only | 2 key satellites (XIAO ESP32-C3 + M5Stack ST25R3916 unit, ~$12 each); the phones they already own | ~$25 |
| Phones + cards for kids and members without phones | + NTAG 424 DNA stickers or cards ($0.45–5 each) | ~$30–45 |
| + FIDO2 keys for people without phones | + $15–29 each (possession; a fingerprint key for intent costs more) | ~$45–75 |
| For comparison | A commercial Aliro keypad/lock: $179–290 per door, plus a platform hub | |

No subscription, no Apple ID or Google account, and the same features in
Illinois, Texas, Portland and Quebec.

## 7. What a household gets — and gives up

| What "familiar faces" is bought for | With household keys | Coverage |
|---|---|---|
| Stop alerting me about my own family moving around the house | Home, set by an intent credential (a phone or watch key): indoor witnesses drop to tamper-only until it lapses (12 h by default) | **Partial** — the indoor noise goes; your own arrival is still one ordinary door `Person` |
| Arm when we leave, disarm when we're back, without a geofence | Away / Home / Night by a signed act; the apps hold no location permission | **Full** once a witness honors the household posture (the hub side is built; no Canary does yet) |
| Tell me when someone who isn't us is at the door | Every `Person` at the door is reported exactly as today — a notice to phones on the home network; off the LAN nothing, because the relay carries no `Person` and this design adds none (§6.4 rule 6) | **Same as today** — nothing here tells "not us" apart, and the system never says "stranger" or "unknown": a Canary "does not accuse," and a forgotten card would make that word a false alarm about your own child |
| Kids without phones | A sticker or card, or a one-button fob — possession: Away or Night, never Home | **Full** — stickers and cards have no battery or radio, so cannot become locators; a fob (an nRF52840) stays silent until pressed |
| Remove someone (a lost phone, a roommate moving out) | Revoke the key; the roster moves to a new epoch. LAN pairings are separate from the roster (tokens are not linked to members), so a paired device keeps reading the posture — never changing it, though its rejected responses can still start a 60-second cooldown that refuses Home — until `household_checker pair --reset` unpairs every client and the household pairs again the ones it keeps | **Full, and better** — a key can be taken back; a face cannot |
| Packages, people, vehicles, animals, zones | Already the witness vocabulary | **Full**, unchanged |
| Name the person at the door ("Alex is home") | — | **Refused.** A key is not a person (a borrowed card, a child with a parent's fob), and a name in a notification is an arrival diary on every recipient's phone |
| Recognize grandparents, the caregiver, the cleaner | They are a `Person`, like anyone | **Refused** — naming someone who carries nothing needs a template of everyone who walks up |
| Per-person alerts ("tell me when Alex arrives") | — | **Refused** — the basic stalkerware tool, and the "where is my kid" locator the Tin Can's refusal list rules out ([`canary_tincan_kids_watch.md`](../design/canary_tincan_kids_watch.md)) |
| "Who's home" list, search by person, AI descriptions that name people | — | **Refused** (Invariant VII; non-negotiable 1) |
| Unlock the door on arrival | A lock of the household's choosing, outside the fleet (§6.7) | **Refused** on Canaries |

Beyond the rows above, the household gives up two things: doors without a
satellite get nothing, and in a one- or two-person home anyone can infer who
set the posture — the system itself never knows a name. What it gets: $0 a
year, no account, keys it can take back, no biometric from anyone, and no
vendor cloud to breach.

## 8. Cryptographic best practices — the checklist this design is held to

**Identity**

1. **Identity is a key, not a face.** A member's credential is a private key in
   hardware they hold. Readers store no member material, and the hub stores
   only **public keys** for every asymmetric credential, so extracting a reader
   yields nothing that impersonates anyone — the property a face template can
   never have. AES cards are the exception, said out loud: checking one needs
   that card's AES key on the hub, which is clone-capable.
   Card keys live only in the checker's private data, sealed under a key kept
   outside it (keeping that key out of any backup of the data, yet backed up
   on its own, is the operator's job: losing it loses the household), and a
   hub compromise means re-keying every card. They are not the hub's only
   secret: its posture signing key, sealed beside them, alone decides whether
   indoor witnesses sense — whoever holds it can sign Home without any intent
   credential — so a hub compromise also means a new posture key and
   re-pairing every witness.
2. **Any biometric is local user verification on the member's own device**
   (Face ID, Android Class 3, a fingerprint FIDO key) unlocking the key — the
   FIDO/WebAuthn architecture, where the relying party sees a signature and
   never the biometric. That is why "without Apple devices" costs nothing: an
   Android phone or a fingerprint FIDO key fills the same slot, and a plain FIDO
   key or a sticker works with no biometric at all — as possession, which may
   raise sensing and never lower it.
3. **Two curves, on purpose** (§6.5): credentials are P-256 (ECDSA, checked on
   the hub with a vetted library); what the household *signs as the household*
   — roster, posture statement, policy rows, receipts — stays Ed25519, the repo
   standard ([`spec/beacon_channel_v0.md`](../../spec/beacon_channel_v0.md),
   [`src/crypto/signatures.rs`](../../src/crypto/signatures.rs)). Never reuse a
   key across algorithms or purposes — above all never feed an Ed25519 key to
   X25519, the Opera pairing bug F33 fixed
   ([`spec/canary_mesh_network_v0.md`](../../spec/canary_mesh_network_v0.md)
   §5.3) — and carry the algorithm in the record.

**Protocols**

4. **A fresh nonce stops replay, not relay.** NCC Group relayed BLE proximity
   unlocks (a Tesla Model 3, Kwikset Kevo locks) adding about 8 ms. Authority
   needs a deliberate centimeter-range act or secure ranging, and UWB is not a
   proof either: Ghost Peak cut a measured 12 m to 0 m with up to ~4% success
   per attempt on $65 of hardware. Write "relay-resistant," never
   "relay-proof" — and since a relay defeats possession, possession only
   raises sensing.
5. **Cards answer a live challenge, not a counter.** An NTAG 424 DNA Secure
   Unique NFC (SUN) tap message (an AES-CMAC over the UID and a read counter)
   is replay-safe only if the checker keeps each card's last counter — a
   per-card usage tally §6.4 rule 4 forbids. So cards are used only through
   live mutual authentication (`AuthenticateEV2First`) with reader random
   RndA = SHA-256(challenge)[0..16], which binds nonce, door, epoch and purpose
   with no stored counter; SUN messages never carry authority.
6. **Membership is a signed set with an epoch** (§6.6).
7. **Pairing borrows HAP's shape**: a short-code PAKE (password-authenticated
   key exchange: SRP-6a as HAP does, or SPAKE2+ as Matter does) with an
   attempt cap and both long-term keys bound in, then Station-to-Station
   sessions over X25519 → HKDF → an AEAD (authenticated encryption). The
   repo's Opera pairing (an X25519 exchange confirmed by a 6-digit code) is not
   this shape and must not be copied: a comparison code is not a PAKE, it
   commits to neither side's ephemeral, so a relay can grind a matching code in
   minutes, it binds neither long-term key, and only the canary-wap tree HKDFs
   the X25519 output
   ([`spec/canary_mesh_network_v0.md`](../../spec/canary_mesh_network_v0.md)
   §5.3, §11.1 — open, awaiting crypto review).

**Implementation hygiene (repo-wide)**

8. Vetted libraries only (libsodium or Monocypher on MCUs, ESP-IDF v6's PSA
   Crypto, RustCrypto on the hub); no AES-GCM with random nonces under a
   long-lived key; ESP32 nonces only after RF is up and first-boot keys inside
   `bootloader_random_enable()`/`disable()`; RSA-3072 Secure Boot v2 over the
   ROM ECDSA path (AR2026-006), flash encryption and per-device keys —
   AR2026-005's recovery (the S3's general-purpose AES accelerator, not its
   flash-encryption block; rated low) is per-device
   ([`hardware_root_of_trust.md`](../design/hardware_root_of_trust.md), opt-in
   today); constant-time comparison, zeroized keys, fail closed.

**Evidence at rest**

9. **Per-object random keys, sealed to one holder.** HKSV's per-clip key is
   already the repo's sealed snapshot (one frame sealed to one X25519 key the
   device cannot open; [`sealed_snapshot_vault.md`](../sealed_snapshot_vault.md)),
   and it stays single-recipient by construction ([`GLOSSARY.md`](../GLOSSARY.md)).
   HKSV's other half — any household device opens any clip — is not to be
   copied: any one member could open a raw frame alone. Multi-recipient
   sealing, if ever proposed, is threshold (k-of-n) opening through that doc's
   maintainer sign-off, padded to a fixed number of wraps so the file size
   hides the household's size, with the Canary accepting a recipient set only
   on m-of-n admin signatures it checks itself — never on the hub's word,
   never from the household-key roster. (MLS does not fit: it has no group
   public key for a non-member Canary to encrypt to.)
10. **Hybrid post-quantum for anything kept for years** (harvest now, decrypt
    later): an X25519 + ML-KEM-768 (FIPS 203) hybrid for long-retained evidence
    envelopes — Apple PQ3 and Signal moved key exchange first. X-Wing is still
    an Internet-Draft, so pin the version; signatures are less urgent.
11. **Block egress at the network — a router policy the owner configures (a
    `won't`, not a `can't`).** HomeKit's "Restrict to Home" (§2) for every
    camera in the house, ours or not: no internet route, the hub the only
    thing it can reach.

## 9. Ethereum and smart contracts — evaluated, not adopted

The question was whether a public chain improves anything here. The three jobs
a household could give one are (a) proving *when* the witness log existed, (b)
an auditable break-glass quorum, and (c) transparent revocation of household
keys. **Ethereum does none of them better than what the repo already has or
has designed, and it costs privacy on all three.**

- **(a) Time anchoring.** The repo ships RFC 3161 anchoring of four ledger
  heads at two time-stamping authorities (TSAs) with a constant per-run request
  count ([`docs/timestamping.md`](../timestamping.md)), and specifies
  **OpenTimestamps** (OTS) as a second leg, not yet built
  ([`spec/quorum_unseal_v2.md`](../../spec/quorum_unseal_v2.md) §4): free, no
  wallet, and a completed proof (a few hours after submission) rests only on
  SHA-256 and Bitcoin headers — checked against your own pruned Bitcoin node,
  or by trusting a block explorer. An Ethereum L1 anchor is cheap — one
  ~22,280-gas calldata anchor a day (our arithmetic from EIP-7623's floor) is
  roughly $1.50–$11 a year at 0.07–0.54 gwei (an October 2026 spot reading and
  an August average) and ETH ≈ $2,545 — but pins time to the 12-second slot: a
  daily anchor publishes a 365-point activity series, and an off-schedule one
  after an incident is visible to anyone, the opposite of what Invariant III
  asks for. Funding the address almost always means an identity-checked (KYC)
  exchange withdrawal that address clustering links. L2s cost about $0.0015 a
  transaction (the Q1 2026 average across Arbitrum, Base and Optimism) but add
  Security Council upgrade powers and a centralized sequencer to the trust
  base.
- **(b) Break-glass quorum.** A Safe n-of-m would publish the trustees'
  addresses, the threshold, who approved and a 12-second-precise timestamp of
  every unseal, forever. The repo's quorum is auditable without that: signed
  approvals, a receipt chain, `break_glass_receipt_head` anchoring
  ([`src/break_glass/`](../../src/break_glass/)). Fusaka's P-256 precompile
  (EIP-7951, 6,900 gas, live since 2025-12-03) makes passkey signers cheap
  *on-chain*; they are free to check *off-chain*, where this design checks
  them.
- **(c) Revocation transparency.** `policy_head` already anchors the
  break-glass policy history; OTS and the planned C2SP tlog checkpoints with
  fleet cross-witnessing would make it rollback-detecting for anyone the
  household chooses to show. The roster is not in that history (§6.6);
  anchoring it would be one more subject on the same schedule — never an
  Ethereum Attestation Service (EAS) ledger, where a "revocation" deletes
  nothing and broadcasts a roommate leaving or a divorce.

**Never on any chain, at any tier:** a face or voice embedding or template, or
any hash, commitment or "hash-equivalent" of one; names or contact details;
device identifiers, MACs or public keys; the household roster or trustee
identities; event contents, types, counts or per-event hashes (low-entropy,
so guessable). The EDPB's blockchain guidelines (02/2025, adopted July 2026)
say hashed or encrypted personal data can still be personal data. **World
(formerly Worldcoin)** says it stores only a "hash-equivalent" of the iris (its
contracts README: "which happens on a blockchain") behind zero-knowledge
proofs, and still drew enforcement from 2023 to 2026 — Kenya, Spain and
Bavaria's BayLDA among at least ten jurisdictions. The liability sits at
capture; no chain construction moves it.

**If an Ethereum leg is ever offered** (an opt-in enterprise anchor, never a
default): one per-run Merkle root over all ledger heads, blinded with a fresh
nonce kept off-chain; calldata- or event-only transactions, or the audited EAS
predeploy, never a new stateful contract; a fixed schedule with a constant
count; an address funded without linking it to the household. Any contract the
project deploys is immutable (no proxy, no admin: OWASP's Smart Contract Top
10:2026 ranks access control first), holds no value and accepts only a
`bytes32`. The 2025 Bybit theft (about 401,000 ETH plus liquid-staking tokens,
~$1.46B, attributed by the FBI to DPRK) broke no contract: a compromised
Safe{Wallet} front end fed hardware-wallet signers a disguised `delegatecall`
they blind-signed. Key, signer and infrastructure compromise dominate losses
since — the risk a household would take on.

## 10. What this research does NOT green-light

Beyond the verdicts in §12 and a key's limits in §6.3, three plausible-sounding
requests are refused here:

- **Soft biometrics as a substitute** — gait, body shape, clothing, height,
  re-identification across cameras. [`strategy/14`](../strategy/14-pose-estimation-v2-ai.md)
  already refuses skeletons as an identity substrate; Google's 2026 "clothing
  and body signals" matching is exactly the thing.
- **Static identifiers as credentials** — MIFARE Classic, 125 kHz prox, static
  iBeacons (including the Home Assistant Android app's BLE Transmitter). All
  cloneable.
- **Cloud relays in the credential path** — passkey "hybrid" flows tunnel
  through a third-party relay; an Apple or Google home hub as a requirement.

## 11. The strongest case for a face, and why it still fails

A refusal is only fair if it beats the best version of the idea: a dedicated
**face key**, never a Canary and never fed by Canary frames — powered only by a
button, with near-field optics and an IR + depth liveness gate (a K230
structured-light pipeline, or IR flood + global-shutter IR + ToF) limiting it
to one face at 0.3–1 m, **a possession credential presented first** so the
match is a true 1:1, the template wrapped by a per-device key and matched in
RAM, attempt limits, and one signed household command out, with no name. It is
designable, and should still not be built:

**The gating dilemma.** The only non-biometric way to know a person is
enrolled *before* looking at them is a key. Gated behind one, the face is
mostly redundant; ungated, the device scans every courier. Even gated, it
computes the faces of those least likely to have consented — whoever holds a
stolen or borrowed card — and the gate is a firmware `won't` one malicious
update removes.

**What the face adds over a PIN.** At ESP-DL's published operating point
(FAR 1e-4, §4), the zero-effort odds over five tries are about 5 in 10,000 — a
4-digit PIN's; a 6-digit PIN beats it about a hundredfold (our arithmetic from
the published figures), before NIST's 10–100× demographic differentials. And
the realistic household impostor is family — twins, look-alike siblings,
children under 13 — exactly where even Face ID's false-match rate rises (§2).

**Everything else** is §1, §4 and §5 applied to a face key, each enough alone:
a face cannot be revoked (re-enrolling leaks it again — IronMask, 2025), its
template comes out of ESP32-class silicon in days, no open stack or consumer
lock has certified liveness, the usable weights are non-commercial or of
contested provenance, and BIPA reaches "any individual." Its known parts cost
~$58–85 a door before depth hardware, PAD testing, model rights or legal work,
*plus* the card it needs anyway — against ~$12 for a satellite.

**Verdict:** refused, with no automatic revisit trigger. Use the Face ID the
household already owns, on its own phone (or a fingerprint FIDO key), to
unlock a key; the door checks only a signature (§6). Everything that made the
face key safe came from its possession half.

**If the owner still chooses it**, it is two RFCs, not one: "introducing
fields that enable identity" is on governance §2.3's **prohibited** list, which
"MUST be rejected," and §3.2's RFC path covers only *potentially weakening*
changes ([`governance_and_invariants.md`](../governance_and_invariants.md)).
First amend governance to create a narrow category (say, "self-verification
devices outside the witness system"); then a face-key RFC that at minimum
rewords every affected text (Invariants II and I, conformance, AGENTS
non-negotiable 1, `CONTRIBUTING.md`, `SECURITY.md`) and squares it with voice
rule 5; keeps Free Signals D and F untouched; carries a threat model (spoofs,
look-alike family, demographic differentials, extraction and reflash,
coercion, covert enrollment of a partner, the borrowed-card bystander); shows
§6 failing a named need after a wristband or ring credential was tried;
enforces structurally (the button power gate, credential-first matching,
wrapped templates, a secure element, secure boot and flash encryption before
any enrollment, no link into `witnessd` or a Canary); ships sentinel tests
like [`tests/frigate_integration.rs`](../../tests/frigate_integration.rs);
publishes false-accept and false-reject rates with demographic breakdowns (the
FTC 2023 statement; *Rite Aid*), an accredited ISO/IEC 30107-3 Level 2 PAD
result and latency benchmarks before any claim, and never says "Face ID-like,"
"secure" or "verified"; documents rights to the weights; carries the legal
pack (written releases with guardians for minors, a public retention policy,
a non-biometric alternative, the admission that it cannot geofence); replaces
the shipped "it can't" copy (FAQ, glossary, store page, website, the legal
audit's differentiator) in the same change; and sunsets itself, crypto-erasing
templates, on a PAD break, a weight-license change or a failed audit.

The canary-wap household decision (§6.8) asked a smaller version of the same
question — owner-paired identity, with a revocable key instead of a face — and
the answer was to drop it. A project that declined to store an owner's phone
key should not store an owner's face.

## 12. Verdicts

| Option | Verdict | Revisit trigger |
|---|---|---|
| Familiar faces on any camera — Canary or not, local or not, opt-in or not | **Refused, permanently** (Invariant II) | None |
| Ingesting another system's face or plate identity (Frigate `sub_label`, Double Take, CompreFace, ESP-WHO, UniFi) | **Refused** — already stripped in code | None |
| An ungated face door ("is this one of my household?") | **Refused** — a 1:few scanner of everyone who presses | None |
| A face key built by this project (§11) | **Refused** | Only the two RFCs in §11; no automatic trigger |
| Passive phone presence (BLE IRK) as a recognition or quieting signal | **Refused** (§6.2); "drop" for the canary-wap household layer (§6.8) | None — keeping it would be the audit's "carve-out for owner-paired devices," which needs a governance RFC, not a decision record |
| **Household keys as signed posture commands (§6)** | **Recommended**, design stage; the hub side is built and host-tested | A Canary acting on it waits for the posture/inbound RFC (§13 step 3) |
| Aliro / Matter door locks | Interop only, separate device class (§6.7) | Google Wallet shipping Aliro home keys *and* a provisioning path that needs no platform hub |
| Copying HKSV's custody model — per-object keys and egress blocking, *not* its any-member-opens decryption (snapshots stay single-recipient) | **Adopt** as practice (§8) | — |
| Ethereum anchoring, Safe quorums, on-chain revocation | **Declined**; build the specified OpenTimestamps leg instead (§9) | An opt-in enterprise anchor under §9's constraints, on a customer's written request |
| Matter 1.5 cameras | Transport only, video verdict unchanged | Apple's "semantic camera" trigger in `apple_home_integration.md` §2 |

## 13. Next steps, smallest first

1. ✅ This document, with two corrections: `apple_home_integration.md` §1.2
   (the HKSV publishing point, §2), and
   [`PROVENANCE_INTEROP.md`](../security/PROVENANCE_INTEROP.md) no longer calls
   Bybit's wallet a "3-of-3" multisig (its on-chain threshold was three
   signatures; secondary analyses describe three of six owners).
2. ✅ **The audit's household decision record**,
   [`household_presence_and_posture.md`](../design/household_presence_and_posture.md)
   (Proposed). Its firmware removal and NVS purge are follow-up work.
3. **The posture/inbound RFC** (decisions #3 and #4) before any Canary acts on
   a household posture — what it must settle is in the decision record.
4. ✅ (hub side) **The contract and a host-tested hub core** (with sentinel
   tests that plant a marker handle and prove it reaches no output). **Still to
   build:** the key-satellite firmware and its device-class page, the phone and
   watch apps, the relay wiring.
5. **The OpenTimestamps leg** already specified in `quorum_unseal_v2.md` §4.
6. A **FAQ entry** ("Can it tell my family from strangers?") once a household
   can use step 4, and a line in the Tin Can's refusal list — the kids' watch
   is not a door key (no NFC; a door credential is a household secret; "is my
   kid home" is the locator role it refuses).
7. Nothing in the kernel. The kernel's answer to "who" remains: it cannot say.

---

## Sources (external, read 2026-10-13)

Where a primary host refused direct fetches from the research sandbox, the
claim rests on search excerpts of that page and is marked *(excerpt)*.

- **Apple:** [Platform Security — camera security / HKSV](https://support.apple.com/guide/security/camera-security-sec525461d19/web) (May 2024 PDF edition read in full; current page by excerpt) ·
  [Face ID, Touch ID, passcodes](https://support.apple.com/guide/security/optic-face-touch-passcodes-passwords-sec9479035f1/web) *(excerpt)* ·
  [iCloud HomeKit Secure Video plans](https://support.apple.com/guide/icloud/mme054c72692) *(excerpt)* ·
  [HKSV Open Source Compatibility Guide (2026-06-03)](https://developer.apple.com/download/files/HomeKit-Secure-Video-Open-Source-Compatibility-Guide.pdf) ·
  [HomeKit ADK (archived 2025-10-28)](https://github.com/apple/HomeKitADK)
- **Products:** [Frigate face recognition docs](https://github.com/blakeblackshear/frigate/blob/dev/docs/docs/configuration/face_recognition.md) ·
  [Ring Familiar Faces](https://ring.com/support/articles/z3yhg/familiar-faces) *(excerpt)* ·
  [Google familiar faces](https://support.google.com/googlehome/answer/9268625) ·
  [Sen. Markey, Dec 2025](https://www.markey.senate.gov/news/press-releases/just-in-time-for-holiday-deliveries-sen-markeys-probe-exposes-amazons-alarming-privacy-violations-with-facial-recognition-technology-in-ring-doorbell) ·
  *Sigwalt v. Amazon.com, Inc. and Ring LLC*, No. 2:26-cv-01887 (W.D. Wash., filed 1 June 2026; CourtListener RECAP docket `gov.uscourts.wawd.363574`) *(docket listing and press reports)* ·
  [Reolink on face recognition](https://reolink.com/blog/facial-recognition-security-camera-buying-guide/) *(excerpt)* ·
  [Matter 1.5](https://csa-iot.org/newsroom/matter-1-5-introduces-cameras-closures-and-enhanced-energy-management-capabilities/) *(excerpt)*
- **Hardware and models:** [ESP-DL face recognition](https://github.com/espressif/esp-dl/blob/master/models/human_face_recognition/README.md) ·
  [K230 DPU API](https://github.com/kendryte/k230_docs/blob/main/en/01_software/board/mpp/K230_DPU_API_Reference.md) ·
  [OpenCV zoo SFace](https://github.com/opencv/opencv_zoo/tree/main/models/face_recognition_sface) ·
  [InsightFace license](https://github.com/deepinsight/insightface#license) ·
  [google-coral/edgetpu (archived 2026-04-19)](https://github.com/google-coral/edgetpu) ·
  [NIST IR 8280](https://nvlpubs.nist.gov/nistpubs/ir/2019/NIST.IR.8280.pdf) ·
  [Silent-Face-Anti-Spoofing](https://github.com/minivision-ai/Silent-Face-Anti-Spoofing)
- **Template protection:** [Keller, Osadchy, Dunkelman (arXiv 2012.13293)](https://arxiv.org/abs/2012.13293) *(excerpt)* ·
  [Otroshi Shahreza et al., CVPR 2025 Workshops](https://openaccess.thecvf.com/content/CVPR2025W/ABAW/papers/Shahreza_Face_Reconstruction_from_Face_Embeddings_using_Adapter_to_a_Face_CVPRW_2025_paper.pdf) *(excerpt)* ·
  [IronMask multi-enrollment attack (PoPETs 2025)](https://www.petsymposium.org/popets/2025/popets-2025-0154.php) ·
  [FIDO: how FIDO works](https://fidoalliance.org/how-fido-works)
- **Credentials and radios:** [Aliro 1.0](https://csa-iot.org/newsroom/introducing-aliro-1-0-a-unified-standard-to-transform-the-access-control-ecosystem/) *(excerpt)* ·
  [Samsung Digital Home Key](https://news.samsung.com/us/samsung-wallet-launches-digital-home-key-smart-door-locks) *(excerpt)* ·
  [NCC Group BLE relay advisory](https://www.nccgroup.com/research/technical-advisory-ble-proximity-authentication-vulnerable-to-relay-attacks/) *(excerpt)* ·
  [Ghost Peak, USENIX Security 2022](https://www.usenix.org/conference/usenixsecurity22/presentation/leu) *(excerpt)* ·
  [NTAG 424 DNA datasheet](https://www.nxp.com/docs/en/data-sheet/NT4H2421Gx.pdf) ·
  [Home Assistant Private BLE Device](https://www.home-assistant.io/integrations/private_ble_device/)
- **Espressif security:** [advisories index (AR2023-007, AR2026-005, AR2026-006)](https://www.espressif.com/en/support/documents/advisories) *(excerpt)* ·
  [ESP-IDF random number generation](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/system/random.html)
- **Cryptography:** [RFC 9420 (MLS)](https://www.rfc-editor.org/rfc/rfc9420) ·
  [FIPS 203](https://csrc.nist.gov/pubs/fips/203/final) · [X-Wing draft](https://datatracker.ietf.org/doc/draft-connolly-cfrg-xwing-kem/)
- **Ethereum:** [EIP-7607 (Fusaka)](https://eips.ethereum.org/EIPS/eip-7607) · [EIP-7951](https://eips.ethereum.org/EIPS/eip-7951) ·
  [EIP-7773 (Glamsterdam)](https://eips.ethereum.org/EIPS/eip-7773) · [EAS contracts](https://github.com/ethereum-attestation-service/eas-contracts) ·
  [OpenTimestamps client](https://github.com/opentimestamps/opentimestamps-client) ·
  [OWASP Smart Contract Top 10](https://github.com/OWASP/www-project-smart-contract-top-10) ·
  [FBI PSA on Bybit](https://www.ic3.gov/PSA/2025/PSA250226) · [Bybit post-mortem (Cyfrin)](https://www.cyfrin.io/blog/safe-wallet-hack-bybit-exploit) ·
  [EDPB Guidelines 02/2025 on blockchain](https://www.edpb.europa.eu/documents/guideline/guidelines-022025-on-processing-of-personal-data-through-blockchain_en) *(v2.0 read from a mirror)* ·
  [BayLDA order on World](https://www.lda.bayern.de/media/pm/pm2024_08.pdf)
- **Law:** [740 ILCS 14 (BIPA)](https://www.ilga.gov/legislation/ilcs/ilcs3.asp?ActID=3004) *(excerpt)* ·
  [P.A. 103-0769](https://ilga.gov/legislation/publicacts/103/103-0769.htm) *(excerpt)* ·
  [EU AI Act](https://eur-lex.europa.eu/eli/reg/2024/1689/oj) and [Regulation (EU) 2026/1744](https://eur-lex.europa.eu/eli/reg/2026/1744/oj) *(read from mirrors)* ·
  [EDPB Guidelines 3/2019 on video devices](https://www.edpb.europa.eu/sites/default/files/files/file1/edpb_guidelines_201903_video_devices_en.pdf) ·
  [CJEU C-212/13 Ryneš](https://curia.europa.eu/juris/liste.jsf?num=C-212/13) ·
  [FTC Rite Aid order](https://www.ftc.gov/news-events/news/press-releases/2023/12/rite-aid-banned-using-ai-facial-recognition-after-ftc-says-retailer-deployed-technology-without) ·
  [FTC biometric policy statement](https://www.ftc.gov/legal-library/browse/policy-statement-federal-trade-commission-biometric-information-section-5-federal-trade-commission)

*Nothing in this document is legal advice.*
