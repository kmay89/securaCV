# The Lab & Flasher — plug it in, watch it come to life, always know it's well

> **North star.** Bringing up a Canary and keeping it healthy should feel like
> *magic* — frictionless, mindless to get right, and impossible to mess up. When
> something does go wrong it should be obvious and one tap to fix, never scary.
> This doc is the spine for that experience so we build it deliberately, reuse
> what already exists, and nothing rots. Grep tokens: `come-to-life`,
> `two-port`, `fleet-over-network`, `remember-wifi`, `mac-keychain`.

## The arc (one story, told in stages)

```
plug it in ─▶ it flashes (can't pick wrong, can't brick)
           ─▶ it COMES TO LIFE  — a receipt + the board's identity (its public
              key, drawn as randomart — verifiable, though not a signed transcript)
           ─▶ it PROVES ITSELF  — a live self-check you can watch (no screen needed)
           ─▶ ONE obvious next step for THIS board (Wi-Fi, watch presence, …)
           ─▶ later: you always KNOW, at a glance, that every Canary is
              up to date and healthy — and heal the ones that aren't
```

Every stage tells the *same* story from *one* source of truth — the device's
**public** self-manifest (`j`) — so the flasher, `securacv.com/canary`, and the
fleet view can never disagree (anti-rot — see `self_star_roadmap.md`). Important:
the manifest is **public, unauthenticated** metadata — the JSON itself is not
signed. Over USB you trust it because you are physically holding the cable; over
the LAN (the fleet view) it must be **authenticated before it is trusted** — see
the security model below.

## What already exists — REUSE, do not rebuild

| Need | Already have | Where |
|---|---|---|
| Flash over USB, can't-pick-wrong, can't-brick | Browser flasher + native app, chip guard, signed images | `canary-local/`, `desktop/`, `docs/browser_flasher.md` |
| A board's status/identity, machine-readable & **public-only** | Self-manifest (`j`) — board, fw, pubkey+fp, chain head, health, boots, tamper, features | `firmware/common/attest/self_manifest.h` |
| "Is it healthy / does it work" | `/api/selftest` aggregator, `diag_run_selftest()`, the manifest `health` score, the self-* self-repair fixes | `canary_wap.ino`, `securacv_diagnostics`, `self_star_roadmap.md` |
| Updates that can't hurt | Signed pull-OTA (Ed25519), **anti-rollback floor**, **safe-mode / A-B** decision layer | `firmware/common/ota/`, `firmware/common/health/boot_policy.h`, `docs/firmware_ota.md` |
| "Which Canaries are near / mine" | Fleet roster + beacon + the console fleet view | `firmware/common/fleet_link/`, `firmware/common/ui/fleet_view.h` |
| Type the home Wi-Fi once for a batch | **Shipped** — remember across boards (session + opt-in persist) | `canary-local/assets/wifi-memory.js` |
| The camera module's model, pinned & fresh | WE2 XMODEM flasher + the freshness check | `canary-local/assets/we2-*.js`, `.github/workflows/vision-model-verify.yml` |

The new work is almost all **surfacing and connecting** these — not new trust
machinery.

## The hard platform truth (this decides *where* each feature can live)

A **browser** page is sandboxed. It **cannot**: read the OS keychain / saved
Wi-Fi password, do mDNS/Bonjour discovery, or fetch arbitrary LAN devices
(mixed-content + our locked CSP `connect-src` = same-origin + loopback + GitHub).
So the browser flasher is superb for **USB bring-up**, and that's its lane.

The **native app** (Tauri, `desktop/`) is a real local process: it can do mDNS
discovery, talk to Canaries on the LAN, read the Mac keychain, and shell out.
**So the network fleet-view, the "use this Mac's Wi-Fi" magic, and remote OTA
belong in the native app** — reusing the very same device APIs and self-manifest
the browser reads over USB. Same story, more reach.

## Phases

### Phase 1 — bring-up magic (USB · browser + native) · *next*
- **Firmware clarity:** say what the picks *are* (Sense = presence · Sense·Wellbeing = +vitals, a bigger privacy surface; Canary vs WAP vs Vision) inline, so three images on one chip aren't confusing.
- **Come-to-life receipt + one next step:** turn the flat post-flash card into a receipt (what was written, ✓verified, the signed identity) + the single obvious next action for THIS board. Pure `postFlashNextStep(product)` proven in `tests/`.
- **Prove-it-works self-check:** reuse `/api/selftest` / the manifest health so a headless board (Sense/mmWave) *shows* it works — "walk past it, presence flips" — instead of being a silent dud.
- ✅ **Remember the home Wi-Fi** (shipped).

### Phase 2 — two-port devices (Vision) · never walk away half-done
The Vision is **two flashes on two ports**: the ESP32 (Vision firmware) and the
Grove Vision AI V2 / WE2 camera module (its model, a different USB VID/PID).
- **Recognize each port** by USB vendor/product id (`port.getInfo()` — we already
  know the WE2's VID/PID in the catalog) and route to the right engine (both
  exist). No guessing which cable is which.
- **A 2-of-2 completion checklist** — flash one, it says "1 of 2 done; now the
  other port" with clear instructions; it won't call the job done until both are.
- **Self-check both:** the ESP32's manifest confirms it's running Vision firmware
  *and* sees the camera + model live — the 1:1 "did both actually take" proof.

### Phase 3 — the fleet view over the network (NATIVE app) · the big one
"Always know, at a glance, that every Canary is up to date and healthy."
- **Discover** Canaries on the LAN (mDNS advertise per the device registry, or
  add one by address) — read-only.
- **Status** by fetching each one's public self-manifest + `/api/selftest`:
  version (↑ update available?), health, tamper, uptime — the same readout the
  USB flasher shows, now for the whole fleet.
- **One-tap update:** poke the device to run its *own* signed OTA check. The
  device stays the trust boundary; the tool never holds a key or pushes an image.
  **Prerequisite:** offer over-the-air update only once safe-mode/A-B is enabled
  in the target build (see the security model) — until then a bad OTA can need a
  cable, which is unacceptable for a *remote* update, so this stays gated.
- **Doctor & heal:** surface the existing self-test + self-repair fixes per
  device; where safe-mode/A-B is live, a bad update reverts or degrades to a
  recoverable console instead of hanging.

### Phase 4 — the last-mile magic
- **"Use this Mac's Wi-Fi"** (native): read the current SSID + its password from
  Keychain (one-time Allow / Touch ID) and pre-fill — zero typing.
- **Cross-platform** fleet-view once the shape is proven on the Mac.

## Security model for the networked features (thought out up front)

The reason this is safe is that **the device — not the tool — is the trust
boundary**, and that's already true:

- **Updates:** the Canary only ever installs an **Ed25519-signed** image it
  verifies itself, and the **anti-rollback floor** refuses any downgrade even if
  validly signed. The tool's "update" button only asks the device to *check*; it
  carries no signing key and pushes no bytes. **Recoverability, told honestly:**
  the **safe-mode / A-B** net that makes a bad update *revert* instead of hang is
  written but **not yet enabled in the shipping Arduino/Canary builds**
  (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` + the boot wiring — `self_star_roadmap.md`
  §TODO 2). Over USB you can always recover (the bootloader is mask ROM); **over
  the air, until safe-mode/A-B ships, a bad update can leave a device needing a
  cable.** So **remote fleet-update must gate itself on safe-mode/A-B being live**,
  and until then the UX says "may need a cable to recover," never "can't brick."
- **Discovery is read-only.** mDNS is advertise-only; discovering a Canary opens
  no control channel.
- **Status is public-only, and must be *authenticated* over the LAN.** The
  self-manifest leaks nothing (no secret ever leaves the device — same rule as the
  trust card), so reading it is safe. But it is **not signed as transmitted**: over
  USB you trust it by physical connection; over the LAN a spoofer could serve a
  fake or stale one. So the fleet view must **verify each device against its pinned
  key via challenge-response** — the `SECURACV-ATTEST-v1` attestation already in
  `firmware/common/health/test_console.h` (nonce → signed by the identity key) —
  before showing a Canary as authentically "healthy / up to date." Anything
  unverified is shown *as* unverified, never as authentic.
- **Control is authenticated.** Anything that *changes* a device (trigger OTA,
  config, pairing) rides the device's existing credential gate (the canary-wap
  route-security model: every mutating route is Bearer/session/pair-token gated).
  The tool authenticates to the device; it is never trusted implicitly.
- **Feel, not fear.** Because the guarantees are real, the UI can *say* them
  plainly — "signed ✓ · verified ✓ · recoverable ✓" (USB flashing is always
  recoverable; over-the-air becomes so once safe-mode/A-B ships) — and keep
  warnings gentle and actionable, never alarming.

Any weakening of these defaults goes through the `THREAT_MODEL.md` /
`LESSONS_LEARNED.md` process in the PR template.

## Build order

Phase 1 first — it's the reusable foundation (the come-to-life receipt +
prove-it-works self-check is exactly what Phase 2's two-port self-check and
Phase 3's fleet "is it healthy?" reuse). Phase 3 (network fleet-view) is the
headline, and it's native-app work that stands on Phases 1–2 plus the signed-OTA
trust machinery — and its remote-update step depends on the safe-mode/A-B
enablement (RoT Phase 1) actually shipping first, so the two tracks are linked.

## Adjacent bets (captured, not yet scheduled)

- **Beyond the Canary — a Raspberry Pi "just works" Home Assistant appliance.**
  Same spirit: type your Wi-Fi, flash, and it boots into a working setup with the
  Canary bridge + tools baked in, self-healing and self-updating for years. Two
  honest constraints shape it: (1) a Pi flashes a **whole OS image to an SD card /
  USB stick**, not USB-serial — and a browser can't write raw block devices, so
  this is a **native-app** job (the shape Raspberry Pi Imager / balenaEtcher use,
  with elevated privileges); (2) much of "flash a Pi for Home Assistant" is
  *already solved* by **Home Assistant OS + Raspberry Pi Imager**, so our leverage
  is a **pre-baked image or HAOS add-on** that bundles the Canary integration and
  the self-heal / auto-update posture — not a new imager. Anything genuinely
  reusable there (a clean flash-and-provision flow, the self-check UX) could be
  contributed back to the HA community. A separate, larger track — worth doing,
  but after the Canary bring-up + fleet-view flow is proven.

## Related

[`browser_flasher.md`](../browser_flasher.md) · [`firmware_ota.md`](../firmware_ota.md) ·
[`self_star_roadmap.md`](self_star_roadmap.md) · [`hardware_root_of_trust.md`](hardware_root_of_trust.md) ·
[`device_trust.md`](../device_trust.md)
