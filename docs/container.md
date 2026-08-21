# Container deployment (witnessd)

This container wraps `witnessd` and **exposes only the Event API**. It does not
export raw media, and raw frames remain confined to in-memory buffers and the
local vault path as defined by the kernel invariants.

## Get the image

The image is published to GHCR on every push to `main` as
`ghcr.io/kmay89/securacv-witnessd` (tags: `latest` plus the exact commit sha —
it ships from `main`, not from version tags), built and smoke-tested by the
`Container images` workflow. Currently `linux/amd64`.

```bash
docker pull ghcr.io/kmay89/securacv-witnessd:latest
```

Or build it yourself from the repo root — the same Dockerfile the workflow
validates on every PR:

```bash
docker build -t witnessd:local .
```

## Run

The kernel needs an RTSP URL; everything else has working defaults. In
particular **no key setup is required**: on first start `witnessd` generates a
device key seed (`devkey:` + 64 hex chars) and persists it next to the
database at `/data/witness.ed25519.seed` (mode 0600). Persist the database and
vault under `/data`.

```bash
docker run --rm \
  -e WITNESS_RTSP_URL=rtsp://user:pass@camera.example/stream \
  -e WITNESS_API_ADDR=0.0.0.0:8799 \
  -p 8799:8799 \
  -v $(pwd)/data:/data \
  ghcr.io/kmay89/securacv-witnessd:latest
```

**Back up `/data/witness.ed25519.seed`.** It is the signing key for the sealed
log: with the `/data` volume (or at least that file) backed up, signatures can
be re-verified forever; without it, a lost volume means a lost key.

### Configuration

Required:
- `WITNESS_RTSP_URL`: RTSP URL for the ingestion source (`stub://` is for local dev/test only)

Optional:
- `DEVICE_KEY_SEED`: device key seed. Leave unset to auto-generate (see
  above). To supply your own, the convention is 64 hex characters, e.g.
  `openssl rand -hex 32`. Once a seed is persisted in `/data`, a *different*
  `DEVICE_KEY_SEED` is rejected at startup rather than silently re-keying the
  log.
- `WITNESS_API_ADDR`: Event API bind address (default in container: `0.0.0.0:8799`)
- `WITNESS_API_TOKEN_PATH`: file path for the Event API capability token
- `BREAK_GLASS_SEAL_TOKEN`: path to a break-glass seal token JSON (enables vault sealing)

### Health

The image declares a `HEALTHCHECK` that probes the Event API's unauthenticated
`/health` endpoint, so orchestrators (compose `service_healthy` conditions,
swarm, `docker inspect`) see real liveness rather than just a running process.

### Run under systemd

No compose stack needed: [`examples/witnessd.service`](../examples/witnessd.service)
is a ready-made unit that runs this container with `Restart=on-failure` (the
container's non-zero exits — and only those — trigger a restart). Install it
with:

```bash
sudo cp examples/witnessd.service /etc/systemd/system/witnessd.service
sudo systemctl edit witnessd.service   # set your WITNESS_RTSP_URL override
sudo systemctl enable --now witnessd.service
```

## Operator tooling (verify and export evidence)

The image ships the operator CLIs alongside `witnessd`, so the sealed log and
vault can be verified and exported **inside the container that owns `/data`**
— the encrypted database never has to leave the volume. The auto-generated
seed is read back from its persisted file:

```bash
# Verify the hash chain + signatures of the sealed log
docker exec <container> sh -c \
  'DEVICE_KEY_SEED=$(cat /data/witness.ed25519.seed) log_verify --db /data/witness.db'

# Export events to a signed bundle (export is quorum-gated: it requires a
# break-glass token — see `break_glass --help` and spec/break_glass.md),
# then verify the bundle against the DB's export receipts
docker exec <container> sh -c \
  'DEVICE_KEY_SEED=$(cat /data/witness.ed25519.seed) export_events \
     --db-path /data/witness.db --output /data/export.json \
     --break-glass-token /data/<token-file>'
docker exec <container> sh -c \
  'DEVICE_KEY_SEED=$(cat /data/witness.ed25519.seed) export_verify \
     --db /data/witness.db --bundle /data/export.json'

# Verify a sealed evidence envelope
docker exec <container> envelope_verify --bundle /data/vault/<envelope>

# Break-glass quorum policy and unlock workflow (request → approve →
# authorize → unseal) — also where the BREAK_GLASS_SEAL_TOKEN file above
# comes from
docker exec <container> sh -c \
  'DEVICE_KEY_SEED=$(cat /data/witness.ed25519.seed) break_glass policy show --db /data/witness.db'
```

Run `docker exec <container> <tool> --help` for the full flags of each tool.

### Notes

- The Event API returns only event claims with coarse time buckets and local zone
  identifiers, per the event contract.
- The container intentionally **does not** expose any raw media endpoints.
- Watching Frigate instead of raw RTSP? That is a different (smaller) image:
  `ghcr.io/kmay89/securacv-sidecar` — see
  [Frigate integration](frigate_integration.md) and the quickstart compose
  files under `docker/sidecar/`.
