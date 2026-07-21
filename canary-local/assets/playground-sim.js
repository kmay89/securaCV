// canary-local/assets/playground-sim.js — DOM-free core for the Playground.
//
// This file is the Playground's honesty layer: a faithful JavaScript port of
// the Waveshare 4.3B dev-playground firmware, so the page stages the SAME
// terminal-block behavior and the SAME `PG1` serial text the device prints.
//
//   station model + driver   ports firmware/projects/canary-display/.../playground.cpp
//   station cards/instructions ports firmware/projects/canary-display/.../playground_ui.cpp
//   PG1 comms + pin tracker  docs/hardware/dev_playground_43b.md §Comms / §Pin tracker
//
// The port is line-for-line where it matters: DI is active-LOW through the
// optocoupler (raw bit alongside the interpretation, exactly as the glass
// shows it), a DI0 press rings DO0 (the "ding" link), DO drives are bounded
// (1.5 s pulse / 30 s latch auto-off — outputs release themselves), the ToF
// trip counts a falling edge under the threshold, and the I2C census flags
// the CH422G/GT911 command addresses as reserved. tests/playground.test.js
// re-pins these against the firmware's own constants, so a drift in either
// side breaks CI.
//
// The board pins, terminal map, station instructions, bring-up code and the
// PG1 field grammar are carried in devices/playground.json — generated and
// drift-gated by tools/gen_playground.py against the firmware. Nothing here
// is invented: this file is *behavior*, that file is *facts*.
//
// No DOM, no fetch, no Date.now() — every method takes an explicit virtual
// clock `now` (ms) so Node tests (and the browser) drive it deterministically.

// ============================================================================
// Constants — mirror playground.cpp's compile-time values (VERIFY-tagged in
// the firmware; the tests assert these equal the .cpp).
// ============================================================================

export const PG = {
  DI_ACTIVE_LEVEL: 0, // optocoupler pulls the EXIO bit LOW when energized
  DI_POLL_MS: 20,
  DO_PULSE_MS: 1500, // bounded pulse
  DO_LATCH_MS: 30000, // latch safety auto-off
  LIGHT_POLL_MS: 500,
  TOF_POLL_MS: 100,
  PAD_POLL_MS: 50,
  SCAN_EVERY_MS: 3000,
  SNAP_EVERY_MS: 1000,
};

// VL53L0X trip thresholds cycled from the glass (action_tof_cycle_trip).
export const TOF_TRIPS = [50, 100, 200, 400];

// MPR121 sensitivity presets: {touch, release} deltas. Lower = more sensitive
// = triggers through thicker printed plastic (the shell-thickness coupon test).
export const PAD_PRESETS = [
  { name: "contact", touch: 12, release: 6 },
  { name: "2mm shell", touch: 8, release: 4 },
  { name: "4mm shell", touch: 4, release: 2 },
  { name: "max gain", touch: 2, release: 1 },
];

// Sensor I2C addresses (see the board README's reserved-address table).
export const SENSORS = {
  veml7700: 0x10,
  bh1750: 0x5c, // ADDR pin HIGH; 0x23 is the CH422G's — do not use it
  vl53l0x: 0x29,
  mpr121: 0x5a,
};

// CH422G command addresses + GT911 — burned on the shared 8/9 bus.
const RESERVED = new Set([0x23, 0x24, 0x26, 0x38, 0x5d, 0x14]);

// Known-address lookup — ports i2c_device_name() in playground.cpp.
const I2C_NAMES = new Map([
  [0x10, "VEML7700 light"],
  [0x5c, "BH1750 light"],
  [0x23, "CH422G (WR_OC)"],
  [0x24, "CH422G (WR_SET)"],
  [0x26, "CH422G (RD_IO)"],
  [0x38, "CH422G (WR_IO)"],
  [0x29, "VL53L0X ToF"],
  [0x39, "APDS-9960"],
  [0x44, "SHT4x temp/RH"],
  [0x45, "SHT4x temp/RH"],
  [0x5a, "MPR121 touch"],
  [0x5b, "MPR121 touch"],
  [0x5d, "GT911 panel touch"],
  [0x14, "GT911 panel touch"],
  [0x70, "TCA9548A mux"],
  [0x76, "BME280/BMP280"],
  [0x77, "BME280/BMP280"],
]);

export function i2cReserved(addr) {
  return RESERVED.has(addr);
}
export function i2cName(addr) {
  return I2C_NAMES.get(addr) || "";
}

// A 0x0AB-style hex, matching the firmware's %02X / %03X formatting.
export function hex(n, width) {
  return "0x" + Number(n).toString(16).toUpperCase().padStart(width, "0");
}

// The banner main.cpp prints on entry: PG1 HELLO board=<id> fw=<ver>.
export function helloLine(boardId, fw) {
  return `PG1 HELLO board=${boardId} fw=${fw}`;
}

// ============================================================================
// Live model — one field per PgState member in playground.h.
// ============================================================================

function freshState() {
  return {
    di0: { raw: false, active: false, events: 0, last_edge_ms: 0, active_ms: 0 },
    di1: { raw: false, active: false, events: 0, last_edge_ms: 0, active_ms: 0 },
    do0: { sinking: false, latched: false, auto_off_at_ms: 0, pulses: 0 },
    do1: { sinking: false, latched: false, auto_off_at_ms: 0, pulses: 0 },
    light: { found: false, addr: 0, lux: 0 },
    tof: { found: false, valid: false, mm: 0, trip_mm: 100, trips: 0, tripped: false },
    pad: { found: false, touched_bits: 0, preset: 0, events: 0 },
    bus: { count: 0, addrs: [] },
    display_ok: true,
  };
}

// ============================================================================
// PlaygroundSim — the driver, as (stimulus, now) → emitted PG1 lines.
//
// Every mutator returns the array of PG1 lines it emitted (usually 0–2) and
// also appends them to `this.log`, so a UI can either react to the return or
// tail the log. `snapshot(now)` is the 1 Hz SNAP; `tick(now)` runs the
// bounded-output safety auto-off. There is no wall clock — the caller owns it.
// ============================================================================

export class PlaygroundSim {
  constructor() {
    this.reset();
  }

  reset() {
    this.g = freshState();
    this.log = [];
  }

  _emit(now, station, body) {
    const line = `PG1 ${now} EVT ${station} ${body}`;
    this.log.push(line);
    return line;
  }

  // ── Isolated digital inputs (DI0 doorbell, DI1 intrusion) ──────────────
  // `energized` = the field side is driven (button pressed / contact closed /
  // beam broken). The optocoupler then pulls the EXIO bit LOW, so raw = 0 and
  // the interpretation active = (raw == DI_ACTIVE_LEVEL). Ports di_update().
  setDI(ch, energized, now) {
    const key = ch === 0 ? "di0" : "di1";
    const station = ch === 0 ? "doorbell" : "intrusion";
    const c = this.g[key];
    const rawBit = energized ? 0 : 1; // LOW when energized
    const active = (rawBit === 0) === (PG.DI_ACTIVE_LEVEL === 0);
    c.raw = rawBit !== 0;
    const out = [];
    if (active === c.active) return out;
    c.active = active;
    if (active) {
      c.events++;
      c.last_edge_ms = now;
      c.active_ms = 0;
      out.push(this._emit(now, station, `state=active count=${c.events}`));
      // Doorbell "ding" link: a DI0 press rings whatever DO0 drives.
      if (ch === 0) out.push(...this.doPulse(0, now));
    } else {
      c.active_ms = now - c.last_edge_ms;
      out.push(this._emit(now, station, `state=clear held_ms=${c.active_ms}`));
    }
    return out;
  }

  // ── Isolated open-drain outputs (DO0 chime, DO1 strobe) ────────────────
  _doDrive(ch, sink, now) {
    const c = ch === 0 ? this.g.do0 : this.g.do1;
    const station = ch === 0 ? "chime" : "strobe";
    c.sinking = sink;
    return [this._emit(now, station, `out=${sink ? "on" : "off"}`)];
  }

  doPulse(ch, now) {
    const c = ch === 0 ? this.g.do0 : this.g.do1;
    c.latched = false;
    c.pulses++;
    c.auto_off_at_ms = now + PG.DO_PULSE_MS;
    return this._doDrive(ch, true, now);
  }

  doLatchToggle(ch, now) {
    const c = ch === 0 ? this.g.do0 : this.g.do1;
    if (c.sinking) {
      c.latched = false;
      c.auto_off_at_ms = 0;
      return this._doDrive(ch, false, now);
    }
    c.latched = true;
    c.auto_off_at_ms = now + PG.DO_LATCH_MS; // safety: never latched forever
    return this._doDrive(ch, true, now);
  }

  // ── I2C hot-plug (census attach/detach) ────────────────────────────────
  // kind: 'veml' | 'bh' | 'tof' | 'pad'. Ports the census() hot-plug branch.
  attach(kind, now) {
    if (kind === "veml" || kind === "bh") {
      if (this.g.light.found) return [];
      this.g.light.found = true;
      this.g.light.addr = kind === "veml" ? SENSORS.veml7700 : SENSORS.bh1750;
      return [this._emit(now, "light", `attached=${hex(this.g.light.addr, 2)}`)];
    }
    if (kind === "tof") {
      if (this.g.tof.found) return [];
      this.g.tof.found = true;
      return [this._emit(now, "tof", `attached=${hex(SENSORS.vl53l0x, 2)}`)];
    }
    if (kind === "pad") {
      if (this.g.pad.found) return [];
      this.g.pad.found = true;
      return [this._emit(now, "captouch", `attached=${hex(SENSORS.mpr121, 2)}`)];
    }
    return [];
  }

  detach(kind, now) {
    if ((kind === "veml" || kind === "bh") && this.g.light.found) {
      this.g.light.found = false;
      this.g.light.addr = 0;
      return [this._emit(now, "light", "detached=1")];
    }
    if (kind === "tof" && this.g.tof.found) {
      this.g.tof.found = false;
      this.g.tof.valid = false;
      return [this._emit(now, "tof", "detached=1")];
    }
    if (kind === "pad" && this.g.pad.found) {
      this.g.pad.found = false;
      this.g.pad.touched_bits = 0;
      return [this._emit(now, "captouch", "detached=1")];
    }
    return [];
  }

  // ── Light: set the live lux (no EVT — it rides the SNAP) ────────────────
  lightSample(lux) {
    if (this.g.light.found) this.g.light.lux = lux;
    return [];
  }

  // ── ToF: a fresh range sample; a falling edge under trip counts one ─────
  tofSample(mm, now) {
    if (!this.g.tof.found) return [];
    const t = this.g.tof;
    t.valid = mm > 0 && mm < 2000;
    t.mm = mm;
    const tripped = t.valid && mm < t.trip_mm;
    const out = [];
    if (tripped && !t.tripped) {
      t.trips++;
      out.push(this._emit(now, "tof", `trip=1 mm=${mm} count=${t.trips}`));
    }
    t.tripped = tripped;
    return out;
  }

  cycleTofTrip(now) {
    const i = TOF_TRIPS.indexOf(this.g.tof.trip_mm);
    this.g.tof.trip_mm = TOF_TRIPS[(i + 1) % TOF_TRIPS.length];
    return [this._emit(now, "tof", `trip_mm=${this.g.tof.trip_mm}`)];
  }

  // ── Cap touch: preset cycle + electrode bitmap ──────────────────────────
  cyclePadPreset(now) {
    this.g.pad.preset = (this.g.pad.preset + 1) % PAD_PRESETS.length;
    return [this._emit(now, "captouch", `preset=${PAD_PRESETS[this.g.pad.preset].name}`)];
  }

  padTouch(bits, now) {
    if (!this.g.pad.found) return [];
    const p = this.g.pad;
    const masked = bits & 0x0fff;
    const out = [];
    if (masked & ~p.touched_bits) {
      // any newly-down electrode
      p.events++;
      out.push(this._emit(now, "captouch", `pads=${hex(masked, 3)} count=${p.events}`));
    }
    p.touched_bits = masked;
    return out;
  }

  // ── I2C census — report a bus scan (attaches known sensors) ─────────────
  census(addrs, now) {
    const list = [...addrs].filter((a) => a >= 0x08 && a <= 0x77);
    this.g.bus = { count: list.length, addrs: list };
    const out = [];
    const has = (a) => list.includes(a);
    if (has(SENSORS.veml7700)) out.push(...this.attach("veml", now));
    else if (has(SENSORS.bh1750)) out.push(...this.attach("bh", now));
    if (has(SENSORS.vl53l0x)) out.push(...this.attach("tof", now));
    if (has(SENSORS.mpr121)) out.push(...this.attach("pad", now));
    return out;
  }

  // ── Bounded-output safety: release any DO past its deadline ─────────────
  tick(now) {
    const out = [];
    for (const ch of [0, 1]) {
      const c = ch === 0 ? this.g.do0 : this.g.do1;
      if (c.sinking && c.auto_off_at_ms && now >= c.auto_off_at_ms) {
        c.auto_off_at_ms = 0;
        c.latched = false;
        out.push(...this._doDrive(ch, false, now));
      }
    }
    return out;
  }

  // ── 1 Hz full snapshot — byte-identical field order to snapshot() ───────
  snapshot(now) {
    const g = this.g;
    const line =
      `PG1 ${now} SNAP di0=${g.di0.active ? 1 : 0} di1=${g.di1.active ? 1 : 0} ` +
      `do0=${g.do0.sinking ? "on" : "off"} do1=${g.do1.sinking ? "on" : "off"} ` +
      `lux=${(g.light.found ? g.light.lux : -1.0).toFixed(1)} ` +
      `tof_mm=${g.tof.valid ? g.tof.mm : 0} tof_ok=${g.tof.found ? 1 : 0} ` +
      `pads=${hex(g.pad.touched_bits, 3)} preset=${PAD_PRESETS[g.pad.preset].name} ` +
      `i2c=${g.bus.count}`;
    this.log.push(line);
    return line;
  }
}
