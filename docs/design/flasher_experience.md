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
           ─▶ it COMES TO LIFE  — a receipt + the board's signed identity
           ─▶ it PROVES ITSELF  — a live self-check you can watch (no screen needed)
           ─▶ ONE obvious next step for THIS board (Wi-Fi, watch presence, …)
           ─▶ later: you always KNOW, at a glance, that every Canary is
              up to date and healthy — and heal the ones that aren't
```

Every stage tells the *same* story from *one* source of truth (the device's
signed self-manifest), so the flasher, `securacv.com/canary`, and the fleet view
can never disagree (anti-rot — see `self_star_roadmap.md`).

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
- **Doctor & heal:** surface the existing self-test + self-repair fixes per
  device; a bad update degrades to safe-mode/A-B (Phase-1 of the RoT work), never
  a brick.

### Phase 4 — the last-mile magic
- **"Use this Mac's Wi-Fi"** (native): read the current SSID + its password from
  Keychain (one-time Allow / Touch ID) and pre-fill — zero typing.
- **Cross-platform** fleet-view once the shape is proven on the Mac.

## Security model for the networked features (thought out up front)

The reason this is safe is that **the device — not the tool — is the trust
boundary**, and that's already true:

- **Updates:** the Canary only ever installs an **Ed25519-signed** image it
  verifies itself, and the **anti-rollback floor** refuses any downgrade even if
  validly signed. **Safe-mode / A-B** (RoT Phase 1) means a bad update reverts or
  degrades to a recoverable console — it *cannot* brick. The tool's "update" button
  only asks the device to check; it carries no signing key and pushes no bytes.
- **Discovery is read-only.** mDNS is advertise-only; discovering a Canary opens
  no control channel.
- **Status is public-only.** The self-manifest is public by construction (no
  secret ever leaves the device — same rule as the trust card). Reading it over
  the LAN leaks nothing.
- **Control is authenticated.** Anything that *changes* a device (trigger OTA,
  config, pairing) rides the device's existing credential gate (the canary-wap
  route-security model: every mutating route is Bearer/session/pair-token gated).
  The tool authenticates to the device; it is never trusted implicitly.
- **Feel, not fear.** Because the guarantees are real, the UI can *say* them
  plainly — "signed ✓ · verified ✓ · can't brick ✓ · always recoverable" — and
  keep warnings gentle and actionable, never alarming.

Any weakening of these defaults goes through the `THREAT_MODEL.md` /
`LESSONS_LEARNED.md` process in the PR template.

## Build order

Phase 1 first — it's the reusable foundation (the come-to-life receipt +
prove-it-works self-check is exactly what Phase 2's two-port self-check and
Phase 3's fleet "is it healthy?" reuse). Phase 3 (network fleet-view) is the
headline, and it's native-app work that stands on Phases 1–2 plus the shipped
OTA + safe-mode trust machinery.

## Related

[`browser_flasher.md`](../browser_flasher.md) · [`firmware_ota.md`](../firmware_ota.md) ·
[`self_star_roadmap.md`](self_star_roadmap.md) · [`hardware_root_of_trust.md`](hardware_root_of_trust.md) ·
[`device_trust.md`](../device_trust.md)
