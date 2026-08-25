# The competitor app landscape — Ring, Wyze, Eufy, Reolink

**Researched: August 2026.** An app-level competitive analysis of the four
consumer smart-home security ecosystems people most often compare a Canary
fleet against — what their apps actually do, what they charge for, where
they break trust, and what that means for SecuraCV's Apple apps, the web
Lab, and the desktop apps. This is the full-app companion to the
notification-focused survey in
[`../design/alerts_event_history.md`](../design/alerts_event_history.md) §2,
which stays the design of record for the alert lifecycle itself.

Three questions this doc answers:

1. **What do the four apps offer, feature by feature?** (§1–§3, the matrix)
2. **Where do we already exceed them — and where do we honestly lose?** (§4–§5)
3. **What should be built next, using only capabilities the platform already
   has?** (§6, the ranked roadmap; §7, the ecosystem-legibility findings)

Method: vendor feature pages and support documents, App Store / Play
listings, release notes, mainstream reviews, and community forums (each
vendor's own forum plus Reddit), all fetched August 2026. Load-bearing
sources are listed at the end. Repo claims cite the file that proves them.

---

## 1. The four, one paragraph each

**Ring (Amazon)** is the volume leader and the purest subscription machine:
with no plan, a Ring camera records *nothing* — free accounts get live view
and a doorbell ping, and every 2024–2026 headline feature (Smart Video
Search, AI Video Descriptions, Single/Unusual Event Alerts, 24/7 recording)
sits in the $19.99/mo top tier. The plan was renamed twice in 15 months
(Protect → Ring Home, Nov 2024 → Solo/Multi/Pro, Jan 2026) amid price hikes
that doubled grandfathered $10 plans. Apple coverage is deliberately hollow
(no HomeKit, no Apple TV app, watch app removed in 2021, web-only desktop
since 2021) because the integration budget goes to Alexa. The privacy arc is
whiplash: a 2023 FTC settlement over employee viewing of customer video and
~55k compromised accounts, the police request portal killed in January 2024,
then reversed in 2025 via an Axon partnership — plus Familiar Faces facial
recognition and neighborhood-scanning features that drew Congressional
letters. Its opt-in E2EE disables so much of the product that effectively
nobody runs it.

**Wyze** is the budget-volume player: $20–$60 cameras, one app for a
sprawling catalog, genuinely free microSD recording — and a documented trust
deficit. Three separate incidents let users see strangers' cameras or data
(a 2019 leak covering 2.4M accounts; a 2023 web-view bug; February 2024,
when ~13,000 users received other households' event thumbnails and 1,504
tapped through), plus a vulnerability left unpatched for three years. All of
its 2025–2026 AI differentiation — Descriptive Alerts, AI Video Search, the
NBD 1–5 severity filter — is locked to a $19.99/mo tier, while the free
tier's 5-minute cooldown between events functions as a penalty. Repeated
AWS outages have taken down live view, events, and notifications. There is
no HomeKit, no Matter, no watch/TV/Mac apps, and the web view streams one
camera per month unless you pay.

**Eufy Security (Anker)** is the closest analog to our position — "no
monthly fee, local storage" — and the cautionary tale for it. The HomeBase 3
gives storage-bounded local retention with free on-device AI, and
monetization runs through hub hardware rather than subscriptions. But in
2022 researchers showed the "local only" marketing was false — event
thumbnails (with face data) went to eufy's cloud, and live streams were
viewable unauthenticated off-site — and Anker first denied it, then quietly
deleted eleven privacy promises from its website before apologizing. In 2025
it paid users $2 per video of (including staged) package thefts to train AI.
Its app history is a five-app maze mid-merge into a super-app whose new tabs
include a store, which security customers read as ads inside their alarm
panel. HomeKit was abandoned for four years (returning cautiously in 2026);
there is no watch, TV, or Mac app — eufy sells an 8-inch display instead.

**Reolink** is the prosumer local-first camera vendor: high-resolution
hardware, microSD/NVR/Home Hub storage with zero subscription for the core
experience, RTSP/ONVIF openness, a first-class Home Assistant integration —
and a mobile app widely rated the weakest link (≈3.5★ on Google Play across
38K reviews against strong hardware reviews). Rich notification thumbnails
are paywalled on most models — the most resented toll on a "no fees" brand —
scrubbing forces a stream reload on every seek, there is no geofencing after
five years of requests, and firmware updates mean hunting `.pak` files by
hardware revision. It is the only one of the four that ships iOS Critical
Alerts. Its 2026 direction (the ReoNeura AI Box) moves AI on-device, with
event severity levels arriving via new hardware.

## 2. The pattern (what all four get wrong — our openings)

These recur across all four vendors' own forums, reviews, and incident
histories; each is an opening because our architecture already answers it:

1. **Push is treated as the system of record.** Best-effort cloud push with
   no reconciliation — silence is indistinguishable from "nothing happened."
   Delayed and silently-lost notifications are the #1 complaint category for
   all four. None records *whether an alert actually reached you*.
2. **The alert-fatigue fixes are sold back as subscriptions.** Ring's
   episode collapse is top-tier; Wyze's severity filter is $19.99/mo; eufy's
   cross-camera dedup needs a $150 hub; Reolink's severity levels need the
   2026 AI Box. The free tiers are deliberately noisy.
3. **Nobody has an acknowledgment model.** Ten household members each
   re-triage the same event; nothing tells anyone that someone already
   answered. Every report flags this; none of the four ships it.
4. **The Apple surface is unowned.** Across all four: zero watch apps, zero
   Apple TV apps, zero Live Activities, zero Focus integration, zero
   complications; HomeKit absent (Ring/Wyze/Reolink) or freshly rebuilt
   after years away (eufy). Only Reolink uses iOS Critical Alerts.
5. **Cloud outage = blind house.** AWS incidents repeatedly took out Wyze
   and degrade Ring; even the local-first brands relay push through their
   clouds. Local recording survives; *alerting* doesn't.
6. **Trust breaks at the marketing/architecture seam.** Eufy's "local only"
   falsified; Wyze's cross-user thumbnails; Ring's FTC findings and police
   whiplash. Every vendor asks to be *believed*; none can be *checked*.
7. **Tier and surface churn destroys legibility.** Three subscription
   renames in 15 months (Ring), six coexisting plan regimes (Wyze), five
   apps mid-merge (eufy), four surfaces with four feature sets (Reolink).

## 3. Feature matrix — the load-bearing rows

Legend: **HAS** · **PARTIAL** · **PAY** (paywalled) · **ABS** (absent) ·
**N/A** (absent by design — the refusal is the product thesis).

### 3.1 Alerting

| Feature | Ring | Wyze | Eufy | Reolink | SecuraCV today |
|---|---|---|---|---|---|
| Typed detection alerts (person/vehicle/animal/package) | PAY | PAY | HAS | HAS | PARTIAL — class rides the BLE v2 beacon (`ios/.../Wire/FleetBeacon.swift`); folded + rendered as of this change |
| Severity tiers mapped to OS interruption levels | ABS | PAY | ABS | ABS | **HAS** (`AlertCenter`: digest/important/critical) |
| Critical alerts that break DND/mute | ABS | ABS | ABS | HAS | **HAS** (honest degrade when the entitlement is absent) |
| Episode collapse / repeat damping | PAY | ABS | PARTIAL (hub) | ABS | **HAS free** (`RepeatGovernor`, `AlertStorm`) |
| Quiet hours that cannot silence life-safety | PARTIAL | ABS | ABS | PARTIAL | **HAS** (`QuietHours` cannot hold `.critical`) |
| Alert-fatigue learning | PAY | PAY | ABS | ABS | **HAS free, local** (`AlertTuning` — counters never leave the phone) |
| Delivery recorded, refusals recorded with the reason | ABS | ABS | ABS | ABS | **HAS** (`AlertDelivery` + `undeliveredReason`) |
| Heartbeat / dead-man's-switch + measured path test | ABS | ABS | ABS | ABS | **HAS** (`Heartbeat`, measured ms) |
| Escalation when unacknowledged, then a second person | PAY | PAY | PAY | ABS | **HAS free** (`EscalationPolicy` + CloudKit household relay) |
| Away alerting without a vendor cloud | ABS | ABS | ABS | ABS | **HAS** (content-free wake via the user's own iCloud) |
| Life-safety acoustic events (smoke/CO cadence, glass break) | PAY | PARTIAL | HAS (hardware) | ABS | **HAS as of this change** — the firmware wire names (`smoke_alarm_t3`, `co_alarm_t4`, `glass_break`, `knock`, `doorbell`) now land at honest severities instead of the unknown-event fallback |

### 3.2 Timeline & history

| Feature | Ring | Wyze | Eufy | Reolink | SecuraCV today |
|---|---|---|---|---|---|
| Any event history without paying | ABS | PARTIAL (microSD) | HAS | HAS | **HAS** (free by local construction) |
| Day-shape / density view | PAY | ABS | PARTIAL | PARTIAL | **HAS** (`TimelineScrub` ribbon: coarse buckets, declared gaps) |
| Filtering / search of history | PAY | PARTIAL | HAS | PARTIAL | PARTIAL — no search; per-witness view added by this change |
| Open/resolved lifecycle, seen-state, ack | ABS | ABS | ABS | ABS | **HAS** (`AlertRecord` lifecycle) |
| Per-device history screen | HAS | HAS | HAS | HAS | **HAS as of this change** (device detail history section) |
| Video scrubbing | PAY | PARTIAL | PARTIAL | PARTIAL (worst) | N/A — no pixels exist to scrub; the record is semantic events |

### 3.3 Sensors

| Feature | Ring | Wyze | Eufy | Reolink | SecuraCV today |
|---|---|---|---|---|---|
| Camera line | HAS (broadest) | HAS | HAS | HAS (highest-res) | PARTIAL — events only, never footage (the thesis) |
| Contact/entry sensors | HAS | HAS | HAS | ABS | PARTIAL (`contact_state_change` in the dictionary; Sentinel designed) |
| Radar/mmWave as a *product* (occupancy, breathing) | PARTIAL (in-camera) | ABS | PARTIAL | PARTIAL | **HAS** (canary-sense MR60BHA2) — modeled in the app, rendering landing incrementally |
| Wi-Fi CSI sensing (no camera, no radar) | ABS | ABS | ABS | ABS | **HAS, unique** (`docs/csi_modules.md`) |
| Smoke/CO awareness | PAY (listener) | PARTIAL (cloud sound-AI) | HAS (hardware) | ABS | **HAS** — on-device cadence detection (NFPA 72 T3 / UL 2034 T4), no audio content retained |

### 3.4 Platforms & Apple ecosystem

| Surface | Ring | Wyze | Eufy | Reolink | SecuraCV |
|---|---|---|---|---|---|
| iPhone / iPad | HAS / stretched | HAS / stretched | HAS / stretched | HAS / stretched | HAS — real iPad sidebar idiom (not yet in the App Store; the pipeline is built, credentials pending) |
| Apple Watch | ABS (removed 2021) | ABS | ABS | ABS | **HAS** — full app + complications |
| Apple TV | ABS | ABS | ABS | ABS | **HAS** — native Witness Wall, CI-tested (store availability pending) |
| Mac | ABS (web only) | ABS | PARTIAL (web portals) | PARTIAL (aging client) | **HAS** — two native apps shipping today (Flasher, Lab) |
| Widgets / Live Activities / Dynamic Island | PARTIAL / ABS | PARTIAL / ABS | ABS / ABS | ABS / ABS | **HAS / HAS** |
| HomeKit | ABS | ABS | PARTIAL (2026 return) | ABS | PARTIAL-strong — sensor projection + automation concierge; HKSV refused by design |
| Siri / Shortcuts / Focus | ABS | PARTIAL | ABS | ABS | **HAS** — four App Intents + a real Focus filter |

### 3.5 Money & trust

| | Ring | Wyze | Eufy | Reolink | SecuraCV |
|---|---|---|---|---|---|
| What the full alerting/history stack costs | $4.99–19.99/mo | $2.99–19.99/mo | $0 + hub hardware | $0 local, cloud extras | **$0 forever, no account** |
| Resentment engine | price hikes, rename churn | features removed, +50% hike, ads | store inside the security app | thumbnails paywalled on a "no fees" brand | none priced; the honest caveat is store availability |
| Verifiable, not just claimed | E2EE guts the product | no E2EE | claims falsified 2022 | encrypted, not E2E | **chain verified client-side** — the phone and the TV recompute Ed25519 against pinned keys and refuse to say "Verified" for anything they didn't walk |
| Law-enforcement exposure | Axon portal (2026) | ECPA path | ECPA carve-out | none by default | none possible in the default path — nothing is hosted |

## 4. Where we already exceed — and should say so louder

Each of these is shipped code, and paywalled or absent at all four
competitors:

1. **$0/month, no account, structurally.** The episode collapse, 30-day
   history, away alerts, escalation, and second-person notification that
   competitors sell are free here *by local construction* — there is no
   server to meter.
2. **Delivery honesty nobody else has.** Confirmed-post-or-recorded-refusal,
   the heartbeat dead-man's-switch, coverage lanes, and a reach chip on
   every history row. The industry's #1 complaint — silent notification
   loss — is made legible instead of possible to ignore.
3. **The acknowledgment model.** Ack fans out across the owner's devices;
   a storm collapses to one summary whose ack answers every Canary it spoke
   for; escalation reaches a second person with nothing but a class word on
   the wire. Every competitor lacks this entirely.
4. **Apple-surface depth.** Watch app + complications, Lock Screen and Home
   Screen widgets, Live Activity/Dynamic Island with an episode policy, four
   Siri verbs, a real Focus filter, a native iPad idiom, native tvOS, native
   Mac apps. The segment's Apple story is open ground we already hold — with
   the one honest caveat that the iOS/tvOS apps are not yet store-installable.
5. **Verifiable rather than believable.** The client recomputes the chain.
   Eufy's collapse came from an unverifiable "local only" claim; our
   equivalent claim is checked on the buyer's own device, every refresh.
6. **Sensing without cameras.** Radar occupancy/breathing and Wi-Fi CSI
   presence with a privacy-class contract. None of the four has a
   no-camera sensing story at all.
7. **Honest status everywhere.** Demo banners, "as of" ages,
   stale-never-renders-fine, dashed-ghost concept devices, measured claims
   only. The exact inverse of the patterns that broke each competitor's
   trust.

## 5. Where we honestly lose (as of this research)

Named out loud, per non-negotiable #4:

- **Store availability.** None of the four requires a GitHub release page.
  Until the Apple pipelines get credentials, "we have the best watch app in
  the category" is true and unprovable to a normal user.
- **Onboarding.** Ring's pairing is best-in-class; our WAP pairing pastes a
  receipt JSON. Rooms/renaming have no UI; the first-run wizard is unbuilt.
- **Search.** Ring/Wyze ship natural-language history search (paid, cloud).
  We have filters-by-construction (coarse buckets, per-witness views) and
  deliberately no bulk-searchable index (Invariant VII) — but the honest gap
  is that even simple in-app history narrowing shipped only now.
- **Geofencing/modes.** All four have home/away modes; three have some
  geofencing. We deliberately hold no location permission and delegate
  composition to Shortcuts/Apple Home — right call, under-told story.
- **Sensor breadth in-market.** Leak/climate/keypad hardware exists at
  Ring/Wyze/eufy as shipping products; our equivalents are dictionary
  entries, designs, and firmware ahead of retail availability.
- **Claim-vs-code debts of our own** (auditor findings, tracked): the iOS
  README claims Secure Enclave key custody that is not implemented; the
  notification service extension performs no signature verification;
  CloudKit syncs the device list but not rules, so two of the owner's
  devices can disagree about what is armed.

## 6. The roadmap — match and exceed, from what we already have

Constraint: every item builds on wire surfaces, parsers, models, or seams
that ship today — no new firmware or kernel work. Items marked **(this
change)** landed with the change that added this document.

### Tier A — the sensor alerting + timeline story on iOS

- **A1 (this change). Life-safety acoustic vocabulary.** The firmware's
  acoustic chokepoint emits `smoke_alarm_t3`, `co_alarm_t4`, `knock`,
  `doorbell`, `glass_break`, `mic_muted`, `mic_unmuted`
  (`firmware/projects/canary-wap/arduino/canary_wap/acoustic_events_module.cpp`);
  the app's device dialect (`ios/Shared/EventVocabulary.swift`) did not know
  the names, so a heard smoke alarm rendered at the calm unknown-event
  fallback. Now: honest severities (smoke/CO alert; glass break warns;
  knock/doorbell/mic notices), per-event glyphs, and plain headlines. The
  mic-mute pair doubles as the audit trail eufy never had: the timeline
  shows when the device stopped listening.
- **A2 (this change). Detection class from the BLE v2 beacon.** The
  person/vehicle/animal/package class + confidence that Ring and Wyze sell
  as their core AI already rides our presence beacon
  (`FleetBeacon.detectClass`) and was dropped by the fold. Now folded into
  `Witness` and rendered on the device screen — live, broker-less, free.
- **A4 (this change). Per-witness history.** The one table-stakes row every
  competitor had and we lacked: the device detail screen now carries the
  day-shape ribbon and recent records for that one Canary, from the same
  ledger (nothing new is recorded).
- **A3 (partly landed — the rendering half). Light up the radar/wellbeing
  story.** `radarPresent`, `radarOccupants`, `breathingLock`, `tempC`,
  `humidityPct` were modeled on `Witness` and invisible; the device screen
  now renders them as a Wellbeing section (presence, an honest 2+-capped
  occupant count, breathing rhythm sensed/not, locale-aware temperature).
  The transport half stays open, stated precisely: canary-sense runs no
  HTTP listener — its readings ride retained MQTT only, which the phone
  deliberately doesn't speak — so the honest path is extending the shared
  `/api/fleet` self-report with optional coarse presence/occupants/
  breathing keys and letting the display (which already parses the radar
  MQTT state and serves `/api/fleet`) aggregate peer rows. Temperature and
  humidity have **no producer anywhere in the repo** yet and stay demo-only
  even after that — wiring them now would invent data.
- **A5 (landed — the vocabulary half). CSI/wellbeing event vocabulary.**
  `presence_changed`, `breathing_confirmed/lost`, `unusual_motion`,
  `unusual_breathing`, `daily_summary`, and the ribbon tick now land at
  chosen severities and honest sentences (anomalies against the device's
  own learned baseline warn; everything else stays everyday; "breathing no
  longer sensed" is worded as a sensing fact, never a medical claim), with
  a firmware-parity test reading every chokepoint module's `type_name`
  table off disk. The honest caveat, which applies to the acoustic dialect
  too: the chokepoint's event types reach the WAP's LAN-only
  `/api/csi/stream` and `/api/events/today` today, which the phone does not
  yet call — the vocabulary is the anti-rot half; the transport slice
  (polling those endpoints, or the witness-log payload surfacing them) is
  its own follow-up. Ring sells the anomaly-baseline version of this at its
  top tier.
- **A6. Per-type tamper narration.** Ten tamper kinds exist in the HA
  vocabulary (`custom_components/securacv/const.py`); no iOS transport
  populates `tamperKind` yet. When one does, map kinds to glyphs and
  sentences ("Power was cut", "SD card removed") — tamper already pierces
  every mute.
- **A7 (landed, deliberately narrowed). Modality as a device fact.** The
  scout found no payload the phone reads carries a per-event modality or
  attestation field (the witness record's signed preimage is frozen at
  seven fields), so per-row chips would have been a guess — and a per-row
  "device-attested" chip a constant, which is decoration. What shipped is
  the honest slice: a "Senses" row on the device screen (camera / 60 GHz
  radar / Wi-Fi sensing / contact) derived from the type the device
  publishes, pinned against `DEVICE_TYPE_MODALITY` in the HA integration's
  `const.py` — the map's wire-contract home — by a test that reads it off
  disk. Unmapped types get no row, never a guess. Per-event provenance
  waits on a versioned chain formula that could sign the field.
- **A8. HomeKit `low_battery` + class-scoped motion.** The bridge mirrors
  ten dictionary signals but derives four; battery and the beacon class are
  already in the model. "Person at the door turns on the porch light, app
  closed, no cloud" is one signal away.
- **C-tier follow-ups:** deep links from notification to the exact row; a
  tvOS Top Shelf extension; Chirp advert scanning beside the presence
  beacon. The vault's `frame_sealed` receipt **landed** with the wave-2
  vocabulary: "a frame was sealed" as a calm timeline fact (the acoustic
  trigger that caused it alerts on its own row) — capture that announces
  itself, the inverse of eufy's secret thumbnail.

### Tier B — ecosystem legibility ("from the Flasher to the Lab")

- **B1 (this change). The iOS app points at the family.** A "SecuraCV
  everywhere" section (Keys tab) names every surface and its one-sentence
  job with honest availability; the Fleet empty state now says where
  Canaries come from instead of dead-ending.
- **B2 (this change). The Flasher's Atlas gains the family.** The app that
  mints birth certificates "to match the one the iPhone will show" now
  tells users the iPhone app exists, and names its sibling Lab app.
- **B3 (this change). Dead ends offer our own tools before Chrome.** The
  browser flasher's no-Web-Serial fallback now leads with the native
  Flasher; Chrome stays as the in-browser path.
- **B5 (this change). The Flasher stops calling itself "the Lab".** The
  window title and splash said "SecuraCV Lab" while a separate SecuraCV Lab
  app ships — the exact naming churn users punish Ring and Reolink for.
- **B6 (this change). The Witness Wall bench points at the real TV app**,
  with honest status (built and CI-tested; store availability pending).
- **B7 (this change). The Glossary's surface roster gains the companion
  app**, and getting-started opens with the journey across surfaces.
- **B4. `ecosystem.json` — one machine-readable family map** (name, job,
  canonical URL, get-it-here, honest status), consumed by the Atlas, the
  Lab, and both Apple About panels, drift-gated in CI like
  `build-line.json`. The competitors' coherence rot is uncorrected drift;
  ours should be a tested artifact. This is the right long-term home for
  everything B1–B7 hardcodes today.
- **B8. Settle the Playground/Factory vocabulary** across
  `help_ecosystem_layout.md`, the Lab manifest, and the Atlas — and update
  that standard's iOS story, which still describes the pre-native plan.

## 7. What we will not adopt (with reasons)

- **Familiar-face suppression / face recognition** — the best anti-fatigue
  feature Nest and Ring ship is an identity substrate. Invariant II forbids
  the mechanism, not just the misuse.
- **Cloud retention ladders** — retention here is a product decision, not a
  price sheet.
- **AI-generated notification text** — the vocabulary is deterministic and
  CI-locked; a probabilistic sentence about a security event is a liability
  wearing a feature's clothes.
- **A live video wall** (on any surface, TV included) — Invariant I. The
  Witness Wall renders the verified record; that refusal is the product.
- **A neighborhood feed** — Ring's Neighbors is the cautionary tale. The
  Chirp channel remains PII-free templates between consenting devices.
- **Engagement mechanics** — no store tab in a security app, no promotional
  pushes in the alert channel, no ads in playback. Each is a documented
  resentment point at a competitor; the quiet app is the product working.

## Sources (curated)

The load-bearing subset; each section's full trail lives in the vendor's
support portal and the linked coverage.

- Ring plans & features: <https://ring.com/plans> ·
  <https://ring.com/support/articles/97l0i/Ring-Video-Descriptions-Single-Event-Alert-Beta> ·
  <https://ring.com/support/articles/7e3lk/Understanding-Video-End-to-End-Encryption-E2EE> ·
  <https://techcrunch.com/2024/10/09/amazon-revamps-ring-subscriptions-with-ai-video-search/> ·
  <https://www.scrippsnews.com/business/company-news/amazon-s-ring-announces-big-change-and-customers-are-not-happy>
- Ring privacy record: <https://www.ftc.gov/news-events/news/press-releases/2023/05/ftc-says-ring-employees-illegally-surveilled-customers-failed-stop-hackers-taking-control-users> ·
  <https://www.pbs.org/newshour/politics/ring-will-no-longer-allow-police-to-request-doorbell-camera-footage-from-users> ·
  <https://www.cnbc.com/2025/10/16/amazon-ring-cameras-surveillance-law-enforcement-crime-police-investigations.html> ·
  <https://www.eff.org/deeplinks/2026/02/no-one-including-our-furry-friends-will-be-safer-rings-surveillance-nightmare-0>
- Wyze plans & AI: <https://www.wyze.com/pages/service-plans> ·
  <https://www.wyze.com/blogs/smart-home/introducing-the-nbd-filter> ·
  <https://support.wyze.com/hc/en-us/articles/32693597570587-What-is-Descriptive-Alert> ·
  <https://support.wyze.com/hc/en-us/articles/46057417133851-Cam-Plus-Annual-Price-Increase-FAQ>
- Wyze incidents: <https://www.washingtonpost.com/technology/2024/02/20/wyze-camera-security-breach/> ·
  <https://www.consumerreports.org/home-garden/home-security-cameras/wyze-didnt-completely-fix-security-camera-flaws-for-3-years-a3726294358/> ·
  <https://ipvm.com/reports/wyze-leak>
- Eufy architecture & record: <https://www.techhive.com/article/1441481/eufy-security-issues.html> ·
  <https://www.androidpolice.com/eufy-removes-privacy-language/> ·
  <https://techcrunch.com/2025/10/04/anker-offered-to-pay-eufy-camera-owners-to-share-videos-for-training-its-ai/> ·
  <https://homekitnews.com/2026/01/06/eufy-is-back-with-a-trio-of-new-homekit-compatible-devices/> ·
  <https://service.eufy.com/article-description/Introducing-the-Cross-Camera-Tracking-Function-in-the-eufy-Security-App>
- Reolink: <https://support.reolink.com/articles/10883740871833-Introduction-to-Reolink-Rich-Notification-Function/> ·
  <https://support.reolink.com/articles/44949969778713-Introduction-to-Reolink-AI-Video-Search/> ·
  <https://reolink.com/blog/reolink-subscription-cost/> ·
  <https://community.reolink.com/topic/14811/timeline-scrubbing-needed-asap> ·
  <https://www.home-assistant.io/integrations/reolink/>
