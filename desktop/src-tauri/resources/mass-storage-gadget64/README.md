# mass-storage-gadget64 — populated by release CI

This directory ships inside released builds as the payload for the bundled
`rpiboot` sidecar: Raspberry Pi's signed **mass-storage gadget**, vendored from
the same pinned `raspberrypi/usbboot` checkout that builds the sidecar (see
`.github/workflows/desktop-flasher-release.yml`). When a Pi is connected in USB
device-boot mode, `rpiboot -d <this dir>` serves it this gadget and the Pi then
presents its SD card and NVMe to this computer as ordinary USB disks — the
card-reader-less flash path (design doc §7 step 6).

Only this README is committed. A dev build without the real payload fails the
"use the Pi itself" flow with a clear message and everything else still works.
