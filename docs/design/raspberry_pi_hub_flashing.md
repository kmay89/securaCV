# Design: the Raspberry Pi hub, flashed in one shot

**Status:** accepted — build in progress (§7 steps 1–5 implemented; hardware
validation of the write + first boot outstanding, Windows backend + full-stack
seed remaining) · **Date:** 2026-07-22 · **Owner:** TBD

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

## 7. Build plan (native image-writer, HAOS base, full stack)

The decisions in §8 are locked, so this is a concrete build order rather than a
menu. The destructive part (writing a raw disk) is sequenced the way the firmware
sequences anything that can hurt: **the pure decision layer lands and is tested
first, and the risky wiring is gated behind it** (cf.
`firmware/common/health/boot_policy.h`). "Native image-writer" does not mean we
reinvent an imager — the `flasher_experience.md` adjacent-bet is explicit that
the leverage is the *pre-baked HAOS image*, so the app writes **our seeded HAOS
image** using a proven write approach, in-app, so the operator never leaves for
Raspberry Pi Imager.

- **The catalog spine (landed).** `canary-local/devices/hub_image.json`, generated
  by `gen_hub_image.py` and drift-gated in CI, is the one source the writer, the
  "Hub" page, and the docs read — so they can't disagree about the image, the card
  floor, or what's baked in. Every fact is *derived*: the base OS version rides the
  Hub's upstream snapshot, the card floor is parsed from `hub_disk.rs`, and the
  full-stack payload is enumerated from the repo's own HA assets. It can't rot.
- **Step 1 — the target-disk safety layer (landed).** Pure, host-tested:
  `desktop/src-tauri/src/hub_disk.rs` decides what is a legal write target and
  refuses the system disk / internal fixed disks / too-small / unknown-size
  devices, with human-readable reasons and advisory warnings. No byte-writing code
  exists yet.
- **Step 2 — enumerate + confirm UI.**
  - *Core (landed):* `hub_enumerate.rs` — pure functions over `/proc/mounts` +
    `/sys/block` that map a device to its whole disk, find which disk backs `/`
    (or `/boot`) so it's flagged `system`, and detect external drives from the
    bus — host-tested, plus a thin read-only Linux `enumerate()`. Smoke-verified
    against real hardware: the root disk is correctly refused. The external call
    stays conservative: unproven ⇒ refused.
  - *macOS backend (landed):* `hub_enumerate_macos.rs` — a minimal,
    dependency-free plist reader over `diskutil list/info -plist`, with the APFS
    subtleties fixture-tested: the root's synthesized disk is followed through
    `APFSPhysicalStores` to the *physical* boot disk (refused as `system`),
    external means an explicit `Internal = false`, and container devices are
    never offered as raw targets. Host-tested like the Linux half.
  - *Command + picker (landed):* `list_hub_targets` (src-tauri/src/hub.rs)
    returns every disk with its verdict, and the app's Hub tab polls it live —
    a lone eligible card selects itself, warnings ride each row, and refused
    disks appear under "hidden — here's why" with hub-core's own reason. The
    write only arms after a typed ERASE.
  - *Remaining:* the Windows backend (PowerShell `Get-Disk`).
- **Step 3 — acquire the image.**
  - *Resolver + verify decision (landed):* `hub_core::hub_image` turns the
    catalog into a typed `WritePlan` (board → image URL, the card-size
    requirement), and `verify_download` makes the trust call — a repo-pinned hash
    is authoritative, else the download must match HA's published `.sha256`, else
    refuse; malformed hashes fail loudly. Pure and host-tested, so an
    unverifiable or wrong image can't be blessed for writing.
  - *Pin ceremony (landed):* `canary-local/tools/pin_hub_image.py` double-sources
    each board image's SHA-256 (HA's published `.sha256` **and** GitHub's asset
    digest, required to agree) into a committed pins file that `gen_hub_image.py`
    folds back into the catalog — `pinned: true` only while the pins name the
    current HAOS version, so a version bump honestly un-pins. The freshness
    workflow re-runs the ceremony weekly after the upstream snapshot refresh, and
    a separate `--verify` job alarms on link rot or a hash that moves under a
    pinned version. A pin never moves silently under the same version.
  - *I/O (landed):* `desktop/hub-io` (`fetch`/`xz`) streams the download to
    disk hashing as it arrives (no hash-after-download window), fetches HA's
    published `.sha256` for the unpinned fallback, and stream-decompresses the
    `.img.xz` hashing the raw bytes — the value the post-write read-back must
    reproduce. Host-tested against a loopback HTTP server and real xz fixtures;
    PR-gated by the Hub Core workflow alongside hub-core.
- **Step 4 — the guarded write (hardware-validated before merge).**
  - *Authorization gate (landed):* `hub_core::hub_flash::authorize_write` is the
    only way to obtain a `WriteAuthorization`, and it demands a `VerifiedImage`
    proof (returned by `verify_download`), a target that still `classify`s as
    eligible, and explicit confirmation. The write entry point takes a
    `WriteAuthorization` by value — so "write without verify + an eligible target
    + confirm" is a compile error, not a bug you can introduce. Pure + host-tested.
  - *Write + read-back (implemented; hardware validation OUTSTANDING):*
    `hub_io::write::write_image` takes the `WriteAuthorization` by value,
    streams 4 MiB chunks, then re-reads every byte off the device and demands
    the verified hash — a lying/counterfeit card fails at the desk. The
    device-agnostic core is host-tested (including a bit-flipping fake
    device); platform opens are Linux `O_EXCL` (with best-effort pre-unmount)
    and macOS `diskutil` unmount + raw `rdisk` via Apple's `authopen` (the
    Raspberry Pi Imager / Etcher path — the app never runs as root). **A real
    flash on real hardware must be validated before this ships in a tagged
    flasher release.**
- **Step 5 — seed the boot partition.**
  - *Wi-Fi keyfile generator (landed):* `hub_core::hub_seed::wifi_keyfile` builds
    the NetworkManager keyfile HAOS reads at first boot from the SSID +
    passphrase — SSID as a byte array (any characters survive), passphrase
    validated to WPA rules, escaping handled. Pure + host-tested. (Real-HAOS
    acceptance still needs a flash to confirm — the crate tests the generation,
    not the boot.)
  - *Seed drop (implemented; real-HAOS boot validation OUTSTANDING):*
    `hub_io::seed` mounts the freshly written boot partition (udisks /
    diskutil), writes `CONFIG/network/<id>` via the tested keyfile generator,
    syncs, and ejects — the Wi-Fi form in the app's Hub tab feeds it, values
    never logged. Whether the target HAOS build accepts the seed end-to-end
    still needs a physical first boot to confirm.
  - *Remaining:* upgrade the typed-once Wi-Fi persist store to the OS
    keychain, and seed the curated **full-stack** securaCV backup (Mosquitto +
    PWK add-on + Frigate/go2rtc + dashboards + blueprints) so first boot comes
    up pre-wired — until then the hub boots as stock HAOS + Wi-Fi and the
    guide carries the user from `homeassistant.local:8123`.
- **Step 6 — the card-reader-less path: flash the Pi through its own USB-C.**
  Accepted 2026-07-23. Many laptops (every recent MacBook) have no SD reader —
  but the Pi 5 doesn't need one: the BCM2712 boot ROM has a USB *device* boot
  mode (hold the power button while connecting USB-C to the computer → the Pi
  enumerates as `0a5c:2712 "BCM2712 Boot"`), and Raspberry Pi's official
  `usbboot`/`rpiboot` tool pushes their signed **mass-storage gadget** over
  that cable — the Pi then presents its own SD card *and NVMe* to the host as
  an ordinary USB disk (`RPi-MSD-…`). This is exactly how Raspberry Pi Imager
  flashes Compute Modules; the host's USB-C also powers the board during the
  flash, so the desk needs one cable, nothing else.

  The reason this slots in almost for free: once the gadget is running, the
  Pi-as-a-disk walks in through the pipeline's existing front door.
  `hub_enumerate` sees an external USB mass-storage disk, `hub_disk::classify`
  offers it (removable/external, size-checked — and it is the ONLY way our
  flow can reach the Pi 5's NVMe, the durable default, without an enclosure),
  and the verified write, read-back, and Wi-Fi seed run **unchanged**. The
  only new machinery is the on-ramp:

  - *Sidecar:* bundle `rpiboot` + the signed `mass-storage-gadget64` payload
    from `raspberrypi/usbboot` exactly like `espflash` (CI-fetched, pinned,
    checksummed; licenses vendored). No protocol re-implementation.
  - *Detection:* watch USB for `0a5c:2712` (Pi 5) / `0a5c:2711` (Pi 4) while
    the Hub tab is open; on sight, run the sidecar, then wait for the
    `RPi-MSD` disk to appear in the normal enumerator poll.
  - *UI copy:* "No card reader? Plug the Pi itself in: hold its power button,
    connect USB-C to this computer, release" → "Your Pi is now a disk — we're
    writing to the card inside it." When more than one MSD LUN appears (SD +
    NVMe), the existing picker already handles the choice, warnings and all.
  - *Hardware validation:* same bar as the write itself — a real Pi 5 over
    USB-C on macOS + Linux before any tagged release claims the path.

- **Step 7 — community + convergence.** Extract the SD-health add-on for upstream;
  submit the add-on repo to the community store; fold the hub image into the
  signed release train; converge with the §7.7 Canary onboarding (Improv /
  zeroconf) so hub and Canaries share one adoption flow.

> **CI note (resolved).** The Tauri app (`desktop/src-tauri`) only builds on
> release tags (`app-v*` / `flasher-v*`) and needs webkit/gtk, so it can't be
> tested in PR CI. The footgun-critical logic therefore lives in its own
> dependency-free crate, `desktop/hub-core` (`hub_disk` + `hub_enumerate`), which
> the `Hub Core` workflow `cargo test`s (+ `fmt`/`clippy -D warnings`) on every PR
> that touches it — so the disk-writer safety layer is verified continuously, not
> just at release. `src-tauri` will consume it via a path dependency when the
> first command that uses it lands.

## 8. Decisions (locked 2026-07-22)

1. **Base OS → build on Home Assistant OS.** Post-process the pinned official
   image; inherit self-heal / A-B rollback / auto-update from the HA project and
   let our add-on ride its update channel. We do not own a rotting fork. (The
   custom-image alternative was declined — it would put the "never rots" burden
   back on us.)
2. **First delivery vehicle → the native image-writer in the desktop flasher.**
   Straight to the in-app SD/USB/NVMe writer, not a scripted-recipe interim — the
   "flash it like a Canary" experience end to end. Accepts the bigger lift and the
   wrong-disk footgun, which §7 and §9 mitigate with the safety-first sequencing.
3. **Pre-bake scope → the full stack.** The flashed hub boots with Wi-Fi +
   Mosquitto + the PWK add-on **and** Frigate/go2rtc + the Lovelace dashboards +
   the alert/digest blueprints already wired — the most "magical" out-of-box. Cost:
   more surface to keep working across HA versions; the curated backup is
   version-pinned and drift-checked like the rest of the catalog.

Cross-reference: [`flasher_experience.md`](flasher_experience.md) §"Adjacent
bets" (the native-app / pre-baked-image framing) and §7.7 of
[`docs/strategy/11-home-assistant-platform-architecture.md`](../strategy/11-home-assistant-platform-architecture.md)
(the Canary onboarding this hub flow fronts).

## 9. Risks & anti-goals

- **Wrong-disk wipe** (§7 step 4): a legal target must be *external to this
  machine* — removable **or** enumerator-confirmed external, so the SSD/NVMe
  durable default still qualifies — never the system disk or an internal fixed
  disk, and at least the **32 GB supported minimum** (the hub hardware list);
  plus size/model confirmation and verify-after-write. These are the guarantees
  `hub_disk` now encodes and tests. The enumerator's external-vs-internal call
  must stay **conservative** — unsure ⇒ not external ⇒ refused — so a
  misclassification costs a card swap, never a wiped internal disk.
  Non-negotiable before shipping the writer.
- **Don't fork HAOS.** A bespoke image is the rot risk in disguise.
- **SD as boot media** is the weak link for multi-year uptime; steer to NVMe/SSD on
  Pi 5, keep the wear monitor loud.
- **Secure Boot / dm-verity parity** with the ESP32 eFuse story (Secure Boot v2 +
  flash encryption) is a *later, separate* concern — a Pi equivalent is TPM/OP-TEE
  + dm-verity, out of scope for the first cut.
- **Keep local-only custody** (Inv. IV): no cloud, nothing phones home, the image
  is a post-processed pinned official artifact.
