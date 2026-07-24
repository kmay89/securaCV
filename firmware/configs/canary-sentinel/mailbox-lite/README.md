# Preset: mailbox-lite (LITE tier)

Mailbox / shed / porch guardian on the low-cost Lite board. **Deltas on `door`,
but it changes the TIER.**

**Intent.** Turns OFF the two body-present modalities — radar and CSI
(`FEATURE_MMWAVE_RADAR = 0`, `FEATURE_WIFI_CSI = 0`) — because the Lite board is
a single XIAO ESP32-C3 with no radar module. What remains is **PIR +
WiFi-RF + BLE + ambient light** = three independent modality classes (Thermal,
CarriedRadio, Optical). PIR/RF/BLE/light weights are raised to lean on the
channels that remain, and the confirm threshold is lowered (70 → 60) to what two
of the three remaining classes can actually reach.

**Honest limit (requirement R9).** With no radar and no CSI, a **slow,
device-free, still intruder can evade this tier**. That's fine for a mailbox, a
shed, a porch, an interior hallway — casual threats, not a determined adversary.
For a front door, use the Standard `door` preset.
