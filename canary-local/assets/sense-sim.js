// canary-local/assets/sense-sim.js — DOM-free cores for the Sense Lab.
//
// This file is the Sense Lab's honesty layer: a faithful JavaScript port of
// the canary-sense firmware's radar pipeline, so the page stages the SAME
// bytes → parser → FSM → privacy-chokepoint behavior the device runs.
//
//   wire protocol   ports firmware/common/sensors/mmwave_mr60/mr60_uart.{h,cpp}
//   PresenceFSM     ports firmware/common/sensors/mmwave_mr60/mr60_presence.cpp
//   VitalsFSM       ports firmware/common/sensors/mmwave_mr60/mr60_vitals.cpp
//
// The ports are line-for-line: deadline-before-data-guard stall safety, the
// aggregate-snapshot decode, checksum resync, hostile-float clamping, the
// exactly-one-target vitals suppression. tests/sense.test.js re-pins the same
// behaviors the firmware's own host tests pin (firmware/tests_host/
// test_mr60_uart.cpp), so a drift in either port breaks CI.
//
// The physics layer (radarView / vitalsQuality) and the power model take
// their constants from devices/sense.json — generated and drift-gated by
// tools/gen_sense.py against docs/hardware/mr60bha2_radar_notes.md. Nothing
// numeric is invented here.
//
// No DOM, no fetch, no Date.now() — everything is (input) → output so Node
// tests can drive it.

// ============================================================================
// Wire protocol — MR60BHA2 UART frames (mr60_uart.h layout)
// ============================================================================

export const MR60 = {
  SOF: 0x01,
  HEADER_LEN: 8,
  MAX_PAYLOAD: 32,
  MAX_FRAME: 8 + 32 + 1,
  FRAME_QUEUE: 8,
  TYPE_PEOPLE_EXIST: 0x0f09,
  TYPE_TARGET_COUNT: 0x0a04,
  TYPE_DISTANCE: 0x0a16,
  TYPE_BREATH_RATE: 0x0a14,
  TYPE_HEART_RATE: 0x0a15,
};

export const TYPE_NAMES = {
  [MR60.TYPE_PEOPLE_EXIST]: "PEOPLE_EXIST",
  [MR60.TYPE_TARGET_COUNT]: "TARGET_COUNT",
  [MR60.TYPE_DISTANCE]: "DISTANCE",
  [MR60.TYPE_BREATH_RATE]: "BREATH_RATE",
  [MR60.TYPE_HEART_RATE]: "HEART_RATE",
};

// XOR-fold then bitwise-invert — the MR60 checksum for both header and data.
export function mr60Checksum(bytes) {
  let c = 0;
  for (const b of bytes) c ^= b;
  return ~c & 0xff;
}

// Build one full wire frame: SOF, id/len/type big-endian, header checksum
// over bytes [0..6], payload, data checksum over the payload.
export function buildFrame(typeId, payload, frameId = 0) {
  const n = payload.length;
  const f = new Uint8Array(MR60.HEADER_LEN + n + 1);
  f[0] = MR60.SOF;
  f[1] = (frameId >> 8) & 0xff;
  f[2] = frameId & 0xff;
  f[3] = (n >> 8) & 0xff;
  f[4] = n & 0xff;
  f[5] = (typeId >> 8) & 0xff;
  f[6] = typeId & 0xff;
  f[7] = mr60Checksum(f.subarray(0, 7));
  f.set(payload, MR60.HEADER_LEN);
  f[MR60.HEADER_LEN + n] = mr60Checksum(payload);
  return f;
}

function f32le(value) {
  const b = new Uint8Array(4);
  new DataView(b.buffer).setFloat32(0, value, true);
  return b;
}

// Per-type builders (payload layouts per mr60_uart.h).
export function framePeople(present) {
  return buildFrame(MR60.TYPE_PEOPLE_EXIST, Uint8Array.of(present ? 1 : 0, 0));
}
export function frameCount(count) {
  const p = new Uint8Array(4);
  new DataView(p.buffer).setUint32(0, count >>> 0, true);
  return buildFrame(MR60.TYPE_TARGET_COUNT, p);
}
export function frameDistance(meters, valid = true) {
  const p = new Uint8Array(8);
  p[0] = valid ? 1 : 0;
  p.set(f32le(meters), 4);
  return buildFrame(MR60.TYPE_DISTANCE, p);
}
export function frameBreath(bpm) {
  return buildFrame(MR60.TYPE_BREATH_RATE, f32le(bpm));
}
export function frameHeart(bpm) {
  return buildFrame(MR60.TYPE_HEART_RATE, f32le(bpm));
}

// ============================================================================
// FrameParser — JS port of securacv::mmwave::FrameParser (mr60_uart.cpp)
// ============================================================================

export const FrameKind = { None: 0, Presence: 1, Vitals: 2, Unknown: 3 };

function clampU16(v) {
  if (v < 0) return 0;
  if (v > 65535) return 65535;
  return Math.floor(v);
}

// round_bpm(): reject NaN/negatives, clamp, then round half-up — the same
// order as the firmware (clamp BEFORE the float→int cast).
function roundBpm(bpm) {
  if (!(bpm > 0)) return 0; // also rejects NaN
  if (bpm > 65535) return 65535;
  return clampU16(Math.floor(bpm + 0.5));
}

function emptyFrame() {
  return {
    kind: FrameKind.None,
    has_target: false,
    target_count: 0,
    distance_cm: 0,
    breath_rate: 0,
    heart_rate: 0,
  };
}

export class FrameParser {
  constructor() {
    this.crcErrors = 0;
    this.unknowns = 0;
    this.dropped = 0;
    this.reset();
  }

  reset() {
    this.buf = [];
    this.agg = { has_target: false, target_count: 0, distance_cm: 0, breath_rate: 0, heart_rate: 0 };
    this.queue = [];
    // health counters intentionally preserved (monotonic, like the firmware)
  }

  errorCount() { return this.crcErrors; }
  unknownCount() { return this.unknowns; }
  droppedCount() { return this.dropped; }

  push(bytes) {
    if (typeof bytes === "number") bytes = [bytes];
    for (const b of bytes) this._pushByte(b & 0xff);
  }

  poll() {
    if (this.queue.length === 0) return emptyFrame();
    return this.queue.shift();
  }

  _pushByte(b) {
    if (this.buf.length >= MR60.MAX_FRAME) {
      this.buf.shift();
    }
    this.buf.push(b);

    for (;;) {
      // Hunt: discard leading non-SOF bytes.
      if (this.buf[0] !== MR60.SOF) {
        let k = 0;
        while (k < this.buf.length && this.buf[k] !== MR60.SOF) k++;
        this.buf.splice(0, k);
        if (this.buf.length === 0) return;
      }

      const st = this._classify();
      if (st.status === "need-more") return;

      if (st.status === "complete") {
        this._decodeAndQueue(st.payloadLen);
        const total = MR60.HEADER_LEN + st.payloadLen + 1;
        this.buf.splice(0, total);
        if (this.buf.length === 0) return;
        continue; // trailing bytes may begin the next frame
      }

      // bad-checksum / oversized: count, drop the latched SOF, re-hunt.
      this.crcErrors++;
      this.buf.shift();
      if (this.buf.length === 0) return;
    }
  }

  _classify() {
    if (this.buf.length < MR60.HEADER_LEN) return { status: "need-more" };
    if (mr60Checksum(this.buf.slice(0, 7)) !== this.buf[7]) return { status: "bad-checksum" };
    const length = (this.buf[3] << 8) | this.buf[4];
    if (length > MR60.MAX_PAYLOAD) return { status: "oversized" };
    const total = MR60.HEADER_LEN + length + 1;
    if (this.buf.length < total) return { status: "need-more" };
    const payload = this.buf.slice(MR60.HEADER_LEN, MR60.HEADER_LEN + length);
    if (mr60Checksum(payload) !== this.buf[MR60.HEADER_LEN + length]) return { status: "bad-checksum" };
    return { status: "complete", payloadLen: length };
  }

  _leFloat(off) {
    const p = Uint8Array.from(this.buf.slice(off, off + 4));
    return new DataView(p.buffer).getFloat32(0, true);
  }

  _enqueue(kind) {
    if (this.queue.length >= MR60.FRAME_QUEUE) {
      this.queue.shift(); // burst overrun: drop the oldest
      this.dropped++;
    }
    this.queue.push({
      kind,
      has_target: this.agg.has_target,
      target_count: this.agg.target_count,
      distance_cm: this.agg.distance_cm,
      breath_rate: this.agg.breath_rate,
      heart_rate: this.agg.heart_rate,
    });
  }

  _decodeAndQueue(n) {
    const type = (this.buf[5] << 8) | this.buf[6];
    const P = MR60.HEADER_LEN;

    switch (type) {
      case MR60.TYPE_PEOPLE_EXIST: {
        if (n < 2) return false;
        const raw = this.buf[P] | (this.buf[P + 1] << 8);
        this.agg.has_target = raw !== 0;
        if (!this.agg.has_target) {
          // no target zeroes the other scalars — no stale ride-along
          this.agg.target_count = 0;
          this.agg.distance_cm = 0;
          this.agg.breath_rate = 0;
          this.agg.heart_rate = 0;
        }
        this._enqueue(FrameKind.Presence);
        return true;
      }
      case MR60.TYPE_TARGET_COUNT: {
        if (n < 4) return false;
        const count =
          (this.buf[P] | (this.buf[P + 1] << 8) | (this.buf[P + 2] << 16) | (this.buf[P + 3] << 24)) >>> 0;
        this.agg.target_count = count > 255 ? 255 : count;
        this._enqueue(FrameKind.Presence);
        return true;
      }
      case MR60.TYPE_DISTANCE: {
        if (n < 1) return false;
        const valid = this.buf[P] !== 0;
        if (valid) {
          if (n < 8) return false; // malformed: don't enqueue stale aggregate
          const cm = this._leFloat(P + 4) * 100.0; // [BENCH] metres → cm ×100
          if (!(cm > 0)) this.agg.distance_cm = 0; // also rejects NaN
          else if (cm > 65535) this.agg.distance_cm = 65535;
          else this.agg.distance_cm = clampU16(Math.floor(cm + 0.5));
        } else {
          this.agg.distance_cm = 0;
        }
        this._enqueue(FrameKind.Presence);
        return true;
      }
      case MR60.TYPE_BREATH_RATE: {
        if (n < 4) return false;
        this.agg.breath_rate = roundBpm(this._leFloat(P));
        this._enqueue(FrameKind.Vitals);
        return true;
      }
      case MR60.TYPE_HEART_RATE: {
        if (n < 4) return false;
        this.agg.heart_rate = roundBpm(this._leFloat(P));
        this._enqueue(FrameKind.Vitals);
        return true;
      }
      default:
        this.unknowns++;
        return false;
    }
  }
}

// ============================================================================
// PresenceFSM — JS port of securacv::mmwave::PresenceFSM (mr60_presence.cpp)
// ============================================================================

export const Presence = { Unknown: "unknown", Clear: "clear", Present: "present" };
export const CountBucket = { Zero: "0", One: "1", TwoPlus: "2+" };
export const RangeBand = { Unknown: "unknown", Near: "near", Mid: "mid", Far: "far" };

export class PresenceFSM {
  // cfg mirrors PresenceConfig: { present_debounce_ms, clear_timeout_ms,
  // stall_timeout_ms, near_cm, mid_cm } — defaults are the firmware CS_* values
  // handed in from devices/sense.json.
  constructor(cfg) {
    this.cfg = { ...cfg };
    this.reset(0);
  }

  reset(now) {
    this.state = Presence.Unknown;
    this.count = CountBucket.Zero;
    this.range = RangeBand.Unknown;
    this.lastFrameMs = now;
    this.targetSinceMs = now;
    this.targetGoneMs = now;
    this.rawTarget = false;
  }

  bucketOf(raw) {
    if (raw === 0) return CountBucket.Zero;
    if (raw === 1) return CountBucket.One;
    return CountBucket.TwoPlus;
  }

  bandOf(cm) {
    if (cm === 0) return RangeBand.Unknown;
    if (cm <= this.cfg.near_cm) return RangeBand.Near;
    if (cm <= this.cfg.mid_cm) return RangeBand.Mid;
    return RangeBand.Far;
  }

  tick(frame, now) {
    const ev = { state_changed: false, count_changed: false, stalled: false };

    // ---- DEADLINE FIRST (stall-safe): a dead UART can never freeze us ----
    if (this.state !== Presence.Unknown && now - this.lastFrameMs >= this.cfg.stall_timeout_ms) {
      ev.count_changed = this.count !== CountBucket.Zero;
      this.state = Presence.Unknown;
      this.count = CountBucket.Zero;
      this.range = RangeBand.Unknown;
      ev.state_changed = true;
      ev.stalled = true;
      ev.state = this.state; ev.count = this.count; ev.range = this.range;
      return ev;
    }

    // ---- DATA GUARD: only presence frames advance the machine ----
    if (frame.kind !== FrameKind.Presence) {
      ev.state = this.state; ev.count = this.count; ev.range = this.range;
      return ev;
    }

    this.lastFrameMs = now;

    if (frame.has_target && !this.rawTarget) this.targetSinceMs = now;
    else if (!frame.has_target && this.rawTarget) this.targetGoneMs = now;
    this.rawTarget = frame.has_target;

    const prevState = this.state;

    if (frame.has_target) {
      if (this.state !== Presence.Present && now - this.targetSinceMs >= this.cfg.present_debounce_ms) {
        this.state = Presence.Present;
      } else if (this.state === Presence.Unknown) {
        // first data after a stall: report something promptly, settle via debounce
        this.state = Presence.Clear;
      }
    } else {
      if (this.state !== Presence.Clear && now - this.targetGoneMs >= this.cfg.clear_timeout_ms) {
        this.state = Presence.Clear;
      } else if (this.state === Presence.Unknown) {
        this.state = Presence.Clear;
      }
    }

    const prevCount = this.count;
    this.count = frame.has_target ? this.bucketOf(frame.target_count) : CountBucket.Zero;
    this.range = frame.has_target ? this.bandOf(frame.distance_cm) : RangeBand.Unknown;

    ev.state_changed = this.state !== prevState;
    ev.count_changed = this.count !== prevCount;
    ev.state = this.state; ev.count = this.count; ev.range = this.range;
    return ev;
  }
}

// ============================================================================
// VitalsFSM — JS port of securacv::mmwave::VitalsFSM (mr60_vitals.cpp)
// ============================================================================

export const VitalsLock = { Unknown: "unknown", Lost: "lost", Locked: "locked" };

export class VitalsFSM {
  // cfg mirrors VitalsConfig: { lock_confirm_ms, lock_lost_ms, breath_min_bpm,
  // breath_max_bpm, heart_min_bpm, heart_max_bpm }.
  constructor(cfg) {
    this.cfg = { ...cfg };
    this.reset(0);
  }

  reset(now) {
    this.lock = VitalsLock.Unknown;
    this.lastValidMs = now;
    this.validSinceMs = now;
    this.wasValid = false;
    this.breathBpm = 0;
    this.heartBpm = 0;
  }

  plausible(f) {
    if (f.kind !== FrameKind.Vitals) return false;
    if (f.breath_rate < this.cfg.breath_min_bpm || f.breath_rate > this.cfg.breath_max_bpm) return false;
    if (f.heart_rate < this.cfg.heart_min_bpm || f.heart_rate > this.cfg.heart_max_bpm) return false;
    return true;
  }

  tick(frame, singleTarget, now) {
    const ev = { lock_changed: false, stalled: false };
    const prev = this.lock;

    // ---- DEADLINE FIRST (stall-safe) ----
    if (this.lock !== VitalsLock.Lost && this.lock !== VitalsLock.Unknown &&
        now - this.lastValidMs >= this.cfg.lock_lost_ms) {
      this.lock = VitalsLock.Lost;
      this.breathBpm = 0;
      this.heartBpm = 0;
      ev.lock_changed = true;
      ev.stalled = true;
      ev.lock = this.lock;
      ev.bpm_valid = false; ev.breath_bpm = 0; ev.heart_bpm = 0;
      return ev;
    }

    // ---- DATA GUARD: only vitals frames advance the lock machinery. ----
    // Presence/count/distance frames interleave constantly on the real wire
    // (and loop() ticks with an empty frame when nothing arrived); they must
    // not break a valid run or the confirm window could never elapse. Loss
    // of data is the deadline's job, never frame mix. This guard was added
    // to the firmware after this very bench surfaced the missing case.
    if (frame.kind !== FrameKind.Vitals) {
      ev.lock_changed = this.lock !== prev;
      ev.lock = this.lock;
      ev.bpm_valid = this.lock === VitalsLock.Locked && singleTarget;
      ev.breath_bpm = ev.bpm_valid ? this.breathBpm : 0;
      ev.heart_bpm = ev.bpm_valid ? this.heartBpm : 0;
      return ev;
    }

    // ---- SUPPRESSION: no vitals unless exactly one target ----
    const valid = singleTarget && this.plausible(frame);

    if (valid) {
      if (!this.wasValid) this.validSinceMs = now;
      this.wasValid = true;
      this.lastValidMs = now;
      this.breathBpm = frame.breath_rate;
      this.heartBpm = frame.heart_rate;

      if (this.lock !== VitalsLock.Locked && now - this.validSinceMs >= this.cfg.lock_confirm_ms) {
        this.lock = VitalsLock.Locked;
      } else if (this.lock === VitalsLock.Unknown) {
        this.lock = VitalsLock.Lost; // seen data, not yet confirmed
      }
    } else {
      this.wasValid = false;
      if (this.lock === VitalsLock.Unknown) this.lock = VitalsLock.Lost;
      // loss is deadline-driven, not single-bad-frame-driven (no lock flap)
    }

    ev.lock_changed = this.lock !== prev;
    ev.lock = this.lock;
    // bpm_valid additionally requires singleTarget RIGHT NOW
    ev.bpm_valid = this.lock === VitalsLock.Locked && singleTarget;
    ev.breath_bpm = ev.bpm_valid ? this.breathBpm : 0;
    ev.heart_bpm = ev.bpm_valid ? this.heartBpm : 0;
    return ev;
  }
}

// ============================================================================
// Placement physics — what the radar can see from where you put it
// ============================================================================
//
// scene: {
//   mount: "wall" | "ceiling" | "stand",   mountHeight (m), tiltDeg (down+),
//   person: { x, y (m, floor-plane offsets from the mount wall), posture:
//     "standing"|"sitting"|"lying", orientation: "facing"|"side"|"back",
//     moving: bool }, secondPerson: bool, fan: bool,
//   truth: { breathBpm, heartBpm },
// }
// hw: the devices/sense.json "hardware" block (fov, ranges, vitals factors).
//
// Chest height above floor by posture (m) — coarse anthropometry for the
// bench; the point is geometry honesty (angles/ranges), not biomechanics.
const CHEST_HEIGHT = { standing: 1.3, sitting: 0.9, lying: 0.55 };

export function radarView(scene, hw) {
  const chestH = CHEST_HEIGHT[scene.person.posture] ?? 1.2;

  // Radar position + boresight unit vector.
  let rx = 0, ry = 0, rz = scene.mountHeight;
  const tilt = (scene.tiltDeg * Math.PI) / 180;
  let bore;
  if (scene.mount === "ceiling") {
    // Ceiling: facing straight down, tilt leans the beam toward +x.
    bore = [Math.sin(tilt), 0, -Math.cos(tilt)];
  } else {
    // Wall / stand: facing +x into the room, tilt pitches the beam down.
    bore = [Math.cos(tilt), 0, -Math.sin(tilt)];
  }

  // Vector radar → chest.
  const dx = scene.person.x - rx, dy = scene.person.y - ry, dz = chestH - rz;
  const r = Math.sqrt(dx * dx + dy * dy + dz * dz);

  // Angle off boresight (single cone check against the sector spec).
  const dot = (dx * bore[0] + dy * bore[1] + dz * bore[2]) / (r || 1);
  const offBoresightDeg = (Math.acos(Math.max(-1, Math.min(1, dot))) * 180) / Math.PI;
  const halfFov = hw.fov_deg / 2;
  const inFov = offBoresightDeg <= halfFov;

  // Detectable at all? Static presence tops out at hw.presence_max_m.
  const inRange = r >= hw.presence_min_m && r <= hw.presence_max_m;
  const detected = inFov && inRange;

  // Line-of-sight vs chest normal: how much of the chest displacement the
  // radar sees radially. facing ≈ full, side ≈ small, back ≈ damped (the
  // chest wall still moves the back). Ceiling over a lying person is the
  // classic supine geometry: LoS is near the chest normal.
  let facingFactor;
  if (scene.mount === "ceiling" && scene.person.posture === "lying") {
    facingFactor = hw.vitals_orientation.facing;
  } else {
    facingFactor = hw.vitals_orientation[scene.person.orientation] ?? 0.5;
  }

  return { r, offBoresightDeg, inFov, inRange, detected, facingFactor, chestH };
}

// Vitals signal quality in [0,1] with the reasons split out, so the page can
// show WHY a placement fails (range falloff, off-angle, motion, multi-person).
export function vitalsQuality(scene, hw) {
  const view = radarView(scene, hw);
  const out = { quality: 0, view, reasons: [] };

  if (!view.detected) {
    out.reasons.push("outside the detection sector");
    return out;
  }
  if (scene.secondPerson) {
    // The module may still emit numbers; the firmware suppresses them.
    out.reasons.push("two targets — firmware hard-suppresses vitals (count ≠ 1)");
  }
  if (scene.person.moving) {
    out.reasons.push("body motion swamps the mm-scale chest phase signal");
    return out;
  }

  // Range: the literature's U-curve — flat around the hw.vitals_ref_m
  // optimum (~0.7 m), degrading toward the hw.vitals_max_m envelope, then
  // the hold-last-value grace zone, then nothing. Near-field (< presence
  // minimum) is penalized too (the < 0.6 m collapse in arXiv:2603.09791).
  let qRange;
  const graceEnd = hw.vitals_max_m * hw.vitals_grace;
  if (view.r < hw.presence_min_m) {
    qRange = 0.5;
    out.reasons.push("near-field — too close for clean phase extraction");
  } else if (view.r <= hw.vitals_ref_m) {
    qRange = 1;
  } else if (view.r <= hw.vitals_max_m) {
    qRange = 1 - 0.5 * ((view.r - hw.vitals_ref_m) / (hw.vitals_max_m - hw.vitals_ref_m));
  } else if (view.r <= graceEnd) {
    qRange = 0.5 * (1 - (view.r - hw.vitals_max_m) / (graceEnd - hw.vitals_max_m));
  } else {
    qRange = 0;
  }
  if (view.r > hw.vitals_max_m) out.reasons.push(`beyond the ${hw.vitals_max_m} m vitals envelope`);

  // Angle: antenna pattern rolloff toward the sector edge (gentle inside,
  // steep at the rim — the 80° figure is the envelope, not the −3 dB beam).
  const edge = view.offBoresightDeg / (hw.fov_deg / 2);
  const qAngle = Math.pow(Math.cos((edge * Math.PI) / 2 * 0.6), 1.5);
  if (edge > 0.75) out.reasons.push("near the edge of the antenna sector");

  // Orientation: radial projection of the chest displacement.
  const qFacing = view.facingFactor;
  if (qFacing < 0.5) out.reasons.push("chest displacement mostly tangential to the beam");

  // Interference: a fan's blades are a moving reflector inside the sector.
  const qFan = scene.fan ? hw.fan_penalty : 1;
  if (scene.fan) out.reasons.push("fan/moving-reflector interference in the sector");

  out.quality = Math.max(0, Math.min(1, qRange * qAngle * qFacing * qFan));
  return out;
}

// ============================================================================
// Radar frame stream — what the module would say, given the scene
// ============================================================================
//
// A deterministic pseudo-random stream (caller passes rand() in [0,1)) that
// exercises the REAL parser + FSMs: presence/count/distance frames each tick,
// vitals frames on their own cadence, BPM jitter and dropouts scaled by
// (1 - quality). Unplugging the radar = simply not calling this.

export function sceneFrames(scene, hw, tickMs, nowMs, rand) {
  const view = radarView(scene, hw);
  const vq = vitalsQuality(scene, hw);
  const bytes = [];
  const emitted = [];

  const present = view.detected;
  const count = present ? (scene.secondPerson ? 2 : 1) : 0;

  // Presence trio rides every tick (cadence assumption marked [BENCH] in
  // mr60_uart.h — the bench page states it out loud).
  const pushF = (name, u8) => { bytes.push(...u8); emitted.push(name); };
  const flicker = scene.fan && !present && rand() < hw.fan_false_presence;
  pushF("PEOPLE_EXIST", framePeople(present || flicker));
  if (present || flicker) {
    pushF("TARGET_COUNT", frameCount(flicker ? 1 : count));
    const jitterM = (rand() - 0.5) * 0.06;
    pushF("DISTANCE", frameDistance(Math.max(0.1, (flicker ? 1.8 : view.r) + jitterM)));
  }

  // Vitals cadence: every vitals_period_ms, when a person is in the sector.
  if (present && !scene.person.moving && nowMs % hw.vitals_period_ms < tickMs) {
    const q = vq.quality;
    if (rand() >= (1 - q) * hw.vitals_dropout) {
      // BPM noise grows as quality falls; at q≈1 the module tracks truth.
      const jb = (rand() - 0.5) * 2 * (1 - q) * hw.breath_jitter_bpm;
      const jh = (rand() - 0.5) * 2 * (1 - q) * hw.heart_jitter_bpm;
      pushF("BREATH_RATE", frameBreath(Math.max(0, scene.truth.breathBpm + jb)));
      pushF("HEART_RATE", frameHeart(Math.max(0, scene.truth.heartBpm + jh)));
    }
  }

  return { bytes: Uint8Array.from(bytes), emitted, view, vq };
}

// ============================================================================
// Privacy chokepoint — the full wire vocabulary (main.cpp presence_str /
// count_str / range_str + record_event_now)
// ============================================================================

export function chokepoint(fsmState) {
  // Everything that ever leaves the device about what the radar saw:
  return {
    presence: fsmState.presence, // "unknown" / "clear" / "present"
    occupants: fsmState.count,   // "0" / "1" / "2+"
    range: fsmState.range,       // "unknown" / "near" / "mid" / "far"
  };
}

// Event names the witness records (main.cpp drive_fsms).
export const EVENTS = ["presence_detected", "presence_cleared", "occupancy_changed"];

// The v1 `sense` signing canonical (common/identity/device_signature.h).
export function senseCanonical(deviceId, seq, event, presence, occupants, range, bucketUptimeS) {
  return `securacv-canary-sig|v1|sense|${deviceId}|${seq}|${event}|${presence}|${occupants}|${range}|${bucketUptimeS}`;
}

// 10-minute uptime bucket (main.cpp record_event_now).
export function bucketUptime(nowMs) {
  return Math.floor(nowMs / 1000 / 600) * 600;
}

// ============================================================================
// Power model — useful sensing per milliwatt
// ============================================================================
//
// rails: the devices/sense.json "power" block (per-component mW, parsed from
// docs/hardware/mr60bha2_radar_notes.md — datasheet/bench-sourced, cited there).
// knobs: { modemSleep, heartbeatS, ledOn, lux, txPerEventMs, eventsPerHour }.

export function powerModel(knobs, rails) {
  const parts = [];
  const add = (name, mw, note) => { parts.push({ name, mw, note }); };

  add("MR60BHA2 radar module", rails.radar_mw, "always sensing — the whole point of the witness");
  add("ESP32-C6 core (active)", rails.c6_active_mw, "parse + FSMs + witness chain");
  add(
    knobs.modemSleep ? "WiFi (modem sleep)" : "WiFi (listening)",
    knobs.modemSleep ? rails.wifi_modem_sleep_mw : rails.wifi_listen_mw,
    knobs.modemSleep ? "DTIM wake windows only" : "radio RX always on"
  );

  // Publish bursts: heartbeat + events, txPerEventMs of TX each.
  const txPerHour = (3600 / knobs.heartbeatS + knobs.eventsPerHour) * (knobs.txPerEventMs / 1000);
  const txDuty = txPerHour / 3600;
  add("WiFi TX bursts", rails.wifi_tx_mw * txDuty,
    `${(3600 / knobs.heartbeatS).toFixed(0)} heartbeats + ${knobs.eventsPerHour} events/h × ${knobs.txPerEventMs} ms`);

  if (knobs.ledOn) add("WS2812 status LED", rails.led_mw, "dim presence colours");
  if (knobs.lux) add("BH1750 lux", rails.bh1750_mw, "tamper corroboration");

  const totalMw = parts.reduce((s, p) => s + p.mw, 0);
  const perDayWh = (totalMw * 24) / 1000;
  const perYearKwh = (perDayWh * 365) / 1000;

  // Signed claims per joule: how much *witnessing* each unit of heat buys.
  const claimsPerHour = 3600 / knobs.heartbeatS + knobs.eventsPerHour;
  const joulesPerHour = totalMw * 3.6; // mW × 3600 s / 1000
  const claimsPerJoule = claimsPerHour / joulesPerHour;

  // Sensing share: radar + core vs radios + LED overhead.
  const usefulMw = rails.radar_mw + rails.c6_active_mw + (knobs.lux ? rails.bh1750_mw : 0);
  const sensingShare = usefulMw / totalMw;

  return { parts, totalMw, perDayWh, perYearKwh, claimsPerHour, claimsPerJoule, sensingShare };
}

// ============================================================================
// One-tick pipeline — wire the whole chain the way main.cpp's loop() does
// ============================================================================
//
// Owns parser + FSMs + the event record hook. drive() is loop(): drain every
// decoded frame through the FSMs; if none arrived, tick once with an empty
// frame so the stall deadlines still run.

export class SensePipeline {
  constructor(presenceCfg, vitalsCfg, vitalsEnabled) {
    this.parser = new FrameParser();
    this.presence = new PresenceFSM(presenceCfg);
    this.vitals = vitalsEnabled ? new VitalsFSM(vitalsCfg) : null;
    this.vitalsEnabled = vitalsEnabled;
    this.snapshot = {
      presence: Presence.Unknown, count: CountBucket.Zero, range: RangeBand.Unknown,
      radar_ok: false, frame_errors: 0,
      breathing_locked: false, bpm_valid: false, breath_bpm: 0, heart_bpm: 0,
    };
    this.events = []; // {name, now} appended on witness transitions
  }

  reset(now) {
    this.parser.reset();
    this.presence.reset(now);
    if (this.vitals) this.vitals.reset(now);
  }

  feed(bytes) { this.parser.push(bytes); }

  drive(now) {
    let any = false;
    const fired = [];
    for (let f = this.parser.poll(); f.kind !== FrameKind.None; f = this.parser.poll()) {
      if (this.onFrame) this.onFrame(f);
      fired.push(...this._fsms(f, now));
      any = true;
    }
    if (!any) fired.push(...this._fsms(emptyFrame(), now));

    const s = this.snapshot;
    s.presence = this.presence.state;
    s.count = this.presence.count;
    s.range = this.presence.range;
    s.radar_ok = this.presence.state !== Presence.Unknown;
    s.frame_errors = this.parser.errorCount();
    return fired;
  }

  _fsms(frame, now) {
    const fired = [];
    const pev = this.presence.tick(frame, now);
    if (pev.state_changed) {
      if (pev.state === Presence.Present) fired.push("presence_detected");
      else if (pev.state === Presence.Clear) fired.push("presence_cleared");
      // Unknown (stall) is radar-link health, not a witness event.
    }
    if (pev.count_changed && this.presence.state === Presence.Present) {
      fired.push("occupancy_changed");
    }

    if (this.vitals) {
      const single = pev.count === CountBucket.One;
      const vev = this.vitals.tick(frame, single, now);
      const s = this.snapshot;
      s.breathing_locked = vev.lock === VitalsLock.Locked;
      s.bpm_valid = !!vev.bpm_valid;
      s.breath_bpm = vev.breath_bpm || 0;
      s.heart_bpm = vev.heart_bpm || 0;
    }
    for (const name of fired) this.events.push({ name, now });
    return fired;
  }
}
