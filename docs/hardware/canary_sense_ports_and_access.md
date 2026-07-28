# Canary Sense (60 GHz radar) — which port flashes, and opening the case

The Sense is the Seeed **MR60BHA2 60 GHz mmWave kit**: a radar carrier board
with a **XIAO ESP32-C6 seated in its socket**. Two boards, one appliance — and
that is the whole source of confusion, because the port you reach for every
day is not the port that flashes.

Same shape as the Vision's two-port trap
([`grove_vision_ai_v2_guide.md`](./grove_vision_ai_v2_guide.md)), and it fails
the same silent way: you plug into the reachable port, nothing appears in the
device chooser, and the natural conclusion — *my board is dead* — is wrong.

## Which port does what

| What you want to do | Where the cable goes | Which chip answers |
|---|---|---|
| **Flash the firmware** (any install, any reflash) | **The XIAO's own USB-C** — the small board in the socket | ESP32-C6 |
| Serial console / the USB radar bench | **The XIAO's own USB-C** | ESP32-C6 |
| Everyday power, once it's set up | The port you normally leave plugged in | *(power only — nothing to talk to)* |

Everything SecuraCV does over USB — flashing, the provisioning that writes
your Wi-Fi into NVS, the live radar console — talks to the **XIAO's** USB
Serial/JTAG peripheral. There is no path from the power port to the ESP32-C6's
USB, so a flash attempt from there either shows no device at all or shows
something that isn't your Canary. Nothing is harmed; it simply cannot work.

## Do you have to open it?

**Depends on the enclosure — and on the SecuraCV printed radome, no.**

- **Printed SecuraCV radome:** the XIAO's USB-C is routed out through the
  **bottom wall**. In
  [`enclosure/canary_sense_enclosure.scad`](./enclosure/canary_sense_enclosure.scad)
  that opening is cut in `back()`, and the sealed preset's `usb_cover` is a
  **shallow weather recess around** the opening (also a cut, ~1 mm deep) — not
  a lid over it. Plug straight in; don't take anything apart.
- **Stock kit case or any build that swallows the XIAO:** the port is inside,
  so you do have to open it to flash.

Do not remove screws you don't have to: the printed posts take **≤ ~0.3 N·m**
before they strip (use M2 heat-set inserts if you expect to open it often).

Then, either way:

1. **Plug the USB-C data cable straight into the XIAO** — the small board, not
   the big radar board. A charge-only cable will power it and never enumerate;
   the BOM calls for a data-capable one for exactly this reason.
2. **Flash.** If the flasher can't see it, use download mode by hand: hold
   **BOOT**, tap **RESET**, release BOOT.
3. **Go back to the power port** for everyday running.

### If you did open it: the radome rule

Keep the radar's front window clear — **no foil labels, no metal, nothing
added in front of the antenna**, and don't swap in a CF-filled filament. 60 GHz
has to pass through that face, and the window thickness is tuned (≈1.5 mm in
PETG/ASA) rather than merely thin; the quarter-wave band around 0.7–1.1 mm
reflects up to ~20 % back into the antenna and corrupts the µm-scale breathing
phase. Full derivation in the enclosure header.

## Why both flashers say this before you connect

The in-browser flasher and the desktop Flasher app share no UI code, so this
lives once in the catalog — `products[].access`, from `BOARD_ACCESS` in
[`gen_flash.py`](../../canary-local/tools/gen_flash.py) — and each frontend
renders it in its **connect** step, before the cable goes in. A family with no
`access` block gets no card at all: every other Canary is one cable and done,
and inventing ceremony for those would just be noise.

If a future board hides its flashing port the same way, add a `BOARD_ACCESS`
entry for its family and both flashers pick it up.
