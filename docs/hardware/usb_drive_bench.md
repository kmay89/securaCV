# USB-OTG bench build — branded name + browse-on-iPhone drive

A single **opt-in** firmware build (`canary-wap-usbdrive`) that turns on the two
USB things the stock image can't do, so they can be **validated on real
hardware** before anything ships. This is the Phase-2 on-device step the
`usb_evidence_drive` / branded-USB notes have been waiting on — run it and record
the result at the bottom.

> **Not a release profile.** It flips the S3 into USB-OTG/TinyUSB mode, which
> changes how the board enumerates for flashing. Keep it to a bench board.

## What it enables

1. **Branded USB name.** The board's USB descriptor reads **`SecuraCV-Canary`**
   (manufacturer `SecuraCV`) — visible in Chrome's Web Serial chooser, Windows
   Device Manager, and `system_profiler SPUSBDataType` on macOS. Set by the
   `-DUSB_MANUFACTURER` / `-DUSB_PRODUCT` build flags (a runtime
   `USB.productName()` lands too late under CDC-on-boot).
2. **A read-only USB drive** (the "evidence drive"): the SD card's witness files,
   browsable from a computer **or an iPhone/iPad over USB-C↔USB-C in the Files
   app**. `FEATURE_USB_EVIDENCE_DRIVE=1`.

## Hard limits (know these before you test)

- **ESP32-S3 only.** The C3/C6 Canaries (`canary-vision`, `canary-sense`) have
  **no USB-OTG** peripheral — they can never present a drive or a branded name
  over USB. This build is WAP/S3.
- **The SD must be FAT32 or exFAT.** iOS Files mounts exFAT/FAT32/HFS+/APFS. A
  differently-formatted card simply won't appear.
- **The update drop-zone stays computer-only.** The PSRAM drop-zone is FAT16
  (iOS won't mount FAT16), and it can't become FAT32 — a spec-compliant FAT32
  needs ≥ ~33 MB and the PSRAM staging volume is 4 MB. So *browsing* witness
  files works on iPhone; *drop-a-.bin* updates remain a Mac/PC path.
- **Power.** Over USB-C↔USB-C the **iPhone is the host** and gives little power;
  a Canary running WiFi/camera can trip iOS's "accessory needs too much power."
  Power the Canary from its **own** source (or a powered hub that passes data to
  the phone).
- **Reflashing.** Because runtime USB is now TinyUSB, the one-click web flasher
  may not auto-enter download mode — use the **BOOT+RESET** gesture to reflash.
  (Download mode itself always works; it's the ROM USB-Serial-JTAG.)

## Build & flash

```sh
cd firmware/projects/canary-wap
pio run -e canary-wap-usbdrive                 # compile (CI compiles this too)
pio run -e canary-wap-usbdrive -t upload       # flash a bench S3 WAP board
```

Then format the SD as **FAT32** (≤32 GB) or **exFAT** (larger) and seat it.

## Bench checklist (record the result)

Plug the board (self-powered) into a computer first, then an iPhone via USB-C.

- [ ] **Enumerates in OTG mode** without boot-looping.
- [ ] **Name:** Chrome `chrome://device-log` / Web Serial chooser / Windows / macOS
      `system_profiler SPUSBDataType` shows **`SecuraCV-Canary`** by `SecuraCV`.
- [ ] **Computer drive:** the SD appears as a read-only volume; witness files
      are browsable.
- [ ] **iPhone (USB-C↔USB-C):** the drive appears in **Files → Browse → Locations**
      and the witness files open. (Self-powered; SD is FAT32/exFAT.)
- [ ] **Reflash still works** via BOOT+RESET → the web flasher / esptool.
- [ ] **Serial console** still comes up every boot (TinyUSB CDC).

If all six pass on real hardware, this stops being "coded but unproven": we can
then decide whether to surface it as a flasher option and/or promote the branded
descriptor toward a default. If any fail, note which — that's the finding.

## Result log

| Date | Board | Core | Enumerates | Name | PC drive | iPhone Files | Reflash | Console | Notes |
|---|---|---|---|---|---|---|---|---|---|
| _pending_ | XIAO S3 | 3.3.x |  |  |  |  |  |  | first on-device run |
