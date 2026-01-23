# End-to-End Demo

This demo walks the kernel through a full flow:

- synthetic frames/events
- sealed log + vault
- export bundle
- verification
- concise summary

## Run the demo

```bash
cargo run --bin demo
```

Optional overrides:

```bash
cargo run --bin demo -- --seconds 2 --fps 5
cargo run --bin demo -- --vault ./vault_demo --out ./demo_out --seed 42
```

## Expected artifacts

After a successful run you should see:

- `./demo_witness.db` (sealed log + receipts)
- `./vault/envelopes/` (or `--vault` path)
- `./demo_out/export_bundle.json` (export bundle)

## Verify independently

```bash
cargo run --bin log_verify -- --db demo_witness.db
```

You should see `OK: all chains verified.` at the end.

## Tamper test (manual)

1. Run the demo once to generate artifacts.
2. Flip a byte in the export bundle:

```bash
python - <<'PY'
from pathlib import Path
path = Path("demo_out/export_bundle.json")
data = bytearray(path.read_bytes())
data[len(data) // 2] ^= 0xFF
path.write_bytes(data)
print("tampered", path)
PY
```

3. Re-run the demo in verify-only mode (no new events):

```bash
cargo run --bin demo -- --seconds 0 --fps 1
```

The demo should report `verify: FAIL` and exit non-zero.
