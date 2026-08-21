# Run the Witness Wall on a real Apple TV — the fast path

The goal here is simple: get the Witness Wall onto an actual Apple TV with the
**least headache**, in the right order, skipping the steps people burn an
afternoon on. There are two tracks — pick by what you're trying to do.

> **Step 0:** the native SwiftUI tvOS target is **built and continuously
> tested** (`WitnessWall/` — see [`README.md`](README.md)): the wall with
> home / business / apartment profiles and skins, zero-typing LAN discovery
> (it probes `canary.local` by itself), and the Rust verification core wired
> into the poll loop — it verifies a sealed log whenever a source serves one,
> and phrases the fleet's status as the fleet's own report until then (no
> kernel ships the sealed-log endpoint yet). What's left is Apple's side —
> the account and keys — which is exactly what Tracks B and C below walk
> through.

| I want to… | Track | Time | Apple Developer account? |
|---|---|---|---|
| **See it on the TV right now**, for a demo | **A — AirPlay the emulator** | ~2 min | none |
| **Run the real native app** on my Apple TV | **B — Xcode on-device** | ~15 min | a free Apple ID |
| **Share it with a few testers** | **C — TestFlight** | +10 min | the $99/yr Program |

---

## Track A — AirPlay the browser emulator (no account, no Xcode, ~2 min)

tvOS has no user-facing web browser, so you don't open the URL *on* the Apple
TV — you mirror it *from* a Mac, iPhone, or iPad that's on the same network.
This puts the real Witness Wall UI on the actual panel, perfect for showing
someone the idea before a line of Swift exists.

1. Apple TV: nothing to install.
2. On a **Mac**: open **[securacv.com/witness-wall](https://securacv.com/witness-wall)**
   in Safari, go full-screen, then **Control Center → Screen Mirroring → your
   Apple TV**. (On **iPhone/iPad**: Safari → Control Center → Screen Mirroring →
   Apple TV.)
3. Drive it with the arrow keys / a Bluetooth keyboard paired to the Mac, or the
   on-screen remote. Flip on **Alerts** to see them slide in on the big screen.

That's the whole demo. It's the emulator, not the native app — but it's the
real experience on the real television.

---

## Track B — Run the real native app on the Apple TV (Xcode, ~15 min)

Everything here works with a **free Apple ID** — you do **not** need to pay for
the Developer Program to run on your own device. Do it in this order; it's the
low-headache path.

### One-time setup
- A **Mac** with **Xcode** (includes the tvOS SDK and the tvOS Simulator).
- An **Apple ID** (free). The **$99/yr Apple Developer Program** is only needed
  for Track C (TestFlight / the App Store).
- The tvOS app target is in the repo (`tvos/WitnessWall/`, XcodeGen — run
  `xcodegen generate` there, or just open the folder in Xcode after
  `brew install xcodegen`). The witness core and the release pipeline are
  already in place.

### The fast sequence
1. **Simulator first — zero hardware headaches.** Open the Xcode project, pick
   an **Apple TV** simulator as the destination, and **Run**. This proves the
   app builds and the Rust `witness-core` links before any device is involved.
   No account needed beyond signing in Xcode with your Apple ID.
2. **Pair the Apple TV to Xcode (wireless).** Newer Apple TV 4K has no data
   port, so pairing is over the network:
   - Same Wi-Fi for the Mac and the Apple TV.
   - Apple TV: **Settings → Remotes and Devices → Remote App and Devices**.
   - Xcode: **Window → Devices and Simulators → Discovered**, select the Apple
     TV, **Pair**, and enter the code shown on the TV.
3. **Sign with your free Apple ID.** In Xcode → target → **Signing &
   Capabilities**: set **Team** to your personal team, leave **Automatically
   manage signing** on, and give it a **unique bundle id** (e.g.
   `com.<you>.witnesswall`).
4. **Run on the device.** Choose the Apple TV as the destination and **Run**.
   The first launch fails with *"Untrusted Developer"* until you trust it:
   **Apple TV → Settings → General → VPN & Device Management → your Apple ID →
   Trust**. Run again.
5. **Point it at your kernel — usually a no-op.** On launch the Wall probes
   the well-known LAN addresses by itself (`canary.local:8099`, then
   `canary.local` — the same list the desktop Flasher and Lab probe), so on a
   standard install the fleet just appears. Keep the Apple TV and the
   SecuraCV host on the **same LAN**; if your hub lives at a custom address,
   the on-screen prompt takes it once and remembers it. (Unlike iOS, tvOS
   generally doesn't interpose the Local Network permission prompt — if the
   fleet stays empty, it's almost always the router, not a permission.)

### The gotchas that actually bite (and the fix)
- **"Untrusted Developer."** Trust the app under *Settings → General → VPN &
  Device Management* on the Apple TV. One time per Apple ID.
- **App vanishes after ~7 days.** Free Apple IDs sign for 7 days — just **Run
  again** from Xcode to renew, or join the Program for a year + TestFlight.
- **Xcode can't see the Apple TV.** Both devices on the same subnet; re-open
  *Remotes and Devices*; disable **client isolation / AP isolation** on the
  router (it silently blocks device pairing *and* Bonjour).
- **Empty fleet / no events.** Almost always the network, not the app: the
  kernel is on a different VLAN/subnet, or the router's AP/client isolation is
  blocking the TV from reaching `canary.local`. Put both on one subnet and
  turn isolation off; the Wall keeps re-searching by itself and will pick the
  fleet up the moment the route exists.
- **Kernel is `http://` on the LAN.** Already handled — the shipped
  `Support/Info.plist` sets `NSAllowsLocalNetworking`, which permits plain
  `http://` to hosts on your own network (and only those; arbitrary public
  `http://` stays blocked). Nothing to add. The one setup that needs more is
  a kernel served over `https://` with a self-signed certificate — use a
  trusted certificate or plain LAN `http://` instead.
- **Simulator can't see real Canaries.** Bonjour to physical devices is
  unreliable from the Simulator — use a real Apple TV for live data, or the demo
  dataset in the Simulator.

---

## Track C — TestFlight, the moment you want other people on it

This is where the [autopipeline](../docs/tvos/AUTOPIPELINE.md) does the work —
no manual Xcode *Archive → Organizer → Upload* dance.

The pipeline is built and waiting; what it needs from a human is the Apple
account and five credentials. Do these **once, in this order** — each step
exists because skipping it has already cost a burned release
(`.github/RELEASE_LESSONS.md` 2026-07-28 d–h). Which certificate goes in
which secret is mapped in [`docs/APPLE_SIGNING.md`](../docs/APPLE_SIGNING.md) —
read it before touching an `APPLE_*` secret.

1. **Join the Apple Developer Program** ($99/yr) and create the app record in
   **App Store Connect**: platform tvOS, bundle id `com.securacv.witnesswall`.
2. **Mint the App Store Connect API key** (Users and Access → Integrations →
   App Store Connect API) with the **Admin** role — a lesser role fails only
   at export time, and a key's role can never be upgraded afterward. Download
   the `.p8` once and set:
   - `APPLE_API_KEY` — the Key ID
   - `APPLE_API_ISSUER` — the Issuer ID
   - `APPLE_API_KEY_BASE64` — `base64 -i AuthKey_<KEYID>.p8`
3. **Create the Apple Distribution certificate** (if the iPhone app hasn't
   already — one certificate covers every App Store target), export it as a
   `.p12`, and set `APPLE_CERTIFICATE` (base64 of the `.p12`) +
   `APPLE_CERTIFICATE_PASSWORD`. Do **not** reuse the desktop secret — the
   Mac apps' Developer ID identity lives separately in
   `APPLE_DESKTOP_CERTIFICATE` for hard-won reasons.
4. **Set the team**: `APPLE_DEVELOPMENT_TEAM` (or `APPLE_TEAM_ID` — either
   works, set one) to the 10-character Team ID.
5. **Register one device** (any iPhone or the Apple TV itself, Certificates →
   Devices). A brand-new team with zero devices cannot archive — Apple won't
   mint the development profile automatic signing needs, and the failure
   arrives mid-archive with a misleading message.
6. **Flip the repo variable** `ENABLE_TVOS_BUILD` to `true` (Settings →
   Secrets and variables → Actions → Variables). The gate is on the upload,
   not the build — PR CI has been compiling and testing the app all along.
7. **Dry-run first**: Actions → *tvOS release (Witness Wall)* →
   `publish: false`, `export_method: app-store-connect`. This proves signing
   end-to-end without spending a version number (a post-upload rejection
   burns one).
8. **Ship**: bump `MARKETING_VERSION` in `WitnessWall/project.yml` if needed,
   then either press **Update everything (only what needs it)** or:
   ```sh
   git tag tvos-v0.1.0 && git push origin tvos-v0.1.0
   ```
   `tvos-release.yml` builds the Rust core, archives, signs, uploads to
   TestFlight, and cuts the tag only after a verified upload.
9. Add testers in App Store Connect. They install the **TestFlight** app on
   their Apple TV once; every future `tvos-v*` tag reaches them on its own.

---

## The one-screen summary

- **Just to *see* it on the TV today:** Track A (AirPlay), ~2 min, no account.
- **To *run* the real app:** free Apple ID → Simulator (~5 min) → device (add
  ~10 min for pairing + trust). No payment.
- **To *share* it:** the Program, then push a `tvos-v*` tag — TestFlight does
  the rest.

---

### Trademarks

Apple, Apple TV, tvOS, Siri, Mac, iPhone, iPad, HomeKit, AirPlay, Xcode, and
TestFlight are trademarks of Apple Inc., registered in the U.S. and other
countries and regions. App Store and App Store Connect are service marks of
Apple Inc. SecuraCV is an independent project by Errer Labs and is **not
affiliated with, endorsed, sponsored, or certified by Apple Inc.** References to
Apple products here are nominative — for identification and interoperability
only.
