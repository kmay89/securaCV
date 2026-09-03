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
| Still open | 15 in full, 6 in part |

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
(CI-backed rows; the `cargo doc` gate is still open), 36 (ruff over the
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
shipped and where it deviates from the row's original proposal. What is
left — 15 rows in full and 6 in part — is hardware-bound
(items 2, 22 and the §5 bench steps), Apple-toolchain-bound (5, 6, 13), a
maintainer decision (30's version spread, 42, 51), or a larger refactor
(12, 26, 34, 35, 38).

### P0 — wrong evidence or a broken promise a user would hit

| # | Item | Why it matters | Fix | Effort |
|---|---|---|---|---|
| 1 | **(landed, in part)** **The Wall cannot reach any sealed-log source.** The kernel serves `/api/sealed-log` but not `/api/fleet`, which is the only discovery contract the tvOS Wall implements. | The "lights up with no app change" promise in `tvos/discovery/DISCOVERY.md` is false against the only kernel that exists. | Serve `GET /api/fleet` from `src/api/mod.rs` with the same document the firmware boards serve (`firmware/common/fleet_selfreport/`), and add the anti-drift vector for it. | M |
| 2 | **CSI mixes every transmitter into one window.** Neighbor-AP beacons, peer Canaries' ESP-NOW probes and router echoes all land in the same 64-frame window; per-subcarrier variance across alternating links reads as motion. | The presence detector's false-positive floor is set by the neighborhood's Wi-Fi, not by the room. | Filter `rx_cb` on the transmitter address: accept the associated BSSID (and, when the probe layer is up, registered peers) and count the rest under a new `frames_dropped_foreign` stat. `csi_hal.cpp` already has `info->mac`. | S |
| 3 | **(landed)** **Breathing envelope is raw magnitude the driver's AGC removes.** The host test passes because synthetic frames have no automatic scaling. | `quiet` presence and `unusual_breathing` will not fire on a real device. | Landed: the envelope is now each subcarrier band's share of the per-frame-normalized row (four rotation bands, the Goertzel bank run per band, each bin keeping its strongest band), which per-packet gain cannot move; the host tests drive a 0.25 Hz breath through a simulated per-packet AGC into bin 3 and read zero in every bin through 80 s of ±30 % gain flicker. Host-tested on synthetic frames only — the bench pass (§5 step 3) is still what turns this into a claim about a room. | S |
| 4 | **(landed)** **Breathing Goertzel assumes exactly one window per second.** Window cadence is loop-driven and gaps are skipped, so the 6+3i BPM map drifts with loop latency. | Reported breaths-per-minute is a function of CPU load. | Landed: every window close carries its timestamp and the envelope is resampled onto a fixed 1 Hz grid (a close inside the previous slot is averaged into it, a gap is bridged with held copies), and `csi_stats_t` reports `windows_held`, `windows_merged` and `window_period_ms` (appended, both status endpoints surface them). Host tests: 700 ms and 1300 ms cadences both keep 12 BPM in bin 2 with the counters reporting the real pace; a 3 s stall holds two copies. The three copies (common, the sketch mirror, the embedded extractor) moved together. | S |
| 5 | **A TLS-enabled WAP is unreachable from the iOS app.** `URLSession.shared` never answers the server-trust challenge and the receipt's `tls_cert_fp` pin is discarded. | The one configuration that protects the router password in transit is the one the app cannot talk to. | A `URLSessionDelegate` that pins the receipt fingerprint (`ios/Sources/SecuraCV/Transport/DeviceAPI.swift`); reject on mismatch with a user-readable error. | M |
| 6 | **On-phone chain verification targets the wrong API.** `DeviceAPI.witness()` fetches `/api/v1/witness` (the canary-vision Node reference), while the WAP serves `/api/witness` with a different record shape and no signature. | The app's headline trust feature cannot run against any firmware in this repo. | Pick one contract, put it in `spec/`, and make the WAP handler and the Swift decoder both conform; add a fixture test on each side. | M |
| 7 | **(landed)** **Two flashers still disagree on Ed25519 refusals** in one direction: the browser now classifies them as integrity failures, but the desktop Flasher's diagnostic copy and recovery hint differ. | Half the users get the vague message (AGENTS.md rule 7). | Share the classification table as a JSON both frontends load (`canary-local/assets/flash-core.js`, `desktop/src/`). | S |
| 8 | **(landed)** **Add-on image workflow has been red on `main` for six runs.** The aarch64 QEMU leg hits the 90-minute timeout and the verify-public gate misreads a 404. | Home Assistant OS users on Raspberry Pi get no add-on image. | Landed: aarch64 builds natively on `ubuntu-24.04-arm` (no QEMU; the merge commit's QEMU leg had hit the 90-minute timeout), and `verify_published_image.sh` cross-probes GHCR with the workflow token — anonymous 404 plus authenticated 200 is a *private* package (exit 3, with the one-time visibility runbook), authenticated 404 is *not pushed* (exit 4). The 30 August failure was the former: both arches had pushed and the packages were never made public, which is the one step no workflow can do. | M |

### P1 — security posture and trust claims

| # | Item | Why it matters | Fix | Effort |
|---|---|---|---|---|
| 9 | **BLE OTA bypasses the anti-downgrade floor and has no product binding.** Documented as deliberate for rescue, but nothing else enforces it. | A paired phone can push an older signed image with a known bug. | Carry the floor and product id in the BLE manifest and check them in `firmware/common/ota/`; keep a break-glass override that logs. | M |
| 10 | **(landed)** **`glass_web` OTA check/install skip the Origin+CSRF guard** the comment says they share. | A LAN web page can start an update on a display. | Route both handlers through the existing write guard in `net/glass_web.cpp`. | S |
| 11 | **(landed, in part)** **`/api/fleet` is served with `Access-Control-Allow-Origin: *`** and now carries per-peer presence, occupant count and breathing state. | Any drive-by web page on the LAN can read who is home. | Drop the wildcard; the Wall and Lab talk to it from known origins, and the fleet contract can list them. | S |
| 12 | **Headless MQTT variants (display/sense/vision) have no TLS option** while canary-wap does; the gap is undocumented. | A broker credential crosses the LAN in the clear on three of four products. | Port the WAP's `mqtt_mgr` TLS branch into `firmware/common/network/`, and say so in `FIRMWARE_VARIANT_AUDIT.md` until it lands. | M |
| 13 | **Fleet Wi-Fi rollout sends the router password over cleartext HTTP** without telling the user, while the BLE rescue path is bonded. | The user believes the app is the safe path. | Prefer BLE when bonded; otherwise show the disclosure once and require the TLS receipt (item 5). | S |
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
| 21 | **(landed, wave 1)** **The device package** — see §4. Firmware envs, emulator flavor, enclosure CAD, glTF model, fleet figures, flasher catalog and website copy are joined by hand. | Every new device is five hand-edits and three drift gates away from consistent. | Wave 1 landed: `devices/<slug>/device.json` for the 19 devices the repo builds, one JSON Schema, and `scripts/lint_device_manifests.py` (in `lint.yml`) proving every join — each env exists in its family's PlatformIO config and every `flavors.json` env is claimed exactly once, the emulator flavor has a dist artifact, the figure exists and its ladder verdict is *derived*, never typed, the flasher product exists with the same chip, every `build_matrix.json` product has a manifest, and the CAD paths exist. Nothing consumes the manifests yet; waves 2 and 3 (§4) are open. | L |
| 22 | **Two CSI HAL implementations** (`firmware/canary/lib/securacv_csi` vs `firmware/common/csi`) plus the sketch copy; the September pass synced them by hand. | Three copies of the most intricate driver in the project. | Make `securacv_csi` a thin include of `common/csi`; extend `check_csi_sync.sh` to fail on any body divergence meanwhile. | M |
| 23 | **(landed, in part)** **Display env list is typed twice** (`firmware-release.yml` vs `flasher-release.yml`) with no gate against `flavors.json`; the AMOLED was missing from every dev publish. | A flavor can ship from one button and not the other. | Both workflows read the matrix from `flavors.json` via one script. | S |
| 24 | **(landed)** **`FEATURES.md` parity dashboard and `build_matrix.json` omit canary-display**, the most actively released product. | The parity doctrine's own dashboard does not list the flagship. | Add the display lane and let `lint_build_matrix.py` require every `flavors.json` product. | S |
| 25 | **(landed)** **Vendored `device_signature` in the canary-wap sketch has diverged** from `common/`; mesh copies now have a guard, this one does not. | Signature code drifting silently is the worst kind. | Add the pair to `check_mesh_sync.sh` or a sibling and resync. | S |
| 26 | **Emulator `emu_net.cpp` re-implements the Wi-Fi retry decision** instead of calling `wifi_join_policy.h`. | The emulator can diverge from the firmware it exists to preview. | Include the shared header; delete the copy. | S |
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
| 34 | **canary-display builds 21 PlatformIO envs serially** in one 45–50 minute job; comments say 18. | Shard by board family; read the count from `flavors.json`. | M |
| 35 | **Firmware SBOM is hand-written** and no longer matches the build files it cites. | Generate from `platformio.ini` and `library.json` in CI; byte-gate it. | M |
| 36 | **ruff covers only `custom_components`**; the 100+ tooling scripts are unlinted (106 findings at first run). | Add `canary-local/tools` and `scripts` to the ruff step; fix in one sweep. | S |
| 37 | **(landed)** **Tooling Python is unpinned**; half the workflows run whatever `ubuntu-latest` ships while `pyproject` targets 3.11. | Landed: `pyproject.toml` carries `requires-python = ">=3.11"` as the one floor, every Python-running job sets up Python from it, and rule R9 in `CI.md` is machine-enforced by `ci_policy_check.py` (unit-tested). The resolver picks the newest interpreter that satisfies the floor; the floor is the one knob if that ever bites. | S |
| 38 | **Toolchain setup is hand-rolled** (PlatformIO ×15, emsdk ×2, libseccomp ×9, issue-dedup ×3) despite CI.md's composite-action rule. | Four composite actions; CI.md rule R9 to require them. | M |
| 39 | **(landed)** **`gen_qr.py` output is committed with no `--check`** and runs in no workflow. | Add the flag and a line in `lint.yml`. | S |
| 40 | **`bom-pricing` still pushes to `main`** (now gated, still with the default token so zero CI runs on the commit). | Open a PR instead, or use the freshness PAT. | S |
| 41 | **(landed)** **CI.md's concurrency pattern evicts the pending `main` run** when merges land faster than the build. | Landed: test workflows key their group on the commit for pushes to `main` (a merge burst queues instead of evicting) and on the PR for pull requests; publishers stay per-ref under a documented exemption; no bare `cancel-in-progress: true` remains outside documented exemptions; the checker enforces it. | S |
| 42 | **Four dispatch-only release buttons** have not run in 60+ days and overlap "Update everything". | Retire or fold in; `RELEASE_BUTTONS.md` shrinks. | S |
| 43 | **(landed)** **CI never boots 3 of the 5 display flavors** in the emulator; the boot probe hardcodes the watch artifact. | Loop the probe over every `dist/*.meta.json`. | S |
| 44 | **(landed)** **Desktop Flasher release resolves `@tauri-apps/cli ^2` at release time** with no lockfile. | Landed: `desktop/package-lock.json` is committed (the same 2.11.4 the Lab pins, with every platform's optional binary recorded so a Linux-made lockfile installs on the macOS runners); the release workflow runs `npm ci` with the npm cache keyed on it, the audit workflow audits it, Dependabot has an npm entry for `/desktop`, and `RELEASE_LESSONS.md` records the lesson. | S |
| 45 | **(landed)** **`pages.yml` publishes Python generators and shell scripts** as public static files. | Stage an allowlist of web roots (the tests tree is already dropped). | S |
| 46 | **(landed)** **`dist/*.meta.json` stamps a commit unreachable from `main`** (rebuild bot ran on the PR branch). | Landed: `build.sh` stamps the merge-base of `HEAD` and `origin/main` (falling back to `HEAD`, then `dev`), and the rebuild workflow fetches `origin/main` first. The committed stamps keep the old value until the next bot rebuild — dist was not rebuilt here. | S |
| 47 | **(landed)** **`ios-selfheal` is not triggered by the linters that gate iOS sources.** | Add `workflow_run` on `lint.yml`. | S |

### P4 — docs and copy that still say the wrong thing

| # | Item | Fix |
|---|---|---|
| 48 | (landed) `CONSOLIDATION.md` tree map: counts off, seven firmware product trees missing. | Regenerate the table from `ls`; add the seven rows. |
| 49 | (landed, in part) `ENTERPRISE_READINESS_TODO` has unchecked items CI already does, and environment-snapshot items that can never be checked. | Prune; link the CI job for each checked row. |
| 50 | (landed) `docs/homeassistant_setup.md` lists entities the integration does not create and misnames the ones it does. | Regenerate the entity table from `sensor.py`/`binary_sensor.py`. |
| 51 | Mirror README says HACS reads the icon from `brand/`; it does not, and `brand/` ships into every user's config. | Move brand assets to the HACS brands repo; delete from the mirror. |
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

### Proposal

A `devices/<slug>/device.json` per product, validated by one JSON Schema, with
these sections:

```jsonc
{
  "slug": "canary-doorbell",
  "name": "Canary Doorbell",
  "status": "confirmed",              // the ladder; derived evidence lives beside it
  "board": { "mcu": "esp32s3", "psram_mb": 8, "flash_mb": 16, "envs": ["canary-doorbell-s3"] },
  "peripherals": ["es7210", "ws2812x12", "ov2640"],
  "envelope_mm": { "w": 92, "h": 38, "d": 24 },  // the CAD ledger number
  "cad": { "scad": "hardware/enclosure/doorbell.scad", "params": { "wall": 2.0, "bezel_color": "graphite" } },
  "emulator": { "flavor": "doorbell", "face": "portrait" },
  "flasher": { "chip": "esp32s3", "offsets": "s3-16mb" },
  "site": { "page": "canary-doorbell.html", "model": "models/canary-doorbell.glb" }
}
```

The existing generators change from *authoring* facts to *consuming* them:

- `gen_builder_manifest.py` and `gen_stamp.py` read `envelope_mm` and
  `cad.params`, so the SCAD is parametric from the manifest, not from
  constants at the top of the file;
- `gen_builder_manifest.py --site` carries `envelope_mm` into the website's
  `cad-dims.json` as it does today, and the glTF generators keep pinning to it;
- `gen_figures.mjs` reads `status` and `envelope_mm` instead of a second
  `figures.json`;
- `gen_flash.py` reads `board` and `flasher` and stops inferring chips;
- `lint_build_matrix.py` requires every `envs` entry to exist in
  `firmware/envs/` and every `firmware/envs/` entry to be claimed by a device;
- the emulator `build.sh` iterates `emulator.flavor` across devices, and the
  boot probe (item 43) follows.

The gates stay exactly where they are; they just get a common upstream. A new
device becomes: write one manifest, run `./setup.sh regen`, dispatch the dist
rebuild, run the catalogs, commit — the order `CLAUDE.md` already prescribes,
with one file at the front instead of five.

### Migration in three waves

1. **Describe** (S, **landed** — `devices/` and `scripts/lint_device_manifests.py`): write the manifests for the five shipping and confirmed
   devices from the facts as they stand; add the schema and a lint that every
   manifest validates and every `flavors.json` entry has one. Nothing consumes
   them yet, so nothing can break.
2. **Consume** (M): switch `lint_build_matrix.py`, `gen_flash.py` and
   `gen_figures.mjs` to read the manifests, one generator per PR, with the
   byte gates proving the output did not move.
3. **Parametrize** (L): thread `cad.params` and `envelope_mm` into the SCAD
   sources and the glTF generators, so a dimension change is one edit that
   re-renders the enclosure, the AR model and the figure together. This is the
   wave that needs the render previews AGENTS.md requires with every SCAD
   change.

---

## 5 · Wi-Fi sensing: from compiles to senses

The September pass made the CSI stack portable and correct in its indexing;
it did not make it *validated*. The path from here, in order:

1. **Transmitter filtering** (P0 item 2) — without it nothing downstream can
   be tuned against a real room.
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
