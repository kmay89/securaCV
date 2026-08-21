# The automated Help Desk — no humans in the loop, no dead ends

**Status: Phase 1 SHIPPED (on the website) · Phases 2–4 designed, not built.**
The website repo's `/help` page and `js/help-catalog.js` are live; everything
in this doc that touches firmware is a plan awaiting the usual design-doc
sign-off before code. Nothing here changes device behavior yet.

This is the canonical cross-repo account of the support strategy: what the
Help Desk is, why its answers are structurally safe to automate, which
firmware contracts it consumes today, and what Phase 2+ asks of the firmware.

## The thesis

Support for this project was mostly built before anyone called it support:
the device self-tests and keeps its own crash evidence, the flasher
classifies its failures in plain language, the LED grammar is count-coded so
an error can be *said out loud*, the emulator rehearses fixes on the real
firmware, and the warranty is fix-not-return with a tested rule that **no
remedy ever demands a return**. The Help Desk is not a new system — it is
one front door onto all of that, held to one design promise:

> **No dead ends.** Every path a stuck person can be on ends in a fix they
> do themselves, a rehearsal on an emulated Canary, or a handoff that
> arrives pre-diagnosed. "100% satisfaction" is a claim our own linters
> would (rightly) reject; *never stranded* is a property we can build.

Two rules make it safe to run without a human per ticket:

1. **Routing may be fuzzy; answers must be pinned.** Search and matching can
   guess; the words a user finally reads are committed to a repo where the
   copy-honesty, legal-claims, spelling, and naming linters have already
   read them. No answer is generated at runtime, so no answer can hallucinate
   a warranty we never wrote.
2. **The device answers for itself, pull-based only.** Diagnosis reads what
   the device already publishes locally (`/api/selftest`, the boot log, the
   crash breadcrumb, the self-manifest) at the owner's initiative. Nothing is
   collected, phoned home, or retained — Invariants III and IV apply to
   support exactly as they apply to evidence, and that constraint is also
   the liability posture: data we never hold cannot leak, and answers that
   are pinned cannot overclaim.

## What exists today (the inventory the Help Desk fronts)

| Surface | Where | What it gives the desk |
|---|---|---|
| Boot self-test, 10 probes with per-probe fixes | firmware `diag_run_selftest()` → mirrored into the website's `onboarding-spec.json` (`selftest.probes[].fix`) | The fix a user reads is byte-pinned to the firmware; `/checkup` and `/help` render it, never copy it |
| `GET /api/selftest` | `canary-wap` `selftest_api.h` | One-shot verdict, reachable on the captive-portal AP; optional peripherals can never FAIL |
| Crash evidence without a cable | `canary-wap` `hardware_state.h` (`safe_mode_check`, RTC breadcrumb) | "Why is it in recovery?" answered on the dashboard |
| Flash-error classifier (7 kinds) + boot-log signatures (power vs clean-install) | `canary-local/assets/flash-core.js` (browser) + `desktop/src-tauri/src/health.rs` and friends (desktop; parity CI-gated) | The catalog's flashing entries restate these verbatim |
| LED grammar, count-coded | `docs/hardware/canary_qr_onboarding.md` | The website's blink decoder; groups of 2/3/4/5 are pinned by test |
| Diagnostic report (copy-paste, user-reviewed before sending) | `flash-core.js buildDiagnosticReport()` | The claims lane's proof-before-dispatch artifact — **after the Phase 3 tightening below**; as shipped it includes the MAC and an unfiltered serial tail |
| Fix-it flows + physics bench on the real firmware in WASM | `canary-local/emulator/`, `canary-local/assets/guides.js` | The rehearsal layer |
| Coarse fleet/hub health | `firmware/common/fleet_selfreport/fleet_selfreport.h` | The "hub unreachable vs no hub" distinction, address-free |
| Fix-not-return remedies, in code | website `scripts/fulfill.mjs` + terms-of-sale | The no-wasted-shipping gate |

## Phase map

- **Phase 1 — one catalog, one door (SHIPPED, website).** `/help`:
  symptom-first search over a sourced catalog (`js/help-catalog.js`, every
  entry cites the firmware file or doc it restates), the LED blink decoder,
  probe fixes rendered from the firmware mirror, a handoff that composes a
  pre-filled GitHub issue with the failing entry attached, and a
  `llms-full.txt` projection so outside AI assistants answer with the house
  fix. CI makes "no dead ends" structural (`tests/help-facts.test.mjs`).

- **Phase 2 — the device answers first (firmware + website).** Skip the
  questionnaire when hardware is present:
  - Website: read `/api/selftest`, the boot log, and the crash breadcrumb
    over WebSerial (the `/canary` page already holds the serial plumbing)
    and land directly on the verdict's catalog entry.
  - Firmware: a **help QR** — on request (console key or dashboard button,
    never unprompted), render/serve a QR encoding the Help Desk deep link
    for the current verdict. The display flavors already render
    commissioning QRs; the WAP already serves `/api/pairing-qr`. No new
    radio, no new data class — the QR carries only what the
    unauthenticated self-test already says on the AP.
    **Status: SHIPPED on the WAP** — `GET /api/help-qr` (public,
    selftest-parity boundary; route-security allowlisted with the
    justification) + a "Show Help QR" button in the headline dashboard's
    Fleet sheet. The verdict → URL composition, worst-first precedence
    (safe mode > hub down > first mapped failing probe), and the
    **probe-namespace bridge** (the WAP's "wifi"/"sd"/"bluetooth" ids map
    onto the website's `#probe-wifi_ok`/`#probe-sd_card`/
    `#s-ble-not-working` anchors — two ten-entry probe lists exist and
    they are NOT the same list) live in the pure, host-tested
    `help_qr_logic.h`. The website honors `#probe-<id>` deep links as of
    securacv_website#171.
    **Also SHIPPED: the canary-display glass rendering** — a "get help"
    row in Settings on every flavor renders the Help Desk QR on the glass
    (`Page::HelpQr`; verdict composed by the pure, host-tested
    `canary/ui/help_verdict.h`: verify failure > hub down > quiet
    witness, with the FSR_HUB_NONE "no hub is not broken" distinction).
    Its verdict inputs are what Settings reaches without new plumbing —
    the hub link via `mqtt_mgr`. Still open: a console key, and feeding
    the fleet model's witness-staleness/verification signals into the
    glass verdict (they live in main.cpp's fleet instance; the bare Help
    Desk covers them meanwhile).
  - **Build-order warning:** a canary-display change moves the committed
    emulator `dist/` when it touches what `canary-local/emulator/build.sh`
    actually compiles — `src/main.cpp`, the LVGL faces, `care/`, `fleet/`,
    `trust`, the color engine — and a VERSION bump always moves it, for
    every flavor. The `net/` exclusions (`glass_web.cpp`, `discovery.cpp`;
    `fleet_selfreport.h` is a worked no-dist-change example) do not. Check
    `build.sh`'s inputs rather than guessing, then follow the sacred order
    in CLAUDE.md (edit → regen → dispatch the pinned-emsdk rebuild → pull →
    catalogs → commit), and remember the stale-dist failure surfaces in the
    *page logic tests* job, not just the drift check.

- **Phase 3 — diagnosis before dispatch (website + fulfillment).** Warranty
  intake requires the diagnostic report; the classifier's verdict maps in
  code (`fulfill.mjs`) to the smallest remedy — board in a mailer, parts
  pack, printable STL, or "this is a cable, ship nothing." One-way always.
  - **Precondition — tighten the report first.** Today's report targets a
    voluntary Discussions paste and the user sees every line before sending,
    but it is not yet safe to *require*: it includes the device MAC (a
    stable identifier — Invariant III territory the moment it becomes a
    demanded artifact) and blindly appends the last 12 serial lines, while
    the WAP prints its device-unique AP password to serial at boot
    (`canary_wap.ino`, "[WIFI] AP started", acknowledged in
    `build_config.h`). Before the claims lane ships: move the report to an
    explicit field allowlist, drop or truncate the MAC to a non-stable
    form, and filter the log tail against known-sensitive lines (the AP
    password print above all). "Public-only by construction" has to be true
    of the required artifact, not just intended by the voluntary one.
    **Status: DONE in both flashers** — `buildDiagnosticReport()` now
    truncates the MAC to the nursery's non-stable tail (one truncation, one
    behavior — `macTail()`) and scrubs the serial tail with visible
    redaction markers before inclusion; `desktop_parity.test.js` item 12
    pins both frontends so neither can regress alone. What remains for the
    claims lane is the intake itself (Phase 3 proper), not the report.

- **Phase 4 — the loop.** "This didn't help" buttons (shipped in Phase 1)
  feed batched catalog curation; a human tends the corpus, never a ticket.

## What we will not build

- **A hosted-LLM chat widget on the site.** The CSP blocks it, Invariant IV
  extends to the docs, and a generated answer is an express-warranty risk
  (strategy doc 27). The conversational feel comes from device-read verdicts
  and pinned copy, not runtime text generation.
- **Support telemetry.** No session counters, no funnel analytics. Success
  is measured by what arrives on its own: issues and claims per unit
  shipped, and reshipment rate.
- **Full autonomy over money and safety.** Refunds, safety-critical calls,
  and the brand's word stay human (strategy doc 20). The desk hard-escalates
  on mains wiring, lithium cells, and working at height — it never advises.
- **"Can't brick" claims ahead of the code.** Until boot safe-mode + A/B
  rollback ship in the Arduino builds (self-star roadmap TODO 2), remote
  update copy keeps saying "may need a cable to recover." The Help Desk
  inherits that honesty bar.

## Anti-rot

The catalog lives in the website repo beside the tests that police it; this
doc is the firmware-side anchor. If a diagnostic named in the inventory
table moves or changes meaning, the website's `help-facts` and
`plugin-facts` suites are the tripwire — update the catalog entry and its
`source` in the same change, exactly as `onboarding-spec.json` already
demands for the probes.
