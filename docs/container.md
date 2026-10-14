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
  (e.g. `/data/api_token`; the viewer-token and pairing files default
  beside it)
- `WITNESS_API_VIEWER_TOKEN_PATH`: the viewer-token file — long-lived
  bearer tokens good for `GET /api/sealed-log` only, for a Witness Wall
  (default: `viewer_tokens.json` beside `WITNESS_API_TOKEN_PATH`; no token
  path, no viewer tokens). Minted inside the running container with
  `docker exec <container> witness_api mint-viewer-token --label <s>` — see
  [Mint a Witness Wall viewer token](#mint-a-witness-wall-viewer-token)
  below and [the API reference](homeassistant_setup.md#viewer-tokens-witness-wall)
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

Run `docker exec <container> <tool> --help` for the full flags of each tool
(`witness_api help` for `witness_api`, below).

### Mint a Witness Wall viewer token

The Witness Wall walks the sealed log with a **viewer token**, a bearer the
kernel honors on `GET /api/sealed-log` and nowhere else. The image ships
`witness_api` for its operator subcommands; `witnessd` stays the
entrypoint and still does all the serving.

1. Start the container with a token path, so `witnessd` knows where the
   viewer-token file lives — add
   `-e WITNESS_API_TOKEN_PATH=/data/api_token` to the `docker run` above
   (or name the file itself with `WITNESS_API_VIEWER_TOKEN_PATH`).
2. Once `witnessd` has started at least once (it creates the device seed
   whose key the receipt carries), mint one token per screen:

   ```bash
   docker exec <container> witness_api mint-viewer-token \
       --label "living room tv" --base-url http://<docker-host-ip>:8799
   ```

   It prints one JSON line, the pairing receipt, and that line is the
   token's only copy: the kernel keeps its sha256 in
   `/data/viewer_tokens.json` (`0600`). Paste the line into the Wall's
   Settings → Verification ([`tvos/README.md`](../tvos/README.md)). Leave
   out `--base-url` and the Wall uses the address you type into it, with
   the port.

`docker exec` runs in the container's environment (every `WITNESS_API_*`
variable and `DEVICE_KEY_SEED`, if you set one), in its working directory
`/data`, and as the image's user (UID 1001), which owns `/data`. So
`witness_api` opens the same database, finds the seed `witnessd` persisted
beside it with no `DEVICE_KEY_SEED=$(cat …)` prefix, and writes the
viewer-token file `witnessd` reads. Two things break that: `docker exec -u
root` writes a root-owned `0600` file the kernel cannot read (it then
refuses every viewer token), and `docker exec -e` pointing a `WITNESS_API_*`
path somewhere else mints into a file `witnessd` never opens, because it
reads only the path it started with.

**No restart.** `witnessd` re-reads the viewer-token file on every request,
so a new token works on the first request that carries it. The exception is a container
started with neither variable: it has no viewer-token file at all,
`witness_api` refuses to mint ("no viewer token file is configured"), and
the fix is to re-create the container with step 1.

**Revoke** with the `token_id` from the receipt (or from the file); the next
request carrying the token is refused:

```bash
docker exec <container> witness_api revoke-viewer-token <token_id>
```

The default image terminates no TLS (see `WITNESS_API_ALLOW_INSECURE` in
the Dockerfile), so the token crosses the LAN in plaintext; whoever captures
it can read the sealed-log tail, and nothing else. With the token path set,
`witnessd` also redeems Home Assistant pairing codes, and the same binary
issues them: `docker exec <container> witness_api issue-pairing-code` (see
[Pairing](homeassistant_setup.md#pairing-the-docker-sidecar-and-home-assistant)).

### Notes

- The Event API returns only event claims with coarse time buckets and local zone
  identifiers, per the event contract.
- The container intentionally **does not** expose any raw media endpoints.
- Watching Frigate instead of raw RTSP? That is a different (smaller) image:
  `ghcr.io/kmay89/securacv-sidecar` — see
  [Frigate integration](frigate_integration.md) and the quickstart compose
  files under `docker/sidecar/`.
