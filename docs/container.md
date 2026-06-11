# Container deployment (witnessd)

This container wraps `witnessd` and **exposes only the Event API**. It does not
export raw media, and raw frames remain confined to in-memory buffers and the
local vault path as defined by the kernel invariants.

## Build

```bash
docker build -t witnessd:local .
```

## Run

The kernel requires a device key seed and an RTSP URL. Persist the database and
vault under `/data`.

```bash
docker run --rm \
  -e DEVICE_KEY_SEED=devkey:your-seed \
  -e WITNESS_RTSP_URL=rtsp://user:pass@camera.example/stream \
  -e WITNESS_API_ADDR=0.0.0.0:8799 \
  -p 8799:8799 \
  -v $(pwd)/data:/data \
  witnessd:local
```

### Configuration

Required:
- `DEVICE_KEY_SEED`: device key seed (must be consistent with break-glass and tooling)
- `WITNESS_RTSP_URL`: RTSP URL for the ingestion source (`stub://` is for local dev/test only)

Optional:
- `WITNESS_API_ADDR`: Event API bind address (default in container: `0.0.0.0:8799`)
- `WITNESS_API_TOKEN_PATH`: file path for the Event API capability token
- `BREAK_GLASS_SEAL_TOKEN`: path to a break-glass seal token JSON (enables vault sealing)

### Health

The image declares a `HEALTHCHECK` that probes the Event API's unauthenticated
`/health` endpoint, so orchestrators (compose `service_healthy` conditions,
swarm, `docker inspect`) see real liveness rather than just a running process.

## Operator tooling (verify and export evidence)

The image ships the operator CLIs alongside `witnessd`, so the sealed log and
vault can be verified and exported **inside the container that owns `/data`**
— the encrypted database never has to leave the volume:

```bash
# Verify the hash chain + signatures of the sealed log
docker exec -e DEVICE_KEY_SEED=devkey:your-seed <container> \
  log_verify --db /data/witness.db

# Export events to a signed bundle (export is quorum-gated: it requires a
# break-glass token — see `break_glass --help` and spec/break_glass.md),
# then verify the bundle against the DB's export receipts
docker exec -e DEVICE_KEY_SEED=devkey:your-seed <container> \
  export_events --db-path /data/witness.db --output /data/export.json \
  --break-glass-token /data/<token-file>
docker exec -e DEVICE_KEY_SEED=devkey:your-seed <container> \
  export_verify --db /data/witness.db --bundle /data/export.json

# Verify a sealed evidence envelope
docker exec <container> envelope_verify --bundle /data/vault/<envelope>

# Break-glass quorum policy and unlock workflow (request → approve →
# authorize → unseal) — also where the BREAK_GLASS_SEAL_TOKEN file above
# comes from
docker exec -e DEVICE_KEY_SEED=devkey:your-seed <container> \
  break_glass policy show --db /data/witness.db
```

Run `docker exec <container> <tool> --help` for the full flags of each tool.

### Notes

- The Event API returns only event claims with coarse time buckets and local zone
  identifiers, per the event contract.
- The container intentionally **does not** expose any raw media endpoints.
