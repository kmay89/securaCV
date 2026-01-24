# Privacy Witness Kernel - Home Assistant Add-on

Privacy-preserving video surveillance that produces **claims, not recordings**.

## Quick Start

1. Add this repository to Home Assistant:
   - Go to **Settings → Add-ons → Add-on Store**
   - Click ⋮ → **Repositories**
   - Add: `https://github.com/kmay89/securaCV`

2. Install "Privacy Witness Kernel"

3. Configure:
   ```yaml
   device_key_seed: "your-64-character-hex-key"  # openssl rand -hex 32
   go2rtc_discovery: true
   ```

4. Start the add-on

## Features

- **Auto-discovers cameras** from go2rtc/Frigate
- **Local processing** - no cloud, no external servers
- **Privacy by design** - produces event claims, not searchable recordings
- **Cryptographically signed** - tamper-evident event log
- **Configurable retention** - automatic cleanup of old events

## What Events Look Like

```json
{
  "event_type": "BoundaryCrossingObjectLarge",
  "zone_id": "zone:front_door",
  "time_bucket": { "start_epoch_s": 1706140800, "size_s": 600 },
  "confidence": 0.85
}
```

**Note:** No faces, no license plates, no precise timestamps, no raw video.

## Documentation

- [Full Setup Guide](../docs/homeassistant_setup.md)
- [RTSP Configuration](../docs/rtsp_setup.md)
- [Privacy Architecture](../spec/invariants.md)

## Support

- [GitHub Issues](https://github.com/kmay89/securaCV/issues)
