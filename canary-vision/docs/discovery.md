# Fleet Discovery Protocol

Canary Vision uses four passive methods to discover peer devices on the local network. **No subnet scanning is performed** (security decision D5). The device never probes IP ranges, sends broadcast packets, or iterates over address spaces.

## Discovery Methods

### 1. mDNS Service Advertisement

Each Canary device registers itself via multicast DNS using the `_securacv._tcp` service type.

**Service record:**

```
Instance: canary-a3f7
Service:  _securacv._tcp.local.
Host:     canary-a3f7.local.
Port:     80
TXT:
  device_id=canary-a3f7
  name=Front Porch
  model=XIAO ESP32S3
  fw=0.4.1
```

The SPA and other Canary devices listen for `_securacv._tcp` announcements on the local network. When a new service appears, the listener extracts the hostname (e.g., `canary-a3f7.local`) and can resolve it to a LAN IP.

**How it works on the ESP32:**

The ESP32 firmware uses the built-in mDNS library to both advertise its own service and browse for peers:

```
mdns_hostname: canary-a3f7.local
mdns_service:  _securacv._tcp, port 80
```

mDNS registration is logged at boot: `mDNS registered: canary-a3f7.local`

The `network.mdns_enabled` config flag controls whether the device participates in mDNS. When disabled, the device is only reachable by direct IP.

### 2. Peer List API

Each device maintains a list of known peers and exposes it at `GET /api/v1/peers`. When the SPA connects to one device, it can query that device's peer list to discover other Canary devices on the network.

**Example flow:**

1. SPA connects to `canary-a3f7.local` (known device).
2. SPA calls `GET /api/v1/peers` and receives:
   ```json
   {
     "peers": [
       { "device_id": "canary-b1c2", "name": "Garage",   "ip": "192.168.1.103", "mdns_hostname": "canary-b1c2.local" },
       { "device_id": "canary-d4e5", "name": "Back Yard", "ip": "192.168.1.110", "mdns_hostname": "canary-d4e5.local" }
     ]
   }
   ```
3. SPA renders a "Discovered on your network" section on the My Canaries
   view with one card per peer that is not yet in the user's device list.
4. The user taps "Pair this Canary"; the host is pre-filled, and the user
   provides the token for that specific device. Each peer is independently
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
  "last_info": { "..." },
  "added_at": "2026-02-18T15:30:00.000Z"
}
```

Tokens never leave the browser. They are sent only to the corresponding device's private-network address via the `X-Canary-Token` header.

## CORS Implications

When the SPA is served from one device (e.g., `canary-a3f7.local`) and makes cross-origin requests to a peer (e.g., `canary-b1c2.local`), CORS applies. The target device's CORS middleware checks the `Origin` header against its peer list and sets `Access-Control-Allow-Origin` only for recognized peers. This means device A must appear in device B's peer list for the SPA served from A to reach B's API.
