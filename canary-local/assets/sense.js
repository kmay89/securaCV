// canary-local/assets/sense.js — The Sense Lab page driver.
//
// Renders sense.html entirely from devices/sense.json — the generated,
// drift-gated data (tools/gen_sense.py parses the canary-sense firmware +
// docs/hardware/mr60bha2_radar_notes.md). The page hardcodes no pin, no
// frame id, no FSM default, no power rail: a firmware change re-lines every
// surface without anyone editing this file, and CI fails if sense.json goes
// stale against the source.
//
// The bench loop is the firmware's own loop() shape: scene → radar frames
// (real wire bytes) → the ported FrameParser → the ported FSMs → the privacy
// chokepoint → Ed25519-signed witness events → Canary Cards → (optionally)
// the real display firmware in wasm.
//
// Failure posture (same as the WAP/Hub pages): if sense.json can't load,
// degrade to a pointer at the design doc — never a blank screen.

import {
  SensePipeline, sceneFrames, senseCanonical, bucketUptime,
  buildFrame, Presence,
} from "./sense-sim.js";
import {
  buildProtocol, buildStage, buildConsole, buildKnobs, buildPowerLab,
  buildGlassBridge, BenchSigner,
} from "./sense-ui.js";
import { senseCards, renderCardGrid } from "./canary-cards.js";
import { DeviceScene, BUILDERS } from "./scene3d.js";
import { upgradeRealShape } from "./real-shapes.js";

const $ = (sel, root = document) => root.querySelector(sel);
const el = (tag, cls, text) => {
  const n = document.createElement(tag);
  if (cls) n.className = cls;
  if (text != null) n.textContent = text;
  return n;
};
const GH = "https://github.com/kmay89/securaCV/blob/main/";

// Deterministic PRNG — the bench replays the same run for the same seed.
function mulberry32(seed) {
  let a = seed >>> 0;
  return () => {
    a |= 0; a = (a + 0x6d2b79f5) | 0;
    let t = Math.imul(a ^ (a >>> 15), 1 | a);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

async function main() {
  const mount = $("#sense");
  let data;
  try {
    const res = await fetch("devices/sense.json");
    if (!res.ok) throw new Error("HTTP " + res.status);
    data = await res.json();
    for (const k of ["device", "pins", "protocol", "fsm", "privacy", "topics",
                     "entities", "hardware", "power", "placement", "display"])
      if (!data[k]) throw new Error("missing section: " + k);
  } catch (e) {
    mount.append(el("p", "muted",
      "The Sense Lab data failed to load (" + e.message + "). Everything it stages is " +
      "also written out in docs/canary_sense_mr60bha2_design.md and " +
      "docs/hardware/mr60bha2_radar_notes.md."));
    const a = el("a", null, "Open the design doc →");
    a.href = GH + "docs/canary_sense_mr60bha2_design.md";
    mount.append(a);
    return;
  }

  // ── bench state ──
  const TICK = 100; // ms of simulated time per animation step
  const rand = mulberry32(0x5e42);
  const state = {
    flavor: "canary-sense-wellbeing",
    p1OptIn: true,
    presenceCfg: { ...data.fsm.presence },
    vitalsCfg: { ...data.fsm.vitals },
    radarPlugged: true,
    lights: true,
    deviceId: "canary_sense_lab",
    seq: 0,
    chainLen: 0,
    simMs: 0,
    trend: { breath: [], heart: [] },
  };
  const scene = {
    mount: "stand",
    mountHeight: data.placement.mounts[1].height_m,
    tiltDeg: data.placement.mounts[1].tilt_deg,
    person: { ...data.placement.mounts[1].person },
    secondPerson: false,
    fan: false,
    truth: { breathBpm: 14, heartBpm: 68 },
    cfg: state.presenceCfg, // range-band arcs on the stage track the live knobs
  };

  let pipeline = makePipeline();
  const signer = new BenchSigner();
  signer.init();

  function vitalsBuild() { return state.flavor === "canary-sense-wellbeing"; }
  function makePipeline() {
    const p = new SensePipeline(state.presenceCfg, state.vitalsCfg, vitalsBuild());
    p.reset(state.simMs);
    return p;
  }

  // ── sections ──
  renderVersionStrip(data);
  const secDeep = section("deep", "the hardware, all the way down",
    "One chip does the witnessing",
    "The MR60BHA2's ADT6101P runs the whole vital-sign pipeline — range FFT, phase " +
    "extraction, breath/heart separation — on its own Cortex-M3, behind its own antenna. " +
    "The raw radar IQ physically never reaches our ESP32-C6 host: five scalar frame types " +
    "cross a 115200-baud UART, and that wire is the whole attack surface. The radar accepts " +
    "no public configuration commands at all — the only knobs that exist are where you put " +
    "it and how the host firmware judges its claims. That is exactly what this bench stages.");
  renderDeep(secDeep, data);

  const secPlace = section("place", "placement bench", "Put it somewhere — honestly",
    "Drag the person, move the mount, tilt the radar. The sector, the range bands and the " +
    "vitals envelope are the device's published geometry; the quality meter is grounded in " +
    "the vital-sign radar literature (the sweet spot near 0.7 m, the U-shaped error curve, " +
    "orientation projection — citations in the hardware notes). Presets stage Seeed's own " +
    "reference mounts.");
  const stage = buildStage(data, scene, onSceneChange);
  secPlace.append(stage);

  const secPipe = section("pipeline", "bytes → claims", "Watch the pipeline think",
    "Real wire bytes (SOF 0x01, big-endian header, XOR-inverted checksums) stream into a " +
    "line-for-line port of the firmware's parser and FSMs. What crosses the privacy " +
    "chokepoint is the entire vocabulary the device may ever speak: presence, an occupant " +
    "bucket, a range band. Everything else is read and dropped in front of you.");
  const knobs = buildKnobs(data, state, onKnobsChange);
  const consoleUi = buildConsole();
  const sandbox = buildSandbox();
  secPipe.append(knobs, sandbox, consoleUi);

  const secCards = section("cards", "standardized widgets", "Canary Cards — one entity, one card",
    "The same rule Home Assistant uses: a device announces entities; every surface renders " +
    "them through one small card vocabulary. These cards are built from the exact entity set " +
    "the firmware announces over MQTT discovery — including the honest gaps: a presence-only " +
    "build renders its BPM cards as provably absent, not just missing. The schema is " +
    "docs/standard/CANARY_CARDS.md; the renderer is ~200 lines any surface can port.");
  const cardsMountEl = el("div");
  secCards.append(cardsMountEl);

  const secGlass = section("glass", "the screen firmware", "On the glass",
    "The Sense Lab speaks the same wire vocabulary as every canary, so the display firmware " +
    "can watch this bench like any sibling. Boot the real canary-display firmware (compiled " +
    "to WebAssembly, the same artifact the emulator pages use) and this witness joins its " +
    "fleet through the display's own MQTT dispatcher.");
  const glass = buildGlassBridge(data, () => ({
    deviceId: state.deviceId,
    snapshot: pipeline.snapshot,
  }));
  secGlass.append(glass);

  const secPower = section("power", "heat → useful computation", "The power lab",
    "A witness converts watts into signed claims; everything else is waste heat. The rails " +
    "below are calibrated to Seeed's published 0.8 W kit figure (derivation + citations in " +
    "the hardware notes). Move the levers and watch claims-per-joule: the radar is the " +
    "useful computation and it cannot nap — the only discretionary consumer is the C6's " +
    "radio, and the shipped publish-on-change design already spends it well.");
  const powerKnobs = {
    modemSleep: false, heartbeatS: Math.round(data.device.heartbeat_ms / 1000),
    eventsPerHour: 12, txPerEventMs: 40, ledOn: true, lux: true,
  };
  const powerLab = buildPowerLab(data, powerKnobs, () => powerLab.update());
  secPower.append(powerLab);

  renderKeepGoing(data);

  function section(id, kicker, title, lede) {
    const s = el("section", "hub-section");
    s.id = id;
    if (kicker) s.append(el("div", "hub-kicker", kicker));
    if (title) s.append(el("h2", null, title));
    if (lede) s.append(el("p", "hub-lede", lede));
    mount.append(s);
    return s;
  }

  // ── version strip ──
  function renderVersionStrip(d) {
    const strip = $("#sense-versions");
    if (!strip) return;
    const chips = el("div", "hub-chips");
    for (const [label, val] of [
      ["device", d.device.name],
      ["board", d.device.board_short],
      ["firmware", "v" + d.device.fw_version],
      ["train", d.device.fw_train],
      ["radar SoC", d.hardware.soc.split(" ")[0]],
      ["senses", d.device.modality],
    ]) {
      const c = el("span", "chip");
      c.append(el("span", "hub-chip-k", label + " "), el("strong", null, val));
      chips.append(c);
    }
    strip.append(chips);
    strip.append(el("p", "fineprint hub-fresh",
      "every constant here — frame ids, FSM defaults, pins, power rails — is parsed from the " +
      "firmware and docs/hardware/mr60bha2_radar_notes.md by tools/gen_sense.py and " +
      "drift-checked in CI. Nothing is written twice."));
  }

  // ── §deep ──
  function renderDeep(s, d) {
    const two = el("div", "wap-two");
    const left = el("div", "wap-two-col");
    const cv = el("canvas", "sense-3d");
    cv.width = 420; cv.height = 320;
    left.append(cv);
    const dscene = new DeviceScene(cv, null);
    (BUILDERS["canary-sense"] || BUILDERS["canary-wap"])(dscene);
    upgradeRealShape(dscene, "canary-sense");
    dscene.start();

    const hwFacts = el("div", "wap-facts");
    for (const [k, v] of [
      ["Radar SoC", d.hardware.soc],
      ["Band", d.hardware.band],
      ["Sector", d.hardware.fov_deg + "° × " + d.hardware.fov_deg + "° detection sector"],
      ["Presence", d.hardware.presence_min_m + "–" + d.hardware.presence_max_m + " m (4 m effective per Seeed's own troubleshooting doc — bench flag)"],
      ["Vitals", "≤ " + d.hardware.vitals_max_m + " m, sweet spot ≈ " + d.hardware.vitals_ref_m + " m, single still person"],
      ["Displacement", "breathing " + d.hardware.breath_disp_mm + " mm · heartbeat " + d.hardware.heart_disp_mm + " mm chest motion, read as 60 GHz phase"],
      ["Host", d.device.board],
    ]) {
      const row = el("div", "wap-fact");
      row.append(el("span", "wap-fact-k", k), el("span", "wap-fact-v", v));
      hwFacts.append(row);
    }
    left.append(hwFacts);

    const right = el("div", "wap-two-col");
    right.append(buildProtocol(d));
    two.append(left, right);
    s.append(two);

    // what the radar offers that we refuse
    const drop = el("div", "sense-dropped");
    drop.append(el("h4", null, "Frames the radar speaks that this witness refuses to decode"));
    const tbl = el("table", "sense-frames pin-table");
    const tb = el("tbody");
    for (const f of d.protocol.dropped_frames) {
      const tr = el("tr");
      tr.append(el("td", "sense-mono", f.type_hex));
      tr.append(el("td", null, f.name));
      tr.append(el("td", "muted", f.why));
      tb.append(tr);
    }
    tbl.append(tb);
    drop.append(tbl);
    drop.append(el("p", "fineprint",
      "They arrive well-framed and are counted as unknown, then skipped — you can watch the " +
      "unknown counter tick in the pipeline below. The chest-phase waveform and the per-target " +
      "point cloud stay on the wire, by design (design doc §2.2)."));
    s.append(drop);
  }

  // ── §sandbox scenario buttons ──
  function buildSandbox() {
    const pad = el("div", "wap-sandbox sense-sandbox");
    const scenarios = [
      ["walk in", "a person enters the sector — watch debounce, then the signed event", () => {
        scene.person.x = 2.4; scene.person.y = 0.2; scene.person.moving = true;
        scene.person.posture = "standing";
      }],
      ["sit & rest", "still, facing, mid-range — vitals begin their lock confirm window", () => {
        scene.person.x = 1.1; scene.person.y = 0; scene.person.moving = false;
        scene.person.posture = "sitting"; scene.person.orientation = "facing";
      }],
      ["sleep (bedside)", "Seeed's reference wellbeing geometry — the strongest vitals placement", () => {
        applyMount("stand");
      }],
      ["second person", "vitals hard-suppress the moment count ≠ 1 — the firmware rule", () => {
        scene.secondPerson = !scene.secondPerson;
      }],
      ["leave", "absence zeroes the aggregate; clear timeout runs; presence_cleared signs", () => {
        scene.person.x = 7.5; scene.person.moving = false;
      }],
      ["lights out", "lux collapses while presence persists — the tamper-corroboration shape", () => {
        state.lights = !state.lights;
      }],
      ["unplug the radar", "silence drives the FSM to unknown on its stall deadline — never a freeze", () => {
        state.radarPlugged = !state.radarPlugged;
      }],
    ];
    for (const [label, blurb, fn] of scenarios) {
      const card = el("button", "card wap-sand-card");
      card.append(el("strong", null, label), el("span", "muted", blurb));
      card.addEventListener("click", () => { fn(); stage.redraw(); });
      pad.append(card);
    }
    return pad;
  }

  function applyMount(id) {
    const m = data.placement.mounts.find((x) => x.id === id);
    if (!m) return;
    scene.mount = m.id; scene.mountHeight = m.height_m; scene.tiltDeg = m.tilt_deg;
    Object.assign(scene.person, m.person);
  }

  // ── change handlers ──
  function onSceneChange() { stage.redraw(); }
  function onKnobsChange(opts) {
    if (opts.rebuild || opts.recfg) {
      pipeline = makePipeline();
    }
    stage.redraw();
    refreshCards(true);
  }

  // ── keep going ──
  function renderKeepGoing(d) {
    const s = section("more", "keep going", "Where this goes next", null);
    const grid = el("div", "hub-links");
    const items = [
      ["The hardware notes", "the deep dive this bench is parsed from — chip, physics, power, bench flags", d.docs.notes],
      ["The design doc", "why radar, the privacy classes, the two-track plan", d.docs.design],
      ["Canary Cards", "the widget-card schema — how every surface renders every peripheral", d.docs.cards],
      ["The firmware", "the canary-sense project this page stages", d.docs.firmware],
      ["The Hub", "Home Assistant — where these MQTT entities land", "canary-local/homeassistant.html"],
    ];
    for (const [title, body, path] of items) {
      const a = el("a", "hub-link");
      const local = path.startsWith("canary-local/");
      a.href = local ? path.replace("canary-local/", "") : GH + path;
      if (!local) { a.target = "_blank"; a.rel = "noopener noreferrer"; }
      a.append(el("strong", null, title), el("span", "muted", body), el("code", "fineprint", path));
      grid.append(a);
    }
    s.append(grid);
  }

  // ── the bench loop ──
  let phaseTicker = 0;
  let lastCardKey = "";

  async function recordEvent(name) {
    state.seq += 1;
    state.chainLen += 1;
    const snap = pipeline.snapshot;
    const bucket = bucketUptime(state.simMs);
    const canonical = senseCanonical(
      state.deviceId, state.seq, name, snap.presence, snap.count, snap.range, bucket);
    const sig = await signer.sign(canonical);
    consoleUi.pushEvent({ name, seq: state.seq, bucket, canonical, signed: !!sig, sig });
    pipeline.snapshot.last_event = name;
    glass.onEvent(name);
  }

  function refreshChokepoint(gen) {
    const agg = pipeline.parser.agg;
    const snap = pipeline.snapshot;
    const raw = [
      ["has_target", agg.has_target, false],
      ["target_count", agg.target_count, false],
      ["distance_cm", agg.distance_cm, true],
      ["breath_bpm", agg.breath_rate, !state.p1OptIn || !vitalsBuild()],
      ["heart_bpm", agg.heart_rate, !state.p1OptIn || !vitalsBuild()],
      ["phase waveform", "0x0A13 (unknown ×" + pipeline.parser.unknownCount() + ")", true],
    ];
    const out = [
      ["presence", snap.presence],
      ["occupants", snap.presence === "unknown" ? "0" : snap.count],
      ["range", snap.range],
    ];
    if (vitalsBuild()) {
      out.push(["breathing_locked", snap.breathing_locked ? "true" : "false"]);
      if (state.p1OptIn) {
        out.push(["breath_bpm", snap.bpm_valid ? snap.breath_bpm : "null"]);
        out.push(["heart_bpm", snap.bpm_valid ? snap.heart_bpm : "null"]);
      }
    }
    consoleUi.setChokepoint(raw, out);
  }

  function refreshCards(force) {
    const snap = { ...pipeline.snapshot };
    snap.lux = state.lights ? 138 + Math.round(rand() * 6) : 1;
    const meta = {
      vitalsBuild: vitalsBuild(),
      p1OptIn: state.p1OptIn,
      chain: { length: state.chainLen, badge: signer.ready ? "verified" : "unsigned", signed: signer.ready },
      trend: state.trend,
    };
    const key = JSON.stringify([snap.presence, snap.count, snap.range, snap.radar_ok,
      snap.breathing_locked, snap.bpm_valid, snap.breath_bpm, snap.heart_bpm,
      Math.round(snap.lux / 20), state.chainLen, meta.vitalsBuild, meta.p1OptIn]);
    if (!force && key === lastCardKey) return;
    lastCardKey = key;
    cardsMountEl.innerHTML = "";
    const cards = senseCards(snap, meta);
    // lights-out + presence: surface the tamper-corroboration shape
    const luxCard = cards.find((c) => c.id === "illuminance");
    if (luxCard && snap.lux < 5 && snap.presence === "present") {
      luxCard.severity = "warn";
      luxCard.footer = "lux collapsed while presence persists — tamper corroboration fires";
    }
    cardsMountEl.append(renderCardGrid(cards));
    glass.onSnapshot();
  }

  async function tick() {
    state.simMs += TICK;
    const now = state.simMs;

    if (state.radarPlugged) {
      const gen = sceneFrames(scene, data.hardware, TICK, now, rand);
      // every ~2.5 s the module also offers a phase-waveform frame; the
      // parser counts it unknown and stays in sync — watch the counter.
      phaseTicker += 1;
      let bytes = gen.bytes;
      if (phaseTicker % 25 === 0) {
        const phase = buildFrame(0x0a13, new Uint8Array(12));
        const merged = new Uint8Array(bytes.length + phase.length);
        merged.set(bytes); merged.set(phase, bytes.length);
        bytes = merged;
        gen.emitted.push("PHASE(unknown)");
      }
      if (bytes.length) consoleUi.pushHex(bytes, gen.emitted);
      pipeline.onFrame = (f) => consoleUi.pushFrame(f);
      pipeline.feed(bytes);
    } else if (now % 1000 < TICK) {
      consoleUi.pushStall();
    }

    const fired = pipeline.drive(now);
    for (const name of fired) await recordEvent(name);

    // vitals trend rings (P1 sparkline data)
    if (pipeline.snapshot.bpm_valid && now % 1000 < TICK) {
      state.trend.breath.push(pipeline.snapshot.breath_bpm);
      state.trend.heart.push(pipeline.snapshot.heart_bpm);
      if (state.trend.breath.length > 40) state.trend.breath.shift();
      if (state.trend.heart.length > 40) state.trend.heart.shift();
    }

    if (now % 500 < TICK) {
      refreshChokepoint();
      refreshCards(false);
    }
  }

  // Drive simulated time off the render clock, but never through Date —
  // the bench is deterministic for a given seed + interaction sequence.
  setInterval(() => { tick().catch((e) => console.error("sense tick", e)); }, TICK);
  refreshChokepoint();
  refreshCards(true);
}

main();
