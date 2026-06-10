# SD Card Health, Endurance & Replacement Guide

SecuraCV runs 24/7 on flash storage — typically a microSD card in a Raspberry Pi
(Privacy Witness Kernel) or an ESP32 Canary. SD cards are consumables: they have a
finite write budget and, unlike SSDs, expose no SMART health data. SecuraCV is
engineered around that reality on two fronts:

1. **Minimize writes by design** — the kernel stores compact signed event claims
   (never video), batches its retention checkpoints, and bounds its write-ahead log,
   so a typical deployment writes a few megabytes per day instead of the tens of
   gigabytes a video recorder would.
2. **Monitor what's left** — the kernel and Canary firmware track write volume
   against the card's endurance rating and surface a clear, hysteresis-filtered
   health status, including a **Replacement Recommended** signal — so a card swap is
   a planned five-minute maintenance task instead of a surprise outage.

See also: [`docs/operator_guide.md`](operator_guide.md),
[`docs/homeassistant_setup.md`](homeassistant_setup.md),
[`docs/esp32s3_thermal_review.md`](esp32s3_thermal_review.md),
[`config.example.toml`](../config.example.toml).

---

## Choosing the right card

The single highest-impact reliability decision is the card itself.

| Pick | Why |
|---|---|
| **High-endurance / industrial microSD** | Rated for continuous-write workloads (dashcams, surveillance). Typically MLC/pSLC flash with TBW ratings several times higher than consumer cards. |
| **A1/A2 application class** | Guarantees sustained random-I/O performance — what SQLite workloads actually do. |
| **Major brand, genuine stock** | Counterfeit cards routinely misreport capacity and fail early. Buy from reputable channels. |
| **Bigger than you need** | A 64 GB card holding 2 GB of data wear-levels across 30× more flash than it strictly needs — capacity headroom is endurance headroom. |

After installing, set the card's endurance rating in `witness.toml` so the wear
estimate is calibrated to *your* card rather than the conservative default:

```toml
[storage_health]
endurance_tbw = 64.0   # match the card's TBW rating (TB written)
```

If the vendor publishes hours of continuous video recording instead of TBW,
a rough conversion: `TBW ≈ hours × bitrate(MB/s) × 3600 ÷ 1e6`. When in doubt,
leave the conservative default — it only makes the recommendation *earlier*,
never later.

## Heat management

Heat is the other silent killer of flash storage: sustained high temperature
accelerates charge leakage and wear. The kernel reads the SoC temperature
(`/sys/class/thermal`) every health sample and reports it alongside the storage
metrics (`warm` ≥ 70 °C, `hot` ≥ 80 °C — the same territory where a Raspberry Pi
starts throttling).

Practical guidance:

- Give the Pi a case with airflow or a heatsink; avoid sealed enclosures in
  direct sun.
- Keep the SD card slot away from the SoC's hot side where the enclosure allows.
- If the **SoC Temperature** sensor sits in `warm`/`hot` for hours at a time,
  treat that as an enclosure/placement problem to fix — not a number to watch.
- For ESP32 Canary thermal behavior, see
  [`docs/esp32s3_thermal_review.md`](esp32s3_thermal_review.md); Canary devices
  report their die temperature (`temp_c`) on the MQTT health topic.

## How SecuraCV minimizes writes

These behaviors ship enabled — they are why the wear math works out to years,
not months:

- **Events, not video.** A sealed event is a few hundred bytes of signed JSON.
  Raw frames are never written to disk.
- **Batched retention checkpoints.** Pruning old events writes a signed
  checkpoint transaction. The cadence is `retention.check_interval_seconds`
  (default 300 s — 288 transactions/day rather than thousands). Events persist at
  most `retention.seconds + check_interval_seconds`.
- **Bounded write-ahead log.** The SQLite WAL is truncated back to 4 MB after
  checkpoints (`journal_size_limit`), bounding rewrite amplification.
- **Optional `synchronous=normal`.** By default every commit is fsynced
  (`full`) so a power cut can never lose an acknowledged sealed event. Setting
  `storage_health.sqlite_synchronous = "normal"` reduces fsync traffic further;
  in WAL mode this can never corrupt the database or break hash-chain
  verifiability (a power cut just truncates the tail), **but the most recent
  sealed events may be lost** — for a witness device whose threat model includes
  being powered off mid-incident, keep `full` unless your write volume genuinely
  demands otherwise.
- **Lazy firmware counters.** On Canary devices, the endurance counters
  themselves persist to NVS in batches (every 64 writes or 10 minutes) so the
  bookkeeping never becomes its own wear source.

## Reading the health sensors

### Health status model

Both the kernel and Home Assistant use the same four-state model, computed from
free space, estimated wear, and recent write errors, with **hysteresis** (a worse
status needs 3 consecutive samples; recovery needs to clear the threshold by a
margin) so a transient blip never flips an alert:

| Status | Meaning | Action |
|---|---|---|
| `good` | All metrics within thresholds | None |
| `degraded` | Free space < 15 %, or write errors in the last 24 h | Investigate: prune retention, check the card seating, watch the trend |
| `replacement_recommended` | Estimated wear ≥ 80 % of the endurance rating | Schedule a card swap at your convenience (see runbook below) |
| `critical` | Free space < 5 %, wear ≥ 95 %, or >10 write errors in 24 h | Replace the card promptly |

All thresholds are tunable in `[storage_health]` — see
[`config.example.toml`](../config.example.toml).

### Home Assistant entities

When the integration is configured against the kernel, these diagnostic
entities appear on the *Privacy Witness Kernel* device:

- **Storage Health** — the status string above, with all raw metrics as attributes
- **Storage Replacement Recommended** — problem binary sensor (on at
  `replacement_recommended`/`critical`); ideal for an automation/notification
- **Storage Free** (%), **Storage Wear Estimate** (%), **Storage Write Rate**
  (MB/day), **SoC Temperature** (°C)

Canary devices additionally expose **SD Wear Estimate** and **SD Replacement
Recommended** per device, fed by the firmware's NVS-persisted lifetime write
counters over the existing MQTT health topic.

A useful automation: notify when *Storage Replacement Recommended* turns on —
you will typically have weeks of comfortable lead time.

### Kernel API

The kernel serves the full report at the token-gated `/status` endpoint
(same bearer-token auth as `/events`; `/health` remains the open liveness check):

```bash
TOKEN=$(cat /run/witness/api_token)
curl -s -H "Authorization: Bearer $TOKEN" http://127.0.0.1:8799/status | jq
```

```json
{
  "kernel_version": "0.5.0",
  "storage": {
    "status": "good",
    "free_bytes": 10737418240, "total_bytes": 31914983424, "free_pct": 33.6,
    "db_bytes": 1048576, "wal_bytes": 65536,
    "write_errors": 0,
    "write_rate_bytes_per_day": 5242880,
    "lifetime_bytes_written": 128000000000,
    "endurance_tbw": 64.0, "wear_pct": 0.2,
    "estimated_days_remaining": 24000,
    "source_device": "mmcblk0"
  },
  "thermal": { "soc_temp_c": 52.1, "status": "ok" },
  "time_bucket": { "start_epoch_s": 1765400400, "size_s": 600 }
}
```

Consistent with the metadata-minimization invariant, the sample time is a
coarse 10-minute bucket, never a precise timestamp.

## What the wear estimate is (and is not)

SD cards expose **no SMART data**, so no software can read the card's true
remaining life. SecuraCV's estimate is engineered to be honestly conservative:

- It counts **whole-device** writes (from `/sys/block/<dev>/stat`, accumulated
  across reboots in a small state file next to the database). That includes the
  OS and system logs — which is correct for *card* wear, but means the figure
  describes the whole system, not just SecuraCV.
- It compares that total against the **configured** `endurance_tbw`, defaulting
  to a deliberately cautious 64 TBW. Calibrate it to your card.
- `estimated_days_remaining` extrapolates the last 24 h write rate; treat it as
  an order-of-magnitude planning number.

An estimate erring early costs you a planned card swap; an estimate erring late
costs you evidence. SecuraCV chooses the first.

**Container deployments:** inside Docker the host's `/sys/block` is not visible
by default, so wear tracking auto-disables (free space, DB size, write errors
and temperature still work where mounted). To enable it, bind-mount `/sys` and
set `storage_health.block_device` (e.g. `"mmcblk0"`) explicitly.

## Card replacement runbook

When *Replacement Recommended* turns on, schedule this ~5-minute procedure.
The sealed log's hash chain makes the migration verifiable end-to-end:

1. **Stop the kernel** so the database is quiescent:
   `sudo systemctl stop witnessd` (or stop the add-on/container).
2. **Copy the data** to the new card (or to a staging machine). Everything
   lives next to the database path:
   - `witness.db`, plus `witness.db-wal` / `witness.db-shm` if present
   - the device key seed file (`witness.ed25519.seed` — the db path with its
     extension replaced; see `crypto::device_key_path_for_db`)
   - `witness.db.health.json` (wear-tracking state; carrying it over is
     *optional* — leave it behind for a fresh card so the wear estimate
     restarts at zero **and update `endurance_tbw` if the new card differs**)
3. **Verify the chain survived the copy** before going live:

   ```bash
   cargo run --bin log_verify -- --db /path/on/new/card/witness.db
   ```

   A clean verification proves every sealed event and checkpoint signature is
   intact — the migration itself is tamper-evident.
4. **Flash/prepare the new OS card** (if replacing the boot card), restore the
   files to the same paths, and start the kernel.
5. **Confirm** the *Storage Health* sensor returns `good` and *Storage Wear
   Estimate* reads near 0 % on the new card.

For ESP32 Canary devices the chain state lives in NVS (on-chip), redundantly
backed up to `/CHAIN/` on the SD card — swapping the card does not break the
witness chain. After a swap, the firmware's lifetime counters keep counting
(they live in NVS, not on the card); if you want them re-zeroed for the new
card, erase the `diag` NVS namespace or reflash with a full chip erase.

## Configuration reference

```toml
[retention]
seconds = 604800                # how long events are kept
check_interval_seconds = 300    # retention pass cadence (endurance-tuned)

[storage_health]
enabled = true
check_interval_seconds = 600    # health sample cadence
endurance_tbw = 64.0            # set to your card's rating
# block_device = "mmcblk0"      # auto-detected; required in containers
# state_path = ""               # default "<db_path>.health.json"
# free_space_warn_pct = 15.0
# free_space_critical_pct = 5.0
# wear_warn_pct = 80.0
# wear_critical_pct = 95.0
# temp_warn_c = 70.0
# temp_hot_c = 80.0
# sqlite_synchronous = "full"   # "normal" = fewer fsyncs, may lose newest events on power cut
```

Environment overrides: `WITNESS_RETENTION_CHECK_INTERVAL_SECS`,
`WITNESS_STORAGE_HEALTH_ENABLED`, `WITNESS_STORAGE_HEALTH_INTERVAL_SECS`,
`WITNESS_STORAGE_HEALTH_TBW`, `WITNESS_STORAGE_HEALTH_DEVICE`,
`WITNESS_SQLITE_SYNCHRONOUS`.

On Canary firmware, override the card rating at build time in
`canary_config.h`: `#define DIAG_SD_ENDURANCE_TBW 64`.
