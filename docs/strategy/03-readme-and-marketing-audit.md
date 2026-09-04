# 03 — README & Marketing Audit

## What's strong (keep it)

- **Coherent, original thesis.** "A security camera with a 24-hour memory… the only thing
  that persists is a tamper-proof log" and "witnessing, not watching" is a genuinely
  differentiated, memorable pitch. It runs consistently across every README and the
  philosophy docs.
- **Honesty.** The repo documents its own threat model and limits (`docs/root_paradox.md`,
  `spec/threat_model.md`) and `CONTRIBUTING.md` openly rejects feature creep. This builds
  trust with the technical audience that will adopt first.
- **Real install path.** A 3-step HA install and a `cargo run --bin demo` tamper demo both
  exist and are concrete.

## What's weak (the rewrite fixes these)

1. **The README is a 570-line wall that mixes two audiences.** The top is a consumer pitch;
   then it dives into break-glass CLI flags, RTSP/V4L2/ESP32/Tract build instructions, and the
   full HA entity catalog. A prospective user and a kernel hacker need different documents.
   → *Fix:* tighten the top fold to the pitch + install; move deep CLI/ingest reference into
   `docs/` (most already have homes: `docs/rtsp_setup.md`, `docs/v4l2_setup.md`,
   `docs/esp32_s3_setup.md`, `docs/container.md`) and a new `docs/operator_guide.md` for the
   break-glass/export CLI walkthroughs.

2. **No "show, don't tell."** There is a logo GIF but no screenshot of the actual product —
   the verified ✓ events, the timeline, the daily digest. The single most persuasive asset
   (a tamper-evident event with a green check) is missing from the pitch.
   → *Fix:* reserve a screenshot slot near the top; ship a real timeline/verification image
   when the UI exists (tracked in [06](06-feature-prioritization.md) / [07](07-timeline-events-privacy-design.md)).

3. **Credibility wobble: v1 status is inconsistent.** `CHANGELOG.md` lists v1.0.0 as
   "Unreleased," `v1-roadmap.md` still has unchecked acceptance boxes, yet docs read as if
   shipped. New visitors notice this.
   → *Fix:* state status plainly in the README (e.g. "Status: pre-v1, core works end-to-end")
   and align CHANGELOG/roadmap so they don't contradict each other.

4. **The "why" is buried.** The best persuasion asset, `docs/why_witnessing_matters.md`, isn't
   surfaced from the top of the README.
   → *Fix:* add a one-line "Why this exists" link near the top fold.

5. **No "Who it's for."** The README never says who should adopt this, so each visitor has to
   self-sort across three very different audiences.
   → *Fix:* add a short "Who it's for" section reflecting the recommended lead segment
   (privacy-conscious prosumers first; at-risk/evidence users as the deeper why).

6. **Duplicated section headers.** The body restarts ("# Witness Kernel", "## Quickstart")
   as if two READMEs were concatenated.
   → *Fix:* single narrative; dev quickstart folds under one "Building from source" section.

## What the README rewrite does (Deliverable 2)

- Top fold: logo → one-line value prop → 3 "why" bullets (no subscription / private by design /
  tamper-evident) → screenshot slot → 3-step install → "Who it's for" → "Why this exists" link.
- Keep both "How it works" sections (normal people / engineers) + the data-flow diagram +
  canonical-spec links.
- Move the long CLI/ingest reference into `docs/` with short blurbs + links in their place.
- Add an honest one-line status badge so the v1 ambiguity stops undercutting trust.

The voice stays exactly as-is — calm, precise, "witnessing not watching." The goal is only to
let the right reader find the right depth fast.
