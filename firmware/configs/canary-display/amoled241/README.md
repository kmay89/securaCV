# amoled241 — the Canary Glance AMOLED (flagship glance glass)

The nightstand app (`CD_FLAVOR_NIGHTSTAND`: portrait face + witness column +
honest night + the full touch tap/long-press ladder) on the Waveshare
ESP32-S3-Touch-AMOLED-2.41 — an RM690B0 450×600 QSPI AMOLED in the vendor's
metal case. `CD_AMOLED_GLASS` is this config's selector: it swaps in
`hal/display_amoled241.cpp` (QSPI panel + FT6336 polling + the soft power
latch), picks the dash's across-the-room type ladder (the glass is ~310 ppi),
and scales `portrait_ui.cpp`'s row metrics up.

What makes it the flagship:

- **True black is free.** The Quiet Glass Character's `0x000000` ground and
  the night floor cost zero power on an emissive panel — dark-when-safe is
  genuinely dark, and the severity colors sing.
- **Brightness is a panel command** (`0x51`), not PWM — no backlight pin
  exists. The day/night intent API maps onto it in the HAL.
- **Every button and port carries its weight.** BOOT runs the standard
  tap/double/hold ladder; the PWR case button wakes on a tap and powers off
  honestly on a 3 s hold (latch drop + deep sleep, wake on the same button);
  the dedicated SDMMC microSD slot runs the time machine's deep archive
  (`FEATURE_SD_STORAGE 1` — the cleanest slot in the display line); the RTC
  keeps trusted time; UART/I2C side connectors stay free for the bench.

Board truth lives in
[`firmware/boards/waveshare-esp32s3-amoled241/`](../../../boards/waveshare-esp32s3-amoled241/README.md)
— including the four things that will bite you (the GPIO16 power latch, the
expander-routed touch INT, the 16-px panel window, the busless charger).
