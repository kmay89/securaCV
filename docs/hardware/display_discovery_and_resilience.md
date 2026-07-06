# Flock discovery & resilience — how displays and Canaries find and keep each other

> Status: **display-side SHIPPED** (`canary-display` v0.1.x, `FEATURE_MDNS_DISCOVERY`);
> Canary-side gossip + captive-portal onboarding are tracked follow-ups (§5).
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
what flock discovery removes.

## 3. Flock discovery (mDNS `_securacv._tcp`)

Every display advertises an mDNS service on the LAN:

```
_securacv._tcp   TXT: role=display  dt=canary-watch  fw=2.2.0
                      broker=192.168.1.10  bport=1883    <- only while CONNECTED
```

Rules:

1. **Gossip only ground truth.** The `broker=` TXT is added only while the
   device holds a live connection to that broker — the flock never spreads
   guesses.
2. **Join ladder** for a device that needs a broker:
   - compiled/NVS endpoint if real → use it (hand config always wins at boot);
   - else ask the flock: `_securacv._tcp` referral (`broker=` TXT);
   - else any plain `_mqtt._tcp` advert on the LAN (avahi-published mosquitto);
   - else keep rendering "no broker" honestly and re-ask every
     `CD_BROKER_REDISCOVER_MS` (2 min).
3. **Referrals persist.** An adopted endpoint is written to the same NVS
   keys runtime config reads (`mqtt_host`/`mqtt_port`), so the magic
   survives reboots and OTA.
4. **Self-healing rebind.** WiFi healthy but broker dark past 2 min → the
   device re-asks the flock and rebinds. This is the DHCP-moved-broker fix:
   as long as *one* device on the LAN has found the broker's new address
   (or the broker advertises `_mqtt._tcp` itself), the rest converge
   without a re-flash.
5. `.local` broker names are resolved via mDNS at adoption time (plain DNS
   can't resolve them from the connect path).
6. Hostnames are collision-safe and MAC-free: `<device-id>-<6-hex of the
   salted pseudonym>.local` (Invariant III — nothing trackable leaks).

**The user experience:** provision the *first* device by hand (or run a
broker that advertises mDNS). Every device after that: plug in, it joins.

## 4. Failure ladder — what breaks, what keeps working, what the user sees

| Scenario | Canaries | Displays | Recovery |
|---|---|---|---|
| **Home WiFi drops** | Keep witnessing offline; Ed25519 chain keeps advancing (events show as a seq jump, never lost evidence) | Keep rendering last-known fleet, bannered `WIFI DOWN — showing last known state`; staleness ladder runs locally (amber 3 min → red 10 min) | Both sides: exponential backoff (2→30 s), reboot as last resort after 5 min; retained topics repopulate the view in one round-trip |
| **Broker down, WiFi fine** | Sense/keep chains locally; publish resumes on reconnect | Banner `BROKER DOWN`; after 2 min, flock re-ask (in case the broker moved rather than died) | Backoff reconnect + flock rebind |
| **Broker changed IP (DHCP)** | Fail → (follow-up: same rebind) | Fail 2 min → flock referral → rebind + persist → reconnect | Automatic, no re-flash |
| **New device, never configured** | (WAP: captive portal) | Boots, renders "no broker — asking the flock", adopts the first referral, persists it | Automatic if ≥1 sibling is connected |
| **Witness dies silently** | — | Its ring segment/card goes amber (3 min) then red *lost* (Alert-grade; cancels a standing ack) | Baby-monitor semantics: absence is an alarm |
| **Witness LWT "offline"** | — | Amber immediately, red after the lost deadline | Same ladder, honest label |
| **Whole-internet outage** | Unaffected | Unaffected (clock free-runs; SNTP resyncs later) | Nothing to do — the system is LAN-complete |

The one gap that remains by design in v0.1.x: **when WiFi itself is down,
no new events reach the display** (last-known + staleness honesty only).
The planned fix is the passive **BLE Chirp scan** fallback — Canaries
already chirp heartbeat/tamper/alert over connectionless BLE adverts
(`docs/ble_protocol.md` §5), and a display can listen without joining
anything. That closes the loop: WiFi dead, tamper still reaches the glass.

## 5. Follow-ups to finish the magic

1. **Canary-side gossip** — add the same `_securacv._tcp` advert (+
   `broker=` TXT when connected, + the 2-min rebind) to the witness trees
   (`canary/`, `canary-vision`, `canary-sense`). Then *any* single
   provisioned device seeds the whole household, and Canaries survive
   broker moves too. (Displays interoperate with it already — the query
   side is deliberately role-agnostic.)
2. **Captive-portal onboarding for displays** — WAP-parity SoftAP flow for
   WiFi credentials themselves (the flock can't help before the device is
   on the LAN). Until then displays are provisioned by compiled secrets.
3. **BLE Chirp scan fallback** (§4) — off-WiFi alert path.
4. **Broker mDNS advert in the HA add-on / docs** — one avahi service file
   makes even the *first* device zero-config; document it in the getting-
   started guide.
