# Scheduled exports

Automate the owner self-export so a fresh, signed, self-verifying bundle of
your event log always exists outside the device — without ever scripting
around break-glass. Every scheduled run appends a `self_export`-labeled
receipt to the tamper-evident log, so automation stays auditable.

## The building block

`export_events --output-dir` writes `securacv-events-<bucket>.json` (the
filename carries only the coarse 10-minute bucket start) and `--keep N`
rotates the directory down to the newest N files:

```bash
DEVICE_KEY_SEED=devkey:your-seed \
  export_events \
  --db-path /var/lib/securacv/witness.db \
  --ruleset-id ruleset:v0.3.0 \
  --self-export \
  --last 24h \
  --output-dir /var/lib/securacv/exports \
  --keep 30
```

Nightly with `--last 24h --keep 30` gives you a rolling month of daily
bundles, each independently verifiable offline (`envelope_verify`, the
evidence viewer, or `export_verify` against the database).

## systemd timer (recommended)

`/etc/systemd/system/securacv-export.service`:

```ini
[Unit]
Description=securaCV nightly self-export

[Service]
Type=oneshot
# Keep the seed out of the unit file and process lists:
EnvironmentFile=/etc/securacv/export.env   # DEVICE_KEY_SEED=devkey:...
ExecStart=/usr/local/bin/export_events \
    --db-path /var/lib/securacv/witness.db \
    --ruleset-id ruleset:v0.3.0 \
    --self-export --last 24h \
    --output-dir /var/lib/securacv/exports --keep 30
```

`/etc/systemd/system/securacv-export.timer`:

```ini
[Unit]
Description=Run the securaCV self-export nightly

[Timer]
OnCalendar=daily
RandomizedDelaySec=10m
Persistent=true

[Install]
WantedBy=timers.target
```

```bash
sudo install -m 600 /dev/null /etc/securacv/export.env   # then add the seed
sudo systemctl enable --now securacv-export.timer
```

`Persistent=true` catches up after downtime; `RandomizedDelaySec` avoids a
correlatable on-the-minute export pattern.

## cron

```cron
# m h dom mon dow  command
17 3 * * *  . /etc/securacv/export.env && export DEVICE_KEY_SEED && \
  /usr/local/bin/export_events --db-path /var/lib/securacv/witness.db \
  --ruleset-id ruleset:v0.3.0 --self-export --last 24h \
  --output-dir /var/lib/securacv/exports --keep 30 >/dev/null
```

## Home Assistant add-on / sidecar

Inside the add-on or Docker sidecar the kernel API is already running, so
schedule against `GET /export/bundle` instead of the CLI (the receipt is
labeled `api`):

```bash
TOKEN=$(cat /config/api_token)
curl -sf -H "X-Witness-Token: $TOKEN" \
  "http://127.0.0.1:8799/export/bundle?last=24h" \
  -o "/share/securacv/securacv-events-$(date -u +%Y%m%d).json"
```

A Home Assistant automation can run that via a `shell_command` on a time
trigger. For interactive use, the add-on panel's **Download my events**
button is the same endpoint.

## Notes

- **Same bucket, same file**: two runs inside one 10-minute bucket overwrite
  the same filename — by design, the name discloses nothing finer than the
  bucket.
- **Off-device copies**: the bundle is privacy-filtered (coarse buckets, no
  media, no identities) and tamper-evident, so syncing the export directory
  to a NAS/backup target is safe and recommended — that's what makes the
  rolling window useful after the device itself is lost or seized.
- **Why not let witnessd export on its own?** Disclosure stays
  operator-initiated by design (the same reason TSA anchoring is
  operator/cron-initiated, `docs/timestamping.md`): nothing in the kernel
  spontaneously writes evidence outside its boundary. A scheduler you
  configure is an operator decision; receipts record each run.
