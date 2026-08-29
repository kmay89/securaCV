# The enterprise surveillance landscape — and the flip

**Researched: August 2026.** The enterprise companion to
[the competitor app landscape](competitor_app_landscape.md): where that doc
compares us to the consumer camera apps (Ring/Wyze/Eufy/Reolink), this one
compares us to the stack sold to institutions — the ALPR dragnet
(Flock Safety / FlockOS), the retail loss-prevention industry (Verkada,
Auror, face recognition at the door), and the data-fusion layer above them
(Palantir-class) — and works out how the same legitimate demand flips into
witness infrastructure.

Reading order for the strategy this plugs into: the
[black-box evidence model](../strategy/13-black-box-evidence-model.md) (how
believed recorders earn trust), the
[industrial Pro Cam channel](../strategy/26-industrial-pro-cam-services-channel.md)
(whose wedge is already "the signal without the surveillance liability"),
[licensing for institutions](../strategy/21-licensing-structure-for-institutions.md)
(why self-hosting is free at any size), and the
[liability posture](../strategy/27-liability-minimization-posture.md). The
rules of this doc are the sibling doc's: every claim about us cites the file
that proves it; every claim about them carries its outlet and date; where a
thing of ours is demo rather than product, it says so out loud
(non-negotiable #4). New copy derived from here inherits the claims
discipline in [`LEGAL.md`](../LEGAL.md) §6 verbatim: tamper-evident never
tamper-proof, no "admissible," the quorum described as an authorization
gate — because that is what ships.

---

## 1. The pattern

### 1.1 What the stack sells

The enterprise surveillance stack is one product sold three ways, each layer
feeding the next:

- **Dragnet capture.** Flock Safety's Falcon ALPR network: ~120,000 cameras
  in 49 states, 20+ billion plate scans per month, roughly 70% of the US
  population under coverage, plus a "Vehicle Fingerprint" so a vehicle is
  searchable without a plate (NBC News 2026; TechSpot 2026; NC JOLT).
  Retail runs the same motion at the door and the register: Verkada and
  Avigilon face search, FaceFirst/Corsight watchlist matching, Auror's
  retailer network feeding 3,000+ police agencies (Biometric Update
  2024–2026; RNZ).
- **Identity binding.** The premium tier of every major platform is the
  moment a body or a vehicle becomes a *person*: face embeddings,
  plate-to-person lookup (Flock Safety's Nova, reported by 404 Media in
  spring 2025 to have been built partly on breach data until the reporting
  forced a retreat), watchlist enrollment.
- **Fusion.** Palantir Gotham resolves every record about a person into one
  canonical entity and renders it as a dossier an officer can pivot from any
  attribute — the unit of output is the person, not the incident (the leaked
  NCRIC user manual, Vice 2019; the G-Cloud service definition, 2024). Axon's
  Fusus real-time crime centers, Flock Safety's Business Network (1,000+
  companies, including four of the NRF top-10 retailers), and Ring's October
  2025 police-request integration pull private cameras into the same query
  surface (CNBC, Oct 16, 2025).
- **Retention as the default.** Flock Safety kept a 30-day everything-archive
  from founding until August 13, 2026, when it cut the *recommended* default
  to 7 days — while disclosing that more than 90% of partial-plate searches
  happen within a week of capture, i.e. the 30-day national haystack was
  never load-bearing for the investigative pitch (NBC News; Police1).

The commercial through-line: value scales with how broadly stored records
can be searched later, by whoever holds a login — and the sharing decision
is made by the platform and the institutional customer, never by the people
recorded.

### 1.2 What it has cost its customers (the dated record)

The liability record attaches at a specific joint. Every legal loss below
occurs where a system binds a body or vehicle to an identity without that
person's participation, or where a stored pool is queried for a purpose it
was never collected for:

- **May 2025** — a Johnson County, TX deputy searched 83,000 Flock Safety
  cameras across 6,809 networks for a woman suspected of a self-managed
  abortion; the search log read "had an abortion, search for female." No
  warrant. By August 2026: a House Oversight probe, an FTC-investigation
  push from Sen. Wyden and Rep. Krishnamoorthi, and state escalations
  (404 Media; Snopes).
- **2024–2025** — local police nationwide ran 4,000+ immigration lookups for
  ICE through the same network; secret federal pilots gave CBP, HSI, the
  Secret Service, and NCIS direct access while customers were told there was
  "no relationship with DHS"; Illinois' Secretary of State found the company
  in violation of state law and referred it to the Attorney General
  (404 Media, May–Oct 2025; ilsos.gov, Aug 25, 2025).
- **2025–2026** — officer-misuse cases at scale (a Kansas police chief
  tracked an ex-girlfriend's car 164 times in four months; firings,
  decertification, a criminal conviction — Washington Post syndication,
  Aug 2026), and at least 26 documented wrongful stops at gunpoint from ALPR
  misreads since 2018, with a Vallejo, CA randomized trial finding 37% of
  stationary-reader hits were misreads (Institute for Justice, 2026).
- **The customer exodus** — between 30 and ~80 communities canceled,
  deactivated, or rejected the dragnet by mid-2026, depending on the count
  (NBC News: 30+; EFF: 50+). Denver removed all 110 cameras in March 2026
  and replaced them with a competitor **on "stricter data controls"** —
  trust loss priced as a named competitive displacement. Evanston had to
  send a cease-and-desist after cameras were reinstalled against the city's
  wishes (Evanston RoundTable).
- **Retail's identity bill** — Rite Aid: a five-year FTC facial-recognition
  ban (Dec 2023) after thousands of false matches — over 900 alerts on one
  person in five days — with disparate impact in Black and Asian
  neighborhoods. Clearview AI: a $51.75M BIPA settlement paid in company
  equity because it could not fund cash (approved Mar 2025). Meta: $1.4B to
  Texas under CUBI (Jul 2024); Google: $1.375B (2025). Mercadona: €2.52M
  (AEPD). Bunnings: privacy-breach determination substantially affirmed on
  appeal (Feb 2026). Home Depot: a July 2026 California class action over
  perimeter ALPR at all 233 stores. Verkada: a $2.95M FTC penalty and a
  20-year mandated security program after its 2021 breach exposed 150,000
  customer cameras (FTC, Aug 2024).
- **Fusion's audit failure** — LAPD's data-driven LASER program was
  terminated in 2019 after the Inspector General found it could not be
  meaningfully audited; Chicago's "heat list" was decommissioned after RAND
  found its main measurable effect was arrests of listed people, not their
  protection; Pasco County's sheriff **admitted in a December 2024
  settlement** that his predictive program violated the First, Fourth, and
  Fourteenth Amendments. The 2012 Senate PSI report found fusion centers
  produced not one disrupted plot in 13 months of reviewed output — and the
  model expanded anyway, into ICE's ImmigrationOS (~$30M growing to ~$287M
  of ICE work) and an up-to-$1B DHS agreement by 2026 (American Immigration
  Council; EFF; CNBC).

Two structural findings sit on top of that record:

1. **The failure surface is the data model, not the optics** — retention,
   unbounded fan-out, unverified purpose strings, silent sharing. No scandal
   above required a camera to malfunction.
2. **Post-hoc auditability documented abuse; it did not prevent it.** The
   audit logs are how journalists and a Secretary of State proved misuse —
   after the fact. The ACLU's standing objection to the August 2026 policy
   overhaul: nobody outside can bound the miss rate.

### 1.3 The legitimate demand underneath

Stripped of the mechanism, the demand is real and consistent:

- **Dispute and incident evidence that survives scrutiny** — time-stamped
  records tied to specific events, with chain of custody, usable in audits,
  HR disputes, insurance claims, and court.
- **Theft and property-crime cases** — stolen-vehicle recovery and "a car
  was involved; which one?" are the purchase drivers the vendors' own
  customer stories lead with.
- **Cross-site awareness** — an organized-retail-crime crew spans stores; a
  robbery series spans precincts; multi-site operators genuinely drown in
  un-joined records. (Held honestly against the NRF's own record: the trade
  group retracted its "half of shrink is ORC" claim in December 2023, and
  reported shoplifting declined in 2025.)
- **A clean handoff to police or counsel** — evidence-sharing platforms
  exist because the alternative is USB sticks and DVD burns.

The separation that the whole flip rests on: the need is *specific,
incident-anchored, verifiable evidence assembled under due process after
something happened*; the mechanism the industry built is *general,
person-anchored, query-time retrieval before and regardless of any
incident*. Every named failure in §1.2 lives in the gap between those two.

---

## 2. The structural table

Them = the ALPR dragnet, face-recognition LP, and fusion, per §1's record.
Us = only claims makeable under the repo's claims-discipline rules; each
cell cites its invariant or file.

| Dimension | Them | SecuraCV |
|---|---|---|
| **Identity substrate** | The product: plates + vehicle fingerprints, face embeddings and watchlists, person-resolved entities | None, structurally: no plates, no face embeddings, no biometrics — Invariant II ([`spec/invariants.md`](../../spec/invariants.md)); `ObjectClass` is `Person\|Vehicle\|Animal\|Package`, never `Face` or `LicensePlate` ([`AGENTS.md`](../../AGENTS.md) #1) |
| **Unit of record** | The person/vehicle — dossiers, hot lists, canonical entities | The incident — semantic events in a signed, hash-chained log |
| **Queryability** | One query fans out across 83,000 cameras nationally (documented, May 2025); retailer networks police-searchable by contract | No retrospective identity search, no bulk pattern mining; inspection of the past is sequential and context-bound — Invariant VII |
| **Retention** | 30-day everything-archive default until Aug 2026; now a 7-day *recommendation* existing customers can ignore, plus indefinite "Evidence Mode" cold storage | Raw media discarded after processing unless explicitly sealed — Invariant I; ordinary and pre-event frames never persist, while a deliberately sealed snapshot persists as encrypted ciphertext that opens only by break-glass, until its retention window removes it ([vault](../sealed_snapshot_vault.md)). Export timestamps are 10-minute buckets — Invariant III ([why exports work this way](../why_secure.md)) |
| **Retroactive re-analysis** | The pitch: yesterday's footage under tomorrow's model | Events bind to the ruleset active at creation; a new model can never re-mine yesterday's record — Invariant VI |
| **Who can verify** | The vendor and the customer's own compliance function; the ACLU could not bound the audit miss rate (Aug 2026) | Anyone, offline: `sha256sum` + `openssl` per the shipped kit ([court export](../court_export.md)), any Content Credentials tool for the C2PA sidecar ([design](../design/c2pa_export.md)), or the drop-a-file [evidence viewer](../../viewer/evidence_viewer.html) — no account, no network, no vendor |
| **Subject access** | None in the architecture: the scored, mapped, or stopped person gets no notice, access, or verification path | The record's integrity verifies for whoever holds the bundle — accused as well as accuser, no SecuraCV software, no trust in Errer Labs ([court export](../court_export.md)). Honest limit: those checks establish integrity, not authorship — the bundle carries its own key, so authorship is self-attested until checked against a device key obtained separately, and the [evidence viewer](../../viewer/evidence_viewer.html) says exactly that until one is supplied |
| **Access governance** | A login and a free-text reason field (how "had an abortion" entered a search log); case codes arriving end-2026, customer-enforced | n-of-m trustee quorum for sealed evidence, sign-what-you-see approvals, a receipt for every attempt including denials — shipped ([`spec/break_glass.md`](../../spec/break_glass.md), [operator guide](../operator_guide.md)). Honest limit: an authorization gate with tamper-evident receipts, not a cryptographic threshold — the threshold tier is designed, not shipped ([`spec/quorum_unseal_v2.md`](../../spec/quorum_unseal_v2.md)) |
| **Disclosure trail** | Search logs readable by the agency, surfaced publicly only via FOIA or leaks | Every disclosure appends a signed, chained receipt labeled with its authorization mode ([evidence lifecycle](../evidence_lifecycle.md)) |
| **Sharing model** | Opt-in defaults wire private cameras into national police search pools (the HOA checkbox; the Business Network; Fusus enrollment; Ring community requests) | No centralized custody — Invariant IV: the vendor holds nothing and no uninvited party has a query path. Scope stated plainly: an operator MAY delegate remote access (the rotating capability token behind their own TLS proxy or tunnel), and that holder can read events and exports over the local API — what no delegation can grant is an identity or bulk-pattern query, because no such API exists (Invariants II + VII). Disclosure is operator-initiated, per incident, receipted ([scheduled exports](../scheduled_exports.md)). The neighborhood channel carries no PII on the wire — templates only, two-pubkey co-signed, lint-enforced in CI ([`AGENTS.md`](../../AGENTS.md) Beacon invariants) |
| **Failure mode when abused** | A policy violation inside a working system: the abortion search, the ICE side door, and the stalking cases all used the product as designed; audits found them later or never | The abusive query has no API to call — the refusal is structural (Invariants II + VII); a compromised operator still leaves chained receipts, and tampering becomes visible, never "impossible" ([brand rules](../BRAND.md)) |
| **Cost model** | ~$2,500–$3,000 per camera per year (Flock Safety benchmarks); per-camera cloud licenses (Verkada); enterprise SaaS by contract | Free to self-host for a business or government at any size and site count — charge for atoms, hours, and liability, never bits or privacy ([`LICENSING.md`](../../LICENSING.md); [strategy 21](../strategy/21-licensing-structure-for-institutions.md)) |
| **Works with existing cameras** | Proprietary hardware (Verkada) or vendor-owned poles (Flock Safety) | RTSP / V4L2 / Frigate-bridge ingest shipped ([operator guide](../operator_guide.md)); bench-validation caveat carried per [brand rules](../BRAND.md) |

---

## 3. The flip

### 3.1 Name the category

The incumbents sell **surveillance**: standing capture of everyone, identity
binding, and query-time retrieval for whatever purpose the querying
institution has at that moment. Nobody in §1's record sells the other
thing — the category is empty. **Witness infrastructure** is evidence
capture that is:

- **tamper-evident** — signed and hash-chained at the device, with
  C2PA-style provenance on the export, so modification breaks the chain and
  is detectable by anyone, offline ([why exports work this way](../why_secure.md));
- **non-queryable** — no central pool, no national lookup, no person-keyed
  retrieval; a "search 83,000 cameras for a woman" is not a policy violation
  here, it is a missing API (Invariant VII);
- **identity-free** — the record says *what happened*, never *who*
  (Invariant II; [why witnessing matters](../why_witnessing_matters.md));
- **verifiable by the accused as well as the accuser** — the verification
  path is `sha256sum` and `openssl` on the recipient's own machine, the
  exact property every architecture in §1 lacks and the one their
  accountability failures turn on. (Scope, per the table's honest limit:
  the bundle alone proves integrity and timestamps; proving *which device*
  authored it takes a device key obtained separately from the bundle.)

### 3.2 The same demand, answered

- **Dispute evidence** → the receipted export plus the disclosure kit:
  custody-and-control record rendered from signed receipts, RFC 3161 anchor
  tokens, pre-filled FRE 902(13)/(14) certification drafts for the certifier
  to review with counsel, verification instructions opposing counsel can run
  alone ([court export](../court_export.md)). Where the incumbents package
  footage *for police*, this packages a verifiable record for *both sides*.
- **Theft cases** → a sealed, chained event record of the incident window —
  with opt-in, event-triggered sealed snapshots the device itself cannot
  read back ([sealed snapshot vault](../sealed_snapshot_vault.md)) —
  released per incident under the operator's own quorum, instead of a
  standing plate archive with a documented 37% stationary misread rate and
  an officer-stalking case file.
- **Cross-site awareness** → per-site kernels with scheduled, receipted
  nightly exports synced off-site, each bundle independently verifiable
  anywhere ([scheduled exports](../scheduled_exports.md)). Linkage across
  sites happens at the investigator's initiative, per incident, under legal
  process — never by standing bulk access, which is the specific mechanism
  the New Zealand ANPR audits and the US records requests show being abused.
- **The clean handoff** → the kit *is* the handoff: a directory a lawyer can
  file, every file attested in the manifest, unverifiable bundles refused,
  gaps declared rather than hidden ([evidence lifecycle](../evidence_lifecycle.md)).

The one-sentence version, from the project's own systems argument: when harm
occurs, perception data becomes evidence — and today it is controlled by the
party with the liability. "A system that can act in the world must not be
the sole narrator of its own actions"
([why witnessing matters](../why_witnessing_matters.md)). The enterprise
stack makes the venue the sole narrator *and* wires its narration into a
national search pool. Witness infrastructure makes the record independently
checkable and makes bulk narration impossible.

### 3.3 Honest limits — what a venue cannot do with us

Stated as plainly as the features (non-negotiable #4). A venue running
SecuraCV **cannot**:

- **Look up a plate.** No license-plate OCR exists and none will be added.
  No hot-list alerting, no stolen-car hit at the gate. A venue that needs
  NCIC hot-list response needs a different product — and inherits that
  product's record.
- **Match a face.** No embeddings, no watchlists, no alert when a banned
  individual returns. Repeat-offender recognition at the door is exactly the
  mechanism the Rite Aid ban, the BIPA settlement table, and the Bunnings
  determination priced. We do not ship a compliant version of it; we ship no
  version.
- **Track a person across sites.** No identifiers comparable across devices
  or long horizons (Invariant II), so "the same individual at five stores"
  is not answerable — by us, or by anyone who compels us.
- **Search the past for a person or pattern** (Invariant VII), with
  timestamps deliberately coarser than a subpoena might wish
  (Invariant III).
- **Re-mine old records with a new model.** Invariant VI cuts both ways: the
  buyer cannot be feature-crept into surveillance later, and neither can we.
- **Watch a live video wall — from us.** Invariant I: no SecuraCV output
  carries live video; the Witness Wall renders the record, not pixels.
  Scope stated plainly: in a no-rip-and-replace deployment the upstream
  camera or NVR keeps its own live view and recordings (Frigate retains
  both — [integration](../frigate_integration.md)); we add a witness layer
  beside the venue's existing eyes, we do not claim to close them.

Why the refusal is the product: every missing feature above is, verbatim, a
line item in §1.2's liability record. The plate lookup is the abortion
search and the stalking cases; the face match is the FTC ban and the $1.4B
Texas settlement; cross-site person tracking is the class-action theory; the
queryable archive is the ICE side door. "Can't, not won't" means the venue's
counsel is not reviewing a policy — they are reviewing an architecture with
no code path to the liability. And the guarantee is checkable: the claim is
never "trust our controls," it is "verify the record yourself, offline" —
the inverse of the customer-enforced, outside-unverifiable safeguards the
ACLU rejected in August 2026.

Also carried honestly: much of the firmware is CI-verified rather than
hardware-verified ([brand rules](../BRAND.md)); the quorum is authorization
plus audit, not threshold cryptography; recording-consent law is the
operator's to satisfy ([`LEGAL.md`](../LEGAL.md) §8); and the venue UI in
§4's third move is design-plus-demo on top of shipped primitives, and must
be sold as exactly that.

---

## 4. Ranked flip moves

Ordered by leverage divided by distance-to-real. Statuses are honest, not
aspirational.

1. **The disclosure kit as the LP deliverable.** *Buildable now.* Retail's
   demand items #1 and #4 are answered by shipped code: `court_export` +
   RFC 3161 anchoring + offline verification. The pitch to an LP or ops
   team: the artifact you hand an insurer, a card processor, or an officer
   verifies on their machine with `sha256sum` and `openssl` — never with our
   software. Copy discipline: "prepared for FRE 902(13)/(14) certification
   workflows," never "admissible." Work required is packaging and copy, not
   code.
2. **Break-glass quorum as venue governance.** *Buildable now.* Trustees are
   arbitrary Ed25519 keyholders, so a venue maps the roster to
   manager + union rep + counsel (or owner + insurer + attorney) with zero
   code changes; `drill` rehearses the cycle before any real incident and
   the web console is the demo a business signs off on. The direct answer to
   the free-text reason field: access to sealed evidence requires named
   humans co-signing what they see, and every attempt — granted or denied —
   leaves a receipt. Sold with the scope note attached.
3. **The Witness Wall venue story.** *Positioning today.* The narrative
   exists end-to-end as design plus demo: the Lab's TV emulator business
   profile over a real, CI-tested native tvOS Business profile. The
   signature moves (incident capture, dispute pack, close-of-night digest,
   multi-site rows) are emulator-only and must be presented as design, with
   the shipped primitives named underneath.
4. **Multi-site evidence operations on Home Assistant.** *Buildable now.*
   Per-site kernel + add-on panel + one-click signed bundles + scheduled
   nightly receipted exports synced off-site — the honest cross-site answer
   for a 3–30 location operator, sold without inventing the demo-only
   multi-site board.
5. **C2PA Content Credentials on every export.** *Buildable now* (shipped v1
   behind the off-by-default `c2pa-export` feature). The one adjacent trust
   standard courts have begun engaging with, already a sidecar on our
   bundles — with the trust posture stated out loud: well-formed and
   unmodified, signer not on the public trust list.
6. **The no-rip-and-replace hook.** *Buildable now.* Works with the cameras
   you already own — RTSP, V4L2, Frigate bridge are shipped code paths —
   against proprietary-hardware and vendor-owned-pole lock-in.
7. **The licensing wedge.** *Positioning.* Against $2,500–$3,000 per camera
   per year: free self-hosting at any size is a structural price attack that
   doubles as a trust claim — no per-bit revenue means no incentive to hoard
   data ([`LICENSING.md`](../../LICENSING.md)).
8. **The offline evidence viewer as the skeptic's handoff.** *Buildable
   now.* One HTML file an adjuster, opposing counsel, or reporter drops a
   bundle onto — no account, no network, no vendor. Subject-verifiability
   made into a link you can send.
9. **PII-free Beacon for neighborhoods.** *Buildable now* (firmware built
   and CI-tested; bench caveat). The counter-offer in the dragnet's founding
   market — the HOA, where the private-to-police pipeline is a checkbox:
   template-only alerts, no descriptions of people, no plates, no GPS, no
   household identifiers on the wire, two-pubkey co-signing, drills
   wire-distinct from real alerts, several rules lint-enforced in CI.
10. **Graduate the dispute pack from demo to product.** *Needs design.* The
    highest-value product gap this analysis exposes: the emulator's one-tap
    incident capture and dispute pack become real by wiring the tvOS
    Business profile to the shipped primitives (receipted export, court kit,
    sealed snapshots) plus the missing `/api/sealed-log` endpoint.
11. **The enterprise-custody threshold tier.** *Needs design* (spec exists:
    [`spec/quorum_unseal_v2.md`](../../spec/quorum_unseal_v2.md)). Upgrades
    move #2's honest limit into a cryptographic guarantee for buyers whose
    counsel asks the right question. Announced only in the future tense.
12. **The displacement pitch.** *Positioning.* Denver's March 2026 removal
    of all 110 dragnet cameras in favor of a competitor citing "stricter
    data controls" proves institutions now buy on data-model grounds. For
    the 30–80 communities that have canceled or rejected the dragnet, the
    message is the structural table: not stricter controls on the same
    pool — **no pool**. Copy human-gated per [`LEGAL.md`](../LEGAL.md) §9,
    and honest that municipal deployment is not a launched product motion.

---

## Sources (curated)

The load-bearing subset, grouped; dates inline above.

- Scale, pricing, and the cancellation wave:
  <https://www.nbcnews.com/tech/tech-news/flock-police-cameras-scan-billions-month-sparking-protests-rcna230037> ·
  <https://www.spot.ai/blog/flock-safety-pricing-2026> ·
  <https://www.eff.org/deeplinks/2026/06/were-fighting-mass-surveillance-tech-and-winning>
- The federal/immigration and abortion-search record:
  <https://www.404media.co/ice-taps-into-nationwide-ai-enabled-camera-network-data-shows/> ·
  <https://www.404media.co/a-texas-cop-searched-license-plate-cameras-nationwide-for-a-woman-who-got-an-abortion/> ·
  <https://www.ilsos.gov/news/2025/august/250825d1.pdf> ·
  <https://www.wyden.senate.gov/news/press-releases/wyden-flock-safety-enabled-warrantless-federal-surveillance-of-americans-through-secret-pilot-programs>
- Misreads, stalking, litigation:
  <https://ij.org/report/who-watches-the-watchmen/> ·
  <https://ij.org/case/norfolk-alpr/> ·
  <https://www.washingtonpost.com/technology/2026/08/police-flock-surveillance-misuse/>
- Retail loss prevention and biometric liability:
  <https://www.ftc.gov/news-events/news/press-releases/2023/12/rite-aid-banned-using-ai-facial-recognition-after-ftc-says-retailer-deployed-technology-without> ·
  <https://www.ftc.gov/news-events/news/press-releases/2024/08/ftc-takes-action-against-security-camera-firm-verkada-over-charges-it-failed-secure-videos-other> ·
  <https://www.rnz.co.nz/news/politics/569251/police-too-loose-with-number-plate-recognition-system-review-finds> ·
  <https://www.biometricupdate.com/202511/auror-launches-facial-recognition-tool-for-retail-crime-prevention-and-safety> ·
  <https://nrf.com/media-center/press-releases/nrf-statement-organized-retail-crime-report>
- Fusion and predictive policing:
  <https://www.vice.com/en/article/revealed-this-is-palantirs-top-secret-user-manual-for-cops/> ·
  <https://www.courthousenews.com/audit-finds-lapd-predictive-policing-programs-lack-oversight/> ·
  <https://www.americanimmigrationcouncil.org/blog/ice-immigrationos-palantir-ai-track-immigrants/> ·
  <https://www.eff.org/deeplinks/2026/01/report-ice-using-palantir-tool-feeds-medicaid-data> ·
  <https://www.technologyreview.com/2020/06/05/1002709/the-activist-dismantling-racist-police-algorithms/>
