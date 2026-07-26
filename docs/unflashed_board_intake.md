# Unflashed boards — what protects you, and what can't

Status: Normative
Last Updated: 2026-07-26

We tell people to buy a bare ESP32 board and flash it themselves. That is a
real advantage — you can see the silicon, there's no vendor cloud, and the
supply chain is a commodity part instead of a sealed appliance. It also means
the board arrives carrying **somebody else's flash contents and somebody
else's eFuse state**, from a seller we don't control.

This document is the honest account of that: what the risk actually is, what
the flashers do about it, and the parts nothing we ship can reach.

## The thing that cannot work

> "Open our app first, and it acts as a shield when you plug the board in."

It can't, and we should never imply it does. USB enumeration happens in the OS
kernel. By the time Web Serial hands the browser a port — or the desktop app
sees a serial device appear — the device has already introduced itself to the
operating system with whatever descriptors it chose, and any code it wanted to
run on plug-in has run. Chrome doesn't even offer the port until the user picks
it from a chooser, which is later still.

An app that claimed to stand in front of that would be selling a feeling. What
we ship instead is the instruction that genuinely works, and the checks that
are honestly possible after the fact.

## The thing that does work: bring it up cold

**Hold BOOT while you plug the cable in.**

With GPIO0 held low at reset, the chip enters the ROM's download mode. The
second-stage bootloader and the resident application never execute a single
instruction. This is enforced in mask ROM — the same unerasable first stage
that makes the board impossible to brick from the flasher
([browser_flasher.md § Why it's safe to offer](browser_flasher.md)) — not by
anything we wrote.

That matters most because of one specific threat. These are native-USB parts:
an ESP32-S3 in TinyUSB mode can enumerate as a **keyboard** and type at your
desktop the moment it's plugged in. In ROM download mode it presents the
mask-ROM USB-Serial-JTAG descriptor and nothing else, because no code of the
vendor's is running to ask for anything different.

So: **never plug an unflashed board into your computer without holding BOOT.**
That one line closes the entire class.

Both flashers now teach this before the Connect step, and
`canary-local/tests/desktop_parity.test.js` fails the build if either one drops
it.

## What an unflashed board can actually do

| Threat | Real? | What answers it |
|---|---|---|
| HID injection at plug-in (BadUSB) | **Yes** — native USB, TinyUSB mode | The cold-start gesture. Nothing else. |
| Wi-Fi beaconing, joining open APs | Yes, but it holds none of your credentials | Cold start; then the full erase |
| Hostile firmware surviving our install | Only via a *partial* write | Forced full erase on first contact |
| Relabelled flash (4 MB sold as 16 MB) | Common | The alias probe |
| eFuses already burned by a prior owner | Yes — and **erase cannot undo it** | The block-0 read → refuse the board |
| Cloned part with a bogus MAC | Yes | Structural MAC checks + duplicate detection |
| A modified circuit board | Yes | **Nothing.** Out of software's reach entirely. |
| Persisting past a full erase into our firmware | **No** — mask ROM is unwritable, and the device identity keypair is generated on first boot *after* the erase | — |

## The intake check

`canary-local/assets/intake.js` holds the whole decision, pure and unit-tested;
`flash.js` does the I/O and renders it. Every probe is a **read**. The flasher
still issues no eFuse burn command at all, which is what the un-brickable
promise rests on.

### 1. Security eFuses (block 0)

The strongest signal available, and the cheapest. A factory-fresh part reads
every security field as unset, so **anything set here was set by a person**.

Bit offsets come verbatim from ESP-IDF's
`components/efuse/<chip>/esp_efuse_table.csv`, and the test re-asserts them
against those numbers so a transcription slip fails in CI rather than on
someone's desk. `EFUSE_BASE` is not hardcoded — it's read from
`esploader.chip.EFUSE_BASE`, so it always agrees with the esptool build we
vendored. Block 0 is at `EFUSE_BASE + 0x02C`, six words.

Fields that mean **stop** — the board can't become a Canary, and no erase will
change that, because eFuses burn one way only:

- `SECURE_BOOT_EN` — locked to somebody else's signing key
- `SPI_BOOT_CRYPT_CNT` (odd parity) — flash encrypted with a key burned into
  this chip
- `DIS_DOWNLOAD_MODE` — the USB recovery path is gone
- `ENABLE_SECURITY_DOWNLOAD` — reduced command set, so we can't even verify
  what's on it

Fields that mean **not factory-fresh** — worth knowing, not fatal:
`SECURE_BOOT_AGGRESSIVE_REVOKE`, `DIS_DOWNLOAD_MANUAL_ENCRYPT`, `DIS_PAD_JTAG`,
`SOFT_DIS_JTAG`, the USB-JTAG disables, and a non-zero `SECURE_VERSION`
(an anti-rollback floor that may refuse our firmware).

Two deliberate omissions, both to avoid crying wolf on honest boards:

- **`WR_DIS` and `RD_DIS` are not read as signals.** Espressif *does* burn
  eFuses at the factory — flash/PSRAM capability, wafer version — and write-
  protects them. Treating a non-zero `WR_DIS` as "used" would flag every board
  ever made.
- **`DIS_DIRECT_BOOT` is not in the table.** We were not able to confirm its
  factory default across all three chips, and a field whose virgin value we
  aren't sure of is worse than no field at all.

A chip with no verified table (anything outside S3 / C3 / C6) reports
`supported: false` — skipped, explicitly, rather than guessed at and reported
clean.

### 2. Is the flash the size it claims?

A flash die reports its capacity in its JEDEC id, and a relabelled part simply
lies: address lines above the real capacity wrap, so the top of the "16 MB"
chip mirrors the bottom of the real 4 MB one. We read 4 KB from each end and
compare — no writing.

The probe is only conclusive when offset 0 holds something distinguishable. On
a blank chip both ends are `0xFF` and match legitimately, so the verdict is
**inconclusive**, never an accusation.

### 3. The MAC

We assert only what is definitional for an IEEE-assigned station address:
all-zero/all-ones, the multicast bit, the locally-administered bit. We
deliberately do **not** check membership of an Espressif OUI list — we can't
ship a list we're able to keep verifiably correct, and a stale one would fail
honest boards. A clone that slips past those three is caught instead by the
duplicate check against the session roster: two "new" boards sharing one
address is a far louder signal than any vendor list.

### 4. What it arrived running

Matched on the `esp_app_desc_t` project name — a real string read off the
board. MicroPython, CircuitPython, ESP-AT, Arduino, ESPHome, Tasmota and the
usual factory blink sketches are named as known stock. Anything else is
reported as *unrecognized*, which is an invitation to look at the backup before
erasing, not an accusation.

We do **not** claim a corpus of known-good image hashes. We don't have one, and
inventing hashes we can't verify would be worse than saying nothing.

## The forced erase

This is the change with the most practical value, and it fixes a real gap:
full erase used to be an **unchecked Advanced checkbox** described as "use it
if a board is misbehaving." A normal install writes only the regions the image
covers, so anything a previous owner left in a partition we don't touch would
ride straight through onto a board the user now believes is theirs.

On **first contact** — a board with no SecuraCV firmware we can read, and not
one onboarded earlier this session — the full chip erase is now mandatory and
the Advanced toggle cannot turn it off. The confirm card says why.

## Where the two flashers differ, and why we say so

Per CLAUDE.md's "two flashers, two frontends" rule, both get the cold-start
gesture and the forced erase. One check is genuinely browser-only:

**The desktop app cannot read eFuses.** It drives the `espflash` CLI as a
sidecar, and `espflash` has no fuse-read command. Rather than silently omit
it — a missing check reads as a passed check — the desktop app states the gap
on the connect step. Closing it means either bundling `espefuse` or waiting for
`espflash` to grow the command.

The desktop app also can't read what firmware is resident (`espflash
board-info` reports chip, revision, crystal and MAC, not the app descriptor),
so its first-contact decision is a **checkbox that defaults to on** rather than
a reading. The safe default for a board of unknown provenance is to wipe it.

**Known gap:** the desktop app's *local-file* flash path
(`flash_local_file`) does not yet take the first-contact erase. The catalog
flash path does. Tracked as a follow-up.

## After the flash

Once our image is on it, the trust anchor is ours: SHA-256 and the Ed25519
release signature verified before a byte is written, the chip's own MD5 read
back after. The device identity keypair is generated on first boot *after* the
erase, so nothing pre-existing carries into the device's identity.

If you want to be the last person who can ever do this to that board, the
Phase 2 lockdown in [secure_provisioning.md](secure_provisioning.md) burns
secure boot and flash encryption yourself. It is irreversible, so it is an
explicit opt-in and never a default.

## What this does not do

Stated plainly, because the checks above can otherwise read as more than they
are:

- **Nothing here detects a modified circuit board.** An extra part, a rewired
  USB path, a substituted module — no read over USB can see any of it. Photos,
  weight, and buying from a distributor with real provenance are the only
  answers, and they're weak ones.
- **We cannot attest that the silicon is genuine Espressif.** No host-side
  mechanism exists. A perfect clone that answers every probe correctly passes.
- **A board that arrives already secure-booted is rejected, not repaired.**
  There is no un-burn.
- **"Nothing looks wrong" is not "proven safe."** It means the fuses are
  untouched, the flash is the size it claims, and the MAC is well-formed. It is
  a floor, not a guarantee.

## See also

- [browser_flasher.md](browser_flasher.md) — the flasher's trust model and the
  mask-ROM property everything here leans on
- [supply_chain_transparency.md](supply_chain_transparency.md) — the same
  posture applied to *our* binaries: signed provenance you can verify
- [secure_provisioning.md](secure_provisioning.md) — Phase 2 lockdown, if you
  want to close the door behind you
- `canary-local/assets/intake.js` — the decision logic
- `canary-local/tests/intake.test.mjs` — the eFuse offsets, re-asserted
