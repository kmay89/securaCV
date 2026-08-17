# Design: the Raspberry Pi hub, flashed in one shot

**Status:** accepted — build in progress (§7 steps 1–5 implemented, disk
enumeration complete on Linux/macOS/Windows; hardware validation of the write +
first boot outstanding, full-stack seed remaining) · **Date:** 2026-07-24 ·
**Owner:** TBD

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
   This placement is HA-documented, not folklore: the OS configuration docs
   (developers.home-assistant.io/docs/operating-system/configuration) accept a
   USB stick named `CONFIG` and say *"Alternative you can create a `CONFIG`
   folder inside the `boot` partition"*, read on startup; files must be LF-only
   (ours are). The keyfile carries a flash-time-minted stable `uuid=` (HA's
   docs: without one "the IP address … change[s] on every boot") and
   `llmnr=2`/`mdns=2` on `[connection]` — the values HAOS's own default wired
   profile uses — so a Wi-Fi-only hub answers `homeassistant.local` at the OS
   resolver, keeping the no-monitor promise.
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
  - *Windows backend (landed):* `hub_enumerate_windows.rs` — a minimal,
    dependency-free JSON reader over `Get-Disk … | ConvertTo-Json`, with the
    Windows subtleties fixture-tested: the boot disk is flagged `system` from
    `IsBoot`/`IsSystem` **and** the disk hosting `$env:SystemDrive` (so a missing
    flag can't offer it), `USB` is the sole external bus (the same conservative
    stance as Linux/macOS), `SD`/`MMC` are the removable card buses, and the
    Windows-PowerShell-5.1 single-element-array collapse (a lone disk or drive
    letter arriving unwrapped) is tolerated. Host-tested like the other two.
  - *Command + picker (landed):* `list_hub_targets` (src-tauri/src/hub.rs)
    returns every disk with its verdict, and the app's Hub tab polls it live —
    a lone eligible card selects itself, warnings ride each row, and refused
    disks appear under "hidden — here's why" with hub-core's own reason. The
    write only arms after a typed ERASE. It calls `hub_enumerate::enumerate()`,
    which dispatches per-OS, so the Windows backend wired in with no app change.
  - *Step 2 is now complete across all three desktop OSes* — the last enumerate
    gap (Windows) is closed; what remains before the writer ships is the
    hardware validation of the destructive write itself (Step 4).
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
  - *Windows backend (staged behind the gate; VM/hardware validation
    OUTSTANDING):* `open_target` on Windows opens `\\.\PhysicalDriveN`
    (`CreateFileW`, whole-disk I/O via `FSCTL_ALLOW_EXTENDED_DASD_IO`) after
    **locking** (required — a lock it can't take is a hard error, since a
    dismount alone isn't exclusive) and dismounting every volume on that disk.
    The lock handles are owned by the returned device and dropped the instant
    the write ends — success, cancel, or verify failure — so the card is never
    left locked. Hand-rolled Win32 FFI, no `windows-sys` (the crate keeps its
    small dep set, as the macOS path hand-rolls `libc`). The device-agnostic
    core is unchanged: HAOS images are whole-sector, so the 4 MiB chunks and
    aligned tail satisfy raw-disk alignment. The pure `physical_drive_number`
    path parser is host-tested, and the FFI is cross-`cargo check`ed +
    `clippy -D warnings`ed for `x86_64-pc-windows-gnu` (it type-checks; only
    runtime behavior needs a machine). It is **kept disabled** —
    `hub_io::write::write_backend_available()` returns `false` on Windows, so
    the flasher fails fast *before any download* on an unsupported/unvalidated
    OS rather than dead-ending at the write. Enabling Windows is a one-line flip
    of that predicate once a VM pass (write a spare USB stick, confirm the
    read-back) proves it. Opening a physical drive to write needs
    Administrator, so a shipped Windows build will carry an elevation manifest.
- **Step 5 — seed the boot partition.**
  - *Wi-Fi keyfile generator (landed):* `hub_core::hub_seed::wifi_keyfile` builds
    the NetworkManager keyfile HAOS reads at first boot from the SSID +
    passphrase — SSID as a byte array (any characters survive), passphrase
    validated to WPA rules, escaping handled, LF-only as HA's docs require.
    The `[connection]` section carries `llmnr=2`/`mdns=2` (HAOS's own default
    profile values, so the hub answers `homeassistant.local` on Wi-Fi) and a
    stable `uuid=` minted at flash time by `hub_io::seed::uuid_v4` (HA's docs:
    without one the profile is re-imported with a fresh identity — and possibly
    a fresh IP — every boot). Pure + host-tested. (Real-HAOS acceptance still
    needs a flash to confirm — the crate tests the generation, not the boot.)
  - *Seed drop (implemented; real-HAOS boot validation OUTSTANDING):*
    `hub_io::seed` mounts the freshly written boot partition (udisks /
    diskutil), writes `CONFIG/network/<id>` via the tested keyfile generator,
    syncs, and ejects — the Wi-Fi form in the app's Hub tab feeds it, values
    never logged. Whether the target HAOS build accepts the seed end-to-end
    still needs a physical first boot to confirm. `mount_partition` is Linux +
    macOS only, so the **Windows seed-mount is the next Windows piece** after
    the writer: until it lands a Windows flash produces a bootable card without
    the Wi-Fi/account pre-seed (a seed stumble is already non-fatal — the
    verified write is never demoted), and the guide carries the user the last
    step from `homeassistant.local:8123`.
  - *Full-stack payload recipe (Frigate config landed 2026-07-25):* the curated
    Frigate config the seed will carry is committed at
    `homeassistant/frigate/config.yaml` (go2rtc built in, MQTT→Mosquitto,
    person/car/dog/cat to match the kernel labels, recordings OFF for SD
    endurance) and enumerated + drift-gated into `hub_image.json`
    (`payload.frigate_config`). This also fixed two catalog bugs: the Frigate
    add-on slug is `ccab4aaf_frigate` (a third-party store add-on whose
    repository must be added), not `frigate`, and go2rtc is **not** a separate
    add-on (it's built into Frigate), so it's no longer listed as one. Because
    securaCV consumes Frigate over MQTT, the same recipe serves a Pi-with-Coral
    or a dedicated NVIDIA Jetson detector — see
    [`integrations/jetson-detector/`](../../integrations/jetson-detector/README.md).
  - *Provisioning plan (landed 2026-07-25):* `gen_hub_seed.py` →
    `canary-local/devices/hub_seed.json` is the ordered first-boot sequence —
    register the add-on repositories (the step the `ha` CLI **cannot** do, so it
    names the Supervisor API `POST /store/repositories` that can), install the
    broker, install Frigate, write its config to the add-on's *own* config dir,
    then start the witness kernel. Every step carries `why` + `for_what` in plain
    language so an installer can narrate what it's doing to someone's home.
    Derived from `hub_image.json` (slugs/repositories) + the curated Frigate
    config and drift-gated, so a slug fixed in the catalog can't stay wrong here;
    an add-on that vanishes fails the generator loudly rather than emitting a
    wrong plan (both behaviors verified).
  - *Provisioning executor (landed 2026-07-25):*
    [`canary-local/tools/hub_seed_apply.py`](../../canary-local/tools/hub_seed_apply.py)
    is the piece that *runs* the plan, replacing `install.sh`'s punt (the `ha`
    CLI can't register a repository, so the old path degrades to "click through
    the store UI"). It drives the Supervisor REST API and is split the same way
    the flasher is: a **pure** `plan_actions(seed, observed)` that decides the
    ordered, idempotent action list, and a thin `SupervisorClient` that is the
    only thing touching the network — so the whole planner is host-tested with no
    Home Assistant (18 cases). It resolves each add-on's **Supervisor slug** from
    the plan (a third-party add-on answers to `<repo_hash>_<slug>`, so securaCV is
    `d0491a67_privacy_witness_kernel`, not the bare name — install-by-API 404s
    otherwise; the plan now carries this, computed once in `gen_hub_seed.py`).
    `--dry-run` narrates the full sequence — every `why`/`for_what` verbatim from
    the plan, plus the exact API call — needing no hub, and never overwrites an
    existing Frigate config (either `config.yml` or `config.yaml`, since Frigate
    accepts both). It also sets the kernel's `mode: frigate` option **before**
    starting it — the add-on's factory default is `standalone`, which serves only
    a setup wizard, so without that the "unattended" install would come up
    producing no claims. Real runs fail **closed**: any error stops with a "safe
    to re-run, it's idempotent" note rather than leaving a half-built hub.
  - *Provisioning bundle — the assembler's content half (landed 2026-07-26):*
    [`gen_hub_provision_bundle.py`](../../canary-local/tools/gen_hub_provision_bundle.py)
    assembles the self-contained payload the card will carry — the plan, the
    curated Frigate config, the executor, and a one-line `provision.sh` runner —
    and emits a drift-gated manifest
    ([`hub_provision_bundle.json`](../../canary-local/devices/hub_provision_bundle.json))
    that **pins each file's SHA-256**, so the bundle can't silently ship stale
    code. A built bundle runs its own `sh provision.sh --dry-run` with no repo and
    no hub — host-tested for self-containment. This deliberately supersedes the
    early "curated HA *backup*" idea: a narrated, idempotent, auditable executor is
    a better fit than an opaque blob for a stack whose whole point is "understand
    *why* it's configuring each thing". Its landing spot is `CONFIG/securacv/` on
    the boot partition, written by the same `hub_io::seed` mechanism as the Wi-Fi
    keyfile.
  - *Boot-partition write side (landed 2026-07-26):* `hub_io::provision` turns the
    bundle into the `SeedFile`s the existing guarded writer drops under
    `CONFIG/securacv/`. The payload is embedded VERBATIM from the repo
    (`include_str!` of the plan, Frigate config, and executor) and the runner is
    reproduced in Rust — and a **host cross-check hashes every shipped byte against
    the Python manifest's SHA-256 pins**, so the flasher's Rust and the generator's
    Python can't drift apart (the "Hub Core" workflow re-runs it whenever an
    embedded file changes). Same posture as the rest of hub-io: the layout is
    host-tested, the physical boot is not. It carries the runnable payload plus the
    integrity manifest — the same set the generator's `--build` produces; the
    self-documenting `provision.sh` means there's no separate README to drift.
  - *App opt-in + companion-run provisioning (landed 2026-08-01; real-HAOS boot
    validation OUTSTANDING):* the Hub tab's "Let this app finish hub setup by
    itself" toggle seeds two things through the same guarded injection as the
    Wi-Fi keyfile: the provisioning bundle under `CONFIG/securacv/` (now carrying
    `host_provision.sh`, a wrapper that borrows python3 + the Supervisor token
    from the running Core container, so the bundle is runnable from the ONE
    shell a fresh headless hub exposes) and a **maintenance key** as
    `authorized_keys` at the boot-partition ROOT — HAOS's documented
    developer-console mechanism (port 22222). The first-boot companion then
    resolves the "who runs the bundle" question from the operator's computer:
    when the hub answers, it connects over SSH (key minted locally by
    `ssh-keygen`, private half never leaves the operator's machine; TOFU into a
    dedicated `known_hosts` — and a host key that CHANGES stops the run with an
    explanation rather than re-pinning itself, because a reflash and an
    impostor on the same address produce the identical signal) and runs
    `sh /mnt/boot/CONFIG/securacv/host_provision.sh`, streaming
    the executor's narration into the app's console. The decisions (key
    validation, host validation, the exact ssh argv, reflash detection) are pure
    and host-tested in `hub_core::hub_headless`; both seeds are non-fatal notes
    on a stumble, like the account seed. A quit mid-first-boot resumes: the
    "last flash" record remembers an owed setup run.
  - *Hub display — the plan's second opt-in extra (landed 2026-08-17; first
    on-hardware run OUTSTANDING, like self-setup itself):* the hub stays
    headless by default and the no-monitor promise stays intact — but a hub
    with an HDMI touchscreen plugged in (reference panel: 7" 1024x600 IPS,
    USB touch) can now opt into a dashboard on that screen. Rides the exact
    mechanism Pi-hole proved: a feature-tagged step in the generated plan
    (`--with display`), zero footprint unless enabled, surfaced as a checkbox
    in both the desktop Flasher's self-setup panel and the browser guide. The
    step registers the HAOSKiosk community add-on's repository and installs it
    — an X server + browser running ON the HAOS host, showing Home Assistant
    full-screen with touch mapped to the panel — and deliberately does NOT
    start it: the kiosk signs in as the operator's own Home Assistant user, a
    credential the plan never mints or carries, so the last move (typing the
    login into the add-on's Configuration tab and pressing Start) is the
    user's, and the plan's `user_must_finish` says exactly that. With no
    screen attached the add-on simply refuses to start and nothing else on
    the hub is affected. The kiosk's login is a **dedicated non-admin HA
    user** by instruction (the add-on keeps that password in its options, and
    a screen only views dashboards — same reasoning as the broker's `canary`
    account). Trust posture, said out loud: like Frigate and Pi-hole, the
    add-on's repository is registered **unpinned** — the Supervisor's store
    model tracks upstream releases and offers no ref-pinning — so the plan
    and docs state it as the community add-on it is; vendoring it into this
    repo's own add-on repository is the available hardening step if the
    feature graduates from experimental. Validation home:
    [`hub_validation_runbook.md`](../hub_validation_runbook.md) §5 opt-in
    extras.
  - *Remaining:* upgrade the typed-once Wi-Fi persist store to the OS keychain,
    and — the genuinely unresolved, on-hardware-pinned piece — a **true
    zero-touch first-boot hook** (the hub running the bundle with no companion
    at all). HAOS has no supported hook to run an arbitrary boot-partition
    script, so the companion-over-SSH path above is the honest interim; it and
    the seeds still need their first validated run on a real Pi (does this HAOS
    build import the root-partition `authorized_keys`, does the console open,
    does `host_provision.sh` complete?). HAOS ignores files it doesn't
    recognize, so an un-run bundle is harmless and the guide still carries the
    user from `homeassistant.local:8123`.
  - *Account — the onboarding-API companion (IMPLEMENTED 2026-08-05; this is
    the promise):* the account story no longer rests on the unvalidated card
    seed. Once the first-boot watch sees the hub answer, the app calls **Home
    Assistant's own onboarding REST API** (`hub_io::onboarding`, host-tested
    against a scripted loopback HA): read `GET /api/onboarding` for the hub's
    real state, create the owner via `POST /api/onboarding/users` if the slot
    is open, finish the remaining wizard pages (`core_config`, `analytics`,
    `integration`) with an ephemeral token that is revoked on the way out, and
    **verify the typed login opens the hub** via the login flow. It converges
    from any starting state — seed applied, seed ignored, wizard half-clicked
    in a browser, a previous run dead partway — so retrying is always safe,
    and it never claims success it didn't check. The password lives only in
    the frontend's memory for this (a relaunch loses it, and the resume copy
    says so honestly); it is sent only to the operator's own hub on their own
    network.
  - *Account pre-seed (minting + opt-in seed IMPLEMENTED 2026-07-23; now a
    HEAD START, not the promise):* the flasher collects the operator's
    name/username/password alongside the Wi-Fi, mints Home Assistant's auth
    store locally (`.storage/auth` + `.storage/auth_provider.homeassistant`,
    bcrypt-hashed ON THE OPERATOR'S computer — the password gets the same
    custody as the Wi-Fi secret: onto the card, never logged, never sent), and
    `hub_io::seed::seed_card` writes it under the boot partition's `CONFIG/`
    (Mechanism B) in the SAME mount as the Wi-Fi keyfile. A seed failure is
    non-fatal — the verified card is never demoted. Whether HAOS imports it is
    still unconfirmed on hardware; if it does, the companion above simply
    finds onboarding done and verifies the login. Zero-touch restore
    mechanisms still worth confirming on hardware, in order of preference:
      1. **data-partition injection at flash time** — write the backup (or the
         pre-expanded `.storage` + add-on containers, which also collapses the
         10–20 min first boot toward ~2–3 min) into the image's ext4 data
         partition before/while writing the card. Trivial from Linux; macOS
         needs an ext4-write story, so this may start Linux-only;
      2. **boot-partition CONFIG import** — if current HAOS's `hassos-config`
         can be made to carry the backup in from FAT, it works from every OS;
      3. **onboarding "restore backup"** — the fallback: still zero typing,
         one click on first visit.
    The validation session picks the mechanism; the auth-store minting and
    backup assembly land in hub-core/hub-io (pure, host-tested) either way.
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

  - *Sidecar (implemented):* release CI builds `rpiboot` and vendors the
    `mass-storage-gadget64` payload from one `raspberrypi/usbboot` checkout
    (`USBBOOT_REF`, honest-before-pin: tracks upstream until the validation
    session pins the exact tag it proved; every build logs the resolved SHA).
    macOS bundles its own libusb (re-pointed into Resources) so nothing
    depends on a user's Homebrew; the deb declares `libusb-1.0-0`. Both the
    sidecar and that libusb are built **once per architecture** and `lipo`'d,
    because the app ships as one universal download: an arm64-only helper
    fails on Intel Macs with `Bad CPU type in executable`, which is precisely
    what shipped in 0.3.9 and is fixed in 0.3.10. The build asserts both
    slices are present rather than printing them (RELEASE_LESSONS 2026-08-02).
  - *On-ramp (implemented):* `hub_pi_boot_start`/`stop` commands spawn the
    sidecar with the bundled gadget — rpiboot itself does the waiting, so no
    separate USB watcher is needed; its narration streams to the UI and the
    Pi-as-disk arrives through the ordinary target poll, badged "your Pi,
    over USB-C" (`RPi-MSD…`). A build without the payload fails the panel
    with a clear message and every other flow still works.
  - *UI copy (implemented):* "No card reader? Use the Pi itself over USB-C" —
    hold the power button, connect the cable, release; the cable powers the
    board. When more than one LUN appears (SD + NVMe), the existing picker
    handles the choice, warnings and all.
  - *Hardware validation (OUTSTANDING):* same bar as the write itself — a
    real Pi 5 over USB-C on macOS + Linux, then pin `USBBOOT_REF`, before any
    tagged release claims the path.

- **The experience layer (landed 2026-07-23).** Production-hardening so the
  flow is worth running unattended: a cooperative cancel through every chunk
  loop (stopping is always hardware-safe); a verified write is never demoted by
  a seed stumble (it becomes a note + plan B); one automatic download retry;
  an honest indeterminate bar with a per-stage ETA; a local **image cache**
  (re-verified on reuse, so a second flash skips the download, never the
  trust check); a **first-boot companion** that polls `homeassistant.local`
  and flips to "It's alive!" with an OS notification, chime, and a QR to open
  the hub from a phone; remembered non-secret fields; a free-space preflight;
  and calm, humble error copy that always names the next step. All host-tested
  or additive; none touches the safety chain.
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
