# Firmware Variant Parity Plan — canary (ACTIVE) ⇄ canary-wap (Arduino)

> **Decision (founder, this cycle):** do **not** pick one tree — bring **both** the ACTIVE
> modular tree (`firmware/canary/`, PlatformIO) and the `canary-wap` Arduino tree to **full
> bidirectional feature parity**, and ship both as supported v1 images.
>
> **Source of truth:** the CI-guarded dashboard in [`FEATURES.md`](FEATURES.md)
> (`.github/workflows/features-dashboard-guard.yml` blocks any ✅→⚠️/❌ regression without an issue
> ref). This plan is the *closure program* for the cells where the two trees disagree today.
>
> **Companion:** [`../docs/V1_LAUNCH_REVIEW.md`](../docs/V1_LAUNCH_REVIEW.md) (§4 decisions) ·
> [`../docs/V1_BENCH_TEST_RUNBOOK.md`](../docs/V1_BENCH_TEST_RUNBOOK.md) (on-device proof) ·
> [`VARIANT_POLICY.md`](VARIANT_POLICY.md) + [`FIRMWARE_VARIANT_AUDIT.md`](FIRMWARE_VARIANT_AUDIT.md)
> (lifecycle + de-rot plan — note its item #6 recommends *convergence*, which §2 reconciles with
> this *parity* decision).
>
> **Honest framing:** this is a **multi-PR program**, not a one-shot. Several gaps are behavioral
> (mesh, BLE, RF, acoustic) and can only be *closed* — i.e. rated ✅ — once proven on hardware
> (see the bench runbook). Pre-bench, we land the code so it builds CI-green; the dashboard cell
> flips to ✅ when the bench test backs it.

---

## 1. Where the two trees disagree today

Pulled directly from the `FEATURES.md` dashboard (canary **PIO/ACTIVE** vs canary-wap
**Arduino**). Cells where both are equal (the entire core crypto/GPS/SD/CSI/health/battery suite
is already at parity ✅) are omitted.

### Group A — Arduino has it, ACTIVE is partial/absent → **port INTO ACTIVE**

| Capability | ACTIVE | Arduino | Closure is… |
|---|:---:|:---:|---|
| Web UI (embedded PROGMEM dashboard) | ⚠️ | ✅ | code (compile + `selftest-ui`) |
| Camera peek (MJPEG, no frame storage) | ⚠️ | ✅ | code + **bench** (needs camera) |
| Mesh network (Opera / ESP-NOW) | ⚠️ | ✅ | wired already; **bench** (#610) |
| Mesh RSSI from ESP-NOW radio | ⚠️ | ✅ | code + bench |
| BLE discovery (Opera/Chirp/Nearby) | ❌ | ✅ | code + bench |
| RF presence detection | ❌ | ✅ | code + bench |
| Hub failover election | ⚠️ | ✅ | code + **multi-board bench** |
| Chirp channel (broadcast beacon) | ⚠️ | ✅ | code + **two-board bench** |

### Group B — ACTIVE has it, Arduino absent → **port INTO Arduino**

| Capability | ACTIVE | Arduino | Closure is… |
|---|:---:|:---:|---|
| MQTT publish + HA Discovery | ✅ | ❌ | code (compile + hassfest) |
| OTA A/B with rollback safety | ✅ | ❌ | code + bench (rollback) |
| Acoustic alarm-cadence (T3 smoke / T4 CO) | ✅ | ❌ | code + **bench** (mic) |
| Capacitive touch (panic / tamper) | ✅ | ❌ | code + bench |
| Native deep-sleep HAL | ✅ | ❌ | code + bench |
| IR appliance activity (RMT) | ✅ | ❌ | code + bench |
| Internal temp drift tamper | ✅ | ❌ | code + bench |
| Sensing events signed into chain | ✅ | ❌ | code + host test |
| HA MQTT discovery for sensing (11 entities) | ✅ | ❌ | code (compile + hassfest) |
| Sensing dashboard panel | ✅ | ❌ | code + `selftest-ui` |
| WiFi power save (modem sleep / TX ctl) | ✅ | ❌ | code + bench |
| WiFi auto-reconnect (exp. backoff) | ✅ | ❌ | code (compile) |

### Shared gap (neither tree, not a parity item but on the v1 path)
- **TLS (HTTPS self-signed): ❌ / ❌ in the dashboard.** The `canary-wap` sketch *contains* an
  `httpd_ssl_start`-on-443 path, but it's gated on `SECURACV_HAS_HTTPS_SERVER` (ESP-IDF config) +
  a provisioned cert, so the dashboard honestly rates default builds ❌. Enabling it for real is
  the shared TLS gap (launch review §2) and Track D in the bench runbook — do it once and
  mirror into both trees.

---

## 2. The architectural reality that sets the effort

Parity is **not** copy-paste in either direction, because the two trees are structured oppositely:

- **ACTIVE is modular** (`firmware/canary/lib/securacv_*`, composed in `src/main.cpp` via
  `FEATURE_*` flags). Porting an Arduino capability INTO ACTIVE means **refactoring the monolithic
  logic into a `securacv_*` library** with a HAL-clean interface — higher up-front cost, but it's
  the long-term home (per `VARIANT_POLICY.md`, ACTIVE is the lane meant to eventually supersede
  COMPATIBILITY).
- **Arduino is one ~2,700-line sketch** under a binary-size budget (the firmware CI fails over the
  3 MB ceiling). Porting ACTIVE capabilities INTO it **grows the sketch** — watch the size gate,
  and prefer `#if FEATURE_*` gating so variants can drop weight.
- **The dashboard guard is a ratchet:** never let a ✅ cell regress; new work only moves cells
  ⚠️/❌ → ✅, and only once the capability is genuinely functional (compile-only ≠ ✅ for
  behavioral features — keep those ⚠️ until the bench runbook backs them).

> **Strategic note to keep visible:** maintaining two trees at parity is an **ongoing** cost, not
> a one-time push. Parity is the right v1 bridge (ship both, lose no capability), but the
> `VARIANT_POLICY` end-state is ACTIVE-supersedes-COMPATIBILITY. Worth revisiting post-v1 whether
> to converge rather than mirror forever.

---

## 3. Sequenced closure program

**Step 0 — Baseline & scorecard (pre-bench, safe, do first).**
- Re-audit the eight Group-A ⚠️ cells against current code (mesh is now *wired* in
  `main.cpp` — its ⚠️ reflects "unproven on hardware," not "absent"; don't pre-flip to ✅).
- Stand up the parity scorecard (§4) and link it from `FEATURES.md`.

**Tranche A1 — ACTIVE, compile-closeable (no hardware to land CI-green):**
Web UI, WiFi auto-reconnect-style robustness, and the mesh/chirp/RF/BLE *code presence* (build
under `[env:full]`). Each lands behind its `FEATURE_*` flag and must pass the PlatformIO +
host-test jobs. Behavioral cells stay ⚠️ pending Tranche-bench.

**Tranche B1 — Arduino, compile-closeable:**
MQTT publish + HA Discovery (incl. sensing-entity discovery), WiFi auto-reconnect, sensing
dashboard panel, capacitive touch / IR / temp-tamper / deep-sleep scaffolding. Land behind
`#if FEATURE_*`; verify the size gate and `hassfest`/`selftest-ui` jobs.

**Tranche A2 / B2 — bench-gated behavior:**
Everything marked "**bench**" above (mesh, RSSI, BLE discovery, RF presence, hub failover, chirp,
camera peek, acoustic, OTA rollback, power save). Land the code in A1/B1; **flip the dashboard
cell to ✅ only after the matching bench-runbook track passes** and an artifact is captured.

**Shared — TLS:** execute runbook Track D once; mirror the enablement into both trees.

---

## 4. Parity scorecard (fill as cells converge)

| Direction | Capability | Code landed (CI-green) | Bench-proven | Dashboard ✅ |
|---|---|:---:|:---:|:---:|
| →ACTIVE | Web UI dashboard | ☐ | n/a | ☐ |
| →ACTIVE | Camera peek (MJPEG) | ☐ | ☐ | ☐ |
| →ACTIVE | Mesh network | ☐ (wired) | ☐ (#610) | ☐ |
| →ACTIVE | Mesh RSSI | ☐ | ☐ | ☐ |
| →ACTIVE | BLE discovery | ☐ | ☐ | ☐ |
| →ACTIVE | RF presence | ☐ | ☐ | ☐ |
| →ACTIVE | Hub failover | ☐ | ☐ | ☐ |
| →ACTIVE | Chirp channel | ☐ | ☐ | ☐ |
| →Arduino | MQTT + HA Discovery | ☐ | ☐ | ☐ |
| →Arduino | OTA A/B rollback | ☐ | ☐ | ☐ |
| →Arduino | Acoustic T3/T4 | ☐ | ☐ | ☐ |
| →Arduino | Capacitive touch | ☐ | ☐ | ☐ |
| →Arduino | Deep-sleep HAL | ☐ | ☐ | ☐ |
| →Arduino | IR appliance | ☐ | ☐ | ☐ |
| →Arduino | Internal temp tamper | ☐ | ☐ | ☐ |
| →Arduino | Sensing→chain signing | ☐ | ☐ | ☐ |
| →Arduino | Sensing HA discovery (11) | ☐ | ☐ | ☐ |
| →Arduino | Sensing dashboard panel | ☐ | n/a | ☐ |
| →Arduino | WiFi power save | ☐ | ☐ | ☐ |
| →Arduino | WiFi auto-reconnect | ☐ | n/a | ☐ |
| shared | TLS (HTTPS) enabled | ☐ | ☐ | ☐ |

**Exit:** every row ✅ in `FEATURES.md` for both `canary (PIO)` and `canary-wap (Arduino)` columns,
with the bench-gated rows backed by a runbook artifact.
