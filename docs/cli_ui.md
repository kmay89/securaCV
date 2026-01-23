# CLI UI (stderr-only)

The CLI progress UI is strictly **stderr-only**. Standard output remains stable and is safe for scripting or machine-readable output.

Examples:

```bash
cargo run --bin export_events -- --ui=pretty
cargo run --bin export_events -- --ui=plain
```

UI modes:

- `auto` (default): Pretty spinner only when stderr is a TTY and stdout is not redirected.
- `plain`: Deterministic stderr lines (no spinner).
- `pretty`: Spinner on TTY; falls back to plain when stderr is not a TTY.
