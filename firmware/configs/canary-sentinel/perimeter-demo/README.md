# Preset: perimeter-demo (HEAVY tier — the rigged demo)

The "prove-it" configuration. **Deltas on `door` that turn everything on.**

**Intent.** All six physical modality classes live at once: the Standard five
plus door-**contact**, enclosure **tamper**, and the optical **vision** vote
from the Heavy hub (`FEATURE_CONTACT/VISION/TAMPER = 1`, `FEATURE_MESH_NETWORK =
1` for the ESP-NOW head↔hub link). Sensitivities pushed: fast present-debounce
(0.7 s), lower present/confirm thresholds, a low anomaly threshold (40) so
tamper/blinding surfaces immediately, and a short 15 s loiter.

**Why it's near impossible to evade.** To cross this threshold unseen you must
simultaneously emit no heat, reflect no radar, not perturb the WiFi, carry no
powered radio, cast no optical change, and touch nothing — and any attempt to
*blind* a channel is itself an alarm.

**Constraints.** Heavy tier is **dual-board**: this preset builds the C6 sensor
head (`canary-sentinel-demo-head`); the vision vote arrives from a second XIAO
ESP32-S3 hub. See the project README's Heavy-tier section.
