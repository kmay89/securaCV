# Changelog

## [Unreleased]

## [2.4.9] - 2026-08-09

### Every display draws itself, names itself, and the 7" brightness works

Four things an owner could see on the Fleet tab. Three were firmware-side,
and each was the same shape: a capability that worked on one side of the wire
with no path to the client.

- **Displays publish what they are, and which board they are.** A display
  self-reported the family string `canary-display`, which the figure ledger
  leaves unmapped on purpose — four products wear it, so a picture chosen
  from it would be a coin flip. Every display therefore drew a generic marker
  while the ledger held a drawing of it. Each flavor now reports its real
  device type, plus `hw`: the `boards/<id>/pins` header it compiled against.
  That is the only value on the wire that is exact about the SHAPE, because a
  build compiles against exactly one pins header and the wrong one is a dead
  device. It names the board, never the product — one board can serve two
  products (the 7" glass is both the Dash 7 and the Nightstand 7), so a
  reader draws from it and takes the name from elsewhere.
- **The 7" brightness control does something.** On the 4.3" and 7" panels the
  backlight is a CH422G expander line, which is binary in hardware — so
  `day_pct` scaled a value that can only ever be on or off, and the slider on
  the phone and the one on the device's own web page both did nothing you
  could see. Those boards dim by drawing a scrim, and that knob (`bright_pct`)
  was reachable only from the on-glass menu. It is now served by
  `/api/settings`, accepted by `/api/set`, and its presence is how a client
  knows which of the two brightness controls this glass can honor.
- **A device id per unit, not per model.** `CD_DEVICE_ID` is a compile-time
  constant seeded into NVS on first boot, so every Canary Nightstand ever
  flashed answered to `canary_nightstand_001` — one name, one set of MQTT
  topics, and no way to tell two of them apart. First boot now seeds the
  salted per-unit suffix the mDNS *hostname* has always carried, and a unit
  still holding the bare model default migrates once. A device id somebody
  chose is never touched.
- **A display says where it stands with its hub.** `/api/fleet` carries
  `hub`, in three states rather than a flag: no broker configured, one
  configured and unreachable, or connected. "Nobody has given me a hub" and
  "mine is down" are different problems with different fixes, and a bool
  would have sent an owner to restart a hub they never set up.

## [2.4.8] - 2026-08-07

### A Canary can say when it was born, and you can pick the color it glows

Two things the hardware could nearly do and the app could not reach.

- **A born-on date the device stands behind.** The certificate card could
  only say when *this phone* paired, which is a fact about the phone — two
  people looking at the same Canary saw two different "born" dates, and one
  re-paired after a phone restore looked newborn. A Canary's identity is its
  keypair, so its age is its key's age; but the key is generated before any
  clock exists (no battery-backed RTC — the chip boots at the epoch and
  learns the date from GPS later). `common/identity/birth_day.h` decides
  under three rules, each named for the lie it prevents: **written once**
  (a device that can restate its birth day launders its age), **only a
  believable clock** (below the floor is the boot epoch showing through),
  and **exact only when it's exact** — a Canary flashed in a workshop and
  plugged in a week later records the day it was *first dated* and says so.
  A day, never a timestamp: coarser than Invariant III's buckets, and by
  construction it cannot carry a time of day. `/api/fleet` carries
  `born_day`/`born_exact`, omitted entirely until real, so a display with no
  key of its own reports none rather than 1970.
- **A color the glass can actually honor.** The look engine spoke HSV but
  had no way to be told a hue outside its nine curated scenes, so a color
  wheel would have been the app promising more than the device could do.
  `LookParams.custom_hue` and `custom_scene()` build four stops around the
  chosen hue with the same gentle drift the curated scenes use, so a picked
  color still breathes instead of sitting flat. Every renderer — the WS2812
  beacon, the glass wash, and the plumage note overlay — now routes through
  one public `current_look()`, so nobody indexes the scene catalog directly
  and the point of light and the pane can never disagree. **The honesty rule
  is untouched and now tested against the new path:** at Warn and above the
  semantic override still wins, so no hue anyone picks can dress an alarm in
  a friendly color, and safe dark still outranks everything.
- **Every display setting reachable from the app.** `/api/settings` and
  `/api/set` are served by every display, but the app only ever spoke to them
  through the nightlight's lamp keys — so a Watch Station or a Dash served a
  screen brightness, a night window, a red shift and a peek duration that
  nothing in the app could touch. The app now renders whatever the glass
  *reports*, so a display without a lamp offers no lamp controls and firmware
  that grows a capability appears on the phone with no App Store update.
- Two API keys that existed everywhere but the web: `lamp_hue` (−1 = a
  catalog scene is on) and `lamp_minutes`. The lamp's floor is one minute,
  not zero — `LanternModel` clamps anything under a minute up to one, so a
  "0 = stays on" would have promised an untimed lamp and delivered a
  60-second one.

## [2.4.7] - 2026-08-07

### The C6 nightstand fits its flash again — and an unbootable image can no longer ship

fw-v2.4.6's `canary-display-nightstand-c6` factory image carried an app
5 KB bigger than the OTA slot it boots from; a freshly flashed board
boot-looped on `Image length 1971152 doesn't fit in partition length
1966080`. Fixed, and gated so it stays fixed:

- **A partition table sized for the board.** The 4 MB C6 nightstand (and the
  C3 nightlight, which shares its budget) move from the stock `min_spiffs.csv`
  to `partitions_display_4mb.csv`: the 128 KB spiffs region these
  NVS-only devices never used is folded into the A/B slots, growing each
  from 0x1E0000 to 0x1F0000 — the 2.4.6 image fits with ~60 KB to spare.
  **Already-fielded nightstand-c6 boards need one USB re-flash** (the
  Flasher's normal install) to pick up the new table; OTA alone cannot
  deliver it.
- **Byte-accurate size gates where the old ones had holes.** The build passed
  because PlatformIO's `checkprogsize` measures the ELF, not the final
  `.bin`, and the flavor's size guard only watched the S3 watch build.
  `flavors.json`'s `size_guard` is now `size_guards` — a list, one entry per
  slot budget — and the C6/C3 bins are guarded by their real slot sizes.
  The release workflow enforces the same budgets on the exact bytes it
  signs (`check_slot_budget.py`): fatal for the flagship canary/wap
  manifests, a per-variant skip for the vision/sense/display loops — so a
  tag or manual dispatch can no longer publish an OTA image no fielded
  slot can hold.
- **`make_factory.py` refuses to build a brick.** It always read the app
  offset out of the partition table; now it reads the size beside it and
  fails the merge if the app cannot boot from that slot — a per-variant
  skip at release time instead of a published boot loop.
- The full story, including why the C3's guard deliberately stays at the old
  slot size, is in `.github/RELEASE_LESSONS.md` (2026-08-07) and
  `firmware/PARTITIONS.md`.

### The Nightlight turns with the room

Stand the Canary Nightlight on any of its four edges and the clock rights
itself — and the canary **tumbles in from the edge that was up**, bounces
on its perch, and settles, while the clock breathes back in behind it.

- **Real movement only.** The QMI8658 feeds a gravity-settled model
  (host-tested): a flip commits only when the device has come to REST in
  a new orientation — a shake, a carry, a bump, a flat lay-down, or a
  diagonal hold carries no opinion, and a cooldown means a wobbling hand
  can never double-flip it. Turning the lamp feels immediate because the
  commit lands the moment it settles.
- **All four orientations.** Portrait, upside down, and both landscapes —
  the panel rotates in hardware (one MADCTL write; the ST7789's centered
  window makes the geometry exact), and the face recomposes: landscape
  gets a wide clock with the companion perched beside it.
- **A hand on the dial parks AUTO.** Triple-press the BOOT button to turn
  the face by hand (the opt-in third gesture — the lantern's double-press
  timing is unchanged on every other flavor), or pick an orientation in
  the app; either quiets the IMU until the app's "Turn with the room"
  toggle brings it back.
- The orientation persists across reboots, and the app's Nightlight card
  gains the toggle + picker.
- **Honest status:** the board's IMU mounting is unpublished, so the
  accelerometer axis map ships as a documented best guess pending a
  bench check on real glass (`pins.h` `IMU_AXES_SWAP_XY` /
  `IMU_X_SIGN` / `IMU_Y_SIGN` — one line each if it turns the wrong
  way). The model, the gates, and the manual triple-press/app path are
  host-tested and hold regardless of the map.

### The hub finishes its own sign-in, and the Flasher stops lying about USB

Two Flasher (desktop app) fixes, one promise made real:

- **The hub account is now created over Home Assistant's own setup API.** The
  opt-in account panel used to rest entirely on the experimental `.storage`
  card seed — unvalidated on hardware, so first boot could land on a setup
  wizard holding credentials HA had never heard of. Now the first-boot watch
  finishes onboarding the moment the hub answers: it reads the hub's real
  state (`GET /api/onboarding`), creates the owner if the slot is open,
  completes the remaining wizard pages, revokes the ephemeral token it used,
  and **verifies the typed login actually opens the hub** before saying so.
  It converges from any starting state — seed applied, seed ignored, wizard
  half-clicked in a browser, a previous run dead partway — and reports
  honestly when a human still has a click left (including an owner account it
  didn't create). New host-tested `hub_io::onboarding` module; the card seed
  stays as a harmless head start.
- **The USB "Connected" dot can no longer outlive the board.** After a flash
  the serial monitor auto-starts, and starting it froze the 1 Hz port watcher
  — so unplugging the Canary left "Connected · ESP32-S3" on screen forever.
  The watcher now keeps enumerating (a read-only OS query) while the monitor
  owns the port: a rebooting board shows "waiting for it to come back", a
  really-gone board drops to "Scanning for a Canary" within seconds. The
  in-browser flasher gets the same honesty via Web Serial's `disconnect`
  event — unplugging while parked on the connected card returns to the
  connect step instead of showing a chip that isn't there.

### A display hears what the camera sees — no broker, no hub, no setup

A Canary Vision and a Canary Display now speak detection directly over
Bluetooth LE. Power both on, and the person your camera sees shows up on the
glass — as `person 87% (ble)` with an amber glow and a chime — with no MQTT
broker, no Raspberry Pi hub, no WiFi, and no pairing step.

- **Fleet beacon v2.** The 11-byte fleet-link presence beacon gains a 13-byte
  version 2: the same layout plus a detection **class token** (person /
  vehicle / animal / package — the ObjectClass vocabulary, deliberately
  nothing identifying) and a **confidence percentage**. No identity, no
  timestamp on the wire; the advert is the "now". v1-only senders
  (canary-sense, the WAP) stay understood unchanged everywhere.
- **canary-vision sets the alert it always had.** The beacon's `alert` flag
  existed from day one and nothing ever set it. Now the presence FSM drives
  it — debounced presence, not per-frame flicker — and a detection edge
  republishes the advert immediately instead of waiting out the 5 s refresh.
- **canary-display raises a real attention event.** The scanner (BLE and its
  ESP-NOW twin) decodes the class + confidence into the event line and
  timeline with `Sev::Warn`, a per-(witness, class) 60 s edge-dedupe so the
  continuous advert can't spam the log or re-cancel acks, and tamper keeping
  precedence. Unsigned like everything on this channel: it draws attention
  but never touches the `Verified` badge. An unknown Vision still auto-appears
  as `SCV-XXXX` — discovery was already magical; now it carries the sighting.
- **Sibling rosters hear it too.** canary-sense / canary-vision roster scans,
  the modular canary's scan feed, and the WAP's `/api/nearby` all accept the
  v2 length, so a detection-flagged peer lands in every fleet view.

### The same beacon, now over WiFi — still no broker, and now across a house

Bluetooth reaches as far as Bluetooth reaches. A Vision in the driveway and a
Display upstairs were both sitting on the home WiFi and still could not hear
each other without an MQTT broker in the middle. Now the presence beacon rides
that WiFi too, as a UDP multicast datagram — **no broker, no hub, no pairing,
no configuration**. A datagram addressed to a group needs nothing discovered or
logged into: a Vision joined to WiFi starts sending, a Display on the same WiFi
starts hearing.

- **One beacon, three bands, no duplication.** The datagram body is the *same*
  manufacturer blob the BLE advert carries — no UDP framing — so BLE, ESP-NOW
  and LAN multicast all decode through one parser into one ingest. On the
  sending side the bytes are built once and every carrier transmits that buffer
  verbatim, so the bands physically cannot drift into dialects and a new field
  reaches all of them the day it lands.
- **A band adds coverage, never a duplicate.** A witness is keyed on its
  fingerprint suffix, never on the radio that carried it, and every dedupe
  window is band-independent. One Canary heard on BLE *and* WiFi is one device
  and one alert. That included fixing a real latent bug: the tamper dedupe
  compared the event *label*, so the moment labels named their band the same
  tamper would have alarmed once per radio.
- **It heals because there is nothing to reset.** Sockets follow the STA
  address and are reopened when it changes; the link going down drops them. A
  router reboot, a new DHCP lease, a move to another AP all recover on their
  own, and a band that goes quiet simply ages out of "currently carrying" and
  re-credits itself by being heard again — no flag anywhere can outlive the
  interface it described.
- **TTL 1, set rather than inherited.** A presence beacon must not be routable
  off the local subnet; that is a privacy property, so it is asserted in the
  host test rather than left to a stack default. Nothing rides this wire that
  the BLE advert doesn't already broadcast in the clear — flags, coarse
  percentages, a chain height, two fingerprint bytes, and a class token plus
  confidence. No identity, no image, no audio, and unlike a broker there is no
  third party that receives, stores or forwards any of it.
- **The glass names the band honestly.** `person 87% (wifi)` when it crossed
  the LAN, `(ble)` over Bluetooth, `(mesh)` over ESP-NOW — which also fixes
  ESP-NOW frames having always reported themselves as `(ble)`.

Both bands are on by default (`FEATURE_FLEET_UDP`), and either can be vetoed
per board by a size guard without touching a call site.

### A Canary that drops off WiFi can get back on

Field report from an ESP32-C3 Vision: flashed with good credentials, it would
not rejoin its network. The fleet roster's BLE scan went **continuous** the
moment the STA link was down — but BLE and WiFi share one 2.4 GHz radio on the
C3/C6, and `wifi_loop()` is retrying that entire time, so the scan starved the
association it was waiting for. Miss the boot-join timeout once and the device
could stay locked out, each retry landing in a radio the scanner never
released.

Continuous scanning now requires that there be **no join to protect**: only an
unprovisioned unit (nothing to associate with, so the beacon really is the last
channel) scans continuously. A provisioned unit that is merely offline keeps
its low-duty 3 s / 60 s bursts and leaves the radio free to rejoin. The
presence beacon itself is unchanged, so broker-free discovery — including the
new detection alerts above — keeps working exactly as before.
## [2.4.6] - 2026-08-05

### The Canary Nightlight — a kid's bedside clock with a friend living in it

The first firmware for the Waveshare **ESP32-C3-LCD-1.47** (the pocket-case
board): a new `nightlight` flavor of the display family, made to be flashed,
joined to WiFi on the glass, and tuned from the iPhone app with zero other
tools.

- **A 7-segment clock over a lamp** — 12-hour by default, ghost segments by
  day, still digits at night, and a look-engine lamp wash behind it: the warm
  Lantern orange it ships with, the full Rainbow sweep, the new **Moonbeam**
  bright-white scene, and the rest of the ring (BOOT double-press, a tap
  while lit, or the app walks it).
- **A companion on your rhythm** — the living canary visits the stage a few
  seconds at a time: up with you in the morning, a little song at midday,
  winding down in the evening, asleep beside the clock after bedtime. Visits
  are staging, never a face — the bird's expression stays the mood engine's,
  and any real attention takes the stage back instantly.
- **A lamp that never lies** — the nightlight never renders safety as light:
  the lamp is decor, the clock is information, and the glance line carries
  link/clock honesty in words. The lamp burns through quiet hours by default
  (that is the product), and the attention veto still puts it out the moment
  anything is wrong.
- **A heat budget you can't exceed** — backlight duty is hard-capped at 50%
  in the HAL, at this board's I2C expander PWM register, underneath every
  settings path. Closed PETG pocket case; a can't, not a won't.
- **Standalone honesty for the whole bedside family** — a never-configured
  hub now reads as *standalone*, not link trouble: no permanently worried
  bird, no lamp refusing to light, on any fleet-less bedside glass.
- The iPhone app shows it as a **Nightlight** with a native settings card
  (lamp color from the device's own scene list, strength, night hours,
  12-hour clock); both flashers carry the product on the release and dev
  channels; OTA product `securacv-canary-display-nightlight-c3`.

Pin map from Waveshare's own engineering-sample repo + schematic (they
agree): ST7789T at 180×320/offset-30, CS/RST/backlight behind the EXIO
expander. Compile-tested; bench-verify colors and panel edges on first boot
(the board README documents both checks).

## [2.4.5] - 2026-08-03

### The setup wizard stops crashing — for the reasons 2.4.4 didn't reach

2.4.4's four first-boot fixes were real but not the whole story: a Nightstand 7
still looped clock → "Let's get you connected" → reset, so wizard-entered
Wi-Fi could never persist (credentials write only on a successful join). Five
further hardenings close the wizard-start window the crash brackets (PR
pending):

- **The SoftAP never rises mid-scan.** The wizard's pre-AP sweep (~3.9 s for a
  full 13-channel active scan) outlives the 2.6 s Hello beat, so `WIFI_AP_STA`
  + `softAP()` fired with a live scan handle — the one radio sequence the
  battle-tested WAP wizard explicitly refuses ("a pending scan handle keeps
  the radio busy"). The wizard now waits the sweep out under the Hello scene
  (bounded), harvests it, and drops any lingering handle before the mode flip.
- **LVGL 9 leaves the fixed 64 KiB pool for the heap.** `LV_USE_STDLIB_MALLOC`
  (a v9-only key; v8 ignores it) routes `lv_malloc` to the C library. Under v9
  the pool also fed draw tasks, layers, and the QR canvas; its worst moment was
  the wizard's two-live-screens window, and its failure mode was
  `LV_ASSERT_MALLOC`'s `while(1)` — a halt the armed task watchdog converts
  into a panic loop. The ELF confirms the builtin pool is gone.
- **The join QR proves it rendered before the card shows.** The v9 widget can
  leave a buffer-less canvas on allocation failure, and update reports its own
  verdict; both are checked now. On failure the Join scene degrades to plain
  text join instructions — no blank card, no dead end, same destination.
- **First-boot NVS writes moved off the live panel.** The runtime config's
  `dev_id` persist and the pseudonym's `id_salt` mint used to fire after the
  face was up — a flash commit under RGB scanout, the exact garble mechanism
  2.4.4 removed from esp_wifi, still alive in our own writes. Both stores warm
  before `display_init` now.
- **The AP raise rides a backlight dip.** SoftAP bring-up's TX calibration
  burst is the boot's steepest current spike, and on the 7" glass it landed on
  a full-day backlight from one USB feed. The light dips to ambient across the
  raise (the Hello→Join scene change masks it) so a marginal supply cannot
  brown out mid-wizard.

Field debugging also gains its missing breadcrumbs: boot now logs whether
Wi-Fi credentials were seeded and via which scheme (never the values), and
the wizard logs its sweep results, AP raise, and join requests.

## [2.4.4] - 2026-08-03

### The 7" glass survives its own first light — and every Wi-Fi seed is honored

The Canary 7 (Nightstand 7) crash-looped through first boot: splash, clock,
"Let's get you connected" — then a garbled panel and a reboot, forever, the
join QR never shown. Four distinct defects, all fixed (PR #1416), all
affecting the dash family and two reaching vision/sense too:

- **LVGL 9 scene fades were memory events.** The onboarding wizard faded a
  full-screen container's `opa`, which on the LVGL 9.5 dash builds composites
  the whole 800x480 subtree through ~22 KB-per-frame layer chunks from the
  same 64 KB pool that already holds two live screens plus the QR buffer;
  exhaustion halts, NULL-derefs, or livelocks — a panic exactly at the scene
  change. The wizard now fades label text per-part (no compositing) and cuts
  its finish handoff on v9.
- **Wi-Fi bring-up no longer writes flash mid-scanout.** Arduino Wi-Fi
  persistence (on by default) committed esp_wifi config to NVS on every
  mode/softAP/begin call; a flash write stalls the RGB panel's non-IRAM
  bounce-refill and garbles the glass. `WiFi.persistent(false)` now precedes
  the first radio call — credentials live in our own NVS namespace.
- **The wizard feeds the task watchdog.** Raised from loop() (the
  wrong-password path), it ran under the armed 30 s TWDT without ever
  feeding it — a guaranteed panic mid-setup. Every wizard wait loop and the
  bounded boot connect now feed it.
- **Both Wi-Fi seed schemes are honored.** A stale flasher frontend writes
  blob-scheme `wifi_ssid`/`wifi_pass`, which the string readers saw as
  present-but-empty and re-raised onboarding over good credentials. The
  credential loaders (display, vision, sense) now fall back to the blob when
  a key exists but reads empty; `desktop_parity.test.js` pins it.

### `cargo audit` goes green again — with the one unfixable advisory named, not hidden

The weekly dependency audit has been failing since 2026-07-27 on a single
advisory, which meant every lockfile-touching PR inherited a red security
gate that had nothing to do with it — the exact way a real finding gets
missed.

- **The advisory:** RUSTSEC-2023-0071, the Marvin Attack — `rsa 0.9.10`'s
  private-key operations aren't constant-time, so timing can leak the RSA
  private key (5.9 medium).
- **There is no bump that fixes it.** `rsa` enters the tree through exactly
  one crate, `c2pa 0.90.3` (newest published), which requires `rsa ^0.9.10`
  non-optionally. The constant-time rewrite is in `rsa 0.10`, still a
  release candidate. Upstream `c2pa` is the only real fix.
- **securaCV was never on the vulnerable path.** The attack recovers an RSA
  *private* key, and we hold none: the C2PA credential chain is Ed25519 end
  to end (`PKCS_ED25519` certs, `SigningAlg::Ed25519`), and `c2pa` compiles
  in only behind the optional, non-default `c2pa-export` feature. `rsa` can
  only ever run a public-key verification here — no secret to leak.
- **So it is ignored in the open**, in the new `.cargo/audit.toml`, with the
  full reachability analysis recorded in SECURITY.md as "Group 3" and a
  standing instruction to delete the entry once `c2pa` moves off `rsa 0.9`.
  Every other advisory stays fatal; the 9 informational warnings are
  unchanged. Verified locally: `cargo audit` exits 1 without the config and
  0 with it, on the same `Cargo.lock`.

### The Lab's build line is finally complete — and it can't silently rot again

The bench migration begun with the Lab shell is finished: every committed
page now lives on the six-stage build line, and a new CI gate keeps it that
way.

- **Three finished benches come out of the shadows.** The Case Catalog
  (browse every enclosure, with the three-question finder as its guided
  depth) docks at Build; "Listen for a smoke alarm" — the WAP's ears, real
  firmware wasm — docks at Sense as the new **Sound** track, alongside
  camera and radar. All three were built, tested and cross-linked, but
  absent from `build-line.json`, so the shell, the room, the site map and
  the desktop app never showed them.
- **The map now contains itself.** `site-map.html` is listed in the
  manifest's HTML inventory it renders.
- **The completeness gate the design always promised.**
  `canary-local/tests/build_line.test.js` fails CI when a committed page is
  missing from the manifest, a manifest reference points at nothing, a
  redirect target doesn't resolve, slugs collide, or the room's offline
  slug map drops a bench — the "if it's not in here, it doesn't appear on
  the line" rule, enforced instead of hoped. Three existing test files that
  were never enumerated in CI (`vision_checklist`, `vision_session`,
  `wifi_memory`) are now wired in too.
- **Every bench exits to the Lab, not through the old front door.** The
  fineprint back-links that pointed at the deprecated `index.html` redirect
  now land on `lab.html` directly; the House's "meet them live" door goes
  straight to the fleet cards.
- **The room's degraded mode matches the manifest.** The isometric room's
  hardcoded fallback tools had drifted (old nouns, missing stations); they
  now mirror the manifest, and its offline slug map covers every bench and
  depth page.
- **The manifest stops promising a surface that doesn't exist.** `app.nav`
  no longer declares a native "stage-tabs" toolbar the desktop app never
  grew; it documents the real architecture — the same adaptive shell,
  rendered from the same manifest, in a native window.

## [2.4.3] - 2026-08-02

### The Canary Nightstand 7 — the 7" glass, turned toward the bed

The 7" board now ships as two products. The Dash 7 stays the wall panel; the
new **Nightstand 7** is the same hardware wearing a bedside face, with its own
OTA product so a bedroom glass can never cross-grade into a wall dashboard.

- **A clock you can read from bed.** The hero is drawn in seven segments from
  UI primitives rather than typed in a font — the built-in faces stop at 48 px,
  nowhere near a 7" bedside clock — so it stays crisp at any size and
  red-shifts after dark with the rest of the palette.
- **Day complications, night focus.** By day: the temperature now, today's
  high and low, tomorrow, comfort words for the bedroom, sun times, and a quiet
  strip of the fleet. After dark it becomes a clock — red digits, tomorrow's
  weather in a whisper, everything else black.
- **Every kind of dim on this board is drawn**, because its backlight is on/off
  only. That includes the gentle wake's sunrise, which is now rendered as a
  warm field rising rather than a backlight ramp.

### A night light that never lies about your fleet

The honest night light the nightstand line specified is built, on the 7" glass
and on the small portrait displays that live in hallways — where the screen
itself *is* the lamp. You summon it, it times out, and it never comes back on
its own after a reboot. Its light is a decorative scene, never a state color,
so it cannot say "safe" by glowing — and the instant anything needs attention
it hands the glass back and stays out until you ask again. The across-the-room
beacon is never part of the lamp: dark still means all is well.

### The glass checks in

On an idle, lit display the canary now stirs every few minutes, or the status
quietly surfaces and fades — minutes apart by day, rarer at night, and never
over an alarm or into a dark room. Two displays on one dresser won't move in
lockstep.

Also: the BOOT button finally does something on the boards without a touch
panel (tap to peek, double-press for the light, hold to acknowledge); the hub
weather feed can carry tomorrow's forecast and weather advisories, shown but
never sounded; and the color engine gained a Rainbow scene.


## [2.4.2] - 2026-07-29

### The Canary Watch Station is back in the train

2.4.1 published six of the seven display products; the Watch Station image
was missing, as it had been from every release before it. The cause was
toolchain state, not the firmware: the release job installs three ESP32 core
versions in one runner, each restoring a toolchain cache into the same
directory, and an FQBN carries no version — so the watch compiled against the
newest installed core with the older core's library row beside it and died in
seconds, warned away by a deliberately non-blocking build loop. Each display
row now uninstalls other core versions and asserts it got the one it asked
for, and the fix was proven on the dev channel (a real watch factory image)
before this version was cut. Nothing on the device changed: same 2.4.1
firmware behavior, now actually built and published for every board.

## [2.4.1] - 2026-07-28

### On the device: what 2.4.1 changes since 2.4.0

- **The Nightstand Touch (1.69") sounds the "canary wakes" boot chime on
  power-on** — the audible sibling of the boot-yellow beacon, so a shelf
  device answers the power switch before its panel initializes.
- **Radar tuning suite**: every Sense knob is live over USB serial, from
  both flashers — tune presence detection on the bench without a reflash.
- **Broker-down mDNS discovery seats sibling displays, not witnesses
  only** — displays finding each other no longer depends on the MQTT
  broker being up.
- **Every display has a live emulated twin** (nightstand + touch169 WASM
  flavors join the fleet page), compiled from the same firmware these
  images carry.

### Headless Pi 5 hub, verified against HA's own docs — no monitor, ever

The "flash → power → it appears on your network" promise is now built on the
documented Home Assistant OS contract instead of hope, and the whole path
assumes nobody ever attaches an HDMI cable. The Wi-Fi keyfile the Flasher
seeds onto the boot partition (`CONFIG/network/` — HA's docs: *"Alternative
you can create a CONFIG folder inside the boot partition"*, read on startup,
LF-only) now carries two things it was missing: a stable `uuid=` minted at
flash time (HA warns the hub's IP can change on every boot without one) and
`llmnr=2`/`mdns=2` on the connection — the same values HAOS's default wired
profile uses — so a Wi-Fi-only hub answers `homeassistant.local` at the OS
resolver level, not just once Home Assistant is fully up.

The finding side stopped assuming mDNS works everywhere. The Flasher's
first-boot watch now says up front that no monitor or keyboard is needed, and
if the hub hasn't answered after 25 minutes it swaps "be patient" for a
concrete checklist: try it from a phone (some computers can't resolve
`.local` even when the hub is up — including the watcher itself), read the
router's device list for "homeassistant", and — since a typo'd Wi-Fi password
is invisible from outside — plug in ethernet (zero setup) or re-flash. The
Lab's Hub wizard carries the same promise and the same fallbacks
(two-frontends rule).

### Every app keeps itself fresh — and says what's changing

Self-update is now a contract both desktop apps honor, not a Flasher-only
feature. The **Flasher** (0.3.5) re-checks on a six-hour routine while it
stays open (and when a stale window regains focus), shows the pending
release's own notes in the update banner and About page, and records every
check and install in its local activity log. The **Lab** (0.2.0) gains the
whole shape for the first time: signed self-update against its own rolling
`lab-latest` pointer, a boot + six-hour check routine, a native "what's
changing" dialog that asks before anything installs, and a local
`update-journal.log` — desktop only, with the iOS/iPadOS builds compiling it
out (the App Store owns updates there).

What's-changing text is now a checked artifact, not prose that rots: each app
carries a `RELEASE_NOTES.md` (newest-first, per version, written for the
user), `release_notes.py check` fails any build whose newest section doesn't
match the version being shipped, and the section flows into both the GitHub
release body and the updater manifest's `notes` (via the now-shared
`harden_updater_manifest.py`). Publishing the Lab's draft triggers
`desktop-lab-updater-pointer.yml`, which hardens the manifest, refuses to
proceed unless every updater URL resolves, and only then advances
`lab-latest`. `desktop_parity.test.js` pins the whole contract (endpoint tag
= workflow tag, distinct pointers, one shared pubkey, updater artifacts on).

### Pet & sleep presets for the radar (movement wake/sleep + dog/human vitals)

Three researched presets for the Canary Sense radar, with feasibility stated
honestly (the MR60BHA2 computes breath/heart BPM on-module, band-passed for
human physiology; this firmware reads those scalars):

- **🐭 Mouse / small-pet cage (both builds)** — a *movement* wake/sleep watch
  for a caged small rodent, not vitals. For a fixed cage, a live moving
  occupant reads as awake and sustained stillness as asleep, off the existing
  presence pipeline. A mouse's heart (300–800 bpm) and breathing (80–230/min)
  are 4–8× above the module's human passband, so vitals are deliberately left
  untouched — the preset never pretends to read them.
- **🐕 Dog kennel / crate (wellbeing)** — a real vitals preset. A resting
  dog's heart (≈50–160 bpm) and breathing (≈8–35/min) overlap the human bands
  the module is tuned for, so a settled dog within ~1.5 m reads like a human
  torso. Bands widened for small-breed hearts and faster resting breathing;
  longer lock windows suit an animal that stills in bursts.
- **🛌 Human sleep & wake (wellbeing)** — the native case, bands trimmed to a
  sleeping adult.

Vitals presets are gated to the wellbeing build (the presence-only build has
no breath/heart knobs, so offering them there would be a lie); the mouse
preset applies to both. Each carries an ⓘ explainer stating what it can and
can't do. Feasibility contract pinned in `flash_epic.test.js`.

### Radar tuning suite: every Sense knob live over USB, in both flashers

The canary-sense firmware grows a **serial tuning console**
(`firmware/common/console/tuning_console.h`, host-tested): line commands
(`help` / `cfg` / `set <knob> <value>` / `reset` / `stream` / `raw`) at
115200 8N1, live from `setup()` — before WiFi, before the broker — so a
freshly-flashed board is tunable and testable immediately. Every runtime
knob goes through the same clamping NVS setters HA uses, and the four
vitals plausibility bands (`breath_min/max_bpm`, `heart_min/max_bpm`) are
promoted from compile-time constants to full runtime knobs (NVS +
`cfg/breath_min/set`-style MQTT topics + HA number entities + serial), so
the breathing/heart-rate lock can be calibrated to a real person on the
bench. A default-on 1 Hz `[radar]` stream line shows what the radar sees
(state/count/range, lock + BPM on wellbeing); the opt-in `raw on` bench
mode echoes raw cm/BPM on the attended cable only — the documented,
session-scoped exception to the coarse-vocabulary rule.

Both flashers grew the matching **tuning suite UI** (two frontends, no
shared code — parity is now CI-gated in `desktop_parity.test.js`): the
browser radar bench (`flash.html`, straight from the post-flash "prove it"
button) gains catalog-driven sliders for every knob wired to the console,
restore-defaults / stream-cadence / raw-detail controls, and a classified
live log (stream quiet, verdicts pop, errors glow) with hold-scroll and
clear; the desktop Flasher's serial monitor gains the same panel over its
existing send path. Sliders reconcile only to the firmware's `[cfg]`
snapshot line — never to their own optimistic state — and both frontends
say so honestly when older firmware has no console. Knob vocabulary,
bounds, and defaults flow from one source (`gen_flash.py` → catalog
`reflexes.knobs[].console`), parsed from the firmware headers so no
surface can drift.

### The offline viewer now reads event exports (Reading Room P1a)

`viewer/evidence_viewer.html` accepts the `ExportBundle` JSON that
`export_events` writes — not just full evidence envelopes. A new
`verifyExportBundle` in `verify_core.js` mirrors the Rust
`verify_export_bundle` check-for-check (receipt entry hash → signature →
artifact binding), pinned to a shared fixture
(`tests/fixtures/export_bundle/`, generated by
`cargo run --example gen_export_bundle_fixture`) that both the Rust and JS
test suites verify. The viewer then *shows* the record: an event timeline
with inline gaps, the signed disclosure receipt (authorization, window,
hashes), the device key with an optional paste-to-confirm-authorship
field (integrity-only verdicts carry an honest self-attested note), and a
privacy-coarsening caption. A committed sample export powers a "Try the
sample export" demo. Verified in a real browser: sample/trusted-key/
wrong-key/tampered-drop flows plus a zero-network-requests assertion.
First slice of docs/design/witness_log_viewer.md.

### C2PA Content Credentials for export bundles (`c2pa-export` feature)

`export_events --c2pa` now signs an industry-standard C2PA sidecar manifest
(`<bundle>.c2pa`) over the exact export bytes, so any Content Credentials
tool can verify a SecuraCV export without SecuraCV installed. Keys derive
from the device seed with domain-separated HKDF (never stored, never the
chain key); the credential chain is a byte-reproducible device-local CA, so
the trust anchor can always be re-derived (`--c2pa-anchor-out` writes a
convenience PEM). `export_verify --c2pa-manifest` requires the sidecar to validate to
`Trusted` against the device CA (a well-formed manifest under any other
credential is `TAMPER`) **and** enforces the cross-binding: the manifest's
`org.securacv.witness` assertion must name exactly the receipt entry
embedded in the bundle, with the artifact hash that receipt committed to.
Scheduled-export pruning (`--output-dir --keep`) removes a bundle's
`.c2pa` sidecar together with the bundle. Fully
offline (no OpenSSL, no HTTP backend, no TSA); the hash-chained log remains
the root of trust. Design: `docs/design/c2pa_export.md`. Tests run in CI
(`cargo test --features c2pa-export`).

### Releasing is one button, and every dead end now names itself

Following the release path as a first-time operator found the pipeline blocked on
one missing key — and *every* button's answer to that was silence. Fixed by making
preconditions part of the plan instead of a surprise in someone else's workflow run.

- **The master button knows why firmware can't ship.** "Update everything (only
  what needs it)" used to dispatch a firmware release that dies 20 seconds later
  when the Ed25519 signing key is absent, having reported *releasing firmware*.
  It now reports firmware as **gated, with the one-time ceremony inline**, and
  ships every other target. Implemented through the existing tested gate
  machinery: `release-targets.yml` gained `gate_var` + a new `gate_reason` (the
  default text describes a repo *variable*, which for a derived gate would send
  you hunting a flag that was never the problem). Four new unit tests — 47 total.
- **A key-state answer that needs no key.** `firmware/scripts/ota_key_state.py`
  reports whether the release public key is embedded or still all zeros (OTA
  hard-disabled), with the key id, from the committed header alone. Any button —
  or any human, locally — can ask. Also printed on every `lint.yml` run, because
  a repo without a key is legitimate but a *silent* one is what made every product
  read "unavailable" with no clue why.
- **"Do exactly this" now refuses the impossible.** `release-one-click.yml` stops
  before dispatching a firmware release that cannot be signed, and warns when
  publishing an app version that is already released — which overwrites that
  release's assets rather than cutting a new download. Its input descriptions now
  say when *not* to use it, and it points at the master button.
- **One version per app, enforced.** `desktop/scripts/check_app_versions.py`
  requires `tauri.conf.json` (names the release tag), `package.json`, and
  `Cargo.toml` (**what the app shows the user**) to agree — for both apps, on
  every PR. The previous check compared two of the three and missed the one
  humans read, so the Flasher shipped as `0.2.2` displaying `v0.1.0`. Flasher
  bumped to **0.2.3** across all three files and `Cargo.lock`.
- **Both flashers name the pinned tag.** The desktop app used to swallow the
  manifest fetch error entirely; it now says *"pinned to firmware release
  fw-v2.3.0, which has no published images"* — matching the browser flasher.
  They share no frontend, so this is a fix that has to be made twice, and that
  is now written down in `CLAUDE.md`, `AGENTS.md`, and Principle 14.
- **The ceremony script tells the whole truth.** `setup_release_key.sh` printed a
  commit list that omitted the regenerated `canary-local/devices/flash.json` —
  following it exactly turned `canary-local` red on the next push and left the
  flasher on checksum-only verification. It now lists all four files, says to
  clear the clipboard after pasting a master signing key, and explains the PEP 668
  venv escape when `pip install cryptography` is refused by a Homebrew Python.
- **[`docs/RELEASE_BUTTONS.md`](docs/RELEASE_BUTTONS.md)** — the operator's index:
  every button, when to press it, when not to, the three failures that have cost
  real time, and the invariants CI holds up so a red run makes sense. Linked from
  the docs index, `CLAUDE.md`, and `AGENTS.md`.
### Home Assistant — a Philips Hue bulb becomes an alert beacon

A new importable blueprint,
[`docs/blueprints/securacv_hue_alert_light.yaml`](docs/blueprints/securacv_hue_alert_light.yaml),
drives any color light HA controls (Philips Hue is the reference setup) as a
silent alert signal for a Canary: critical events — tamper, smoke/CO heard,
witness-chain failure — snapshot the light, pulse it red, and hold solid red
until the sensors clear; the all-clear blinks green and restores the light's
previous state, and a device going offline gives three gentle amber pulses.
No firmware or integration changes — it rides the binary sensors the HA
integration already exposes. Referenced from the setup guide (Step 5) and
picked up by the hub image catalog.

### Release pipeline — three silent failures made loud, and guarded

Three ways the pipeline could ship something broken while every workflow stayed
green. Each is fixed *and* given a gate, because all three were invisible until
someone went looking.

- **`releases/latest` belongs to the firmware again.** Every Canary polls
  `releases/latest/download/manifest-<product>.json`, and GitHub's "latest" is
  the newest published release of any kind — so publishing the Flasher or the
  Lab silently re-pointed the whole fleet's update URL at a release with no
  manifests in it. Devices would 404 on their next check and stop seeing
  updates, with nothing failing anywhere. New
  `.github/actions/keep-firmware-latest` asserts the invariant and re-points
  `latest` at the newest published `fw-v*` release; the app workflows call it
  directly (CI-created releases don't fire `release:` events, which is exactly
  the case that breaks the fleet) and `release-latest-guard.yml` covers every
  release a human publishes, edits, or creates in the UI.
- **The Canary Displays can update now.** Each display flavor's `config.h`
  pointed `SECURACV_OTA_MANIFEST_URL` at `manifest-canary-display-<flavor>.json`
  and **no workflow had ever signed one** — the boards were flashable over USB
  and structurally unable to self-update, silently. `firmware-release.yml` now
  signs, verifies, and indexes the watch / dash / dash-modes manifests,
  best-effort like the display builds themselves: a flavor that didn't compile,
  or whose binary doesn't carry this release's version, is dropped with a
  warning rather than sinking the signed release or publishing a manifest that
  would trap devices in a rollback loop. New
  `firmware/scripts/check_ota_channels.py` (in "Regression Guards") proves
  statically that every URL the firmware polls is one the release publishes —
  or is declared unpublished with a reason. It reads **both** places a device
  learns that URL: the C/C++ `#define` fallbacks and the PlatformIO
  `-DSECURACV_OTA_MANIFEST_URL=` build flags in the env files. That second half
  matters — the per-board envs are where the exotic flavors live, and they turned
  out to hold three more identities with no channel behind them
  (`dash-mic`, `dash7`, `nightstand-s3`), now declared alongside the Arduino-path
  ones (`watch-modes`, `nightstand`, bare `canary-display`) instead of forgotten.
- **A dark flasher says why it's dark.** `flash.json` pins `manifest_url` to an
  exact `fw-v<train>` tag (right — `/latest/` is unsafe in a shared namespace),
  but a tag bumped before its release is cut made every product read
  "unavailable" behind a banner indistinguishable from "nothing has ever
  shipped". The page now names the pin — "pinned to firmware release fw-v2.3.0
  — no release manifest (HTTP 404)" — via the pure, host-tested
  `releaseTagFromManifestUrl()`, and `canary-local` CI warns on every run while
  the pin doesn't resolve (advisory: the bump-then-release window is
  legitimate, being invisible was not).
- **Smaller things on the same path.** The Flasher release fails fast on
  `tauri.conf.json` / `package.json` version drift (they had drifted to 0.2.1 vs
  0.1.3, so the app stated its version two ways); the resolved `usbboot` commit
  is now a `::notice::` plus a run-summary line instead of buried build output,
  so the session that finally pins `USBBOOT_REF` can read it off a build that
  worked. All four lessons are written up in `.github/RELEASE_LESSONS.md` with
  new Principles 6–8, and `docs/RELEASE_PROCESS.md` documents the
  `releases/latest` ownership rule where releases are actually run from.

### SecuraCV Flasher 0.2.2 — the Witness Wall ships, and the release itself is hardened

- **Version bump to cut a real download.** `flasher-v0.2.1` was published
  before the Witness Wall embed, LAN discovery, and the release-hardening work
  landed, so those three never reached an installer. `tauri.conf.json` moves to
  **0.2.2**, which is what the "Desktop Flasher — build & release" workflow
  reads to name the tag — so this release carries:
  - the **embedded Witness Wall**, with a just-flashed Canary appearing on it,
  - **real LAN discovery** after a flash, so the wall fills with live devices,
  - the **hardened publish path** — per-job asset reconciliation with backoff,
    a single-writer `latest.json` rewritten with stable name-based URLs and
    signatures read from the `.sig` assets the release actually carries, and a
    consistency guard that fails the release rather than shipping an updater
    manifest that can't self-update.
- **Nothing else changes in the app.** This is a packaging release: the bump
  exists so the above reaches users as a downloadable `.dmg` / `.AppImage` /
  `.deb` instead of sitting on `main` behind an already-published tag.

### canary-display — the emulator now VOICES the Canary, and the sound board is on the map

- **Hear the display in context.** The in-browser emulator (`canary-local/`)
  now plays the real Canary Voice grammar — the boot chirp on power-on, the
  alert/warn/all-clear tiers as the emulated fleet changes, and the
  ack/page/mute interaction tones as you touch the glass. `FEATURE_CHIME` is
  enabled **in the emulator build only** (it's not real hardware, so the
  piezo-unpopulated gate that keeps it off in shipped firmware doesn't apply).
- **Faithful, not a beeper.** The emulator's chime is now a persistent Web-Audio
  "piezo" voice whose frequency *and* gain follow the firmware's real
  per-control-tick LEDC writes: the tone channel's duty drives audio amplitude
  (`emu_support.cpp` routes it to `onTone`), so you hear the actual envelope,
  glissando, warble, and volume model — not a flat square blip. Because the
  sound is the real firmware driving it, it cannot drift from `voice_score.h`.
  A remembered corner mute toggle keeps the room yours; audio primes on the
  boot gesture so the power-on chirp isn't lost to the autoplay policy.
- **The sound board has a home.** The standalone board (`canary-local/voice/`)
  is now registered in `build-line.json` — the one manifest every nav surface
  reads (sitemap, room, app) — so it's reachable, and a new
  `voice_nav_probe.mjs` (in CI) fails if it ever loses its spot or stops being
  the generated artifact. Between that, the preview drift gate, and the dist
  drift gate, every way you can hear these sounds is now guarded against rot.

### firmware/common — power-event resilience + an honest outage log (shared core)

- **A pure, host-tested decision core for "when did the power go out."** New
  `firmware/common/power/power_events.h` classifies how the *previous* power
  session ended, from the reset cause plus a clean-shutdown flag, an RTC-domain
  survival marker, and a liveness heartbeat — and names each event the correct
  way: **cold boot**, **clean reboot**, **brownout reset** (supply *sagged*
  below the detector), **power restored (outage)** (mains lost while running,
  now back), and **fault reset** (a crash, tracked separately). It keeps a
  durable ring of the last events plus monotonic counters (outages, brownouts,
  longest outage). Crucially honest: a device can't record the instant it lost
  power (it's off), so an outage carries an explicit **lower-bound** duration
  (now − last-seen-alive), never a fabricated timestamp.
- **Proven, not reviewed.** `firmware/tests_host/test_power_events.cpp` (66
  checks, `-Wall -Wextra -Werror`, wired into the common host suite) pins the
  whole classification table, the terminology, the lower-bound arithmetic, and
  the ring/counter behavior.
- **Shared by construction.** The header sits on every firmware's include path
  (`-I firmware/common`), so all Canaries adopt the same logic. Following the
  repo's core-first convention (as with `boot_policy.h`), the pure core lands
  here host-tested; the per-firmware boot-path glue lands separately and
  hardware-validated — the recipe, the brownout-detector hardening notes, and
  the witnessed-event option are in `docs/design/power_events.md`. The 4.3C
  (which has a PCF85063 RTC for real wall-clock times) is the priority surface;
  its README's "staged, not started" outage line is now half-built.
- **Canary base reference wiring.** `firmware/canary/include/canary_power_events.h`
  is the boot-path glue — `on_boot()` (classify the previous session's ending +
  append to the NVS log + console line), `witness_incident()` (sign a restored-
  outage/brownout record), `heartbeat()` (the loop liveness persist, clock-
  gated) — wired into `main.cpp` at three sites. It feeds the pure core real
  signals (esp_reset_reason, an RTC-domain survival marker, the boot counter)
  and is the copy-me template for the other firmwares. Its C++ + core-API usage
  is verified against the real header signatures; the ESP build compiles on CI.

### canary-display — Canary Voice: a browser preview that can't rot, and a sound clearance guard

- **You can hear the signatures now — and the preview can't drift.** A browser
  sound board (`canary-local/voice/`) plays every Canary Voice signature with
  its real envelopes, glissando, warble, and the live volume/night model. It is
  **generated from the firmware** — `tests_host/dump_voice_score.cpp` renders
  each signature and the volume table straight from `voice_score.h` via the
  real compiler, and `canary-local/tools/gen_voice_preview.mjs` injects that
  into the page — so the preview owns no audio math of its own and cannot
  diverge. A CI drift gate (`canary-local.yml`) fails if the committed page is
  stale, the same mechanism that guards the emulator dist and `workshop.json`.
- **Acoustic clearance, machine-checked.** New `display_sound_clearance.md`
  audits every signature as original and unencumbered (public-domain pentatonic
  scale; no sampled or trademarked chime; the alarm uses the IEC 60601-1-8
  *principle* via two bare frequencies, not a protected melody). New
  `tests_host/test_voice_clearance.cpp` (+ CI step) proves no signature
  reproduces a **regulated life-safety cadence** — ISO 8201 Temporal-Three
  (fire) or Temporal-Four (CO) — so a status chirp can never be confused with a
  real alarm; the detectors self-verify against the genuine cadences so the
  guard can't rot into a rubber stamp.

### canary-display — the mic does something useful: wake the screen on a sound

- **Opt-in "wake on sound".** A dark 4.3C dash now lights the moment you walk
  in — a door close, a knock, a footfall. It's a pure, host-tested
  `TransientDetector`: a loud *onset* well above the tracked ambient
  (refractory-gated, seeded so the capture-start glitch can't fire it),
  running on the **same RMS envelope the alarm path uses** — no samples, no
  classification, never speech. The mic layer only latches a request;
  `main.cpp` turns it into a wake window, the identical path a finger-tap
  takes. Off by default (Settings → microphone → wake on sound), NVS-
  persisted, active only while the mic is listening. The mic contract doc +
  the on-glass caption now state the second use honestly; everything stays
  inside `FEATURE_MIC_ALARM` (default/emulator builds byte-identical).
- **Fix (review): mic capture was pointed at the speaker line.** The vendor
  I2S signal names are from the codec's view, so `ASDOUT` (ADC Serial Data
  OUT = the mic ADC's output = the ESP32's data-IN) is **GPIO43**, not the
  GPIO15 `DSDIN` (DAC data-in = speaker path) we'd filled. Confirmed against
  Waveshare's own speaker-microphone example. Reading the wrong one would have
  left `SNAP rms` silent; `AUDIO_PIN_I2S_SDIN` now = GPIO43.
- **Fix (review): no weekly date-only board-facts PRs.** `gen_board_facts.py`
  now keeps a board's committed entry — `verified_utc` included — verbatim
  when a clean fetch finds no factual change, so the freshness job only files
  a PR when facts actually move (the date means "as-of the current facts").

### canary-local — the 4.3C mic made visible, and drift-locked against rot

- **The decision core, in the browser.** New `assets/mic-sim.js` is a DOM-free
  1:1 JavaScript port of the firmware's `mic_logic.h` — the adaptive
  noise-floor `Envelope`, the NFPA-72 T3 / UL-2034 T4 `CadenceDetector`, the
  opt-in wake-on-sound `TransientDetector`, the sensitivity presets, and the
  listening `Gate` — with the same integer arithmetic. It takes an RMS
  **scalar** per frame, never samples, exactly like the runtime past its
  privacy barrier.
- **No rot: it's re-pinned to the firmware every CI run.** New
  `tests/mic.test.js` reads the committed Arduino mirror of `mic_logic.h`,
  asserts every constant (presets, beep windows, gaps, wake threshold,
  refractory, wire names) matches the port, **and replays the host test's own
  scenarios through the JS** — smoke needs two cycles, a doorbell/speech never
  alarms, zero-duration timing fails safe, the floor self-calibrates, wake
  fires once per onset. A drift on either side breaks the build.
- **The magic: it runs in the browser two ways.** New page `dash-mic.html`
  drives that core from scripted sounds (smoke, CO, doorbell, a door close,
  speech — the alarms fire, the non-alarms correctly don't) **or** from the
  visitor's live microphone, reducing each 20 ms to one loudness number and
  showing exactly that number as the only thing crossing the barrier. Same
  gesture-gated, discard-every-frame, never-recorded safeguards as the WAP
  acoustic bench, and a standing "demonstration, not a life-safety device"
  line.
- **Reassurance, from one source.** The page's does / never-does copy comes
  from `MIC_FACTS` in the sim and the numbers from the drift-locked constants,
  so the words and the figures can't disagree with the firmware. Linked from
  the fleet emulator and the WAP bench; registered in the Lab manifest.
- **The help + flasher match it.** The display usability protocol's mic task
  gains wake-on-sound probes and a comprehension pre-check against the new
  page; the flasher's display twin points to the same does/doesn't mic bench.

### canary-display — the 4.3C mic front end wired (ES7210 bring-up)

- **`es7210_init` written**: with the I2S pins now vendor-exact, the ES7210
  ADC gets a datasheet-grounded register bring-up (soft-reset → I2S slave →
  16-bit frame → mic channels powered at mid-scale PGA → DC high-passed),
  run the moment the I2S master starts clocking. This closes the last gap
  between "pins are right" and "the mics actually capture."
- Honest about what's proven: the sequence is compile-verified by the
  `canary-display-dash-mic` env and **bench-validated by the `MIC1 SNAP rms`
  line** — rms climbing off zero in a live room is the pass signal; the gain
  (0x43/0x44) and clock-ratio (0x07) values are the two bench knobs if
  capture is silent or clipped. The console now reports `es7210_init=<n>`
  (writes the ADC refused; 0 = clean). Everything stays inside
  `FEATURE_MIC_ALARM` (default/emulator builds byte-identical), off by
  default, and arm-gated — the privacy contract is untouched.

### canary-display — Canary Voice (the sound engine grows up) + BLE 5 long range

- **The chime became a *voice*.** The severity-tiered chime engine is now a
  full acoustic-signature engine (`hal/voice_score.h` — pure, host-tested —
  behind the LEDC streamer `hal/chime.cpp`). On the same single passive piezo
  it now renders **shaped envelopes** (an eased attack/release that de-clicks
  every note — the difference between an instrument and a beeper),
  **glissando** (a note can chirp), and **warble** (a fast vibrato — the
  songbird it's named for). Nothing is blocking; a note is rendered a few ms
  at a time from the main loop.
- **A family of signatures, uniquely ours.** Beyond the four alert tiers there
  are now Boot ("the canary wakes" — a rising warbled chirp), JoinSuccess
  (onboarding), AckConfirm, PageTurn, and MuteOn/MuteOff, all drawn from a
  warm **major pentatonic** so they share one timbre and can never clash — a
  household learns them like a marque's startup chime. The **Tier-1 alarm
  alone** keeps its frozen IEC 60601-1-8 pitches, deliberately outside the
  pretty scale: a dead battery must never sound like an intruder.
- **Volume that makes sense.** One knob (0–4: Off…Full, persisted to NVS).
  Category **ceilings** keep a UI blip quieter than a fault; **night**
  silences the interaction/ambient voices and attenuates notices while Wake
  and Alert sound through; and the **Tier-1 alert keeps an audible floor at
  every volume including Off** — the one sound allowed to break the night
  can't be dialed to nothing. Interaction tones are the *optional* half
  (a single toggle, default on). Wired at the real event sites (boot, ack,
  page, mute, onboarding), all under `FEATURE_CHIME` so default builds stay
  byte-identical; the pure grammar is pinned by `test_voice_score.cpp` in CI.
- **BLE 5 long range for off-grid resilience — `FEATURE_BLE5_SCAN`.** The
  passive Chirp scanner can now arm the BLE 5 extended scanner for
  **Coded-PHY (LE Long Range)** chirps — ~4× the legacy-1M range, the
  difference between the far-corner Canary's tamper reaching the glass with
  the router cut and not. Same 17-byte chirp payload (BLE 5 buys range, not a
  new format); the flag widens the scan dwell and raises the BLE heap gate
  (`ble_gate.h`) for the extended scanner's larger controller footprint.
  Compiled + CI-built (`canary-display-dash-ble5`) but **disabled by default**
  and bench-gated exactly like the chime — real Coded-PHY reception also needs
  the NimBLE build's extended advertising and a transmitting Canary, so a
  radio coexistence + range soak is the last gate.

### hardware — vendor board-facts snapshot, kept fresh, and the model made exact

- **A machine-readable snapshot of the Waveshare dash boards' facts**
  (`canary-local/devices/board_facts.json`): the full pin maps, onboard
  silicon, and parameters for the **4.3 / 4.3B / 4.3C**, transcribed (facts
  only — Waveshare's wiki stays canonical and linked) from their pages by a
  new parser/refresher `canary-local/tools/gen_board_facts.py`.
- **It can't go stale.** A weekly (+ on-demand) freshness loop
  (`.github/workflows/board-facts-freshness.yml`, modeled on the
  Home-Assistant one) re-pulls each vendor page and opens a PR when any fact
  moves. Self-healing: a board's facts advance only on a clean fetch+parse; a
  403 / dead feed / reshaped page keeps the last good snapshot verbatim, and
  `verified_utc` (shown as an age on the reference page) tells on a broken
  loop.
- **Our model made exact.** The 4.3C's I2S audio pins — long shipped as `-1`
  because "never ship a guessed mic pin" — are now filled from the vendor's
  own pin-mapping table (MCLK 6, SCLK 44, LRCK 16, mic-data-in 43; PA on
  CH422G EXIO4). They're facts now, not guesses; only the ES7210 register
  init remains bench-pending, and the mic stays off-by-default + arm-gated.
  A drift-lock test (`canary-local/tests/board_facts.test.mjs`) proves every
  board's firmware RGB/LCD map equals the vendor snapshot, so our `pins.h`
  and the vendor page can't silently diverge.
- **First-party reference** `docs/hardware/waveshare_board_reference.md` — our
  own words over the facts (pin maps, the CH422G "switched" EXIO bits, the
  per-board interface differences, mechanicals), the vendor page linked as
  canonical, and the freshness mechanism explained. Stale mic-doc / board-
  README claims about `-1` audio pins corrected to match.

### release pipeline — one-click firmware release (dev or prod), and the flasher gap it closes

- **`firmware-release.yml` is now dispatchable** (Actions → "Firmware
  Release"): pick `channel` (release / dev) and a `version`, and it builds,
  signs, and publishes exactly like the tag-push path — then creates the tag
  itself from the version at the current commit, so tag and source can never
  disagree. The version's grammar is cross-checked against the channel (a
  dev version on the release channel, or vice versa, is refused), an
  already-published tag is refused, and the existing version-string guard
  still fails closed if the headers weren't bumped. The `push: tags: fw-v*`
  ceremony is unchanged.
- **The whole-pipeline launcher now includes firmware.**
  `release-one-click.yml` ("Release — one click (firmware + apps + web)")
  gains a `firmware` selector (none / dev / release) + `firmware_version`, so
  one button can cut the firmware release — the OTA `.bin` images **and** the
  browser-flasher factory images + `manifest-flash.json` — alongside the
  desktop apps and the site deploy.
- **`flasher-release.yml`** (rebuild the flasher assets for an existing tag)
  now defaults a blank tag to the stable `releases/latest`, so refreshing the
  flasher for what's live is a zero-input click.
- **Why it mattered:** the three Canary Display flavors were fully wired into
  the catalog and both release workflows but invisible in the in-browser
  flasher — the only published release predated them, and a firmware release
  was a local `git tag` step outside the one-click pipeline. A product is
  "unavailable" until a release carries its `manifest-flash.json` entry;
  cutting that release is now one click. Recorded in
  `.github/RELEASE_LESSONS.md` (Principle 6 + a dated entry) and
  `docs/RELEASE_PROCESS.md`.

### canary-sentinel — the multi-sensor fusion guardian (Phase 0)

- **A new project: a doorway/window guardian that fuses physically independent
  sensing channels** — PIR (thermal), 60GHz radar (radio reflection), WiFi CSI
  (channel perturbation), WiFi/BLE device counting (carried radio), ambient
  light and — Heavy tier — door-contact/tamper/vision (mechanical/optical) —
  into one debounced, privacy-preserving people-detection decision. The thesis
  is corroboration across independent physics: to be invisible you must defeat
  every modality class at once, and *blinding* a channel raises an alarm rather
  than lowering the score.
- **The novel core is host-tested, not just asserted.** `common/fusion`
  (`sentinel_fusion.*` + `sentinel_channels.h`) is allocation-free, Arduino-free
  and time-injected; `tests_host/test_sentinel_fusion.cpp` and
  `test_sentinel_presets.cpp` pin the independence-weighted scoring, the
  fraud-detection posture (denied-channel suspicion, blind-while-present →
  Anomaly, uncorroborated silent-body dwell → Anomaly), the debounce/dwell FSM,
  and the preset→`FusionConfig`→engine pipeline. 77 checks green under
  `-Wall -Wextra -Werror`, wired into the existing host-tests job.
- **One brain, three cost tiers, five presets.** Lite (C3, PIR+RF+BLE+light —
  honestly labeled as evadable by a slow device-free intruder), Standard
  (C6+MR60, +radar+CSI — the front-door recommendation), Heavy (dual-board,
  +contact+tamper+vision — the rigged demo). Presets (`door`/`window`/`hallway`/
  `mailbox-lite`/`perimeter-demo`) are pure config data; behavior never forks
  by preset, only the numbers do. Every channel is compile-time selectable and
  runtime-weightable — fully modular.
- Board pin maps (`xiao-esp32c6-sentinel`, `xiao-esp32c3-sentinel-lite`), build
  envs (`envs/platformio/canary-sentinel.ini`), and the full spec
  (`docs/canary_sentinel_fusion_design.md`, incl. the evasion threat model and
  the ATM/fraud-detection lineage). The on-device witness/MQTT/OTA path is
  Phase 1; the project is not yet in `flavors.json` (phases in like canary-sense).

### canary-display — mic detection presets, grounded in the standards

- **The cadence windows are now derived from the alarm standards, not
  guessed.** T3 = 0.5 s ±10% beeps (ISO 8201 / ANSI S3.41 / NFPA 72); T4 =
  four 100 ms ±10 ms pulses + a 5 s ±0.5 s pause (UL 2034). The beep-duration
  windows (T3 350–800 ms, T4 60–300 ms) stay disjoint so a miscount can't
  cross-classify; the group-gap (900 ms) sits between T3's intra-beep and
  inter-cycle gaps; the streak-reset (12 s) is proven to exceed T4's 5 s pause
  (or a standing CO alarm would reset its own streak). Each constant carries
  its derivation in a comment and is host-tested against the source timing.
- **Level detection is now noise-floor-relative, so it's gain-independent.**
  A UL alarm is ≥ 85 dBA @ 10 ft (≥ 79 dBA low-freq) — +14 dB over the worst
  household ambient, +20–40 dB typically — so thresholds set as "N dB over the
  tracked floor" are correct without knowing the ES7210's absolute gain (the
  one thing the bench hasn't pinned). The envelope tracks the room with an
  asymmetric follower (rises slowly toward loud, falls fast toward quiet) so a
  standing alarm can never inflate the bar it must clear.
- **Three sensitivity presets** (Settings → microphone → sensitivity,
  NVS-persisted): **quiet** (bedroom, +9/+5 dB — catches a faint alarm),
  **standard** (default, +10/+6 dB), **noisy** (kitchen/workshop, +13/+8 dB —
  ignores clatter). The dB margins are gain-independent physics; only the
  dead-silence clamp is a soft bench anchor that fails safe when low. The
  `MIC1 SNAP` line now prints `floor`/`on`/`off` live as the bench's one-glance
  calibration readout, and `HELLO`/`SNAP` report the active preset.
- Nine new/updated mic-core host tests: adaptive floor tracks ambient and
  freezes a beep out, self-calibration (the same RMS reads as a beep in
  silence but as "the room" in a loud kitchen), the preset sensitivity ladder
  is monotone, plus the standards-window and type-switch pins. Mic doc gains
  the "how loud is a beep" section; usability protocol + serial appendix note
  the new SNAP fields.

### canary-display — mic soundness pass (a fails-safe becomes a works-right)

- **The acoustic cadence detector now runs on the audio clock, not the
  main-loop clock.** The runtime drained a batch of buffered 20 ms frames
  each `mic_loop`, but stamped every frame in the batch with the single
  `millis()` value passed in — so whenever the loop ran slower than the
  frame rate (i.e. always, once the 800×480 glass is rendering), a beep
  group collapsed onto one instant, every beep measured ~0 ms, the T3/T4
  duration windows rejected it, and a real smoke/CO alarm went
  **undetected**. `read_frame_rms` now returns each frame's own duration
  and the loop advances a frame-counted clock by it (re-anchored if the
  stream ever falls >1 s behind); the detector is fed that. The old bug
  failed safe (missed alarms, never phantom ones) — a new host test
  (`test_collapsed_timing_fails_safe`) pins that safety property at the
  core so it can't ever regress into a false positive.
- **DMA depth doubled to 160 ms** (`dma_buf_count` 4→8) so a UI stall
  shorter than that clips no beep; **each listening session resets the
  envelope + cadence state to silence** so re-arming next to a sounding
  alarm inherits no stale half-group; and the **"first detection is
  immediate" property now re-applies per session** (the 30 s re-raise
  throttle is scoped between events, and a fresh arm clears it).
- Four new mic-core host tests beyond the timing one: switching alarm type
  needs a fresh two cycles, the count/duration windows are disjoint (a
  miscount can't cross-classify T3↔T4), and confidence grows monotonically
  and caps at 95. Mic doc's privacy-barrier section gains the audio-clock
  note.

### canary-display — the usability protocol (testing the promises on real people)

- New `docs/hardware/display_usability_protocol.md` — the post-flash
  counterpart to the bench runbooks: every product claim rewritten as a
  think-aloud task a stranger either completes or doesn't. First light
  with no phone in the loop; glance comprehension at 2 m; half-asleep
  hold-to-ack + per-witness mute; night manners; settings without a
  manual; the mode gears (including the strand-in-a-gear check on the 3 s
  exit); the demo storyline narration; proof-on-glass; and the 4.3C's
  **"is it listening?" battery (task H)** — where the two safety-critical
  comprehension probes allow no assisted passes at all. Scoring
  thresholds, a results ledger (failures filed with VERIFY-note honesty),
  and a serial-grammar appendix (PG1/DM1/DBG1/ARC1/MIC1 in one table).
- 4.3C power truth folded in (vendor-documented): the CS8501
  charge/boost chip (~580 mA, single-cell 3.7 V → 5 V), the PWR/CHG/DONE
  LEDs, and the **side switch = battery connect/disconnect** (USB powers
  the board in either position; CHG-blink + DONE-lit with no pack is
  normal). New POWER & BATTERY section in the 4.3C pin map +
  "Power, the battery, and the side switch" in its README. `HAS_BATTERY`
  stays 0 with the reason upgraded from "no charge silicon confirmed" to
  "charging is hardware-managed; the firmware has no battery view" —
  `BATTERY_PIN_ADC -1` (VERIFY) records the sense-pin conflict (series
  demo says ADC1 ch3 = GPIO4, which this map carries as touch INT) that
  only the schematic can arbitrate. The outage-witness follow-up is
  staged in the README, not started.
- Polish/honesty fixes found writing it: `mic_pins_ok()` now computes
  from the pin map rather than gate state, so debug mode's System page
  reports the board truthfully even though the gears never initialize the
  mic (and "the gears never listen" is now stated in the mic contract);
  the 4.3C README gains the dark-glass survival note — a dark panel is
  the ST7701 init follow-up, not a brick, and the serial console + the
  device's own web mirror remain the working screens.

### canary-display — the mic-bearing dash (Waveshare 4.3C) and the listening contract

- **First-class support for the one display SKU that physically carries
  microphones** — the 4.3C "AI voice" (dual-MIC array behind an ES7210,
  ES8311 alongside), previously documented only as "unsuitable". Handled as
  a **distinct privacy surface** (the Sense-Wellbeing rule): its own board
  map (`boards/waveshare-esp32s3-lcd43c`, registered compile-tested), its
  own env (`canary-display-dash-mic`, in flavors.json so CI builds it), and
  its own reserved OTA product — never cross-installed with the mic-free
  dashes, which keep their unchanged "never microphone" promise.
- **What the mics do — and only do:** `FEATURE_MIC_ALARM` hears the two
  regulated alarm grammars (smoke **T3**, CO **T4**), each requiring two
  consecutive on-grammar cycles, raising `acoustic_smoke_alarm` /
  `acoustic_co_alarm` as UNSIGNED local events (Sev::Alert). The pipeline
  is the WAP's privacy-barrier design as a pure core
  (`canary/io/mic_logic.h`): one RMS scalar per ~20 ms frame, buffer zeroed
  before the read returns, booleans-and-milliseconds downstream — no
  spectra, no models, no recording, no streaming. Host-tested
  (`test_mic_logic.cpp`, CI step + Makefile): grammar windows, two-cycle
  rule, doorbell/speech/stale-streak rejection, envelope hysteresis.
- **You always know if it's on — one bit, three surfaces:** the amber
  ● MIC chip is lit exactly while the I2S driver is installed (the same
  gate action does both; the host test proves no
  listening-without-chip state is representable); Settings → microphone
  says `off / listening / pins unset` in words with the contract as the
  page caption; the `MIC1` serial grammar heartbeats 1 Hz only while
  capturing. **Off is the default and off is real**: arming is on-glass
  NVS opt-in only (no remote path), disarm = `i2s_driver_uninstall` (pins
  released — the verifiable hard mute), and the shipped audio pins are
  **-1 (VERIFY)** so the mics are provably un-driven until the bench fills
  them from the vendor schematic (`feature_sanity` refuses the flag on any
  board that never declared `HAS_MICROPHONE`). The dash transparency sheet
  renders the live mic state instead of a stale promise.
- Docs: `docs/hardware/display_mic_variant.md` (the listening contract +
  status ledger), board README with the bench session, hardware index row,
  sibling-note updates in the 4.3 map.

### flasher — detection-led firmware selection (families + smartPick)

- **The picker now scales to many boards and flavors without intimidating
  anyone** — the Arduino-IDE lesson inverted: everything the flasher can
  READ narrows the choice, and the human only answers what silicon can't.
  New pure selection ladder `smartPick` (`flash-core.js`, 13 tests in
  `tests/flash_select.test.js`): a `?product=` ask wins outright; else the
  app descriptor already on the board ("This board already runs X —
  installing keeps it, updated in place"); else the chip **plus the
  measured flash size** — an ESP32-S3 with 16 MB flash can only be the
  Waveshare panel module, with 8 MB it's the XIAO class — with the evidence
  stated on the page; else the authored per-chip default. Where size
  genuinely can't distinguish (both Vision C3 boards are 4 MB) the picker
  claims nothing — honesty over cleverness.
- **The family layer:** `flash.json` gains `families` (five stories:
  Canary / WAP / Vision / Sense / Display) and per-product `family`,
  `board_label`, `flash_mb` (derived from the firmware's board settings via
  the new `BOARD_FLASH_MB` single-source table) and `pick_label`. The
  "show all (developer)" browse renders grouped under family headers, each
  multi-variant family asking one plain-language question ("Which glass is
  in your hands?"). The generator refuses a family with no products, a
  variant family with no question, or a product with no answer; adding a
  board or flavor is one PRODUCTS entry + one family membership.
- Docs: `browser_flasher.md` § How the picker chooses. All flasher suites
  green (81 + 13).

### flasher — the display family joins the release train (watch / dash / dash-modes)
### flasher — the modes multi-tool joins the display release train
- **`securacv-canary-display-dash-modes` is flashable** alongside the
  watch and dash cards that already ride the train: the 4.3B multi-tool
  that boots the fleet face and carries the bench / demo / debug / arcade
  gears behind Settings → modes. Built like its siblings — an arduino-cli
  `--profile modes` build (the sketch's committed profile pins the
  Waveshare 4.3B FQBN), a signed per-product manifest, an entry in
  `build_flash_manifest.py`'s BUILD map, and a `gen_flash.py` product row
  regenerated into `flash.json`.
- **Distinct OTA/flash product, on purpose.** A modes unit must never
  cross-grade to the plain-dash image — that would silently strip the
  gears. Same rule that keeps a watch image off a dash.
- **Display-specific hatch moments:** the join-QR first light, plus a
  "with gears" variant (`display-modes`) that points at the modes doorway.
  The displays' emulator cards now cross-link their flashable product
  (`flash_product`); the flash button still lights only when a release's
  `manifest-flash.json` actually offers the image. Flasher docs gain
  §The display family (`docs/browser_flasher.md`); the modes spec's Waves
  ledger gains wave 7.

### GPS/GNSS — RMC status-flag fix + GPS-derived system clock

- **RMC 'V' (void) sentences no longer trusted as fixes.** Both NMEA parsers
  (`firmware/canary/lib/securacv_gps` and the canary-wap sketch's inline
  parser) parsed the RMC status field into a local `status` variable and then
  never read it — a void sentence (no fix, or a warm-up sentence some
  receivers emit before ephemeris loads) still updated speed, course, time,
  and date, and still flipped the UTC time to `valid = true`. Both parsers now
  gate all of that on `status == 'A'`.
- **New `firmware/common/gnss/gnss_time.h`.** Pure, header-only NMEA UTC
  date/time -> validated Unix epoch conversion: `gnss_calendar_valid()`
  rejects out-of-range fields (a corrupt-but-checksum-valid sentence, or a
  receiver emitting all-zeros before it has ephemeris) with a leap-year-aware
  day-of-month check and a century-wide trust window; `days_from_civil()` is
  Howard Hinnant's exact civil-calendar epoch algorithm, used instead of
  `mktime`/`timegm` (unreliable across ESP32 Arduino cores, and TZ-sensitive).
  Host-tested in `firmware/projects/canary-wap/tests_host/test_gnss_time.cpp`
  against known reference epochs and the validity guardrails. Staged into the
  canary-wap sketch (`arduino/canary_wap/gnss_time.h`) following the existing
  device_pseudonym/witness_store single-source pattern, with a
  `check_csi_sync.sh` drift guard.
- **GPS now actually sets the system clock.** Neither `firmware/canary` nor
  canary-wap has an SNTP path — the UTC time GPS already parsed was surfaced
  in diagnostics but never fed to `settimeofday()`, so `time(nullptr)` never
  left its post-boot state. That silently dead-ended wall-clock-gated
  features already in the code (canary-wap's NFPA-72 monthly self-test
  chirp's waking-hours check and 30-day NVS-persisted cadence both gate on
  `time(nullptr) >= 1700000000`, a condition that could never become true).
  Both trees now seed/correct the system clock from a validated GPS fix when
  one is available — floored against the same "clock looks unset" epoch,
  re-checked periodically to correct crystal drift, and only stepping the
  clock when it disagrees by more than a second so a synced clock isn't
  re-written every cycle.### canary-display — the gears turn: full mode runtime + the browser twin

- **The mode system is now end-to-end firmware** (Built · compile-gated ·
  bench-pending — `docs/hardware/display_modes.md` §Waves). The glue
  (`src/mode/mode_glue.cpp`) resolves every boot through the host-tested
  registry (NVS `mode` token, legacy `devmode` migration, fail-safe to the
  fleet face) and dispatches the gears this build carries; `main.cpp`'s
  bench-only branch is generalized to it, and the playground's 3 s exit now
  clears both latches via the registry's uniform `mode_exit_to_fleet()`.
- **Three new gears, each an empty TU without its flag** (default builds and
  the emulator stay byte-identical):
  - `FEATURE_DEMO_MODE` (`src/mode/demo_mode.cpp`, both flavors): the
    host-pinned storyline feeds the REAL `FleetModel` and renders the REAL
    faces under an unmissable DEMO chip on LVGL's top layer; tap = face
    nav, long-press = the ack demo, tamper beats raise/clear the witness
    tamper flag; `DM1` serial. Badges deliberately stay honest — synthetic
    events carry no signature, the verified ✓ is never faked.
  - `FEATURE_DEBUG_MODE` (`src/mode/debug_mode.cpp`, both flavors): five
    tap-to-page diagnostic faces (system+link / fleet raw / events / touch
    crosshair / I²C census). The one non-fleet gear with the network UP —
    WiFi comes up non-blocking (a dead AP is a finding, never a reboot
    loop), the broker is attempted on a bounded 5 s cadence so the retained
    fleet repopulates the raw table; SSID on the glass, the password never;
    `DBG1` 1 Hz snapshots.
  - `FEATURE_ARCADE` (`src/mode/arcade_mode.cpp`, dash): Canary Catch on
    the new pure core `canary/mode/arcade_logic.h` (host-tested,
    `test_arcade_logic.cpp` + CI step) — a seeded shuffle that visits every
    touch zone exactly once and replays from its printed seed, in-cell
    target placement, and a PASS/FAIL verdict that fails on a dead zone, a
    slow zone, or a stray tap. The score screen is the factory report;
    `ARC1` serial.
- **The Settings doorway grew up:** the "dev mode" row becomes a **modes**
  list (bench / demo / debug / arcade, each confirm-gated through
  `mode_request()`); a bench-only build keeps the familiar row verbatim.
- **CI coverage without touching any default build:** new
  `canary-display-dash-modes` (all four gears, 4.3B pins) and
  `canary-display-watch-modes` (demo + debug) envs in `flavors.json`;
  `feature_sanity.h` gains the display+touch contract for the new flags
  (+ test cases); `./setup.sh arduino modes` + a `modes` sketch profile
  stage the Arduino twin.
- **The browser twin ("emulator" for the mode system):**
  `canary-local/assets/mode-sim.js` — a DOM-free port of the registry,
  latch semantics, and demo storyline — drift-locked by
  `canary-local/tests/mode.test.js` (10 tests, wired into canary-local CI)
  against the committed Arduino mirror, playground-style. The twin is ready
  to be carried to the website as **`/modes`** (the five gears, the policy
  matrix rendered from the sim, a live latch simulator, and the storyline
  player) — the website page itself is a pending follow-up in the
  securacv_website repo.
- **Polish: the storyline reaches the real firmware in the browser.** The
  canary-local emulator page grows a **"play the demo storyline"** control:
  the same drift-locked beats stream through the page's staged witnesses
  (`witnessEvent`/`witnessTamper` — real Ed25519-signed chains where the
  browser supports it) into the actual compiled wasm firmware, at 6×, with
  a coached note line ("hold the glass to acknowledge") — no dist rebuild,
  player state survives view rebuilds, and stopping never leaves a staged
  tamper standing. Demo mode gains its missing signature moment — the
  **hold-to-ack ring** now sweeps closed exactly as the fleet face does
  (fires during the hold, not on release) — plus believable RSSI/comfort
  seeding on the cast and an exit hint on the dash's DEMO chip; debug's
  System page states its own exit. And wave 6 became executable:
  `board_43b_activation_bench.md` **§6 "The mode system"** is the on-bench
  checklist (doorway/latch/migration, per-gear pass signals, exits) whose
  pass moves each gear from Built · compile-gated toward Driven.

### canary-display — the glass gets gears (mode architecture) + the 4.3B peripheral catalog

- **Mode system spec'd, registry implemented.** New
  `docs/hardware/display_modes.md` defines the five-gear mode architecture —
  fleet / bench (the existing playground) / demo / debug / arcade — with a
  no-bloat contract (a mode must be a tool with an operator story, reuse the
  product's organs, and ship as a host-tested pure core behind a default-off
  gate), a per-mode policy table (only fleet takes OTA; only fleet arms the
  watchdog; debug is the one non-fleet gear with the network up), and a
  uniform entry/exit choreography (Settings doorway in, 3 s long-press out).
  The registry core is implemented pure + host-tested
  (`include/canary/mode/mode_registry.h`, `tests_host/test_mode_registry.cpp`,
  CI step added): NVS `mode`-token round-trip, the build-capability gate, and
  boot resolution that **fails safe to the fleet face** on anything
  unknown/uncarried and **migrates the legacy `devmode` bool** (a unit
  latched under old firmware still lands in the bench it asked for). Zero
  behavior change to any existing build — runtime glue is wave 1.
- **Demo mode's storyline core, drift-locked to the real vocabulary.**
  `include/canary/mode/demo_script.h` — a four-canary synthetic cast
  (reserved `demo-` ids) and a ~2½-minute looping storyline meant to be fed
  through the REAL fleet model into the REAL faces. Host test
  (`test_demo_script.cpp`, CI step added) links `src/fleet/fleet_model.cpp`
  and pins every beat's intended severity to `classify_event()` — the demo
  can never tell a story the product vocabulary doesn't — plus timeline
  invariants (covers every severity tier, the alarm beat holds ≥ 15 s for
  the hold-to-ack demo, the loop resolves to all-quiet before wrapping) and
  wrap-safe fires-once-per-lap stepping.
- **The peripheral catalog.** New `docs/hardware/display_peripheral_catalog.md`
  — the curated what-plugs-into-the-4.3B ledger by wiring surface (isolated
  DI/DO, I²C, RS485/Modbus, CAN, radios): why each peripheral and what it's
  for (PIR/reed/break-beam/glass-break/panic on DI; siren, strobe, and the
  alert-gated **night-vision IR illuminator** on DO; lux/ToF/RTC/environment
  on I²C; zone-expansion DI modules and energy meters on RS485), a
  simplicity contract (≤ 3 screw-terminal wires, bench-provable, one
  paragraph of docs), an honest **"not this board"** section (cameras/RTSP
  stay out by promise — vision witnesses and the HA/viewer layer own them),
  and the ATM-security combination plays (two-technology confirmation,
  fail-loud NC loops, the bait asset, witnessing the utilities).

### supply-chain — signed build provenance on release artifacts

- **Every published firmware binary and browser-flasher factory image now
  carries SLSA build provenance.** `firmware-release.yml` signs an in-toto
  provenance attestation over every artifact it publishes — the firmware `.bin`s,
  the factory images it builds in the same run, the manifests, and the checksum
  file — via GitHub's OIDC identity (`actions/attest-build-provenance`), records
  it in the Sigstore **Rekor public transparency log**, and attaches an
  offline-verifiable `provenance-*.sigstore.jsonl` bundle to each release. Anyone
  can now confirm a download was built from the open source, in the open —
  `gh attestation verify <file> --repo kmay89/securaCV` — a *public* check that
  complements the device-checked Ed25519 OTA signature and `sha256sums.txt`. (The
  out-of-band `flasher-release.yml` rebuild path stays SHA-256 + same-origin: its
  tagged-firmware-plus-current-tooling build has no single source a provenance
  statement could honestly name.)
- **Honest about reproducibility.** New `docs/supply_chain_transparency.md`
  gives the verify recipes (online + air-gapped) and states plainly that full
  bit-for-bit reproducibility isn't guaranteed on the ESP32 toolchain yet —
  provenance is the guarantee we can make *and verify* today, with hermetic
  builds on the roadmap.

### canary-display — Canary Cards on the glass (radar witnesses get type-aware cards)

- **The display firmware learns the Canary Cards kinds.** A card-bearing
  witness (canary-sense) now renders its coarse claim vocabulary —
  presence/occupants/range band + breathing lock and P1 BPM — on the wall
  dash and the watch detail, instead of the generic Witness fallback that
  dropped every radar numeric on the floor. New pure-logic card model
  `include/canary/fleet/fleet_cards.h` (`build_cards()` → a fixed-capacity
  `CardSet`; `format_card_strip()` the text realization of the six card kinds),
  host-tested in `tests_host/test_fleet_cards.cpp` against the schema
  (`docs/standard/CANARY_CARDS.md`) **and** parity with the JS reference
  renderer (`canary-local/assets/canary-cards.js senseCards`) — same 11 cards,
  same order, same ids/kinds/privacy classes, same `null`-as-unknown and
  provably-`absent` BPM cards on a presence-only build.
- **The fleet model + MQTT dispatch carry the radar surface.** `Witness` gains
  a compact radar block (presence/occupants/range/radar_ok/frame_errors/lux/
  breath+heart BPM, a few bytes each) and `on_sense_state()`; `mqtt_mgr` parses
  the coarse vocabulary from the `securacv/<id>/state` payload — the presence/
  count/range strings, lux, and the P1-gated BPM numerics that were previously
  read past. Raw distance and per-target data never appear (the device
  coarsened them at its own privacy chokepoint).
- **Honest by construction.** `null` renders "—" (a stalled radar is *unknown*
  presence, not *no* presence); BPM entities compiled out of a build render as
  provably `absent`, never silently missing; the strip drops absent + unknown
  cards so a wall dash stays glanceable. The Arduino parity sketch is
  regenerated (`setup.sh regen`) and the sync guard stays green.

### USB onboarding — frictionless plug-in (START-HERE drive file + one-tap)

- **Injection-free zero-touch open.** The read-only drive now carries a
  `START-HERE.html` (+ Windows `.url` + macOS `.webloc`) at its root, generated
  by the firmware and pointing at the device's help URL. Plugging in and opening
  that file launches the help page with **no keystroke injection at all** — the
  frictionless default. Builders live in `usb_onboard_logic.h` (host-tested,
  allow-list-gated, `&`-escaped for well-formed HTML/XML).
- **One-tap launch.** A single physical **BOOT** press now opens the help page
  directly (the press is the consent) — no serial console needed. The console
  `u` announced-preview path still exists for choosing the OS launch method. The
  consent state machine and its tests were updated accordingly; typing still
  requires a physical press and never happens from the Off state.
- **Opt-in hands-off auto-open.** New default-off `USB_ONBOARD_AUTOLAUNCH` flag
  auto-fires the launch a few seconds after enumeration with no button — genuine
  HID auto-typing (BadUSB-shaped), documented as such and off by default.

### USB onboarding — "plug me in" (consented HID + read-only drive + guided recovery)

- **The anti-BadUSB keyboard.** On the opt-in `[env:usb-onboard]` (USB-OTG)
  build the Canary enumerates as a composite device — CDC console (unchanged),
  a HID keyboard, and the SD card read-only over MSC. The keyboard exists to
  open one page (`https://securacv.com/canary`) so a person who just plugged the
  device in lands somewhere that explains recovery and unsealing. It **never**
  types on its own: enumerating does nothing; the console `u` key announces the
  exact URL and arms a 15-second window; only a physical **BOOT** press then
  types. The payload is compile-time fixed and run-time allow-listed to the help
  origin, so even a corrupted device id can never become a typed command.
- **Trust model is host-tested, not asserted.** `firmware/common/usb/`
  `usb_onboard_logic.h` holds the consent state machine, the help-URL
  builder/sanitizer, and the keystroke-plan allow-list;
  `firmware/tests_host/test_usb_onboard_logic.cpp` pins the properties that make
  a self-typing keyboard safe (types only after announce+confirm; can only ever
  emit the allow-listed https help URL).
- **Guided recovery / unsealing.** New console keys `v` (recovery) and `k`
  (unseal) surface the existing SD-wins chain reconciliation and the off-device
  `unseal_snapshot.py` flow. Read-only MSC lets `/WITNESS`, `/HEALTH`, `/CHAIN`,
  `/VAULT` be browsed and copied off without an app.
- **Ships off.** `FEATURE_USB_ONBOARD=0` in every stock profile; the glue is not
  compiled unless the flag is on. Default builds/flashing are untouched. Phase 2
  (on-device HID/MSC validation) pending before any release profile enables it.
  Design: `docs/design/usb_onboard.md`.

### canary.local — the Sense Lab (the radar witness, placed right)

- **`senselab.html` — the MR60BHA2 placement + pipeline bench.** The radar
  accepts no configuration commands (verified against the vendor library
  and both ESPHome components), so the only knobs that exist are placement
  and host-side judgment — the Lab stages exactly that: drag a person
  through top-down/side views against the real 80°×80° sector, the live
  CS_* range bands and the 1.5 m vitals envelope, with a quality meter
  grounded in the vital-sign radar literature (U-curve optimum ≈ 0.7 m,
  orientation projection, interference penalties).
- **The pipeline, honest end to end.** Real wire bytes (SOF 0x01,
  big-endian header, XOR-inverted checksums) stream through line-for-line
  JS ports of the firmware's FrameParser and both FSMs — pinned in CI to
  the same behaviors `firmware/tests_host/test_mr60_uart.cpp` pins — across
  the privacy chokepoint (distance and BPM visibly read-and-dropped, phase
  waveform/point-cloud frames counted `unknown`), into Ed25519-signed
  events over the real v1 `sense` canonical.
- **Firmware fix the bench found: vitals lock was unreachable.**
  `VitalsFSM::tick()` treated every non-vitals frame as an invalid vitals
  observation, so the interleaved presence/count/distance traffic of the
  real wire reset the lock-confirm window forever. The FSM now data-guards
  non-vitals frames (loss stays deadline-driven; multi-person suppression
  stays immediate), with host + JS regression twins streaming the realistic
  frame mix.
- **Canary Cards (schema v1).** The standardized widget-card layer —
  one HA-discovery entity, one card, on every surface
  (`docs/standard/CANARY_CARDS.md`, reference renderer + validator in
  `assets/canary-cards.js`): binary/stat/band/sparkline/event/trust kinds,
  null-as-unknown, severity semantics, and provably-`absent` cards for
  entities compiled out of a build (the presence-only BPM story, rendered).
  The documented on-ramp for canary-display's dash/glance/mirror surfaces
  and the companion app to stop rebuilding per-peripheral UI.
- **On the glass.** The Lab stages its witness on the real canary-display
  firmware (wasm) through the display's own MQTT dispatcher — the honest
  gap (generic Witness rendering today, Canary Cards tomorrow) bannered.
- **The power lab.** Rails calibrated to Seeed's published 0.5/0.8 W kit
  envelope; levers (modem sleep, heartbeat cadence, LED, lux) price every
  signed claim in joules and show the sensing share of the heat budget.
- **Deep hardware dossier.** `docs/hardware/mr60bha2_radar_notes.md`:
  the ADT6101P all the way down, the wire beyond what we decode, six new
  bench flags (incl. a distance-unit conflict with ESPHome's cm reading),
  placement physics with citations, and the power derivation — the
  `SIM:` tables are the drift-gated source for the Lab's physics/power.
- **Honest data path, pinned.** `tools/gen_senselab.py` parses the firmware +
  notes doc into `devices/senselab.json` (sys.exit on drift, CI-gated);
  `tests/senselab.test.js` re-derives every fact and exercises the DOM-free
  cores; `tests/senselab_probe.mjs` drives the page in headless Chromium
  (stream → signed event → breathing lock → flavor flip → stall path).

### canary.local — the physical test bench (the layer the firmware can't see)

- **The Bench tab.** Display sheets gain a full physical test bench:
  the USB-C cable, the battery (with live state-of-charge, charging,
  and brownout-at-empty), the board's ON/OFF slide switch, and the
  BOOT/RESET buttons — all interactable, all driving the live wasm
  firmware through the same boundary the power rail drives silicon.
  Pull the cable mid-frame and the glass dies (or rides the battery and
  the firmware never notices); NVS is flash, so everything learned
  survives every power event.
- **The hardwired lights, honestly.** PWR/CHG/DONE on the dash and the
  XIAO's CHG (including its documented no-battery flicker) + the unused
  USER LED on the watch are modeled on the wires they actually hang off
  — rail and charge chip. The bench's answer to "can firmware turn them
  off?" is the true one: no, here or on your desk.
- **Straps behave like straps.** BOOT is sampled only at reset: held
  through RESET (or even a software restart) it parks the mask ROM in
  download mode — screen dead, rail up, serial saying `waiting for
  download` — and a plain RESET recovers. Rail transitions print the
  ESP32-S3 ROM's verbatim reset banners, staged and labeled as such
  (the ROM is the one program that can't compile to wasm); everything
  after the banner is the real firmware.
- **Debug mode.** A live diagnostics pane (power source, boot stage,
  uptime, backlight duty, framebuffer flushes, link state, MQTT
  session, NVS key count) plus a symptom-first bench troubleshooting
  curriculum (`BENCH_FIXES`): always-on lights, flickering CHG, dark
  screen with PWR lit, download mode, dead-on-unplug, self-reboots,
  "BOOT does nothing".
- **Honest data path, pinned.** Per-board bench hardware facts live in
  the device registry's new `bench` block (never in code);
  `tests/bench.test.js` pins the power truth table (rail up ⇔ USB ∨
  battery∧switch∧charge), the strap semantics, the LED wiring, and the
  ROM banner text; `tests/bench_probe.mjs` drives the real page's
  Bench tab in headless Chromium through the whole loop (ride-through,
  rail death, restore, download mode) in CI.

### canary.local polish — the rail is the ring

- **Style chips wear their Characters.** Each chip in the lab's style
  rail now paints itself in its Character's own ground, ink, and accent
  — read from the firmware table via a new `emu_character_color`
  binding (backed by a clamped `character_def()` accessor in the
  display firmware). The selection ring glows in the chip's accent.
  The rail stopped describing the ring and started being it.
- **The tour lands on the invitation.** A closing step points the
  visitor at Try it: flip all seven looks, meet the bird for the first
  time, break the household on purpose.

### canary.local — the lab wears the Character, meets the bird, shows the real parts

- **The Character ring is on the page.** The display sheets' Try-it view
  gains a style rail: all seven ages, names/captions/ring order read back
  from the wasm firmware itself (never hardcoded), applied through the
  same knob the on-glass picker turns — the choice even persists through
  emulated reboots via the real debounced settings commit. A tour step
  dresses the glass in Heirloom mid-walkthrough.
- **"Meet the bird again."** One button reboots the emulated display as a
  true first boot: the hop in, the head-tilt, the typed introduction and
  the privacy promise — the first-meeting splash, previously unreachable
  on the page, now the demo's opening act. (Emulator shells learned
  `retire()` so a replaced module can never draw over its successor.)
- **Every 3D card is the real part.** Device cards upgrade from
  procedural approximations to the actual geometry: the displays load the
  OpenSCAD-rendered preview meshes (drum/bezel/stand, frame/back/stand)
  and the witnesses load their print-validated shells from
  `docs/hardware/enclosure/` — assembled from their own bounding boxes,
  with the live firmware still texturing the glass. Approximations remain
  only as an offline fallback.
- Emulator bindings: `emu_apply_character` + ring/name/caption reads
  (pointer-return strings decoded page-side — cwrap's own string path can
  return a never-settling promise under ASYNCIFY); `UTF8ToString` joins
  the exported runtime methods.

### Display Character wave 4 — Terminal, Blueprint, and a mirror that dresses to match

- **Two more ages on the ring**
  ([`docs/hardware/display_character.md`](docs/hardware/display_character.md)
  §6): **Terminal** — aged phosphor on green-black (AAA), cursor-phosphor
  accent, machine-steady temperament, console voice ("All nominal" /
  "back online") — and **Blueprint** — white ink on Prussian blue (AAA),
  chalk-cyan accent, careful-drafter temperament, drafting voice
  ("All to plan" / "as you were"). Both dark grounds carry the canonical
  semantic bytes (measured ≥3:1 on both tiers); the ring is now seven
  ages, and a compile-time static_assert refuses an age that doesn't
  take a ring seat.
- **The phone mirror wears your Character.** `/api/glass` carries the
  active look's day palette and semantic set; the mirror re-skins its
  page live (ground, tiers, severity hues), with older mirror HTML
  ignoring the field harmlessly and the mirror's warm-dim night
  emulation unchanged on top.

### Display Character wave 3 — Almanac, the paper Character

- **The ring's first light ground**
  ([`docs/hardware/display_character.md`](docs/hardware/display_character.md)
  §6): **Almanac** — warm paper `#F2EAD8`, warm ink (12.8:1, AAA),
  fountain-pen indigo accent, bookish-calm temperament, almanac
  weather-speak voice ("All calm" / "good day"). The age of print.
- **Semantics hold their meaning on paper.** The canonical timeline hues
  fail measured contrast on a light field (amber is 1.98:1 — an invisible
  alarm color is the dishonest option), so the semantic accessors became
  Character-served at the theme choke point: canonical bytes on every
  dark ground, darkened-**within-family** stops on Almanac (ok `#276B2B`,
  warn `#8F5300`, alert `#B71C1C`, signed `#01579B` — each ≥4.5:1 on both
  paper tiers). Re-hueing stays impossible by construction.
- **Night is now uniform, not assumed.** `character_set_night()` (set by
  the render tick from the quiet-hours mode) makes the ground/tier
  accessors serve the dark Quiet Glass set for *every* Character — a
  cream glass structurally cannot glow in a bedroom, and decorative
  accents go ember-dim after dark (never blue).

### Display Character wave 2 — the Voice

- **Each Character now speaks its register** — in the ambient copy only
  ([`docs/hardware/display_character.md`](docs/hardware/display_character.md)
  §9). Three slots per Character: the calm-fleet hero word ("All quiet" /
  "All is well" / "All clear" / "All good"), its inline form under the
  time hero, and the returning-boot greeting ("hello again" /
  "welcome home" / "welcome back" / "hey again"). The honesty line is
  structural: severity words, badge text, link labels, event copy, and
  every degraded-state instruction never come from the Voice table — a
  Character may rephrase contentment, never trouble. The first-meeting
  splash script stays canonical; only the *returning* greeting takes the
  register.
- **The phone mirror speaks in the wall's voice.** The active calm words
  ride the `/api/glass` snapshot (`aq`/`aql`), so mirror and glass can
  never disagree on register; trouble words the mirror still derives from
  the invariant severity vocabulary.

### Display Character wave — choose how the glass feels, without choosing wrong

- **Four curated era looks, one honest system**
  ([`docs/hardware/display_character.md`](docs/hardware/display_character.md)).
  A Character bundles ground/chrome palette, type feel, and the bird's
  temperament into one named, pre-validated look: **Heirloom** (warm
  charcoal + brass, one type size up per role, slow sparing bird — the
  large-print hallway edition, AAA contrast), **Quiet Glass** (the default —
  byte-for-byte today's look), **Aqua** (millennium blue-black gloss), and
  **Neon** (vivid, quick, springy). Curated flip-through, not a color
  picker: every stop is a decision already made correctly, so there is no
  wrong answer to reach (the Apple-watch-face model; anti-choice-anxiety
  by design).
- **Two lines no Character may cross.** Semantic hues stay at
  timeline-card parity (a state is the same color on the wall and in the
  app — a Character restyles the room, never the alarms), and night stays
  sacred: the red-shifted, melatonin-band-free night palette belongs to
  the night engine and outranks every look. Enforced at the `theme.h`
  choke point — semantics and `ncol_*` remain compile-time constants.
- **The bird has a temperament** (`canary_mark_temperament()`): three
  clamped scalars — breath rate, flourish cadence, hop energy — layered
  UNDER the mood engine, Kismet-style. A worried bird is worried in every
  Character; Neon just gets there a beat quicker. The mood engine
  (`bird_mood.h`, pure/host-tested) is untouched.
- **The picker** is one more One-Screen-One-Decision editor
  (settings › style): name, era caption, ring dots, flip ‹ ›. The screen
  is the preview taken literally — each flip restyles the open settings
  surface live, applies + persists immediately (landing IS choosing), and
  reset comes home to Quiet Glass.
- **Settings blob v1 → v2 with a real migration** (Codex review catch on
  #904): the loader recognizes the frozen v1 layout and copies it
  field-for-field — night hours, glow, peek, and brightness all survive
  the upgrade; only the new `character` field defaults. The blob rewrites
  as v2 through the existing debounced commit; reject-to-defaults stays
  reserved for genuinely corrupt blobs.
- **CI: the review-threads gate absorbs the reply→resolve race.** GitHub
  Actions has no thread-resolution trigger (App-webhook only — verified
  against the docs; the header's original claim was right), so the gate
  now holds its verdict through a grace window on reply/review events
  instead of instantly grading the stale in-between state; silent
  resolves keep the documented manual re-run.

### Seal / unseal / vault UX pass — checked, plain-spoken, and rewarding

- **Evidence Viewer now reveals its verification check-by-check.** After a
  bundle is verified (verification still runs to completion first — nothing
  shown is speculative), each real result slides in with a drawn tick, a
  live "Check 3 of 7" counter, and a progress bar; the panel settles into
  the permanent "What was verified" record. A failing bundle shows the
  passed checks, then the failing one in red with a plain-language headline
  keyed to the structured failure kind. Reduced-motion users get identical
  content instantly.
- **Dual-register check copy.** Every check row leads with plain language
  ("Every event is chained to the one before it — nothing added, removed,
  or reordered") with the verifier's exact technical claim one line below,
  behind a "Show technical detail" toggle (always printed in the saved
  report). Presentation-only: `verify_core.js` and its pinned strings are
  untouched.
- **Break-glass console gets a four-step tracker, an up-front "what
  happens when you break the glass" reassurance panel, and a granted state
  that explains the receipt.** Each step carries a one-sentence simple
  explanation plus a labeled Technical line; quorum-reached and unseal
  outcomes now say what was verified and that the decision (granted or
  denied) was itself recorded as a signed receipt. Trustee share-link page
  copy clarified likewise. Console behavior, endpoints, and the single
  script block its tests extract are unchanged (`breakglass.test.js` still
  green).

### First-boot install-path audit (HA OS + Raspberry Pi)

- **Frigate config location corrected everywhere.** The Frigate add-on
  (0.16+) reads `/addon_configs/ccab4aaf_frigate/config.yml`, not
  `/config/frigate.yml`. README install steps, `docs/frigate_integration.md`,
  and `scripts/install.sh` now say so; the install script additionally seeds
  Frigate's real config directory with the generated template when visible.
  (The add-on wizard still writes only the `/config/frigate.yml` template —
  wiring it to Frigate's config dir needs an `all_addon_configs` mapping and
  is left as a code follow-up.)
- **`scripts/install.sh` slugs fixed**: `ccab4aaf_frigate` and
  `d0491a67_privacy_witness_kernel` (Supervisor slugs are
  `sha1(repo-url)[:8]_<addon>`); the previous values could never resolve.
  Removed calls to `ha addons repository add` / `ha addons repositories` —
  the ha CLI has no repository commands, so the script now falls back to
  clear manual instructions instead of silently failing.
- **README HA install steps** now include the Mosquitto add-on and the
  Frigate add-on repository as explicit prerequisites.

### Repo legibility pass (structural only, no functional changes)

- Root doc files relocated into `docs/` (`log_verify_README.md` ->
  `docs/log_verify.md`, `why_this_matters.md`, `securaCV_whitepaper.md`,
  `v1-roadmap.md`); superseded root `spec.md` pointer retired in favor of
  `spec/README.md`; committed test-output dump `.tmp/t.txt` removed.
- `.gitattributes` added: generated firmware trees (Canary Display Arduino
  parity sketch, gzipped web-asset header) excluded from GitHub language
  stats; `vendor/**` marked vendored.
- README dieted to ~190 lines by linking out (Docker sidecar quickstart,
  hardware table, trust-roots explainer now live in their docs pages);
  firmware section lists all five Canary projects with the boards the
  PlatformIO envs actually target.
- `docs/homeassistant_setup.md` gains transport and per-tamper-type
  catalogs with honest Implemented / Experimental / Planned status per row;
  the transport catalog separates transport status from the per-transport
  health entities, which are pending a firmware publisher
  (`mqtt_publish_transport()` is defined but never called).

## [2.3.2] - 2026-07-27

### The nightstand wakes up canary yellow

The 1.47" nightstand boards' behind-glass WS2812 beacon (GPIO8 on the
ESP32-C6 board, GPIO38 on the S3 stick) lights bright canary yellow
(0xFFD44F) as the very first line of setup() — power-on is answered before
the panel initializes. A liveness signal only: the first render tick hands
the LED to the real fleet state, and the dark-when-safe night behavior is
unchanged.

### Wi-Fi preload is standard on every board

Both flashers can bake your network into any Canary at install time (each
firmware already read the same NVS namespace; the desktop app now writes
both encodings — string for sense/vision/display, blob + wifi_en for
canary/wap). Optional everywhere it isn't usb-secrets: skip it and the AP
portal / on-glass setup is untouched.

## [2.3.1] - 2026-07-27

### Every Canary Display ships in this release — including the Dash 7

fw-v2.3.0 published without the ESP32-S3 display images (watch, dash,
dash-modes, dash7, nightstand-s3): the Dash 7 and Nightstand S3 binaries
compiled successfully, but the isolated-core nightstand-c6 build that
follows them changed the PlatformIO project checksum, which silently
cleaned `.pio/build` and erased their outputs before the signing step —
the non-blocking display loop then dropped them from the release with
only a warning. Both release workflows now stage each display env's build
outputs the moment they exist and restore them after the C6 run, so the
packaging, signing, and factory-image steps see every board that built.

### The update channel is now a device setting, not a reflash

The pull-OTA engine understands two channels: **release** (the compiled-in
manifest URL riding `releases/latest`, guaranteed to be a signed stable
firmware release) and **dev** (the same product's manifest on the rolling
`fw-dev-latest` prerelease). The channel persists in NVS
(`securacv_ota_set_channel` / `securacv_ota_get_channel`); the dev URL is
derived from the compiled default by rewriting the one canonical release
segment, so every product gets both channels with no per-board
configuration — and a custom-server manifest override still wins outright.
Both channels verify the same Ed25519 signatures from the same release
key; switching back to release never downgrades (the anti-rollback floor
holds until the stable channel moves past the running build). The Canary
Display exposes the switch over MQTT — retained
`securacv/<id>/update/channel` ("release"/"dev"), commands on
`securacv/<id>/update/channel/cmd` — next to the existing install/auto
topics, and checks the new channel promptly after a switch. Host-tested
(`test_ota_logic.cpp`).

## [0.6.0] - Unreleased

**State summary.** The Frigate -> MQTT -> sealed-log pipeline is verified
end-to-end in CI (real-broker ingest test included); the kernel test suite
passes (458 tests). Five firmware projects build under
`firmware/projects/` (Vision C3, WAP S3 Sense, Sense C6 + MR60BHA2 radar,
Display S3, OTA S3/ESP-IDF). On-device hardware validation and the v1 tag
remain open (`docs/v1-roadmap.md`); everything below shipped since 0.5.0.

### security: re-derive the break-glass quorum at every verification point

A security review found the break-glass path trusted a receipt's recorded
`outcome: Granted` verbatim without ever recomputing the quorum. Because a
`BreakGlassReceipt` is device-signed and hash-chained, a holder of the
**device signing key alone** could forge `Granted` over an *empty* approval
set, append it through the normal signed path, mint a token, and unseal —
with zero genuine trustee approvals. That defeats Invariant V ("no single
actor/credential/process can unilaterally access sealed evidence"); the
device key is exactly the credential the quorum exists to render
insufficient.

- **H1 — quorum is now re-derived, never trusted.** Both the audit verifier
  (`verify_approvals_against_policy`, shared by
  `verify_break_glass_receipts_with` and the `receipts` CLI) and the runtime
  unseal/export gate (`break_glass_receipt_outcome_for_verifier`) now
  recompute the count of *distinct, valid, known-trustee* approvals against
  the configured `QuorumPolicy` and refuse any `Granted` receipt that does
  not meet `policy.n`. The runtime gate also checks the receipt's
  `approvals_commitment` so a swapped `approvals_json` is rejected before the
  count. New helper `count_valid_distinct_approvals` dedups on the public
  KEY. Tests: a forged empty-approvals `Granted` receipt is rejected at both
  the audit verifier and the unseal gate; a real quorum still passes.
- **M1 — duplicate trustee keys rejected.** `QuorumPolicy::validate` enforced
  id-uniqueness but not key-uniqueness, so one key-holder listed under two
  ids filled two quorum slots and satisfied a k-of-n quorum alone. It now
  rejects a public key reused across trustee entries.
- **L1 — request-hash field framing.** `UnlockRequest::request_hash` now
  length-prefixes the variable-length `envelope_id`/`purpose` (mirroring
  `token_signing_hash`) so no boundary-shifted `(envelope, purpose)` pair can
  collide.
- **L2 — residual key zeroization.** `seal_v2` scrubs the source DEK copy
  left in `DerivedDek` after wrapping it in the drop-guard, and the KEM
  shared secret is zeroized after DEK derivation on both the seal and decrypt
  paths.
- **R1 — receipts bind their policy era (review follow-up).** Re-deriving the
  quorum against the *mutable current* policy would false-positive historical
  `Granted` receipts after a legitimate policy rotation (raised threshold or
  changed trustee set). Each receipt now records a signed `policy_commitment`
  (`QuorumPolicy::commitment` — threshold + member count + sorted trustee
  id/pubkeys). The audit verifier skips the quorum re-derivation for a receipt
  whose commitment marks a different era (chain hash + device signature remain
  its tamper evidence), so a rotation no longer raises false integrity alarms;
  within the current era it re-derives in full. The runtime unseal gate instead
  fails **closed** on a commitment mismatch — a token backed by a prior-era
  receipt is refused rather than released against a rotated quorum. Old
  receipts without the field default to the current-era treatment.

### firmware (canary-wap): beacon-audit SD recovery now requires chain linkage

The beacon audit chain-head recovery adopted the last `"head":"…"` substring
from `/beacon/audit.jsonl` verbatim, with none of the guards the witness
recovery uses — a torn power-cut tail, a corrupt line, or a spliced fragment
could silently redirect the append-only chain. The beacon format has no
per-line sequence and its entries are peer-authored, so the witness recovery's
seq-ahead and device-key checks do not port; the portable guard is **chain
linkage**. Recovery now adopts the newest *complete* line's head only when its
`prev` matches the previous line's `head` (or genesis for a first record),
refusing an unlinkable tail and keeping the NVS head. Extracted the decision
into the pure, host-tested `beacon_audit_recover.h`
(`test_beacon_audit_recover.cpp`, wired into `firmware.yml`) and corrected the
`witness_store.h` comment that overstated parity between the two recoveries.
Per-beacon Ed25519 signatures remain the primary tamper-evidence for entry
contents. The boot read window is 4 KiB — the writer caps a line at 768 bytes,
and recovery needs a torn partial + the newest complete line + its predecessor
(and the predecessor's starting delimiter) all in view, or the window could
start inside the predecessor, find no verifiable predecessor, and keep a stale
NVS head — forking the log in exactly the stale-cache case the guard exists to
fix (review catch on this PR).

### kernel: chacha20poly1305 0.10 → 0.11 (vault AEAD, wire format unchanged)

Supersedes the Dependabot bump (#828), which could not merge because the
new `aead` 0.6 trait API is a source-breaking change in our vault crypto.
Migrated `src/vault/crypto.rs` to `AeadInOut::{encrypt,decrypt}_inout_detached`
with `TryFrom`/reference conversions for key/nonce/tag (same four cipher
sites, same fail-closed error mapping).

**The on-disk sealed-envelope format is provably unchanged:** a new
known-answer test (`aead_known_answer_rfc8439`) pins the exact
ciphertext+tag bytes for both the V1 and V2 AAD constructions against
goldens generated with an independent implementation (Python
`cryptography`, RFC 8439). The test was added and verified green on
0.10.1 BEFORE the bump, and passes identically on 0.11 — envelopes sealed
under 0.10.1 keep decrypting byte-for-byte. If that test ever fails after
a future bump, sealed evidence would no longer open; the goldens must
never be "fixed".

The crate's `zeroize` feature is enabled explicitly: 0.10.x scrubbed the
cipher's internal key copy on drop unconditionally, but 0.11 gates that
behind an off-by-default feature — without it, the bump would have
silently left DEK/master-key copies in process memory after each
seal/decrypt (review catch on this PR). Also picks up crossbeam-epoch
0.9.20 (lockfile-only) for RUSTSEC-2026-0204, a pre-existing transitive
advisory published 2026-07-06 that began failing `cargo audit` in CI.

### kernel: retire the legacy bare-hash signature fallback

`SignatureMode::Compat` used to fall back to verifying Ed25519 signatures
over the **bare entry hash** (the pre-domain-separation "v1" construction)
when the domain-separated check failed. That fallback shared an undomained
signature namespace with everything else — the same cross-context surface
the trustee-approval domain fix just closed — and there is no deployed
pre-domain-separation data that needs it.

- Removed the bare-hash fallback and the now-dead `verify_ed25519_legacy`.
  **All** Ed25519 verification now requires domain separation, in every
  mode; an undomained signature no longer verifies anywhere. New test
  `compat_mode_rejects_bare_hash_signature` pins it.
- **`Compat` now differs from `Strict` only in that the post-quantum
  signature is optional** — both require a domain-separated Ed25519
  signature. PQ posture is deliberately unchanged: this is *not* the
  "make PQ mandatory" change (that would force the `pqc-signatures`
  feature always-on and every signer to carry a PQ key — a separate PQC
  rollout decision), so `--no-default-features` builds still verify.
- No fixtures needed regeneration: the committed "legacy" envelope
  fixture is domain-separated (its "legacy" is the absent `auth_mode`
  receipt field, not a bare-hash signature) and still verifies.
  `docs/security/SECURITY-AUDIT.md` updated to state domain separation is
  now mandatory for all signature contexts.

### kernel: domain-separate trustee approvals (BREAKING for `.approval` artifacts)

Trustee quorum approvals (Invariant V) were the **one** kernel signature
context with no domain separation — they were signed over the bare
32-byte request hash, sharing an undomained namespace with the legacy
verify path. Any Ed25519 signature a trustee key produced over a bare
value was cryptographically interchangeable across contexts. This binds a
trustee's consent to its own domain:

- New `DOMAIN_TRUSTEE_APPROVAL = "securacv:pwk:trustee-approval:v2"`.
  Approvals are now signed and verified over
  `domain_separated_hash(DOMAIN_TRUSTEE_APPROVAL, request_hash)` via the
  same `sign_ed25519_only`/`verify_ed25519_only` helpers the break-glass
  token path uses. New `Approval::signed` / `sign_approval` /
  `verify_approval` centralize it; all signing/verifying sites (CLI,
  session, backend, HTTP) and the in-browser signer (`breakglass.html`,
  which now reproduces the kernel's domain-hash byte layout) move in
  lockstep.
- **Breaking:** any `.approval` artifact minted before this change no
  longer verifies — they were signed over the bare hash. Re-sign with the
  updated `break_glass approve` / trustee console. New tests pin that a
  bare-hash signature and a cross-context (break-glass-token domain)
  signature are both rejected as approvals, and vice versa.
- `docs/security/SECURITY-AUDIT.md` corrected — it previously claimed all
  Ed25519 contexts were domain-separated while omitting (the then-
  unseparated) trustee approvals. No protected spec edited; Invariant V
  (`spec/invariants.md`) motivates the change and is left untouched.

### canary-wap battery gates round two: CSI drain + MQTT heartbeat cadence

PR #847 enforced the first power-policy gates (camera, record interval,
CPU/WiFi-PS, deep sleep) but left several `PolicyFeatures` bits computed
and never read. This wires the two that are honest power wins:

- **CSI drain is now gated.** When the policy turns CSI off (battery
  saver and below) the main loop skips the CSI ring-drain and 1 Hz
  module dispatch, stopping the pipeline's per-loop work. CSI is pure
  environmental sensing (no life-safety), so this matches the profiles'
  intent; the ring fills and drops harmlessly while gated and resumes on
  re-enable with no re-init.
- **Routine MQTT heartbeats stretch under battery load.** The MQTT link
  is kept alive in every mode (LOW_POWER holds it up so panic events
  reach Home Assistant), so instead of skipping publishes the routine
  heartbeat cadence (status/health/mesh-snapshot/beacon) stretches ×4 in
  battery-saver and ×8 in low-power/shutdown. Life-safety (acoustic
  `/sensing`) and event-driven (record counts / chain head) publishes are
  never stretched.
- **Mesh servicing is deliberately kept always-on** (not gated off like
  the profiles' advisory bit suggested): `mesh_network::update` carries
  inter-canary security/tamper alerts, so dropping alert reception to
  save a little CPU is the wrong trade for a security device — only its
  routine MQTT snapshot cadence is stretched. `vision` has no subsystem
  in this sketch and stays advisory.

Decisions live in a pure, host-tested `power_gate_logic.h` (feature
run/skip + cadence stretch with overflow saturation); the enforcement
comment in `power_policy.h` and the battery guide are updated in lockstep
so the enforced-vs-advisory table stays honest.

### PIO canary: durable witness log on SD (/WITNESS/records.jsonl)

The PlatformIO canary (firmware/canary) signed every witness record but
never wrote one to the card — `sd_writes` was a counter with no write
behind it, and the `/WITNESS` "per-record files" that rotation,
verification, and export code operated on never existed. Ported the
canary-wap durable tier:

- **Every signed record now lands on SD** as one self-describing JSON
  line in the append-only `/WITNESS/records.jsonl` — the same byte-exact
  format canary-wap writes and `tools/verify_witness_log.py` proves
  offline (chain hashes + Ed25519 + gap/torn-tail honesty). The pure
  line/parse/reconcile logic is now a single canonical header
  (`firmware/common/witness/witness_store.h`) shared by both trees:
  the wap sketch carries a byte-identical staged copy (guarded by
  `check_csi_sync.sh`, staged by `setup.sh`), the PIO tree includes it
  directly, and the existing host suite runs against the canonical copy
  in CI.
- **Ordering is load-bearing and now correct:** the SD append happens
  BEFORE the periodic NVS persist, so a power cut leaves SD ahead —
  exactly the state the new signature-verified SD-wins boot recovery
  repairs (a foreign or tampered card can never move the chain head).
- **`/WITNESS` is never rotated** (Invariant IV) — the rotation pass now
  bounds only `/HEALTH`. Deleted two dead APIs that read a raw-struct
  record-file format that never existed on disk and would have misread
  the new log: `datamgmt_verify_chain` and `datamgmt_export_records`
  (zero callers; offline verification is `verify_witness_log.py`).
- **Boot recovery binds seq to the signature** (review hardening, both
  trees): the tail's Ed25519 signature covers only the chain hash, so
  recovery now recomputes that hash from the line's own fields
  (prev, ph, seq, tb) and requires a match before adopting — a tampered
  card keeping a genuine hash/signature pair while editing `seq` can no
  longer move the device sequence. Host test pins the tamper class.
- **`[env:full]` moves to the canonical `default_8MB` partition table**
  (per `firmware/PARTITIONS.md`): the durable log pushed FULL past 100%
  of the 1.9 MB `partitions_ota.csv` slot it was never meant to ship on.
  On the 3.2 MB slots FULL fits with ample headroom, and the old
  "self-update could never fit" rationale is gone — FULL regains the
  signed pull-OTA path. Switching tables requires a USB reflash, which
  is already how FULL is installed.

### Witness unification part 1: one canonical chain core, sense aligned, vision signs

Four witness-chain implementations had drifted apart. This lands the
shared core and brings every firmware onto the same construction:

- **`firmware/common/witness/witness_chain.h` is now THE canonical
  chain construction** — a small header-only core (domain strings, the
  72-byte big-endian chain pre-image builder, mbedTLS-backed
  `wc_chain_advance`/`wc_genesis`) instead of a dead never-implemented
  C API. A new host test pins the byte layout and hashes against
  python-hashlib goldens, so the firmwares and
  `tools/verify_witness_log.py` can no longer drift apart silently.
  The dead, never-buildable `firmware/projects/canary-wap/src/main.cpp`
  (sole consumer of the old phantom API) is deleted.
- **canary-sense adopts the canonical construction.** Its chain hash
  previously omitted the sequence number and time bucket (records could
  be renumbered/time-shifted without breaking links) and used a
  pubkey-derived genesis. It now uses `wc_chain_advance` (seq + bucket
  in the hash) and the canonical device-id genesis. Invisible to Home
  Assistant — HA verifies Ed25519 envelopes, never internal links —
  and upgraded devices simply continue forward from their stored head.
- **canary-vision becomes a signing witness.** Previously it published
  bare unsigned JSON with no device identity. It now carries the same
  Ed25519 identity module as canary-sense (NVS-persisted key, fail-closed),
  chains every presence/dwell event, signs the LOCKED sense canonical
  (its presence/occupants semantics fit; range is honestly `"unknown"`),
  and publishes retained `health` (with `public_key` for HA's TOFU
  pinning) and signed `chain` topics. Vision events now verify in HA
  through the existing `verify_sense_event` path with zero HA-side
  changes — a new HA test pins a vision-shaped payload end-to-end.

### canary-wap polish: crash-proof health logs, offline witness verifier, honest standby UX

Round two of the storage/camera audit — three finishing gaps closed:

- **Health logs now survive reboots.** `log_health()` was a 100-entry
  RAM ring; after a crash, the evidence of WHY was gone. Every entry now
  also lands in a per-boot SD file (`/HEALTH/boot_<n>.jsonl`) — the
  previous boot's file IS the crash forensic. Entries are staged into a
  small PSRAM ring from any task and drained by the loop task (the one
  SD-writer task), JSON-escaped because health details can carry
  peer-controlled bytes (mesh sender names). Missing card degrades to
  the RAM ring behind one latched warning; old boot files are bounded
  by the existing /HEALTH rotation. New host test pins the escaping and
  the format.
- **The sealed log is now provable offline.** `tools/verify_witness_log.py`
  re-verifies `/WITNESS/records.jsonl` from the card alone: recomputes
  every chain hash, checks every Ed25519 signature against the device
  public key, verifies segment continuity, and reports card-absent gaps
  and torn power-cut tails honestly instead of hiding them. Its test
  suite proves edited fields, wrong keys, re-signed lines, and
  reordering all fail loudly.
- **A parked camera no longer reads as broken.** Standby is now a
  first-class state: `/api/peek/status` reports `standby` and the gate
  reason, the self-test says "Asleep to save power — wakes when used"
  (PASS) instead of "Sensor offline" (FAIL), the dashboard's preview
  button stays usable (starting the preview wakes the sensor), and the
  thermal/battery gates show their own copy. The init-failure
  diagnostics card only appears for genuine failures. Panel state is a
  pure `WEBUI_LOGIC` function with five node tests.

### canary-wap camera: event triggers, idle standby, thermal shedding, real battery gating

The camera was "always ready, never watching": initialized once at boot
with its 20 MHz clock free-running forever, but only capturing on demand
(peek, QR, sealed vault) — and the only automatic triggers were the
acoustic alarms. Three changes get more out of it while making it run
cooler and last longer on battery:

- **Two new opt-in sealed-vault triggers.** "Motion (Wi-Fi sensing)"
  seals one encrypted frame the moment the presence engine confirms an
  arrival (the fused CSI+RF "rf_presence_started" transition — immediate,
  not the bundled witness commit) — the sensing radio is already
  listening, so this costs no extra power and works in the dark. "Mesh alarm" seals one frame when a paired Canary
  reports tamper / motion / breach (battery housekeeping alerts do not
  fire it). Both ride the existing write-only escrow: key-gated,
  cooldown-bounded, OFF by default, device can't decrypt its own
  captures. New `.svlt` trigger bytes 4/5; the unlock tool and both
  golden-fixture test suites updated in lockstep.
- **Idle standby + on-demand wake.** A camera unused for 5 minutes is
  parked via `esp_camera_deinit()` (XCLK stops, framebuffers freed —
  less idle current, less heat). Anything that needs a frame — peek,
  QR, a vault seal — wakes it (~1 s, mutex-guarded re-init through the
  existing boot ladder). Decisions are pure and host-tested
  (`camera_gate_logic.h` + `test_camera_gate_logic.cpp`).
- **Thermal protection with teeth.** At the existing 80 °C critical
  threshold the peek stream (the actual heat source) is stopped, new
  streams are refused with "Device is too hot", and the sensor parks
  until it cools. Vault captures stay allowed — one life-safety frame
  is worth more than the watt it costs.
- **The battery policy is now enforced for the camera, not just
  documented.** `PolicyFeatures.camera_peek` had zero call sites — the
  docs said "camera off on battery" while the firmware ignored it. On
  battery the peek endpoints now answer 503 with an honest reason and
  the sensor parks; the policy's record-interval also acts as a real
  floor on the witness record cadence. Vault and QR still work in every
  mode. The hardware guide and `power_policy.h` now state exactly which
  policy fields are enforced and which remain advisory (csi/mqtt/mesh/
  vision have no consumers yet — no more doc fiction).

### Audit remediation: witness records now durable on SD; break-glass tokens single-use across invocations

An end-to-end audit of the SD write paths, the device hash chain, the
vault locking/unlocking crypto, and the kernel quorum design found two
holes worth fixing immediately — one on each side.

**canary-wap: the sealed log now actually reaches the card.**
`create_witness_record()`'s "store to SD" branch had only ever
incremented a counter — every signed witness record lived in RAM alone,
and only the chain head + sequence survived reboot (via NVS, persisted
every 10 records). Separately, the whole data-management layer addressed
`/sd/WITNESS`, `/sd/CHAIN/backup.bin`, `/sd/EXPORT`… — a phantom `sd/`
subdirectory the mount path never creates (the SD library already roots
paths at the card), so the hourly HMAC'd chain backup and the export
bundles were writing into the void. Fixed:

- Every signed record now appends one self-describing JSON line to the
  append-only `/WITNESS/records.jsonl` (close-per-write crash model,
  latched health warning when no card — the beacon-audit two-tier
  pattern). The line carries seq/time-bucket/type/payload-hash/prev/
  chain-hash/signature, so any off-device tool can re-verify the chain
  and every Ed25519 signature from the card alone.
- Boot and hot-mount reconcile the NVS chain-head cache against the SD
  tail: SD wins only when strictly ahead AND the tail record's signature
  verifies under this device's public key (a foreign or tampered card
  can never move the chain head). This also closes the power-cut window
  where up to 9 records of chain advance were silently lost.
- All data-management paths are root-level now; the periodic sweep no
  longer lists /WITNESS as rotatable (Invariant IV: the sealed log is
  never rotated), and the export-bundle write + rotation target the
  /EXPORT directory that actually exists.
- New host test `test_witness_store_logic.cpp` (CI) pins the byte-exact
  line format, torn-tail recovery, malformed/overflow rejection, and the
  SD-wins decision.

**Kernel: break-glass tokens are now single-use across process
boundaries.** The token's `consumed` flag lived only in memory and a
token FILE re-parses as unconsumed, so within its 10-minute validity
bucket a granted token could authorize repeated unseals/exports across
separate CLI invocations. The kernel now burns each token's nonce in a
`consumed_break_glass_tokens` table (same SQLite DB as the receipts) —
burn-first, before any cleartext exists — in all three consumer paths
(CLI `break_glass unseal`, the served backend unseal, and
`export_events_authorized`). New integration test proves a re-parsed
token file is refused on second use inside the same bucket.

Not changed (flagged for follow-up decisions): canary-sense's chain-hash
construction differs from canary-wap's (no seq/time-bucket in the hash;
different genesis) — aligning it would break the shipped Home Assistant
verifier and needs a coordinated version bump; canary-vision publishes
unsigned events (no witness chain at all); and the never-implemented
`firmware/common/witness/witness_chain.h` C API remains a spec-only
header.

### canary-wap: update-available alerting (health log + dashboard banner)

The daily OTA check used to complete silently — a pending update was
only visible if you happened to open Settings -> Device. Now the result
surfaces on its own:

- Firmware: when a check finds a newer signed release, the device logs a
  once-per-version health NOTICE ("Firmware update available", with the
  version as detail), so it lands in the health panel and anywhere else
  health events flow. Re-checks of the same version stay quiet; a newer
  version alerts again.
- Dashboard: a banner appears at the top of every panel when an update
  is pending, naming the version, with "View & install" (jumps to
  Settings -> Device) and "Later" (silences exactly that version —
  the next release banners again). Refreshes with the dashboard start
  and every 30 minutes; visibility logic is a pure `WEBUI_LOGIC`
  function with node tests.
- canary-vision and canary-sense need nothing here: their control
  surface is the Home Assistant MQTT update entity, and HA natively
  badges pending updates for those.

### WiFi/CSI sensing overhaul: it detects things now — frame supply fixed, feature math rewritten, RF-presence fusion actually wired

Field report: "currently it doesn't seem to detect much at all." Three
root causes, all fixed:

**1. The device was starving for frames.** CSI sensing measures
received WiFi frames; the 50 Hz active probe that was supposed to give
paired Canaries a deterministic frame supply was never initialized,
started, or pumped — dead code. It now runs: every Canary broadcasts a
10 Hz ESP-NOW sensing probe (≈0.03 % airtime) so peer Canaries sense
off each other; a solo Canary rides the home AP's ~10 Hz beacons. The
dashboard footer shows the live supply (`signal 11/s · probing`) and
when genuinely starved (AP-only, no peers) the device now says
honestly *"no WiFi signal to sense with — join your home WiFi or add a
second Canary"* instead of confidently reporting "Empty" off zero
data (module ticks are skipped below 2 frames/window).

**2. The feature math couldn't separate people from physics.**
Rewritten in the canonical extractor, the wap staged copy, and the
canary-tree lib, with a host physics test proving each fix:
- Amplitude motion is now per-subcarrier TEMPORAL variance after
  per-frame AGC normalization and true-magnitude (√(I²+Q²))
  conversion. The old pooled variance mostly measured the room's
  static multipath fingerprint + receiver gain flicker + the L1
  amplitude's rotation wobble — noise that didn't change when a
  person moved.
- "Doppler" is now a CFO-corrected relative band rotation:
  Im(C_band·conj(C_tot))/|C_tot|², which cancels the ESP32's random
  per-frame phase offset exactly (the old raw cross-product was
  offset noise), is gain-invariant, and alias-proof (magnitude
  accumulation, sign carried separately).
- Breathing (0.10–0.45 Hz) is now measured where it physically lives:
  a Goertzel bank over a cross-window envelope ring (~64 s @ 1 Hz).
  The old code ran 8 numerically identical near-DC filters inside a
  single 1 s window — 0.2 Hz cannot be resolved in 1 s; its "dominant
  bin"/BPM was fiction. Bins stay zero until ≥24 windows exist, the
  bin↔BPM map now matches core_breathing, and the ring is scrubbed on
  sensing stop (reset_history — same privacy contract as the sample
  buffers). Empty room now reads ≈0 on every axis in any environment,
  which is what makes the presets/calibration portable across homes.

**3. CSI → RF presence fusion was never connected.**
`rf_presence::feed_csi_window()` existed since Phase 2 but
`set_legacy_features_hook` was never called — the RF presence FSM
never saw a single CSI window. Wired at boot; the two systems now
corroborate.

Also: host physics test suite (`test_csi_features.cpp`: static
channel + random CFO must read empty, ±30 % AGC flicker must read
empty, a moving scatterer must be detected through CFO, a 0.25 Hz
breathing envelope must land in bin 3 and dominate, reset_history
must wipe), signal-supply fields on `/api/csi/stream`, and
`docs/hardware/csi_sensing_guide.md` (placement, environments,
calibration, verification, honest limitations). Re-run Calibrate
after updating — the feature scales changed.

### OTA: unified-update audit — partition pin, doc truth, first-release runbook

Survey result across the three field firmwares (canary-wap on XIAO
ESP32-S3, canary-vision hosting the Grove Vision AI V2, canary-sense
MR60BHA2 heartbeat on ESP32-C6): all three already consume the shared
signed pull-OTA engine (`firmware/common/ota/` — Ed25519-signed manifests
and images, A/B partitions, boot self-test with automatic rollback, NVS
anti-rollback floor), and the `fw-v*` release workflow builds and signs
all seven product manifests. What has kept it from working in the field
is operational, not code: no `fw-v*` release has ever been cut, and the
committed release public key is still the all-zero placeholder that
hard-disables installs by design. Both failure modes already report
honestly on-device ("Release public key not provisioned" / "Failed to
fetch manifest").

Hardening shipped here:

- canary-vision now PINS its OTA-capable partition tables
  (`default.csv` on the C3 envs, `default_8MB.csv` on xiao-s3) instead
  of inheriting whatever the board package defaults to — a silent board
  default change could have dropped the second app slot.
- `docs/firmware_ota.md`: covered-variants list now includes
  canary-sense and the vision board products; documents that the Grove
  Vision AI V2 module's own firmware/model is SenseCraft/USB-C-loaded
  and not host-flashable (host ESP32 image only over the air); adds a
  first-release runbook (keygen -> pubkey header -> OTA_SIGNING_KEY_PEM
  secret -> `fw-v*` tag -> on-device verification, and key rotation).
- `firmware-release.yml` header comment no longer omits canary-sense.

### Acoustic detection: shipped in release builds + two-stage (tone-gated) matcher

Root-cause fix for the field report "pressed my smoke alarm's TEST
button, nothing happened": the published `release`/`release_ha` OTA
images compiled the entire acoustic subsystem **out** — production
devices had no mic code at all. Alongside shipping it, the detector was
upgraded to the two-stage structure the industry uses for alarm-sound
recognition (spectral gate + temporal template — cf. US 9,087,447 /
US 8,269,625, ISO 8201, and fully-on-device recognizers like HomePod
Sound Recognition). Applied to the canonical `securacv_audio` module
and the canary-wap vendored copy in lockstep (sync guard clean).

- **Release builds now include the sensing suite** (`[env:release]`,
  inherited by `release_ha`/`standalone`): acoustic T3/T4, capacitive
  touch, IR RMT, temp-tamper, sensing-witness signing. Phase 2b
  transients (knock/doorbell/glass) stay dev/full opt-in.
- **DC-removed RMS**: envelope now uses `E[x²]−E[x]²`; a PDM DC offset
  can no longer pin the envelope ON and blind the matcher.
- **Alarm-band tone gate**: an RBJ band-pass biquad (fc 3.4 kHz,
  Q≈1.8 → ≈2.6–4.4 kHz, where UL 217/2034 sounders sit) summarizes each
  envelope state into a 0..200 tone ratio; T3/T4 beeps must be
  alarm-band dominant (≥50 normal / ≥30 self-test). Rhythmic slams,
  voices and TV can fake the cadence but not the spectrum. Known
  limitation (documented): 520 Hz low-frequency sounders don't gate.
- **Sample-stream clock**: envelope/cadence timing now advances
  frames × 20 ms instead of reading `millis()` at drain time, so
  burst-draining after a stalled loop can't distort beep/gap durations;
  wall time remains for buckets/deadlines/staleness.
- **DMA ring 4→8 buffers** (80→160 ms) and `audio_process()` drains up
  to 8 frames/call — the observed ~100 ms main-loop stalls (TLS, NVS,
  OTA checks) no longer overflow mid-beep.
- **Diagnostics**: `audio_transition_t` gains `tone_x100` (layout
  unchanged — carved from reserved bytes); `/api/audio/level` (canary)
  and `/api/audio/transitions` (wap) now report per-transition `tone`.
- **Bench procedure**: `docs/hardware/acoustic_alarm_bench_test.md` —
  five-minute verification from level meter to self-test to cadence
  trace, tone-value interpretation table, field check, limitations.
- **Host tests**: cadence suite extended to 11 cases — off-band (500 Hz)
  T3 cadence must NOT match, DC offset reads as silence, T3 still
  matches with a frozen wall clock (stream-clock regression), transition
  ring exposes the tone ratio; existing T3/T4/knock/doorbell/glass/mute
  cases re-scripted with spectrally honest waveforms.

### canary-wap: PSRAM static diet, wave 2 — ~26 KB more internal DRAM back (running total ~67 KB)

Second sweep of task-context-only statics into PSRAM via `csi_large_calloc`
(same fail-safe contract as wave 1: NULL disables the owning feature, an
ESP32-C3 keeps its old footprint through the internal fallback):

- chirp channel tables (~19 KB): recent-chirps heap, dedup bloom filter,
  nearby-device cache, pubkey rate-limit LRU, self-test dedup — all
  allocated in `chirp_channel::init()`, which now fails (channel disabled)
  if any allocation does. Safe because every access runs from
  `mesh_network::update()` on the loop task; the ESP-NOW callback only
  stages 250 B and sets a flag.
- mesh alert history (2.9 KB) — allocated at mesh `init()`;
  `store_alert` drops records fail-safe while NULL.
- GPS byte ring (2 KB) — placement-new into PSRAM at setup; the UART is
  still drained even if buffering is unavailable.
- airtime governor send-window ring (2 KB) — allocated in its `init()`;
  covered by the existing `test_mesh_coexistence` host suite, where the
  allocator falls back to plain calloc.

Deliberately NOT moved, so this stays a pure win: `csi_hal::s_ring`
(written per WiFi frame from the CSI callback — the one genuinely hot
path) and `mesh_network::g_peers` (`OperaPeer` carries session keys;
key material stays in on-die SRAM rather than on an externally
probeable PSRAM bus). The RAM Audit workflow's DRAM regression
assertion now covers all wave-2 buffers and triggers on the touched
sources.

### canary-wap: PSRAM static diet, wave 1 — ~41 KB of internal DRAM back for the Bluetooth budget

The ELF-level RAM audit (RAM Audit workflow, PR #834) showed 158 KB of the
S3's 320 KB internal DRAM bank spent on static globals; the biggest
project-owned ones are task-context-only buffers with no reason to live
there. They now allocate from PSRAM via the new `csi_mem.h`
`csi_large_calloc()` (PSRAM-first, internal-heap fallback, NULL disables
the owning feature fail-safe — an ESP32-C3 without PSRAM keeps exactly its
old footprint):

- `g_health_log_ring` (14 KB, 100 entries) — allocated at the very top of
  `setup()`; if even the fallback fails, `log_health` degrades to
  Serial-only and the ring stays empty.
- `emit_summary()`'s 64-row scratch (11.5 KB) and `csi_features`'
  amplitude history (10 KB) — both CSI-library copies, allocated lazily on
  the owning task; a NULL skips the summary / disables the feature
  pipeline instead of crashing.
- fleet-scan cache + handler snapshot (2 x 2.5 KB) — the handler answers
  `out of memory` honestly if allocation ever failed.

Every reclaimed KB lands 1:1 in the internal heap the BLE stack needs
(~40 KB free measured in the field vs the ~96 KB guard), and shrinking
`.bss` also grows the heap contiguously, helping the 48 KB
largest-block requirement. Regression-guarded: the RAM Audit workflow now
FAILS if any of these buffers reappears in the internal-DRAM window
(address-filtered — nm's section letters count ESP-IDF's writable-marked
`.flash.rodata` as RAM), and it invokes gawk explicitly for `strtonum`.

### Canary Vision: unboxing-to-using walkthrough + boxes-only "Aim camera" view

- **Getting-started guide** (`docs/hardware/canary_vision_getting_started.md`):
  one clean path from a sealed box to a publishing witness — assemble
  (camera ribbon + XIAO stacking orientation), load the Person Detection
  model with Seeed's SenseCraft flasher (kept for exactly that one job,
  per the strategy doc), flash canary-vision, watch MQTT discovery
  populate HA, import the dashboard, aim, tune, troubleshoot.
- **Aim assist (firmware)**: a new HA switch streams the best person box —
  coordinates, score, voxel cell, never pixels — at ~5 Hz on
  `securacv/<id>/aim` (non-retained; empty frames at 1 Hz so the view
  clears). Off by default, 10-minute auto-off, quiet publisher (no serial
  spam at 5 Hz).
- **Aim camera Lovelace card** (`custom:securacv-aim-card`, auto-served by
  the integration like the timeline card): draws the live bounding-box
  wireframe, score, and voxel-grid highlight on a canvas over HA's MQTT
  websocket, with a Start/Stop aiming button bound to the switch and
  honest status lines (needs an HA admin user for the live stream; clears
  to stale after 5 s of silence). Replaces re-plugging a laptop into the
  module's USB port for SenseCraft preview after deployment — that stays
  the one-time bench check; in-situ aiming never exports a frame. Node
  unit tests cover the payload/geometry/discovery helpers (11 cases,
  wired into CI).

### canary-wap: sealed alarm snapshots — opt-in, write-only-escrow camera frames on life-safety triggers

New `FEATURE_VAULT_SNAPSHOT` subsystem (FULL/S3 only; needs camera + PDM
mic): on a T3 smoke / T4 CO / glass-break acoustic detection — each trigger
individually opted in, **all off by default** — the device captures one JPEG
and seals it to `/VAULT` on the SD card with an X25519 sealed box (ephemeral
ECDH → HKDF-SHA256 → ChaCha20-Poly1305, 64-byte header as AAD). The device
stores only the operator's **public** key and cannot decrypt what it wrote;
unlock happens off-device with the new `tools/unseal_snapshot.py`
(gen-key / inspect / unseal). Device-side analog of the witness kernel's
break-glass vault.

- Fail-closed decision table in host-tested `vault_logic.h`: no key (not
  even the Test capture), no SD, no camera, QR scan active, seal in flight,
  per-trigger cooldown — every refusal except "not opted in" health-logs
  its reason; a raw frame is never staged unless the decision is CAPTURE.
  Clearing the key forces all triggers off.
- Capture + seal run on a one-shot worker task (PSRAM staging, framebuffer
  returned immediately, tmp+rename, full zeroize); the loop adopts the
  result and emits a `media.vault/frame_sealed` witness event whose
  allow-list is a `"<trigger tag> <ciphertext SHA-256 prefix>"` note +
  time bucket — image bytes structurally cannot cross the chokepoint. The
  event is anomaly-category and stateless so a night-time (quiet-hours)
  seal still chains its hash and the bundler cannot fold two seals into
  one row.
- Ring of newest 20 sealed files via the tested `datamgmt::rotate_dir`;
  512 KB per-frame cap; 10–3600 s per-trigger cooldown (default 60 s).
- 8 auth-gated routes (`/api/vault/*`: status, config, key set/clear, list,
  download, delete, test) + a "Sealed Alarm Snapshots" card in the Camera
  panel (key registration with key-id echo, per-trigger toggles that stay
  disabled without a key, sealed-file table with download/delete, Test
  capture). Dashboard/footer copy updated to disclose the opt-in exception
  honestly.
- Tests: `test_vault_logic.cpp` (decision matrix incl. millis wrap,
  malformed-header rejects, golden 64-byte header fixture) and
  `tools/test_unseal_snapshot.py` (crypto round-trip, wrong-key/tamper/AAD
  negatives, the same golden fixture verbatim) — both wired into
  firmware.yml.
- **Requires maintainer sign-off:** `docs/security/THREAT_MODEL.md:134`
  says "Camera | Preview only"; this PR does not edit constitutional docs
  and flags the tension in `docs/sealed_snapshot_vault.md`.

### canary-wap: BLE bring-up moves to a worker task (loop watchdog crash fix) + memory-budget instrumentation

Field regression from the deferred bring-up: ~21 s after boot,
`task_wdt: loopTask` with both cores idle — the loop was parked inside the
BLE bring-up (NimBLE controller/host init synchronizes with the WiFi
coexistence layer and can block its caller past the loop's 8 s watchdog
budget), two crashes from tripping safe mode.

- The deferred Bluetooth/BLE bring-up now runs on a **one-shot worker task**
  (same pattern as the SD mount worker and the MJPEG stream worker); the
  loop task only spawns it and can never be blocked by it.
- The `canary.local` steward skips its blocking mDNS operations while a
  fleet browse is in flight — the mDNS component serializes API calls, so
  stacking the loop's delegate ops behind the worker's ~3 s `_securacv._tcp`
  search parked the loop for the sum.
- **Memory-budget ledger**: one `[HEAP] after <phase>: internal free/largest`
  line after each heavy boot step (camera, SD, audio, network, mesh) and
  around the BLE bring-up, so the "can Bluetooth fit on this build?"
  question reads directly off any boot log instead of being reconstructed
  from crash forensics. Field measurement so far: the FULL/S3 profile
  reaches the BLE gate with ~40 KB free internal where the stack needs
  ~96 KB — Bluetooth on FULL requires a feature trade or a custom
  (non-prebuilt-core) build; the guard now proves it per-boot.
- Bluetooth API errors carry the recorded refusal reason: `/api/bluetooth/
  enable`, advertise/pair auto-enable, and scan-start now return "internal
  RAM too low (largest block N KB/48 KB, free N KB/96 KB)" instead of
  "Failed to enable Bluetooth" or the stale "check antenna / NimBLE
  library" guess.

### canary-wap: two Canaries on one WiFi now coexist — fleet discovery, canary.local dedupe, Bluetooth boot-order fix

Field report with two devices on the same home network: Bluetooth showed
"NimBLE init failed" on both (PSRAM enabled!), the Fleet sheet said "No other
Canaries found yet", the WiFi-QR generator rendered a bare red "Failed", and
`canary.local` reached an arbitrary device that could *change between
requests* — silently invalidating the session cookie mid-use.

- **Bluetooth initializes from the loop once the provisioning window clears
  — not from setup() at all.** (Corrected from the first cut of this change,
  which initialized BLE *before* WiFi: on the FULL build the stack's
  ~55–65 KB internal-RAM spend then starved the network — httpd couldn't
  create its socket (`ENOBUFS`), the SoftAP's WPA2 handshake failed so
  phones looped on the password prompt, and the heap monitor sat in
  EMERGENCY at 2 KB free.) The whole bring-up now runs one-shot from
  `ble_discovery_start_if_due()` after the setup AP is torn down — the point
  of *maximum* free internal memory — and the heap guard gained a total-free
  axis (`bt_defaults::MIN_INIT_TOTAL_FREE`, host-tested): BLE only starts if
  it leaves real operating margin, because "Bluetooth up, network dead" is
  strictly worse than "no Bluetooth, honest reason shown".
- **The self-test now says WHY Bluetooth is off** instead of the catch-all
  "NimBLE init failed": `bluetooth_channel::init_fail_reason()` distinguishes
  the heap-guard refusal ("internal RAM too fragmented (largest free block
  N KB, need 48 KB)") from a real stack failure, surfaced in `/api/selftest`,
  `/api/bluetooth`, and the Bluetooth settings tab.
- **Fleet now discovers Canaries on the LAN.** Every device already
  advertised a `_securacv._tcp` mDNS service with its identity — nothing
  consumed it; the Fleet sheet listed only ESP-NOW opera-mesh members (an
  explicit pairing flow), so WiFi-sharing devices were invisible to each
  other. New `GET /api/fleet/scan` browses the service from a short-lived
  worker task (the ~2 s blocking browse never stalls the single httpd task —
  same lesson as the MJPEG stream) with a 10 s cache; the Fleet sheet lists
  every Canary (self marked "this one") with name, unique
  `canary-<name>.local` hostname, IP, and an Open link to its dashboard.
- **canary.local can no longer be double-claimed.** The bare-hostname
  catch-all used a single 600 ms first-wins probe at STA join; two devices
  powering up together (power restored) both probed into silence and BOTH
  claimed it — the field's session-flip. The claim is now scheduled with a
  fingerprint-derived stagger (one claims first, the other's probe sees it),
  and a periodic conflict check applies a deterministic IP tie-break so a
  surviving double-claim resolves to exactly one keeper (host-tested
  antisymmetry, `catchall_logic.h` + `test_catchall_logic.cpp`).
- **WiFi-QR provisioning:** the button's opaque red "Failed" now reports the
  actual cause (session expired / rate-limited / error N); the generator
  refuses credentials containing `;` with a clear 400 instead of encoding a
  QR that silently truncates at scan time (the payload is `;`-delimited with
  no escape sequence).

### canary-wap: camera preview no longer freezes the whole dashboard, and its numbers are real

The MJPEG preview handler used to run its frame loop **on esp_http_server's
single worker task**, so for as long as a preview streamed, every other HTTP
request queued behind it: `/api/peek/status` polls (hence "Current: Unknown",
"THROUGHPUT 0 kbps", "STREAM UPTIME —" *while streaming*), the sensor tuning
sliders, and every other dashboard tab. Field-reported as "camera settings
don't even work".

- **Stream moved to a dedicated FreeRTOS worker task** via
  `httpd_req_async_handler_begin/_complete`, freeing the httpd task
  immediately — status polls, sliders, and the rest of the dashboard stay
  live during preview. The worker runs at priority 3 with an unconditional
  ≥20 ms `vTaskDelay` on every loop path (the WDT-subscribed IDLE tasks stay
  fed; the pace floor is host-test-pinned), uses an 8 KB internal-RAM stack,
  and exactly one stream runs at a time (second client → 409 Conflict).
- **Stream uptime freezes at stream end** instead of collapsing to 0, so
  "LAST STREAM" throughput/uptime stay truthful (`peek_stream_logic.h`,
  host-tested: `test_peek_stream_logic.cpp` + CI step).
- **Resolution controls fixed end-to-end**: the "320×240" button sent
  framesize 4 (which is 240×240) — now sends QVGA (5); `framesize_name()`
  learned `FRAMESIZE_240X240` so no supported size reads "unknown";
  `/api/peek/resolution` and `/api/peek/init` now *wait* for the stream
  worker to exit (bounded, fail-closed 503 on timeout) instead of a blind
  100–150 ms sleep, and report `stream_stopped` so the UI reconnects the
  preview — the old blind `g_peek_active = true` restore couldn't resurrect
  a finished HTTP response.
- **Preview looks professional**: live metric chips overlaid on the video
  (LIVE / resolution / fps / throughput / uptime, straight from
  `/api/peek/status`), the wall of raw sensor toggles collapsed behind one
  "Advanced sensor tuning" disclosure, throughput rendered human-readable
  (`fmtKbps`, node-tested in the WEBUI_LOGIC block), and resolution status
  falls back to the firmware-reported name for any framesize set via API.
- **Bluetooth settings tab fixes**: settings toggles (auto-advertise, allow
  pairing, long-range) now reload every time the tab is opened instead of
  only at first page load; the enable/disable toggle reverts on failure
  instead of showing a state the radio isn't in; advertise start/stop decides
  its verb from live device status instead of a possibly-stale cache.

### canary-wap: a FULL build without PSRAM no longer compiles (it could never run)

Field evidence from the SD-crash-loop aftermath: a FULL-profile XIAO ESP32-S3
build flashed with the Arduino IDE's default **PSRAM=Disabled** boots, but
internal heap collapses to EMERGENCY (<1 KB free) once WiFi + HTTP + camera +
CSI are up — BLE can't start (the heap guard refuses), SD writes fail until
the card is marked failed, mDNS/`canary.local` times out, dashboard pages
half-render, and the device limps instead of witnessing. The IDE toggle also
silently reverts when the board selection changes, so this misconfiguration
kept recurring.

- `build_config.h` now **refuses to compile** FULL + XIAO_ESP32S3 without
  `BOARD_HAS_PSRAM`, with the exact fix in the error message (Tools > PSRAM >
  "OPI PSRAM" / `--fqbn ...:PSRAM=opi` / `--profile xiao_sense`). Experts can
  define `SECURACV_ALLOW_NO_PSRAM` to bypass.
- Every CI/release Arduino build now compiles with `PSRAM=opi` — the bare
  board FQBN used before defaults to PSRAM=Disabled, so CI was validating
  exactly the broken-in-the-field configuration, **and the release workflow
  was shipping OTA binaries built without PSRAM**. Fixed in `firmware.yml`,
  `csi_module_disable_matrix.yml`, and `firmware-release.yml`; where a job
  overrides `build.extra_flags` (which replaces the board expansion carrying
  `-DBOARD_HAS_PSRAM`), the define is re-added explicitly. The guard itself
  is gated on `ESP_PLATFORM` so host g++ test builds that include
  `build_config.h` are exempt.

### canary-wap: a slow or wedged SD card can no longer crash-loop the device

`SD.begin()` is a chain of yield-free CPU spin loops in the SPI SD driver
(500–1000 ms card waits, ×3 retries, across two mount speeds plus FAT sector
reads) with **no overall deadline**. It ran directly on the watchdog-subscribed
loop task, and `sd_mount_safe`'s "2 s timeout" was checked only *between* the
two blocking attempts — it bounded nothing. A card that holds the bus (wedged,
dying, or incompatible) blew the 8 s panic watchdog mid-mount and rebooted the
device. Worse, the loop's periodic SD recheck re-ran the same blocking mount
**even in safe mode** at ~38 s — before safe mode's 60 s recovery window — so
safe mode itself crash-looped forever (observed in the field: `consecutive
crash count 7/3…` climbing, AP flapping every ~40 s, captive portal blank).

Fixes, layered:

- **All blocking mount work moved to a dedicated worker task** at
  `tskIDLE_PRIORITY` (both cores' IDLE tasks are watchdog-subscribed and the
  SD driver never yields — at any higher priority a stuck mount would just
  move the panic to IDLE0/IDLE1). The loop task polls a state byte, feeding
  the watchdog, up to a 4 s budget; a result that lands later is adopted by a
  subsequent loop pass instead of being lost. Only the loop task writes
  `g_hw`; the worker hands back a private result (single-writer model kept,
  no 64-bit tearing for httpd readers). Nothing ever cancels a running mount
  (`SD.end()` under the worker would be use-after-free).
- **Safe mode now truly skips SD**: the periodic recheck honors the same
  contract as boot (pure host-tested decision table, `sd_mount_logic.h`).
- **Watchdog fed between setup Phase-3 steps** (camera → SD → audio →
  network): they used to share ONE unfed 8 s budget from watchdog-arm to the
  first loop pass, which is how camera-init seconds plus a slow card panicked
  the very first boots after flashing.
- **Late/hot-plug mounts get provisioned**: `/WITNESS`, `/HEALTH`, `/CHAIN`,
  `/EXPORT` and the CSI event log are created on the mount transition in
  loop(), so a card that mounts after the boot budget still gets its layout.
- **GPIO21 hazard guarded**: on the XIAO ESP32-S3 the user LED *is* the SD
  chip-select pin (`LED_BUILTIN == GPIO21 == SD_CS`). LED writes (provisioning
  blink, visual chirps) are skipped while a mount is in flight so they can't
  glitch CS mid-transaction on the worker.

### canary-wap: BLE init no longer boot-loops a low-memory build

A build with PSRAM disabled (Arduino IDE default is easy to miss) boots with
only ~127 KB of internal heap. Once the WiFi AP and HTTP server are up, no
contiguous block remains for the BLE controller's ~30 KB init allocation, so
`NimBLEDevice::init()` fails — but the controller *asserts and panics* rather
than returning an error, tripping the interrupt watchdog. The device then
boot-loops on `BLE_INIT: Malloc failed`, and because the crash is below the
app, even safe mode can't recover it.

Every `NimBLEDevice::init()` call site (the pairing channel, BLE discovery, and
the CSI BLE Scout — the last of which ran even in safe mode) now checks the
largest free internal block first and skips the stack, leaving the radio off,
when there isn't enough room. BLE degrades to "off" instead of bricking, so the
device always comes up as a reachable AP + dashboard. The threshold and
decision are a pure, host-tested predicate (`bt_defaults::init_has_headroom`);
the heap read is thin glue (`ble_heap_guard.h`). Enabling PSRAM (Tools > PSRAM
> "OPI PSRAM") remains the real fix — this just makes a mis-set build fail safe
instead of unrecoverable.

### canary-wap provisioning: BLE scan no longer starves the SoftAP join

Joining the device's setup Wi-Fi was intermittently failing — the phone's
"enter password" sheet looping instead of associating. Root cause: BLE
Discovery's Nearby scanner runs a 5-second, ~99%-duty **active** scan
(window 99 / interval 100) pinned to the shared WiFi/BLE core, and the first
burst fired at boot. When a phone's WPA2 4-way handshake to the provisioning
SoftAP overlapped a burst, the handshake frames were starved and the join
failed; catch a gap and it worked — hence the flaky loop. The firmware itself
already rates AP + STA + BLE as unstable and drops the AP once the STA is up to
escape it, but during provisioning the AP has to stay up.

Fix: BLE Discovery still initializes at boot, but its radio activity (Opera
advertising, Nearby active scanning, boot chirp) is now **deferred out of the
join window** — brought up once the management SoftAP has actually been torn
down (the firmware keeps it up for a grace window after the STA gets an IP so
the phone can read the success card, *then* drops it to the stable STA+BLE
combo, so gating on AP-down rather than mere `WL_CONNECTED` also keeps the scan
out of that protected handoff). In AP-only standalone mode — where the AP is
permanent — it starts after a short settle so the operator's first association
lands cleanly, and a normal device whose home Wi-Fi never comes up starts after
a 5-minute max-hold fallback rather than staying disabled forever. BLE stays on
by default; it just doesn't transmit while a phone is mid-join. The decision is
a pure, wrap-safe predicate (`provisioning_logic::ble_discovery_start_due`) with
host-test coverage, and the self-test reports the pre-start window honestly as
"Radio up · all features idle" (SKIP, non-gating).

### canary-sense witness signing: Ed25519 events, hash chain, verified-green in HA

Completes the canary-sense design doc's Phase 2 trust items, reusing the
signing surface field-proven in the recent canary-wap PRs:

- **Device identity**: Ed25519 keypair generated from the hardware RNG on
  first boot, NVS-persisted (`securacv/privkey` — the wap storage
  contract), fingerprint via the wap `sha256_domain` formula. The signer
  itself is now a shared module (`firmware/common/identity/
  device_signature.{h,cpp}`, host-tested) with a new v1 `sense` canonical
  kind alongside the locked chain/event/counts formats.
- **Every witnessed transition advances a domain-separated SHA-256 hash
  chain** (`securacv:fw:chain:v1`, genesis bound to the device key),
  NVS-persisted — offline gaps show up as seq/length jumps, never lost
  tamper evidence. Events publish with `v`/`alg`/`fp`/`sig` over the
  `sense` canonical; the retained `chain` topic reuses canary-wap's exact
  wire schema, and the retained `health` topic carries `public_key` so
  Home Assistant TOFU-pins the device with its existing subscription.
- **HA integration verifies radar events**: `signature.py` gains the
  `sense` canonical + `verify_sense_event`, and the events handler
  dispatches by payload dialect (CSI vs radar) so each verifies against
  its own canonical.
- **Fingerprint derivation bug fixed (wap ⇄ HA)**: firmware fingerprints
  are `SHA256(domain || 0x00 || pubkey)` but HA's
  `fingerprint_from_pubkey_hex` omitted the NUL separator, so every
  pinned device rendered "Fingerprint changed without rotation" instead
  of the green badge. HA now matches the deployed firmware byte-for-byte
  and heals previously stored pins on load.
- **Task watchdog wired on canary-sense** (IDF5 `esp_task_wdt_reconfigure`,
  the canary-wap pattern; 30 s to clear the bounded broker-connect block).
- Host tests lock the canonical bytes + b64url encoding
  (`firmware/tests_host/test_device_signature_common.cpp`); HA pytest
  covers sense-event verify (happy path, missing fields, tampered
  payload) and the fingerprint healing migration.

### canary-wap admin console: the last dead controls now work

Follow-up to the panel revival — the three controls that were honestly
labeled "not available on this build" are now functional:

- **RF Presence tab**: `rf_presence` is wired up — `init()` in setup
  (privacy-preserving session/token state; no radio started), `update()`
  in loop, and the seven `/api/rf/*` routes registered behind the
  Bearer/session `auth_gated` trampoline the module's header mandated.
  Sensing stays opt-in (enable from the tab, persisted in NVS); BLE already
  feeds the fusion scorer. The v0 scoring FSM is unchanged.
- **Storage cleanup** ("Clean up old export bundles"): a real, auth-gated
  `POST /api/logs/rotate` trims `/sd/EXPORT` (witness-export bundles that
  otherwise accumulate unbounded) to the newest 20 via the tested
  count-based `datamgmt::rotate_dir`. It never touches `/sd/WITNESS` or
  `/sd/CHAIN` — the sealed evidence is untouchable (Invariant IV).
- **Device "Save Configuration"**: record interval, time bucket, and log
  level are now real NVS-persisted runtime settings via `POST /api/config`.
  The time bucket coarsens event timing (Invariant III) and is clamped so
  it can only be widened past its 5000 ms floor, never narrowed — privacy
  is monotonic. The log threshold is clamped to ≤ WARNING so ERROR/CRITICAL
  are always stored.

All clamps live in Arduino-free logic headers with host tests
(`config_logic.h`), the route-security/budget CI guards extend to the new
routes, and `web_assets_gz.h` is regenerated.

### canary-sense Phase 2: the mmWave radar witness publishes

- **Full network stack on the MR60BHA2 kit** (XIAO ESP32-C6), mirroring
  canary-vision: NVS-backed runtime config (OTA-safe identity/credentials),
  supervised WiFi STA, MQTT with LWT + Home Assistant discovery, heap-health
  diagnostics, and the shared signed pull-OTA engine with an HA `update`
  entity + auto-update switch. Presence-only and wellbeing flavors are
  distinct OTA products with separate signed manifests, so images can never
  cross-install between privacy surfaces.
- **Privacy chokepoint enforced at the publish layer**: events carry only
  `presence_detected` / `presence_cleared` / `occupancy_changed` with the
  coarse vocabulary (presence state, 0/1/2+ occupant bucket, near/mid/far
  range band) and a 10-minute-coarsened uptime bucket instead of a precise
  timestamp (metadata minimization; `seq` preserves ordering). Raw distance
  and vitals never leave the device via events;
  wellbeing builds publish the P0 breathing lock and P1-gated BPM numerics
  on the state channel only, suppressed unless exactly one target is present.
- **HA entity set** per the design doc: presence, occupants, range band,
  radar-link problem sensor, frame-error counter, BH1750 illuminance
  (new minimal vendored driver in `firmware/common/sensors/bh1750/`),
  uptime, RSSI + free-heap diagnostics, firmware update card.
- **CI + release wiring**: the wellbeing env joins the build matrix with a
  secrets pre-step and an OTA-slot size guard; `firmware-release.yml` now
  builds, signs, verifies, and publishes `canary-sense` +
  `canary-sense-wellbeing` binaries and manifests.

### canary-vision robustness parity with the ESP32-S3 tree

- **WiFi auto-reconnect with exponential backoff** (2 s → 30 s cap) replaces
  the reconnect-or-hang loop; a sustained 5-minute outage reboots as the
  recovery of last resort, and the blocking MQTT reconnect now defers to the
  WiFi supervisor instead of spinning while the link is down.
- **WiFi power policy**: optional modem sleep + TX power cap (config.h).
- **Heap monitor with 3-level degradation** (S3 thresholds + hysteresis):
  inference cadence stretches 2×/5× under critical/emergency pressure;
  heap + degradation level + RSSI ride the status heartbeat and surface as
  HA diagnostic entities.
- **Dev secrets actually compile in again**: the `-I secrets` include path
  lived in a project-level `[env]` block that PlatformIO overrides, so a
  user's `secrets/secrets.h` silently fell back to the CI stub; the path now
  lives in the effective env definitions and `runtime_config` accepts both
  include spellings (canary-sense inherits the fixed pattern).

### canary-wap dashboard/settings: revive the whole panel

- **The settings panel is functional again.** On the default Arduino-IDE
  build the active HTTP server budgeted 123 URI-handler slots but registered
  154; esp_http_server silently drops everything past the budget, so the
  last 31 routes 404'd for every client — the entire Presence tab, all
  `/api/audible-chirp*` (speaker), every `/api/chirp/*`, the three
  `/api/ble` routes, and Bluetooth "Clear All". The handler table is now
  itemized per feature flag and sized to fit, guarded by a CI check
  (`check_route_budget.py`) that emulates the preprocessor for FULL/S3,
  DEV/S3 and FULL/C3.
- **Correct credentials stop getting locked out.** A missing `Authorization`
  header was counted as a brute-force failure, so one dashboard tab left
  open after its session cookie expired polled the device into a permanent
  429 that rejected even a correct pasted token. Credential-less requests no
  longer feed the lockout; real token guesses still do.
- **Bluetooth is enabled by default** so the pairing channel (offline
  console, BLE Wi-Fi provisioning, OTA, log/witness export) is reachable out
  of the box. Pairing still requires an on-device PIN confirmation — the
  radio is on, not open.
- **Honest self-test.** The pre-flight health check no longer hard-FAILs
  Bluetooth during the boot window or in recovery/safe mode (SKIP, with a
  reason), distinguishes a Wi-Fi link-drop from a radio-off, counts SKIP
  rows in its summary, and surfaces `safe_mode` so a recovering-but-healthy
  device stops reading as broken. A "Device self-test" card in Settings
  re-runs the same checks on demand.
- **Fixed controls:** BLE Discovery Alert/Heartbeat now POST to
  `/api/ble/chirp/send` (not the community handler that 400s); the camera
  peek stream drops the bogus `token=null` and bounds its retry loop;
  Settings Wi-Fi "Connect"/standalone accept an admin credential (session
  cookie / Bearer) as an alternative to the wizard's pair token, so they
  work post-setup; the Community Chirp toggle is wired
  (`chirp_channel::init`/`update`); the dead "Breath sound" switch is gone;
  and dashboard `localStorage` access is guarded so a cookie-blocking
  browser degrades gracefully instead of killing every binding.
- **Security:** the three `/api/ble` routes (previously unauthenticated,
  reachable only once the budget fix resurrected them) and the `qr-scan`
  read/cancel endpoints are now gated; a CI check
  (`check_route_security.py`) asserts every registered route is credential-
  gated or on a documented public allowlist.
- **Honest-labeled** the controls fronting subsystems not wired into this
  build (RF signal presence, SD log rotation, runtime record/bucket config —
  the last touches time-coarsening, Invariant III) instead of firing
  requests that 404. Documented as follow-up feature work.
- Docs: the Arduino README and `sketch.yaml` now correctly list
  `NimBLE-Arduino` (2.x) as a required separate library.

### API hardening: per-IP rate limit + at-rest encryption pinned

- **Event API rate limiting**: every endpoint except `/health` is now capped
  per client IP (fixed one-minute window, default 120/min, 429 with
  `retry_after`). Applies before token validation, so neither a tokenless
  hammer nor a leaked capability token can drive the single-threaded API
  (`POST /verify` walks the whole sealed log). Configure via
  `[api] rate_limit_per_minute` or `WITNESS_API_RATE_LIMIT_PER_MINUTE`
  (0 disables); complements the existing auth-failure lockout.
- **At-rest encryption documented and pinned**: the kernel database has
  always been SQLCipher-encrypted with a seed-derived key — an early audit
  note claiming "unencrypted by default" was wrong. Now stated in
  SECURITY_MODEL.md and docs/why_secure.md, and pinned by regression tests
  (kernel DBs are never plaintext SQLite; opening without the key fails).

### canary-wap first-run wizard: truthful joins, standalone mode, calm portal

- **A successful WiFi join no longer looks like a failure.** Joining a home
  network on a channel other than the SoftAP's dragged the single radio —
  and the setup network — to that channel, kicking the provisioning phone;
  the AP was then torn down 8 s after connect, so the wizard timed out and
  reported "Couldn't connect" on a join that succeeded. The AP now survives
  120 s after connect (long enough to re-associate and see the success
  card), wizard activity resets the 15-minute setup window instead of the
  device rebooting mid-setup, and the timeout copy explains the network
  handoff honestly.
- **Standalone (AP-only) mode**: "Use without home WiFi" in the wizard.
  The device completes setup and lives permanently on its own
  `SecuraCV-XXXX` network (`canary.local` dashboard, captive DNS stays up,
  the AP is never torn down, no STA join attempts). Persisted via
  `wifi_ap_only` in NVS; saving real credentials later exits the mode.
  New pairing-token-gated `POST /api/wifi/ap-only`; `/api/wifi` reports
  `ap_only`.
- **Stale setup links self-heal**: the wizard's pairing token (RAM-backed,
  10-minute TTL, wiped by reboot) is silently re-issued via the new
  setup-only `GET /api/wifi/pair-token` and the credentials resent once —
  "This setup link has expired" now only appears when the wizard truly
  can't recover. Same Host-gated posture as the `/` redirect that mints
  the original token.
- **Scan without kicking the phone off**: the device pre-scans at boot
  (before anything joins the AP) and serves a cached list (5-min TTL,
  `cached`/`age_s` in the response); only an explicit "Scan again" sweeps
  the radio under a live client — the sweep is what used to drop the
  wizard's scan fetch ("Scan failed: Load failed").
- **Calm capability note**: the red "insecure origin / Web Bluetooth"
  banner is now an informational note ("WiFi setup and the dashboard work
  fine without it") and is gone entirely — along with the Bluefy footer —
  inside the WiFi wizard, where Web Bluetooth is irrelevant. Real errors
  still render red.
- **Password field hygiene**: the typed WiFi password is wiped on
  page-hide/tab-background (the app never stored it — Safari's page cache
  restored the form value; verified no credential ever touches
  localStorage/sessionStorage).
- **Compatibility**: no wire or NVS breakage — new NVS key and endpoints
  only; provisioned devices behave as before apart from the longer
  post-join AP grace.

### Fail-closed configuration and verification hardening

- **Unknown config keys are now parse errors.** A misspelled key in
  `adapter_host.toml` or `witness.toml`/`witness_config.json` used to fall
  back silently to the permissive default — `auth_toke` left the webhook
  listener unauthenticated, a `tls_key` typo fell back to plaintext, a
  `cameras` typo processed every camera, and `sensitve` under `[zones]`
  removed the sensitive-zone policy. All config-file structs now reject
  unknown keys with an error naming the key, at startup and on SIGHUP
  reload (reload keeps the running config).
- **Confidence-gated routes require stated confidence** (`mqtt_sensor` +
  webhook shared routing): a payload that omits or misspells `confidence`
  no longer sails past a `min_confidence` floor as 1.0. Routes without a
  floor keep accepting bare trigger payloads unchanged.
- **canary-vision config API fails closed**: `PUT /api/v1/config` (and
  `/:section`) rejects unknown sections/keys with 400 `invalid_config`
  instead of merging typo'd keys while the real setting kept its default
  (e.g. `auto_purge_hour` leaving auto-purge on the longer window).
- **Wizard POST hardening**: a malformed or negative `Content-Length` is a
  clean 400 (previously an unhandled traceback), and bodies over 1 MiB are
  refused with 413.
- **`verify_pipeline.sh` can no longer false-pass**: the live-stack smoke
  check now excludes retained MQTT messages, publishes a nonce-tagged
  event and requires the bridge to ingest *that* event, and requires
  `witness.db` to have been written during the run — stale logs, retained
  payloads, and schema-only databases all fail. A docker-shim regression
  suite replays the old false-pass scenarios in CI.
- New export-boundary absence tests pin that the Frigate object id
  (embedded precise timestamp) and below-floor confidence values never
  appear in a serialized export.
- **Compatibility**: configs carrying stray or misspelled keys now refuse
  to load, and sensors that never publish `confidence` no longer pass
  confidence-gated routes — both deliberate fail-closed breaks; correct
  existing configs and payloads are unaffected.

### Export & diagnosis follow-ups: one-click download, scheduling, inspectors, break-glass UX

- **One-click "Download my events"**: token-gated `GET /export/bundle` on the
  event API returns the full signed ExportBundle as a browser download, with
  optional `?last=24h` / `?start=&end=` windows (bucket-aligned; recorded on
  the `api`-labeled receipt). Surfaced as a window-picker download button in
  the HA add-on ingress panel (token never reaches the browser). The add-on
  proxy fails closed: an unknown or misspelled window parameter is rejected
  with 400 `bad_window` instead of silently widening the export to
  everything retained.
- **Scheduled exports**: `export_events --output-dir DIR --keep N` writes
  rotating `securacv-events-<bucket>.json` files; docs/scheduled_exports.md
  ships systemd timer/cron units and the add-on curl recipe.
  `ExportWindow::aligned`/`::last` + `parse_duration_s` move into the library
  (one alignment rule for CLI and API).
- **Lineage & checkpoint inspectors**: `log_verify --lineage` /
  `--checkpoints` walk everything instead of failing closed — per-epoch
  valid/invalid/unverifiable with reasons, per-checkpoint signer resolution
  against the genesis-anchored lineage, signature checks, and
  timestamp/cutoff regressions, with plain-language guidance (`--json`
  supported). The inspectors never panic on truncated/tampered key blobs —
  they exist to diagnose exactly that database.
- **Break-glass console UX**: shareable trustee signing links
  (`#sign&hash=…` — signer-only page, no token, no server calls), live
  auto-refreshing quorum status with per-trustee pills and a progress bar;
  the console resumes live polling by itself when it connects to an
  already-open request. No backend changes; operator guide now documents
  the console.

### Canary Vision: runtime detection settings (no-rebuild model swaps)

- **Runtime detection config** (`firmware/projects/canary-vision`): the
  person class index, score threshold, lost timeout, and dwell-start window
  are now NVS-backed and exposed as Home Assistant **number entities**
  (device Configuration section, MQTT Discovery). Swapping the SSCMA model
  on the Grove Vision AI V2 via SenseCraft no longer requires a firmware
  rebuild — adjust the class index from HA. Compiled constants in
  `config.h` seed the first boot only; values persist across reboots and
  OTA installs. New retained topic `securacv/<id>/cfg/state` mirrors the
  live values; `securacv/<id>/cfg/{target,score,lost,dwell}/set` accept
  writes (clamped, junk-rejected).
- **Boot banner** now reports the live (NVS) detection settings and the
  actual host board name (was hardcoded to the DevKit).
- **Unified firmware version bumped to 2.2.0** across canary, canary-wap,
  and canary-vision — the first release train that publishes the per-board
  canary-vision OTA images (`-xiao-c3`, `-xiao-s3`) introduced in #786.
  Tag `fw-v2.2.0` to ship.

### Export UX, owner self-export, and verification diagnosis

- **Owner self-export** (`export_events --self-export`): export the
  privacy-filtered event artifact with the device key seed alone — no trustee
  quorum. A signed, chained receipt is still always written; receipts now
  carry an optional `auth_mode` (`break_glass` / `self_export` / `api`) so
  owner-authorized and quorum disclosures stay distinguishable. Sealed-vault
  evidence and unsealing remain quorum-only.
- **Export time windows**: `--last 24h` / `--start`/`--end` on
  `export_events`. Windows are aligned outward to 600 s bucket boundaries,
  filtered on true (pre-jitter) buckets, and recorded on the signed receipt
  (optional `window` field).
- **Actionable verification failures**: `log_verify` and `envelope_verify`
  now print a plain-language diagnosis (where the chain broke, what kind of
  check failed, likely causes, next steps); timeline warnings carry one-line
  hints. `log_verify` gains `--json`. `VerifyReport` (also `POST /verify`)
  gains an additive structured `failure` object; the `error` string is
  unchanged.
- **Offline viewer**: shows where the chain broke with the same guidance,
  explains each note inline, and adds a "What verification proves — and what
  it can't" panel. New `docs/why_secure.md` plain-language explainer.
- **Compatibility**: legacy bundles (receipts without `auth_mode`/`window`)
  verify forever — pinned by a new `valid_envelope_legacy.json` fixture in
  both the Rust and JS suites. The one caveat: verifiers older than this
  release reject *new* bundles whose receipts carry the new fields (their
  receipt re-serialization drops unknown fields); verify new bundles with a
  current viewer/`envelope_verify`.

### RFC 3161 trusted timestamping (chain anchors)

- **New `log_anchor` CLI** anchors the witness chain head (or any export
  digest) at a public Time Stamping Authority: independent third-party
  proof of *when* the chain existed, removing the device clock — and even
  the device key — from the trust base for back-dating attacks. Online
  flow behind `--features tsa`; a query/import offline flow works
  air-gapped with no special build. `log_anchor verify` checks imprint
  consistency and chain membership in-tree and delegates the CMS
  countersignature to an independent implementation (`openssl ts
  -verify`). Anchors live in a new additive `tsa_anchors` table; the
  sealed-log schema and existing verifiers are untouched. Requests carry
  only a 32-byte digest plus a random nonce, and nothing in witnessd ever
  calls a TSA on its own (anchoring is operator/cron-initiated). See
  docs/timestamping.md.

### Physical tamper sealed into the witness chain

- **New `EventType::TamperDetected` / `ClaimKind::TamperDetected`**
  (`tamper_detected` in routes): tampering with the witnessing device itself
  — enclosure opened, camera covered/blinded, thermal-attack temp drift.
  Previously the Canary firmware signed tamper into its device-side chain
  but the kernel's sealed log never saw it (and the dedicated
  `securacv/<id>/tamper` MQTT topic had **zero publishers**).
- **Firmware**: the sensing witness callback now queues tamper alerts and
  the main loop publishes `{"state":"on","confidence":0..1,"kind":...}` on
  `securacv/<id>/tamper` (pending-flag pattern; re-arms on publish failure).
- **Adapter path**: `mqtt_sensor` / `webhook` allowlists include the new
  kind; example route in `adapter_host.example.toml`. Everything flows
  through the existing `Kernel::append_event_checked` gates — no new kernel
  surface.
- Specs updated (normative): `spec/event_contract.md` §11 vocabulary,
  `spec/sensor_adapter_contract_v0.md` §6 mapping.
- Compatibility: same statement as the heartbeat/lifecycle records — old
  DBs verify unchanged; old `log_verify` accepts new DBs; old
  `export_events` binaries error on records carrying the new event type.

### Logging & witnessd chain audit remediation

- **New sealed record types** `heartbeat` and `lifecycle`
  (spec/event_contract.md §12). One heartbeat per 10-minute bucket anchors
  the chain tail — previously, deleting the newest N records passed
  `log_verify` because checkpoints were only written when retention pruned.
  Lifecycle records seal daemon `start`/`shutdown_clean`; a boot that finds
  a trailing `start` seals a `PowerLoss` failure record (unclean-shutdown
  proxy). **Compatibility**: existing databases verify and read unchanged;
  databases written by the new witnessd still verify under older
  `log_verify` binaries (chain checks are payload-agnostic), but older
  `export_events` binaries will error on the new record types — upgrade
  tooling together with witnessd.
- **witnessd no longer dies silently on hardware faults**: a camera
  stall/disconnect is supervised (one sealed `GapMissingData` per outage
  after `ingest.failure_threshold_s`, reconnect with backoff, recovery
  visible in the next heartbeat) instead of crashing the daemon with no
  record; retention-enforcement and time-bucket errors are likewise
  witnessed instead of fatal. SIGINT/SIGTERM now seal a clean-shutdown
  record before exit.
- **Previously dead failure types now emitted**: `ClockSkew`
  (monotonic-vs-wallclock drift / bucket regression), `PowerLoss`
  (unclean-shutdown detection), `StorageFull` (free-space preflight,
  distinct from `StorageWriteFailed`). `SensorDisagreement` and
  `FirmwareIntegrity` remain explicitly deferred — no consensus/attestation
  infrastructure exists (docs/failure_semantics.md has the full mapping).
- **`log_verify` timeline audit**: warns on stale tails (possible tail
  truncation), missing heartbeat buckets, `created_at` regressions
  (softened when a ClockSkew record covers the jump), and back-dated or
  future-dated checkpoints. New `--strict` flag turns warnings into a
  non-zero exit. `VerifyReport` gains an additive `warnings` field (omitted
  when empty).
- **Operational log rate fixed**: the 5-second INFO health dump (~35k
  lines/day) is now transition-based — one WARN when ingest goes unhealthy,
  one INFO on recovery, a single key=value summary per
  `health.log_interval_s` (default 60s), detailed dumps at DEBUG. New
  docs/logging.md documents levels, volume, and RUST_LOG usage.
- New config sections (all defaulted, existing configs unchanged):
  `[health]` heartbeat/log_interval_s, `[storage]`
  min_free_mb/check_interval_s, `[clock]` skew_tolerance_s, and
  `[ingest]` failure_threshold_s/reconnect_backoff_max_s.

### Frigate zero-friction release (add-on 0.6.0)

- **Zero-config HA add-on**: the broker is auto-discovered from the
  Supervisor MQTT service (`services: mqtt:want`; explicit options still
  win), the device key is auto-generated and persisted (0600) when absent,
  `mqtt_publish.enabled` defaults to `true`, and the ingress Web UI is now
  a persistent status panel (chain badge, 24h digest, Verify Now, Lovelace
  dashboard generator) instead of a first-run-only wizard.
- **Docker sidecar** (`docker/sidecar/`, published as
  `ghcr.io/kmay89/securacv-sidecar`): one container (witness_api +
  frigate_bridge + event_mqtt_bridge + log_verify) for standalone Frigate
  users; only `FRIGATE_MQTT_HOST` is required. Includes an
  `entrypoint.sh doctor` diagnostic (broker reachability/auth, live
  Frigate traffic, sealed-log verification) and quickstart compose files.
  Repairs `integrations/ha_frigate_mqtt/docker-compose.yml`, which built a
  nonexistent Dockerfile.
- **One-click verification**: new `POST /verify` on the event API runs the
  full sealed-log check via the shared `verify_runner` (also the new core
  of `log_verify`); exposed in HA as `button.pwk_verify_now` +
  `binary_sensor.pwk_chain_problem`, with scheduled re-verification
  (`verify_interval_hours`, default 24).
- **Daily digest**: new `GET /digest` (rolling 24h, per-zone counts, 6-hour
  day periods, cached verify outcome — built solely from the
  privacy-filtered export path); exposed as `sensor.pwk_daily_digest` and
  deliverable via the new `docs/blueprints/securacv_daily_digest.yaml`
  blueprint (no entity-ID surgery).
- **Frigate reviews + topic prefix**: `frigate_bridge` can subscribe to
  `<prefix>/events` and `<prefix>/reviews` (`--frigate-topic-prefix`,
  `--enable-reviews`); the reviews parser now handles the real Frigate
  0.14+ before/after schema (the previous flat shape never matched live
  payloads). Compatibility statement: tested against the Frigate 0.14–0.17
  MQTT schema, with verbatim 0.17 fixtures in the test suite.
- **Fixes**: `event_mqtt_bridge` daemon no longer 401s after the
  10-minute capability-token rotation (token file re-read per request);
  `frigate_bridge` honors retention via `--retention-secs` instead of a
  hardcoded 7 days; `log_verify --device-key-seed` (env `DEVICE_KEY_SEED`)
  derives both the SQLCipher and verifying keys for operator-friendly
  verification of bridge-produced logs.

## [1.0.0] - Unreleased

### What v1.0 means

v1.0 means **everything documented works end-to-end**: every feature described in
the README/docs runs end-to-end, the install path succeeds on the first try, and
the test suite passes cleanly. This is the project's canonical definition of v1
(see `v1-roadmap.md`). It is **not** feature-complete — see "Explicitly deferred"
below — but nothing documented is allowed to be aspirational at the v1 tag.

### What's included

- **Privacy Witness Kernel** (Rust): hash-chained, Ed25519-signed append-only
  event log with break-glass N-of-M quorum access, vault sealing, event
  contract enforcement, module sandboxing (seccomp on Linux).
- **Vault sealing status is explicit at startup** (F-05): vault frame sealing is opt-in (it runs
  only when `BREAK_GLASS_SEAL_TOKEN` supplies a valid break-glass token). `witnessd` now logs whether
  sealing is ENABLED (with the crypto mode) or DISABLED at startup — and when disabled it states that
  boundary events are still signed/logged but no frame is sealed into the vault, plus how to enable
  it — so an operator is never silently led to believe evidence is being sealed when it is not.
- **DB key decoupled from the signing key** (F-04 / Stream B2 prerequisite): set
  `SECURACV_DB_KEY_SEED` and the SQLCipher key is derived from that independent secret
  (`resolve_db_encryption_key`) instead of the Ed25519 signing key, so the database key no longer
  pins the device identity — the storage-layer prerequisite for signing-key rotation.
  `rekey_database_file()` rotates the DB key itself in place (`PRAGMA rekey`). Backward compatible —
  without the env var, the legacy signing-key derivation is byte-identical, so existing databases
  open unchanged. (Full signing-key rotation additionally needs `device_metadata` identity-rotation
  support, which `Kernel::open` still pins; tracked as remaining Stream B2 work.) See
  [`docs/db_key_rotation.md`](docs/db_key_rotation.md).
- **CLI binaries**: 9 core — witnessd, log_verify, break_glass, export_events,
  export_verify, frigate_bridge, event_mqtt_bridge, witness_api,
  grove_vision2_ingest — plus the `adapter_host` daemon and `envelope_verify`,
  and the `demo` / `tamper_demo` / `ingest_run` / `detect_eval` helpers (15 total in `src/bin/`).
- **Sensor Adapter framework** (`src/adapter/`): an open, vendor-neutral interface that
  generalizes the `frigate_bridge` pattern so any source (acoustic/impulse, PIR/contact,
  presence, generic MQTT/webhook sensors, Frigate) can feed coarse, privacy-preserving claims
  into the same `append_event_checked` choke point — broad integration with no vendor lock-in and
  no new privilege. Includes the `adapter_host` binary (config-driven, one daemon, many adapters),
  Frigate + generic MQTT reference adapters, and the normative
  `spec/sensor_adapter_contract_v0.md` / `spec/witness_mesh_os_v0.md`. Expanded the event
  vocabulary with `acoustic_impulse_in_zone`, `presence_in_restricted_zone`,
  `vehicle_presence_after_hours`, `contact_state_change`, and `object_removed_from_zone`.
  - **Webhook ingress adapter** (`adapter-webhook`): a std-only HTTP `POST` listener so any
    device/script can register a sensor with a single `curl` — no MQTT broker required.
  - **Optional seccomp sandboxing** (`adapter-sandbox`, `with_sandbox(true)`): adapters can parse
    untrusted payloads inside the kernel's forked seccomp sandbox, upgrading the adapter audit
    boundary toward a security boundary for the parse step.
  - **Home Assistant surfacing**: the new claim types render in the "Last Event" sensor with
    friendly labels and per-type icons (`EVENT_TYPE_METADATA` in `const.py`).
  - **Webhook authentication + rate limiting + worker pool**: the webhook ingress (the one
    untrusted, network-facing surface) supports constant-time `Authorization: Bearer` or
    HMAC-SHA256 body-signature auth, per-path token-bucket rate limiting (`429`), and a bounded
    connection worker pool (`503` when saturated) that ends the unbounded per-connection thread
    spawn.
  - **BLE presence adapter** (`adapter-ble-presence`): turns ESPresense-style room-presence MQTT
    feeds into coarse presence claims, deliberately discarding device identity.
  - **Meshtastic LoRa-mesh adapter** (`adapter-meshtastic`): turns Meshtastic Detection Sensor
    Module nodes (PIR/contact/acoustic on a GPIO, alerting over LoRa) into kilometer-scale,
    off-grid witness sources via a gateway node's MQTT JSON uplink. Node ids are local routing
    keys only; positions, precise timestamps, RSSI/SNR, and alert text are never retained
    (export-scrub asserted in `tests/adapter_meshtastic.rs`). Inbound only; the outbound and
    LoRa-transport directions are specified in `docs/meshtastic_integration.md`.
  - **Adapter observability**: per-adapter counters (polls/emitted/sealed/filtered/rejected +
    last-seal time) on the host, a periodic stats log, and an optional read-only `/stats` +
    `/healthz` HTTP endpoint (`stats_addr`) — operational counts only, never event content.
  - **Webhook TLS** (`adapter-webhook-tls`): optional rustls TLS on the webhook listener
    (`tls_cert`/`tls_key`), so bearer tokens aren't sent in clear on non-loopback deployments.
  - **HMAC replay protection**: opt-in `X-Timestamp` + `X-Nonce` bound into the signature
    (`hmac_replay_window_secs`), rejecting replayed or stale signed requests.
  - **Home Assistant native adapter-stats sensor**: configuring an "Adapter Host stats URL" adds a
    diagnostic sensor (per-adapter counters as attributes) via a dedicated coordinator — no
    hand-written YAML needed.
  - **Parser fuzz sweep** (`tests/adapter_parser_fuzz.rs`): seeded, panic-free robustness tests
    over the untrusted webhook/mqtt/BLE/Frigate/Meshtastic parsers.
  - **Webhook mutual TLS**: optional client-certificate auth (`tls_client_ca`) — machine-to-machine
    sensors authenticate by certificate, with no shared secret on the wire.
  - **Prometheus metrics**: the stats endpoint serves `/metrics` (text exposition format) alongside
    JSON `/` and `/healthz`, for Grafana/Alertmanager scraping.
  - **SIGHUP config hot-reload**: `adapter_host` reloads `min_confidence` and each adapter's
    route/room/filter attributes (and webhook paths) live on SIGHUP, without restarting listeners
    or dropping connections; changing an mqtt_sensor's subscribed topic, or adapter topology,
    still requires a restart (and is logged).
- **Home Assistant integration** (HACS): 3 setup modes (MQTT / Kernel HTTP /
  both), MQTT auto-discovery, device PKI trust management (TOFU + manual pin +
  rotation), 5 sensor types, 11 binary sensor types (tamper + transport),
  Ed25519 signature verification, diagnostics, and a bundled **verified-✓
  timeline Lovelace card** (with a pure-YAML fallback for those who prefer
  built-in cards — see `docs/lovelace_timeline.md`).
- **Home Assistant add-on**: first-run setup wizard with preflight checks,
  camera TCP test, Frigate config generation, post-setup health verification,
  two operating modes (Frigate integration, standalone RTSP).
- **Install script**: single `curl | bash` command installs Mosquitto, Frigate,
  integration, add-on, generates device key, deploys automations + dashboard.
- **Firmware** (ESP32): canary-vision (ESP32-C3 + Grove Vision AI V2),
  canary-wap (XIAO ESP32-S3 Sense) — BLE discovery, Chirp community alerts,
  Beacon harm-reduction broadcast, Opera mesh networking, OTA updates.
- **Detection backends**: stub (testing), CPU (background subtraction),
  Tract ONNX (local inference).
- **Frame sources**: RTSP (GStreamer/FFmpeg), V4L2, ESP32 HTTP, local files.
- **Automations**: daily digest, pattern-break alerts, integrity failure alerts.
- **CI**: Rust tests + clippy, firmware builds, HACS/hassfest validation,
  SBOM generation, secrets scanning, CodeQL analysis, release workflow. Two
  real-decode ingest gates: `ingest-ffmpeg` (file → signed log) and
  `ingest-rtsp`, which serves the committed fixture over RTSP (MediaMTX + ffmpeg
  publisher) and drives the real `RtspSource` end-to-end through
  decode → detection → signed events → verify (`tests/rtsp_e2e.rs`). A third
  end-to-end gate, **`frigate-mqtt-e2e`**, covers the Frigate → MQTT path:
  `tests/frigate_mqtt_e2e.rs` drives a `frigate/events` payload through the bridge
  pipeline into a SQLCipher-encrypted sealed log and verifies it with the real
  `log_verify` binary, while the CI job runs the real `frigate_bridge` ingesting a
  message from a live mosquitto broker (`integrations/ha_frigate_mqtt/ci_smoke.sh`).
  `verify_pipeline.sh` was corrected (it queried the encrypted DB with plain
  sqlite3, expected vault envelopes the bridge never creates, and a break-glass
  export bundle nothing generates) and is now an honest manual operator smoke check.
- **Security docs**: the **audit boundary vs security boundary** distinction is now
  stated authoritatively in `docs/security/THREAT_MODEL.md` (*Trust Boundaries*):
  which producer surfaces (`DetectorBackend`, `SensorAdapter`, the `InferenceView`
  handoff) are hand-audited contracts vs. the mechanically enforced security
  boundary (the three fail-closed gates in `Kernel::append_event_checked`). Closes
  the corresponding v1 acceptance item.
- **Firmware privacy hardening (no raw MAC / no precise GPS, all trees)**: the salted, MAC-free
  device pseudonym and GPS coarsening are now shared helpers in `firmware/common/`
  (`identity/device_pseudonym.h`, `gnss/gps_privacy.h`) adopted across every firmware tree.
  `canary-vision` no longer leaks the efuse MAC in its MQTT client ID or boot banner (it shows a
  salted "Hardware ID" instead); `canary/src` routes all operator-facing lat/lon through
  `gps_coarsen_deg()` (3 dp ≈ 110 m); the `canary-wap/src` scaffold no longer reads the MAC. The
  `regression_check.sh` privacy guardrail now **hard-fails** on raw MAC or un-coarsened lat/lon in
  *any* tree (previously a per-tree warning), and both helpers are host-tested
  (`tests_host/test_device_pseudonym_common.cpp`, `test_gps_coarsen.cpp`). Closes the v1 firmware
  invariant gate.

### Explicitly deferred (not in v1.0)

- Multi-camera standalone mode (currently single-camera only in standalone;
  Frigate mode supports multiple cameras via Frigate's own config)
- LoRa transport
- SCQCS audio transport
- CAP gateway interop (specification exists, implementation deferred)
- GPU-accelerated detection
- Tract detection confidence threshold override (hardcoded at 0.5)
- Pre-built Docker images on ghcr.io / Docker Hub

### Known limitations

- v1 e2e pipeline verification (`verify_pipeline.sh`) requires a live
  docker-compose stack with Frigate + Mosquitto — not automated in CI.
- The HA add-on builds from source inside the container, which is slow on
  first install (~5-10 min on Pi 4). Pre-built images are planned for v1.1.
- Standalone RTSP mode processes one camera at a time. For multi-camera,
  use Frigate mode.

## [2.1.0] - 2026-05-27

### Added — Production Feature Plan (Phases 0-6) for ESP32-S3 Canary firmware

Seven-phase plan completing the firmware's production-readiness across both
PlatformIO (canary/) and Arduino WAP (canary-wap/) builds with full parity.

**New PlatformIO libraries:**

- **securacv_power** — Battery ADC (2:1 voltage divider on GPIO 1), 16-point
  LiPo discharge curve for SoC, software inference fallback, charge state
  machine with hysteresis, graceful brownout shutdown, battery health history
  persisted to NVS (charge cycles, voltage extremes, brownout count).
- **securacv_power_policy** — 6-mode runtime state machine (PLUGGED_IN,
  BATTERY_NORMAL, BATTERY_SAVER, LOW_POWER, SHUTDOWN, USB_ONLY). Per-mode
  CPU frequency scaling, WiFi power save, record interval tuning, progressive
  feature gating. Deep sleep cycling in emergency mode.
- **securacv_setup** — First-boot captive portal with DNS hijack, device
  naming, 15-minute timeout. NVS flag persists setup completion.
  - **Stays-connected onboarding**: OS connectivity probes are answered
    per-platform so the phone never flags the AP "no internet" and
    disconnects mid-setup — Apple gets the instruction page (Captive Network
    Assistant sheet), Android gets `204 No Content`, Windows gets the exact
    NCSI bodies. The captive DNS redirector runs for the whole life of the
    always-on AP (not just first boot), so rejoining the management AP after
    provisioning works too. The redirector answers only `A` queries and
    returns NODATA for `AAAA`/`HTTPS`, so `canary.local` resolves promptly on
    Android Chrome; `192.168.4.1` is the always-works fallback. The pure
    response logic is extracted into host-unit-tested headers — the DNS builder
    (`captive_dns.h`) and the per-platform probe policy (`captive_probe.h`,
    driving a single `handle_captive_probe` handler) — both run in CI.
- **securacv_diagnostics** — Heap monitoring (free/min/largest block/PSRAM/
  stack HWM/fragmentation), 3-level automatic feature degradation with 5KB
  hysteresis, SD health tracking (atomic write/error counters, space warnings),
  10-test boot self-test suite (NVS, heap, PSRAM, crypto, SD, WiFi, temp,
  uptime, watchdog, chain).
- **securacv_ble_status** — NimBLE GATT server with standard Battery Service
  (0x180F) and custom SecuraCV service exposing device name, firmware version,
  chain sequence, health score, degradation level, uptime, SD usage over BLE.
- **securacv_data_mgmt** — SD log rotation (witness 500, health 200, auto at
  85% SD usage), chain backup/restore with HMAC-SHA256 integrity (keyed by
  device private key), chain integrity verification (Ed25519 + hash continuity,
  capped at 100 records with watchdog yield), witness record export to /EXPORT/.

**New WAP header-only ports (full parity):**
power_monitor.h, power_policy.h, ble_status_api.h, data_mgmt_api.h, plus
sys_monitor.h enhanced with heap degradation levels and SD health tracking.

**New REST API endpoints (both builds):**
- `GET /api/diagnostics` — full diagnostic snapshot as JSON
- `GET /api/selftest` — re-run self-test suite on demand
- `GET /api/battery/history` — NVS-persisted battery health stats

**New serial commands:** `b` (battery), `p` (power policy), `d` (diagnostics),
`r` (data management).

**New feature flags:** FEATURE_POWER_MONITOR, FEATURE_POWER_POLICY,
FEATURE_SETUP_WIZARD, FEATURE_DIAGNOSTICS, FEATURE_BLE_STATUS,
FEATURE_DATA_MGMT.

### Security hardening

- Chain backup uses HMAC-SHA256 (was CRC-32) — prevents SD-level forgery
- Chain verification capped at 100 records with delay(1) yield — prevents
  watchdog timeout on large directories
- BLE GATT characteristics are read-only (no write/auth bypass possible)
- New REST endpoints (/api/diagnostics, /api/battery/history) auth-gated with
  rate limiting. Note: WAP's /api/selftest is intentionally unauthenticated
  (reachable on the captive-portal AP during setup, by design)
- Power policy rejects manual override to LOW_POWER/SHUTDOWN (anti-blinding)

## [0.5.0] - 2026-05-12

### Added — harm-reduction broadcast layer (Beacon channel) + audit artifacts

- **Beacon channel** (`spec/beacon_channel_v0.md`,
  `firmware/projects/canary-wap/arduino/canary_wap/beacon_channel.{h,cpp}`):
  Smoke-detector-grade neighborhood harm-reduction broadcast layer with
  two-pubkey cryptographic co-signing on every origination, NFPA-72-style
  supervised health state (`Normal / Trouble / Alarm / Supervisory`),
  CAP-aligned wire fields, narrow life-safety-only template set (~13
  templates), daily Ed25519-signed self-test heartbeat with 36 h Trouble
  threshold, append-only chain-hashed audit log as a ring buffer
  NVS-persisted under the flash-encryption gate. Off by default
  (`FEATURE_BEACON_CHANNEL=0`).
- **Beacon REST API** (`beacon_api.h`): Bearer-token-gated surface —
  `/api/beacon`, `/set`, `/pair/*`, `/revoke`, `/originate`, `/cosign`,
  `/cancel`, `/active`, `/audit` (paginated), `/selftest`.
- **COSIGN_REQ/RESP encryption**: X25519 ECDH between paired device
  pubkeys, HKDF-SHA256 domain-separated key derivation
  (`securacv:beacon:cosign:v0`), ChaCha20-Poly1305 AEAD. X25519 keypair
  NVS-persisted across reboots.
- **Distinct Beacon airtime telemetry** in `airtime_governor`.
- **HA MQTT discovery** for both Chirp + Beacon NFPA states.
- **Audible `PATTERN_BEACON`** (1200/1700/2200 Hz ≤600 ms sequential),
  deliberately distinct from any reserved emergency-broadcast tone.
- **CAP gateway interop spec** (`spec/beacon_cap_gateway_v0.md`) —
  specification only; implementation deferred.
- `docs/audit/mesh_and_chirp_audit_v1.md` — full audit of Opera mesh +
  Chirp channel with per-finding traceability.
- `docs/audit/v0.3_closeout.md` — closure summary mapping each finding
  to source location, test, and PR.
- `docs/audit/hardware_verification_checklist.md` — outstanding
  hardware-bound verification recipes for the QA team.
- `docs/research/harm_reduction_prior_art.md` — CAP, IPAWS/WEA/EAS,
  NFPA 72, MUTCD DMS, Hawaii 2018 false-alert, harm-reduction movement,
  Meshtastic / GoTenna prior art.
- Host tests: `test_chirp_protocol_invariants.cpp`,
  `test_chirp_security.cpp`, `test_beacon_origination.cpp`,
  `test_mesh_opera_security.cpp` — per-finding regression coverage.
- CI lints: `scripts/lint_no_impersonation.sh` (reserved phrases /
  tones / colors) and `scripts/lint_cap_mapping.sh` (CAP template
  coverage).

### Changed — Chirp v0.2 hardening (closes audit C1–C17)

- End-to-end Ed25519 signature verification on every received witness /
  ACK / suppress-vote.
- `confirm_count` no longer carried on the wire; receivers track
  confirmations locally as a set of unique confirmer `session_pubkey`s.
- Relayers re-sign with their own session key; original signer's
  pubkey + signature preserved in `signed_origin` envelope so
  downstream receivers verify end-to-end.
- Signed `CHIRP_MSG_SUPPRESS_VOTE` wired end-to-end.
- Priority storage for received chirps — EMERGENCY survives a flood.
- 4 KB / 4-hash Bloom-filter nonce dedup with periodic reset.
- Wall-clock-anchored timestamps; origination refused when SNTP
  unsynced; conservative night-mode when unsynced.
- 5-emoji session display (~1 M distinct).
- `chirp_api.h` REST endpoints Bearer-gated via the standard
  template-trampoline pattern; wired into `canary_wap.ino`.
- Presence requirement also gates ACK origination.
- Per-`session_pubkey` rate limit on incoming witnesses.
- `TPL_AUTH_FEDERAL_PRESENCE` removed; 0x04 slot reserved.
- `PROTOCOL_VERSION` bumped from 0 to 1.

### Changed — Opera mesh v0.2 hardening (closes audit O1–O3)

- Message freshness anchored on per-peer monotonic counter; wall-clock
  TTL retired.
- `opera_secret` NVS persistence requires flash encryption enabled.
- `remove_peer()` now executes a full transactional rekey:
  generate new `opera_secret`, encrypt under each surviving member's
  session key, wait for ACKs, commit on all-ACK or 60 s timeout.

### Security

- Non-impersonation contract CI-enforced. No reserved
  emergency-broadcast phrases, no reserved-tone audio pair, no pure
  red as a primary alert color in any alert/chirp/beacon firmware or
  UI source.
- Beacon `scope = Private` always.
- No PII on the Beacon wire — templates only.
- 10 hardwired Beacon-channel invariants added to `AGENTS.md`.

## [0.4.0] - 2026-02-18

### Added
- **BLE Discovery subsystem** for Canary firmware (Opera/Chirp/Nearby):
  - **Opera**: BLE server advertising with SecuraCV custom GATT service — privacy-safe device
    identifier derived from Ed25519 pubkey hash, read-only status characteristics, writable
    command characteristic
  - **Chirp**: Connectionless BLE broadcast alerts between Canary devices — manufacturer-specific
    advertising data with truncated chain hash, coarsened timestamps, rate-limited (10s minimum)
  - **Nearby**: BLE scanner running on dedicated FreeRTOS task — discovers other Canaries via
    service UUID, tracks RSSI for proximity, thread-safe with mutex-protected shared state
  - New firmware files: `ble_config.h`, `ble_opera.h`, `ble_chirp.h`, `ble_nearby.h`, `ble_manager.h`
  - `FEATURE_BLE` compile flag in `build_config.h` (disabled in MINIMAL/DEV, enabled in FULL)
  - HTTP API endpoints: `GET /api/ble/status`, `GET /api/nearby`, `POST /api/chirp/send`
  - Web UI: BLE Discovery tab in Community panel with signal strength bars, nearby Canary list,
    chirp send buttons
  - NimBLE-Arduino library dependency (lighter than bluedroid, ~60% less RAM)
  - BLE protocol specification: `docs/ble_protocol.md`
  - BLE semantic events added to `spec/event_contract.md`

### Security
- BLE uses NimBLE only (no Bluetooth Classic — smaller binary blob surface)
- Device identity from Ed25519 pubkey hash, not hardware MAC address
- Non-Canary BLE devices counted only, never individually logged (privacy by default)
- All BLE code gated behind feature flag — compiles out completely when disabled
- Graceful degradation: firmware continues if BLE hardware unavailable

## [0.3.1] - 2026-01-21
### Fixed
- `log_verify` now verifies break-glass receipt chain (called from `main`)
- Receipt verification uses `[u8; 32]` device key signature input and supports `--verbose`

All notable changes to the Privacy Witness Kernel will be documented in this file.

## [0.2.0] - 2026-01-21

### Added
- **Frame isolation layer** (`src/frame.rs`):
  - `RawFrame`: Opaque container with private bytes (no Clone, no AsRef<[u8]>)
  - `InferenceView`: Restricted interface for modules (cannot export bytes)
  - `FrameBuffer`: Bounded ring buffer with build-time caps (30s, 300 frames)
  - `Detector` trait: Modules run inference without capturing pixel data
  - `StubDetector`: MVP motion detection via pixel hash comparison
  - `BreakGlassToken`: Placeholder for quorum-gated vault access

- **Ingestion layer** (`src/ingest/`):
  - `RtspSource`: Stub RTSP source with synthetic frames
  - `RtspConfig`: Configuration for RTSP streams
  - Timestamp coarsening at capture time
  - Non-invertible feature hash computation at capture time

- **Runtime improvements**:
  - `env_logger` for structured logging
  - Frame buffer stats logging
  - Verbose mode for `log_verify`
  - Conformance alarm checking in `log_verify`

### Changed
- `Module` trait now receives `InferenceView` instead of `Frame`
- `ZoneCrossingModule` uses `StubDetector` for motion detection
- `witnessd` uses `RtspSource` and `FrameBuffer` for frame handling

### Security
- Raw bytes are now physically inaccessible to modules (type-level enforcement)
- Frame buffer auto-zeroizes on drop and eviction
- Only path to raw bytes is `RawFrame::export_for_vault()` requiring `BreakGlassToken`

## [0.1.2] - 2026-01-21

### Fixed
- `validate_zone_id()` regex now compiled once via OnceLock
- Added negative test for module event-type allowlist rejection

## [0.1.1] - 2026-01-21

### Added
- `ReprocessGuard` wired into `read_events_ruleset_bound()`
- `conformance_alarms` actively written on contract/module violations
- `RawMediaBoundary` choke point scaffold
- Runtime module event-type authorization via `ModuleDescriptor`

### Changed
- Zone ID validation: blocklist → strict allowlist regex

## [0.1.0] - 2026-01-20

### Added
- Initial kernel: sealed log, contract enforcer, bucket key manager
- `witnessd` daemon and `log_verify` tool
- Spec documents: invariants, event contract, threat model, architecture