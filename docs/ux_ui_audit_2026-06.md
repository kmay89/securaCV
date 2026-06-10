# UX/UI Audit — June 2026

Full audit of every user-facing surface: feature controls, button wiring,
flows, micro-copy and tone — plus the firmware CI pipeline's portability to
future flavors. Issues found were fixed on the same branch; this report is the
record of what was checked, what changed, and what was deliberately deferred.

## Scope and methodology

Surfaces audited, in the order requested (canary WAP and PIO first, then
canary Vision, then the rest):

| Surface | Where | Method |
|---|---|---|
| Canary WAP embedded pages | `firmware/projects/canary-wap/arduino/canary_wap/` (`web_ui.h` /admin, `csi_dashboard_html.h` headline, `companion_pwa.h`, `setup_page_html.h` captive portal) | Every `onclick` handler cross-referenced to a defined function; every `/api/...` call cross-referenced to a registered `.uri` route in `canary_wap.ino` + `*_api.h` |
| PlatformIO build system | `firmware/envs/platformio/*.ini`, per-project `platformio.ini`, `.github/workflows/` | Config vs. CI coverage comparison |
| Canary Vision SPA | `canary-vision/spa/app.js` | Handler walkthrough: wiring, loading/empty/error states, confirmations, copy |
| Break-glass console | `src/break_glass/breakglass.html` | All six endpoints verified against the Rust routes in `src/break_glass/http.rs`; per-step flow walkthrough |
| Vault | `src/vault/` (UX surface is the break-glass console) | Flow + copy review |
| PWK setup wizard | `privacy_witness_kernel/wizard/index.html` + `serve_wizard.py` | Every wizard call mapped to a server handler; state-machine walkthrough |
| HA integration | `custom_components/securacv/` (config flow, strings, timeline card) | Flow trace + label conventions |
| Evidence viewer | `viewer/template.html`, `verify_core.js` | Flow + copy review |

## Headline result

Wiring is in good shape across the board: no dead buttons, no TODO stubs, no
unregistered endpoints anywhere. The real findings were a small set of
silent-failure paths, two genuine bugs, copy inconsistencies — and two CI
gaps (static analysis configured but never run; per-flavor hardcoded
pipeline).

## Bugs found and fixed

1. **PWK wizard reported a successful Home Assistant restart on failure.**
   The wizard's `api()` helper resolves with `{ok:false}` payloads rather than
   throwing, so `restartHA()`'s catch-only error handling showed
   "✓ Restarting — page will reload shortly" even when the Supervisor call
   failed. Now checks the payload and falls back to the manual-restart hint.
   (`privacy_witness_kernel/wizard/index.html`)

2. **HA config flow ignored the MQTT-discovered topic prefix.** Discovery
   stored the prefix in `self.context["mqtt_prefix"]` but the form always
   pre-filled the static default. Now the discovered prefix is the form
   default. (`custom_components/securacv/config_flow.py`)

## Silent failures fixed

- **Canary Vision SPA "Copy Link"** did nothing when `navigator.clipboard`
  was unavailable — which is the *normal* case on a plain-HTTP device origin.
  Added a textarea/`execCommand` fallback and an explicit failure message.
- **Break-glass "Copy" (request hash)** had the same silent no-op, plus no
  success feedback at all. Same fix, plus a transient "Copied" label
  (pattern borrowed from the evidence viewer).
- **Break-glass connect/open/unseal** had no in-flight guard — a double-click
  on Unseal could fire two unseal POSTs — and network errors thrown by the
  fetch helper vanished instead of reaching the step banner. Buttons now
  disable while a request runs and errors surface in the banner.
- **Break-glass status refresh** swallowed non-200 responses; 401/409 now show
  a "reconnect" / "no open request" warning.
- **PWK wizard camera "Test"** silently returned on an empty URL; now prompts
  "Enter the RTSP URL first."
- **PWK wizard preflight** could wedge on "Checking prerequisites…" forever
  when the server answered `ok:false`; the missing else-branch now reports the
  error and turns the box amber.

## Flow and confirmation fixes

- **Break-glass "Close request"** discarded all collected trustee approvals on
  a single click with no confirmation, and left step 4 (Unseal) enabled after
  closing. Now confirms with consequences spelled out and resets both steps.
- **Canary WAP /admin terse destructive confirms** ("Remove?", "Disconnect?",
  "Leave opera?", "Reboot device?", "Delete logs > 30 days?",
  "Acknowledge all?") rewritten to the consequence+recovery pattern the OTA
  and forget-device dialogs already used, at ≤ grade-7 reading level.
- **PWK wizard "Start Over"** left the done/error subviews and a disabled
  "Start SecuraCV" button behind; leaving the done screen now resets them.

## Copy and tone fixes

- Ellipsis style unified to `…` across the WAP /admin page (32 strings), the
  Canary Vision SPA, and the PWK wizard. Literal placeholders (token shape
  `cv_...`, receipt JSON) intentionally untouched.
- Canary Vision SPA config forms showed raw API keys (`mqtt_topic_prefix`) as
  field labels; now humanized via `labelFor()` (sentence case + an override
  map for acronym-bearing keys like SSID/MQTT/mDNS/URL).
- "opera" (the mesh-group brand term) now gets a one-time gloss on first use
  in the pairing wizard: "…your opera — the group your Canaries share alerts
  in."
- HA config-flow labels normalized to HA-standard sentence case ("Setup
  mode", "MQTT topic prefix"). Entity and service display names were *left
  alone* on purpose: renaming them would rename entities on users' existing
  dashboards.
- WAP /admin "Saved!"/"Failed" config alert replaced with plain-language
  outcomes.

## Pipeline portability (PIO)

- **`pio check` was configured but never ran.** `check_tool = cppcheck` has
  been in `firmware/envs/platformio/common.ini` since the envs were created,
  with no CI job executing it. There is now a matrix **Static Analysis** job
  running `pio check --fail-on-defect=medium --fail-on-defect=high` per
  flavor. Three pre-existing medium findings were fixed to land the gate
  green (printf format mismatch in `securacv_diagnostics`, uninitialized
  buffers in `securacv_gps`, side-effecting `assert()` in
  `test_mesh_pairing`).
- **The build pipeline was hardcoded per flavor** (separate jobs for canary
  and canary-wap; a whole separate workflow for canary-vision). It is now one
  matrix job driven by **`firmware/flavors.json`** — the single source of
  truth for flavors. Adding a future firmware flavor is one manifest entry
  (plus the usual `envs/`/`configs/` additions per `ARCHITECTURE.md`); no new
  CI jobs. Per-flavor quirks (canary's `intelhex` pip dependency, the
  pioarduino `PLATFORMIO_CORE_DIR` isolation, canary-vision's CI secrets
  header, OTA-slot size guards) are manifest fields, documented in the
  workflow's comment block.
- `firmware_canary_vision_ci.yml` was deleted; its build is a matrix leg of
  `firmware.yml`. **Note: the CI status check is renamed** — if branch
  protection pinned "Firmware CI (Canary Vision) / build", repoint it to the
  new "PlatformIO Build (canary-vision)" check.
- The special-purpose jobs are intentionally untouched: arduino-cli
  dual-toolchain build, sketch-copy sync checks, regression guards, archive
  guard, CSI privacy invariants, mesh/scout host tests, OTA logic tests,
  microcopy lint.
- Microcopy lint now also covers `companion_pwa.h` with the /admin
  internal-jargon list.

## Audited clean — no changes needed

- **Evidence viewer** (`viewer/`): the strongest surface audited — honest
  inconclusive-vs-rejected verdicts, plain-language "What this means" blocks,
  working copy fallback. Used as the house-style reference for the fixes
  above.
- **HA timeline card** (`securacv-timeline-card.js`): honest verification
  badges, empty state, fetch fallback, output escaping all present.
- **Captive portal** (`setup_page_html.h`): the no-JS/no-link design is
  intentional (iOS CNA constraints, documented in the file) and the copy is
  excellent.
- **WAP onboarding wizard backend** (`wizard.cpp`) and **headline dashboard**
  (`csi_dashboard_html.h`): sound state-machine semantics; FKGL 4.55 against
  a 6.0 ceiling.
- **Break-glass Rust backend** (`src/break_glass/`): all console endpoints
  exist and the 422 rejection reasons surface in the UI.
- **Canary Vision firmware** (`firmware/projects/canary-vision/`): headless by
  design (MQTT + HA discovery); its UX surface is the HA integration and the
  SPA, both audited.

## Deferred (recommendations, not regressions)

- **MQTT availability validation in the HA config flow:** a user without the
  MQTT integration configured gets a config entry that silently does nothing.
  Proper fix uses `async_wait_for_mqtt_client` and needs HA test fixtures.
- **Microcopy lint for non-firmware surfaces** (break-glass console, viewer,
  PWK wizard): different audiences (operators, lawyers) need their own
  calibrated term lists; the current grandma-calibrated list would misfire.
- **ENTERPRISE_READINESS_TODO items** (provisioning receipt QR flow, guided
  factory reset, owner-passphrase onboarding steps): new features, out of
  scope for an audit pass; tracked in
  `firmware/projects/canary-wap/ENTERPRISE_READINESS_TODO.md`.
- **Entity/service display-name casing** in the HA integration (see above —
  intentionally not renamed).

## How this was verified

- `firmware/scripts/microcopy_lint.sh` passes (including the new PWA pass);
  `web_assets_gz.h` regenerated from the edited sources.
- cppcheck (`--enable=warning,performance[,portability]`) reports zero
  warning/error-severity findings across all three flavors.
- `node --check` on every edited script block; ESLint clean on the SPA; all
  canary-vision tests (`tests/run-all.sh`), PWK wizard tests
  (`test_serve_wizard.py`, 18), and HA integration tests (34) pass.
- mesh_pairing host test rebuilt and passes after the assert refactor.
- `firmware/flavors.json` validated with `jq`; workflow YAML parses.
- The full `pio run`/`pio check` matrix could not be executed in the audit
  sandbox (package registry blocked); the PR's CI run is the authoritative
  pass.
