# Fleet discovery & resilience — how displays and Canaries find and keep each other

> Status: **display-side SHIPPED** (`canary-display` v0.1.x, `FEATURE_MDNS_DISCOVERY`)
> and **canary-side broker gossip SHIPPED** (`canary-wap`,
> `FEATURE_MDNS_BROKER_GOSSIP` — §5.1); captive-portal onboarding is the
> remaining follow-up (§5). The **direct BLE fleet link** (§3.1) adds a
> broker-free, WiFi-free path — implemented, hardware-validation pending.
> Companion to [`display_ux_design.md`](./display_ux_design.md).

## 1. The promise

Adding the Nth device should feel like magic: power it, and it knows the
household — no IPs typed, no app, no re-pairing the fleet. And when the
network misbehaves, every device heals itself and is *honest* about the gap
(G3: silence ≠ safety).

## 2. What "self-finding" means here (two different problems)

**Devices finding each other — already automatic.** The fleet rendezvous is
the MQTT broker. Displays subscribe to `securacv/+/{status,availability,
health,events,tamper,chain,state}` wildcards, so *every* Canary publishing
to the household broker appears on every display with zero pairing — and
because status/health/tamper/chain are retained, a freshly booted display
repopulates its whole fleet view in one broker round-trip. Ten canaries,
three displays: no N×M pairing, no device lists to maintain.

**Devices finding the broker — the part that used to need a human.** Every
device needs `MQTT_HOST`. That's the one IP a user ever types, and the one
thing that breaks when the broker box picks up a new DHCP lease. This is
what fleet discovery removes.

## 3. Fleet discovery (mDNS `_securacv._tcp`)

Every display advertises an mDNS service on the LAN:

```
_securacv._tcp   TXT: role=display  dt=canary-watch  fw=2.2.0
                      broker=192.168.1.10  bport=1883    <- only while CONNECTED
```

Rules:

1. **Gossip only ground truth — in both directions.** The `broker=` TXT is
   added only while the device holds a live connection to that broker, and
   is retracted (empty-TXT tombstone) the moment the link drops — the fleet
   never spreads guesses, and never keeps seeding a dead or moved endpoint.
   Referral strings are bounded (≥64 chars rejected) — mDNS TXT is
   unauthenticated LAN input.
2. **Join ladder** for a device that needs a broker:
   - compiled/NVS endpoint if real → use it (hand config always wins at boot);
   - else ask the fleet: `_securacv._tcp` referral (`broker=` TXT);
   - else any plain `_mqtt._tcp` advert on the LAN (avahi-published mosquitto);
   - else keep rendering "no broker" honestly and re-ask every
     `CD_BROKER_REDISCOVER_MS` (2 min).
3. **Referrals persist.** An adopted endpoint is written to the same NVS
   keys runtime config reads (`mqtt_host`/`mqtt_port`), so the magic
   survives reboots and OTA.
4. **Self-healing rebind.** WiFi healthy but broker dark past 2 min → the
   device re-asks the fleet and rebinds. This is the DHCP-moved-broker fix:
   as long as *one* device on the LAN has found the broker's new address
   (or the broker advertises `_mqtt._tcp` itself), the rest converge
   without a re-flash.
5. `.local` broker names are resolved via mDNS at adoption time (plain DNS
   can't resolve them from the connect path).
6. Hostnames are collision-safe and MAC-free: `<device-id>-<6-hex of the
   salted pseudonym>.local` (Invariant III — nothing trackable leaks).

**The user experience:** provision the *first* device by hand (or run a
broker that advertises mDNS). Every device after that: plug in, it joins.

## 3.1 Direct BLE fleet link — no broker, no WiFi (the always-there channel)

mDNS/MQTT (§3) is the *rich* channel, but it needs a broker and a shared LAN.
The **direct BLE fleet link** is the fallback that needs neither — a display
finds and reads a nearby Canary over Bluetooth LE alone, whether or not either
device is on home WiFi, and whether or not a broker exists anywhere. BLE is the
right radio here precisely because its use is **not coupled to the WiFi
channel** the way ESP-NOW is: the moment a display joins home WiFi for MQTT,
ESP-NOW to a Canary on a different AP/channel would break — BLE does not.

Two layers, both **unsigned + coarse** (like the Chirp): they feed liveness and
diagnostics and **never** set the `Verified` trust badge.

1. **Presence + status beacon (passive, always-on).** `canary-wap` now
   continuously advertises a compact manufacturer-data beacon (company
   `0xFFFF`, type `0x10`, 11 bytes) carrying its fingerprint suffix, liveness
   flags (tamper / mic-muted / degraded / on-WiFi / alert), battery %, health %,
   and the low 16 bits of its chain height. The 128-bit SCV service UUID and
   `SCV-XXXX` name move to the **scan response** (active-scanning Canaries still
   see both — backward compatible). A display passively scans (`FEATURE_CHIRP_SCAN`)
   and, within a single scan burst, lists every nearby Canary with live status —
   no connection, no pairing, no broker. When WiFi is *also* down the display
   scans continuously instead of in bursts (no coexistence cost to pay). The
   wire format is the single source of truth in `common/fleet_link/fleet_beacon.h`
   and `canary-display/.../beacon_parse.h` (kept identical; host round-tripped).

   **Version 2 — live detections on the glass (canary-vision).** A witness
   with an optical pipeline emits a 13-byte **v2** of the same beacon: the v1
   layout plus a detection **class token** (person / vehicle / animal /
   package — the ObjectClass vocabulary and nothing beyond it) and a
   **confidence percentage**, with the `alert` flag set while its presence
   FSM holds a live detection (an edge republishes immediately; a confidence
   wobble rides the ~5 s refresh). A display that hears it raises a real
   attention event — `person 87% (ble)`, `Sev::Warn`, amber glow + chime —
   with a per-(witness, class) 60 s edge-dedupe so the continuous advert
   can't spam the log or re-cancel acks. Same trust posture as everything on
   this channel: **unsigned**, so it draws attention but never touches the
   `Verified` badge, and the wire carries no identity and no timestamp — a
   class token and a percentage, nothing more (Invariants II/III). Parsers on
   every surface (display BLE + ESP-NOW, sibling rosters, the WAP's
   `/api/nearby`) accept both versions; v1-only senders stay understood
   unchanged.
2. **GATT pull (on-demand rich detail, `FEATURE_FLEET_LINK`).** Tapping a
   Canary on the glass opens a bounded NimBLE **central** connection to that
   device's BLE status GATT service (`5e63a1b0-…`) and reads its fuller
   self-report (chain seq, health, degrade, SD %, mic-muted, battery), then
   disconnects. First-seen address↔fingerprint is TOFU-pinned; a later mismatch
   is refused as a spoof. Read-once-then-disconnect, heap-gated, and it hands the
   shared radio back to the passive listener when done.

> **Status: implemented, hardware-validation pending.** The wire format and
> model ingestion are host-tested and the sketch compiles in CI, but the radio
> behavior (beacon discoverability, the GATT connect, and the peer address
> type on connect) needs a bench smoke-test on real XIAO ESP32-S3 hardware
> before it ships in a signed release. `FEATURE_FLEET_LINK=0` cleanly disables
> the central role if a build needs presence-only. The exact bench gate is
> [`fleet_link_bench_checklist.md`](./fleet_link_bench_checklist.md).

## 3.2 Direct mDNS fleet enumeration — no broker (the WiFi middle path)

Between "MQTT with a broker" (§2/§3, richest) and "direct BLE" (§3.1, no WiFi
at all) sits a WiFi path that needs **no broker**. Every witness firmware
(canary-wap / canary / vision / sense) already advertises `_securacv._tcp` with
identity TXT (`device_id`, `name`, `dt`, `role=witness`; §3). The display now
**enumerates those adverts directly** (`discovery_scan_witnesses`) and drops
each Canary into its fleet — real device id, name, and type — with no MQTT
broker or Home Assistant. It runs **only while the broker is down** (MQTT is the
richer source when present), self-rate-limits (~20 s; the mDNS query blocks
~3 s), and — because mDNS TXT is **unauthenticated LAN input** — feeds
seen+named liveness only, **never the `Verified` badge** (same trust posture as
the broker gossip). Gated by `FEATURE_MDNS_DISCOVERY`.

Net: plug every Canary into the home WiFi and a display finds them all, three
ways in order of richness — **MQTT** (with a broker) → **direct mDNS on the LAN**
(this, no broker) → **direct BLE** (§3.1, no WiFi at all).

## 3.3 Every Canary keeps a fleet roster — mutual "last chirp" awareness

Discovery so far has been display-centric: the glass finds the witnesses. But
the witnesses now keep track of **each other**, too — not just the display. Every
Canary maintains its own **fleet roster**: a small table of the OTHER Canaries it
has heard over the air, keyed by fingerprint suffix, holding each peer's
**last-heartbeat** (`last_seen`, the "last chirp") plus the most recent status its
beacon carried — battery %, health %, chain height, and the liveness flags
(tamper / mic-muted / degraded / on-wifi / alert). Peers age out after 120 s of
silence (the beacon/chirp cadence), the same window the display and the WAP use.

- **Single source of truth.** The roster logic is one pure, host-tested module,
  `firmware/common/fleet_link/fleet_roster.h` — a fixed-size POD table with no
  NimBLE/heap, round-tripped byte-for-byte in `test_fleet_roster.cpp`. Each
  firmware declares one roster and feeds it from its BLE scan callback; a
  status-less chirp (which carries no battery/health) refreshes liveness without
  wiping the last known status.
- **Who feeds it.** `canary-vision` and `canary-sense` gained an RX twin of their
  presence beacon (`src/net/fleet_roster_scan.cpp`, `FEATURE_FLEET_ROSTER`): a
  low-duty passive scan (3 s bursts / 60 s while on WiFi, continuous when fully
  off-grid) that parses each nearby beacon + chirp into the roster. The modular
  `canary` reuses its existing Scout passive scan — one scan, two consumers —
  feeding the same roster (`fleet_roster_feed`, `FEATURE_BLE_SCAN`). The
  `canary-wap` already ran a nearby-Canary roster; it now also decodes the
  11-byte presence beacon, so a sibling's battery/health/chain/flags land in
  `/api/nearby`, not just its presence.
- **Trust posture.** Identical to the beacon it consumes and the display's
  chirp/mDNS ingest: **unsigned presence** — liveness and self-reported status,
  never a verified trust claim. The roster never stores a MAC; peers are keyed by
  the self-reported fingerprint suffix on the wire.

> **Status: implemented + host-tested; radio behavior hardware-validation
> pending.** The roster table and beacon/chirp decode are exercised in host
> tests and every firmware compiles in CI, but the added scan path's on-air
> behavior and BLE/WiFi coexistence cost on the C3/C6 leaf sensors want a bench
> smoke-test (and CI's per-board OTA size guard adjudicates the flash cost —
> `FEATURE_FLEET_ROSTER=0` cleanly disables the scanner per board). Surfacing the
> roster on each witness's serial/MQTT/API is an additive follow-up; the data is
> already maintained and queryable in firmware (`fleet_roster_scan_peer_count()`
> / the WAP's `/api/nearby`).

## 4. Failure ladder — what breaks, what keeps working, what the user sees

| Scenario | Canaries | Displays | Recovery |
|---|---|---|---|
| **Home WiFi drops** | Keep witnessing offline; Ed25519 chain keeps advancing (events show as a seq jump, never lost evidence) | Keep rendering last-known fleet, bannered `WIFI DOWN — showing last known state`; staleness ladder runs locally (amber 3 min → red 10 min) | Both sides: exponential backoff (2→30 s), reboot as last resort after 5 min; retained topics repopulate the view in one round-trip |
| **Broker down, WiFi fine** | Sense/keep chains locally; publish resumes on reconnect | Banner `BROKER DOWN`; after 2 min, fleet re-ask (in case the broker moved rather than died) | Backoff reconnect + fleet rebind |
| **Broker changed IP (DHCP)** | Fail → (follow-up: same rebind) | Fail 2 min → fleet referral → rebind + persist → reconnect | Automatic, no re-flash |
| **New device, never configured** | (WAP: captive portal) | Boots, renders "no broker — asking the fleet", adopts the first referral, persists it | Automatic if ≥1 sibling is connected |
| **Witness dies silently** | — | Its ring segment/card goes amber (3 min) then red *lost* (Alert-grade; cancels a standing ack) | Baby-monitor semantics: absence is an alarm |
| **Witness LWT "offline"** | — | Amber immediately, red after the lost deadline | Same ladder, honest label |
| **Whole-internet outage** | Unaffected | Unaffected (clock free-runs; SNTP resyncs later) | Nothing to do — the system is LAN-complete |

The old v0.1.x gap — **when WiFi itself is down, no new events reach the
display** — is now addressed by the **direct BLE fleet link** (§3.1): the
display passively hears each Canary's presence+status beacon and, on a tap,
pulls fuller detail over a bounded GATT connection, with no broker and no
router. It builds on the passive **BLE Chirp scan** fallback (Canaries chirp
heartbeat/tamper/alert over connectionless BLE adverts, `docs/ble_protocol.md`
§5, `FEATURE_CHIRP_SCAN`). So WiFi dead, presence and tamper still reach the
glass. The fleet link is implemented and host-tested; its on-air behavior is
hardware-validation pending (see §3.1's status note).

## 5.1 Canary-side broker gossip — SHIPPED (`FEATURE_MDNS_BROKER_GOSSIP`)

The witness that the user *actually configures* with broker credentials
becomes the household's broker beacon. `canary-wap` already advertised
`_securacv._tcp` (with `device_id`/`fw`/`host`/`name`/`model` TXT); it now
also publishes **`broker`/`bport` TXT — but only while its own MQTT link is
live** (ground truth), and retracts them (empty-string tombstone) the instant
it drops, on the same link-transition edge the display uses. Byte-for-byte
compatible with the display's parser: `broker` verbatim (IP / DNS / resolvable
`*.local`, ≤63 bytes), `bport` as plain decimal. Net effect: **hand-provision
one canary, and every display afterwards is plug-and-play** — it hears a broker
that is *provably reachable* from a real witness, and never chases a dead one.
The gossip rides the sensor's existing announce blocks plus a loop-level
link-transition sync; gated so MINIMAL (no networking) builds stay clean.

Still open on the sensor side: the same `broker` TXT for the modular
`canary-sense` / `canary-vision` trees (no mDNS there yet — additive when
those become always-on nodes).

## 5.2 First-boot onboarding — SHIPPED (`FEATURE_ONBOARDING`)

The last gap is closed: WiFi itself no longer needs a compiled `secrets.h`.
A fresh display raises a device-unique SoftAP, shows a **join QR on its own
glass**, and walks the user through a captive-portal wizard — wrong-password
recovery, live status, no dead ends — then the fleet referral (§5.1) lands
the broker with zero further input. Full UX choreography, recovery matrix,
and security posture: [`display_onboarding.md`](./display_onboarding.md).
End-to-end: **plug in → scan → password → watching your canaries.**

## 5. Follow-ups to finish the magic

1. **Broker mDNS advert in the HA add-on / docs** — one avahi service file
   makes even the *first* device zero-config; document it in the getting-
   started guide. (With §5.1, a single provisioned canary already covers
   this for any household that has one — this closes the no-canary-yet gap.)

*(BLE Chirp off-WiFi fallback, once listed here, shipped in wave 2 —
`FEATURE_CHIRP_SCAN`, see the trailblazer spec §6. Wave 3 extended it into the
full **direct BLE fleet link** — continuous presence+status beacon plus an
on-demand GATT pull, `FEATURE_FLEET_LINK` — so a display reads nearby Canaries
directly with no broker and no WiFi; see §3.1. Implemented + host-tested,
hardware smoke-test pending before a signed release.)*
