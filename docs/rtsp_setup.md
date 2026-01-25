# RTSP Camera Setup Guide

This guide covers connecting the Privacy Witness Kernel to real IP cameras via RTSP.

## Prerequisites

### Build Requirements

For real RTSP support, you need either GStreamer or FFmpeg installed and the
matching cargo feature enabled. Both backends keep frames in memory, coarsen
timestamps at capture time, and compute non-invertible feature hashes at capture time.

**Ubuntu/Debian:**
```bash
sudo apt-get install -y \
    libgstreamer1.0-dev \
    libgstreamer-plugins-base1.0-dev \
    gstreamer1.0-plugins-good \
    gstreamer1.0-plugins-bad \
    gstreamer1.0-plugins-ugly \
    libseccomp-dev
```

**Ubuntu/Debian (FFmpeg backend):**
```bash
sudo apt-get install -y \
    libavcodec-dev \
    libavformat-dev \
    libavutil-dev \
    libswscale-dev \
    libseccomp-dev
```

**Fedora/RHEL:**
```bash
sudo dnf install -y \
    gstreamer1-devel \
    gstreamer1-plugins-base-devel \
    gstreamer1-plugins-good \
    gstreamer1-plugins-bad-free \
    libseccomp-devel
```

**Fedora/RHEL (FFmpeg backend):**
```bash
sudo dnf install -y \
    ffmpeg-devel \
    libseccomp-devel
```

**macOS (without sandbox - for development only):**
```bash
brew install gstreamer gst-plugins-base gst-plugins-good gst-plugins-bad gst-plugins-ugly
```

**macOS (FFmpeg backend):**
```bash
brew install ffmpeg
```

### Build with RTSP Support

```bash
cargo build --release --features rtsp-gstreamer
```

or

```bash
cargo build --release --features rtsp-ffmpeg
```

---

## Finding Your Camera's RTSP URL

### Method 1: Check Camera Documentation

Most IP cameras document their RTSP URLs. Common patterns:

| Brand | URL Pattern |
|-------|-------------|
| **Hikvision** | `rtsp://user:pass@IP:554/Streaming/Channels/101` |
| **Dahua** | `rtsp://user:pass@IP:554/cam/realmonitor?channel=1&subtype=0` |
| **Reolink** | `rtsp://user:pass@IP:554/h264Preview_01_main` |
| **Amcrest** | `rtsp://user:pass@IP:554/cam/realmonitor?channel=1&subtype=0` |
| **Ubiquiti** | `rtsp://IP:7447/{camera_id}` |
| **Wyze** (with firmware) | `rtsp://user:pass@IP:8554/live` |
| **Tapo** | `rtsp://user:pass@IP:554/stream1` |
| **Eufy** | `rtsp://IP:8554/live0` |
| **ONVIF** | `rtsp://user:pass@IP:554/onvif-media/media.amp` |

### Method 2: Use ONVIF Discovery

```bash
# Install onvif-probe tool
pip install onvif-zeep

# Discover cameras on your network
python -c "
from onvif import ONVIFCamera
import socket

def scan():
    # Simple UDP discovery
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    sock.settimeout(3)

    probe = b'''<?xml version=\"1.0\" encoding=\"UTF-8\"?>
    <e:Envelope xmlns:e=\"http://www.w3.org/2003/05/soap-envelope\">
    <e:Body><Probe xmlns=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\"/>
    </e:Body></e:Envelope>'''

    sock.sendto(probe, ('239.255.255.250', 3702))

    while True:
        try:
            data, addr = sock.recvfrom(4096)
            print(f'Found device at {addr[0]}')
        except socket.timeout:
            break

scan()
"
```

### Method 3: Test with VLC

```bash
# Test the RTSP URL works
vlc rtsp://username:password@192.168.1.100:554/stream1
```

### Method 4: Use go2rtc (Recommended for Home Assistant)

go2rtc auto-discovers many camera types. See the Home Assistant section below.

---

## Configuration

### Standalone Configuration (TOML)

Create `witness.toml`:

```toml
[rtsp]
url = "rtsp://admin:password123@192.168.1.100:554/Streaming/Channels/101"
target_fps = 10
width = 1920
height = 1080
backend = "auto" # auto, gstreamer, ffmpeg

[zones]
module_zone_id = "zone:front_door"
sensitive = ["zone:front_door"]
```

Run:
```bash
export DEVICE_KEY_SEED=$(openssl rand -hex 32)
WITNESS_CONFIG=witness.toml cargo run --release --features rtsp-gstreamer --bin witnessd
```

`auto` prefers GStreamer when both `rtsp-gstreamer` and `rtsp-ffmpeg` are enabled,
and falls back to FFmpeg when only that feature is available.

### Environment Variable Configuration

```bash
export DEVICE_KEY_SEED=$(openssl rand -hex 32)
export WITNESS_RTSP_URL="rtsp://admin:password@192.168.1.100:554/stream"
export WITNESS_RTSP_BACKEND="auto" # auto, gstreamer, ffmpeg
export WITNESS_ZONE_ID="zone:driveway"

cargo run --release --features rtsp-gstreamer --bin witnessd
```

---

## Sub-streams and Performance

Most cameras offer multiple streams:

| Stream | Resolution | Use Case |
|--------|------------|----------|
| **Main stream** | Full HD/4K | Recording, detailed analysis |
| **Sub stream** | 640x480, D1 | Live viewing, motion detection |

For the Privacy Witness Kernel, **use the sub-stream** when possible:
- Lower bandwidth
- Faster processing
- Motion detection doesn't need 4K

```toml
# Hikvision sub-stream
[rtsp]
url = "rtsp://admin:pass@192.168.1.100:554/Streaming/Channels/102"  # 102 = sub
target_fps = 10
width = 640
height = 480
```

---

## Authentication

### Basic Auth (Most Common)

Include credentials in the URL:
```
rtsp://username:password@192.168.1.100:554/stream
```

### Special Characters in Passwords

URL-encode special characters:

| Character | Encoded |
|-----------|---------|
| `@` | `%40` |
| `:` | `%3A` |
| `/` | `%2F` |
| `#` | `%23` |
| `?` | `%3F` |

Example:
```
# Password: P@ss:word
rtsp://admin:P%40ss%3Aword@192.168.1.100:554/stream
```

### Digest Auth

GStreamer and FFmpeg handle digest authentication automatically when using basic auth format.

---

## Troubleshooting

### Camera Not Connecting

1. **Verify URL in VLC first:**
   ```bash
   vlc rtsp://admin:password@192.168.1.100:554/stream1
   ```

2. **Check firewall:**
   ```bash
   nc -zv 192.168.1.100 554
   ```

3. **Enable verbose logging:**
   ```bash
   RUST_LOG=debug GST_DEBUG=3 cargo run --bin witnessd
   ```

### Stream Stalls

1. **Use TCP transport (more reliable):**
   The GStreamer pipeline uses TCP by default.

2. **Reduce FPS:**
   ```toml
   [rtsp]
   target_fps = 5  # Reduce if network is slow
   ```

3. **Use sub-stream:**
   Lower resolution = less bandwidth needed.

### High CPU Usage

1. **Reduce resolution:**
   ```toml
   [rtsp]
   width = 640
   height = 480
   ```

2. **Reduce FPS:**
   ```toml
   [rtsp]
   target_fps = 5
   ```

### "RTSP requires the rtsp-gstreamer or rtsp-ffmpeg feature"

You need to build with a supported RTSP backend:
```bash
cargo build --release --features rtsp-gstreamer
```

or

```bash
cargo build --release --features rtsp-ffmpeg
```

---

## Security Recommendations

1. **Use a dedicated camera user account** with minimal permissions
2. **Isolate cameras on a separate VLAN** if possible
3. **Use sub-streams** to reduce network exposure
4. **Keep camera firmware updated**
5. **Consider go2rtc as an RTSP proxy** for additional isolation

---

## Next Steps

- [Home Assistant Integration](homeassistant_setup.md) - Run as an HA add-on
- [Container Deployment](container.md) - Run in Docker
- [Verification](../spec/verification.md) - Verify event integrity
