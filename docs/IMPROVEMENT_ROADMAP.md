# Improvement roadmap — the 2026-09 three-repo audit

*Written 2026-09-02 from a full read of this monorepo, the HACS mirror
([`securacv-homeassistant`](https://github.com/kmay89/securacv-homeassistant))
and the website ([`securacv_website`](https://github.com/kmay89/securacv_website)),
checked against the trees on `main` that day. The fixes that could be made
without hardware or an Apple toolchain landed in the same pass: monorepo
PR #1628, mirror PR #7, website PR #183.*

This is the **ordered list of what still needs doing**, with the reasoning
beside each item so the next person (or assistant) does not have to re-derive
it. It inherits the two rules every doc here inherits: a group of Canaries is a
**fleet**, and status is tiered `compile-tested → verified`, where *verified*
means a signature checked against a pinned key or a claim checked on real
hardware. Nothing below is marked verified that was not.

Read [`NEXT_STEPS_2026-07.md`](NEXT_STEPS_2026-07.md) first if you want the
July picture; this document is the September one, and it deliberately does not
repeat the A/B rollback, Nightstand Line and fleet-aggregator threads except
where the tree has moved.

---

## 1 · How the audit was run

Thirteen subsystems were read independently, each by a reader told to report
only what it could point at in the tree: the Rust kernel, `firmware/common`,
the CSI / Wi-Fi sensing stack, the emulator and Lab, the browser flasher pages,
the Home Assistant integration and its mirror, the canary-local tooling, the
iOS/watchOS app, the tvOS Witness Wall, the two desktop apps, the CI and
release workflows, the docs and the website. Every finding rated high was then
handed to three separate refuters, each told to try to kill it; a finding
survived only if a majority could not. The counts:

| | |
|---|---|
| Findings reported | 138 |
| Rated high | 27 |
| Landed in the September PRs (first pass) | 78 |
| Open list items landed in the same PR before merge | 34 |
| Landed in the follow-up wave (PR #1635, mirror #9, website #184) | 13 in full, 2 in part |
| Closed by the documentation wave (PR #1647, mirror #11, website #189) | 0 — a fresh docs audit, not this list; see §3 |
| Landed in wave 3 (PR #1664) | 10 in full (four of them rows that were "in part") |
| Landed in wave 4 (2026-09-08) | 7 in full (items 2, 5, 6, 13, 22, 42, 51 — 2 and 22 host-tested, 5/6/13 compile-untested here) |
| Landed in wave 5 (2026-09-11, PR pending) | row 21's Parametrize wave, in part — the device manifests own their cases' board knobs and a generator writes them into the CAD, the regeneration order is one command, the CAD ledger the website pins to carries the knobs; zero `.scad` bytes moved |
| Still open | 0 in full, 2 in part (21's Parametrize leftovers — see §4 — and 30's version spread), plus one decision surfaced in wave 4's review: whether the witness chain's uptime-bucket floor (`TIME_BUCKET_MS`, 5 s in both firmwares) should widen to the ten-minute grid Invariant III names for wall-clock time; and two surfaced in wave 5 (§4): whether the optional render-plan package is wanted, and what to do about `canary-local/devices/registry.json`'s hand-typed `body_mm` |

"Landed" means the change is in a PR and its local checks pass. The firmware
target compiles, the Swift edits, and every claim about device behavior are
still `compile-tested` at best; see §9 for what a bench pass has to confirm.

---

## 2 · What the September PRs changed

One line per area, so the open list in §3 reads against a known baseline.

| Area | What landed | Where |
|---|---|---|
| **Wi-Fi sensing** | IDF-portable CSI config (legacy and HE structs), L-LTF data-tone selection replacing the first-52-pairs copy, a router-echo traffic source, breathing band on the corrected tone index, bundle refreshes no longer spend the commit ceiling, closed bundles reach the event ring, window phase lock resynchronizes, honest probe airtime comment | `firmware/common/csi/`, [`csi_wifi_sensing_research.md`](csi_wifi_sensing_research.md) |
| **Kernel** | Time-bucket coarsening is widen-only; accept loops classify errors instead of exiting; sandbox reaps its child and denies `statx`; the MQTT bridge stops republishing the current bucket | `src/lib.rs`, `src/api`, `src/break_glass`, `src/module_runtime/sandbox.rs`, `src/bin/event_mqtt_bridge.rs` |
| **CI / release** | Freshness workflows fall back to an issue; kernel releases re-mark latest; desktop publishes refuse to run without the updater key; BOM regeneration gated; secret scan covers the file types this project actually has; version-sync, plist, icon, mesh-sync and CSI host-test gates; Dependabot sees the composite actions and pip | `.github/` |
| **Apple** | ATS local networking; privacy manifest for four targets; octet-parsing private-host check with tests; Wall defaults a missing `online` to false and stops saying "verified" without a pinned key; build stamps read the one firmware define | `ios/`, `tvos/` |
| **Desktop** | Least-privilege Tauri capabilities in both apps; Lab copy no longer claims nothing phones home; updater key documented | `desktop/`, `desktop-lab/` |
| **Home Assistant** | Device-id gate on the wildcard subscription; replay watermarks on the signed counters; card URL cache-busting; coordinator refresh on MQTT setup; a weekly mirror-drift check in the HACS repo | `custom_components/securacv/`, mirror `.github/` |
| **Website** | Inline scripts moved out so the CSP header is true; canonical links; no hand-typed sitemap dates; planning notes filed under `docs/`; fair hero randomization; glossary terms | website repo |
| **Docs** | Threat model rows, glossary/FAQ/variant-audit/flight-rules/spec alignment, six regenerated assistant entrypoints, CHANGELOG | `docs/`, `AGENTS.md` |

---

## 3 · The open list, in priority order

Priority is by consequence to a user, then by how much of the project the fix
unblocks. Effort is `S` (an afternoon), `M` (a few days), `L` (a milestone).
Each item names the file to start from.

### Landed before the PR merged

The open list below is the one the audit produced. Thirty-four of its
sixty items landed in the same PR while it was in review, so the numbers
are kept but the rows are marked **(landed)** and the reasoning stays for the
record: 1 (kernel serves `/api/fleet`, self row only — aggregation is still
open), 4 (the envelope ring holds its sample across a late window, so the
time base survives a stall; a timestamped resample is the fuller fix if the
bench shows drift remains), 10, 11 (peer wellbeing words omitted for
cross-site origins; the wildcard itself stays because the Wall needs it), 7
(the desktop flasher recognizes every integrity keyword the browser does, and
a parity test now holds the two tables together), 14, 15, 18, 19 (a full sealed-log document vector the kernel emits and the TV
core walks), 20, 23 (the gate against `flavors.json`; deriving the release
steps from it is still open), 24, 25, 32, 33, 39, 43, 45, 47, 48, 49
(CI-backed rows; the `cargo doc` gate landed in wave 3), 36 (ruff over the
tooling, 82 findings fixed), 40 (the nightly BOM snapshot lands as a PR),
50, 52, 53, 54, 55, 56, 57, 58 (already true at HEAD), 59, 60, plus two the review of the PR
itself found: the MQTT bridge's publish
cursor now ignores export jitter, and the tvOS bundles carry a privacy
manifest that the plist lint actually inspects.

### Landed in the follow-up wave

A second pass on 2026-09-03 (monorepo PR #1635, mirror PR #9, website PR
#184) took the rows that need no hardware and no Apple toolchain, one
package per worktree, each reviewed adversarially before it was merged.
Landed in full: 3, 4, 8, 16, 17, 27, 28, 29, 31, 37, 41, 44, 46. Landed in part: 21 (the device
package's wave 1, the manifests and the linter that proves their joins;
waves 2 and 3 in §4 are open) and 30 (the platform pin lives once, as a pure
refactor; the decision about the version spread is documented in
`firmware/PLATFORMS.md` and still open). Each row below says what actually
shipped and where it deviates from the row's original proposal.

### Landed in wave 3

A third pass on 2026-09-08 (monorepo PR #1664) took the larger firmware and
CI refactors the follow-up wave had left, again one package per worktree with
the merged diff reviewed adversarially. Landed in full: 9 (BLE OTA protocol
v2), 12 (a verified TLS option for every broker link, fail-closed, with the
premise corrected: the WAP's old flag was an unverified handshake), 26 (the
emulator runs the shared Wi-Fi join policy), 34 (the display build sharded by
board family), 35 (a derived, byte-gated SBOM), 38 (four composite actions,
R10 machine-checked); and four rows that had been "in
part" are now whole: 1 (the kernel's `/api/fleet` lists the Canaries its MQTT
bridge heard, presence proven by signed chain publishes), 11 (the CORS
wildcard is an origin allow-list), 23 (both release workflows derive the
display env list from `flavors.json`), 49 (the `cargo doc` gate). The device
package's wave 2 (row 21) landed too — the generators read the manifests with
every byte gate proving nothing moved — leaving only wave 3 (Parametrize)
open on that row. Two things this wave could not do here: run a real release
(the derived env list and the composite actions are host-tested, so the first
tag after this merges deserves a look at its logs), and rebuild the emulator
`dist/` (dispatched to the pinned-emsdk workflow). What was left — 7 rows in
full and 2 in part — was hardware-bound (items 2, 22 and the §5 bench steps),
Apple-toolchain-bound (5, 6, 13), a maintainer decision (30's version spread,
42, and — until it landed later the same day with its premise corrected, see
the row — 51), and the device package's Parametrize wave.

### Landed in wave 4

The seven rows still open in full landed together on 2026-09-08, each in its
own worktree and each saying in its row exactly what it could and could not
prove here:

- **Item 2, the CSI transmitter filter** — `host-tested`: the canonical
  `csi_hal.{h,cpp}` compares a frame's transmitter against the associated
  AP's BSSID (never copies it), counts the rest under
  `frames_dropped_foreign`, and a host test drives the shipped HAL through a
  stubbed ESP-IDF surface. The bench pass in §5 step 3 is what makes it a
  claim about a room.
- **Item 22, one CSI HAL** — `host-tested`: `securacv_csi.cpp` is an
  81-line adapter over `firmware/common/csi`, which the canary build now
  compiles directly; `check_csi_sync.sh` refuses a body the adapter shares
  with the canonical HAL, a direct driver call, or a line count past its
  budget, and a host link test proves the single callback registration. The
  canary product's CSI silence watchdog is live for the first time.
- **Items 5, 6, 13, the Apple-toolchain rows** — landed in a worktree with
  no Swift toolchain, so every Swift line is read-carefully and
  **compile-untested here** (the gated iOS CI is its compiler); the
  canary-wap `/api/v1/witness` endpoint, the shared fixture and its generator
  are host-tested.
- **Item 42** — three redundant release launchers and the Lab's never-built
  Tauri iOS shell workflow are retired; `RELEASE_BUTTONS.md` maps each to
  what to press instead.
- **Item 51** — landed with its premise corrected: Home Assistant 2026.3+
  serves the integration's own `brand/` folder and HACS's validator reads
  it, so the folder is carried, not deleted.

What is left — 0 rows in full and 2 in part — is the device package's
Parametrize wave (row 21) and row 30's version-spread decision, plus the
bench confirmations in §5 that no worktree can run.

One decision surfaced by the wave-4 review and deliberately not taken
here: the witness chain binds `time_bucket = millis() / time_bucket_ms`
with a 5 s floor (`TIME_BUCKET_MS` in canary-wap, `CONFIG_TIME_BUCKET_MS`
and the canary product's `securacv_witness.cpp`), documented in six places
as the privacy floor, while Invariant III names ten-minute buckets for
wall-clock time. The bucket rides every witness surface because the hash
binds it, so `/api/v1/witness` adds no exposure — but an authenticated
reader can place records 5 s apart relative to boot. Widening the floor to
600 000 ms is mechanical (the two constants, `configs/canary-wap/*/config.h`,
`web_ui.h`'s "Minimum 5000 ms" copy, the two READMEs, `LESSONS_LEARNED`
line "5-second buckets (minimum)", `test_config_logic.cpp`'s `FLOOR`) and
changes nothing about verification, but it is a product decision about
both firmwares' chains, not a review fix (`spec/witness_api_v1.md` §3).

### Landed in wave 5

A fifth pass on 2026-09-11 (monorepo PR pending) took the one row still
open in part that needs no hardware: the device package's Parametrize wave
(row 21, §4). One package per worktree, the merged tree reviewed
adversarially before these docs were written to it, and — the proof the
wave was designed around — **zero `.scad` bytes moved**: `git diff --stat`
from the wave's base lists no `.scad`, `.stl`, catalog or builder-manifest
file, every generator's `--check` is green on the landed tree, and a
write-mode run of the new generator over it prints "nothing to write".

- **The manifest owns the board knobs.** `devices/<slug>/device.json`
  `cad.params` names the literal board and module knobs of its case
  (`board_l`, `vm_w`, `stack_sock_h` — the dimensions a case is built
  around, never its walls, never a selector), and
  `docs/hardware/enclosure/gen_cad_params.py` **writes** them into the
  `.scad` — the token on the knob's own line, nothing else — with `--check`
  proving equality in `lint.yml`, in `enclosure.yml` and through
  `scripts/lint_device_manifests.py`. Nothing reads a manifest at render
  time: the Customizer, `render.sh`, the fit check, both catalog parsers and
  `lint_design_lang.py` keep seeing the literal knob they always did. The
  manifest moved to the front of the chain; it did not replace a link of it.
- **A knob that is a board fact references the registry.** A value may be
  `{"brd": "xiao", "dim": "w"}` (a `BRD_REGISTRY` row and column of
  `canary_board_lib.scad`) or `{"brd_fn": "brd_xiao_w_measured"}` (one of
  its measured facts); the generator resolves it before it writes, so a
  registry correction reaches every owned case and `--check` reports the
  drift as "registry says X, file says Y". Converted only where the knob's
  help comment already cites the registry; a knob that merely equals a row
  by coincidence stays a number. 54 knobs across 8 case files (the WAP, the
  Vision — shared by the DevKit — the Sense and five display cases), 29 of
  them references.
- **The regeneration order is one command.** `scripts/regen_cad.py` runs
  the twelve generators and gates in the order each one's inputs dictate,
  stops before the emulator dist rebuild (only Actions can build it; `--from
  gen_flash` resumes), renders the owed previews of every part of every
  changed case with `--previews DIR`, and `--check` names the first stale
  step. Rehearsed end to end on a scratch branch (`canary-wap` `board_w`
  17.5 → 17.8): one `.scad` line, 116 clean renders, the WAP envelope
  36.6 → 36.9 mm, 46 files moved, 26 previews rendered — then reverted. The
  landed tree is a fixed point of a full run.
- **The CAD ledger carries what the website needs.** `gen_builder_manifest.py
  --site` adds, per figure, the resolved `knobs` and the measured `seams_mm`,
  and the whole board registry with its evidence rung (`board_registry`,
  `board_facts`) to `scad/cad-dims.json` — additively (strip the four keys
  and the website's committed bytes are reproduced), unrounded, and refused
  while a manifest disagrees with its case. `--site <checkout> --check` is
  the carry's first gate; a write run writes only what changed.
- **What did not land, named** (the row and §4 carry the detail): the
  Nightstand C6 (`board_l` / `board_w` are a `model` ternary), the 7" frame
  (its panel record is typed in `canary_panel_lib.scad`), the Touch 1.69's
  `aa_dx = 0.0; aa_dy = 0.0;` line, the doorbell (no manifest names its
  case), the website's reading of the new ledger keys (the website repo's
  change), and the optional **render-plan** package — a per-set selector
  vocabulary in the manifests, which rewrites `render.sh`,
  `gen_assembled_dims.py` and enclosure CI and re-homes five Lab cards
  (`enclosures.json` attributes the display sets to the wrong device today)
  — deliberately **not built**: it needs the maintainer's yes. Two decisions
  taken as defaults and reversible: `envelope_mm` is not a manifest input
  (`figure` is the join; the ledger measures envelopes off the STLs), and
  `canary-local/devices/registry.json`'s hand-typed `body_mm` is left as it
  is, with its disagreement recorded (the Dash: 113.7 × 73.6 × 16 there,
  118 × 79 × 38.9 in `figures.json`, `dims_source: board-cad`).
- **One lesson, kept** (`CLAUDE.md`, "Generated files"): the review round
  found the generator's checker answering "the file already says it" for a
  value it could not spell as a Customizer literal — an exponent-form
  number, NaN, a string holding a comment opener — so a wrong manifest
  passed `--check` and a write wrote nothing. A generator that cannot spell
  a value must refuse by name, never pass.

Two wave counters meet here and are easy to confuse: the §3 waves count
PRs (this is the fifth); §4's "three waves" are the device package's own
ladder — Describe 1, Consume 2, Parametrize 3 — which `devices/README.md`
uses. Row 21 reads "waves 1–3" in the second sense.

### The documentation wave

A third pass on 2026-09-04 (monorepo PR #1647, mirror PR #11, website PR
#189) audited the prose itself — the claims in the three repos' docs
checked against the trees, every markdown link resolved — and fixed what
it found, with gates so the same rot cannot recur. It closes **none of
the rows above**: what is still open is hardware-, Apple-, decision- or
refactor-bound, and a docs pass cannot reach it. (The nearest miss is 51
— the wave's new mirror brief records `brand/` as mirror-owned, but the
row's open half, moving those assets to the HACS brands repo and out of
the mirror, is the maintainer decision it always was. Row 51 landed on
2026-09-08 with that premise corrected — `brand/` is carried, not
mirror-owned; see the row.) Recorded here so
the ledger stays complete:

- **Monorepo (#1647), docs, comments and lint scripts only.** Wrong or
  stale claims fixed, each verified against the tree: `MAINTAINERS.md`'s
  CODEOWNERS team that cannot exist on a personal account;
  `CONTRIBUTING.md`'s HACS release recipe, replaced with the real
  mirror-PR flow; `SECURITY.md` and `.cargo/audit.toml` citing a c2pa
  version as newest that no longer was (citations are date-anchored now);
  `docs/LEGAL.md`'s misplaced Vision MIT license path and swapped
  copyright holders; the third-party attribution `NOTICE` promised and
  never appended; a private contact route for the code of conduct; the
  "without a microphone" claim in the getting-started guide and FAQ (now:
  loudness envelope, never sound it keeps, hedged for mic-less variants);
  `docs/frigate_integration.md`'s configuration table against the
  kernel's `config.yaml`; `docs/v1-roadmap.md`'s pre-promotion OTA bullet
  and `firmware/FEATURES.md`'s already-shipped open action; and five
  surviving uses of the banned "-proof" claim, now tamper-evident. Every
  broken relative link and anchor the audit found is fixed — from the
  enclosure README's own TOC to strategy doc 24 linking into a sibling
  checkout of the website repo — and AGENTS.md's unclosed fence (which
  made GitHub render the Beacon-invariants section as one code block) and
  stale `chirp_channel.h` path are gone. Two new gates in `lint.yml` hold
  it: `scripts/lint_md_links.py` (every relative link and `#anchor` in
  every tracked `.md` resolves) and `scripts/lint_fleet_word.py` (rule 3,
  deterministic at last).
- **Mirror (#11).** The two carried files that had drifted — `voice.py`
  (the WAP `system.integrity` tamper kinds missing from the urgency set)
  and the timeline card (missing `TAMPER_KIND_METADATA`) — resynced
  byte-for-byte and proven exact by `check_mirror_sync.py`, making the
  README's "byte-identical" line true again. The README's standalone test
  command now works as written and its freshness description matches the
  workflow; the mirror has its first agent briefs (`AGENTS.md`,
  `CLAUDE.md`), stating the carried-vs-owned split; and
  `.github/scripts/lint_readme.py`, wired into `validate.yml`, checks the
  three mirror-owned prose files — every relative link resolves, no
  banned bird-group word, no overclaims.
- **Website (#189).** A root README at last (the repo had none — a GitHub
  visitor landed on a bare file listing), with its guards in the same
  commit: the claims tests in `tests/copy-honesty.test.mjs` scan the
  README now, a new whole-file test bans the bird-group word on every
  root page, the routed `/tv/` pages, the share-card design and the
  README (`glossary.html`, where the rule is defined and byte-pinned, is
  the one exemption), and `tests/md-links.test.mjs` makes every markdown
  link in the repo resolve. The brief's gaps are closed too:
  `scad/colorways.json` joins rule 7's carried outputs, the one
  deliberately un-byte-checked generator (`share-card.jpg`) is documented
  with its pair rule, the CI-gates table gains the two workflows it
  omitted, and CLAUDE.md's phantom rule reference now says rule 3 — with
  all six vendor entrypoints regenerated.

### P0 — wrong evidence or a broken promise a user would hit

| # | Item | Why it matters | Fix | Effort |
|---|---|---|---|---|
| 1 | **(landed)** **The Wall cannot reach any sealed-log source.** The kernel serves `/api/sealed-log` but not `/api/fleet`, which is the only discovery contract the tvOS Wall implements. | The "lights up with no app change" promise in `tvos/discovery/DISCOVERY.md` is false against the only kernel that exists. | Landed in two steps. The kernel serves `GET /api/fleet` in the firmware's self-report shape with the anti-drift vector (follow-up wave). Wave 3: it also lists the Canaries its MQTT bridge has heard — `src/fleet_peers.rs`, fed by `event_mqtt_bridge --fleet-peers-path` / `WITNESS_FLEET_PEERS_PATH` and read by the API through `api.fleet_peers_path` (schema `securacv/fleet_peers/v1`, one file both processes point at). A peer is `online` only when a live (not broker-retained) chain publish verifies against the key pinned for it on first `health` (a second key is a sticky conflict) within 180 s; wellbeing words ride only on such a row while fresh; the two-row document is a shared vector in `tvos/witness-core/tests/fixtures/fleet_contract_vectors.json`, and its bytes are produced from typed rows so they do not move with `serde_json`'s feature set. Open: wire the flag into the HA add-on's `run.sh` (two JSON config blocks plus one argv entry) so an add-on install lights the Wall up unaided; the Docker sidecar needs a shared volume with witnessd first. | M |
| 2 | **(landed)** **CSI mixes every transmitter into one window.** Neighbor-AP beacons, peer Canaries' ESP-NOW probes and router echoes all land in the same 64-frame window; per-subcarrier variance across alternating links reads as motion. | The presence detector's false-positive floor is set by the neighborhood's Wi-Fi, not by the room. | Landed, host-tested (2026-09-08). `csi_hal.cpp`'s callback compares each frame's transmitter address, in place, against the BSSID of the AP the station is associated with — read back from `esp_wifi_sta_get_ap_info()` on the main loop (polled, and refreshed at once from the STA got-IP handler), held in one static that is never exported or logged and is wiped on `deinit()` — and, on the WAP, against `csi_probe::has_peer()`; everything else is counted under `frames_dropped_foreign` (appended to `csi_stats_t`; on `/api/status` beside the other drop counters with `filter_foreign` / `filter_armed`; on the canary webui's driver-health tile) and never buffered. `filter_foreign` is a persisted `/api/settings` key, default on. Until the STA associates there is nothing to compare against and every frame passes, so AP-only installs sense as before. The staged sketch copy is byte-identical; the canary tree's `securacv_csi.cpp` carries the BSSID half (no probe layer there). **What did not land:** peers are accepted only through the WAP's probe table, which nothing fills yet (the probe is broadcast-only), so a Canary-only install with no association still hears every transmitter; and nothing here is bench-verified — §5 step 3 has to show `frames_dropped_foreign` climbing while the presence floor drops. Host test: `csi_hal_transmitter_filter_test.cpp` compiles the shipped HAL against `host_stubs/`, feeds two transmitters, and scans the ring, the stats and the emitted feature vector for either address (two planted leaks fail it). | S |
| 3 | **(landed)** **Breathing envelope is raw magnitude the driver's AGC removes.** The host test passes because synthetic frames have no automatic scaling. | `quiet` presence and `unusual_breathing` will not fire on a real device. | Landed: the envelope is now each subcarrier band's share of the per-frame-normalized row (four rotation bands, the Goertzel bank run per band, each bin keeping its strongest band), which per-packet gain cannot move; the host tests drive a 0.25 Hz breath through a simulated per-packet AGC into bin 3 and read zero in every bin through 80 s of ±30 % gain flicker. Host-tested on synthetic frames only — the bench pass (§5 step 3) is still what turns this into a claim about a room. | S |
| 4 | **(landed)** **Breathing Goertzel assumes exactly one window per second.** Window cadence is loop-driven and gaps are skipped, so the 6+3i BPM map drifts with loop latency. | Reported breaths-per-minute is a function of CPU load. | Landed: every window close carries its timestamp and the envelope is resampled onto a fixed 1 Hz grid (a close inside the previous slot is averaged into it, a gap is bridged with held copies), and `csi_stats_t` reports `windows_held`, `windows_merged` and `window_period_ms` (appended, both status endpoints surface them). Host tests: 700 ms and 1300 ms cadences both keep 12 BPM in bin 2 with the counters reporting the real pace; a 3 s stall holds two copies. The three copies (common, the sketch mirror, the embedded extractor) moved together. | S |
| 5 | **(landed)** **A TLS-enabled WAP is unreachable from the iOS app.** `URLSession.shared` never answers the server-trust challenge and the receipt's `tls_cert_fp` pin is discarded. | The one configuration that protects the router password in transit is the one the app cannot talk to. | Landed. `ProvisioningReceipt` keeps `tls_cert_fp` (64 hex, normalized; an http device's empty string is no pin), `PairedDeviceRef.tlsCertFingerprint` persists it (and CloudKit `PairedDevice.tlsCertFP` syncs it — the schema must be promoted before the next iOS release, `ios/scripts/cloudkit_schema.sh`), and `DeviceAPI` dials https through `PinnedTrustDelegate`: the leaf certificate's DER is SHA-256'd (CryptoKit) and compared exactly to the pin — public data, so exact is right — `.useCredential` on match, `.cancelAuthenticationChallenge` on mismatch surfaced as `DeviceError.certificateMismatch` with a user-readable message. An https device with no pin is refused (`.tlsPinMissing`: a TLS device the app cannot check is not a checked device), PairView refuses such a receipt at paste time, http devices keep `URLSession.shared`, `isPrivate()` is unchanged and still first. One pinned session per fingerprint (`PinnedSessions`) so the per-poll `DeviceAPI` does not leak sessions; `LivenessProbe` and the rollout's return-watch probe through the same pin. `TLSPin` is XCTest-covered with a real DER certificate and its host-computed digest. **Swift compile-untested here; the WAP endpoint and fixture are host-tested.** | M |
| 6 | **(landed)** **On-phone chain verification targets the wrong API.** `DeviceAPI.witness()` fetches `/api/v1/witness` (the canary-vision Node reference), while the WAP serves `/api/witness` with a different record shape and no signature. | The app's headline trust feature cannot run against any firmware in this repo. | Landed as one contract with two chain formats, [`spec/witness_api_v1.md`](../spec/witness_api_v1.md): the reference server's shape is `reference_v1` (absent `chain_format`), and canary-wap serves `GET /api/v1/witness?last=N` as `wap_v1` from a 16-record RAM ring (`witness_page.h`, pure; `handle_witness_v1`, Bearer-gated like `/api/witness`, in the route-security and handler-budget checks) — each record with its full chain-hash pre-image (`prev_hash`, `payload_hash`, `seq`, `time_bucket`, `time_bucket_ms`) and the per-record Ed25519 signature the firmware already makes over the raw 32-byte chain hash; no second signing scheme. `timestamp` is the ten-minute bucket start and only when the device has met a clock (Invariant III); `event_type` maps the record type (`tamper_detected` is the dictionary id); the device's own `verified` flag is deliberately not on the wire. One shared fixture, `spec/fixtures/witness_page_v1.json` (generator `--check`ed in CI): `tests_host/test_witness_page.cpp` rebuilds it from real Ed25519-signed records and byte-compares (host-tested, passes), and `ios/Tests/SecuraCVTests/WitnessPageFixtureTests.swift` decodes it against the same public key. The Swift decoder tolerates an absent/empty signature (→ `.unsigned`, never Verified), an absent timestamp (anchored coarsely from the page's `uptime_s`), and an unknown format (→ Unverified, not tamper); `ChainVerifier` recomputes `wap_v1` links and verifies over the raw hash; the WAP poll now fetches the page and sets the badge from the verdict. **Swift compile-untested here; the WAP endpoint and fixture are host-tested.** | M |
| 7 | **(landed)** **Two flashers still disagree on Ed25519 refusals** in one direction: the browser now classifies them as integrity failures, but the desktop Flasher's diagnostic copy and recovery hint differ. | Half the users get the vague message (AGENTS.md rule 7). | Share the classification table as a JSON both frontends load (`canary-local/assets/flash-core.js`, `desktop/src/`). | S |
| 8 | **(landed)** **Add-on image workflow has been red on `main` for six runs.** The aarch64 QEMU leg hits the 90-minute timeout and the verify-public gate misreads a 404. | Home Assistant OS users on Raspberry Pi get no add-on image. | Landed, in two steps. The build half: aarch64 builds natively on `ubuntu-24.04-arm` (9 minutes on `main`, where the QEMU leg had been dying at the 90-minute timeout). The gate half took a correction: the first fix taught `verify_published_image.sh` to cross-probe GHCR with the workflow token and to say *private* or *not pushed*, and its author (this document included) read the 30 August 404s as a private package. They were not. The images were public and present all along; the probe's `Accept` list lacked `application/vnd.oci.image.manifest.v1+json`, the media type buildx writes for a single-arch image pushed with `provenance: false`, and GHCR answers that omission with a 404 whose body says so. With that one type added the gate reports all four images public from an anonymous client. No owner action was ever needed. | M |

### P1 — security posture and trust claims

| # | Item | Why it matters | Fix | Effort |
|---|---|---|---|---|
| 9 | **(landed)** **BLE OTA bypasses the anti-downgrade floor and has no product binding.** Documented as deliberate for rescue, but nothing else enforces it. | A paired phone can push an older signed image with a known bug. | Landed as BLE OTA protocol v2 (`firmware/projects/canary-wap/arduino/canary_wap/ble_ota_policy.h`): the 168-byte BEGIN_V2 header puts product, version, size and digest under one domain-separated Ed25519 signature (`scv-ble-ota-v2`, the manifest-signature convention, emitted by `ota_release.py` as the manifest's `ble_signature`); the device verifies first, binds the product, and runs the version through the pull engine's own `securacv_ota_update_decision()` against max(running, NVS floor). A legacy v1 header or a below-floor image is admitted only through the existing BOOT-button provisioning gate (single-use, 30 s), every such acceptance is logged as a floor bypass and surfaced as `break_glass` in `/api/bluetooth/ota`, and the companion page sends v2 automatically. Host-tested with real Ed25519 (`tests_host/test_ble_ota_policy.cpp`, 255 checks; canonical bytes pinned against `test_ota_release.py`); the device path is compile-tested only. Manifests cut before this lack `ble_signature`, so a phone holding one lands on the break-glass path by design. | M |
| 10 | **(landed)** **`glass_web` OTA check/install skip the Origin+CSRF guard** the comment says they share. | A LAN web page can start an update on a display. | Route both handlers through the existing write guard in `net/glass_web.cpp`. | S |
| 11 | **(landed)** **`/api/fleet` is served with `Access-Control-Allow-Origin: *`** and now carries per-peer presence, occupant count and breathing state. | Any drive-by web page on the LAN can read who is home. | Landed. `/api/fleet` no longer sends `Access-Control-Allow-Origin: *`: the kernel answers from `FLEET_ALLOWED_ORIGINS` in `src/api/mod.rs` (the Lab/flasher origin from `firmware/build_matrix.json`, `https://securacv.com`, and `http://localhost` / `http://127.0.0.1` on any port, exact host); native readers send no Origin and get no CORS header; anything else gets none and the browser blocks the read; preflight matches. Consequence, documented in `tvos/discovery/DISCOVERY.md`: a Lab page served from a LAN host cannot read the kernel's fleet any more. Firmware boards still answer `*` — narrowing them the same way is the remaining piece of this row's original scope, and the display's coarse-rows rule is the other honest answer. | S |
| 12 | **(landed)** **Headless MQTT variants (display/sense/vision) have no TLS option** while canary-wap does; the gap is undocumented. | A broker credential crosses the LAN in the clear on three of four products. | Landed, with the premise corrected: canary-wap's `tls` bool produced an unverified `mqtts://` handshake, so no product had a verified option. One shared decision (`firmware/common/network/mqtt_transport_logic.h`, host-tested) and a `WiFiClientSecure` transport header used by canary-display / -sense / -vision (every display flavor but the nightstand-c6, built plain-only for its 0x1F0000 slot — it refuses a provisioned TLS mode rather than connecting plain), applied by canary-wap's esp_mqtt bridge from a drift-gated staged copy. Modes: plain (default — every flashed unit is unchanged), CA-verified, SHA-256 certificate-fingerprint pin, and an explicit lab mode that warns on every connect — the last two on display / sense / vision only: the WAP's esp_mqtt has no pin hook and the pinned core's esp-tls cannot skip verification, so it refuses both by name. Anything incomplete refuses to connect and names the reason without the secret. NVS keys `mqtt_tls` / `mqtt_ca` / `mqtt_fp` (products) and `mqtt.tlsmode` / `mqtt.ca` (WAP); both flashers' NVS builders seed them, parity-gated; the WAP's `/mqtt` page gained the controls. **Breaking for WAP units that had "Use TLS" on with no CA:** they now refuse until a CA is uploaded. Compile-tested and host-tested; no bench pass against a TLS broker. Per-variant table: `docs/FIRMWARE_VARIANT_AUDIT.md`. Open: form fields for the three keys in both flasher frontends; `firmware/canary`'s `securacv_mqtt` client is still plain; a Mosquitto TLS-listener step for the hub seed plan. | M |
| 13 | **(landed)** **Fleet Wi-Fi rollout sends the router password over cleartext HTTP** without telling the user, while the BLE rescue path is bonded. | The user believes the app is the safe path. | Landed. `FleetWiFiRollout.Path` gained `httpCleartext`: a candidate rides `.http` only when it is pinned https (row 5), takes the bonded BLE lane whenever its console is connected — even while online — and rides plain http only when nothing encrypted reaches it. The plan reports `needsCleartextDisclosure`; the sheet shows the disclosure ("sends it across your Wi-Fi unencrypted — anyone already on this network could read it while it's in flight") with a toggle the run cannot start without, names each row's wire, and the runner re-checks `mayPush` at every push so an unapproved cleartext target is "Not sent" with the reason. The pilot order is pinned https, then plain http, then the bonded BLE lane (`FleetWiFiRollout.plan`, proof speed): a pinned device always proves first, but when no device is pinned the proof itself rides plain http ahead of an in-range BLE device — a deliberate ordering, and the one open question on this row. `FleetWiFiRolloutTests` extended (lanes, pilot order, disclosure, gate). **Swift compile-untested here; the WAP endpoint and fixture are host-tested.** | S |
| 14 | **(landed)** **`PinnedKeyStore.pin` swallows the Keychain error**, so a failed pin leaves the device permanently "Signed" with no signal. | The trust ladder silently stalls one rung down. | Surface the error and retry on next launch. | S |
| 15 | **(landed)** **The Wall's mDNS TXT `host` is used unvalidated as a URL host** and discovered sources are never pruned. | A hostile advertiser steers the TV to any host, forever. | Validate against the same private-host rules the iOS app now uses; expire sources not seen for 30 days. | S |
| 16 | **(landed)** **Lab CSP exists on `flash.html` only**; the other 24 pages, including the webcam and microphone benches, have none. | The benches that touch the camera and mic are the least protected pages. | Landed: every `canary-local/*.html` page and the emulator harness carry a policy written by `canary-local/tools/gen_csp.py` from one table (a strict floor plus per-page rows, each with a reason; no `unsafe-*` anywhere; inline scripts and styles moved into files, and the two that cannot move — the flasher's import map and the firmware's captive page inside `wap.html` — hashed from bytes). `tests/csp.test.js` pins the policies and `tests/csp_probe.mjs` loads every page in Chromium with zero violations; both gate CI. | M |
| 17 | **(landed)** **Any LAN host can enable the display's only outbound egress (`wx_direct`)** and store a coarse location via unauthenticated `/api/set`. | Zero-phone-home is a principle a neighbor can flip. | Landed, by a different route than proposed: `POST /api/set` refuses `wx_direct` and `wx_loc` for every caller, token or not (`403 on_glass_only`), before the Origin/CSRF gate — the opt-in is reachable only on the glass (settings → weather → fetch itself). The key class is one Arduino-free, host-tested table (`canary/net/settings_policy.h`); `GET /api/settings` reports it under `on_glass` and serves the location-derived facts only to same-site callers. No bearer token and no button: the glass mints no credential, and the switch is simply not on the network. Open: an on-glass coarse-location entry (the phone app's `wx_loc` post was the only way to store one), and the on-glass caption that still says "from the app" waits for the emulator-dist rebuild wave. Compile-tested. | S |
| 18 | **(landed)** **canary-wap accepts the bearer token as a URL query parameter** on `POST /api/identify`; the kernel rejects that. | Tokens land in router and proxy logs. | Header only, like the kernel. | S |
| 19 | **(landed)** **Anti-drift vectors pin only `domain_separated_hash`**; `hash_entry`, the Ed25519 path and the document shape are never checked against kernel-produced bytes. | The Wall and the kernel can disagree on what a valid chain is with no test going red. | Emit a golden document from `cargo test` into `spec/` and load it in the Swift and Rust tests. | S |
| 20 | **(landed)** **Witness Wall "Verified through <time>" stitches the TV's verdict to a timestamp the fleet self-reported** (firmware sends "now"). | The banner asserts a time the TV did not measure. | Show the TV's own receipt time; label the device time as reported. | S |

### P2 — cohesion: one source of truth per fact

| # | Item | Why it matters | Fix | Effort |
|---|---|---|---|---|
| 21 | **(landed, waves 1–3 — 3 in part)** **The device package** — see §4. Firmware envs, emulator flavor, enclosure CAD, glTF model, fleet figures, flasher catalog and website copy are joined by hand. | Every new device is five hand-edits and three drift gates away from consistent. | Wave 1 landed: `devices/<slug>/device.json` for the 19 devices the repo builds, one JSON Schema, and `scripts/lint_device_manifests.py` (in `lint.yml`) proving every join. Wave 2 landed: the generators read the manifests. `gen_flash.py` takes each flasher product's chip, flash size, registry board and PlatformIO project from the manifest that claims it (the hand-typed `BOARD_CHIP` / `BOARD_FLASH_MB` tables and per-row declarations are gone; `flash.json` byte-identical); `gen_figures.mjs` builds the exact hardware→figure map from the manifests and validates the coarse config→device-type map against them, recording one dispute (`canary-vision/default`: DevKit vs XIAO figures under one device type) for a maintainer; `lint_build_matrix.py` applies the matrix's side of the join through the shared `scripts/_device_join.py`, so the two lints cannot disagree. Still typed by design: the confidence ladder (derived from evidence), `TWIN_ALIASES`, `build_matrix.json`'s mirrored board/mcu cells, the WAP's declared figure (until `flash.json` may move), and `CONFIG_FIGURE` pending that dispute. Wave 3 landed in part (PR wave 5, PR pending): each manifest owns its case's literal board and module knobs (`cad.params`, as numbers or as references into `canary_board_lib.scad`'s registry) and `gen_cad_params.py` writes them into the `.scad` and `--check`s them in three gates; `scripts/regen_cad.py` is the twelve-step regeneration order as one command; `gen_builder_manifest.py --site` carries the resolved knobs, the assembled seams and the board registry into the website's `cad-dims.json`, with `--site --check`. Zero `.scad` bytes moved. Still open on the row: the Nightstand C6's `model` ternary, the 7" frame's panel record, the Touch 1.69's two-knob line, the doorbell (no manifest), the website's reading of the new ledger keys, and the optional render-plan package (per-set selectors in the manifest; rewrites enclosure CI and re-homes five Lab cards — the maintainer's call). | L |
| 22 | **(landed)** **Two CSI HAL implementations** (`firmware/canary/lib/securacv_csi` vs `firmware/common/csi`) plus the sketch copy; the September pass synced them by hand. | Three copies of the most intricate driver in the project. | Landed. `securacv_csi.cpp` is a 76-line `csi::` adapter over `csi_hal::` (it was 1146 lines of HAL + extractor + a `csi_hal::` shim); `securacv_csi.h` includes `csi_types.h` instead of declaring a twin `csi_features_t`, so the module bridge's "two typedefs" rationale is gone; `firmware/canary/platformio.ini` now names `csi_hal.cpp` + `csi_features.cpp` in `build_src_filter`, so the image has exactly one `esp_wifi_set_csi_rx_cb` registration — the canonical one. Three things the canary copy did and canonical did not were ported into `csi_hal.cpp` rather than kept as a second body: `stop()` drains by advancing the consumer's own index, `get_caps()` reads the driver's `CONFIG_IDF_TARGET_*` macro as well as the sketch's board macro, and `process()` fills `v[25]` (dropped_estimate — the canary's `/api/sensing` reads it; canonical had left it 0, so canary-wap's `wifi_channel_activity` now sees it too). A `firmware/canary/include/health_log.h` bridge answers the HAL's `__has_include` probe so its diagnostics keep landing in the canary health log. Found on the way: the former shim's watchdog only ran inside a `csi_hal::process()` nobody called, so the PIO build never had a live CSI watchdog; it does now. `check_csi_sync.sh` gained five guards (name-based shared-definition check with its limits stated, no driver calls in the adapter, a 120-line budget, the header must consume `csi_types.h`, the ini must compile the HAL). **Host-tested only** — `firmware/tests_host/test_csi_hal_adapter.cpp` links the adapter against the real `csi_hal.cpp` + `csi_features.cpp` over a stubbed esp_wifi driver and pushes frames through the one registered callback (single registration, `v[25]`, the watchdog firing on the `csi::process()` path); the five `common/csi` host suites and the canary-wap CSI host suites pass; the canary envs get their compile test from `firmware.yml`'s PlatformIO leg on the PR. | M |
| 23 | **(landed)** **Display env list is typed twice** (`firmware-release.yml` vs `flasher-release.yml`) with no gate against `flavors.json`; the AMOLED was missing from every dev publish. | A flavor can ship from one button and not the other. | Landed. Both release workflows derive the Canary Display env list from `firmware/flavors.json` via a "Resolve the canary-display release envs" step (`flavor_envs.py canary-display --release --json`, consumed through `fromJSON` into the build, staging and signing steps); `flavor_envs.py --check-workflows` fails a workflow that lacks the derivation or types a `pio run -e canary-display-<env>` back in; `flasher-release.yml` overlays today's script and `flavors.json` onto tagged trees. `check_ota_channels.py` reads the same derivation (it parsed only literal loop headers before and went red on the first derived list). Host-tested only — the first real release run deserves a look at that step's log. | S |
| 24 | **(landed)** **`FEATURES.md` parity dashboard and `build_matrix.json` omit canary-display**, the most actively released product. | The parity doctrine's own dashboard does not list the flagship. | Add the display lane and let `lint_build_matrix.py` require every `flavors.json` product. | S |
| 25 | **(landed)** **Vendored `device_signature` in the canary-wap sketch has diverged** from `common/`; mesh copies now have a guard, this one does not. | Signature code drifting silently is the worst kind. | Add the pair to `check_mesh_sync.sh` or a sibling and resync. | S |
| 26 | **(landed)** **Emulator `emu_net.cpp` re-implements the Wi-Fi retry decision** instead of calling `wifi_join_policy.h`. | The emulator can diverge from the firmware it exists to preview. | Landed. `emu_net.cpp` includes `common/network/wifi_join_policy.h` directly and drives its retry through `WifiRetry` / `wifi_next_action()` with the display's own `WIFI_RETRY_*` / `WIFI_OUTAGE_REBOOT_MS` constants from `canary/config.h`; the local copy of the outage decision is deleted (it started with `ever_up = 1`, so an emulator booted with Wi-Fi off would have rebooted after five minutes — the one thing the shared rule forbids). `scripts/lint_wifi_join_policy.py` now requires every supervisor, the emulator included, to be a direct consumer — header included, policy built from the same-named symbols, no local backoff table or doubling — with fixture tests. Host-tested: syntax-checked for all five display flavors, and a linked harness confirmed no reboot from a never-associated link, the 2/4/8/16/30 s schedule with bounded jitter, and a reboot at exactly +300 s after a real drop. `dist/` is rebuilt by the pinned-emsdk workflow. | S |
| 27 | **(landed)** **Website mirrors `verify_core.js`, `kernel-status.json` and `onboarding-spec.json` by hand**; only the CAD carry is automated. | The verify page can check a chain format the kernel no longer writes. | Landed, with two corrections to this row: the verify-core mirror is `tv/vendor/verify_core.js` plus its fixtures (not `js/verify.js`, which is website-authored), and `onboarding-spec.json` is website-authored except its `builds` block. `scripts/carry_to_site.py --site <checkout>` refreshes all three byte-reproducibly (the `builds` block from `build_matrix.json`, `kernel-status.json` via `tools/gen_kernel_status.py --site`, the verifier and fixtures with their provenance file); the website's weekly carry job runs it next to the CAD carry and opens a PR only when bytes moved, and the site pins the carried bytes. Its first run landed the predicted drift: the `/checkup` build matrix was four products behind. | S |
| 28 | **(landed)** **Monorepo → HACS mirror is detect-only.** The new weekly check raises an issue; nothing pushes. | Users on HACS lag the monorepo by up to a week plus a human. | Landed: `homeassistant-mirror.yml` pushes `custom_components/securacv/` (and `conftest.py`) to the mirror as a PR on `bot/mirror-sync` on every `main` change, proving the copy exact with the mirror's own check. It needs a `MIRROR_PAT` secret (fine-grained, contents + pull-requests write on the mirror); without it the run stays green and raises one deduplicated issue saying so. | S |
| 29 | **(landed)** **Dead legacy headers in `firmware/common/` share names with live sketch modules**; `csi_hal.cpp`'s `__has_include` probe depends on which one wins. | Include order decides behavior. | Landed: six unbuilt scaffold headers (`core/log.h`, `core/version.h`, `health/health_log.h`, `network/mesh_network.h`, `rf_presence/rf_presence.h`, `web/web_ui.h`) are gone — no build, manifest, Makefile or `build.sh` reached them, and the hazard was real: the dead `health_log.h` declared a C API, not the namespace the CSI macros use, so the probe resolving to it would not have compiled. The probe itself stays, because `examples/csi_minimal` consumes `common/csi` with no host logger. | S |
| 30 | **(landed as a refactor; the decision stays open)** **The two Arduino platform lines are pinned differently across ini files.** | A board builds against two toolchains depending on the entry point. | Landed: `firmware/envs/platformio/platforms.ini` is the one source for the espressif32 / pioarduino platform pin (five sections, one per distinct literal, each saying who uses it and why); every env interpolates it, `firmware/scripts/lint_platform_pins.py` rejects a literal anywhere else, and `pio project config` resolves every env to the same string as before, so no pin value moved. What stays open is the decision `firmware/PLATFORMS.md` documents: canary floats on `^7.0.0` while the S3/C3 line pins `6.9.0`, the secure env's `^6.5.0` probably resolves to the same bytes under another spelling, and canary-ota's exact `6.5.0` may just be the version current when the project started. Any of those is a build-behavior change that needs a build per env. | S |
| 31 | **(landed)** **`die()` is defined eleven times with three behaviors** across `canary-local/tools`, `_warn()` twice, the repo-root discovery line 36 times. | Tooling scripts disagree on exit codes. | Landed: `canary-local/tools/_tooling.py` is the single definition of `die(msg, code=1)`, `warn(msg)` and `repo_root()`; the ten `die()` copies the grep actually found, both `_warn()` copies and seventeen repo-root lines are gone, and every generator imports it. `die` has one behavior: `<prog>: ERROR: <msg>` on stderr, a `::error::` annotation under GitHub Actions, exit 1 unless the caller says otherwise. `hub_seed_apply.py` stays self-contained because it is embedded and hash-pinned. | S |
| 32 | **(landed)** **Website still calls the wiring bench "The Playground"** in twelve places while the glossary now says Test bench. | Two names for one thing across two repos. | Rename the pages; the glossary term already exists. | S |
| 33 | **(landed)** **The Wall's two `online` defaults are now consistent (false) but `DISCOVERY.md` and the firmware normalizer still describe true.** | Contract doc contradicts both implementations. | Update the contract and add the field to the anti-drift vector. | S |

### P3 — CI, release and tooling hygiene

| # | Item | Fix | Effort |
|---|---|---|---|
| 34 | **(landed)** **canary-display builds 21 PlatformIO envs serially** in one 45–50 minute job; comments say 18. | Landed. `flavors.json` carries an explicit `shards` partition for canary-display (dash 6, dash-features 6, seven-inch 2, core2-panels 6, c6 1 — one `PLATFORMIO_CORE_DIR` class each); `flavor_envs.py --build-matrix` derives `firmware.yml`'s matrix with per-leg size guards, and its validation refuses a partition that drops an env or mixes core dirs. Five products build as nine legs; the display's check is now five checks named `PlatformIO Build (canary-display/<shard>)` — re-select required status checks on `main` if the old name is pinned. Timeout 90 → 60 min, unmeasured; tighten after a week of timings. | M |
| 35 | **(landed)** **Firmware SBOM is hand-written** and no longer matches the build files it cites. | Landed. `scripts/gen_firmware_sbom.py` derives CycloneDX 1.5 from `flavors.json`, every build env's resolved PlatformIO config, `platforms.ini`, the first-party library manifests and the workflows' Arduino core pins; committed at `sbom/sbom-firmware.cdx.json`, byte-gated in `lint.yml`, and the resolver is cross-checked against `pio project config` in `sbom.yml`. Still declared by hand, in one place: `PLATFORM_FACTS` (core/IDF per platform literal — a pin bump without it fails generation). Open: strict CycloneDX schema validation as a CI gate; read `sketch.yaml` core pins and assert they match the workflows'. | M |
| 36 | **(landed)** **ruff covers only `custom_components`**; the 100+ tooling scripts are unlinted (106 findings at first run). | Add `canary-local/tools` and `scripts` to the ruff step; fix in one sweep. | S |
| 37 | **(landed)** **Tooling Python is unpinned**; half the workflows run whatever `ubuntu-latest` ships while `pyproject` targets 3.11. | Landed: `pyproject.toml` carries `requires-python = ">=3.11"` as the one floor, every Python-running job sets up Python from it, and rule R9 in `CI.md` is machine-enforced by `ci_policy_check.py` (unit-tested). The resolver picks the newest interpreter that satisfies the floor; the floor is the one knob if that ever bites. | S |
| 38 | **(landed)** **Toolchain setup is hand-rolled** (PlatformIO ×15, emsdk ×2, libseccomp ×9, issue-dedup ×3) despite CI.md's composite-action rule. | Landed. Four composite actions under `.github/actions/` — `setup-platformio`, `setup-emsdk`, `setup-libseccomp`, `issue-dedup` — replace the inline copies (the real counts were 4, 2, 9 and 5); `CI.md` R10 is machine-checked by `ci_policy_check.py` (an inline PlatformIO or emsdk install fails), and R9 resolves local composite actions that carry `setup-python`. Left by design: the four `apt-get` lines that install libseccomp with other packages, and bom-pricing's per-item exception loop. Follow-up: move `setup-platformio`'s interpreter to `pyproject.toml` once a build has run on it. | M |
| 39 | **(landed)** **`gen_qr.py` output is committed with no `--check`** and runs in no workflow. | Add the flag and a line in `lint.yml`. | S |
| 40 | **(landed)** **`bom-pricing` still pushes to `main`** (now gated, still with the default token so zero CI runs on the commit). | Open a PR instead, or use the freshness PAT. | S |
| 41 | **(landed)** **CI.md's concurrency pattern evicts the pending `main` run** when merges land faster than the build. | Landed: test workflows key their group on the commit for pushes to `main` (a merge burst queues instead of evicting) and on the PR for pull requests; publishers stay per-ref under a documented exemption; no bare `cancel-in-progress: true` remains outside documented exemptions; the checker enforces it. | S |
| 42 | **(landed)** **Four dispatch-only release buttons** have not run in 60+ days and overlap "Update everything". | Landed. `release-one-click.yml`, `firmware-release-if-changed.yml` and `mac-apps-release.yml` are deleted — each was one `only:` / `force:` setting of the master button, so `RELEASE_BUTTONS.md` carries a three-row "press instead" map where it had three sections; one-click's overwrite warning moved into `release_plan.py` (the forced target's plan row says it, two new tests), and the `force` input's help text now says it takes comma-separated names. `desktop-mobile-release.yml` (the Lab as a Tauri iOS shell, never built; the iPhone/iPad app that ships is the native `ios/` companion) is retired in its own commit — a distinct capability, not a duplicate — and `desktop-lab/MOBILE.md` keeps the local recipe plus the revive path. The rule: a new thing to ship is a row in `release-targets.yml`, never a new launcher (`RELEASE_LESSONS.md`, 2026-09-08). | S |
| 43 | **(landed)** **CI never boots 3 of the 5 display flavors** in the emulator; the boot probe hardcodes the watch artifact. | Loop the probe over every `dist/*.meta.json`. | S |
| 44 | **(landed)** **Desktop Flasher release resolves `@tauri-apps/cli ^2` at release time** with no lockfile. | Landed: `desktop/package-lock.json` is committed (the same 2.11.4 the Lab pins, with every platform's optional binary recorded so a Linux-made lockfile installs on the macOS runners); the release workflow runs `npm ci` with the npm cache keyed on it, the audit workflow audits it, Dependabot has an npm entry for `/desktop`, and `RELEASE_LESSONS.md` records the lesson. | S |
| 45 | **(landed)** **`pages.yml` publishes Python generators and shell scripts** as public static files. | Stage an allowlist of web roots (the tests tree is already dropped). | S |
| 46 | **(landed)** **`dist/*.meta.json` stamps a commit unreachable from `main`** (rebuild bot ran on the PR branch). | Landed: `build.sh` stamps the merge-base of `HEAD` and `origin/main` (falling back to `HEAD`, then `dev`), and the rebuild workflow fetches `origin/main` first. The committed stamps keep the old value until the next bot rebuild — dist was not rebuilt here. | S |
| 47 | **(landed)** **`ios-selfheal` is not triggered by the linters that gate iOS sources.** | Add `workflow_run` on `lint.yml`. | S |

### P4 — docs and copy that still say the wrong thing

| # | Item | Fix |
|---|---|---|
| 48 | (landed) `CONSOLIDATION.md` tree map: counts off, seven firmware product trees missing. | Regenerate the table from `ls`; add the seven rows. |
| 49 | (landed) `ENTERPRISE_READINESS_TODO` has unchecked items CI already does, and environment-snapshot items that can never be checked. | Landed. Pruned and CI-linked in the follow-up wave; the last open box — `RUSTDOCFLAGS="-D warnings" cargo doc --no-deps` — is now a step of `rust.yml`'s build job (private items documented too), and the two rustdoc failures it found are fixed.
| 50 | (landed) `docs/homeassistant_setup.md` lists entities the integration does not create and misnames the ones it does. | Regenerate the entity table from `sensor.py`/`binary_sensor.py`. |
| 51 | (landed) Mirror README says HACS reads the icon from `brand/`; it does not, and `brand/` ships into every user's config. | Landed with the premise corrected, not carried out as written. Checked against the platform's own sources: Home Assistant 2026.3+ *does* read `custom_components/<domain>/brand/` and serves it at `/api/brands/integration/securacv/icon.png` (core PR home-assistant/core#163960; a local file beats the brands CDN, and the loader needs no manifest key), HACS's `brands` validation looks for `brand/icon.png` in the tree before `domains.json`, and the brands repository's README now points custom components at the in-repo folder. So the folder is the icon, not dead weight, and the mirror README's "HACS does not read it" (written 2026-09-02) was the false statement — the 2026-08-08 commit that added the folder had it right. Done: the four PNGs moved byte-for-byte from `brands/submission/` into `custom_components/securacv/brand/` (identical to the mirror's), so the folder is carried like every other integration file — `homeassistant-mirror.yml` lost its `--exclude='/brand/'`, the mirror's `check_mirror_sync.py` its `MIRROR_ONLY` case, the monorepo's `validate.yml` its `ignore: brands`; the two root `icon*.png` (153×193, read by nothing) are deleted; the mirror README, AGENTS.md and both workflow headers say where the icon comes from. Residual, stated in the README: Home Assistant older than 2026.3 shows the placeholder, HACS's own dashboard still draws from its feed (hacs/integration#5171), and nothing has been submitted to home-assistant/brands — `brands/home-assistant/README.md` holds the sizes (icons meet the rules; the logo is the icon in another crop) and the optional recipe. Merge order: monorepo first, or the mirror's freshness gate reports `brand/` as EXTRA against `main`. |
| 52 | (landed) Timeline card shows "Verification failed" for merely unsigned (pre-PKI) publishes. | Distinct "unsigned" state in `www/securacv-timeline-card.js`. |
| 53 | (landed) Tamper, transport, mesh and chirp entities move on unsigned publishes with no trust attribute. | Attach the same `trust` attribute the signed entities carry. |
| 54 | (landed) Options flow and the TOFU health hook have no tests. | Add to `tests/`; the mirror check will carry them. |
| 55 | (landed) `install.sh` installs from an unverified moving-branch tarball and ships tests into `/config`. | Pin to a release tag, verify a checksum, exclude `tests/`. |
| 56 | (landed) iOS README claims Secure Enclave key custody; the Keychain layer stores generic-password items. | Either adopt `kSecAttrTokenIDSecureEnclave` or say Keychain. |
| 57 | (landed) The Notification Service Extension sets `.critical` for tamper wakes without checking the entitlement. | Fall back to `.timeSensitive` like `AlertCenter`. |
| 58 | (landed) Desktop README says the Flasher builds for Windows; no target exists. | Remove the claim or add the target. |
| 59 | (landed) `docs/LAYOUT.md` on the website says GitHub Pages; `_headers` and `_redirects` only work on a Netlify-style host. | State the real host; the CSP/HSTS story depends on it. |
| 60 | (landed) No skip-to-content link on any website page; the primary nav is JS-rendered. | One link before the header in the shared template. |

---

## 4 · The device package: one manifest, every surface

This is the largest single cohesion gap and the one the September audit was
asked about directly. A Canary product today is described in at least seven
places that nothing joins:

| Surface | Where the facts live today | How it is refreshed |
|---|---|---|
| Firmware build envs | `firmware/envs/*.ini`, `flavors.json`, `build_matrix.json` | by hand; `lint_build_matrix.py` checks two of the three agree |
| Emulator flavor | `canary-local/emulator/build.sh`, `dist/*.meta.json` | the pinned-emsdk rebuild workflow |
| Enclosure CAD | `hardware/enclosure/*.scad`, `gen_stamp.py`, `gen_builder_manifest.py` | by hand, gated by byte diff |
| 3D / AR model | website `scripts/make-*-glb.mjs`, `scad/cad-dims.json` | weekly carry of the CAD ledger only |
| Fleet figures | `canary-local/devices/figures.json`, `gen_figures.mjs` | reads STL bounding boxes; gated |
| Flasher catalog | `canary-local/tools/gen_flash.py` → `flash.json` | reads `dist/*.meta.json`; gated |
| Website copy | product pages, glossary, `llms-full.txt` | by hand |

Each row has its own generator and its own gate, and the gates are good. What
is missing is the **join**: the fact that the Doorbell is 92 × 38 × 24 mm, has
an S3 with 8 MB PSRAM, an ES7210 mic and a WS2812 ring, builds from
`canary-doorbell-s3` and is `confirmed` on the confidence ladder, is not
written down once. It is written down seven times, in seven schemas.

### Proposal — as it landed

A `devices/<slug>/device.json` per product, validated by one JSON Schema
(`devices/device.schema.json`; the how-to is `devices/README.md`). The
original proposal here carried a `status` and an `envelope_mm`; the schema
as it is has neither, on purpose, and the sample below is the WAP's manifest
as it stands, not a sketch:

```jsonc
{
  "slug": "canary-wap",
  "name": "Canary WAP",
  "family": "canary-wap",                        // the firmware/flavors.json product
  "board": { "mcu": "ESP32-S3", "psram_mb": 8, "flash_mb": 8,
             "board_id": "xiao-esp32s3-sense",   // the firmware/boards/<id>/ registry row
             "envs": ["canary-wap-default", "canary-wap-usbdrive"] },
  "peripherals": ["camera", "microphone", "sd_card", "gnss_uart", "tamper_input"],
  "figure": "device.canary-wap",                 // the fleet figure — and, through it, the envelope
  "cad": {
    "scad": "docs/hardware/enclosure/canary_wap_enclosure.scad",
    "enclosure_sets": ["wap-compact", "wap-battery", "wap-weather", "wap-clip-coupon", "thermal-outdoor-kit"],
    "params": {
      "board_l": { "brd": "xiao", "dim": "l" },  // a board fact: brd_l("xiao") in canary_board_lib.scad
      "board_w": { "brd": "xiao", "dim": "w" },  // the 17.5 spec width — the clips' decision, stated
      "board_h": 1.2,                            // a case measurement: a number
      "board_clear": 0.6, "stack_camera": 8.0, "stack_plain": 4.5
    }
  },
  "flasher": { "product": "securacv-canary-wap" }
  // "emulator": { "flavor": "watch" }           — display devices only (the browser twin)
  // "site":     { "model": "models/canary-vision.glb" } — declared, not verifiable from here
}
```

- **No `status`.** The confidence ladder is derived from evidence on disk by
  the figures generator and rejected by the schema (decided in wave 1;
  `scripts/lint_device_manifests.py` prints the derived verdict).
- **No `envelope_mm`.** The outer size is reached through `figure`,
  measured, never typed: every case derives it from board dimensions plus
  walls, `gen_assembled_dims.py` measures the assembled union off the
  STLs, and the ledger carries it (`figures.json`, `cad-dims.json`) — which
  is what the website's model tests already pin to. Wave 5 took this as a
  default; a linter-asserted mirror in the manifest is a reversible later
  step, and nobody has asked for one.
- **`cad.params` in two forms.** A board fact is a reference into the board
  registry that already carries each board's evidence rung; a case
  measurement with no registry home is a number. Never a wall or a
  tolerance (the design-language canon stays case-owned, with `deviates:`
  reasons), never a selector (`preset`, `host`, `part` — chosen per
  printable set at render time).

What the generators do with it — what actually landed, per wave, against
the original list:

- `gen_flash.py` reads `board` and `flasher` and stopped inferring chips
  (wave 2, as proposed);
- `gen_figures.mjs` builds the hardware→figure map from the manifests
  (wave 2) — it never reads a status or an envelope from them: the ladder
  stays derived and the envelopes stay measured (`docs/design/FLEET_FIGURES.md`
  §5);
- `lint_build_matrix.py` resolves every matrix lane to one manifest through
  the shared `scripts/_device_join.py` (wave 2, as proposed);
- `gen_cad_params.py` **writes** `cad.params` into the case `.scad` and
  `--check`s it (wave 3) — **not** the proposed "the SCAD is parametric from
  the manifest": the file stays a literal Customizer knob, nothing reads a
  manifest at render time, and every consumer of that literal-knob contract
  (`gen_builder_manifest.py`, `gen_stamp.py`, `gen_enclosures.py`,
  `render.sh`, the fit check, `lint_design_lang.py`) is untouched;
- `gen_builder_manifest.py --site` carries the resolved knobs, the measured
  seams and the board registry into the website's `cad-dims.json` beside the
  envelopes it always carried (wave 3); the glTF generators keep pinning to
  the measured envelope, and reading the new keys is the website repo's
  change, still open;
- the emulator `build.sh` still types its flavor allowlist — the manifests
  **prove** it (every `emulator.flavor` is in the allowlist and every dist
  flavor is claimed) rather than drive it; the boot probe loops over the
  dist metadata (item 43, landed).

The gates stay exactly where they are; they have a common upstream. A new
device is: write one manifest, `python3 scripts/regen_cad.py --previews
<dir>` (the manifest into the `.scad`, the STLs, the envelopes, the figures
and their mirrors, the flashers' models, the sketch mirror — then it
**stops**), dispatch the dist rebuild and pull it, `python3
scripts/regen_cad.py --from gen_flash` for the catalogs, commit — the order
`CLAUDE.md` prescribes, now runnable, with one file at the front instead of
five.

### Migration in three waves

The counter here is the device package's own (`devices/README.md` uses it);
the PR waves in §3 are a different count.

1. **Describe** (S, **landed** — the follow-up wave, PR #1635: `devices/` and
   `scripts/lint_device_manifests.py`): write the manifests for the five
   shipping and confirmed devices from the facts as they stand; add the
   schema and a lint that every manifest validates and every `flavors.json`
   entry has one. Nothing consumes them yet, so nothing can break.
2. **Consume** (M, **landed** — PR wave 3, #1664): `lint_build_matrix.py`,
   `gen_flash.py` and `gen_figures.mjs` read the manifests, with the byte
   gates proving no generated output moved (`flash.json` and `figures.json`
   byte-identical); the leftovers are listed on row 21.
3. **Parametrize** (L, **landed in part** — PR wave 5, PR pending): the
   manifest owns the board knobs its case is cut around, and a dimension
   edit is one manifest line that `gen_cad_params.py` writes into the
   `.scad`, from which the chain that already exists re-renders the
   enclosure, re-measures the envelope, redraws the figure and re-carries
   the ledger the AR model is pinned to — `scripts/regen_cad.py` is that
   chain as one command. It landed with **zero `.scad` bytes moved**, so
   no previews were owed; the rehearsal (one WAP knob, 46 files, 26
   previews, reverted) is the proof the obligation is automated
   (`--previews DIR`), and the first real edit is the one that pays it.
   What is left, honestly: the **doorbell** has no manifest (its env is
   claimed by `canary-vision`), so `canary_vision_doorbell.scad` stays
   unowned; the **Nightstand C6**'s `board_l` / `board_w` are a `model`
   ternary, computed, which the generator refuses until someone makes
   them literal in a `.scad` PR with previews; the **7" frame** reads its
   panel record from `canary_panel_lib.scad` through `panel_variant`,
   typed there this wave; the **Touch 1.69**'s `aa_dx = 0.0; aa_dy = 0.0;`
   line holds two knobs and stays unowned until it is split (a `.scad`
   change, with previews); the **selectors** (`host`, `preset`, `radar`,
   `headers`, `port`, `model`, `panel_variant`, `part`) are chosen per
   printable set in `render.sh` — making them manifest-owned is the
   optional **render-plan** package, which rewrites `render.sh`,
   `gen_assembled_dims.py` and enclosure CI and re-homes five Lab cards,
   and was not built because it needs the maintainer's yes; **walls and
   tolerances** stay case-owned by the design-language canon; the
   **sketch envelopes** of devices with no committed CAD stay in
   `massing.mjs` (the Watch's figure among them — it will not follow a
   Watch knob until it is drawn from the parts); and the **website's
   half** — its glTF generators and copy reading `knobs`, `seams_mm`,
   `board_registry` and `board_facts` from the carried ledger — is the
   website repo's change.

---

## 5 · Wi-Fi sensing: from compiles to senses

The September pass made the CSI stack portable and correct in its indexing;
it did not make it *validated*. The path from here, in order:

1. **Transmitter filtering** (P0 item 2) — **landed** 2026-09-08, host-tested:
   one link per window (the associated router's BSSID, plus registered peers
   on the WAP), with `frames_dropped_foreign` saying how much of the
   neighborhood was kept out. Step 3 is what shows the false-positive floor
   actually moved.
2. **AGC-aware envelope and fixed-cadence Goertzel** (items 3 and 4) — **landed** in the follow-up wave, host-tested on synthetic frames; step 3 is what makes it a claim about a room.
3. **Bench pass on three boards** — S3, C3 and C6 (the HE path is compile-only
   today), following [`csi_quickstart.md`](csi_quickstart.md), with the
   `supply` object confirming which traffic source fed each window. Record
   thresholds in `docs/csi_modules.md` per board, since the C6's HE-LTF
   changes the noise floor.
4. **Wander and jitter features** in the style of espressif/esp-radar, which
   the research note evaluates: they are cheaper than the current variance
   stack and are what the upstream detector actually ships. Add as a second
   extractor behind a compile flag and compare on the bench, not in theory.
5. **Multi-device fusion** — `core.multilink_fusion` exists and is
   host-tested; feeding it real peers waits on item 2 and on the probe layer's
   airtime accounting (`csi_probe.h` now states the honest budget; the mesh
   layer still has to reserve it through `airtime_governor`).
6. **802.11bf** stays a watch item: no IDF release exposes it, and the research
   note says why the project should not build on a draft.

Everything in this section stays `compile-tested` until step 3 is written up
in [`V1_BENCH_TEST_RUNBOOK.md`](V1_BENCH_TEST_RUNBOOK.md) with the board, the
firmware sha and the numbers.

---

## 6 · Suggested sequence

Three waves, each shippable on its own:

| Wave | Contents | What it buys |
|---|---|---|
| **A — trust claims are true** | P0 items 1, 5, 6, 7; P1 items 9–20 | Every "verified", "private" and "phone-home" word in the product is backed by code |
| **B — one source per fact** | §4 waves 1–2; P2 items 22–33; P3 items 36–41 | A new device or flavor is one edit; the sync guards stop being the architecture |
| **C — sensing that senses** | P0 items 2–4, 8; §5 steps 3–5; §4 wave 3 | Bench-validated presence on three boards, and a parametric enclosure pipeline to put them in |

Wave A is the one to start tomorrow. It is almost entirely `S`-effort, it is
where a user would be misled today, and none of it needs hardware.
