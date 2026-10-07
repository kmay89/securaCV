# A truly headless hub install — what the card can carry, and what it can't

**Status:** investigation, 2026-10-07. Two of the findings shipped in the same
change (the Flasher's first-boot watch, and the iPhone app's hub walkthrough);
the rest are options with an honest status each, and one recommendation.

**The question.** The Flasher writes Home Assistant OS to a card, puts the
Wi-Fi in the image, and the Pi boots on its own. Yet a person still met Home
Assistant's **setup wizard** — or, with the no-hands option on, a hub with
no SecuraCV integration. Why, and can the Pi finish *itself*, with no second
device at all?

---

## 1. What a card can carry before first boot

Home Assistant OS reads exactly one place a flasher can write from a Mac or
Linux desk: the **boot partition** (`hassos-boot`, FAT16, 64 MiB). Everything
else on the image — `hassos-overlay` (the `/etc` overlay), `hassos-data` (the
Supervisor, Core, every add-on, `.storage`) — is **ext4**, which no shipped
flasher writes without a Linux kernel or an ext4 writer of its own.

What HAOS imports from that FAT partition, as the Flasher already relies on
([`hub_seed.rs`](../desktop/hub-core/src/hub_seed.rs),
[`hub_headless.rs`](../desktop/hub-core/src/hub_headless.rs),
[`gen_hub_provision_bundle.py`](../canary-local/tools/gen_hub_provision_bundle.py)):

| Boot-partition item | What HAOS does with it | We use it for |
|---|---|---|
| `CONFIG/network/<id>` | NetworkManager keyfile, imported at start | the Wi-Fi seed |
| `authorized_keys` (root of the partition, also `CONFIG/authorized_keys`) | opens the developer console on port 22222 | the maintenance key the Flasher's companion uses |
| `CONFIG/udev/*.rules`, `CONFIG/modules/*` | udev rules and kernel-module config, imported | nothing (see option D) |
| anything else | ignored | the self-setup bundle rides here harmlessly |

**There is no first-boot script hook.** Nothing on the boot partition is
*executed*. That single fact is why every "finish the hub" story so far has
needed a second device on the LAN once the Pi is up: the desktop Flasher, and
now the phone.

## 2. Where the setup screen actually came from

Three gaps, found by reading the code rather than the docs:

1. **The first-boot watch fired too early.** `hub_probe_hub` counted *any*
   HTTP answer on `:8123` as "the hub is up". HAOS serves a "Preparing Home
   Assistant" landing page on that port for the minutes Core takes to
   download, so the account creation ran against a page that could not
   create one, retried for thirty seconds, gave up — and the self-setup run
   hit "Core isn't running yet" with no automatic retry. The person then
   opened the hub and met the wizard. **Fixed:** the probe now asks
   `GET /api/onboarding` and answers *offline / preparing / ready*
   ([`onboarding.rs`](../desktop/hub-io/src/onboarding.rs) `HubProbe`,
   tested); the watch acts only on *ready*, narrates *preparing*, and the
   self-setup run retries the two "too early" answers on its own.
2. **The account step needed the Flasher open.** The account seed on the
   card (`CONFIG/.storage/*`) is a head start HAOS has never been proven to
   import; the promise is kept by a companion on the LAN after boot. If that
   companion was closed, no account. **Fixed in kind:** the iPhone app now
   has the same companion ([`ios/Sources/SecuraCV/App/HubSetupRunner.swift`](../ios/Sources/SecuraCV/App/HubSetupRunner.swift)):
   it finds the hub over Bonjour (`_home-assistant._tcp`), waits for *ready*,
   creates the owner over Home Assistant's own onboarding API, finishes the
   wizard pages, verifies the login, and runs the provisioning plan through
   Home Assistant's Supervisor proxy. The phone is the device that is always
   on the home Wi-Fi.
3. **The self-setup bundle never installed the integration.** The plan
   installs Mosquitto, the MQTT entry, Frigate and the kernel *add-on*, but
   not `custom_components/securacv`, its config entry, the blueprints or the
   dashboards — only `scripts/install.sh` (the Terminal & SSH path) does
   those. A "finished" hub still needed HACS. **Not fixed here**; see
   option G.

## 3. The options for finishing with no second device

| | Option | Mechanism | Removes | Status |
|---|---|---|---|---|
| **A** | A second device finishes it | the Flasher (desktop) or the iPhone app, over the LAN after boot | the wizard, the add-on installs | **shipped** (this change); hardware validation per the runbook still owed |
| **B** | Pre-seed the data partition | write `.storage/auth`, `auth_provider.homeassistant`, `onboarding` into `hassos-data` before the write | the wizard only | candidate — the files are already minted ([`account.rs`](../desktop/hub-io/src/account.rs)); needs an ext4 writer in the Flasher |
| **C** | A first-boot unit via the overlay | drop a systemd unit into `hassos-overlay`'s `/etc/systemd/system/` that runs the bundle's `host_provision.sh` and posts `/api/onboarding/users` on `localhost` | everything | candidate — the only *truly* headless HAOS path; unsupported layout, ext4 writer needed |
| **D** | A udev rule as a hook | `CONFIG/udev/*.rules` with `RUN+="systemd-run …"` on the network device's add event | everything, in theory | not recommended — a supported import abused as a hook; timing and udev's sandbox make it fragile across HAOS versions |
| **E** | A curated backup, restored at onboarding | upload and restore a backup through Home Assistant's onboarding restore (Core ≥ 2025.2) before any account exists | the wizard and the installs | rejected before ([the design](design/raspberry_pi_hub_flashing.md)): a binary blob instead of a narrated, idempotent executor — and still a second device |
| **F** | A different base OS | Raspberry Pi OS Lite with Imager's `firstrun`/`custom.toml`, Home Assistant Container, Mosquitto and the kernel as containers | everything | rejected for the default hub — Home Assistant Supervised is no longer supported for new installs, Container has no add-ons, and the self-healing OS story is lost; the existing compose stack ([`integrations/ha_frigate_mqtt`](../integrations/ha_frigate_mqtt)) is the Docker-host escape hatch |
| **G** | The add-on as the provisioner | the kernel add-on's image carries `custom_components/securacv`; on start it places it under `/config/custom_components`, restarts Core once when the bytes changed (needs `hassio_role: manager`), and creates the config entry with the same `scripts/ha_onboard.py` the installer uses | the integration gap, on every path | **proposed** — a maintainer's call, because it grants the add-on a Supervisor role; the whole change is `Dockerfile` + `run.sh` + `config.yaml` |

### Why C is the one worth a spike

The overlay partition exists precisely so `/etc` changes survive OS updates,
and systemd is what boots HAOS. A unit with `ConditionPathExists=/mnt/boot/CONFIG/securacv/host_provision.sh`,
`After=network-online.target docker.service`, and a one-shot script that waits
for the `homeassistant` container, posts the owner account to
`http://127.0.0.1:8123/api/onboarding/users`, and then runs the bundle, would
make the card alone sufficient. Two costs, both real: it depends on an
internal layout Home Assistant does not document as stable, and it needs
the Flasher to write ext4 — on Linux `debugfs`/`mke2fs -d` exist, on macOS
nothing does, so this means a pure-Rust ext4 writer the size of
[`hub_fat.rs`](../desktop/hub-core/src/hub_fat.rs). B needs the same writer
for a smaller prize.

### Why A is enough for now

With the probe fixed and the phone able to finish, the window where a hub
sits onboarding-less is the time between first boot and the owner opening
either app — and the phone is in the pocket. The wizard is no longer the
default outcome; it is the fallback when nobody runs either companion, which
is also exactly what a person expects of a fresh Home Assistant.

## 4. Recommendation

1. **Ship A** (done): the three-state probe, the retries, self-setup on by
   default, and the phone walkthrough.
2. **Do G next.** It is small, closes gap 3 for every install path at once,
   and is the "pre-registered provisioner add-on" the bundle manifest already
   names as the candidate hook. The one decision is the role grant.
3. **Spike C** in the hardware-validation session
   ([runbook](hub_validation_runbook.md)): capture the overlay layout from a
   real hub, place a unit by hand from the developer console, reboot, and see
   whether the card alone finishes. Only if that works does an ext4 writer
   earn its place in the Flasher.
4. Leave B, D, E and F as recorded decisions.

## 5. What the hardware session has to prove

Added to the runbook's list, in the order they gate each other:

- ☐ `GET /api/onboarding` on a fresh HAOS first boot answers 200 with the
  step list only once Core is up — never from the landing page.
- ☐ The Flasher's watch shows *preparing* before *ready*, and the account
  lands with no wizard shown.
- ☐ Closed-Flasher case: the phone's walkthrough finds the hub over Bonjour,
  finishes the account, and the plan's add-on installs go through
  `/api/hassio/…` with the owner's session.
- ☐ `host_provision.sh` retried automatically after "Core isn't running yet"
  completes without a button press.
- ☐ Option C spike as above.
