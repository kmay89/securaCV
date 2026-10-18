# Research — Face ID without a face at the camera: knowing who's home on cheap hardware, with no Apple device and no third party (2026-10)

Can a household get what HomeKit Secure Video's face recognition sells —
"Mom is at the front door," no alert when the family comes home, a real alert
when a stranger does — on cheap hardware, with no Apple device in the house
and nothing leaving it? **Partly — and only by recognizing a key, not a
face.** The camera keeps witnessing *what* happened (`Person` at the front
door) and never learns *who*. A household member presents a key they carry —
a phone, a watch, an NFC card or sticker that costs a dollar or less, a fob —
and that signed act, not the camera, is what the system acts on: it sets the
home's posture (Home, Away, Night), which is where "don't alert me about my
own family" honestly comes from. A key proves possession, not identity (a
borrowed card reads the same), so the system never names anyone: "Mom is at
the front door" is the part this design refuses. If a biometric is wanted in
the loop, it is the Face ID or fingerprint sensor already on the person's own
phone, unlocking that key on that phone. That is how Face ID itself works: the
face unlocks a key inside a secure element, and the other side only ever sees
a signature.

The second half of the answer is the line. A camera that recognizes faces has
to compute a face template for **everyone** who walks into frame — the courier,
the neighbor's kid, the person on the sidewalk — in order to decide that they
are not Mom. That is world → system identification. It is what Invariant II
forbids, what the voice contract already refused for voices, what the 2026
bystander class actions against Ring and Google are about, and — the finding
that settles it for cheap hardware — something a sub-$60 board cannot do to
Face ID's standard anyway.

> **Scope note (this is research and a design, not a built feature).** Nothing
> here ships anything or changes any code. The deliverables are: the line,
> drawn once (§1); what Apple, the cloud vendors and the open-source NVRs
> actually do, with numbers (§2–§5); a buildable design for household
> recognition by credential (§6–§7); a cryptographic best-practice checklist
> for it (§8); the Ethereum verdict (§9); the "never" list (§10); and the
> strongest honest case for a face-based alternative, with what a governance
> RFC for it would have to contain (§11).
>
> **Method.** A research sweep on 2026-10-13 across Apple's Platform Security
> guide and the June 2026 HKSV Open Source Compatibility Guide, the CSA Aliro
> and Matter releases, Espressif/OpenCV/InsightFace/Frigate sources, the
> template-protection and inversion literature, Ethereum EIPs and incident
> reports, and the biometric statutes and regulator guidance named below. Repo
> claims cite the file that proves them. Several primary sites (support.apple.com,
> ring.com, eur-lex, ilga.gov) refused direct fetches from the research sandbox;
> where a fact rests on a search excerpt of the primary page rather than a full
> read, the source line says so. Prices are October 2026 snapshots and moved a
> lot this year (DRAM-driven increases hit every SBC tier).

---

## 1. The invariant question comes first

Three different things get called "Face ID for the house." They are not the
same request, and the constitution treats them differently:

| Direction | What it is | Example | Verdict |
|---|---|---|---|
| **World → system** | The camera identifies whoever walks into view (1:N) | HKSV Face Recognition, Ring Familiar Faces, Nest familiar faces, Frigate face recognition | **Forbidden. Invariant II, forever.** No revisit trigger exists, and none is proposed here. |
| **Owner → system, by biometric on our hardware** | A device of ours enrolls a household member's face and verifies it 1:1 when they present it | A "face key" door verifier — Face ID rebuilt on an ESP32 | **Not allowed.** It would need a governance RFC (§11), and the voice contract's rule 5 is precedent *against* it. |
| **Owner → system, by possession** | A household member proves membership with a key they carry; any biometric happens on *their own* device | Phone passkey, NFC card, fob, watch — Face ID on the phone gating the key | **Allowed, inside the contract in §6.4.** This is the recommendation. |

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
  [FAQ](../FAQ.md#does-it-do-face-recognition) ("No — and it can't"), and the
  two earlier verdicts: HKSV familiar faces is "Refuse, permanently … Not
  deferred: absent" ([`apple_home_integration.md`](../design/apple_home_integration.md)
  §1.3), and the doorbell's Familiar Faces row lists its revisit trigger as
  "none" ([`canary_doorbell_research.md`](../hardware/canary_doorbell_research.md)).
- [`whisper_local_voice.md`](whisper_local_voice.md) §3.1 drew the only
  carve-out the project has — owner → system voice *commands* — and drew it by
  **hardware** ("which device can feed the pipeline at all"), not by
  recognizing who is speaking. Its rule 5 refuses "only respond to my voice"
  enrollment as speaker recognition, "regardless of how convenient it would
  be." The face version of that sentence is Face ID. So the voice precedent
  argues against a face key, not for it.
- [`docs/governance_and_invariants.md`](../governance_and_invariants.md) §3.2:
  any weakening change needs a formal RFC that, among other things, must
  "demonstrate why a strengthening alternative is not viable." §6 of this
  document *is* the strengthening alternative.

The code already acts on the line: Frigate's recognized names
(`sub_label`) and plate strings are parsed as absent when its events are read
in, and the tests plant marker values to prove none get through
([`src/transport/frigate.rs`](../../src/transport/frigate.rs),
[`tests/frigate_integration.rs`](../../tests/frigate_integration.rs)).

## 2. What Apple actually does — and what is worth copying

HomeKit Secure Video is two very different designs under one name.

**The custody model is genuinely good, and worth copying.** Per Apple Platform
Security (May 2024 edition, read in full; current web excerpts match):

- On motion, the camera sends the clip to the **home hub** over a local link
  under a per-session HKDF-SHA512 key. The hub decrypts and analyzes it
  **locally**.
- A clip worth keeping is encrypted with **AES-256-GCM under a fresh random
  AES-256 key**; the ciphertext goes to iCloud, and the key and metadata go to
  CloudKit under iCloud end-to-end encryption. Clip sessions can be split into
  sub-sections with their own keys. Apple stores ciphertext; the household's
  devices hold the keys.
- For live view off the LAN, the hub relays the encrypted stream **without
  decrypting it**.
- Each user's HomeKit identity is an **Ed25519** key pair; accessories pair with
  SRP-6a (3072-bit, an 8-digit code, refusal after 100 failed attempts) and
  every session is Station-to-Station with X25519 ephemerals, HKDF-SHA512 and
  ChaCha20-Poly1305. HomeKit routers add a per-accessory PSK and a "Restrict to
  Home" mode that blocks a camera's internet egress at the network layer.

**Face recognition is the opposite design.** The same guide says HomeKit
stores "information about each person, such as name, as well as images
representing that person's face" in CloudKit, sourced from the owner's Photos
(opt-in) **or "previously analyzed IP camera video"**, and the hub's analysis
session identifies faces in the incoming stream. It is 1:N identification of
anyone in view, with a synced gallery of real face images that grows from
camera footage of visitors. It requires iCloud+ (50 GB → 1 camera, 200 GB → 5,
2 TB and up → unlimited; 10 days of history that does not count against
storage) and a home hub, which under the new Home architecture can only be a
HomePod or an Apple TV.

**Face ID is the design to learn from.** Attention check; IR image plus depth
map from a randomized capture sequence and a device-specific dot pattern;
captures signed and sent to the Secure Enclave; the representation computed and
matched there and never exported or backed up; anti-spoofing networks; five
failures or 48 hours force the passcode; and a match **releases a key** (it
unwraps the Data Protection class keys) rather than returning a yes/no. Apple's
stated random-person false-match probability is 1 in 1,000,000 ("less
than," in its support article), up to 1 in 500,000 with two enrolled
appearances (Touch ID: 1 in 50,000), and higher for twins, look-alike siblings
and children under 13.

The lesson is structural, not about cameras: **the biometric never leaves the
person's own secure hardware, and what leaves is a key operation.** That is
what §6 builds — and the repo already does it once:
[`EnclaveCustody.swift`](../../ios/Sources/SecuraCV/Security/EnclaveCustody.swift)
wraps the sealed-snapshot key under a Secure Enclave P-256 key usable only
after Face ID, Touch ID or the passcode. Face ID there is an unlock gate on the
holder's phone, never an identity in a witness record.

**Two corrections this research makes to earlier docs:**

- Apple's **HKSV Open Source Compatibility Guide** (Developer Preview, 37 pages,
  dated 2026-06-03) exists as
  [`apple_home_integration.md`](../design/apple_home_integration.md) §1.2
  describes — but it never says the CMAF recording publishing point is on the
  home hub. The controller writes a publishing-point URL and server CA to the
  camera; implementers report the camera uploads to an Apple endpoint while the
  hub only provisions it. That doc's sentence is corrected in this change. The
  guide also grants no license, needs HAP R17 (distributed to MFi licensees),
  and still needs an Apple home hub — so it offers no path to "without Apple
  devices" either way. The HKSV verdict in that doc stands unchanged.
- Apple's own next step points the same way. Reporting in October 2026 says
  Apple's new home hub will recognize household members by face and voice from
  an ordinary camera to pick whose content to show, described as less reliable
  than Face ID and needing iPhone confirmation for sensitive actions — face as
  *personalization*, not authentication. (Rumor-grade until Apple publishes it;
  recorded only as context.)

## 3. What everyone else does — who processes where, and what leaks

| Product | Where faces are matched | What leaves the house | Notes |
|---|---|---|---|
| **Apple HKSV** | Home hub (HomePod/Apple TV) | Encrypted clips + an E2EE gallery of names and face images (iCloud) | Needs iCloud+ and an Apple hub |
| **Ring Familiar Faces** (Dec 2025) | Ring's cloud ("encrypted and stored in the cloud, not on your Ring devices") | Faces of everyone the camera sees; unnamed faces kept 30 days | Incompatible with Ring E2EE; Amazon says it is not offered in Illinois, Texas or Portland, OR |
| **Google Nest familiar faces** | Not stated by Google | Google Home Premium subscription | Off in Illinois "as a precaution"; three N.D. Cal. bystander suits filed Jun–Jul 2026 |
| **Eufy** (HomeBase 3 "BionicMind") | On the HomeBase | Marketed as local — in 2022 thumbnails and face data were found going to AWS with cloud storage off | NY AG settlement, $450,000 (Jan 2025) |
| **Frigate** 0.16+ (Aug 2025) | Your own server (YuNet detection; FaceNet on CPU or ArcFace on GPU/NPU) | Nothing for recognition itself — but GenAI descriptions can send frames (and the recognized name) to a cloud LLM if configured, and Frigate+ trains on uploaded images | Keeps up to 200 recent face crops **including unrecognized people**, and publishes every recognition attempt over MQTT |
| **UniFi Protect** | On-premises (camera or AI Port/Key) | Nothing by design | $210–$1,155 of accessories |
| **Reolink** | — | — | Says its cameras do not currently offer face recognition, citing privacy |

Three patterns matter for this project:

1. **"AI features" and end-to-end encryption are mutually exclusive in the
   cloud products.** Ring Familiar Faces cannot be used with Ring E2EE, and
   Ring's 2026 "TAKE" default exists so its cloud can still decrypt frames for
   up to 24 hours for AI features. The vendor sees the frames, or the feature
   does not work.
2. **Every product that sells familiar faces computes a template of every face
   in view**, including the ones it keeps as "unknown." Frigate does it
   locally, Ring does it in the cloud; both do it to people who never agreed.
3. **The documented third-party failures are about custody, not cryptography**:
   Ring's 2023 FTC case ($5.8M; employee access to private videos), Wyze's
   February 2024 cross-account thumbnail exposure (about 13,000 users), Eufy's
   2022 "local" uploads, and Ring's police pipelines (Axon Community Requests;
   the Flock Safety tie-up announced and then canceled in February 2026 before
   launch). The surest way not to leak a face is never to compute one.

**Matter 1.5** (November 2025; 1.5.1 in March 2026) standardized cameras —
WebRTC live view and CMAF clip upload over HTTPS — but defines no analytics and,
as far as this research could confirm, no end-to-end encryption for recordings.
As of October 2026 only SmartThings ships Matter camera support. It is a
transport to interoperate with later, not a recognition design; the
[`apple_home_integration.md`](../design/apple_home_integration.md) verdict on
video applies to it unchanged.

## 4. Can cheap hardware do Face ID? The honest numbers

Compute is not the problem. Every tier from a $7 Luckfox Pico (RV1103) up can
run a door-grade 1:1 face verifier in well under a second. What cheap hardware
cannot do is the part that makes Face ID Face ID: **liveness, accuracy at a
one-in-a-million false-accept rate, a key that physical access cannot extract,
and a model you are allowed to ship.**

| Tier (Oct 2026 price) | What it can honestly do | Accuracy published | Liveness (PAD) |
|---|---|---|---|
| ESP32-S3 (ESP32-S3-EYE kit, $47.50) — Espressif ESP-DL / ESP-WHO, MIT | RGB 1:1 in about 300 ms (`mfn_s8_v1` ≈ 247 ms model step) | 90.03% TAR at FAR 1e-4 on IJB-C (`mfn_s8_v1`) | None shipped. Assume a printed photo works. |
| ESP32-P4 (Function EV board, ~$55 at launch) | RGB 1:1 in ~85–190 ms | 93.94% TAR at 1e-4 (`mbf_s8_v1`) | None shipped |
| Luckfox Pico RV1103/RV1106 ($7–27), MaixCAM ($40–76) | RetinaFace + MobileFaceNet / InsightFace R50 demos | Model-dependent | None |
| Kendryte K230 (CanMV, ~$50) | The only sub-$60 SoC found whose SDK documents a hardware depth engine for "3D face recognition" (structured-light speckle) | — | Architecturally possible; projector/IR module cost not found; nothing certified |
| Raspberry Pi 5 + AI HAT+ (Hailo) — Pi 5 2 GB is $77.50 since 2026-10-01 | Massive headroom (arcface_mobilefacenet at 1,666 fps on Hailo-8L, LFW 99.4%) | Ships InsightFace-derived weights | None (RGB) |
| Commercial 3D face locks — SwitchBot Lock Vision ($170–230), Eufy E40 ($300), Lockly Visage ($349) | Structured-light/IR face unlock, templates kept on the lock | Vendor claims only (e.g., "below 0.0001%") | No public iBeta PAD letter found for any consumer lock |

What the table means:

- **Accuracy.** Apple claims under 1e-6 FAR. The best small open model above
  publishes about 90–94% true-accept *at 1e-4* — roughly a hundred times more
  false accepts at the operating point, while still rejecting about one honest
  attempt in ten. LFW scores (99.4–99.8% for every model above) cannot tell a
  door-grade verifier from a Face ID-grade one. NIST IR 8280 found 1:1 false
  positives 10–100× higher across demographic groups depending on the
  algorithm, so one global threshold gives unequal error to different family
  members. Multiply the per-attempt FAR by attempts and enrolled people and the
  exposure grows again.
- **Liveness.** No RGB-only open stack ships presentation-attack detection; the
  best open RGB anti-spoofing model (MiniFASNet, Apache-2.0) describes itself as
  "limited in robustness." iBeta Level 1/2 testing caps attack materials at $30
  and $300; dual-/IR-camera access terminals cleared Level 2 in 2026, consumer
  locks have no public letter, and a 2023 paper ("DepthFake") spoofed
  commercial depth-based liveness with a 2D photo.
- **The template is the face.** Inversion attacks rebuild faces from embeddings
  that pass *other* face-recognition systems: about 78% through a fuzzy
  commitment (Keller et al.), about 96% against ArcFace on LFW at FMR 1e-3
  (Otroshi Shahreza et al., CVPR 2025 workshops), 84–93% even against systems
  designed to resist inversion (DiffMI, 2025). Every cheap protection scheme has
  a published break for faces (fuzzy commitment, IronMask on re-enrollment,
  fuzzy extractors under the 2025 PIPE attack, BioHashing once its token leaks).
  Invariant I's "embeddings are raw media" is not caution; it is the literature.
- **The key comes out.** The ESP32 family's secure boot and flash encryption
  have a long record of practical physical attacks — CVE-2019-17391, Ledger
  Donjon's 2022 side-channel key recovery on ESP32-V3, CVE-2023-35818, the
  C3/C6 CPA+FI chain (AR2023-007), and in 2026 AR2026-005 (ESP32-S3 AES key
  recovery by voltage glitch) and AR2026-006 (ROM ECDSA secure boot accepting
  invalid signatures on several newer parts). A face template on such a device
  should be assumed extractable by anyone who holds the device for a few days —
  and unlike a key, **a face cannot be revoked.** (The repo's own hardware is
  weaker still by default: Secure Boot and flash encryption are opt-in, and NVS —
  where keys live — is plaintext at rest on every current build;
  [`key_at_rest.h`](../../firmware/common/identity/key_at_rest.h),
  [`hardware_root_of_trust.md`](../design/hardware_root_of_trust.md).)
- **Licensing.** Code is usually permissive (YuNet MIT, SFace Apache-2.0,
  EdgeFace BSD-3, ESP-DL MIT, dlib's model public domain); **weights** mostly
  are not. InsightFace states all its models are "for non-commercial research
  purposes only" (so are the Hailo and MaixPy defaults derived from them), most
  others trace to MS-Celeb-1M (withdrawn in 2019), WebFace260M, VGGFace2 or
  CASIA-WebFace, and ESP-DL does not disclose its training data. The leading
  anti-spoofing dataset (CelebA-Spoof) is non-commercial.
- **Distance.** All of the above is the *easy* case: a cooperative person at
  0.3–1 m. A doorbell or yard camera identifying people at 3–10 m, at an angle,
  at night under IR, on color-trained models, is a much weaker system (Frigate's
  own docs warn against IR and grayscale input) — and it is the world → system
  case §1 forbids regardless.

**Verdict for cheap hardware:** face *convenience* is buildable for under $60;
Face ID-class *security* is not, and no design in this price range should claim
it. The cheapest honest factor that clears "a stranger can't get in" is not a
better face model — it is a key.

## 5. The law draws the same line

The legal research converged on the same distinction §1 draws, which is a
useful sign that the line is in the right place:

- **EU AI Act.** Art. 3(36) defines *biometric verification* as automated
  **one-to-one** verification; Recital 17 and Annex III point 1(a) exclude
  verification used for "unlocking a device or having security access to
  premises" from remote biometric identification, which Annex III makes
  high-risk — and the Commission's own example of a high-risk use is
  identifying a burglary suspect from **a CCTV camera on private property.**
  Art. 2(10) exempts only a natural person's purely personal *deployer*
  obligations; a provider's obligations remain. The Digital Omnibus
  (Regulation (EU) 2026/1744) moved the Annex III date to 2 December 2027 — a
  delay, not a rewrite.
- **GDPR.** The household exemption fails once a home camera covers public
  space (CJEU *Ryneš*, C-212/13, 11 December 2014). EDPB Guidelines 3/2019's
  hotel example is the bystander problem in one sentence: recognizing enrolled
  VIPs is unlawful "unless all other guests monitored (in order to identify the
  VIPs) have consented." **Rejecting a face is still processing it.**
- **Illinois BIPA.** "Private entity" includes **"any individual"** (740 ILCS
  14/10), so a homeowner is covered on the statute's face; written release
  before collection; $1,000 per negligent and $5,000 per intentional or reckless
  violation, with a private right of action (the 2024 amendment, P.A. 103-0769,
  made repeated collection from one person a single violation). There is no
  verification or security exemption.
- **Texas CUBI** (commercial purpose; AG enforcement up to $25,000 per
  violation; the $1.4B Meta and $1.375B Google settlements), **Colorado**
  HB24-1130 (any amount of biometric data, since 1 July 2025), **Washington**,
  **California** (biometrics are sensitive personal information; risk
  assessments required from 2026), and the **FTC** (the 2023 Rite Aid order:
  five-year ban plus deletion of derived models; the May 2023 biometric policy
  statement, still listed as of October 2026, makes unsubstantiated accuracy
  claims a Section 5 problem).
- **The vendors' answer is a geofence.** Amazon has said Ring's Familiar Faces
  is not offered in Illinois, Texas or Portland, Oregon; Google and Wyze turn theirs off in
  Illinois; Arlo in Illinois, Texas and Quebec. A local-first, account-free,
  open-source system **cannot** geofence — it would need location telemetry it
  refuses to collect, and a fork can delete the check. For this project,
  "compliance by geography" is not an option; compliance has to come from the
  design. A design that never computes a template stays outside every
  biometric statute surveyed here — no biometric consent flow, no template
  retention schedule, no geofence — which is the point
  [`legal-audit-2026-07.md`](../legal-audit-2026-07.md) already makes ("keep
  that line bright; it's a genuine differentiator"). It does not lift the
  ordinary duties of a camera that sees public space (the *Ryneš* point
  above); those apply with or without faces.

## 6. The design: household keys — Face ID's architecture, with the face left on your own phone

### 6.1 The shape

```
 tap / hold at the door                               nothing here ever sees a face
   │
   ▼
 key satellite (new device class, never a Canary)  ── NFC field only, ~10 cm
   │  serves a fresh hub challenge over NFC
   ▼
 the person's credential:
   phone  ── its OWN Face ID / Class 3 fingerprint unlocks a P-256 key (optional)
   card   ── NTAG 424 DNA / DESFire EV3 answers with AES
   FIDO2 key, button fob
   │  signs { challenge ‖ door ‖ epoch }
   ▼
 hub checker (own process, no internet route)
   │  checks the signature against a roster of PUBLIC keys
   ▼
 one household COMMAND ── v0: set the home's posture (Home / Away / Night)
   │
   ▼
 posture changes what witnesses sense and how loudly the hub notifies — for everyone, equally
```

Canaries are untouched. A door Canary keeps reporting `Person` at the front
door in a coarse time window, exactly as today, whether or not a key was ever
used. The household reads both facts — "Person at front door" and "Posture:
Home, set at the front door" — and draws the obvious conclusion itself. That
is Free Signals Invariant D working as written: "Canary presents signals;
humans interpret meaning."

### 6.2 Why commands, and not "recognition by key"

The first draft of this design let a key presented at the door *reword* the
Canary's alert for that door ("a household key was used" instead of "Person at
front door") and let a nearby household phone quiet it. Adversarial review
killed both, and the reasons are worth recording:

- **It is the hard stop.** [`spec/canary_free_signals_v0.md`](../../spec/canary_free_signals_v0.md)
  §5 says a Canary "MUST NOT be used to … distinguish 'authorized' from
  'unauthorized'." Joining a credential to a Canary's `Person` event to decide
  how that event is worded uses the Canary's output to make exactly that
  split, wherever the credential reader lives. Invariant F adds "no 'known
  device' vs 'unknown device' distinction in event output."
- **Passive phone presence is not evidence of anything.** A phone's Bluetooth
  resolvable private address is 22 random bits plus a 24-bit hash keyed by the
  phone's identity resolving key (IRK), and resolution takes **no time input**
  — so one sniffed advertisement keeps resolving under that IRK indefinitely,
  not just for the ~15-minute rotation period. It can also be relayed live with
  about 8 ms added (NCC Group, 2022), and it carries through walls, so "a
  household phone is nearby" is true every time anyone is home. And the IRK
  itself is effectively a global identifier: a phone hands the same IRK to
  every peer it bonds with.
- **The repo already learned this lesson once.**
  [`HomePresence.swift`](../../ios/Sources/SecuraCV/Alerts/HomePresence.swift):
  "An inference meant to remove noise had quietly become a mute switch on the
  loudest alarm … a false 'away' is silence during a fire, a false 'home' is a
  notification you didn't need. We take the notification."

What survives is the part that was always sound: **a deliberate, signed act
by a household member, presented at a door, is a command** — the owner →
system direction, like a voice command but carrying a signature, which is why
it may do what voice may not (voice contract rule 4: voice never arms or
disarms anything, because "a voice command carries no signature").

### 6.3 What a key does, and what it never does

| A household key may | A household key may never |
|---|---|
| Set the home's **posture** (Home / Away / Night) — the vocabulary [`apple_home_integration.md`](../design/apple_home_integration.md) open decision #3 says must exist first, dictionary-first | Reword, demote, quiet, collapse or delay any Canary event or its notification |
| Act as **the inbound trust surface** decision #4 asks for: "who may write it … what it may touch (sensing posture only, never chain/Beacon/config) … whether it demands the physical-confirm idiom." A key tap *is* the physical-confirm idiom, and it needs no Apple Home | Touch the chain, Beacon, break-glass, retention or configuration |
| Drive "quiet when home" (indoor witnesses drop to tamper-only while Home) — the privacy-positive direction: less witnessing of the family at home | Mute tamper, integrity, offline or pattern pokes, or stop an outdoor witness |
| Be revoked in one step, by epoch | Name anyone, anywhere — no label, handle or per-person count in any output |
| | Unlock a door or actuate anything on a Canary (Free Signals D) |
| | Work passively, or from outside the home network |

### 6.4 The household-key contract (the load-bearing part)

Modeled on the voice contract in
[`whisper_local_voice.md`](whisper_local_voice.md) §3.1:

1. **The boundary is the hardware.** Credentials enter only through a **key
   satellite** (or the hub itself), never a Canary. Canary RF stays symmetric
   and identity-free. The satellite has no camera, no microphone and no
   sensor that witnesses.
2. **Presentation, never observation.** Every input is a deliberate act inside
   a ~10 cm NFC field (or a button press on a fob, or a confirm on a phone at
   the satellite). Nothing listens for passing phones. The satellite forwards
   nothing about a card until it has selected the household's application, so
   a bystander's transit or payment card is never read.
3. **Commands, not classification.** A key's only effect in v0 is a posture
   change, which applies to every event equally. It never joins to a witnessed
   event.
4. **Transient by default.** Challenges are single-use, live ≤ 10 s, in RAM.
   No per-credential counter, usage tally or arrival history exists anywhere;
   freshness comes from the hub's single-use nonce. The posture change itself
   is journaled like any user action (voice rule 2's precedent) — *without*
   the credential, the door or a precise time, using the event contract's
   coarse buckets. Whether even that is too much (a daily posture change is a
   coarse arrival diary) is an explicit open question for the posture RFC.
5. **No who, in any output.** Not in the journal, the alert ledger, digests,
   Watch snapshots, MQTT, Home Assistant entities or logs. Names exist only on
   admins' phones, for managing keys. The door gives the same neutral "read"
   feedback to every tap; acceptance is shown only on the presenter's own
   phone, so the street never learns who is "authorized."
6. **Fail toward today.** Jamming, a dead satellite or a missing key produce
   exactly today's behavior. Home posture never silences anything the relay
   carries, and adds no new remote poke class. (The relay's vocabulary is
   tamper/integrity/offline/pattern, and widening it "is a privacy decision,
   not a tweak" — [`src/relay/mod.rs`](../../src/relay/mod.rs).)
7. **Local only, forever.** No cloud, no Apple or Google service, no passkey
   "hybrid" tunnel, no URL on any tag (an iPhone background-NDEF universal link
   would put Apple's associated-domains CDN and a public hostname into the
   flow). Off the home network, a key does nothing.

### 6.5 Components

| Piece | What it is | Notes |
|---|---|---|
| **Key satellite** | XIAO ESP32-C3 (already a board in the repo) + ST25R3916 NFC front end, MCU inside the wall, antenna outside | A new device class with its own contract, as the voice satellite is. ESP-IDF, not the Arduino 2.0.17 core, so NVS encryption exists; secure by default (NVS encryption via the HMAC scheme, Secure Boot v2 RSA-3072, secure download mode), a tamper switch, a per-station Wi-Fi PSK. Stores no member material at all — only its own Ed25519 identity, the pinned hub key and a hub-signed roster epoch high-water mark. Its uplink runs on a fixed-size, padded metronome so a sniffer outside cannot time taps (Invariant III; the HomeKit pacer in [`src/bridge/homekit.rs`](../../src/bridge/homekit.rs) is the precedent). |
| **Hub checker** | A separate crate and process, not `witnessd`; no internet route | Holds the roster in the app's private data, excluded from Home Assistant backups, with secrets wrapped under a key outside backup scope. The crate split keeps kernel code from linking it; file isolation is a Unix-permission `won't`, not a `can't`. |
| **Phone keys** | Android Keystore/StrongBox P-256 gated by a Class 3 biometric; iOS Secure Enclave P-256 behind `.userPresence` | No Apple hub, no Apple Home. The phone runs a foreground NFC session: the satellite emulates an ISO 7816 tag, serves the challenge, and takes the signature back over NFC — so the challenge exists only in the RF field, works before the phone rejoins Wi-Fi, and cannot be stockpiled. Phone door keys always require user presence (no "express" keys: a stolen unlocked phone must not be a house key). |
| **Cards** | NTAG 424 DNA (AES-128, ~$0.45–1 per sticker, ~$2.50–5 per PVC card) or MIFARE DESFire EV3 (EAL5+, with a relay-resistant proximity check) | Random ID on; per-card keys diversified from a random value read after authentication, never from the UID; encrypted PICC data. Honest label: a card is *possession, not intent* — it answers any reader field — so offer shielding sleeves. |
| **FIDO2 keys** | Any CTAP2 NFC key ($15–29) | Register with user verification required (`credProtect` 3) so harvested credential IDs cannot silently probe a key elsewhere; freshness comes from the hub's nonce, and the signature counter is deliberately not stored (clone detection is traded away so no per-credential usage tally exists — say so). Disclose that the relying-party ID and credential IDs are not secrets. |
| **Fob** | nRF52840 with one button (~$10–12 DIY) | Challenge–response after a button press; the button is the deliberate act. |

**Two curves.** Member keys are P-256 (it is what phones' secure elements,
passkeys, FIDO2 and Aliro use); the household's own signatures — hub, roster,
satellite — stay Ed25519, the repo standard. Admin entries are
algorithm-tagged (`ed25519 | es256`, and `cli | enclave | fido2`), each with
its own verifier and explicit domain separation; one admin is one person,
decided at an enrollment ceremony, not by de-duplicating key bytes. (The
existing `QuorumPolicy` holds 32-byte Ed25519 keys only —
[`src/break_glass/core.rs`](../../src/break_glass/core.rs) — so it is a pattern
to follow, not a structure to reuse as-is.)

### 6.6 Protocol and roster, in brief

```
hub → satellite   CHALLENGE { n_h: 32 random bytes, door_id (random, not the zone name),
                              epoch, ttl 10 s }          — bound to that satellite's session
satellite → card/phone/key   over NFC: sign or MAC  "securacv-hk-v1\0" ‖ door_id ‖ epoch ‖ n_h ‖ purpose
satellite → hub   ASSERT { response, satellite signature }
hub               single-use, unexpired, session-bound; credential in roster at the CURRENT epoch;
                  signature valid (ES256 / AES-EV2 / FIDO assertion) → execute the command
hub → satellite   neutral "read" feedback; result to the presenter's own phone only
```

- **The roster** is a signed set of member public keys (and card key
  references) with a strictly increasing epoch. Satellites keep a hub-signed
  epoch high-water mark, so restoring an old hub backup cannot quietly revive
  a revoked key; a rollback forces admin re-approval.
- **Adding a member** takes m-of-n admins, with a physical confirm on the
  *inside* (the hub, or the satellite's indoor half). **Revoking a member** is
  cheap on purpose — any one admin, or the holder. **Changing the admin set or
  the threshold** takes m-of-n current admins, a cooldown and a notice to every
  admin, so one stolen admin phone cannot revoke the others and take over.
  Every epoch bump flushes outstanding challenges.
- **Guest passes are not in v0.** A guest pass's use log is attendance
  tracking of a cleaner, caregiver or dog walker. If they are ever added, they
  must not create any per-use record.
- **Never on a chain** (§9): the roster, membership changes and member keys
  stay in the house. The roster head can ride the existing RFC 3161 anchoring
  run at its constant request count.

### 6.7 Door locks and Aliro: interop only, as a separate device class

CSA's **Aliro 1.0** (26 February 2026) is the standard phone-and-watch door
key: NFC tap, BLE, and BLE + UWB hands-free, asymmetric cryptography, offline
operation. It is the right thing for a *lock* to speak — but not the core of a
no-Apple design, because every wallet provisioning path observed runs through
a platform home app and hub: Apple Home Key needs Apple Home and an Apple
Matter hub; Samsung Wallet's Digital Home Key (announced 2 March 2026) is added
during SmartThings/Matter onboarding; Google Wallet had not shipped Aliro home
keys as of the latest evidence (announced, not live). Espressif's `esp-aliro`
(July 2026) supports Aliro over NFC only so far, with the core shipped as a
compiled library under a custom license. Locks stay outside the fleet (Free
Signals D; [`kernel/rf_presence_architecture.md`](../../kernel/rf_presence_architecture.md):
"No actuator interfaces (no door locks, no alarms)"). A household that owns an
Aliro lock keeps it; the project does not integrate it or ingest its identity
output.

### 6.8 The existing canary-wap household layer: recommend "drop"

[`household.h`](../../firmware/projects/canary-wap/arduino/canary_wap/household.h)
stores up to eight bonded phones' IRKs to quiet their own arrivals, and
`presence_context.h` flips HOME/AWAY from owner sightings. The July 2026
audit flags the IRK store as HIGH-severity privacy debt in tension with Free
Signals Invariants A and F, notes that enrollment is unwired (`add_irk()`'s
only caller is a test, so the device pins `CTX_AWAY` forever), and asks for a
decision record — "carve-out for owner-paired devices, or drop the features"
([`esp32s3_firmware_full_audit_2026-07.md`](../audit/esp32s3_firmware_full_audit_2026-07.md)).
This research recommends **drop**, for the reasons in §6.2: an IRK is a
long-lived identifier shared with every bonded peer, a captured address keeps
resolving indefinitely, the signal is relayable and passes through walls, and
it lets a passive inference change event generation. Household keys (§6.3)
cover the legitimate need — "we're home" — with a deliberate, signed act. Until
the decision record lands, `household.h`'s comment that IRKs are
"flash-encrypted at rest on ESP32 with secure boot" should be corrected: flash
encryption does not cover NVS, NVS encryption cannot be built under the
Arduino 2.0.17 core, and secure boot is opt-in, so the IRKs (like the identity
key) are plaintext in NVS on default builds
([`key_at_rest.h`](../../firmware/common/identity/key_at_rest.h)).

### 6.9 Bill of materials (October 2026 snapshots)

| Household (4 people, 2 doors) | Parts | Approx. cost |
|---|---|---|
| Phones only | 2 key satellites (XIAO ESP32-C3 + M5Stack ST25R3916 unit, ~$12 each); the phones they already own | ~$25 |
| Phones + cards for kids and guests-at-home | + NTAG 424 DNA stickers or cards ($0.45–5 each) | ~$30–45 |
| + FIDO2 keys for people without phones | + $15–29 each | ~$45–75 |
| Optional UWB ranging (design only) | + Qorvo DWM3000 (~$24 qty 1) per door | +~$50 |
| For comparison | A commercial Aliro keypad/lock: $179–290 per door, plus a platform hub | |

No subscription, no Apple ID or Google account, and the same features in
Illinois, Texas, Portland and Quebec.

## 7. What a household gets — and gives up

| What "familiar faces" is bought for | With household keys | Coverage |
|---|---|---|
| Stop alerting me about my own family moving around the house | Home posture, set by a key tap at the door or from the app (user presence required): indoor witnesses drop to tamper-only. The arrival itself is still one ordinary door `Person` event, the same as anyone's | **Partial** — the indoor noise goes; your own arrival still shows as a person at the door |
| Arm when we leave, disarm when we're back, without a geofence | Away / Home / Night by a signed act; the apps keep holding no location permission | **Full** (once the posture vocabulary exists) |
| Tell me when someone who isn't us is at the door | Every `Person` at the door is reported, as today | **Full** for the signal; the system never says "stranger" or "unknown" — a Canary "does not accuse," and a forgotten card would make that word a false alarm about your own child |
| Kids without phones | A sticker or card on a backpack, or a one-button fob | **Full** — passive tags have no battery and no radio of their own, so they cannot become locators |
| Remove someone (a lost phone, a roommate moving out) | Revoke the key; the roster moves to a new epoch | **Full, and better** — a key can be taken back; a face cannot |
| Packages, people, vehicles, animals, zones | Already the witness vocabulary | **Full**, unchanged |
| Name the person at the door ("Alex is home") | — | **Refused.** A key is not a person (a borrowed card, a child with a parent's fob), and a name in a notification becomes an arrival diary on every recipient's phone |
| Recognize grandparents, the caregiver, the cleaner | They are a `Person`, like anyone | **Refused** — naming someone who carries nothing needs a template of everyone who walks up |
| Per-person alerts ("tell me when Alex arrives") | — | **Refused** — the basic stalkerware tool, and the coercion the Tin Can refuses by design |
| "Who's home" list, search by person, AI descriptions that name people | — | **Refused** (Invariant VII; non-negotiable 1) |
| Unlock the door on arrival | A lock of the household's choosing, outside the fleet (§6.7) | **Refused** on Canaries |

What the household gives up, plainly: visitors who carry nothing stay "a
person"; a borrowed or stolen key reads as a household key (which is why
nothing ever says a name); doors without a satellite get nothing; and in a one-
or two-person home the owner can of course infer who set the posture — the
system itself never knows a name. What it gets: $0 a year, no account, keys it
can take back, no biometric from anyone, and no vendor cloud to breach.

## 8. Cryptographic best practices — the checklist this design is held to

Most of this is the household-credential layer; some of it applies
repo-wide and is flagged as such.

**Identity**

1. **Identity is a key, not a face.** A member's credential is a private key in
   hardware they hold (phone secure enclave / StrongBox, FIDO2 key, NTAG 424
   DNA, an nRF52 fob). Readers store no member material, and the hub stores
   only **public keys** for every asymmetric credential (phone, FIDO2, fob),
   so physically extracting a reader yields nothing that impersonates anyone —
   the property a face template can never have. AES cards are the exception
   and must be said out loud: verifying NTAG 424 DNA or DESFire needs a
   symmetric secret on the hub (per-card keys diversified from a household
   master secret), which is clone-capable material. It lives only in the
   checker's private data, wrapped under a key outside backup scope, and a hub
   compromise means re-keying every card.
2. **Any biometric is local user verification on the member's own device**
   (Face ID, Android Class 3, a fingerprint FIDO key), unlocking the key. This
   is the FIDO/WebAuthn architecture: the relying party sees a signature and
   never the biometric. It is also why "without Apple devices" costs nothing:
   an Android phone's Class 3 biometric, a $29 FIDO key or a sub-dollar NFC sticker fills the
   same slot.
3. **Two curves, on purpose.** Phones' secure elements, passkeys, FIDO2 (ES256),
   Aliro and the cheap secure elements (ATECC608B, OPTIGA Trust M) are
   **P-256**; the repo's device identities and signatures are **Ed25519**
   ([`spec/beacon_channel_v0.md`](../../spec/beacon_channel_v0.md),
   [`src/crypto/signatures.rs`](../../src/crypto/signatures.rs)). Credentials
   are P-256 (ECDSA, verified on the hub with a vetted library); everything the
   household *signs as the household* — the roster, policy rows, receipts —
   stays Ed25519. Never convert one into the other; carry the algorithm in the
   record.

**Protocols**

4. **Challenge–response with a fresh nonce stops replay; it does not stop
   relay.** NCC Group relayed BLE proximity unlocks (a Tesla Model 3, Kwikset
   Kevo locks) adding about 8 ms, defeating both link encryption and latency
   checks. So anything that grants authority needs either a deliberate
   centimeter-range act (an NFC tap, a button press on the credential) or
   secure ranging (UWB), and UWB itself is not a proof: Ghost Peak reduced a
   measured 12 m to 0 m with up to ~4% success per attempt on $65 of hardware.
   Write "relay-resistant," never "relay-proof."
5. **Cards answer a live challenge, not a counter.** An NTAG 424 DNA "SUN"
   tap message (an AES-CMAC over the UID and a read counter) is
   replay-protected only if the verifier stores each card's last counter and
   rejects anything not strictly greater — a per-card usage tally that §6.4
   rule 4 forbids. So cards are used only through live mutual authentication
   (`AuthenticateEV2First`) with the hub's single-use nonce as the reader's
   random, which needs no stored counter; SUN tap messages are never used for
   anything with authority.
6. **Membership is a signed set with an epoch.** Adding or revoking a member is
   a signed roster change with a monotonic epoch; readers refuse a stale epoch.
   That makes revocation of a lost phone a single, auditable act — and the
   roster itself never leaves the house.
7. **Pairing borrows HAP's shape**: a short-code PAKE (SRP-6a as HAP does, or
   SPAKE2+ as Matter does) with an attempt cap, long-term identity keys, and
   Station-to-Station sessions over X25519 → HKDF → an AEAD. The repo's Opera
   pairing (X25519 + HKDF-SHA256 + a 6-digit SAS) is already this shape.

**Implementation hygiene (repo-wide)**

8. **Vetted libraries only:** libsodium or Monocypher (Ed25519/X25519/XChaCha20)
   on MCUs; ESP-IDF v6's PSA Crypto (Mbed TLS 4 / TF-PSA-Crypto) for TLS and
   hardware-backed operations — note v6 removes most legacy `mbedtls_*` crypto
   APIs; RustCrypto, aws-lc or BoringSSL on the hub. No hand-rolled primitives.
9. **AEAD nonces:** never AES-GCM with random 96-bit nonces under a long-lived
   key (nonce reuse leaks plaintext XOR and the authentication key). Use
   XChaCha20-Poly1305 where nonces are random, or strict counters / AES-GCM-SIV.
10. **ESP32 entropy:** the hardware RNG is only true-random while the radio
    (Wi-Fi, BT or 802.15.4) is on, the bootloader is running, or the SAR ADC
    entropy source is explicitly enabled; generate nonces and
    ephemeral keys after RF is up. Prefer deterministic signatures (Ed25519,
    RFC 6979 ECDSA).
11. **Secure boot on Espressif parts:** prefer RSA-3072 Secure Boot v2 over the
    ROM ECDSA path (AR2026-006), enable flash encryption in release mode, and
    give every device its **own** keys — AR2026-005's voltage-glitch key
    recovery is per-device and does not scale to a fleet unless keys are shared.
    (The repo's ladder for this is
    [`hardware_root_of_trust.md`](../design/hardware_root_of_trust.md); today it
    is opt-in.)
12. **Constant-time comparison** for MACs and tags, zeroize key buffers, keep
    keys out of logs and crash dumps, and fail closed.

**Household data at rest and in sync (HKSV's good half, without Apple)**

13. **Per-object random keys, wrapped to the household.** HKSV's pattern — a
    fresh AES-256 key per clip, the key end-to-end encrypted to the household's
    devices — maps onto the repo's sealed snapshot (one frame sealed to an
    X25519 key the device cannot open; [`sealed_snapshot_vault.md`](../sealed_snapshot_vault.md)).
    Extending "one holder" to "the household" is HPKE (RFC 9180) to each
    member's key, or an MLS (RFC 9420) group key when members join and leave
    often — MLS's commits give forward secrecy and post-compromise security, so
    a removed phone reads nothing new.
14. **Hybrid post-quantum for anything kept for years.** Encryption is the
    urgent half (harvest now, decrypt later): Apple PQ3 and Signal's
    PQXDH/SPQR both moved key exchange first. Use an X25519 + ML-KEM-768 hybrid
    (FIPS 203, August 2024) for long-retained evidence envelopes; note X-Wing is
    still an Internet-Draft, so pin the version and keep algorithm agility.
    Signatures are less urgent.
15. **Block egress at the network, not by promise.** HomeKit routers' "Restrict
    to Home" is the right idea for every camera in the house, ours or not:
    cameras get no internet route, and the hub is the only thing they can reach.

## 9. Ethereum and smart contracts — evaluated, not adopted

The question was whether a public chain improves anything here. The three jobs
a household could give one are (a) proving *when* the witness log existed, (b)
an auditable break-glass quorum, and (c) transparent revocation of household
keys. **Ethereum does none of them better than what the repo already has or
has already designed, and it costs privacy on all three.**

- **(a) Time anchoring.** The repo ships RFC 3161 anchoring of four ledger
  heads at two TSAs with a constant per-run request count
  ([`docs/timestamping.md`](../timestamping.md)), and specifies **OpenTimestamps**
  as a trust-kind-independent second leg, not yet built
  ([`spec/quorum_unseal_v2.md`](../../spec/quorum_unseal_v2.md) §4). OTS is free,
  needs no wallet, batches digests into one Bitcoin transaction per calendar
  cycle, and a proof rests only on SHA-256 and Bitcoin headers — no signing key
  that can expire. An Ethereum L1 anchor is cheap in October 2026 (a
  calldata-only anchor is ~22,280 gas; at 0.07–0.54 gwei and ETH ≈ $2,545 that
  is roughly $1.50–$11 a year at one anchor a day, but $41 at 2 gwei and $414
  at 20), and it pins time to the 12-second slot. That precision is the
  problem: a daily anchor from one address publishes a 365-point activity
  series, and an off-schedule anchor after an incident is visible to anyone —
  the opposite of the constant-count, schedule-not-incident anchoring
  Invariant III asks for. Funding the address almost always means a KYC
  exchange withdrawal, which address-clustering heuristics link. L2s cost about
  $0.0015 a transaction but add Security Council upgrade powers and a
  centralized sequencer to the trust base.
- **(b) Break-glass quorum.** A Safe n-of-m would publish the trustees'
  addresses, the threshold, which trustees approved, and a 12-second-precise
  timestamp of every unseal — permanently. The repo's quorum is already
  auditable without that: signed approvals, a receipt chain, and
  `break_glass_receipt_head` anchoring ([`src/break_glass/`](../../src/break_glass/)).
  Fusaka's P-256 precompile (EIP-7951, 6,900 gas, live since 2025-12-03) makes
  passkey signers cheap *on-chain*; the same keys are free to verify *off-chain*,
  which is where this design verifies them.
- **(c) Revocation transparency.** Anchor `policy_head` (quorum-gated roster and
  trustee changes) at the TSAs plus OTS, and add the planned C2SP checkpoints
  with fleet cross-witnessing. That is append-only and rollback-detecting for
  anyone the household chooses to show — without broadcasting a roommate
  leaving or a divorce on a ledger where an EAS "revocation" never deletes
  anything.

**What must never touch any chain, at any tier:** a face or voice embedding,
template, or any hash, commitment or "hash-equivalent" of one; names, emails,
phone numbers, addresses; device identifiers, MACs or device public keys; the
household roster or trustee identities; event contents, types, counts or
per-event hashes (low-entropy, so confirmable by guessing). The EDPB's
blockchain guidelines (02/2025, final version adopted July 2026) say hashed or
encrypted personal data can still be personal data, and that personal data
should not go on-chain at all where it cannot later be erased. **World
(formerly Worldcoin)** is the cautionary tale for "biometrics plus crypto":
it put only a "hash-equivalent" of the iris on-chain behind Semaphore
zero-knowledge proofs and still drew enforcement in at least ten
jurisdictions from 2023 to 2026 (Kenya, Spain, Portugal, Hong Kong, Bavaria's
BayLDA, Brazil, Indonesia, the Philippines, Colombia, Thailand). The liability
sits at capture and enrollment; no chain construction moves it.

**If an Ethereum leg is ever offered** (an opt-in enterprise anchor, not a
default), the constraints are: one per-run Merkle root over all ledger heads,
blinded with a fresh random nonce kept off-chain; calldata- or event-only
transactions, or the audited EAS predeploy, never a new stateful contract; a
fixed schedule with a constant count; a dedicated address funded without
linking it to the household. Any contract the project ever deploys is immutable
(no proxy, no admin — OWASP Smart Contract Top 10:2026 ranks access control #1
and adds proxy/upgradeability at #10), holds no value, accepts only a
`bytes32`, ships an ERC-7730 clear-signing descriptor, and is checked with
Slither, Foundry invariant tests and Halmos. The 2025 Bybit theft (about
401,000 ETH plus liquid-staking tokens, ~$1.46B, attributed by the FBI to DPRK) did not break a contract: a compromised
Safe{Wallet} front end fed hardware-wallet signers a disguised `delegatecall`
they blind-signed. Losses since are dominated by key, signer and infrastructure
compromise, which is the risk a household would be taking on.

## 10. What this research does NOT green-light

Named explicitly, because each is a plausible-sounding feature request:

- **Familiar faces on any camera, Canary or otherwise, local or not, opt-in or
  not.** Invariant II; §1.
- **Ingesting someone else's face recognition** — Frigate `sub_label`, Double
  Take, CompreFace, ESP-WHO, a UniFi webhook. The Frigate transport already
  discards these; nothing new may fold them back in.
- **A "face key" door verifier built by this project.** §11 is the strongest
  case for it and the reasons it fails today.
- **Soft biometrics as a substitute** — gait, body shape, clothing, height,
  re-identification across cameras. [`strategy/14`](../strategy/14-pose-estimation-v2-ai.md)
  already refuses skeletons as an identity substrate; Google's 2026 "clothing
  and body signals" matching is exactly the thing.
- **Naming a person anywhere** — the witness log, a notification, a digest, a
  Home Assistant entity, a log line (§6.4 rule 5).
- **Joining a credential to a witnessed event.** A key never rewords, demotes,
  quiets or delays a Canary's event or its notification; posture applies to
  every event equally (§6.2–§6.3).
- **Passive phone presence as a signal at all.** Rotating-address resolution
  can be replayed indefinitely and relayed live, carries through walls, and
  rests on a long-lived identifier (§6.2).
- **Static identifiers as credentials** — MIFARE Classic, 125 kHz prox, static
  iBeacons (including the Home Assistant Android app's BLE Transmitter). All
  cloneable.
- **Cloud relays in the credential path** — passkey "hybrid" flows tunnel
  through a third-party relay; an Apple or Google home hub as a requirement.
- **Any biometric, identity or roster data on a blockchain** (§9).
- **Access-control actuation on a Canary.** Free Signals Invariant D ("no
  integration with access control or enforcement systems";
  [`spec/canary_free_signals_v0.md`](../../spec/canary_free_signals_v0.md)) stands.
  A door lock is a separate device class (§6.6).

## 11. The strongest case for a face, and why it still fails

A refusal is only fair if it beats the best version of the idea, not Ring's.
So here is the best Face ID-like verifier a privacy-first project could build
on cheap hardware — a dedicated **face key**, never a Canary and never fed by
Canary frames:

- a physical button is the only thing that powers its sensors;
- near-field optics and an IR + depth liveness gate (a K230 structured-light
  pipeline, or IR flood + global-shutter IR + ToF) limit it to one face at
  0.3–1 m;
- **a possession credential is presented first**, so the match is a true 1:1
  against a claimed identity and nobody else's face is ever computed;
- the template is wrapped by a per-device key (and by the credential, where
  the credential can compute a secret), matched in RAM, never exported;
- Apple-style attempt limits and a fallback secret;
- its only output is one signed household command that carries no name.

It is designable. It should still not be built, for one reason that decides
it and several that would each be enough:

**The gating dilemma.** The only non-biometric way to know a person is enrolled
*before* looking at them is that they present something only enrolled people
have — a key. Gated behind a key, the face is mostly redundant; ungated, the
device is a scanner that computes a template for every courier who presses the
button. Even gated, it still computes the faces of exactly the people least
likely to have consented — whoever is holding someone else's stolen or
borrowed card — and the gate itself is a firmware rule (a `won't`) that one
malicious update turns into an ungated recognizer.

**What the face adds over a PIN.** At ESP-DL's published operating point
(about 90–94% true-accept at FAR 1e-4), the zero-effort odds over five tries
are about 5 in 10,000 — a 4-digit PIN's odds; a 6-digit PIN beats it by about
a hundred times (our arithmetic, from the published figures). NIST's
demographic differentials could make it 10–100× worse for some household
members. And the realistic household impostor is family: Apple's own caveat
names twins, look-alike siblings and children under 13 as where Face ID's
false-match rate rises, and a cheap verifier is weakest precisely there.

**Everything else:**

| Cost | Face key | Household key (§6) |
|---|---|---|
| Revocable if leaked | **No** — and re-enrolling the same face leaks it again (the 2025 IronMask multi-enrollment attack) | Yes, by epoch |
| Extractable from the device | Template on ESP32-class silicon: assume days of physical access (§4) | Satellite holds no member material |
| Certified spoof resistance | None available in open stacks or consumer locks | The phone's certified secure hardware does the biometric, if any |
| Model you may ship | InsightFace weights non-commercial; most others of contested provenance | No model |
| Delegable to a guest | No | Yes (if guest keys are ever added) |
| Known-parts cost per door | ~$58–85 before depth hardware, PAD testing, model rights or legal work, *plus* the card it needs anyway | ~$12, plus under $1–5 per card |
| Law | BIPA reaches "any individual" with no verification exemption; written releases, a retention policy, an always-available alternative (EDPB) | Outside every biometric statute surveyed |
| Constitution | Invariant II and I; governance §2.3; voice rule 5; Free Signals A, D, F; shipped "never who" copy | No invariant text changes |

**Verdict:** refused for the project, with no automatic revisit trigger. The
recommendation is to use the Face ID the household already owns: a biometric
on the person's own phone (or a fingerprint FIDO key) unlocks a key, and the
door checks only a signature (§6). Everything that made the face key safe came
from its possession half; remove the face and nothing safety-relevant is lost.

**If the owner still chooses it.** The project owner can amend their own
constitution, and the constitution says how. As written, though, the RFC path
in [`governance_and_invariants.md`](../governance_and_invariants.md) §3.2 covers
*potentially weakening* changes; "introducing fields that enable identity" is
in §2.3's **prohibited** list, which "MUST be rejected." So it is two RFCs, not
one: first amend governance itself to create a narrow category (say,
"self-verification devices outside the witness system"); then a face-key RFC
that, at minimum:

1. names every affected text with replacement wording — Invariant II, Invariant
   I's embeddings clause, conformance, AGENTS non-negotiable 1 and its
   forbidden capability and dependency lists, `CONTRIBUTING.md`, `SECURITY.md`
   — and either amends voice rule 5 or argues why owner face enrollment is
   acceptable when owner voice enrollment is not;
2. keeps Free Signals D and F untouched — the face key's result never becomes
   a Canary event and never drives access control on a Canary;
3. carries a threat model covering photo, screen and 3D-mask spoofs, look-alike
   family members, demographic differentials, physical extraction and
   malicious reflash, coercion, covert enrollment of a partner, and the
   borrowed-card bystander;
4. shows the strengthening alternative (§6) failing a named household need —
   the only plausible one is a person who can carry neither a phone nor a card
   nor a fob and cannot remember a PIN, and a wristband or ring credential
   should be tried first;
5. enforces structurally, not by policy: the button power gate,
   credential-first matching, credential-wrapped templates, a secure element,
   refusal to enroll without secure boot and flash encryption, and a build that
   cannot link into `witnessd` or any Canary;
6. ships sentinel tests in the style of [`tests/frigate_integration.rs`](../../tests/frigate_integration.rs)
   proving no embedding is computed without a valid credential and no frame,
   crop, score or name persists or reaches a sealed record;
7. publishes evidence before any claim: FAR/FRR on a consented set with
   demographic breakdowns (the FTC 2023 policy statement; *Rite Aid*), an
   accredited ISO/IEC 30107-3 PAD result at Level 2, and latency benchmarks —
   and never says "Face ID-like," "secure" or "verified";
8. documents commercial rights to the model weights, or retrains on consented
   data;
9. carries the legal pack: written releases (guardians for minors), a public
   retention and destruction policy, an always-available non-biometric
   alternative, and an honest statement that an account-free project cannot
   geofence;
10. replaces the shipped copy in the same change (the FAQ's "it can't," the
    glossary's "absent," the HACS store page, the website) and retires the
    legal audit's "forbidden by design" differentiator honestly;
11. includes a sunset clause: a published PAD break, a weight-license change or
    a failed audit voids the carve-out, and enrolled templates are crypto-erased.

And it should come *after* the canary-wap household decision record (§6.8),
which asks a smaller version of the same question — owner-paired identity,
with a revocable key instead of a face. A project that has not yet decided to
store an owner's phone key should not be deciding to store an owner's face.

## 12. Verdicts

| Option | Verdict | Revisit trigger |
|---|---|---|
| Familiar faces on any camera — Canary or not, local or not, opt-in or not | **Refused, permanently** (Invariant II) | None |
| Ingesting another system's face or plate identity (Frigate `sub_label`, Double Take, UniFi) | **Refused** — already stripped in code | None |
| An ungated face door ("is this one of my household?") | **Refused** — a 1:few scanner of everyone who presses | None |
| A face key built by this project (§11) | **Refused** | Only the two RFCs in §11; no automatic trigger |
| Passive phone presence (BLE IRK) as a recognition or quieting signal | **Not recommended**; recommend "drop" for the canary-wap household layer (§6.8) | The audit's decision record |
| **Household keys as signed posture commands (§6)** | **Recommended**, design stage | — |
| Aliro / Matter door locks | Interop only, separate device class (§6.7) | Google Wallet shipping Aliro home keys *and* a provisioning path that needs no platform hub |
| Copying HKSV's custody model (per-object keys, household E2EE, egress blocking) | **Adopt** as practice (§8) | — |
| Ethereum anchoring, Safe quorums, on-chain revocation | **Declined**; build the specified OpenTimestamps leg instead (§9) | An opt-in enterprise anchor under §9's constraints, on a customer's written request |
| Matter 1.5 cameras | Transport only, video verdict unchanged | Apple's "semantic camera" trigger in `apple_home_integration.md` §2 |

## 13. Next steps, smallest first

1. This document — the line, drawn once, against the strongest version of the
   idea. ✅ With it, two corrections: `apple_home_integration.md` §1.2 no longer
   says the HKSV recording publishing point is on the home hub, and
   [`PROVENANCE_INTEROP.md`](../security/PROVENANCE_INTEROP.md) no longer calls
   Bybit's wallet a "3-of-3" multisig (its on-chain threshold was three
   signatures; secondary analyses describe three of six owners).
2. **Answer the audit's household decision record** with "drop" (§6.8), and
   correct `household.h`'s encryption-at-rest comment in the meantime.
3. **The posture RFC** (decision #3) and **the inbound trust surface**
   (decision #4), with household keys as the no-Apple inbound path, including
   the open question in §6.4 rule 4 about journaling posture changes at all.
4. **The key-satellite device-class contract** — this document's §6.4 as its
   own page — and a host-tested hub checker core (challenge, roster, epoch,
   algorithm-tagged admin verification), with sentinel tests that plant a
   marker label and prove it reaches no output.
5. **The OpenTimestamps leg** already specified in `quorum_unseal_v2.md` §4.
6. A **FAQ entry** ("Can it tell my family from strangers?") once anything in
   step 4 exists to point at, and a line in the Tin Can's refusal list — the
   kids' watch is not a door key (no NFC; a door credential is a household
   secret; "is my kid home" is the locator role it refuses).
7. Nothing in the kernel. Nothing on any Canary. The kernel's answer to "who"
   remains: it cannot say.

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
  [Otroshi Shahreza et al., CVPR 2025 Workshops](https://openaccess.thecvf.com/content/CVPR2025W/ABAW/papers/Shahreza_Face_Reconstruction_from_Face_Embeddings_using_Adapter_to_a_Face_CVPRW_2025_paper.pdf) ·
  [IronMask multi-enrollment attack (PoPETs 2025)](https://www.petsymposium.org/popets/2025/popets-2025-0154.php) ·
  [FIDO: how FIDO works](https://fidoalliance.org/how-fido-works)
- **Credentials and radios:** [Aliro 1.0](https://csa-iot.org/newsroom/introducing-aliro-1-0-a-unified-standard-to-transform-the-access-control-ecosystem/) *(excerpt)* ·
  [Samsung Digital Home Key](https://news.samsung.com/us/samsung-wallet-launches-digital-home-key-smart-door-locks) *(excerpt)* ·
  [esp-aliro](https://github.com/espressif/esp-aliro) ·
  [NCC Group BLE relay advisory](https://www.nccgroup.com/research/technical-advisory-ble-proximity-authentication-vulnerable-to-relay-attacks/) *(excerpt)* ·
  [Ghost Peak, USENIX Security 2022](https://www.usenix.org/conference/usenixsecurity22/presentation/leu) *(excerpt)* ·
  [NTAG 424 DNA datasheet](https://www.nxp.com/docs/en/data-sheet/NT4H2421Gx.pdf) ·
  [Home Assistant Private BLE Device](https://www.home-assistant.io/integrations/private_ble_device/)
- **Espressif security:** [advisories index (AR2023-007, AR2026-005, AR2026-006)](https://www.espressif.com/en/support/documents/advisories) *(excerpt)* ·
  [ESP-IDF random number generation](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/system/random.html)
- **Cryptography:** [RFC 9420 (MLS)](https://www.rfc-editor.org/rfc/rfc9420) · [RFC 9180 (HPKE)](https://www.rfc-editor.org/rfc/rfc9180) ·
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
