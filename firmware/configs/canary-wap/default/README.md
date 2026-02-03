# Canary WAP - Default Configuration

Standard configuration for the Canary Wireless Access Point witness device.

## Target Hardware

- **Board**: XIAO ESP32-S3 Sense
- **Peripherals**: L76K GNSS, microSD card, OV2640 camera

## Features Enabled

| Feature | Status | Description |
|---------|--------|-------------|
| SD Storage | ✅ | Append-only witness record storage |
| WiFi AP | ✅ | Local access point for monitoring |
| WiFi STA | ✅ | Connect to home network |
| HTTP Server | ✅ | REST API and Web UI |
| Camera Peek | ✅ | Live camera preview streaming |
| Mesh Network | ✅ | Opera protocol peer-to-peer |
| Bluetooth | ✅ | BLE pairing and configuration |
| RF Presence | ✅ | Privacy-preserving device detection |
| GNSS | ✅ | GPS location tracking |
| Watchdog | ✅ | Hardware watchdog timer |
| Tamper GPIO | ❌ | Enclosure breach sensor |

## Privacy Settings

- **Time Coarsening**: 5-second buckets
- **RF Presence**: No MAC address storage
- **Session Rotation**: Every 4 hours

## Network Settings

| Setting | Value |
|---------|-------|
| AP SSID | SecuraCV-{MAC} |
| AP Password | witness2026 |
| AP Channel | 1 |
| HTTP Port | 80 |

## Usage

This configuration is used by default when building the `canary-wap` project
with the `xiao-esp32s3-sense` board.

To override settings, create a `config_local.h` in your project that defines
the values before including `config.h`.
