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

### Notes

- The Event API returns only event claims with coarse time buckets and local zone
  identifiers, per the event contract.
- The container intentionally **does not** expose any raw media endpoints.
