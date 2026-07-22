# Design: the Raspberry Pi hub, flashed in one shot

**Status:** proposal / RFC · **Date:** 2026-07-22 · **Owner:** TBD

> *"Can our flashing tool support the Raspberry Pi with a custom flash that adds
> all the tools they need for it to just work — they put in their Wi-Fi, flash it,
> and the Pi just boots into an amazing Home Assistant setup, no barrier to entry?
> And make sure it never rots, works for years, and can self-heal and update."*

Short answer: **yes — and the durable version is less work than it looks,
because Home Assistant already ships the self-healing, auto-updating appliance we
would otherwise have to build, and we already publish the add-on it auto-installs.**
The job is to collapse today's four-step manual setup into one flash — not to
invent a magic OS.

This doc records the current reality, the recommended architecture, and the two
or three decisions that are a product call before we build.

---

## 1. What the Pi is today (so we build, not rebuild)

The Pi is already a first-class part of the product — as the **Home Assistant
host where your Canaries converge**, running stock Home Assistant OS (HAOS). A lot
of the "just works" machinery already exists:

| Piece | Where | State |
|---|---|---|
| Privacy Witness Kernel as an **HA add-on**, multi-arch **aarch64** image pulled from ghcr (no on-device Rust build) | `privacy_witness_kernel/config.yaml`, `.github/workflows/addon-image.yml` | Ships |
| **Ingress setup wizard + status panel**, MQTT broker **auto-discovery**, auto device key, zero-config Frigate mode | `privacy_witness_kernel/` (`serve_wizard.py`, `wizard/`), `config.yaml` | Ships |
| **HACS integration**, MQTT discovery (`securacv/#`), TOFU per-device PKI, Lovelace cards, blueprints, automations | `custom_components/securacv/`, `homeassistant/` | Ships |
| **SD-card endurance / wear-health** monitor engineered for *years, not months* (no-SMART wear estimate, hysteresis health model, `replacement_recommended`, SoC-thermal) | `docs/sd_card_health.md`, kernel `[storage_health]` | Ships |
| **"Type Wi-Fi once"** flasher memory (session / opt-in persist) | `canary-local/assets/wifi-memory.js` | Ships (ESP flasher) |
| **"The Hub"** teaching page — 3D scrub-apart Pi build, bench terminal, dashboard sketch | `canary-local/homeassistant.html` (`"real": false`) | Ships as a **simulation** |
| Signed **release train** + OTA machinery: `fw_train`, pinned `fw-v<train>` tag, Ed25519-signed manifest, CI "pubkey matches committed key" gate | `canary-local/tools/gen_flash.py`, `firmware/scripts/ota_release.py`, `.github/workflows/firmware-release.yml` | Ships (firmware) |

So the HA-side software, the discovery contract, the endurance story, and the
"type Wi-Fi once" UX are **already built**. The onboarding roadmap in
`docs/strategy/11-home-assistant-platform-architecture.md` §7.7 already commits us
to ESP-Web-Tools / Improv / zeroconf onboarding for Canaries — this feature is the
**hub-side front door** that logically precedes it.

## 2. The gap (what a user does today)

To get a working hub today, per `docs/homeassistant_setup.md`, a user must:

1. Hand-flash HAOS to an SD card themselves (Raspberry Pi Imager or raw `dd`),
   wait out a ~20-minute first boot, create the owner account.
2. Install the Mosquitto broker add-on.
3. Install HACS (`curl | bash`) and restart.
4. Add our custom repo, install the integration and/or the PWK add-on, restart
   again — several restarts before anything appears.

"The Hub" page *simulates* step 1 ("you can't `dd` a card from a web page, so the
bench replays the real commands"). Nothing is actually flashed. **The whole gap is
steps 1–4 collapsed into: type Wi-Fi → write card → boot → done.**

## 3. The one genuinely new thing: writing an OS image to a disk

This is the honest caveat that shapes everything else. **A Canary is a serial
firmware write to an ESP32; a Pi hub is an OS image written to a block device**
(SD / USB / NVMe). Our flasher engine (`espflash` / esptool-js) cannot do it — it
speaks the ESP serial download protocol, not "write bytes to `/dev/sdX`."

That means a new **image-writer** path, and it carries a risk the ESP path never
did:

- **You cannot brick a Canary** — worst case, re-enter download mode. **You *can*
  wipe the wrong disk.** An image writer needs hard guardrails: removable-media
  only, explicit size/model confirmation, and read-back verification after write.
- It needs **elevated privileges** (raw disk access) — different from serial.

This is well-trodden ground (Raspberry Pi Imager, `bmaptool`, balenaEtcher all do
exactly this, open-source), but it is a *new engine with a new footgun*, not a
tweak to the existing flasher. Budget for it as such.

## 4. Recommended architecture: build **on** Home Assistant OS

**Do not roll a custom Raspberry Pi OS. Post-process the official HAOS image.**

The user's hardest asks — *never rots, works for years, self-heals, auto-updates* —
are exactly what HAOS already is, and it's maintained by the whole HA project, not
by us:

- **Immutable A/B root filesystem (RAUC)** with **automatic rollback** on a failed
  boot. This is the *same contract* our Canary firmware implements
  (`docs/firmware_ota.md`: download to the inactive slot → verify → boot →
  self-test → confirm-or-revert; `firmware/common/health/boot_policy.h`). HAOS is
  the Pi analog, already shipped.
- **Supervisor watchdog** restarts crashed add-ons; **hands-off OS + add-on
  auto-updates**. Our aarch64 add-on already rides that update channel — a new
  release is pulled to the fleet's hub with no user action.
- **Community trust.** Tens of thousands of users already run HAOS on a Pi. We
  inherit their soak-testing and their upgrade path instead of owning a bespoke
  image that rots the day we stop babysitting it.

Building a custom image would mean re-implementing RAUC-style A/B, the supervisor,
and the update server ourselves — precisely the "make sure it never rots" burden,
now on our shoulders. The reuse analysis in the OTA machinery agrees: the A/B +
`PENDING_VERIFY` contract "maps cleanly onto RAUC/Mender/OSTree" — and RAUC *is*
what HAOS uses. So: **inherit it, don't rebuild it.**

### How "put in Wi-Fi → flash → boom" actually works on HAOS

The deliverable image = **official HAOS + a small seed written into the boot
partition.** Two seeds, both using established HAOS mechanisms (exact hooks to be
pinned on-hardware in Phase 1 — we mark things `verified` vs `planned` here like
everywhere else):

1. **Wi-Fi** — drop a NetworkManager keyfile onto the boot/`CONFIG` partition
   (`CONFIG/network/my-network`) carrying the user's SSID/PSK. This is the same
   secret our flasher already collects (`wifi-memory.js`) and the same
   local-only-custody promise: it goes onto the card, never to a cloud.
2. **securaCV, zero-touch** — seed a curated **HA backup / first-boot config** into
   the boot partition so onboarding brings up Mosquitto + the PWK add-on (and, at
   the chosen scope, dashboards + blueprints + the integration) already wired. First
   boot → HA onboarding → the **SecuraCV** panel is already there and Canaries are
   already discoverable over MQTT.

We do **not** fork HAOS, host a mirror, or phone home. We post-process a pinned,
checksummed official image and write two files into its boot partition — the same
class of thing Raspberry Pi Imager's customization does.

## 5. Never-rot / self-heal / update — the full mapping

| The ask | How it's met | Owned by |
|---|---|---|
| *Never rots / years* | HAOS immutable A/B rootfs + auto OS updates | HA project |
| *Self-heal* | RAUC rollback on failed boot; Supervisor watchdog restarts add-ons | HA project |
| *Auto-update* | HAOS auto-updates; our add-on rides the Supervisor update channel; extend `gen_flash.py`/`fw_train` with a hub-image asset row so the image itself is a signed, pinned release | HA project + us |
| *SD longevity* | Existing wear-health monitor (years-not-months) covers the card; **recommend NVMe/SSD boot on Pi 5** as the durable default; heat guidance already documented | us (`docs/sd_card_health.md`) |
| *Add-on resilience* | Enable `watchdog:` on the add-on (already on the strategy-doc P0 list) so the Supervisor restarts the kernel if it dies | us (one line in `config.yaml`) |

Net: we get the appliance-grade durability for free by standing on HAOS, and the
one thing genuinely ours — SD/flash wear over years — we already solved.

## 6. Helping the Home Assistant community

Real, upstreamable gifts (the user's instinct here is right):

- **The SD-card endurance / wear-health monitor** is useful to *every* HA-on-Pi
  user, not just ours: a no-SMART wear estimate, a hysteresis health model, and a
  `replacement_recommended` signal with weeks of lead time. Factor it out as a
  standalone add-on (or surface it through HA **Repairs** issues per strategy §7.2)
  and it stands on its own. This is the strongest community contribution.
- **A clean HAOS first-boot provisioning recipe** (Wi-Fi keyfile + backup seed),
  documented and scripted, is reusable by anyone building a turnkey HA appliance.
- Submitting our **add-on repo** to the community add-on ecosystem (needs
  DOCS.md/icon/CHANGELOG polish — already on strategy P1/P2) widens distribution.

## 7. Phased plan

- **Phase 0 — this doc + the decisions in §8.** No code until the base OS and the
  first delivery vehicle are chosen.
- **Phase 1 — the fast "boom", minimal new code.** A scripted recipe that
  post-processes the pinned official HAOS image: inject the Wi-Fi keyfile + seed
  the curated securaCV backup. Turn "The Hub" page from `"real": false` simulation
  into a real, guided path (download → write with Imager/`dd` → the seed is already
  inside). Delivers the one-flash experience with **no new flasher engine yet**.
- **Phase 2 — native image-writer in the desktop flasher.** Add an SD/USB/NVMe
  writer to `desktop/` alongside the ESP path: enumerate **removable** disks with
  hard safety guards, decompress + write + verify, then write the boot-partition
  seed. Reuse `wifi-memory.js`; upgrade the persist store to the OS keychain (the
  desktop README already anticipates this). This is the real engineering lift.
- **Phase 3 — community + convergence.** Extract the SD-health add-on for upstream;
  submit the add-on repo to the community store; fold the hub image into the signed
  release train; converge with the §7.7 Canary onboarding (Improv / zeroconf) so
  hub and Canaries share one adoption flow.

## 8. Open decisions (product calls before we build)

1. **Base OS.** *Recommended:* HAOS-derived (post-process the official image) — we
   inherit self-heal / A-B OTA / years-of-life and don't own a rotting fork.
   *Alternative:* a fully custom securaCV image (more branding/control, but we then
   own the durability burden the user explicitly wants avoided).
2. **First delivery vehicle.** *Recommended:* the Phase-1 scripted/docs recipe on
   the official image — ships the "boom" fastest, no wrong-disk footgun yet.
   *Alternative:* go straight to the Phase-2 desktop image-writer engine.
3. **Pre-bake scope.** Minimal (Wi-Fi + Mosquitto + PWK add-on) vs the full
   opinionated stack (also Frigate / go2rtc + dashboards + blueprints). Minimal is
   the safe default; the stack is the more "magical" out-of-box.

## 9. Risks & anti-goals

- **Wrong-disk wipe** (Phase 2): removable-only enumeration, size/model
  confirmation, verify-after-write. Non-negotiable before shipping the writer.
- **Don't fork HAOS.** A bespoke image is the rot risk in disguise.
- **SD as boot media** is the weak link for multi-year uptime; steer to NVMe/SSD on
  Pi 5, keep the wear monitor loud.
- **Secure Boot / dm-verity parity** with the ESP32 eFuse story (Secure Boot v2 +
  flash encryption) is a *later, separate* concern — a Pi equivalent is TPM/OP-TEE
  + dm-verity, out of scope for the first cut.
- **Keep local-only custody** (Inv. IV): no cloud, nothing phones home, the image
  is a post-processed pinned official artifact.
