# SecuraCV — Legal, Claims & Risk Audit

**Date:** 2026-07-23 · **Scope:** the `securaCV` code/hardware/firmware repo and the
`securacv_website` marketing site · **Operator:** Errer Labs (a solo maker) ·
**Prepared with:** Claude Code

> **This is not legal advice.** It is an engineering-grade risk map written by
> an AI assistant so a solo operator can spot the sharp edges, fix the cheap
> ones today, and take the two or three genuinely consequential decisions to a
> licensed attorney with the homework already done. Nothing here creates an
> attorney–client relationship.

---

## 0. Headline

SecuraCV is, for a solo project, **unusually well-behaved on risk**. It ships
Apache-2.0 with full warranty/liability disclaimers, a permissive-only
dependency license gate (no GPL/AGPL in the tree), an SBOM with SLSA/Rekor
provenance, a vulnerability-disclosure policy that promises no SLA it can't
keep, an explicit *forbidden-capabilities* list that bans face recognition,
plate OCR and audio transcription by design, and written house rules against
overclaiming (`AGENTS.md:182`, `README.md:157`, `BRAND.md:96`). Most
organizations ten times its size are messier than this.

So the real exposure is **not** that you are cutting corners. It is the
opposite failure mode plus one naming problem:

1. **A few absolute claims outrun what any device can promise** — "surveillance
   is architecturally impossible," "the code was never written," "tamper-proof,"
   "can't be bricked," "no one can." These are the classic FTC-substantiation
   and courtroom-cross-examination targets. They are also *unnecessary* — the
   honest version of each claim is nearly as strong and actually defensible.
2. **The product line is named "Canary"** — which is a live U.S. registered
   trademark **in this exact product category**. This is a bigger live risk than
   the "flock" word you already guard against.
3. **The "solo nonprofit / BBB A+" framing needs a reality check** — not because
   anything you've published is false (it isn't), but because the goal as worded
   isn't reachable for a one-person shop, and the model you're *actually* already
   building (a transparent fiscal host) is the better, trailblazing answer.

Severity legend: 🔴 **High** (fix before it bites) · 🟠 **Medium** (fix this
quarter) · 🟢 **Low / hygiene** (cheap, do when convenient). Each finding says
what I **changed now** vs. what **needs your decision**.

---

## 1. 🔴 High-severity findings

### H1 — "Canary" collides with a registered trademark in your own category

**What's there.** The camera line is named **Canary**, with a whole family —
`Canary Vision`, `Canary WAP`, `Canary Sense`, `Canary Vision Pro` — across
essentially every page and in `store.json`, `onboarding-spec.json`, firmware,
and docs. `TRADEMARK.md:14` asserts: *"The plain English word 'canary' is
nobody's property, ours included."*

**Why it matters.** **CANARY** is USPTO Reg. **#4,735,877**, owned by Canary
Connect, Inc. (rights now held by Arlo), **registered for home-security video
cameras with motion/temperature/humidity sensors** — your category, almost to
the letter. The same owner holds **CANARY FLEX**. Trademark rights don't attach
to a word in the abstract; they attach to a word *used on particular goods*. In
the abstract, "canary" is indeed free. **On a home-security camera it is not** —
that is precisely the use that is registered. So the sentence in `TRADEMARK.md`
is the single most legally wrong statement in either repo: it's true in general
and false in exactly the context you use the word.

The test for infringement is *likelihood of confusion* (similar mark + related
goods + overlapping buyers). "SecuraCV Canary," a privacy security camera,
against "Canary," a home security camera, scores high on every factor. The
`SecuraCV` prefix helps but does not cure it — courts routinely find "house mark
+ someone else's mark" still confusing.

**This needs your decision** — three honest paths, cheapest first:
- **(a) Get a real clearance opinion.** ~$1–3k with a trademark attorney buys a
  formal likelihood-of-confusion read and, if it clears, a paper trail that
  rebuts *willfulness* (which is what drives enhanced damages). This is the
  minimum I'd do before scaling sales or filing your own mark.
- **(b) Reposition "canary" as descriptive, not a brand.** Stop using it as a
  product *name* and use it only as a metaphor ("a canary in the coal mine for
  your space") with the real product names being `SecuraCV Vision`, `SecuraCV
  WAP`, `SecuraCV Sense`. Much of your copy already leans on the metaphor; this
  is the lowest-cost structural fix.
- **(c) Rename the line.** Highest cost, lowest residual risk. Only worth it if
  (a) comes back unfavorable.

**Changed now:** I did **not** rename anything (that's your call) but the audit
flags `TRADEMARK.md:14` for correction — see the recommended edit in §6. Do not
publish the "nobody's property" sentence as-is; it reads as *notice* that you
considered and dismissed the conflict, which is unhelpful if it ever surfaces.

---

### H2 — Absolute / impossibility claims (FTC substantiation + evidentiary cross-exam)

**What's there (representative, not exhaustive):**

| Where | Claim |
|---|---|
| `index.html:3561` | "Six mechanisms that make surveillance **architecturally impossible**." |
| `index.html:3004`, `:3102`; `llms.txt:5` | "That code **doesn't exist**… the capability **was never written**." |
| `compare.html:224` | "Ours is **architecturally incapable** of it." |
| `how-it-works.html:783`; `vault.html:87,428,453` | "**no one can** forge / open / edit." |
| `firmware_ota.md:4` | "why it **can't be bricked** or fed a forged image." |
| `docs/security/SECURITY_MODEL.md:16` | "creates **tamper-proof** records" — *and this doc ships in every evidence export.* |
| `docs/security/SECURITY_MODEL.md:177` | "Records **cannot be modified** … **cannot be deleted**." |

**Why it matters.** Two distinct exposures:

- **Advertising law (FTC Act §5).** Objective product claims need a *reasonable
  basis* before you publish them. The FTC actively enforces against deceptive
  privacy/security representations and, in 2023, warned ~700 firms that
  unsubstantiated product claims invite civil penalties. Absolutes are the
  easiest claims to falsify: a single reachable code path, config, or CVE that
  contradicts "the capability was never written" or "architecturally impossible"
  converts confident copy into a documented misrepresentation.
- **Evidence use.** `SECURITY_MODEL.md` is handed to *lawyers and journalists in
  every export*. Calling the output "tamper-proof" in that document invites the
  obvious cross-examination ("so it is *impossible* to alter? Let me introduce
  this…"). One demonstrated edge case discredits the whole exhibit.

**The good news: you already know the answer.** `README.md:108` and
`BRAND.md:35` state the rule explicitly — *"The promise is not that tampering is
impossible. It's that tampering becomes visible."* The fix is to make the
shipped docs and the website obey your own rule. The honest phrasings are barely
weaker and are *defensible*:

- "tamper-proof" → **"tamper-evident"** (this is the correct term of art, and
  the very next clause already says "any alteration or deletion is detectable").
- "surveillance is architecturally impossible" → "the design has **no code path
  that** produces face/plate/identity output — [see the forbidden-capabilities
  list]."
- "cannot be modified" → "**any** modification **breaks the chain and is
  detectable**."
- "can't be bricked" → "an A/B partition with signed images and **automatic
  rollback**, so a failed update returns to the last good firmware."
- "no one can forge" → "forging a record requires the device's private key,
  which never leaves the secure element."

**Changed now:** I aligned the highest-stakes instances — the "tamper-proof"
language in the shipped **evidence** document (`SECURITY_MODEL.md`) and in
`THREAT_MODEL.md` — to **"tamper-evident,"** matching your own README/BRAND
rule. See §6. **Needs your decision:** the website marketing absolutes are brand
voice; I've listed the swaps above but did not rewrite your headlines
unilaterally. Recommend doing them in one editorial pass.

---

### H3 — "Evidence" / "courtroom-grade" framing + the always-listening mic

**What's there.**
- `linux.html:556` — "add real cameras and **courtroom-grade evidence**."
- Pervasive "evidence," "sealed evidence," anti-subpoena framing across
  `canary.html`, `vault.html`, `index.html`.
- The Canary mic is described as **"always listening"** (`getting_started_canary.md:136`)
  even though it only extracts a loudness envelope and *"does not record audio.
  Ever."* (`:154`). There is **no user-facing recording-consent / wiretap notice
  anywhere.**

**Why it matters.**
- **"Courtroom-grade"/"admissible" implies a legal conclusion the device cannot
  guarantee.** Admissibility depends on jurisdiction, foundation, chain of
  custody, and a judge — not on the hardware. Promising it is both
  unsubstantiated and the kind of claim a defense attorney loves to blow up.
- **Recording-consent (wiretap) law.** Even a mic that only computes RMS can
  trip state all-party-consent statutes (CA, IL, PA, WA, etc.) in strict
  readings, and "always listening" is a phrase that invites the question. Your
  *technical* defense is strong (no oral communication is captured or
  recoverable — "speech is structurally impossible to recover," `:158`), but
  you publish **no consent guidance** telling buyers that pointing a
  witness/mic device at other people can carry legal duties **they** own.
- **Biometric statutes (BIPA/CCPA).** You're clean today — biometrics are
  *forbidden by design* — and your own pose-estimation strategy doc
  (`strategy/14:113`) correctly refuses skeleton/gait as "a biometric identity
  substrate." Keep that line bright; it's a genuine differentiator.

**Needs your decision (recommended):**
- Downgrade "courtroom-grade evidence" → "**tamper-evident records designed to
  support evidentiary use**" (or similar), and add a one-liner: *"Whether any
  record is admissible is decided by a court, not by us."*
- Add a short, friendly **"Recording other people — know your local law"**
  notice to the buyer-facing docs and the store: a witness/camera near shared
  spaces or audio may require notice or consent where you live; you (the
  operator) are responsible for lawful placement. This *reduces* your liability
  by shifting the well-understood duty to the user, the way every reputable
  camera vendor does.
- Reword "always listening" → "**always alert for two alarm tones**" — accurate
  and much less alarming.

---

## 2. 🟠 Medium-severity findings

### M1 — You sell physical hardware, but only the *software* has terms
`store.html:197` promises "DOA hardware replaced, 30-day returns," and
`store.json` lists real prices ($15–$239) with "save $9/$18" discounts. The
Apache-2.0 "AS IS" disclaimer covers **code**; it does **not** cover **goods**.
Selling tangible kits pulls in implied warranties (merchantability/fitness under
UCC Art. 2, and Magnuson-Moss if you label any warranty). **Recommend:** a short
**Terms of Sale** page — who the seller is, the 30-day/DOA policy stated as your
*limited* remedy, an "as-is beyond that, no liability for installation or
consequential loss" disclaimer, and a note that print-it-yourself kits carry no
warranty at all. One page closes the gap.

### M2 — "Nothing phones home / never leaves your device" is broader than the wiring
The strong data-locality claims (`corps.html:169`, `strategy/18:106`,
`canary-local/README.md:73`) are true for **footage and identity** but not
literally for **all traffic**: MQTT publish to Home Assistant is **on by
default** (`config.yaml:69`), firmware does a **daily HTTPS OTA manifest check**
(`firmware_ota.md:28`), and RFC-3161 timestamping is an opt-in outbound call.
**Recommend:** scope the copy once — *"No footage and no identity ever leave the
device. The only outbound traffic is a signed update check and the events you
publish to your own hub."* That sentence is airtight; "nothing phones home"
isn't.

### M3 — Donations & the "fiscally-hosted community fund" (get the plumbing real before the copy goes live)
`request.html:183`, `ecosystem.html:223` describe a "community donation pool"
and a "fiscally-hosted community fund with a public ledger." The repo's strategy
names the intended host — **Open Source Collective** (`strategy/19:172`).
**Two things to keep clean:**
- **Solicitation.** Once you actively ask the public for donations, ~40 U.S.
  states have charitable-solicitation registration regimes. A **fiscal sponsor
  (OSC)** is exactly the shelter a solo maker uses to accept funds transparently
  *without* forming your own 501(c)(3) — but the relationship must be **live and
  named** before the copy implies it. Today it reads present-tense but is
  strategy-stage.
- **Never imply tax-deductibility.** You currently don't (good). If donations go
  live under OSC, one line — *"Errer Labs is not a 501(c)(3); contributions are
  not tax-deductible unless made through our fiscal host, [OSC]"* — removes all
  doubt.

### M4 — Entity disclosure & a mixed copyright signal
- The only contact is a **personal Gmail** (`errerlabs@gmail.com`) with **no
  physical/mailing address** anywhere. CCPA/GDPR both assume a reachable
  controller; a role address (`privacy@…`) and a mailing address (even a PO box)
  are the norm.
- **"© Errer Labs. All rights reserved."** sits next to an **Apache-2.0** grant
  on nearly every page. That's a mixed message — you are in fact granting broad
  rights. This is the flip side of the audit: here you *under-claim* your own
  openness (see §4). Use "Licensed under Apache-2.0" and drop "All rights
  reserved," or "Some rights reserved."
- Copyright holder is inconsistent: `engine.html:434` says "© SecuraCV" while
  everywhere else says "© Errer Labs." Pick one (Errer Labs is the legal name).
- Consider naming the **legal form** once (sole proprietor? single-member LLC?).
  An **LLC** is the single highest-leverage protection for a solo operator
  selling hardware — it caps personal liability. Worth the ~$100–800 to form.

### M5 — Two conflicting trademark policies in one repo
`TRADEMARK.md` (root, generous, ESPHome-style) vs.
`firmware/projects/canary-vision/TRADEMARK.md` (older, stricter, asserts
domain/social-handle marks). Conflicting published policies undercut both.
**Recommend:** delete/redirect the firmware copy to the root policy.

### M6 — Health-adjacent claims (breathing / heart-rate / fall detection)
`watch-over.html` markets mmWave "breathing and heart readings" and
`llms.txt:22` says "healthcare fall detection." You have the **right disclaimer**
already (`watch-over.html:273`: *"not a medical device, and not a substitute for
emergency services or in-person care"*) — keep it, and make sure it sits
**adjacent to every** physiological claim, not just once. Avoid any wording that
implies diagnosis, monitoring of a medical condition, or reliance for
life-safety (that's what pulls a product toward FDA/UL scrutiny).

---

## 3. 🟢 Low-severity / hygiene

- **L1 — No export-control note despite strong + post-quantum crypto**
  (ChaCha20-Poly1305, Ed25519, ML-KEM/ML-DSA behind flags). As
  publicly-available open source you almost certainly qualify for the EAR
  §742.15(b) open-source carve-out, but the customary step is a one-time **email
  notification to BIS/NSA** and a short `ENCRYPTION.md` noting it. Cheap; do it
  before broad international distribution.
- **L2 — Competitor comparison accuracy & dating.** `compare.html` lists
  competitor prices and rates Eufy "historically murky." Nominative comparison
  is legal, but keep it **factual, sourced, and dated** ("prices as of <date>")
  — comparative claims are the ones competitors complain about. The
  `witness-fingerprints.json` device-fingerprint catalog is the most aggressive
  use of competitor marks; keep it framed as interoperability research (it is)
  and factual.
- **L3 — Privacy policy is stale & names no data-protection specifics.**
  `privacy.html` is dated **"January 24, 2025"** (18 months old). Refresh the
  date, confirm the analytics description still matches
  `analytics-server.js`, and add the mailing address from M4.
- **L4 — "Sarah Chen (94%)" mock.** `index.html:3247` uses a realistic
  personal name + a fabricated "94%" accuracy figure in a *simulated
  surveillance* readout. It's clearly illustrative, but swap to an obviously
  fictional label (e.g. "SUBJECT_04 · 94% ⚠ illustrative") so no one can quote
  it as a real benchmark or a real person.

---

## 4. Where you are *over*-protected (you asked — relax these)

You specifically didn't want to "sound over-protected." You mostly don't; your
honesty rules are a feature. The genuine over-caution / mixed-signal spots:

- **"All rights reserved" beside Apache-2.0** (M4) — you are giving people broad
  rights; the line tells them the opposite. Loosen it.
- **The stricter firmware `TRADEMARK.md`** (M5) — it's more restrictive than
  your real, generous policy. Kill it.
- **Don't over-disclaim into invisibility.** The "prototype — verify before
  relying" footers are honest and *good*; keep them. The move is not fewer
  disclaimers, it's *right-sized* ones: strong where money/evidence/safety are
  involved (M1, H3, M6), light everywhere else.

Net: your problem is 90% *over-claiming* on capability and 10% *over-reserving*
on rights. Fixing both makes you sound **more** confident, not less — because
every remaining claim will be one you can defend.

---

## 5. The "solo nonprofit / BBB A+" goal — a reality check and a better target

You want to run this like the best-rated nonprofit — "BBB A+." Two honest
clarifications, then the model I'd actually chase:

- **"A+" and "BBB Accredited Charity" are different programs.** "A+" is the
  BBB *business* letter grade. Charities are evaluated by **BBB's Give.org**
  against the **20 Standards for Charity Accountability** (pass/fail, not a
  letter). Conflating them is common but worth getting right in your own head.
- **A solo operation structurally cannot meet the charity standards.** Standard 1
  requires a **governing board of at least five voting members**, meeting **≥3×
  a year**, expressly *"to avoid power being concentrated in the hands of one or
  two people."* Others require **≥65% of expenses on program / ≤35% on
  fundraising**, audited financials, and an independent conflict-of-interest
  policy. None of that is reachable — or honest — for a one-person shop, and
  **claiming** charity/nonprofit/tax-exempt status without it is itself the
  legal risk. (You currently make no such claim — that's the right call.)

**So don't cosplay a charity. Do the thing that actually earns the same trust,
which you're already halfway to building:**

1. **Transparent fiscal host, public ledger** (your `ecosystem.html` plan via
   Open Source Collective). This is the *legitimate* way a solo maker handles
   money in the open — money flows vendor → collective → maker, never
   customer → founder. Ship it for real, then the copy is true.
2. **Adopt the charity standards you *can* meet, voluntarily, and publish that
   you do:** truthful solicitations, accurate website, donor-privacy, a
   complaints channel, and an **annual transparency report** (finances in,
   spend out, what shipped). Borrowing the *communications* standards is
   trailblazing for a solo project; borrowing the *governance* ones you can't
   staff is not.
3. **Form an LLC** (M4) so "the project" and "the person" are legally distinct —
   the single most protective, most professional move available to you.
4. If you ever want the real nonprofit halo, the path is a **≥3-person board +
   501(c)(3) or a fiscal-sponsor umbrella** — a deliberate later step, not a
   label to adopt now.

The trust you're reaching for comes from **verifiable transparency**, which is
your whole thesis anyway. Lean into that, not into a badge you'd have to
overstate to wear.

---

## 6. What the "never-rot / self-updating / Claude + GitHub" model needs to stay safe

Your self-healing, auto-updating, AI-operated pipeline is a genuine
differentiator. Three guardrails keep it trustworthy rather than a liability:

- **A human owns every outward-facing change.** An automated "always up to date"
  system that can push copy, prices, or firmware is only as safe as its review
  gate. You already do this well (`engine.html:337`: "the number on the store
  page is a human decision, on the record"; signed OTA with human-gated keys).
  **Keep price, legal, and safety-critical files behind a human approval** even
  when everything else is autonomous. Never let the pipeline auto-publish a
  *claim*.
- **"Never rots" is a maintenance posture, not a warranty.** Much of it is
  correctly *delegated to Home Assistant OS* and is design-stage
  (`raspberry_pi_hub_flashing.md:3`). Keep it **future-tense** in public until
  hardware-validated, per your own `BRAND.md:103` rule. "Designed to stay
  current for years" is safe; "never rots" as a present-tense guarantee is not.
- **Signed, rollback-safe, logged** — which you have (Ed25519 manifests, A/B
  rollback, Rekor transparency log). That triad is exactly what turns "auto-
  update" from a risk into a selling point. Document it once in plain English on
  the site so buyers see *why* auto-update here is trustworthy.

---

## 7. Prioritized action checklist

**Applied in this change (safe, intent-aligned):**
- [x] `SECURITY_MODEL.md` / `THREAT_MODEL.md`: "tamper-proof" → **"tamper-evident"**
      (aligns the shipped evidence doc with your own README/BRAND rule). *(H2)*
- [x] This audit report, committed to `docs/` for the record. *(all)*
- [x] Companion claims memo in the website repo (`docs/claims-audit-2026-07.md`). *(H2/H3)*

**Recommended edit to `TRADEMARK.md:14` (needs your OK — it's a policy line):**
> Replace *"The plain English word 'canary' is nobody's property, ours
> included…"* with something that doesn't waive the point:
> *"'Canary' is a common word, but note that others hold trademark rights in
> 'CANARY' for security cameras; we use it only in combination as 'SecuraCV
> Canary,' and are reviewing our long-term naming. Third parties should not rely
> on this document as clearance to use 'Canary' for camera products."*

**Your decisions (highest leverage first):**
1. **H1 — the "Canary" name.** Clearance opinion, reposition as metaphor, or
   rename. Don't scale sales until this is settled.
2. **H2 — one editorial pass** swapping website absolutes for the defensible
   phrasings in §1 (H2 table).
3. **H3 — add** a recording-consent notice + soften "courtroom-grade."
4. **M1 — add** a one-page Terms of Sale for the store.
5. **M4 — form an LLC**, add a mailing + role email, fix the copyright line.
6. **M3 — stand up the OSC fiscal host** before the donation copy goes live.
7. **M2 / L1–L4** — copy scoping, export note, privacy-policy refresh, mock-data
   relabel — cheap, batchable.

**Bottom line:** you're not over-protected and you're not cutting corners. Trim
a handful of absolutes down to the (nearly-as-strong) truth, settle the Canary
name, put a liability wrapper around the hardware sales and the person, and make
the transparency you already believe in the thing you're rated on. That's the
trailblazing version — and it's mostly editing, not rebuilding.
