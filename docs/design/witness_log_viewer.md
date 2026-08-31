# The Witness Reading Room — log viewer + verifier — scope

Status: **Draft (scoping RFC)** — no code yet; this document decides what to
build, on which foundations, in what order.
Intended Status: Informative (product + architecture scope)
Last Updated: 2026-07-25
Companion docs: [c2pa_export.md](c2pa_export.md) (the C2PA layer this views),
[`viewer/`](../../viewer/) (the existing offline verifier this extends),
[evidence_lifecycle.md](../evidence_lifecycle.md).

> One sentence: one application where a person — owner, trustee, court clerk,
> journalist — can **verify everything** about a witness record (hash chain,
> signatures, export receipts, C2PA Content Credentials) and then actually
> **read it**: a timeline of events, the disclosure audit trail, and the
> chain's health, with the verification verdict always on screen.

## 1. Why this tool

Verification today is real but *terminal-shaped* (`export_verify`,
`log_verify`, `envelope_verify`), and reading the data is scattered: the HA
timeline card shows live events, the offline HTML viewer verifies an
envelope but barely displays it, and an export bundle — the thing you hand
to a third party — has **no reader at all**. The moment C2PA sidecars ship
(PR #1247), the gap widens: we produce evidence any Content Credentials tool
can check, but offer no first-party place to *look at* what was disclosed.

The unit of trust this tool serves: **"here is a file someone gave me — is
it real, and what does it say?"** That question must be answerable by
someone who is not the device owner, has no toolchain, and may have no
internet.

## 2. What already exists (build on it, don't duplicate it)

| Piece | What it gives us | Gap |
|---|---|---|
| `viewer/evidence_viewer.html` + `verify_core.js` | Offline, dependency-free, single-file browser verifier: Ed25519 + full hash chain + canonical JSON, **byte-parity-tested against the Rust verifier** (`tests/fixtures/envelope/*`) | Verifies, but barely *shows*; no export-bundle browsing; no C2PA awareness |
| `export_verify --c2pa-manifest` (Rust) | Full verification incl. C2PA `Trusted` + witness cross-binding | CLI only |
| Event API (`witness_api`): `/events`, `/events/latest`, `/digest`, `/status`, `/export/bundle`, `POST /verify` | Token-gated live access to the running kernel | No UI consumes it except HA |
| Lovelace timeline card | Live ✓-badged events in Home Assistant | HA-only; not for third parties |
| `desktop/` (Tauri: Flasher/Lab) | Proven native-app patterns, Rust core reuse | No viewer role today |

## 3. Product shape: one reading room, three surfaces

The same mental model everywhere: **verdict bar on top, record below.**
Verdict = three lights, each honest about what was and wasn't checked:

1. **Chain** — receipts hash-chained, signatures valid under the device key
2. **Binding** — file bytes match the receipt the chain committed to
3. **Content Credentials** — C2PA sidecar validates (and to which anchor)

### Surface A (P1): the offline Reading Room — extend `evidence_viewer.html`

Target user: the person handed `export.json` (+ `export.json.c2pa` +
`device_ca.pem`). Works from a double-clicked HTML file, air-gapped,
nothing to install. Scope:

- **Inputs**: drag-drop / file-pick the bundle, optional sidecar, optional
  anchor PEM, optional device public key — with a "what are these files?"
  explainer for non-technical recipients.
- **Verify** (reusing `verify_core.js`, extended): export-bundle receipt +
  artifact binding (already parity-serialized in `verify_core.js`), chain
  checks, and the C2PA sidecar's **witness-binding consistency** (parse the
  JUMBF far enough to surface the `org.securacv.witness` assertion and
  check it names this bundle's receipt + artifact hash).
- **Honest C2PA limit in v1**: the browser does *not* do full COSE/X.509
  validation (that would mean vendoring the CAI WASM SDK or hand-rolling
  cert-chain crypto — the latter is banned, the former breaks the
  single-file zero-dependency property that makes this viewer trustworthy
  and auditable). The C2PA light in v1 shows **"present, binding consistent
  — signature verifiable with `export_verify` or any Content Credentials
  tool"**, never a fake green. Full in-browser C2PA is a P3 decision
  (§6).
- **Read** — the new half. Three views over the decoded bundle:
  - **Timeline** — ✅ **shipped** (see "The timeline scrub view" below):
    events grouped by 10-minute bucket → zone → event type, with
    confidence; failures/gaps rendered inline, not hidden. (The privacy
    coarsening is a *feature* to display: "times shown are 10-minute
    buckets with deliberate jitter" as a first-class caption.)
  - **Disclosures**: the export-receipt trail — when, under which
    authorization (`self_export` vs `break_glass`), window, artifact hash —
    the "who has seen what" audit.
  - **Chain health**: entry counts, checkpoint coverage, key fingerprint
    (randomart, matching the serial console's `l` banner), kernel/ruleset
    versions.
- **Ship it**: stays a committed, built single file (`build.mjs` template
  pipeline + parity tests as today), distributed via the Lab and linked
  from every export ("view this file at …" printed by `export_events`).

### The timeline scrub view (shipped) — "the day has a shape"

The first piece of Surface A's reading half. A flat table gave every row the
same weight, so the only way to learn what kind of day it was, was to read
all of it. The scrub view borrows Sublime Text's minimap instead: every alert
is drawn as one greeked stroke — width from its label, indent from its zone —
so a day of alerts has the silhouette of a page, and the words are read only
where the shape asks for them.

| Part | What it does |
|---|---|
| Minimap | Drag to scrub; the reading pane follows and a lens reports the bucket under the cursor. Recurring zones line up into columns. |
| Fold pleats | An hour or more of quiet collapses to a fixed pleat (Sublime's code folding), reporting the heartbeats sealed inside it — "quiet, with proof of watching". |
| Day header | A density strip, one cell per bucket, **worst status winning outright** so a busy hour never averages away the one thing in it that mattered. |
| Reading pane | The rows themselves — event, declared gap, or system-trace record — with the bucket range, zone, and confidence. |

Four properties are load-bearing, and a change that breaks one is a bug, not
a restyle:

1. **A declared gap can never be folded.** Quiet folds; a sealed failure
   record is an anchor. The one thing a witness log must not do is hide the
   times it could not see.
2. **The scrubber snaps to buckets**, and says so on screen. Finer timing was
   never recorded (Invariant III), so offering a position the record cannot
   justify would be a privacy regression wearing a UI hat.
3. **Uncovered is not quiet.** Hours outside the export's window render as
   absent, never as a flat "nothing happened".
4. **Unparseable entries are counted, not dropped.** The caption says how
   many, so the picture never quietly omits part of the ledger.

**One model, two languages, one fixture.** The arithmetic lives in
`viewer/timeline_core.js` (DOM-free, `node --test`-able, inlined into the
built viewer) and is ported to `ios/Shared/TimelineScrub.swift`
(Foundation-only). Both are asserted against
`viewer/fixtures/timeline/scrub_parity.json`, generated from the JavaScript by
`viewer/tools/gen_timeline_parity.mjs` and checked in CI on both sides. This
is the §4 "one timeline model" rule made mechanical: two surfaces that fold
quiet differently disagree about what a day *was*, and then the record stops
being a record. **Changing a folding or layout rule is a three-part commit:
both implementations plus a regenerated fixture.**

**The interaction grammar (the polish pass).** The bucket is a detent, not a
limitation, and every layer agrees on it: the web lens magnetizes to the
nearest inked bucket (reachability, zero extra precision — it always names
the full bucket range), the iOS ribbon ticks a selection haptic per record
bucket and a firmer nudge entering a declared gap or tamper bucket (never
scaled by severity — a scrub must not become a Geiger counter), and pulling
the finger down slows the drag Music-scrubber-style without ever offering a
sub-bucket position. Every scrub ends with an acknowledgment (the landing
row flashes; the iOS caret lingers while the list glides), tamper is exempt
from the minimap's density cap exactly as gaps are, `n`/`p` step between
tamper and gap records and the day-header counts jump to them — navigation
order, never a filtered view — and the spotlight is a visible mode: the
caption line names it, Escape clears it, and switching it on brings the
first match into view, because "dimmed, not hidden" must never look like
"gone".

The second pass made the worst records the easiest to reach and the map
fully productive: a tamper or declared-gap tick within a few pixels catches
the scrub magnetically (one resolver feeds the scrub target, the lens, and
the landing flash, so they can never disagree; the held snap is announced by
a ring on the lens), the row's viewport alignment mirrors the pointer's map
position so no region of the map is a dead zone, long jumps teleport most of
the way and glide only the last viewport with the flash paid out on arrival,
and the day's edges rubber-band on iOS — but never at declared-coverage
edges, where uncovered time must stay freely scrubbable. VoiceOver gets the
day as an Audio Graph: recorded buckets as one series, declared gaps as a
second named series, and absent buckets as absent data rather than zeros,
because this feed declares no coverage.

On the phone the same model drives a horizontal day-shape ribbon in the
Alerts tab (`ios/Sources/SecuraCV/Views/Components/TimelineScrubView.swift`),
colored by the app's existing `Severity` → `Theme.Role` vocabulary rather
than a second color language, and scrubbing the list beneath it.

`ios/Shared/TimelineScrub.swift` deliberately carries **no**
`SecuraCV-Parity` marker: the tvOS Wall has no time-bearing event data to
draw — sealed entries carry only coarse buckets, and while the kernel now
serves `/api/sealed-log` (token-gated), the TV holds no token yet — so
shipping it there would mean inventing a clock the record does not have.
When that data reaches the TV, the marker plus a
`tvos/WitnessWall/project.yml` entry is the whole port.

### Surface B (P2): live viewer on the hub — feed the same UI from `witness_api`

Same reading-room UI, but pointed at the running kernel instead of a file:
the event API already serves `/events`, `/digest`, `/status`,
`/export/bundle`, and `POST /verify`. P2 packages the Surface-A views as a
small SPA served locally (token-gated, LAN-only, same zero-external-fetch
CSP as the website), adding: live tail, "verify now" button, and one-click
"export + C2PA + download the three files" so the handoff to a third party
is a single gesture.

### Surface C (P2/P3): native desktop for full-fat verification

A `desktop/`-family Tauri app (or a mode of the existing Lab app — decision
in §6) that reuses the **Rust** kernel + `c2pa` crate directly: full C2PA
`Trusted` validation with anchor management, opening a `witness.db`
read-only (seed-derived SQLCipher key) for owners, and batch-verifying a
folder of scheduled exports. This is where the C2PA light goes fully green
offline with no browser limitations.

## 4. Non-goals (all surfaces)

- **No raw media.** The reading room reads *semantic events*; PEEK frames
  and sealed-vault evidence stay in their existing quorum-gated flows.
- **No cloud, no accounts, no telemetry.** Surface A runs from a local
  file; Surface B is LAN + token; nothing phones home (Principle 2).
- **No editing.** Read-only by construction; the only "write" anywhere is
  Surface B's export button, which goes through the normal receipted path.
- **No second verifier implementation.** Browser surfaces extend
  `verify_core.js` (parity-pinned); native surfaces call the Rust kernel.
  Nothing new interprets the formats by hand.

## 5. Suggested build order

1. **P1a — bundle reader**: `evidence_viewer.html` accepts an export
   bundle, shows verdict bar + Timeline + Disclosures views. (Verification
   logic for bundles already exists in `verify_core.js`; this is mostly UI
   + the bundle→view decode.)
2. **P1b — C2PA awareness**: sidecar input, witness-binding consistency
   check, honest C2PA light, `export_events` prints the "view this at…"
   pointer.
3. **P2 — live mode**: same views over `witness_api`, served token-gated
   from the hub container; one-gesture verified export.
4. **P3 — native full verification** per §6 decisions.

Each step is independently shippable and none blocks the others' users.

## 6. Open questions (decide before P2/P3)

1. **Native home**: new Tauri app vs. a "Viewer" tab in the existing Lab
   desktop app vs. extending `canary-vision`? Leaning: Lab tab — the
   Flasher/Lab already owns "the native SecuraCV app on your desk" and a
   third app dilutes that.
2. **Full C2PA in the browser**: vendor the CAI `c2pa-js` WASM (≈ MBs,
   ends the single-file zero-dep era, but CSP-safe since it's embedded) vs.
   keep full validation native-only. Leaning: keep the single-file viewer
   pure; revisit if third-party recipients actually ask for in-browser
   COSE verification rather than using public CC tools.
3. **Bundle format hint**: should `export_events` embed a tiny
   `viewer_hint` field (viewer URL + minimum version) in the bundle so any
   future reader can self-describe? (Spec change — needs the same
   invariants review as any envelope field.)

## 7. Definition of done (P1)

- A non-technical recipient can open one HTML file, drop in the three
  export files, and see: green/amber verdict lights with plain-language
  explanations, the event timeline, and the disclosure trail — all offline.
- Verdict semantics are byte-parity-tested against `export_verify` on the
  same fixtures (extend `tests/fixtures/` with a bundle + sidecar pair,
  including tamper cases).
- The viewer still builds reproducibly (`node viewer/build.mjs` idempotent
  in CI) and still contains zero external fetches.
