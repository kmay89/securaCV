# The iPhone app — "set it up once, then it just watches with you" — design

> **Status:** Draft RFC for review. This is a *design and scoping* document —
> the shape of a native iOS companion, the decisions it forces, and why each one
> is the invariant-safe choice. Nothing here ships until the open decisions at
> the end are settled, and **nothing here is a gate**: the product must remain
> fully usable with no phone app at all (see §9).
>
> **Scope:** what the iPhone app is *for*, how it stays alive for a decade
> without App Store churn, how it does alerts and "cloud" without SecuraCV ever
> holding your data, and how you sign in without us learning who you are.
>
> **Out of scope:** flashing/onboarding (that's the desktop Flasher and the web
> Lab), and the sealing/unsealing cryptography (already built — see
> `docs/design/vault_operator_ux_v1_1.md`).
>
> Grep tokens: `iphone companion`, `witness console`, `self-describing UI`,
> `metadata-only relay`, `cloudkit private db`, `hide-my-email`, `secure enclave
> unseal`, `no-account`.
>
> **Implementation:** the native SwiftUI foundation for this design now lives in
> [`ios/`](../../ios/README.md) — the four surfaces, the device transports
> (mDNS/HTTP/BLE), Dynamic Island / Live Activities, HomeKit, AirPlay, on-device
> Ed25519 verify, CloudKit-private-DB sync, and a self-healing nightly pipeline.
> It builds and signs on a Mac / in gated CI (Apple Developer account required).

---

## The through-line

The desktop apps and the web Lab are **birth tools** — they flash a Canary,
provision it, and prove it hatched. You use them once per device and then
mostly forget them.

The iPhone app is the opposite. It is the **living-with-it** surface: the thing
in your pocket for the *years* after setup. Its job is not to configure the
fleet — it is to let you **trust the fleet at a glance, be told when something
matters, and hold the keys that only you should hold.** It is a *witness
console*, not a camera app. It never shows a live video wall, because there is
no video to wall — the fleet emits events, not pixels, and the app is faithful
to that.

That reframing is the whole design. Everything below follows from "the phone is
a **remote control and a key ring over data you already own**," never "the phone
is where your footage lives."

And it is the **hero** of everything we built. The invariants, the on-sensor
detection, the signed chain, the fleet — all of that is *invisible virtue* until
something makes it *felt*. The phone is where it becomes felt: the calm "all's
well" green, the one moment of magic when a Canary you just tapped appears on its
own, and — the emotional peak of the entire product — a **smoke-alarm-grade alert
that reaches you anywhere, running on our cheapest device, for $0/month** while
the cloud cameras charge a subscription for less. Get the alert right and the app
is the reason people trust the whole system. That is §5b, and it is the point.

---

## 1. The five things you asked it to be, mapped to what the system already is

| The ask | How the architecture already answers it |
|---|---|
| **"Hardly ever needs an App Store update. Just works forever."** | Put the logic that changes on the *device*, not in the app. The Canary already self-describes (`/api/v1/info`, `/config` schema, mDNS TXT, `fleet_model.h`). The app is a **thin renderer over self-describing contracts** with strict version negotiation — new firmware features light up without a new binary. §2. |
| **"Maximally useful to manage everything, not complicated."** | One screen answers "is everything OK?" (the fleet health ladder: `Ok/Notice/Warn/Alert/Tamper`). Depth is *revealed*, not *presented*. §3. |
| **"Once set up, the way to get alerts and even cloud."** | On your network: subscribe to MQTT / the SSE witness stream / BLE — zero cloud. Away from home: the *only* invariant-legal cloud touch is a **metadata-only wake relay** — a token, never footage, never event content. §5. |
| **"Apple hidden email + OTP login, like Anthropic — but don't own the data; do it on device / an iCloud job."** | The app needs **no account to work locally** (matches the "no account" promise). Where a contactable identity is genuinely required, use **Sign in with Apple + Hide My Email** so we only ever see a relay address. The "iCloud job that runs" is literally **CloudKit's private database** — your data lives in *your* iCloud; we cannot read it. §4. |
| **"Own their data."** | Secrets (per-device tokens, the vault private key) live in **Secure Enclave / Keychain**. Sealed `.svlt` snapshots are encrypted to *your* public key and can be unsealed **on the phone** — the one place decryption is allowed. We are structurally unable to. §4, §6. |

---

## 2. "Just works forever" — the anti-rot architecture

Apps rot for three reasons: the **backend** they call changes or dies, the **OS
APIs** they use get deprecated, and the **features** they hard-code drift from
the product. We can defeat all three, because SecuraCV is unusually well-suited
to it.

**a) There is almost no backend to rot.** The app talks to *devices on your LAN*
and to *your own iCloud*. There is no SecuraCV API server in the path that can
break, get a breaking version bump, or be shut down. A product with no server
is a product whose app doesn't get bricked by a server.

**b) The device describes its own UI — the app renders it.** This is the single
most important decision for longevity, and the codebase is already built for it:

- `GET /api/v1/info` and `GET /api/v1/config` return the device's capabilities
  and a *sectioned config schema*. The app should render settings **from that
  schema**, not from a hard-coded form. A firmware update that adds a new config
  section appears in the app **with no app update**.
- `fleet_model.h` is the canonical device-state schema (`Witness`, `Sev`,
  `Badge`, `Link`, radar/vitals/tamper fields). The app's data model mirrors it
  and treats **unknown fields as forward-compatible** (render what you know,
  pass through what you don't).
- The mDNS TXT record (`device_id`, `name`, `fw`, `model`, `dt`, `role`,
  `broker`, `bport`) is enough to populate the fleet list before any HTTP call.

The rule: **the app ships transports and primitives (list, ladder, timeline,
toggle, approve, unseal, verify); the device ships meaning.** New device *types*
(today: WAP, Vision, Sense, Display) render through the same primitives.

**c) Version negotiation, both directions.** Every device advertises `fw` and an
API version; the app advertises the contract range it understands. Old app + new
firmware = graceful "there's a newer capability here, update the app to manage
it" — but core status/alerts keep working. New app + old firmware = hide what
isn't there. Nothing hard-fails on a mismatch; it degrades to the intersection.

**d) Lean on frozen OS primitives, avoid trendy ones.** The APIs this app needs
— **Network.framework** (mDNS/Bonjour + TCP), **CoreBluetooth**, **Keychain /
Secure Enclave**, **CloudKit**, **UserNotifications**, **CryptoKit** (Ed25519
verify, the unseal) — are a decade-stable core of the platform. Build the UI in
**SwiftUI** but keep it plain; the churn in Apple's world is at the fashionable
edges, not in these load-bearing frameworks. A high `minimumSystemVersion` floor
you *raise slowly* beats chasing every new API.

> **Why native, not the scaffolded Tauri-mobile shell?** `desktop-lab/MOBILE.md`
> already scaffolds a Tauri v2 wrapper of the web Lab, and that is the right call
> for *reusing the Lab* on a tablet. But the living-with-it app wants three
> things a WebView fights you on: **CoreBluetooth** (iOS Safari has no Web
> Bluetooth — the device's own `/companion` PWA has to tell people to install
> Bluefy; a native app erases that footnote), **Secure Enclave key custody**,
> and **reliable background notifications**. Those are exactly the native
> capabilities the `desktop-lab` `native_capabilities()` seam marks as "Phase
> 2." This app *is* Phase 2.

---

## 3. What it actually does (the surfaces)

Four tabs. Model the information hierarchy on the calmest "is my home OK?"
apps — one honest answer up top, detail on demand.

**① Today.** The emotional product. A single verified timeline of *events, in
coarse 10-minute buckets*, per zone: "package at the front door," "presence in
the drive," "someone tapped a Canary" — each with its trust badge
(`Verified ✓ / Signed / Unsigned / Failed`). This is the "morning digest" the
strategy docs keep pointing at, finally in a pocket. It never shows a face,
plate, or clip, because those don't exist to show.

**② Fleet.** The health ladder for every Canary — one row per witness, colored
by `Sev`, with `Link` liveness (`Online/Stale/Lost/Offline`), battery, RSSI,
and the tamper flag. Tap through to the device: its config (rendered from the
device's own schema), its signed witness-chain length, a "Verify now" action
that re-checks the Ed25519 chain **on the phone**. Per-witness mute lives here.
This is the `fleet_model.h` view, made touchable.

**③ Alerts.** Not a settings dump — a plain-language "tell me when…" list:
tamper, integrity-chain break, a witness going `Lost`, a zone pattern-break.
Each rule says *where it can reach you*: on Wi-Fi only (default, zero cloud), or
also away from home (opt-in relay, §5). Honesty about reach is a feature.

**④ Keys.** The part no web SPA can do well. Your vault **trustee** role
(approve / deny a break-glass request from another trustee, N-of-M), and — when
*you* are the key holder — **unseal a `.svlt` snapshot right here**, decrypted
in Secure Enclave, shown once, never written to the cloud. Plus the pinned-key
trust list (TOFU): what "Verified" means for each device, and a visible,
auditable "this key changed" alarm.

**⑤ The fifth surface is not a screen: ask, don't open.** The app exports
four verbs as App Intents — *check the fleet*, *test the alert path*, *quiet
hour*, *resume alerts* — so Siri, the Shortcuts app, Spotlight, the Action
button, and Focus/location automations can ask on the user's behalf with
zero setup. This is how the app earns "daily touchpoint" without becoming a
daily chore, and how it stays flexible without scope creep: "quiet the fleet
while I mow" is a Shortcut the user composes, not a setting we ship. The
spoken answer keeps every honesty rule (an old snapshot states its age,
sample data is labeled, a quiet hour can never silence tamper) — see
`ios/README.md` "Ask, don't open" and `ios/Shared/GlanceAnswer.swift`.

Pairing a device is a **guided, physical-presence flow**, reusing the existing
Trust-on-Pair gate: the app finds the Canary over mDNS/BLE, tells you to
**short-tap the BOOT button**, and receives the one-shot `{device_id, base_url,
token}` receipt. No account, no scan of a cloud registry — the device hands you
its own key because you touched it.

---

## 4. Identity & data ownership — how you sign in without us owning you

This is the crux, and the honest answer is layered. **The default is no account
at all**, and we add identity *only* at the exact seams that require it.

**Layer 0 — Local: no identity, no login.** On your own network, the app needs
nothing but the per-device tokens it earned by the BOOT-button tap, held in
**Keychain**. This is the whole product for most people, and it keeps the store's
promise literally true: *"no account, no app store, nothing that expires."* You
can use the app for a decade having never typed an email.

**Layer 1 — Your data syncs through *your* iCloud, not our server.** The "clever
iCloud job that runs" you were reaching for already exists as a platform: it's
**CloudKit's private database**. The device list, alert rules, pinned-key trust
metadata, and event *digests* live in **your** iCloud account. Apple end-to-end
protects it; **SecuraCV has no server in the loop and cannot read any of it.**
Your second iPhone or your iPad just… has your fleet, because it's your iCloud.
That is data ownership implemented as infrastructure, not as a pledge. (Secrets
are handled more carefully — see below.)

**Layer 2 — Identity only where a *reachable* identity is unavoidable.** Two
features genuinely need "a way to reach a specific human": the away-from-home
alert relay (§5) needs *somewhere to send a wake token*, and trustee invitations
need to reach another person. For those — and *only* those — offer **Sign in
with Apple with Hide My Email**, exactly the private-relay pattern you admired:
we receive an opaque, per-app Apple user ID and a `@privaterelay.appleid.com`
address, never your real email, never your name. An **email-OTP fallback** (a
one-time code, no password, no profile) covers people not in the Apple
ecosystem. There is no password, no username, no profile, nothing to breach —
because there is almost nothing stored.

**Layer 3 — Secrets stay on the device that owns them.** Per-device tokens and,
above all, the **vault private key**, live in the **Secure Enclave / Keychain**.
The sealed-snapshot promise is *"the Canary holds only your public key, so it is
structurally unable to open them — only you can."* The phone is the natural
"you." **Open question flagged loudly:** iCloud Keychain would sync that private
key across your devices (convenient) but softens "only this device can open it."
Default should be **device-bound, non-syncing** key material with an explicit,
well-explained opt-in to iCloud-Keychain sync — never silent. (§10.)

The net: **we own nothing.** Locally there's no account. Your fleet metadata is
in your iCloud. Your secrets are in your Enclave. The only thing that ever
touches a SecuraCV-adjacent server is a contentless wake token — and even that
is optional and self-hostable.

---

## 5. Alerts & the *one* cloud touchpoint the invariants allow

Alerts come in two tiers, and the app is honest about which you're getting.

**Tier 1 — On your network (default, zero cloud).** When the phone is home, it
gets events live: subscribe to the MQTT bus (`securacv/#`), or the device's
**SSE witness stream** (`/api/v1/witness/stream`), or hear a **BLE chirp/beacon**
directly from a Canary even when Wi-Fi is down. This needs no relay, no account,
no internet. Local notifications fire from on-device logic. For many homeowners
this is enough and it is *completely* cloud-free.

**Tier 2 — Away from home (opt-in, metadata-only).** iOS won't run a socket in
your pocket across town, so a true remote push needs a relay. The invariants and
roadmap already drew this exact line and left this exact door open:

> *"a metadata-only push relay (tokens, never footage) is the only acceptable
> cloud touchpoint."* — `docs/review/02-roadmap.md`

So the relay is deliberately **stupid and blind**:

- The Canary (via your hub) sends the relay a pre-registered **APNs device
  token** and a **severity class only** — `tamper`, `integrity`, `offline`,
  `pattern` — the same coarse vocabulary already in `const.py`. **No zone name,
  no time, no content, no device identity.** The notification that lands says
  *"A Canary needs your attention"* and deep-links into the app, which fetches
  the real detail from **your** stuff (your iCloud digest, or the LAN when you
  get home). This mirrors the in-repo VAPID/"we never send pixels" prior art
  (`docs/audit/wap_multi_device_ux_audit.md`), moved to APNs because that's what
  iOS backgrounds reliably.
- The relay **stores nothing** beyond the ephemeral routing needed to deliver
  one push. It is **self-hostable** — ship it as a tiny container next to the
  Home Assistant hub for people who want zero third-party touch, with a
  default hosted instance for everyone else. Because it's contentless, a
  compromised relay leaks *"this household got some alerts,"* never *what*.
- It's **strictly opt-in, per alert rule.** Turning it off returns you to a
  fully local product.

This keeps every invariant: no raw export (I), no identity substrate (II —
the token is per-install and revocable, not a global ID), metadata minimization
(III — severity class + batching/jitter, no precise time), local ownership (IV —
content is fetched from your own store, never the relay).

### What actually shipped: the same wake, with nobody in the middle

The app implements Tier 2 **without building the relay above** — through the
user's own iCloud instead (`ios/Sources/SecuraCV/Native/AwayPush.swift`). A
`CKQuerySubscription` on their **private** database makes Apple's push service
deliver a wake to all of their devices whenever a wake record appears; a
device that is home and can see the fleet writes that record.

It is the same contentless wake this section specifies — the coarse class and
nothing else — and it satisfies the constraints *more* completely than a
hosted relay would:

| | hosted relay | shipped (iCloud) |
|---|---|---|
| Third party in the path | SecuraCV (or a self-hosted box) | none but Apple, whom the user already trusts with their iCloud |
| APNs key / token registry | we hold both | we hold neither |
| What a breach leaks | "this household got some alerts" | there is no server to breach |
| Setup for the user | register with a relay | sign in to iCloud (already done) |

**The honest limit**, stated in the UI rather than hidden: iOS will not run a
socket in your pocket across town, so *something at home* has to notice and
post the wake — the phone while it's on Wi-Fi, or better an always-on SecuraCV
(Apple TV / Mac / a docked iPad). This is the same constraint HomeKit has with
its home hub. With nothing home, away alerts cannot happen, and `AwayReach`
says exactly that instead of leaving an "Anywhere" toggle looking like a
working promise.

The relay is **not abandoned** — it remains the right answer for households
that want away alerts without iCloud, and it stays cheap to add: the receiving
side (`Shared/WakePayload.swift`) already decodes both envelope shapes, the
flat `{"sev": …}` body a relay would send and CloudKit's query notification,
so standing one up needs no change to the app or the NSE.

---

## 5b. The alert *is* the hero — smoke-alarm-grade, on a $12 Canary

The away-from-home alert is the feature people pay Ring/Nest **$60–240 a year**
for, and our cheapest Canary — a XIAO ESP32-S3 that already runs person
detection on its NPU, signs with Ed25519, and carries a TLS stack — can do the
whole job for **$0/month**. But "it can send a push" is the easy 20%. The hard,
*heroic* 80% is making that push behave like a **smoke alarm**: silent for
months, unmissable when it matters, and *provably alive* the entire time in
between. A security alert you aren't certain will fire is worse than none — it is
false comfort. So the design borrows the four properties that make a smoke alarm
something you actually trust with your life:

**1. Silent almost always — so you believe it when it speaks.** Push is a scarce,
sacred channel and we refuse to abuse it. Only a tiny, *user-armed* set of events
may ever push: tamper, a Canary going dark unexpectedly, and the specific
critical zone events you opted into. Everything else — normal comings and goings,
the daily digest, "package at the door" — is **pull, never push**: it waits in
the app for when you choose to look; it never buzzes your pocket. iOS interruption
levels map this one-to-one: `passive` for digests, `time-sensitive` for the
things that matter, and — for genuine life-safety — Apple's **Critical Alert**
level, the one that pierces silent mode and a Focus. That is the literal
smoke-alarm bypass, and Apple gates it behind an entitlement you must justify
(§10).

**2. Provably alive — the chirp that earns the trust.** A smoke alarm chirps to
prove its battery; ours proves the *entire delivery path*. The Canary sends a
signed **heartbeat** along the same remote route a real alarm would take. The app
shows "last verified end-to-end: 4 min ago" and offers a one-tap **Test Alert**
that round-trips device → relay → APNs → your phone and lights a green check. This
is the single most important reliability feature and almost nobody ships it: you
learn the alarm works *before* the emergency, not during it. It is also what makes
the app the hero — it turns an invisible promise into a green check you can press.

**3. Fails loud, never silent.** Low battery, weak Wi-Fi, an unreachable relay, a
certificate about to lapse, a heartbeat that stopped — each surfaces as a calm,
proactive "your watch has a gap" nudge. Quiet degradation is the one thing a
life-safety device may never do.

**4. Trips on silence, not only on sight — the dead-man's-switch.** A cheap camera
that pushes only when it *sees* something is defeated by unplugging it. Ours
inverts that: **the absence of the heartbeat is itself the alarm.** Cut the power,
jam the Wi-Fi, smash the device — the *silence* trips it. Who notices that silence
while your phone sleeps is the one thing a lone $12 device can't do by itself, and
the fleet already answers it: **Canaries witness each other.** The mesh's existing
beacons/chirps (`spec/beacon_channel_v0.md`) are exactly this off-grid liveness —
any surviving peer with connectivity escalates the "a sibling went dark" alert.
Where a home hub (HA / Pi) exists it watches too; the relay can watch as a last
resort. No single point whose failure is silent.

**5. Escalates like interconnected alarms.** An unacknowledged critical re-alerts,
then reaches a second household member or trustee. One ignored buzz is not the end
of the chain.

### The honest hardware/relay split (so we never oversell it)

The ESP32-S3 genuinely does the whole part that matters — **detect, decide, sign,
trigger, heartbeat** — on-device, no cloud, no subscription. The *one* thing it
cannot do alone is present an **Apple-authenticated** push to *your specific
phone* across the open internet, because APNs demands the app publisher's signing
key — which must **never** ship on a $12 device (leaking it would be
catastrophic). That final leg needs the tiny, stateless **relay** from §5, and it
is **untrusted by construction**: because every alert is **Ed25519-signed by the
device and content-free**, the relay can *drop* a message but can neither *read*
nor *forge* one — the phone verifies the device's signature, so a compromised
relay cannot inject a fake 3 a.m. alarm. It is **self-hostable** on the same Pi
hub many owners already run, so away-alerts can involve **zero** third party; a
default hosted instance — trivial, content-free, cheap — covers everyone else.
Either way: **$0/month, forever**, against the subscription the cloud cams charge
for a strictly weaker guarantee.

> The line to hold: the Canary earns the alert; the relay only carries it, blind;
> the phone proves it — and proves, every few minutes, that it still works.

---

## 6. Apps to model after — and the exact thing to borrow from each

You asked to copy the ones that pulled "works forever, beautifully" off. The
lesson from each is specific, not vibes:

- **Apple Home** — *the calm hierarchy.* One status line answers the whole house;
  everything else is a tap down. And it survives OS changes because the
  *accessories* carry the capabilities (HomeKit's self-describing model), not the
  app. Borrow: server-driven capability rendering, and the one-honest-answer top.
- **Sign in with Apple / the Anthropic login you cited** — *identity as a
  relay.* Prove a human without learning the human. Borrow: Hide-My-Email + OTP,
  no password, nothing to store.
- **Tailscale / WireGuard clients** — *thin app over a protocol that rarely
  changes.* The intelligence is the network contract; the app is a small, stable
  window onto it. Borrow: version-negotiated contracts, forward-compatible
  fields, degrade-to-intersection.
- **Working Copy / Secure Enclave-first apps** — *the phone as a key ring.*
  Keys never leave hardware; sensitive ops happen locally and show once. Borrow:
  the whole Keys tab.
- **Delta Chat / local-first tools** — *no company account, sync through
  infrastructure you already have.* Borrow: CloudKit-private-DB-as-backend, the
  "we run no server" posture as a feature.

The through-line of all five: **the app is thin and the contract is smart.** That
is *why* they don't need constant updates, and it's the bet this design makes.

---

## 7. What it will deliberately *not* do (so it can't rot into surveillance)

Longevity includes not accreting the features that would betray the invariants.
The app **cannot**, by construction, ever grow:

- a live video wall or clip scrubber (Inv. I — there are no pixels to show);
- face/plate/"who was that" search, or any identity selector (Inv. II, VII);
- a cross-device history search or "follow this person" (Inv. VII);
- precise timestamps or a map pin on an event (Inv. III);
- a SecuraCV-hosted footage store or "cloud DVR" (Inv. IV);
- a way for one person to unseal the vault alone (Inv. V — always N-of-M).

Writing these down is part of the spec: an app that *can't* do them is one users
never have to trust us not to do.

---

## 8. Build path (reuses what exists)

1. **Skeleton + discovery.** SwiftUI shell, four tabs, Network.framework mDNS
   browse for `_securacv._tcp`, render the fleet list from TXT records.
   Reference: `canary-vision/docs/discovery.md`.
2. **Pairing + local read.** Trust-on-Pair BOOT-button receipt → Keychain token;
   `GET /info`, `/config`, `/witness`; render Today + Fleet. Mirror the existing
   SPA (`canary-vision/spa/app.js`) for behavior, not for code.
3. **Live + verify.** SSE witness stream; Ed25519 chain verify on-device with
   CryptoKit (mirror `lib/witness-chain.js`); local notifications on Wi-Fi.
4. **BLE.** CoreBluetooth against the GATT console (Service `8fc1cee0-…`,
   Snapshot char `8fc1cee1-…`, bonded) + chirp/beacon scan — off-grid status.
5. **Keys.** Secure-Enclave unseal of `.svlt`; trustee approve/deny flow (see
   `docs/design/vault_operator_ux_v1_1.md`).
6. **CloudKit sync** of device list / rules / digests (private DB).
7. **Away tier.** Sign in with Apple + Hide-My-Email / OTP; the metadata-only
   relay (self-hostable container + default hosted); APNs registration.

Phases 1–5 are a **complete, fully local, no-account, no-cloud app** — shippable
on its own and true to the brand. 6–7 are the opt-in "alerts anywhere" layer.

---

## 9. The brand tension, said out loud

The store says *"$0/month forever — there is no account, no app store, and
nothing that expires."* An App Store app has to *strengthen* that line, not
quietly break it. Non-negotiables:

- The app is **free, optional, and account-free**; the fleet is fully usable
  without it (Home Assistant, the device drive, the web Lab all still stand).
- It **never gates** a feature behind sign-in or the relay.
- No subscription, no upsell, no telemetry — the privacy policy already promises
  *"it would be hypocritical to surveil our visitors."* The app inherits that.

Framed right, the app *proves* the promise instead of straining it: here is a
security app with no account to create, no footage in anyone's cloud, and keys
only you hold.

## 10. Open decisions (settle before building past Phase 5)

1. **Vault private key sync.** Device-bound only, or opt-in iCloud-Keychain sync?
   (Recommendation: device-bound default, loud explicit opt-in.)
2. **Relay: hosted default + self-host, or self-host only?** Trades reach for
   "zero third party by default."
3. **Away-tier identity:** Sign in with Apple only, or also email-OTP from day
   one? (Recommendation: both — Apple is the happy path, OTP the inclusive one.)
4. **iOS floor.** Which `minimumSystemVersion`? Higher floor = fewer legacy API
   headaches = longer life between updates.
5. **Android.** CloudKit is Apple-only; the private-DB-as-backend trick needs a
   different answer (self-hosted sync, or none) on Android. In scope now or later?
6. **Critical Alert entitlement (§5b).** Apply to Apple for the smoke-alarm
   "pierce silent mode" entitlement (must justify life-safety), or ship with
   `time-sensitive` only at first? (Recommendation: launch time-sensitive, pursue
   the entitlement in parallel — it's the literal hero capability.)
7. **Who watches the heartbeat (§5b).** Default silence-detector: home hub when
   present, else fleet peers (mesh mutual-watch), else the relay as last resort —
   confirm the precedence and the miss-window (how many missed beats = "dark").
8. **Relay economics/abuse.** A free hosted relay needs a cheap anti-abuse story
   (rate limits, per-install token revocation) that stays content-free and adds
   no identity — spec it before turning the default instance on.
