# WiFi (CSI) Sensing — Setup, Environments & Verification

How Canary's WiFi sensing actually works, what it needs to work well,
and how to verify it in *your* rooms. Written alongside the 2026-07
sensing overhaul (frame-supply fix + feature-math rewrite); if your
firmware predates it, update first — earlier builds could sit on
"Sensing…" forever in most homes.

**Companion:** the sensing dashboard (`/` on the device) · RF presence
(BLE) runs alongside and is fused automatically.

---

## 1. The one thing to understand: sensing needs frames

CSI sensing works by watching how WiFi radio waves *change* after
bouncing around your room. The device cannot generate its own
observations — it measures received frames. **No received frames, no
sensing.** Everything else follows from where those frames come from:

| Setup | Frame source | Supply | What you get |
|---|---|---|---|
| Canary joined to home WiFi | Router beacons (~10/s) + traffic | Good | Motion + breathing, one room |
| Two+ Canaries in range | Each broadcasts a 10 Hz sensing probe | Good, deterministic | Motion + breathing + multi-link corroboration — each device lights up the others |
| Canary in AP-only mode, alone | Almost nothing | **Starved** | No sensing — the dashboard says so |

The dashboard footer shows the live supply:
`motion 12 · breathing 3 · signal 11/s · probing`.
**8+/s is healthy.** 2–7/s works but reacts slower. Below 2/s the orb
parks on "Sensing…" with *"no WiFi signal to sense with — join your
home WiFi or add a second Canary"* — the device refuses to claim a
room is "empty" when it has no data (that would be a confident lie).

Two Canaries genuinely work better together: each one's probe gives
the other a steady 10 frames/s to measure with, independent of your
router's mood, and the multi-link fusion module cross-checks their
claims.

## 2. What the scores mean (post-overhaul)

- **Motion (0..100+)** — how much the radio environment is *changing*.
  Empty room ≈ 0–3 in any environment (the math normalizes out receiver
  gain, the chip's per-frame phase noise, and your room's static
  multipath fingerprint — the three things that used to drown the
  signal). Walking typically 30–90. Waving an arm near the device:
  10–30.
- **Breathing (0..100)** — strength of a periodic 6–27 BPM oscillation
  in the radio envelope, measured over the **last ~64 seconds**. It
  needs a *quiet* minute to lock: a still person in range, no walking.
  It reports 0 for the first ~24 s after sensing starts by design —
  breathing physically cannot be measured faster.
- The presence states build on these: Empty → Subtle → Quiet
  (breathing) → Active → Together.

## 3. Placement & environment optimization

- **Coverage is between the device and the frame source.** A Canary
  sensing off router beacons watches the space *between itself and the
  router* most sensitively. Put that path through the room you care
  about. With two Canaries, the probe path between them is the
  sensitive zone — place them across the room, 3–8 m apart, roughly
  chest height.
- **2.4 GHz only.** CSI comes from the 2.4 GHz radio. The Canary must
  join the 2.4 GHz side of your network.
- **Avoid**: mounting inside metal enclosures, directly on large metal
  surfaces, or right next to the router (< 1 m — the direct path
  swamps the reflections that carry the motion signal).
- **Fans, HVAC, pets**: the shimmer filter rejects RSSI flutter without
  phase rotation, and Pet Mode (Settings) delays breathing claims. If a
  ceiling fan reads as motion, run Calibrate (below) with the fan ON —
  the thresholds learn your ambient.
- **Busy networks help.** Streaming video through the router adds
  frames. A silent IoT VLAN gives the ~10 Hz beacon minimum — that's
  fine post-overhaul, just slightly slower to confirm.

## 4. Calibrate for your room (2 minutes)

Dashboard → Settings → **Calibrate**. Leave the room (or stand still
far from the device) for the ~10 s sampling window. The device
measures its ambient noise floor and proposes thresholds a margin
above it. Accept to persist.

- Re-run after moving the device, the router, or large furniture.
- **Re-run after this firmware update** — the feature scales changed
  (an empty room now genuinely reads ≈0, so thresholds calibrated
  against the old noisy scores are far too high).
- Presets: *Sensitive / Balanced / Quiet* plus a sensitivity slider,
  in Settings, if you'd rather tune by feel.

## 5. Verify it works (5 minutes)

1. **Supply**: footer shows `signal ≥ 8/s`. If not → §1 table.
2. **Motion**: walk across the room. The orb should go Subtle/Active
   within ~2–4 s and the footer's motion number should jump well above
   its still-room value.
3. **Empty**: leave the room for 2 minutes. Motion should sit at 0–3
   and the state should settle to Empty.
4. **Breathing** (optional): sit still 3–5 m from the device for a
   full minute. Breathing score should climb and the state reach
   Quiet; with "Detailed metrics" enabled you'll see a BPM estimate.
5. **Serial one-liner** (installers): the boot log prints
   `[CSI] OK: N frames received, M windows emitted in 3s` — a healthy
   radio shows N ≥ 15 on home WiFi.

## 6. Honest limitations

- Through-wall sensitivity varies enormously with construction —
  verify, don't assume.
- Breathing detection is best-effort at ≤ ~5 m line-of-sight in a
  quiet room; it is a wellbeing signal, not a medical device.
- A solo Canary in AP-only mode (no home WiFi) cannot sense — that's
  physics, not a bug. Add a second Canary or join the home network.
- Neighbors' WiFi does not feed sensing (frames from other channels
  are filtered), but a very congested channel can slow the mesh probe;
  the channel-hop coordinator moves the fleet automatically.
