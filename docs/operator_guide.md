# Operator Guide (CLI & Ingest Reference)

Deep reference for operators and developers. The top-level [README](../README.md) covers the
consumer/Home-Assistant path; this guide holds the command-line workflows and the per-source
ingest setup that used to live inline in the README.

See also: [`docs/rtsp_setup.md`](rtsp_setup.md), [`docs/v4l2_setup.md`](v4l2_setup.md),
[`docs/esp32_s3_setup.md`](esp32_s3_setup.md), [`docs/container.md`](container.md),
[`docs/homeassistant_setup.md`](homeassistant_setup.md), [`docs/frigate_integration.md`](frigate_integration.md),
[`docs/sd_card_health.md`](sd_card_health.md) (card selection, endurance monitoring, replacement runbook),
[`docs/timestamping.md`](timestamping.md) (RFC 3161 anchors: third-party proof of when the chain existed).

**Prerequisites:** a clone of this repository and a Rust toolchain
([rustup](https://rustup.rs)); on Ubuntu/Debian also
`build-essential libseccomp-dev pkg-config`. Every command below runs from
the repository root.

---

## Demo & tamper evidence

```bash
cargo run --bin demo
cargo run --bin log_verify -- --db demo_witness.db
```

The demo writes `./demo_witness.db`, `./demo_out/export_bundle.json`, and the vault directory
at `./vault/envelopes` (override with `--out` / `--vault`).

### Tamper demo

```bash
cargo run --bin demo
cargo run --bin log_verify -- --db demo_witness.db
cargo run --bin export_verify -- --db demo_witness.db --bundle demo_out/export_bundle.json
printf "\n" >> demo_out/export_bundle.json
cargo run --bin export_verify -- --db demo_witness.db --bundle demo_out/export_bundle.json  # should FAIL
```

## Device public key location

The device Ed25519 **verifying key** is stored locally in the witness database at
`device_metadata.public_key` (row `id = 1`). External verification tools like `log_verify` read
this public key from the database by default, or accept an explicit key via `--public-key` /
`--public-key-file` as documented in `docs/log_verify.md`.

## Break-glass policy management

The break-glass CLI stores the quorum policy in the kernel database so `break_glass authorize`
can evaluate approvals against a persistent trustee roster.

### Guided setup (`init` + `trustee enroll`)

The easiest way to stand up a quorum. `init` pins the device identity and opens an
`n`-of-`m` **setup draft**; `trustee enroll` adds trustees one at a time. The quorum
policy commits automatically when the roster is **complete** — the moment the
`m`-th trustee is enrolled — and not before: earlier enrollments edit only the draft,
so you never hand-assemble a policy or hold a partial one. Growing or shrinking a
live roster afterwards is a quorum-gated policy change (`policy propose` →
`policy approve` → `policy set --approvals`). For running any of this as a witnessed
ceremony — who is in the room, what is read aloud, what is retained — see
[`security/CEREMONY_RUNBOOK.md`](security/CEREMONY_RUNBOOK.md):

```bash
DEVICE_KEY_SEED=devkey:your-seed \
  cargo run --bin break_glass -- init --threshold 2 --trustees 3 --db witness.db

# Import a trustee who generated their own key and sent you the public half:
DEVICE_KEY_SEED=devkey:your-seed \
  cargo run --bin break_glass -- trustee enroll --id alice --public-key 0123... --db witness.db

# …or mint a keypair for them (writes the signing key at mode 0600 to hand over):
DEVICE_KEY_SEED=devkey:your-seed \
  cargo run --bin break_glass -- trustee enroll --id bob --generate --output bob.key --db witness.db
```

The draft is a plain file next to the database (`<db>.setup-draft.json`, override with
`--draft`). It holds only public keys and counts — no secrets. It is kept **separate**
from the committed quorum policy on purpose: `QuorumPolicy::validate` rejects an empty
or partial roster (Invariant V), so a half-finished enrollment can never be mistaken
for a live gate. When you're done, rehearse with [`drill`](#rehearse-it-drill) and
confirm with [`doctor`](#health-check-doctor).

### Manual policy (`policy set`)

To set the whole roster in one shot (e.g. from an existing key inventory):

```bash
DEVICE_KEY_SEED=devkey:your-seed \
  cargo run --bin break_glass -- policy set \
  --threshold 2 \
  --trustee alice:0123... \
  --trustee bob:4567... \
  --db witness.db

DEVICE_KEY_SEED=devkey:your-seed \
  cargo run --bin break_glass -- policy show --db witness.db
```

Trustee entries use the format `id:HEX_PUBLIC_KEY`, where the public key is the hex-encoded
32-byte Ed25519 verifying key.

**Auditing without the signing seed.** `receipts`, `policy history`, and `policy show`
open the encrypted database with either `--device-key-seed` or the database key alone
(`--db-key` / `SECURACV_DB_KEY`), which `break_glass db-key --device-key-seed …` derives.
Hand an observer or relying party that key plus the device's public key file — never
`DEVICE_KEY_SEED`, which signs every ledger. Pass `--public-key-file device.pub` to
`receipts` and `policy history` for a pinned verdict; without it both tools say
`Verifying key: read from the audited database — … self-consistent; identity unverified`,
because a key read out of the database being audited proves internal consistency only.

### Health check (`doctor`)

Before you rely on the vault, confirm it is actually set up correctly:

```bash
DEVICE_KEY_SEED=devkey:your-seed \
  cargo run --bin break_glass -- doctor --db witness.db --vault-path vault/envelopes
```

`doctor` is read-only. It reports whether a quorum policy is configured (`n`-of-`m`,
with every trustee key well-formed), whether the device identity is pinned, and the
state of the vault master key — including a loud reminder that the master key is
plaintext on disk (the honest state until hardware-backed keys land; see
[the v1.1 design](design/vault_operator_ux_v1_1.md)). It **exits non-zero if
anything is missing or invalid**, so it can gate a deploy in a script or CI step.

### Rehearse it (`drill`)

Prove the whole break-glass path works *before* you need it at 3 a.m.:

```bash
cargo run --bin break_glass -- drill --threshold 2 --trustees 3
```

`drill` runs a full request → approve → authorize → seal → unseal cycle in a
**throwaway sandbox** — a temporary database, a temporary vault, and ephemeral
trustee keys, all discarded when it finishes. It touches nothing real. It seals a
dummy payload behind a fresh `n`-of-`m` quorum, unseals it with a second quorum
token, and confirms the recovered bytes match byte-for-byte, then prints
**DRILL PASSED** (exit 0) or fails non-zero. Use it to confirm a build/host can
actually execute break-glass, and to let trustees practice the flow with zero risk.

## Break-glass unseal workflow

Ensure a quorum policy is stored first (`break_glass policy set`). Then create an unlock request,
gather trustee approvals, and authorize the request before unsealing. A request names who is
asking (`--requested-by`, your printed name) and why, as a reason code from a closed vocabulary
(`--reason`; pass an invalid value to see the list) plus free text (`--purpose`); all of it is
bound into the hash trustees sign and recorded in the receipt. The authorization step logs a
receipt (granted or denied) and issues a sensitive token file via `--output-token`. The unseal
command writes the clear envelope to `--output-dir` (default: `vault/unsealed`).

```bash
cargo run --bin break_glass -- request \
  --envelope <envelope_id> \
  --purpose "incident response" \
  --requested-by "Alice Operator" \
  --reason incident-review \
  --ruleset-id ruleset:v0.3.0 \
  --output-request unlock.request

# Each trustee, on their own machine — the tool recomputes the hash from the
# file's fields and shows them before signing (never a dictated hash):
cargo run --bin break_glass -- approve \
  --request unlock.request \
  --trustee alice \
  --signing-key /path/to/alice.signing.key \
  --output alice.approval

DEVICE_KEY_SEED=devkey:your-seed \
  cargo run --bin break_glass -- authorize \
  --request unlock.request \
  --approvals alice.approval,bob.approval \
  --db witness.db \
  --ruleset-id ruleset:v0.3.0 \
  --output-token /path/to/break_glass.token

cargo run --bin break_glass -- unseal \
  --envelope <envelope_id> \
  --token /path/to/break_glass.token \
  --db witness.db \
  --ruleset-id ruleset:v0.3.0 \
  --vault-path vault/envelopes \
  --output-dir vault/unsealed
```

### Break-glass web console

`break_glass_serve` hosts a single-page console (default `http://127.0.0.1:8800/breakglass`)
that walks the same four phases without file shuffling:

1. **Connect** with the capability token from the server's token file.
2. **Open a request** — you name the envelope, the purpose, yourself, and a reason code; the
   server computes the request hash over all of it, and the page produces a **trustee signing
   link** (`…/breakglass#sign&hash=…&envelope=…&purpose=…&by=…`) carrying every field the hash
   binds, to send to each trustee.
3. **Collect approvals** — each trustee opens their link, which shows a signer-only page
   (no token needed; it makes no server calls — the fragment never leaves their browser). The
   page displays the request's fields, **recomputes the request hash from them in the browser**,
   and enables signing only if it matches the hash in the link — a link whose fields were altered
   is refused, and so is a link that carries only the hash (a trustee who receives one signs from
   the request file with `break_glass approve --request` instead). The link carries the request
   fields in clear text and stays in the trustee's browser history, so send it over the same
   precommitted channel you would use for the request itself. The trustee signs in-browser with
   their key seed and sends you back the signature to paste. Offline trustees can keep using
   `break_glass approve --request`.
4. **Quorum & unseal** — status now auto-refreshes every few seconds with a per-trustee
   signed/pending table and a progress bar; **Unseal** enables when quorum is met.

Every attempt, granted or denied, is recorded as a tamper-evident receipt either way.

## Event export

Write a local artifact with coarse time buckets and batched events (no precise timestamps or
identity selectors). There are two authorization modes; **every export, in either mode, appends
a signed, hash-chained receipt** to the tamper-evident log, labeled with its `auth_mode` — so
disclosures are always auditable.

**Owner self-export** — your everyday "download my events". Possession of the device key seed is
the credential (the seed both decrypts the database and signs the receipt; without it the export
cannot run at all). This exports the same privacy-filtered artifact the local API serves:

```bash
DEVICE_KEY_SEED=devkey:your-seed \
  cargo run --bin export_events -- \
  --db-path witness.db \
  --self-export \
  --output witness_export.json
```

**Break-glass export** — trustee-quorum authorization for the same artifact, when you want a
disclosure countersigned by your trustees (e.g. handing evidence to a third party).
`--break-glass-token` takes the token file produced by the break-glass `authorize` step above
(`--output-token`). Sealed-vault evidence and unsealing always require break-glass — self-export
never touches the vault.

```bash
DEVICE_KEY_SEED=devkey:your-seed \
  cargo run --bin export_events -- \
  --db-path witness.db \
  --break-glass-token /path/to/break_glass.token \
  --output witness_export.json
```

**Time windows** — restrict the export to a range with `--last 24h` (also `7d`, `90m`) or
`--start <epoch_s> --end <epoch_s>`. Windows are aligned outward to 600 s bucket boundaries
(start floored, end ceiled) and the aligned window is printed and recorded on the signed
receipt, so a window can never disclose finer-than-bucket timing.

`export_events` emits a single JSON artifact with batched buckets, applying default jitter and
batching unless overridden by CLI flags. Exported times are intentionally coarse: 10-minute
buckets with ±120 s jitter by default (see `spec/event_contract.md` and `docs/why_secure.md`
for why) — don't be surprised that bucket labels differ slightly from wall-clock time.

**Scheduled exports** — `--output-dir DIR --keep N` writes rotating
`securacv-events-<bucket>.json` files for cron/systemd timers; see
[`docs/scheduled_exports.md`](scheduled_exports.md) for ready-made units. The event API offers
the same as a one-click download: token-gated `GET /export/bundle[?last=24h|?start=&end=]`
returns the signed bundle as a file (surfaced as the **Download my events** button in the
Home Assistant add-on panel).

---

## RTSP camera setup

1. Install RTSP dependencies (GStreamer or FFmpeg):
   ```bash
   # GStreamer backend
   sudo apt-get install libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
       gstreamer1.0-plugins-good gstreamer1.0-plugins-bad libseccomp-dev
   # OR FFmpeg backend
   sudo apt-get install libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libseccomp-dev
   ```
2. Build with RTSP support:
   ```bash
   cargo build --release --features rtsp-gstreamer   # or rtsp-ffmpeg
   ```
3. Configure your camera (`witness.toml`):
   ```toml
   [rtsp]
   url = "rtsp://admin:password@192.168.1.100:554/stream1"
   target_fps = 10
   width = 640
   height = 480
   backend = "auto" # auto, gstreamer, ffmpeg

   [zones]
   module_zone_id = "zone:front_door"
   ```
4. Run:
   ```bash
   export DEVICE_KEY_SEED=$(openssl rand -hex 32)
   WITNESS_CONFIG=witness.toml cargo run --release --features rtsp-gstreamer --bin witnessd
   ```

See [`docs/rtsp_setup.md`](rtsp_setup.md) for camera URL patterns and troubleshooting.

**Architecture:** `witnessd` decodes RTSP in-memory and produces `RawFrame` values without
writing frames to disk. Time coarsening and non-invertible feature hashing happen at capture
time. GStreamer is gated behind `rtsp-gstreamer`, FFmpeg behind `rtsp-ffmpeg`; `auto` prefers
GStreamer. The `stub://` scheme is test/dev only and is rejected in release builds.

## V4L2 camera setup (USB / local devices)

1. Build: `cargo build --release --features ingest-v4l2`
2. Configure (`witness.toml`):
   ```toml
   [ingest]
   backend = "v4l2"

   [v4l2]
   device = "/dev/video0"
   target_fps = 10
   width = 640
   height = 480

   [zones]
   module_zone_id = "zone:front_door"
   ```
3. Run:
   ```bash
   export DEVICE_KEY_SEED=$(openssl rand -hex 32)
   WITNESS_CONFIG=witness.toml cargo run --release --features ingest-v4l2 --bin witnessd
   ```

See [`docs/v4l2_setup.md`](v4l2_setup.md). The V4L2 backend never writes raw frames to disk and
never exposes them over the network.

## ESP32-S3 camera setup (HTTP MJPEG/JPEG or UDP RTP)

1. Build: `cargo build --release --features ingest-esp32`
2. Configure (`witness.toml`):
   ```toml
   [ingest]
   backend = "esp32"

   [esp32]
   url = "http://192.168.1.50:81/stream"
   target_fps = 10

   [zones]
   module_zone_id = "zone:front_door"
   ```
3. Run:
   ```bash
   export DEVICE_KEY_SEED=$(openssl rand -hex 32)
   WITNESS_CONFIG=witness.toml cargo run --release --features ingest-esp32 --bin witnessd
   ```

See [`docs/esp32_s3_setup.md`](esp32_s3_setup.md).

## Tract (ONNX) detection backend

Local ONNX inference for object detection. Feature-gated (`backend-tract`); requires a **local**
model file and explicit width/height (RTSP or V4L2 ingest only).

Fetch the recommended small model (Apache-2.0) with one command — it downloads and
**SHA-256-verifies** the model into `vendor/models/` (a one-time, operator-initiated step; the
kernel itself never downloads models at runtime):

```bash
bash scripts/fetch_detection_model.sh
```

Then enable the backend. `detect.tract_model` is **optional** — it defaults to the path the
fetch script writes, so you only set the backend:

```toml
[detect]
backend = "tract"
# tract_model = "vendor/models/tinyyolov2-8.onnx"  # default (host-only; runs in tract on Pi/x86,
#                                                  # NOT on the ESP32-S3 — that uses Grove Vision AI V2)
# tract_format = "yolov2"  # default; "postnms" for models that already emit final boxes
confidence = 0.5    # minimum detection confidence, 0.0–1.0 (default 0.5)

[rtsp]      # or [v4l2]
width = 320
height = 320
```

Build with the feature and run: `cargo run --release --features backend-tract --bin witnessd`.
If `backend=tract` but no model is found at the resolved path, witnessd fails fast with a clear
load error naming the expected file.

Environment overrides: `WITNESS_DETECT_BACKEND=tract`,
`WITNESS_TRACT_MODEL=/absolute/path/to/model.onnx`,
`WITNESS_DETECT_CONFIDENCE=0.6`. Frame dimensions must match the model input.
`detect.confidence` sets the minimum confidence (0.0–1.0) for a detection to be reported —
raise it to suppress weak/false detections, lower it to catch more. Values outside the range
are rejected at startup.

## Container deployment

See [`docs/container.md`](container.md) for building and running the containerized `witnessd`
artifact with RTSP configuration and Event-API-only exposure.

## Home Assistant entity reference

Full sensor/binary-sensor/service catalog (kernel sensors, Canary MQTT sensors, per-tamper-type
sensors, multi-transport status, MQTT Discovery details) lives in
[`docs/homeassistant_setup.md`](homeassistant_setup.md).
