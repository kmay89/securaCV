# The alert relay — remote "pokes" without a cloud (design)

**Status:** the flagship ntfy lane is **built** — the `alert_relay` bin behind the
`alert-relay` Cargo feature, with the poke vocabulary, the fingerprint-free
subscribe list, and the per-class debounce as a pure, unit-tested core
(`src/relay/`). Fan-out beyond ntfy, payload encryption, the away-detection
policy, and the mesh-gateway outage path remain design (§7). Two build-time
decisions the implementation made, recorded here: the fan-out substrate is an
in-tree Rust notifier, not an Apprise sidecar (§7 leaned Apprise; zero new
crates beat a Python runtime on the hub — `ureq` was already vetted for `tsa`),
and the poke carries **no timestamp field at all** — §1's example `ts` would
rebuild the event-timing oracle that Invariant III's bucketing removes, so the
ntfy lane ships without it and the per-sink postures are: ntfy = coarse
owner-facing sentence, iPhone lane = contentless `{sev}` wake (the stricter
contract from the iPhone RFC §5).

**The second-person leg is built, on a different substrate than this doc
assumed.** "If nobody answers, tell someone else" now ships in the iPhone app
over CloudKit sharing (`docs/design/alerts_event_history.md` §6,
`cloudkit_backend.md` §6.5): the owner invites people through Apple's own
sharing sheet and their devices subscribe to one shared zone holding nothing
but "an alarm wasn't answered". For an Apple household that beats every option
below — no server, no account, no third party, and the participant's push is
worded by the participant's own device. What this doc still owns is everyone
that route cannot reach: a household member on Android, a neighbor, a channel
the owner picked themselves, and the outage path in §5 where the house has no
internet at all.

This scopes *how a Canary tells you something while you're
away* without breaking the local-first, own-nothing promise. It builds directly on the invariant the
[iPhone companion RFC](./iphone_companion_app.md) §5 already set: **the only acceptable cloud touch is
a metadata-only wake relay — a token or a coarse alert string, never footage, never event content.**
This doc makes that concrete, vendor-neutral, and — crucially — resilient to the one event that
breaks every naïve design: a power outage.

**The one-sentence version:** the device emits a tiny local event; a pluggable relay on the *hub*
(not the device) fans a coarse, opt-in "poke" out to whatever free channel the owner picked — ntfy by
default — and the poke never carries the witness data.

---

## 1 · The three rules that keep this invariant-legal

1. **Metadata only, never content.** What leaves the LAN is a short human-readable string the owner
   opted into ("Living Room 4 °C — furnace may be down") plus a deep link. No sensor stream, no image,
   no sealed event, no stable device fingerprint in the cloud path. Even the worst-privacy channel
   below only ever sees that one sentence you chose to leak.
2. **The relay lives on the hub, not the ESP32.** The device publishes one minimal local event
   (MQTT/HTTP, no internet). A constrained, internet-exposed sensor must not hold TLS trust stores or
   API tokens for N vendors, and adding a channel must not require a firmware flash. Secrets and
   vendor logic stay on the hub (HA add-on / a Pi / `adapter_host`).
3. **The link points home, not to the cloud.** The poke's deep link opens the **local** UI; if the
   owner is off-LAN, *they* initiate the connection home (or via HA/Nabu Casa remote). The alert
   transport never carries the content it's alerting about.

```
ESP32 sensor ──(local MQTT/HTTP, no internet)──▶ Hub (HA / Pi / adapter_host)
                                                   │  evaluate rule · is owner "away"?
                                                   ▼
                                          pluggable alert-sink (Apprise-shaped fan-out)
                                          ├─ ntfy      (flagship default)
                                          ├─ HA push / pushover / telegram / …  (owner-added)
                                          └─ github-issue / trello   (optional "away timeline" log)
```

**Minimal payload** — coarse text + local deep link, nothing else:
```json
{ "sev": "warn", "title": "Furnace may be down",
  "body": "Living Room dropped to 4 °C",
  "link": "https://canary.local/e/8f2a", "ts": 1753372800 }
```

---

## 2 · The flagship default: ntfy

[ntfy](https://ntfy.sh/) is the recommended zero-config default because **its native model *is* our
invariant**: you `POST` a string to a topic URL and subscribers get a push — account-free, and both
server and mobile apps are open source, so an owner can self-host and remove all limits.

Honest caveats we must document, not paper over:

- **Not end-to-end encrypted out of the box.** The documented pattern is an unguessable topic name
  (treat it like a password) and, if you want secrecy, **encrypt the payload yourself** before
  POSTing. Never call it "E2E" unless we add that layer. For a coarse alert string this is usually an
  acceptable trade; say so plainly.
- **Free public server ≈ 250 messages/day.** Plenty for real alerts, nothing near it unless a rule
  loops — so rules must debounce (§4).
- **iOS instant push cannot be *fully* self-hosted — and that's fine.** Apple requires background
  pushes to arrive via APNs, so a self-hosted ntfy forwards a **contentless** "wake up" through
  ntfy.sh → APNs, and the phone then pulls the actual text *from your box*. That contentless upstream
  poke is arguably the strongest possible form of our invariant — the cloud sees a wake, never the
  words. The honest line for iPhone owners: "literally zero cloud touch" isn't physically achievable
  for instant push; the contentless relay is as close as physics allows.

An ESP32 or hub POST is one line (`curl -d "Living Room 4C" ntfy.sh/<topic>`), so even a fallback
path that skips the hub is trivial.

---

## 3 · Channels — pick by owner type, stay vendor-neutral

The smart design is **not** a driver per vendor — it's a thin fan-out so the owner lists channel
URLs in config and we stay neutral. [Apprise](https://github.com/caronc/apprise) (a maintained
library + [API sidecar](https://github.com/caronc/apprise-api) normalizing **100+ services** behind
one URL syntax) is the right foundation for that layer: the hub exposes one internal endpoint, Apprise
does the rest. It is a *router*, not a transport, and not E2E — exactly the role we want it in.

| Owner type | Recommended path | Why |
|---|---|---|
| **Default / non-technical** | **ntfy** (ntfy.sh, one-tap topic) | account-free, self-hostable later, invariant-shaped |
| **"I run Home Assistant"** | **HA companion-app `notify`** | push is **free via FCM/APNs without Nabu Casa** (Nabu Casa buys remote access/voice, *not* the notifications); zero extra vendor. HA can also drive ntfy natively |
| **"Zero cloud, self-host all"** | **self-hosted ntfy** (Android clean; iOS needs the contentless APNs relay) or **Gotify** (Android/web; weak iOS) | no third party sees anything but a contentless wake |
| **Crypto-maximalist** | **Matrix** (self-host + E2E) or **Signal** (`signal-cli`, E2E) | genuinely private, but heavy setup — power-user only |
| **Reliable paid** | **Pushover** (one-time ~$5, 10k msgs/mo) | purpose-built, rock-solid; the fee is the only friction |
| **Easiest POST target** | **Telegram bot** / **Discord webhook** | trivial, ubiquitous — but plaintext to the vendor; fine for a coarse string, never more |

Two things to **avoid**: **email-to-SMS gateways are dead** (AT&T shut theirs June 2025; Verizon and
T-Mobile likewise) — never build on them; and **IFTTT/Zapier free tiers** effectively gate out
webhook alerting — not a dependency worth taking.

---

## 4 · The clever durable-log tier (the "Trello / GitHub" instinct)

There's a real distinction the design must respect: **real-time push vs. durable timeline.** The
SaaS-as-a-bus ideas are *bad* push and *excellent* logs — offer them as an **optional secondary "away
timeline" sink alongside** a real push channel, never as the primary alert:

- **A GitHub issue per alert** — an append-only, cryptographically-timestamped, greppable, exportable
  audit trail, with GitHub's own notifications riding along. The cleverest free durable log; it even
  rhymes with our tamper-evident ethos (though it is *not* the sealed chain — it's a convenience
  mirror). Keep the rate low so it reads as genuine alerts, not automated spam.
- **A Trello card per alert** — a friendlier, browsable "what happened while I was away" timeline in a
  normal mobile app a non-technical owner can actually use.
- Google Sheets / Notion are fine *data* sinks (a row per event) but give no real push.
- **MQTT** (incl. free brokers like HiveMQ Cloud) is an internal *bus*, **not** a phone-push
  mechanism — a phone only gets notified if an app holds a live connection or something bridges
  MQTT→push. Great device↔hub fabric; wrong for the last mile.

**Rule:** durable-log sinks debounce and batch; push sinks fire on threshold crossings with
hysteresis so one noisy reading never pages the owner.

---

## 5 · The outage case — why this needs a mesh, not just WiFi

The single most important design consequence, and the reason [LoRa/Meshtastic](../meshtastic_integration.md)
belongs here: **in a power outage your router, WiFi, and internet are down too — so a WiFi/cloud alert
physically cannot get out at the exact moment you most want one** (power out → HVAC dead → the room
your pet is in starts drifting; see the climate/pet use case). The alert path must not depend on house
power or house internet.

**The resilient design — reusing patterns this fleet already trusts:**

- **Detect the outage by absence, not by a heroic last gasp.** A mains-powered node heartbeats; a
  **battery/solar or mesh-powered peer** (or the hub on a UPS) infers the outage from the *silence* —
  the same "presence via active signal, absence via timeout" idiom used for Car Mode departure,
  Guardian check-in, and the Meshtastic adapter. This is robust; a supercap "I'm losing power" last
  chirp is a **best-effort bonus on top**, never the thing you trust (we already found the Meshtastic
  OFF-transition unreliable, meshtastic/firmware#8977, and a supercap last-gasp timing-critical).
- **Carry the poke out over the mesh to an independently-powered gateway.** A battery/solar mesh
  gateway (or a neighbor's node, or a small cellular-backed hub) that has power *and* connectivity
  when the house doesn't is what actually delivers the alert. The gateway runs the §3 relay.
- **Normal times stay simple:** device → hub → relay over the LAN. The mesh path is the *fallback the
  outage forces*, not the everyday path.

This is also why the outage/power feature and the alert relay are the same design problem: the alert
about losing power is the one alert whose transport must survive losing power.

---

## 6 · Never let it rot

- **One relay, every device.** Climate, air-quality, stove, laundry, presence — every Canary alert
  flows through this one pluggable sink. Adding a channel is a hub config line, not a per-device
  firmware change.
- **Vendor-neutral by construction** (Apprise-shaped), so a service dying (RIP email-to-SMS) is a
  config edit, not a rebuild.
- **Invariant enforced structurally:** the payload schema (§1) has no content field; the device can't
  emit footage even if it tried, because it only ever publishes the coarse local event.

---

## 7 · Open items

- **Pick the fan-out substrate:** adopt Apprise-API as a hub sidecar vs. write a thin Apprise-shaped
  `Notifier` interface ourselves (fewer deps, less coverage). Leaning Apprise.
- **"Away" detection** — how the hub knows the owner is away to escalate to a remote poke (phone
  presence, an explicit arm, HA's own presence). Reuses existing presence, but the policy is unspecced.
- **Payload encryption option** for ntfy (owner-supplied key, encrypt-before-POST) if an owner wants
  content secrecy on a public server — designed, not built.
- **The mesh-gateway relay (§5)** is a real hardware+software path (a powered gateway running the
  relay) — scoped here, not built; ties into the Fence Guard / Ranger mesh work.
- **Severity ladder + debounce/hysteresis defaults** so alerts are trustworthy, not noisy — per
  signal type (a wildfire-smoke spike vs. a laundry-done ping deserve very different cadences).

---

*Sources: ntfy (self-host, free-tier, iOS APNs relay, no native E2E); Apprise / Apprise-API (100+
service fan-out); Home Assistant push-without-Nabu-Casa; Pushover / Telegram / Discord / Matrix /
Signal trade-offs; the email-to-SMS gateway shutdowns (AT&T Jun 2025 et al.); GitHub/Trello as durable
logs; HiveMQ free-tier MQTT. Full URLs live with the research that produced this doc; the invariant
foundation is [`iphone_companion_app.md`](./iphone_companion_app.md) §5 and the strategy docs' "a
metadata-only push relay is the only acceptable cloud touch."*
