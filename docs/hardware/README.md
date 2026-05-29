# Canary Hardware Documentation

Build plans and bills of materials for the physical SecuraCV Canary devices —
what to buy, how peripherals wire to the MCU, and which parts are required vs.
optional.

| Document | Description |
|----------|-------------|
| [`canary_peripheral_build_plan.md`](./canary_peripheral_build_plan.md) | Master build plan & BOM: audible chirp (buzzer), status LED, button/tamper/touch inputs, battery, and enclosure — for both Canary WAP and Canary Vision. |
| [`bom_canary_wap.csv`](./bom_canary_wap.csv) | Machine-readable BOM — Canary WAP (XIAO ESP32-S3 Sense). |
| [`bom_canary_vision.csv`](./bom_canary_vision.csv) | Machine-readable BOM — Canary Vision (ESP32-C3 + Grove Vision AI V2). |

The CSVs use a flat, RoHS-style schema:

```
Item,RefDes,Qty,Required,Category,Description,Manufacturer,MPN,Mouser,DigiKey,LCSC,UnitUSD,ExtUSD,Lifecycle,RoHS,Notes
```

> **Pin definitions are authoritative in firmware**, not here — see
> [`firmware/boards/`](../../firmware/boards/). This area documents the hardware
> *build*, and the pin map mirrors the firmware defaults so a board wired to
> spec needs no code changes.

For getting a finished device online, see
[`../getting_started_canary.md`](../getting_started_canary.md).
