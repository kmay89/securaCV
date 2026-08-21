# Fleet Discovery Protocol

Canary Vision uses four passive methods to discover peer devices on the local network. **No subnet scanning is performed** (security decision D5). The device never probes IP ranges, sends broadcast packets, or iterates over address spaces.

## Discovery Methods

### 1. mDNS Service Advertisement

Each Canary device registers itself via multicast DNS using the `_securacv._tcp` service type.

**Canonical TXT schema:**

```
Instance: canary-a3f7
Service:  _securacv._tcp.local.
Host:     canary-a3f7.local.
Port:     80
TXT:
  device_id=canary-a3f7      # stable device identifier
  name=Front Porch           # user-assigned display name
  host=canary-a3f7.local     # mDNS hostname
  fw=0.4.1                   # firmware version
  model=XIAO ESP32S3         # hardware model
  dt=canary-wap              # canonical device type (see below)
  role=witness-beacon        # human-readable display role
  broker=192.168.1.2         # MQTT broker in use (MQTT-onboarded types)
  bport=1883                 # MQTT broker port
```

**Device types (`dt`) and display roles:**

| `dt` | Display role | What it is |
|------|--------------|------------|
| `canary-wap` | witness beacon | GPS + signed event log with its own WiFi hotspot; HTTP API + mDNS |
| `canary-vision` | camera witness | On-device person detection; semantic events only, never stores video |
| `canary-sense` | radar witness | 60 GHz mmWave presence/breathing/heartbeat; no camera, no microphone |

The SPA and WAP-class devices listen for `_securacv._tcp` announcements on the local network. When a new service appears, the listener extracts the hostname (e.g., `canary-a3f7.local`) and can resolve it to a LAN IP.

**How it works on the ESP32 — behavior differs by type:**

- **canary-wap** both advertises its own `_securacv._tcp` service *and*
  browses for peers, and it serves the HTTP API (including
  `GET /api/v1/peers`). It is the only variant with an HTTP server.
- **canary-vision** and **canary-sense** *advertise only*. They run no
  HTTP server and cannot be paired over HTTP; they are onboarded through
  Home Assistant via MQTT discovery (their `broker`/`bport` TXT keys show
  which broker they use). Peers relayed by a WAP's fleet scan carry their
  `dt` and `role` so clients can render them correctly.

```
mdns_hostname: canary-a3f7.local
mdns_service:  _securacv._tcp, port 80
```

mDNS registration is logged at boot: `mDNS registered: canary-a3f7.local`

The `network.mdns_enabled` config flag controls whether the device participates in mDNS. When disabled, the device is only reachable by direct IP.

### 2. Peer List API

Each WAP-class device maintains a list of known peers (gathered by its mDNS fleet scan) and exposes it at `GET /api/v1/peers`. When the SPA connects to one WAP, it can query that device's peer list to discover other Canary devices on the network — including MQTT-only vision/sense sensors, whose `dt`/`role` the WAP relays from their TXT records (the API normalizes `dt` to `device_type`).

**Example flow:**

1. SPA connects to `canary-a3f7.local` (known device).
2. SPA calls `GET /api/v1/peers` and receives:
   ```json
   {
     "peers": [
       { "device_id": "canary-b1c2", "name": "Garage",   "device_type": "canary-vision", "ip": "192.168.1.103", "mdns_hostname": "canary-b1c2.local" },
       { "device_id": "canary-d4e5", "name": "Back Yard", "device_type": "canary-sense",  "ip": "192.168.1.110", "mdns_hostname": "canary-d4e5.local" }
     ]
   }
   ```
3. SPA renders a "Discovered on your network" section on the My Canaries
   view with one card per peer that is not yet in the user's device list,
   badged with the peer's type and a one-line explanation.
4. The user taps "Pair this Canary". For WAP-class peers the host is
   pre-filled and pairing proceeds over HTTP (BOOT-tap or token). For
   vision/sense peers there is no HTTP pairing — the SPA explains the
   device and points the user to Home Assistant, where the sensor appears
   automatically via MQTT discovery. Each HTTP peer is independently
   authenticated — knowing a peer exists does not grant API access.

The SPA prefers `mdns_hostname` over `ip` when constructing the peer's
`base_url` because IPs change over DHCP leases but mDNS names don't. The
field is optional; clients fall back to `ip` when absent.

The peer list is read-only. There is no endpoint to add or remove peers via the API. Peers are discovered by the device itself through mDNS and stored in device state.

**Important:** Knowing a peer's IP from the peer list does **not** grant API access. Each device has its own independent token.

### 3. Manual Entry

The SPA allows the user to manually add a device by entering:

- **Device address:** mDNS hostname (e.g., `canary-a3f7.local`) or LAN IP (e.g., `192.168.1.47`)
- **API token:** The device's `X-Canary-Token` value

The SPA validates that the entered address resolves to a private-network IP (RFC 1918 or `.local`) before making any request. Public internet addresses are rejected.

After entry, the SPA calls `GET /api/v1/info` with the provided token to verify connectivity and retrieve device metadata.

### 4. Provisioning Receipt Import

During physical provisioning (first-time setup), each device generates a JSON provisioning receipt. The user can paste this receipt into the SPA's "Add Canary" form.

**Receipt format:**

```json
{
  "device_id": "canary-a3f7",
  "base_url": "http://canary-a3f7.local",
  "token": "cv_a3f7_8b2e4f1a9c3d7e0b5f2a8c4d6e1b3a7f"
}
```

Alternative field names `host` (for `base_url`) and `api_token` (for `token`) are also accepted.

The SPA parses the receipt, validates the URL is a private-network address, and tests the connection before saving.

---

## What Is NOT Supported

| Technique | Status | Reason |
|-----------|--------|--------|
| Subnet scanning (e.g., iterating 192.168.1.1-254) | Blocked | D5: exposes network topology, slow, unreliable |
| Broadcast/multicast probes (non-mDNS) | Not implemented | Unnecessary given mDNS |
| UPnP/SSDP discovery | Not implemented | Attack surface, not needed |
| Cloud-based device registry | Not implemented | Violates local-only principle |
| QR code scanning | Not yet implemented | Planned future alternative to receipt paste |

## SPA Storage

Discovered devices are stored in `localStorage` under the key `canary_devices` as a JSON array. Each entry contains:

```json
{
  "id": "canary-a3f7",
  "name": "Front Porch",
  "base_url": "http://canary-a3f7.local",
  "token": "cv_a3f7_8b2e4f1a9c3d7e0b5f2a8c4d6e1b3a7f",
  "device_type": "canary-wap",
  "last_info": { "..." },
  "added_at": "2026-02-18T15:30:00.000Z"
}
```

Tokens never leave the browser. They are sent only to the corresponding device's private-network address via the `X-Canary-Token` header.

## CORS Implications

When the SPA is served from one device (e.g., `canary-a3f7.local`) and makes cross-origin requests to a peer (e.g., `canary-b1c2.local`), CORS applies. The target device does **not** allowlist peers: being in a peer list grants no cross-origin access (a compromised peer could otherwise pivot across the mesh). Instead, the target answers CORS only for its own origin and for origins enrolled by trust-on-pair — plus any private-network origin for the BOOT-gated `/api/provisioning-receipt` endpoint only (see [security.md](security.md), D3).

So for the SPA served from device A to reach device B's API, device B must be paired *from that SPA* once: a physical BOOT-button press on B releases its provisioning receipt to the SPA's origin, and B records that origin as durably allowed from then on.
