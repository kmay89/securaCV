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
  device_id=canary-a3f7      # device identifier (kept across OTA updates and the 10 s hold; an install in either flasher can change it)
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
| `canary-wap` | witness beacon | GPS + signed event log with its own WiFi hotspot; its own HTTP API + mDNS (not paired from this SPA, see §2) |
| `canary-vision` | camera witness | On-device person detection; semantic events only, never stores video |
| `canary-sense` | radar witness | 60 GHz mmWave presence/breathing/heartbeat; no camera, no microphone |

The canary-wap listens for `_securacv._tcp` announcements on the local network (a browser cannot, so the SPA learns peers from §2's list). When a new service appears, the listener extracts the hostname (e.g., `canary-a3f7.local`) and can resolve it to a LAN IP.

**How it works on the ESP32 — behavior differs by type:**

- **canary-wap** both advertises its own `_securacv._tcp` service *and*
  browses for peers, and it runs an HTTP server. That server is the WAP's
  own API, not the one this SPA pairs with: it serves neither
  `GET /api/v1/peers` nor `GET /api/v1/info` (see §2).
- The flagship canary firmware (`firmware/canary`, `dt` `canary`, which has
  no row above and no card in this SPA) also advertises, browses and runs an
  HTTP server, and it serves `GET /api/v1/peers` (§2), but not
  `GET /api/v1/info`.
- **canary-vision** and **canary-sense** *advertise only*. They run no
  HTTP server and cannot be paired over HTTP; they are onboarded through
  Home Assistant via MQTT discovery (their `broker`/`bport` TXT keys show
  which broker they use). Peers relayed in a device's peer list (§2) carry
  their `dt` and `role` so clients can render them correctly.

```
mdns_hostname: canary-a3f7.local
mdns_service:  _securacv._tcp, port 80
```

mDNS registration is logged at boot: `mDNS registered: canary-a3f7.local`

The `network.mdns_enabled` config flag controls whether the device participates in mDNS. When disabled, the device is only reachable by direct IP, and the SPA reaches a device by IP only from that device's own address (§3).

### 2. Peer List API

The reference device-api (`device-api/`, the server this SPA ships with) maintains a list of known peers and exposes it at `GET /api/v1/peers`, and the flagship canary firmware (`firmware/canary`, `dt` `canary`) serves the same route from its mDNS browse cache. When the SPA connects to one such device, it can query that device's peer list to discover other Canary devices on the network — including MQTT-only vision/sense sensors, whose `dt`/`role` the device relays from their TXT records (the API normalizes `dt` to `device_type`).

**The canary-wap firmware is not paired from this SPA.** On `GET /api/provisioning-receipt` its 403s, its 409 and the receipt itself carry no `Access-Control-Allow-Origin`, so a browser hides them from this page: the BOOT-tap capture never hears the device answer and ends as "The Canary didn't answer" (recorded in Chromium against a stub that answers the way the sketch does, sweep F355). Past the receipt, the WAP takes its token as `Authorization: Bearer` and never reads `X-Canary-Token`, and it serves neither `GET /api/v1/info` nor `GET /api/v1/peers`. Pair a WAP in the SecuraCV iPhone app, from the recovery kit its dashboard saves (**Settings → Device → Save my recovery kit**) or through its Bluetooth setup, and bring it into Home Assistant, which finds it over MQTT. Its dashboard's own pairing card ("Pair with the Canary Vision app", **Show QR**) points back to this SPA's QR scan, and that QR fails here the way §3's entry does, so it is no way in either.

No firmware in this monorepo serves `GET /api/v1/info` or reads `X-Canary-Token`, so the flagship canary is not paired from this SPA either, though it serves the peer list above. The HTTP pairing this section and §3–§4 describe is the reference device-api's.

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
4. The user taps "Pair this Canary". For an HTTP peer (a reference
   device-api) the host is pre-filled and pairing proceeds over HTTP
   (BOOT-tap or token). A canary-wap peer is offered the same button and
   the same BOOT-tap step today, and against WAP firmware that step cannot
   complete (above); its card says where a WAP pairs instead. For
   vision/sense peers there is no HTTP pairing — the SPA explains the
   device and points the user to Home Assistant, where the sensor appears
   automatically via MQTT discovery. Each HTTP peer is independently
   authenticated — knowing a peer exists does not grant API access.

The SPA prefers `mdns_hostname` over `ip` when constructing the peer's
`base_url` because IPs change over DHCP leases but mDNS names don't. The
field is optional; clients fall back to `ip` when absent, and a peer listed
with only an `ip` is not reachable from the SPA this server serves. The
server's CSP `connect-src` is `'self' http://*.local`: CSP cannot name an IP range, so a
request to `http://192.168.1.103` is refused by the browser before it is
sent ([security.md](security.md#security-headers)), and to the page that
looks like an unreachable device (the fetch rejects with the same
`TypeError`). Such a peer has to be added by its `.local` name (§3).

The peer list is read-only. There is no endpoint to add or remove peers via the API. Peers are discovered by the device itself through mDNS and stored in device state.

**Important:** Knowing a peer's IP from the peer list does **not** grant API access. Each device has its own independent token.

### 3. Manual Entry

The SPA allows the user to manually add a device by entering:

- **Device address:** mDNS hostname (e.g., `canary-a3f7.local`). The form also takes a LAN IP (e.g., `192.168.1.47`), but the SPA reaches an IP only when it is the address the SPA itself was opened from.
- **API token:** The device's `X-Canary-Token` value

The SPA checks that the entered address is a private-network one (an RFC 1918, link-local, loopback or IPv6 unique-local address, or a `.local` name) before making any request, and rejects public internet addresses. The browser then holds every request to the CSP the server sends with the SPA, whose `connect-src` is `'self' http://*.local` ([security.md](security.md#security-headers)): a `.local` name on port 80 is let through, and an IP that is not the SPA's own origin is refused before the request is sent, though it passed the SPA's own check. Enter a device by its `.local` name.

After entry, the SPA calls `GET /api/v1/info` with the provided token to verify connectivity and retrieve device metadata. That is the reference device-api's contract: a canary-wap's address and token fail here, since the WAP serves no `/api/v1/info` and reads no `X-Canary-Token` (§2).

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

The SPA parses the receipt, validates the URL is a private-network address, and tests the connection before saving; a receipt whose `base_url` is an IP fails that test under the CSP the way §3's IP entry does. A canary-wap's receipt (its recovery kit, or the QR its dashboard shows) fails that test the way §3's entry does; the kit belongs in the SecuraCV iPhone app.

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

So for the SPA served from device A to reach device B's API, device B must be paired *from that SPA* once: a physical BOOT-button press on B releases its provisioning receipt to the SPA's origin, and B records that origin as durably allowed from then on. CSP comes before CORS: the SPA addresses B by its `.local` name, since the CSP A serves with the SPA refuses B's IP before the request is sent (§2, [security.md](security.md#security-headers)).

This is the reference device-api's CORS. The canary-wap firmware sends no `Access-Control-Allow-Origin` on its receipt route's 403, 409 or receipt, and keeps no list of paired origins, which is part of why it is not paired from this SPA (§2).
