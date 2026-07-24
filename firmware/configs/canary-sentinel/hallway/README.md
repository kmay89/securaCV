# Preset: hallway (Standard tier)

Interior corridor / room-occupancy sensing. **Deltas on `door`.**

**Intent.** A presence preset, not an alarm preset. vs `door`: radar range
widened (250/500 cm) to cover a corridor, gentler present-debounce (1.5 s), a
much longer loiter window (2 min — people dwell indoors legitimately), a higher
anomaly threshold (65), and the **silent-body rule turned off**: a still,
device-free body indoors (someone reading on a couch) is normal here, not an
anomaly.

**Constraints.** Standard tier. Use for occupancy/automation, not intrusion.
