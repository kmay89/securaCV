# csi_pc_listener

Subscribe to a SecuraCV Canary's CSI stream from a laptop, print rolling
motion / breathing scores once per second, and (optionally) chart them
live.

```
+----------+   GET /api/csi/stream    +------------+
|  Canary  | -------- SSE ----------> |  laptop    |
| ESP32-S3 |   {"t":4,"motion":12,    |  listener  |
|          |    "breathing":3,...}    |            |
+----------+                          +------------+
```

## Quick start (3 commands)

```bash
# 1. (Optional) plot dependency.
python3 -m pip install --user matplotlib

# 2. Run against canary.local (mDNS — same network).
python3 listener.py

# 3. Or against an explicit IP, with the live chart.
python3 listener.py --host 192.168.1.42 --plot
```

If your firmware doesn't yet expose `/api/csi/stream` (Phase 4 of the
plan), pass `--poll` to fall back to scraping the existing
`/api/status` JSON.

## What you'll see

```
[csi_pc_listener] subscribed to http://canary.local/  (Ctrl-C to stop)
t=   1  motion=  4  breathing=  2  conf=tentative  frames=18
t=   2  motion=  4  breathing=  3  conf= observed  frames=19
t=   3  motion= 87  breathing=  6  conf=confirmed  frames=20   <-- hand wave
t=   4  motion= 92  breathing=  4  conf=confirmed  frames=20
t=   5  motion= 12  breathing=  3  conf=tentative  frames=20
```

The values are aggregate scalars only — no MAC addresses, no raw I/Q,
no precise timestamps.

## License

MIT.
