# Run the Witness Wall on a real Apple TV — the fast path

The goal here is simple: get the Witness Wall onto an actual Apple TV with the
**least headache**, in the right order, skipping the steps people burn an
afternoon on. There are two tracks — pick by what you're trying to do.

> **Step 0 (be honest):** the native SwiftUI tvOS target is design-stage
> ([`README.md`](README.md)). The **Apple-side mechanics below are evergreen** —
> they're the part that actually costs time — so this runbook is ready the day
> the app target lands. Track A works *today* with zero code.

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
- The tvOS app target from [`README.md`](README.md) (Step 0). The witness core
  and the release pipeline are already in place.

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
5. **Point it at your kernel.** The app finds the local witness kernel over
   **Bonjour**, so keep the Apple TV and the SecuraCV host on the **same LAN**.
   tvOS will prompt for **Local Network** access on first launch — allow it, or
   discovery stays empty. (The built-in demo data renders offline without it.)

### The gotchas that actually bite (and the fix)
- **"Untrusted Developer."** Trust the app under *Settings → General → VPN &
  Device Management* on the Apple TV. One time per Apple ID.
- **App vanishes after ~7 days.** Free Apple IDs sign for 7 days — just **Run
  again** from Xcode to renew, or join the Program for a year + TestFlight.
- **Xcode can't see the Apple TV.** Both devices on the same subnet; re-open
  *Remotes and Devices*; disable **client isolation / AP isolation** on the
  router (it silently blocks device pairing *and* Bonjour).
- **Empty fleet / no events.** That's the **Local Network** prompt being
  declined, or the kernel being on a different VLAN. Re-enable Local Network in
  the TV's app settings and put both on one subnet.
- **Kernel is `http://` on the LAN.** Add an **App Transport Security**
  exception for the local domain (or use the kernel's TLS). Details land in
  [`README.md`](README.md) with the target.
- **Simulator can't see real Canaries.** Bonjour to physical devices is
  unreliable from the Simulator — use a real Apple TV for live data, or the demo
  dataset in the Simulator.

---

## Track C — TestFlight, the moment you want other people on it

This is where the [autopipeline](../docs/tvos/AUTOPIPELINE.md) does the work —
no manual Xcode *Archive → Organizer → Upload* dance.

1. Join the **Apple Developer Program** ($99/yr) and create the app record in
   **App Store Connect**. Add the signing secrets and flip `ENABLE_TVOS_BUILD`
   ([`README.md`](README.md)).
2. Bump the version, then:
   ```sh
   git tag tvos-v0.1.0 && git push origin tvos-v0.1.0
   ```
   `tvos-release.yml` builds, signs with the App Store Connect API key, and
   uploads the build to TestFlight automatically.
3. Add testers in App Store Connect. They install the **TestFlight** app on
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
