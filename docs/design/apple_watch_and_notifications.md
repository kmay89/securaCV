# Apple Watch app & the notification experience — scoping RFC

Status: **design / RFC — no code yet.**

This document scopes two things that are really one thing:

1. A notification doctrine for the SecuraCV monitor apps — the specific
   discipline that makes alerts *the* hero feature instead of the reason
   people turn alerts off.
2. An **Apple Watch app** ("SecuraCV on your wrist") that rides on that
   doctrine, plus the "everything still works without Apple" web path so a
   user on Chrome, Firefox, or Android gets the same truth with zero native
   install.

It builds directly on:

- [`iphone_companion_app.md`](iphone_companion_app.md) — the foundational
  RFC, especially §5b ("the alert *is* the hero").
- [`alert_relay.md`](alert_relay.md) — the metadata-only remote poke.
  **Still the critical path, still unbuilt.** Everything wrist-shaped in this
  doc is downstream of it.
- `docs/review/02-roadmap.md` — the invariant sentence: *"A metadata-only
  push relay (tokens, never footage) is the only acceptable cloud
  touchpoint."*
- `firmware/projects/canary-wap/arduino/canary_wap/notify.{h,cpp}` — the
  Phase 8 quiet-by-default decision layer (decides, does not deliver).
- `ios/Sources/SecuraCV/Alerts/AlertCenter.swift` and `Heartbeat.swift` —
  the delivery-side discipline that already exists in the iPhone app.

A note on scope honesty up front: users arriving from Ring or Wyze expect
"alerts + live view + clip history." SecuraCV Canaries are witnesses, not
cloud cameras — by construction there are no pixels to stream
(`iphone_companion_app.md` §7, Invariant I). This doc scopes the version of
that expectation we *can* meet better than anyone: **live truth** (who's
where, what fired, is the chain intact, is every device provably alive)
delivered calmly, everywhere, without a cloud.

---

## 1. Notification doctrine: effective and not annoying

Industry data is unambiguous: notification fatigue is the #1 reason users
disable security alerts entirely, which defeats the product. Ring's answer
(Single Event Alert — group related motion into one notification) and
Wyze's answer (an AI "no big deal" filter, sold as a $20/mo subscription)
both bolt suppression onto a firehose. We are in a stronger position: the
firmware **already decides quietly at the edge** (Phase 8's five-way filter,
15-minute dedup ring, severity-vs-context gate). What's missing is the
delivery layer that honors those decisions end-to-end — firmware
`notify.h` names this "Phase 10," and this doc is its delivery contract.

### 1.1 The three-tier ladder (already coded, now doctrine)

`AlertCenter.swift` already maps `AlertLevel {digest, important, critical}`
onto Apple's interruption levels. Make that the *system-wide* contract, on
every surface (iPhone, Watch, web, HA, displays):

| Tier | When | iOS/watchOS delivery | Web delivery | Sound/haptic |
|---|---|---|---|---|
| **Digest** | Routine: household presence, ambient events, daily summary | `.passive` — no light-up, no sound, sits in Notification Center / Smart Stack | No push at all; visible on next open + morning digest | none |
| **Important** | Phase 8 says notable: non-household presence, device offline past tolerance, health warning | `.timeSensitive` — breaks through Focus, once | Web Push, normal | single |
| **Critical** | Tamper, chain-integrity failure, heartbeat declared **dark/failed** | `.critical` (entitlement pending — ship `.timeSensitive` until granted, per `iphone_companion_app.md` §10 #6) | Web Push flagged urgent | distinct, repeats until acked |

Doctrine, in five rules (these are the §5b properties made operational):

1. **Silent almost always.** A normal week produces zero audible
   notifications. Digest tier never buzzes anything, anywhere.
2. **One event, one notification.** Dedup is the *firmware's* job (Phase 8
   ring) and the relay must not re-fan a deduped event across transports.
   If the phone got it on LAN (SSE/MQTT) and acked receipt, the relay
   suppresses the push. Ring is retrofitting this as "Single Event Alert";
   we get it for free by deciding at the edge.
3. **Escalate by silence, not volume.** The scary failure is the alert that
   *can't* be sent (power/Wi-Fi cut). The heartbeat dead-man's-switch
   (`Heartbeat.swift`: 5-min beat, 3-miss tolerance, `dark` state) is what
   turns silence into a Critical. Watch and web surfaces show heartbeat
   state at a glance precisely so users learn that *quiet + green* means
   safe, not broken.
4. **Ack travels.** Acknowledging on any surface (Watch tap, phone,
   display's physical ack, HA) clears the alert everywhere. Per-witness
   mute with tamper punch-through already exists in `fleet_model.h`; the
   apps expose it, never reinvent it.
5. **Content-free over the wire.** Pushes carry `{sev, device_id, sig}` and
   nothing else (relay payload schema in `alert_relay.md` — no content
   field, by construction). The Notification Service Extension hydrates
   human copy from the local digest cache. Complete the two NSE TODOs
   (verify `userInfo["sig"]` against the pinned key; hydrate from the app
   group cache) as part of this work — an unverifiable push renders as
   "Unverified alert," visibly worse than a verified one.

### 1.2 User controls that prevent fatigue instead of apologizing for it

- **Per-witness, per-tier reach** — `AlertRule.Reach {onWiFiOnly, anywhere}`
  already models "only bother me remotely for X." Surface it as one
  screen, not a preferences maze.
- **Quiet hours affect Digest and Important only.** Critical punches
  through by definition — this is the smoke-alarm posture, and it is also
  exactly what Apple's `.critical` level is for.
- **The first-run default is our taste, not a questionnaire.** Everything
  ships at the table above; the wizard asks nothing about notifications.
- **A "why did/didn't I get this?" trace.** Phase 8 already produces bounded
  natural-language reason strings. Show them. Trust in the filter is what
  lets users leave it on.

---

## 2. What "live view" means here

- **Live fleet truth, not live video.** The live surface is the fleet
  model's real-time ladder (`Sev`, `Badge`, `Link`, heartbeat) streaming
  over SSE/MQTT — the same data the Witness Wall renders on tvOS. That is
  the thing the Watch glance, the phone's Today tab, and the web app all
  show within seconds of it changing.
- **PEEK stays what it is**: attended, physically-confirmed, thermally
  throttled aiming tooling (`/api/peek/*`). It never becomes a monitoring
  feature and it never reaches the Watch.
- **Analytics come from data we already keep**: the 24-bucket time machine
  in `fleet_model.h` (`hist_count_[24]`, `hist_worst_[24]`) is a ready-made
  sparkline/heat-strip. The web side already has the card vocabulary
  (`canary-cards.js`: `stat`, `band`, `sparkline`, `event`, `trust`) —
  reuse those kinds, don't invent chart types per surface.

---

## 3. Apple Watch app scope

### 3.0 Naming

Not "Canary Watch" — `canary-display-watch` is already the **Canary Watch
Station**, the round bedside display in `canary-local/devices/registry.json`.
The watchOS target is simply **SecuraCV for Apple Watch** (bundle
`com.securacv.witness.watchkitapp`), branded in copy as "SecuraCV on your
wrist." (And a group of Canaries is a *fleet* — here as everywhere.)

### 3.1 Phase W0 — free, ship first: mirrored notifications done right

watchOS mirrors iPhone notifications automatically, with dedup coordinated
by the system. So the moment the relay + iPhone tiers work, the Watch
already buzzes correctly — *if* we do the small work of making mirrored
notifications first-class:

- A `WKUserNotificationHostingController` scene so alerts render as our UI
  (severity color, device name, reason string, verified badge) instead of
  generic text.
- **Actionable categories**: Ack, Mute 1h (tamper punch-through preserved),
  View. Ack from the wrist is the single highest-value interaction.
- Haptic discipline: tier table above; Critical uses a distinct repeating
  pattern.

No new infrastructure. This phase is almost entirely UI + category
registration inside the existing iOS target.

### 3.2 Phase W1 — glanceable: Smart Stack widget + Live Activity mirroring

- `ios/Shared/FleetActivityAttributes.swift` (severity + headline +
  healthy/total + `lastVerifiedAgo`) is already the right Codable payload
  and already compiles into two targets. Add the watchOS widget extension
  as a third consumer: a **Smart Stack fleet card** (accessoryRectangular)
  and **complications** (accessoryCircular/corner: worst-severity dot +
  healthy/total).
- Live Activities started by the iPhone app (e.g. an unacked Critical, or
  "device dark — investigating") appear in the watch Smart Stack
  automatically on watchOS 11+; on watchOS 26 the Relevance API can float
  the fleet widget when an alert is active.
- Data path: the app group (`group.com.securacv.witness`) cache the NSE
  already hydrates from is the same cache the widget timeline reads.
  Background budget honesty: widget refresh is coarse (system-budgeted);
  the *push* is what carries urgency, the widget carries ambient state.

### 3.3 Phase W2 — the app itself: three screens, no more

1. **Fleet glance** — worst-first list: severity dot, name, `Link` state,
   last-verified age. Tap → witness detail with the 24-bucket heat strip
   and reason log.
2. **Heartbeat** — the dead-man's-switch state (`alive / testing / dark /
   failed`) as the hero, with "Test the path" triggering the existing
   round-trip self-test (`runTestAlert(roundTrip:)`) from the wrist.
3. **Ack/mute** — surface `fleet_model.h` semantics; nothing new invented.

Connectivity: `WCSession` transfer from the phone as primary (the phone's
`FleetStore` is the source of truth); optional direct-to-hub URLSession
fetch of `GET /api/fleet` (the tvOS `FleetClient` contract) when the phone
is unreachable and the Watch is on home Wi-Fi. The `isPrivate()` guard from
`DeviceAPI.swift` applies unchanged — the Watch also refuses public hosts.

### 3.4 Explicit non-goals for the Watch

- No video, no PEEK, no audio (invariant, and also the honest reading of
  the hardware: 60 GHz radar, CSI, and edge-inference witnesses don't have
  a stream to show).
- No pairing/provisioning, no key management, no settings beyond
  per-witness mute — that's phone territory.
- No independent-watch-app APNs registration in W0–W2. Independent tokens
  double the relay's audience list for marginal benefit; revisit only if
  cellular-Watch-without-phone becomes a real user story.

---

## 4. iPhone monitor app deltas (to meet "Ring/Wyze-grade" expectations)

- **Adopt the SSE witness stream.** `FleetStore` polls `/info` + `/witness`
  every 20 s today; the device API already serves an authenticated SSE
  stream (`/api/v1/witness/stream` + ticket). On-LAN latency should be
  "display-grade" (~seconds), and SSE also gives the LAN-receipt signal the
  relay uses to suppress duplicate pushes (rule 1.1-2).
- **Today tab gets the time-machine strip** (24-bucket heat strip per
  witness, fleet roll-up sparkline) using Swift Charts — first and only
  chart dependency in the app.
- **Alerts tab becomes the doctrine's UI**: the tier table, per-witness
  reach, quiet hours, and the "why?" trace.
- **Finish the NSE TODOs** (§1.1 rule 5).

---

## 5. "Everything just works" — the non-Apple path

One rule: **the web app is not a consolation prize; it is the reference
monitor.** Apple surfaces add magic on top (Live Activities, complications,
Critical Alerts); they never gate function.

- **Surface**: the device-hosted PWA (`canary-vision/spa/`) and the hub's
  fleet view, both already no-build vanilla JS with a webmanifest. Add
  installability polish (icons, `display: standalone`, offline shell) so
  "Add to Home Screen" yields an app-shaped monitor on any OS.
- **Live**: the SSE witness stream is already built and ticket-authed.
  That's the live view on every browser — no polling, no WebSockets
  needed.
- **Push**: **Web Push with VAPID keys from the hub/relay** — exactly the
  proposal already written in `docs/audit/wap_multi_device_ux_audit.md`
  (zone name + badge + deep link, never pixels, no third party). State of
  support: Chrome/Firefox/Edge for years on desktop + Android; Safari on
  macOS; iOS since 16.4 for home-screen web apps (and iOS 26 opens
  home-screen sites as web apps by default). Safari 18.4+ adds
  **Declarative Web Push** (JSON pushes rendered without a service worker)
  — adopt the declarative format as the payload shape and fall back to the
  service-worker path on Chrome/Firefox, so one relay payload serves both.
  Tier mapping: Digest = no push; Important = normal; Critical = urgency
  header + `renotify`.
- **Android**: the installed PWA with Web Push *is* the Android app. No
  native Android target is scoped; revisit only if PWA limits actually
  bite.

---

## 6. The relay: still the critical path

Nothing above changes `alert_relay.md`; everything above depends on it.
The additions this doc makes to its requirements:

1. The relay speaks **two delivery backends from day one**: APNs
   (token-based, `.p8` key lives on the hub, never on a Canary) and Web
   Push (VAPID private key likewise hub-held). Same content-free payload,
   same dedup ledger.
2. **LAN-receipt suppression**: a client that saw the event over SSE/MQTT
   acks receipt; the relay drops the corresponding push within its window.
3. The heartbeat monitor terminates at the relay (hub → fleet peers →
   relay precedence is `iphone_companion_app.md` §10 #7 — still open, still
   owned by that doc).

---

## 7. Anti-rot, self-healing, self-updating

Use the machinery that already exists — the Watch target must be a *row*,
not a snowflake:

- **`.xcodeproj` never committed.** The watchOS app and widget extension
  are target blocks in `ios/project.yml`; `ios/scripts/heal.sh` generates
  the project on every build and nightly, and `ios-selfheal.yml` remains
  the PR compiler (iOS/watchOS code is authored in containers with no
  Swift toolchain — CI is the first compile).
- **Release catalog**: the Watch app ships inside the iOS app bundle, so it
  rides the existing `ios` row in `.github/release-targets.yml`
  (`ios-v*`, `MARKETING_VERSION` in `ios/project.yml`, gate
  `ENABLE_IOS_BUILD`) — no new row until/unless it ever ships separately.
  The web monitor rides the existing `web` (Pages) row. A future relay
  component gets its own row the day it has code.
- **Lessons already paid for** (`.github/RELEASE_LESSONS.md`): `cp -RL`
  for bundled payloads; `publish: true` is load-bearing; never re-ship a
  new tree under a published version. Apply to the Watch/widget targets on
  day one, per the standing rule ("apply it to every app target, not just
  the one that broke").
- **Self-updating, per surface**: iOS/watchOS via TestFlight/App Store
  (`ios-release.yml`); web is inherently current (Pages deploy); the PWA
  service worker uses a versioned cache keyed off the build stamp so
  clients self-refresh; device firmware stays on the separate signed OTA
  path. `stamp_build.sh` already injects `SECURACV_BUILD_REV` /
  `SECURACV_FW_TRAIN` — show both in every surface's About screen so
  "which version are you on?" is never a support question.
- **Self-healing in the product sense**: the heartbeat is the user-facing
  self-heal — the system notices its own silence and says so. Add a
  monthly automatic path self-test (existing `runTestAlert(roundTrip:)`)
  surfaced as a quiet Digest entry: "Alert path verified end-to-end ✓."

---

## 8. Phasing summary

| Phase | Depends on | Deliverable |
|---|---|---|
| **N0** | — | SSE adoption in `FleetStore`; NSE signature-verify + hydrate; tier/controls UI in Alerts tab |
| **R1** | `alert_relay.md` substrate decision | Relay MVP: APNs + Web Push, content-free payload, dedup + LAN-receipt suppression, heartbeat terminus |
| **W0** | N0, R1 | Watch: custom notification scene + Ack/Mute/View actions |
| **W1** | W0 | Smart Stack widget, complications, Live Activity mirroring |
| **W2** | W1 | 3-screen Watch app (fleet glance / heartbeat / ack) |
| **P1** | R1 | Web monitor as installable PWA + Web Push (declarative payload, SW fallback) |

Apple-side blockers, unchanged from the iPhone RFC: an Apple Developer
account actually enabled (`ENABLE_IOS_BUILD`), and the Critical Alerts
entitlement grant (ship `.timeSensitive` meanwhile; `SecuraCV.dev.entitlements`
already handles unentitled dev signing).

## 9. Open decisions

1. Relay fan-out substrate (owned by `alert_relay.md` §7; this doc adds the
   requirement that whatever is chosen must front both APNs and Web Push).
2. Does the hub or the phone start Live Activities for long-running
   incidents? (Phone-started is simpler and works today; hub-started via
   ActivityKit push needs R1's APNs channel plus a per-activity token.)
3. watchOS floor: 11 (Live Activities in Smart Stack) vs 26 (Relevance
   API). Recommendation: floor 11, progressively enhance on 26.
4. Whether the W2 direct-to-hub path is worth its test burden in v1, or
   `WCSession`-only ships first.
