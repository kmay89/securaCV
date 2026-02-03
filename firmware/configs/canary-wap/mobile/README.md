# Canary WAP - Mobile Configuration

Power-optimized configuration for portable/mobile deployment.

## Target Hardware

- **Board**: XIAO ESP32-S3 Sense
- **Power**: Battery-powered operation

## Optimizations

| Setting | Default | Mobile | Savings |
|---------|---------|--------|---------|
| Record Interval | 1s | 5s | ~80% less records |
| Mesh Heartbeat | 30s | 60s | ~50% less mesh traffic |
| RF Scanning | 10s | 30s | ~67% less scanning |
| BLE Advertising | 100ms | 500ms | ~80% less radio use |
| BLE TX Power | 0 dBm | -12 dBm | Lower power |
| Camera Preview | On | Off | No streaming overhead |

## Features

Same as default configuration, except:

- Camera preview streaming disabled
- Reduced network activity
- Lower power consumption

## Battery Life Estimates

| Mode | Estimated Battery Life |
|------|------------------------|
| Default | 4-6 hours (1000mAh) |
| Mobile | 12-18 hours (1000mAh) |

## Usage

Build with the mobile environment:

```bash
pio run -e canary-wap-mobile
```
