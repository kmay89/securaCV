// canary-local/tests/senselab.test.js — the Sense Lab honesty gate.
//
// Two jobs, mirroring wap.test.js:
//   1. Re-derive every fact in devices/senselab.json from the firmware source
//      and cross-check — so a hand-edit that bypasses gen_senselab.py is caught
//      exactly like generator drift.
//   2. Exercise the DOM-free cores (assets/sense-sim.js, assets/canary-cards.js):
//      the JS parser/FSM ports are pinned to the SAME behaviors the firmware's
//      own host tests pin (firmware/tests_host/test_mr60_uart.cpp), so the
//      page's simulation cannot drift from the device's semantics.
//
// Run: node --test canary-local/tests/senselab.test.js

const { test } = require("node:test");
const assert = require("node:assert");
const { readFileSync } = require("node:fs");
const { join } = require("node:path");

const ROOT = join(__dirname, "..");
const REPO = join(ROOT, "..");
const read = (p) => readFileSync(p, "utf8");

const data = JSON.parse(read(join(ROOT, "devices/senselab.json")));
const FW = join(REPO, "firmware");
const mr60h = read(join(FW, "common/sensors/mmwave_mr60/mr60_uart.h"));
const cfgDefault = read(join(FW, "configs/canary-sense/default/config.h"));
const cfgWell = read(join(FW, "configs/canary-sense/wellbeing/config.h"));
const pinsH = read(join(FW, "boards/xiao-esp32c6-mr60/pins/pins.h"));
const mainCpp = read(join(FW, "projects/canary-sense/src/main.cpp"));
const haCpp = read(join(FW, "projects/canary-sense/src/ha/ha_discovery.cpp"));
const sigH = read(join(FW, "common/identity/device_signature.h"));
const notesMd = read(join(REPO, "docs/hardware/mr60bha2_radar_notes.md"));

const cs = (src, name) => {
  const m = src.match(new RegExp(name + "\\s+(\\d+)"));
  assert.ok(m, name + " missing");
  return parseInt(m[1], 10);
};

// ---------------------------------------------------------------------------
// 1. sense.json vs the source
// ---------------------------------------------------------------------------

test("firmware version + train ride the registry", () => {
  const versionH = read(join(FW, "projects/canary-sense/include/canary/version.h"));
  const v = versionH.match(/CANARY_FW_VERSION\s+"([^"]+)"/)[1];
  assert.strictEqual(data.device.fw_version, v);
  const registry = JSON.parse(read(join(ROOT, "devices/registry.json")));
  assert.strictEqual(data.device.fw_train, registry.fw_train);
  assert.ok(v.startsWith(registry.fw_train));
});

test("frame type ids match mr60_uart.h", () => {
  const want = {
    PEOPLE_EXIST: "0x0F09", TARGET_COUNT: "0x0A04", DISTANCE: "0x0A16",
    BREATH_RATE: "0x0A14", HEART_RATE: "0x0A15",
  };
  assert.strictEqual(data.protocol.frames.length, 5);
  for (const f of data.protocol.frames) {
    assert.strictEqual(f.type_hex.toLowerCase(), want[f.name].toLowerCase(), f.name);
    assert.ok(mr60h.includes(want[f.name]), f.name + " id in header");
  }
  assert.strictEqual(parseInt(data.protocol.sof, 16), 0x01);
  assert.strictEqual(data.protocol.header_len, 8);
});

test("[BENCH] notes are quoted from the header", () => {
  assert.ok(data.protocol.bench.length >= 3);
  for (const b of data.protocol.bench) {
    // first six words of each note must appear verbatim (modulo whitespace folding)
    const probe = b.split(" ").slice(0, 6).join(" ");
    assert.ok(mr60h.replace(/\s*\n \*\s*/g, " ").includes(probe), "bench note: " + probe);
  }
});

test("FSM defaults are the CS_* config values (both flavors agree)", () => {
  const p = data.fsm.presence;
  assert.strictEqual(p.present_debounce_ms, cs(cfgDefault, "CS_PRESENT_DEBOUNCE_MS"));
  assert.strictEqual(p.clear_timeout_ms, cs(cfgDefault, "CS_CLEAR_TIMEOUT_MS"));
  assert.strictEqual(p.stall_timeout_ms, cs(cfgDefault, "CS_RADAR_STALL_MS"));
  assert.strictEqual(p.near_cm, cs(cfgDefault, "CS_RANGE_NEAR_CM"));
  assert.strictEqual(p.mid_cm, cs(cfgDefault, "CS_RANGE_MID_CM"));
  assert.strictEqual(p.present_debounce_ms, cs(cfgWell, "CS_PRESENT_DEBOUNCE_MS"));
  const v = data.fsm.vitals;
  assert.strictEqual(v.lock_confirm_ms, cs(cfgWell, "CS_VITALS_LOCK_MS"));
  assert.strictEqual(v.lock_lost_ms, cs(cfgWell, "CS_VITALS_LOST_MS"));
  assert.strictEqual(v.breath_min_bpm, cs(cfgWell, "CS_BREATH_MIN_BPM"));
  assert.strictEqual(v.breath_max_bpm, cs(cfgWell, "CS_BREATH_MAX_BPM"));
  assert.strictEqual(v.heart_min_bpm, cs(cfgWell, "CS_HEART_MIN_BPM"));
  assert.strictEqual(v.heart_max_bpm, cs(cfgWell, "CS_HEART_MAX_BPM"));
});

test("pins match the board def", () => {
  assert.strictEqual(data.pins.radar_tx, cs(pinsH, "RADAR_UART_TX"));
  assert.strictEqual(data.pins.radar_rx, cs(pinsH, "RADAR_UART_RX"));
  assert.strictEqual(data.pins.baud, cs(pinsH, "RADAR_UART_BAUD"));
  assert.strictEqual(data.pins.sda, cs(pinsH, "I2C_PIN_SDA"));
  assert.strictEqual(data.pins.scl, cs(pinsH, "I2C_PIN_SCL"));
  assert.strictEqual(data.pins.led_pin, cs(pinsH, "LED_WS2812_PIN"));
});

test("privacy vocabulary + events + canonical are the firmware's", () => {
  for (const w of ["present", "clear", "unknown", "near", "mid", "far", "2+"])
    assert.ok(mainCpp.includes(`"${w}"`), w);
  for (const e of data.privacy.events) assert.ok(mainCpp.includes(`"${e}"`), e);
  assert.ok(mainCpp.includes("600UL) * 600UL"), "10-minute bucket");
  assert.strictEqual(data.privacy.bucket_s, 600);
  assert.ok(sigH.includes("|sense|"), "sense canonical domain");
  assert.ok(data.privacy.canonical.startsWith("securacv-canary-sig|v1|sense|"));
});

test("every entity id exists in ha_discovery.cpp; P1 gate is compile-time", () => {
  assert.ok(data.entities.length >= 14);
  for (const e of data.entities)
    assert.ok(haCpp.includes(`_${e.id}`), "entity " + e.id);
  assert.ok(haCpp.includes("CANARY_SENSE_VITALS"));
  assert.ok(haCpp.includes("FEATURE_VITALS_BPM_P1"));
  const bpm = data.entities.filter((e) => e.p1_only);
  assert.strictEqual(bpm.length, 2, "exactly two P1 entities (breath/heart)");
});

test("hardware + power tables come from the notes doc (drift both ways)", () => {
  assert.ok(notesMd.includes("SIM:hardware") && notesMd.includes("SIM:power"));
  assert.ok(notesMd.includes(String(data.hardware.fov_deg)), "fov in doc");
  assert.ok(notesMd.includes("0.8 W"), "kit power anchor");
  const rails = data.power.rails;
  const total = rails.radar_mw + rails.c6_active_mw + rails.wifi_listen_mw +
    rails.led_mw + rails.bh1750_mw;
  assert.ok(total >= 700 && total <= 950,
    "default rails must stay on the published 0.8 W kit anchor, got " + total);
});

// ---------------------------------------------------------------------------
// 2. the DOM-free cores — pinned to the firmware host-test behaviors
// ---------------------------------------------------------------------------

const simP = import("../assets/sense-sim.js");
const cardsP = import("../assets/canary-cards.js");

test("wire golden: each frame type decodes like the firmware golden test", async () => {
  const S = await simP;
  const parser = new S.FrameParser();
  parser.push(S.framePeople(true));
  parser.push(S.frameCount(1));
  parser.push(S.frameDistance(1.5)); // metres on the wire → 150 cm aggregate
  parser.push(S.frameBreath(15.5));  // rounds half-up → 16
  parser.push(S.frameHeart(72));
  let last = null, n = 0;
  for (let f = parser.poll(); f.kind !== S.FrameKind.None; f = parser.poll()) { last = f; n++; }
  assert.strictEqual(n, 5);
  assert.strictEqual(last.has_target, true);
  assert.strictEqual(last.target_count, 1);
  assert.strictEqual(last.distance_cm, 150);
  assert.strictEqual(last.breath_rate, 16);
  assert.strictEqual(last.heart_rate, 72);
  assert.strictEqual(parser.errorCount(), 0);
  assert.strictEqual(parser.unknownCount(), 0);
});

test("absence zeroes the aggregate (no stale ride-along)", async () => {
  const S = await simP;
  const p = new S.FrameParser();
  p.push(S.framePeople(true)); p.push(S.frameCount(2)); p.push(S.frameDistance(2.0));
  p.push(S.framePeople(false));
  let last = null;
  for (let f = p.poll(); f.kind !== S.FrameKind.None; f = p.poll()) last = f;
  assert.strictEqual(last.has_target, false);
  assert.strictEqual(last.target_count, 0);
  assert.strictEqual(last.distance_cm, 0);
});

test("corruption raises error_count then resyncs on the next valid frame", async () => {
  const S = await simP;
  const p = new S.FrameParser();
  const bad = S.framePeople(true);
  bad[7] ^= 0xff; // wreck the header checksum
  p.push(bad);
  p.push(S.framePeople(true));
  let got = 0;
  for (let f = p.poll(); f.kind !== S.FrameKind.None; f = p.poll()) got++;
  assert.ok(p.errorCount() >= 1, "corruption counted");
  assert.ok(got >= 1, "recovered a valid frame after corruption");
});

test("well-framed unknown types are counted, not errors (phase waveform 0x0A13)", async () => {
  const S = await simP;
  const p = new S.FrameParser();
  p.push(S.buildFrame(0x0a13, new Uint8Array(12)));
  p.push(S.framePeople(true));
  let got = 0;
  for (let f = p.poll(); f.kind !== S.FrameKind.None; f = p.poll()) got++;
  assert.strictEqual(p.unknownCount(), 1);
  assert.strictEqual(p.errorCount(), 0);
  assert.strictEqual(got, 1);
});

test("hostile floats are clamped, never UB-shaped", async () => {
  const S = await simP;
  const p = new S.FrameParser();
  p.push(S.frameBreath(Infinity));
  p.push(S.frameHeart(NaN));
  p.push(S.frameDistance(Infinity));
  p.push(S.frameDistance(-3));
  let last = null;
  for (let f = p.poll(); f.kind !== S.FrameKind.None; f = p.poll()) last = f;
  assert.strictEqual(last.breath_rate, 65535);
  assert.strictEqual(last.heart_rate, 0);
  assert.strictEqual(last.distance_cm, 0); // the -3 came last → rejected to 0
});

test("PresenceFSM: debounce, clear timeout, stall — the firmware integration walk", async () => {
  const S = await simP;
  const cfg = data.fsm.presence;
  const fsm = new S.PresenceFSM(cfg);
  fsm.reset(0);
  const pf = (t) => ({ kind: S.FrameKind.Presence, has_target: t, target_count: t ? 1 : 0, distance_cm: t ? 100 : 0, breath_rate: 0, heart_rate: 0 });

  // first data after boot lifts Unknown→Clear promptly
  let ev = fsm.tick(pf(true), 10);
  assert.strictEqual(fsm.state, S.Presence.Clear);
  // sustained target settles to Present via the debounce
  ev = fsm.tick(pf(true), 10 + cfg.present_debounce_ms);
  assert.strictEqual(fsm.state, S.Presence.Present);
  assert.ok(ev.state_changed);
  assert.strictEqual(fsm.count, S.CountBucket.One);
  assert.strictEqual(fsm.range, S.RangeBand.Near); // 100 cm <= near_cm

  // absence: holds through the clear window, then Clear
  const t0 = 10 + cfg.present_debounce_ms;
  fsm.tick(pf(false), t0 + 10);
  assert.strictEqual(fsm.state, S.Presence.Present, "clear timeout not yet elapsed");
  ev = fsm.tick(pf(false), t0 + 10 + cfg.clear_timeout_ms);
  assert.strictEqual(fsm.state, S.Presence.Clear);

  // silence: the stall deadline drives Unknown even with no frames at all
  const t1 = t0 + 10 + cfg.clear_timeout_ms;
  ev = fsm.tick({ kind: S.FrameKind.None }, t1 + cfg.stall_timeout_ms + 1);
  assert.strictEqual(fsm.state, S.Presence.Unknown);
  assert.ok(ev.stalled);
});

test("VitalsFSM: lock confirm, instant multi-target suppression, deadline loss", async () => {
  const S = await simP;
  const cfg = data.fsm.vitals;
  const fsm = new S.VitalsFSM(cfg);
  fsm.reset(0);
  const vf = { kind: S.FrameKind.Vitals, has_target: true, target_count: 1, distance_cm: 90, breath_rate: 16, heart_rate: 70 };

  fsm.tick(vf, true, 10);
  assert.strictEqual(fsm.lock, S.VitalsLock.Lost, "seen data, not yet confirmed");
  let ev = fsm.tick(vf, true, 10 + cfg.lock_confirm_ms);
  assert.strictEqual(fsm.lock, S.VitalsLock.Locked);
  assert.ok(ev.bpm_valid);
  assert.strictEqual(ev.heart_bpm, 70);

  // a second person appears: BPM reporting stops IMMEDIATELY, lock rides its window
  ev = fsm.tick(vf, false, 30 + cfg.lock_confirm_ms);
  assert.strictEqual(ev.bpm_valid, false);
  assert.strictEqual(ev.breath_bpm, 0);

  // silence past the lost deadline → Lost via the deadline-first branch
  ev = fsm.tick({ kind: S.FrameKind.None }, true, 30 + cfg.lock_confirm_ms + cfg.lock_lost_ms + 1);
  assert.strictEqual(fsm.lock, S.VitalsLock.Lost);
  assert.ok(ev.stalled);
});

test("vitals lock survives real interleaved traffic (the bench-found firmware bug)", async () => {
  // Regression twin of firmware/tests_host test_vitals_lock_survives_
  // interleaved_presence: presence frames at 10 Hz + empty loop() ticks
  // between 1 Hz vitals reports must not reset the confirm window.
  const S = await simP;
  const cfg = data.fsm.vitals;
  const fsm = new S.VitalsFSM(cfg);
  fsm.reset(0);
  const vf = { kind: S.FrameKind.Vitals, breath_rate: 16, heart_rate: 70 };
  const pf = { kind: S.FrameKind.Presence, has_target: true, target_count: 1, distance_cm: 100, breath_rate: 16, heart_rate: 70 };
  fsm.tick(vf, true, 0);
  let ev;
  for (let t = 100; t <= cfg.lock_confirm_ms + 1000; t += 100) {
    if (t % 1000 === 0) ev = fsm.tick(vf, true, t);
    else { fsm.tick(pf, true, t); ev = fsm.tick({ kind: S.FrameKind.None }, true, t); }
  }
  assert.strictEqual(fsm.lock, S.VitalsLock.Locked, "interleaved traffic must not prevent the lock");
  assert.ok(ev.bpm_valid && ev.heart_bpm === 70);
  // non-vitals ticks still suppress BPM the instant a second person appears
  ev = fsm.tick({ kind: S.FrameKind.None }, false, cfg.lock_confirm_ms + 1100);
  assert.strictEqual(ev.bpm_valid, false);
});

test("an ambiguous interval resets the acquiring run (Codex review regression)", async () => {
  // Twin of firmware test_vitals_ambiguity_resets_acquiring_run: a second
  // person visible only on non-vitals ticks must restart the confirm window
  // — a lock may never be acquired on credit that straddles count != 1.
  const S = await simP;
  const cfg = data.fsm.vitals;
  const fsm = new S.VitalsFSM(cfg);
  fsm.reset(0);
  const vf = { kind: S.FrameKind.Vitals, breath_rate: 16, heart_rate: 70 };
  const pf = { kind: S.FrameKind.Presence, has_target: true, target_count: 2, distance_cm: 100, breath_rate: 16, heart_rate: 70 };
  fsm.tick(vf, true, 0);
  for (let t = 1000; t <= 3000; t += 1000) fsm.tick(vf, true, t);
  assert.strictEqual(fsm.lock, S.VitalsLock.Lost);

  fsm.tick(pf, false, 3300); // second person, non-vitals tick only

  let ev = fsm.tick(vf, true, 4500); // old run would have confirmed by now
  assert.strictEqual(fsm.lock, S.VitalsLock.Lost, "must not lock across ambiguity");
  assert.strictEqual(ev.bpm_valid, false);

  for (let t = 5500; t <= 4500 + cfg.lock_confirm_ms; t += 1000) ev = fsm.tick(vf, true, t);
  assert.strictEqual(fsm.lock, S.VitalsLock.Locked, "clean restarted run locks");
});

test("implausible vitals are rejected by the config bands", async () => {
  const S = await simP;
  const cfg = data.fsm.vitals;
  const fsm = new S.VitalsFSM(cfg);
  fsm.reset(0);
  // the empty-room phantom: heart nonzero, breath zero (Seeed-documented fan artifact)
  const phantom = { kind: S.FrameKind.Vitals, breath_rate: 0, heart_rate: 80 };
  fsm.tick(phantom, true, 10);
  fsm.tick(phantom, true, 10 + cfg.lock_confirm_ms * 2);
  assert.notStrictEqual(fsm.lock, S.VitalsLock.Locked, "phantom must never lock");
});

test("placement physics: the Seeed bedside geometry is the strong placement", async () => {
  const S = await simP;
  const hw = data.hardware;
  const bedside = {
    mount: "stand", mountHeight: 1.0, tiltDeg: 45,
    person: { x: 0.9, y: 0, posture: "lying", orientation: "facing", moving: false },
    secondPerson: false, fan: false, truth: { breathBpm: 14, heartBpm: 68 },
  };
  const q1 = S.vitalsQuality(bedside, hw);
  assert.ok(q1.quality > 0.6, "bedside quality " + q1.quality);

  const acrossRoom = { ...bedside, tiltDeg: 5, person: { ...bedside.person, x: 3.2, posture: "standing" } };
  const q2 = S.vitalsQuality(acrossRoom, hw);
  assert.ok(q2.quality < q1.quality, "3 m must be worse than bedside");
  assert.ok(q2.reasons.some((r) => r.includes("vitals envelope")), "names the envelope");

  const moving = { ...bedside, person: { ...bedside.person, moving: true } };
  assert.strictEqual(S.vitalsQuality(moving, hw).quality, 0, "motion kills vitals");

  const twoPeople = { ...bedside, secondPerson: true };
  assert.ok(S.vitalsQuality(twoPeople, hw).reasons.some((r) => r.includes("suppress")),
    "multi-person names the firmware suppression");
});

test("power model: calibrated to the kit anchor; modem sleep is the lever", async () => {
  const S = await simP;
  const rails = data.power.rails;
  const base = { modemSleep: false, heartbeatS: 5, eventsPerHour: 12, txPerEventMs: 40, ledOn: true, lux: true };
  const m1 = S.powerModel(base, rails);
  assert.ok(m1.totalMw > 700 && m1.totalMw < 950, "default total on the 0.8 W anchor: " + m1.totalMw);
  const m2 = S.powerModel({ ...base, modemSleep: true }, rails);
  assert.ok(m2.totalMw < m1.totalMw - 150, "modem sleep saves > 150 mW");
  assert.ok(m1.sensingShare > 0.5, "sensing must dominate the heat budget");
  assert.ok(m1.claimsPerJoule > 0);
});

test("the sense canonical matches the locked v1 format", async () => {
  const S = await simP;
  const c = S.senseCanonical("canary_sense_lab", 7, "presence_detected", "present", "1", "near", 600);
  assert.strictEqual(c,
    "securacv-canary-sig|v1|sense|canary_sense_lab|7|presence_detected|present|1|near|600");
  assert.strictEqual(S.bucketUptime(11 * 60 * 1000), 600);
});

// ---------------------------------------------------------------------------
// 3. Canary Cards — schema contract
// ---------------------------------------------------------------------------

test("every sense card validates against the schema; entity join keys hold", async () => {
  const C = await cardsP;
  const snap = {
    presence: "present", count: "1", range: "near", radar_ok: true, frame_errors: 0,
    lux: 140, last_event: "presence_detected",
    breathing_locked: true, bpm_valid: true, breath_bpm: 14, heart_bpm: 68,
  };
  const meta = { vitalsBuild: true, p1OptIn: true, chain: { length: 4, badge: "verified", signed: true }, trend: { breath: [14, 15], heart: [68, 69] } };
  const cards = C.senseCards(snap, meta);
  assert.ok(cards.length >= 10);
  const entityIds = new Set(data.entities.map((e) => e.id));
  for (const card of cards) {
    const errs = C.validateCard(card);
    assert.deepStrictEqual(errs, [], card.id + ": " + errs.join("; "));
    if (card.id !== "chain") assert.ok(entityIds.has(card.id), card.id + " is a real HA entity");
  }
});

test("presence-only build renders BPM cards as provably absent — never silently missing", async () => {
  const C = await cardsP;
  const snap = { presence: "clear", count: "0", range: "unknown", radar_ok: true, frame_errors: 0, lux: 10, last_event: "boot" };
  const cards = C.senseCards(snap, { vitalsBuild: false, p1OptIn: false, chain: { length: 0, badge: "unknown" }, trend: { breath: [], heart: [] } });
  const byId = Object.fromEntries(cards.map((c) => [c.id, c]));
  assert.strictEqual(byId.breathing.absent, true);
  assert.strictEqual(byId.breath_rate.absent, true);
  assert.strictEqual(byId.heart_rate.absent, true);
  assert.strictEqual(byId.presence.absent, undefined, "presence card present in every build");
  for (const c of cards) assert.deepStrictEqual(C.validateCard(c), []);
});

test("the cards doc pins the invariant the code implements", () => {
  const doc = read(join(REPO, "docs/standard/CANARY_CARDS.md"));
  assert.ok(doc.includes("one entity to one card"));
  assert.ok(doc.includes("CARD_SCHEMA_V") || doc.includes("schema v1"));
});
