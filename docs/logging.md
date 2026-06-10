# Operational Logging Guide

This page covers the **operational** (stderr) logs emitted by the daemons.
Operational logs are diagnostics for humans and log collectors — they are
**not evidence**. Evidence lives in the sealed, hash-chained log inside the
witness database and is verified with `log_verify` (see
[`log_verify_README.md`](../log_verify_README.md)). Nothing security-relevant
may *only* appear on stderr; anything the spec requires to be witnessed is
sealed into the chain (see [`failure_semantics.md`](failure_semantics.md)).

## Framework and configuration

All Rust daemons use `log` + `env_logger`, writing plain text to stderr with
UTC timestamps. The default level is `info`. Configure with `RUST_LOG`:

```bash
RUST_LOG=info  witnessd                       # default
RUST_LOG=warn  witnessd                       # transitions and errors only
RUST_LOG=debug witnessd                       # adds 5s counter dumps, reconnect detail
RUST_LOG=witness_kernel::ingest=debug witnessd  # per-module filtering
```

## Level philosophy

| Level  | Meaning | Examples |
|--------|---------|----------|
| ERROR  | An operation failed; the daemon keeps running but something needs attention | module inference failed, vault seal failed, sealed-record append failed |
| WARN   | State transition that degrades operation, or a security-relevant condition | ingest source became unhealthy, event rejected, vault sealing disabled, low disk space, clock skew |
| INFO   | Normal operation: startup configuration, per-event lines, periodic summaries, recoveries | `event #42: ...`, pipeline summary, `ingest recovered after 38s` |
| DEBUG  | High-frequency detail for live troubleshooting | 5-second ingest/pipeline dumps, frame-buffer size, reconnect attempts |

Two rules keep the logs consumable:

1. **Transitions are logged once, steady state is summarized.** An unhealthy
   camera produces one WARN when it goes down, rate-limited WARNs (max one
   per 30s) while down, and one INFO on recovery — not a line per failed
   frame. Disk-space and clock monitors latch the same way.
2. **Counters are summarized at INFO on `health.log_interval_s`** (default
   60s, one `key=value` line) and dumped in detail at DEBUG every 5s.

## Expected volume (witnessd at 10 fps, sparse events)

| Level | Steady-state volume |
|-------|---------------------|
| WARN  | transitions only (≈0 when healthy) |
| INFO  | ~1 summary line/min + 1 line/event ≈ **1.5–2k lines/day** |
| DEBUG | + ~12 lines/min ≈ 19k lines/day |

There is no per-frame logging at any level.

## What is never logged

- Capability token values, key seeds, or any key material (paths only).
- Raw frame content or anything derived from pixels.
- MQTT or RTSP credentials.
- In *sealed* failure records: backend names only (`backend=rtsp`), never
  URLs or device paths.

## Log rotation / journald

The daemons write to stderr and do not rotate logs themselves. Under systemd,
journald handles retention (`SystemMaxUse=` in `journald.conf`). If you
redirect stderr to a file, pair it with `logrotate`.

## The sealed system-trace records (for completeness)

These are *chain* records, not stderr logs, but operators discover them here:

- `heartbeat` — one per 10-minute bucket (`[health] heartbeat`, default on):
  ingest-health flag plus per-bucket deltas for frames captured, events
  appended, and failure records. Anchors the chain tail (tail truncation
  becomes a detectable missing-heartbeat gap) and gives auditors a health
  timeline.
- `lifecycle` — `start` on boot, `shutdown_clean` on SIGINT/SIGTERM. A boot
  that finds a trailing `start` seals a `PowerLoss` failure record
  (unclean-shutdown proxy).

`log_verify` audits these (stale tail, missing heartbeat buckets, timestamp
regressions) and reports warnings; `log_verify --strict` turns those warnings
into a non-zero exit.
