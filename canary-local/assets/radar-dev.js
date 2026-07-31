// canary-local/assets/radar-dev.js — the Proving Ground page driver.
//
// One bench, two cables: a real flashed Sense over Web Serial, or the
// EmuSense twin (radar-emu.js) speaking the identical console dialect. The
// page cannot tell them apart past the transport seam — every line goes
// through the same flash-core.js parsers either way, which is the honesty
// property that makes the drills meaningful: a drill that passes on the twin
// is the exact behavior the cable must reproduce.
//
// Sections: the five-step journey (onboarding), the live bench, the drill
// suite (radar-drills.js DrillEngine), the zone-calibration wizard
// (calibPlan), and the placement studio (placementAdvice + the drift-gated
// placement copy from devices/sense.json).
//
// Failure posture (house rule): if the data files can't load, degrade to a
// pointer at the design doc — never a blank page.

import {
  parseSenseLine, parseCfgLine, parseTuneLine, senseLineTone,
} from "./flash-core.js";
import { EmuSense, knobTable, defaultScene, TICK_MS } from "./radar-emu.js";
import {
  drillsFor, DrillEngine, calibPlan, median, placementAdvice,
} from "./radar-drills.js";
import { chirp } from "./chirp.js";

const $ = (sel, root = document) => root.querySelector(sel);
const el = (tag, cls, text) => {
  const n = document.createElement(tag);
  if (cls) n.className = cls;
  if (text != null) n.textContent = text;
  return n;
};
const GH = "https://github.com/kmay89/securaCV/blob/main/";

const prefersCalm = () =>
  window.matchMedia && window.matchMedia("(prefers-reduced-motion: reduce)").matches;

// ── page state ──────────────────────────────────────────────────────────────

const state = {
  data: null,       // devices/sense.json
  hw: null,         // devices/senselab.json → hardware (the SIM physics)
  presets: null,    // devices/flash.json → sense product reflexes.presets
  transport: null,  // { kind:"usb"|"twin", send(cmd), close() }
  wellbeing: false, // detected from [cfg] (usb) or chosen (twin)
  synced: false,    // first [cfg] seen
  twin: null,       // EmuSense when kind === "twin"
  model: {
    presence: "unknown", count: "0", range: "unknown",
    breath: null, heart: null, locked: false,
    lastSeenMs: 0, frameErrs: null, raw: null,
  },
  cfgValues: {},
  engine: null,     // DrillEngine
  calib: { phase: "idle", nearSamples: [], midSamples: [], plan: null },
  listeners: [],    // onLine fanout
};

function emitLine(line) {
  for (const fn of state.listeners) {
    try { fn(line); } catch (e) { console.error("radar-dev line", e); }
  }
}

function send(cmd) {
  if (state.transport) state.transport.send(cmd);
}

// ── transports ──────────────────────────────────────────────────────────────

async function connectUsb() {
  const port = await navigator.serial.requestPort();
  await port.open({ baudRate: 115200 });
  const reader = port.readable.getReader();
  let writer = null;
  try { writer = port.writable.getWriter(); } catch { writer = null; }
  const enc = new TextEncoder();
  let alive = true;

  const transport = {
    kind: "usb",
    async send(cmd) {
      if (!writer) return;
      try { await writer.write(enc.encode(cmd + "\n")); } catch { /* mid-unplug */ }
    },
    async close() {
      alive = false;
      try { await reader.cancel(); } catch {}
      try { reader.releaseLock(); } catch {}
      try { writer && writer.releaseLock(); } catch {}
      try { await port.close(); } catch {}
    },
  };

  (async () => {
    const dec = new TextDecoder();
    let tail = "";
    try {
      for (;;) {
        const { value, done } = await reader.read();
        if (done || !alive) break;
        tail += dec.decode(value, { stream: true });
        const lines = tail.split("\n");
        tail = lines.pop() || "";
        for (const line of lines) emitLine(line.replace(/\r$/, ""));
      }
    } catch { /* unplugged — the disconnect button is right there */ }
    if (alive) setTransport(null);
  })();

  // Handshake: ask for the knob snapshot (twice — the board may still be
  // booting), then be honest if this firmware predates the console.
  setTimeout(() => { if (state.transport === transport && !state.synced) transport.send("cfg"); }, 600);
  setTimeout(() => { if (state.transport === transport && !state.synced) transport.send("cfg"); }, 2500);
  return transport;
}

function bootTwin(wellbeing) {
  const twin = new EmuSense({
    wellbeing,
    hw: state.hw,
    seed: 0x5e42,
    scene: defaultScene(),
  });
  twin.onLine = (line) => emitLine(line);
  let timer = null;
  const t0 = performance.now();
  const transport = {
    kind: "twin",
    send(cmd) { twin.write(cmd + "\n"); },
    close() { clearInterval(timer); },
  };
  state.twin = twin;
  state.wellbeing = wellbeing;
  // Boot theatre: the real banner, verbatim from the firmware (drift-gated
  // data) — so the twin's first breath looks exactly like the cable's.
  const banner = state.data.serial.banner || [];
  banner.forEach((l, i) => setTimeout(() => emitLine(l), i * 25));
  setTimeout(() => {
    timer = setInterval(() => twin.tick(performance.now() - t0), TICK_MS);
    transport.send("cfg");
  }, banner.length * 25 + 120);
  return transport;
}

const transportHooks = []; // sections push a re-render here

function setTransport(t) {
  if (state.transport && state.transport !== t) {
    try { state.transport.close(); } catch {}
  }
  state.transport = t;
  if (!t) {
    state.synced = false;
    state.twin = null;
    state.model.presence = "unknown";
  }
  state.engine = null; // a new cable is a new proof
  ui.renderJourney();
  ui.renderBenchHead();
  for (const fn of transportHooks) fn();
}

// ── the parsed-line pump: model + engine + calibration all feed from it ─────

function onLine(line) {
  ui.logLine(line);

  const cfg = parseCfgLine(line);
  if (cfg) {
    state.synced = true;
    state.cfgValues = { ...cfg.values };
    // The wellbeing build carries the vitals knobs in its [cfg] line —
    // that's how the bench knows without asking.
    if (state.transport && state.transport.kind === "usb") {
      state.wellbeing = Number.isFinite(cfg.values.vlock);
    }
    // The drill list depends on the detected flavor: an engine built before
    // this first [cfg] (or inherited across a reconnect) may carry the wrong
    // drills — rebuild it so a wellbeing board gets its vitals drill and a
    // presence board doesn't inherit one.
    if (state.engine && state.engine.wellbeing !== state.wellbeing) {
      state.engine = null;
      if (ui.renderDrills) ui.renderDrills();
    }
    ui.syncKnobs(cfg);
    ui.renderJourney();
  }

  const verdict = parseTuneLine(line);
  if (verdict) ui.tuneVerdict(verdict);

  const ev = cfg || verdict || parseSenseLine(line);
  const now = performance.now();
  if (ev && !cfg && !verdict) {
    state.model.lastSeenMs = now;
    if (ev.kind === "sense" || ev.kind === "presence" || ev.kind === "radar") {
      if (ev.presence === "present" && state.model.presence !== "present") chirp("found");
      state.model.presence = ev.presence;
      if (ev.count != null) state.model.count = ev.count;
      if (ev.range != null) state.model.range = ev.range;
    }
    if (ev.kind === "radar") {
      if (ev.lock) {
        state.model.locked = ev.lock === "locked";
        if (state.model.locked && Number.isFinite(ev.breath) && ev.breath > 0) {
          state.model.breath = ev.breath; state.model.heart = ev.heart;
        }
      }
      state.model.raw = ev.raw || null;
      if (Number.isFinite(ev.frame_errs)) state.model.frameErrs = ev.frame_errs;
      if (ev.raw && state.calib.phase === "near") state.calib.nearSamples.push(ev.raw.dist_cm);
      if (ev.raw && state.calib.phase === "mid") state.calib.midSamples.push(ev.raw.dist_cm);
      if (state.calib.phase === "near" || state.calib.phase === "mid") ui.calibProgress();
    }
    if (ev.kind === "bpm") {
      state.model.breath = ev.breath; state.model.heart = ev.heart; state.model.locked = true;
    }
    if (ev.kind === "vitals") state.model.locked = !!ev.locked;
    ui.renderStatus();
  }

  if (state.engine && ev) state.engine.feed(ev, now);
}

// ── boot ────────────────────────────────────────────────────────────────────

async function main() {
  const mount = $("#radar-dev");
  try {
    const [sense, senselab, flash] = await Promise.all([
      fetch("devices/sense.json").then((r) => { if (!r.ok) throw new Error("sense.json HTTP " + r.status); return r.json(); }),
      fetch("devices/senselab.json").then((r) => { if (!r.ok) throw new Error("senselab.json HTTP " + r.status); return r.json(); }),
      fetch("devices/flash.json").then((r) => (r.ok ? r.json() : null)).catch(() => null),
    ]);
    state.data = sense;
    state.hw = senselab.hardware;
    if (flash && Array.isArray(flash.products)) {
      const p = flash.products.find((x) => x.id === "securacv-canary-sense") ||
                flash.products.find((x) => /canary-sense/.test(x.id));
      state.presets = p && p.reflexes && p.reflexes.presets ? p.reflexes.presets : null;
    }
  } catch (e) {
    mount.append(el("p", "muted",
      "The Proving Ground data failed to load (" + e.message + "). Everything it stages is " +
      "also written out in the design doc."));
    const a = el("a", null, "Open the canary-sense design doc →");
    a.href = GH + "docs/canary_sense_mr60bha2_design.md";
    mount.append(a);
    return;
  }

  state.listeners.push(onLine);
  buildPage(mount);
}

// ── page assembly ───────────────────────────────────────────────────────────

const ui = {}; // render hooks filled in by the builders below

function section(mount, id, kicker, title, lede) {
  const s = el("section", "hub-section");
  s.id = id;
  s.append(el("div", "hub-kicker", kicker));
  s.append(el("h2", null, title));
  if (lede) s.append(el("p", "hub-lede", lede));
  mount.append(s);
  return s;
}

function buildPage(mount) {
  renderVersionStrip();
  buildJourney(section(mount, "journey", "from cable to confidence", "Five steps, one green wall",
    "Right after a flash the radar is a silent black box — this page is its voice. " +
    "Follow the steps in order; each one lights when the board (real or twin) proves it."));
  buildBench(section(mount, "bench", "the bench", "What the radar sees, live",
    "The same coarse truths it publishes to Home Assistant — present/clear, 0/1/2+, " +
    "near/mid/far — plus every knob, live on this cable. No board on hand? The twin " +
    "speaks the identical console dialect, generated by the firmware's own ported pipeline."));
  buildDrills(section(mount, "drills", "prove it", "The drill suite",
    "Nine checks between “it flashed” and “it works”. The auto drills prove the cable, " +
    "console and framing by themselves; the guided ones need your legs — walk, sit, " +
    "bring a friend. Run them on the twin first to see what passing looks like."));
  buildCalibrate(section(mount, "calibrate", "make it yours", "Calibrate the zones to this room",
    "The near/mid/far bands are the firmware's range gates — and the defaults are a guess " +
    "about your room. Mark your real edges and the wizard turns them into numbers, applies " +
    "them over the console, and saves them to the chip."));
  buildPlacement(section(mount, "placement", "where it lives", "Placement — get the physics on your side",
    "Placement is the tuning nobody can do in software. The three blessed geometries, the " +
    "keep-out list, and a scorer running the same physics model the Sense Lab stages."));
  buildKeepGoing(mount);

  ui.renderJourney();
  ui.renderBenchHead();
  ui.renderStatus();
}

function renderVersionStrip() {
  const strip = $("#radar-dev-versions");
  if (!strip) return;
  const d = state.data;
  const chips = el("div", "hub-chips");
  for (const [label, val] of [
    ["device", d.device.name],
    ["radar", d.radar.module],
    ["firmware", "v" + d.device.fw_version],
    ["console", d.serial.baud + " 8N1"],
  ]) {
    const c = el("span", "chip");
    c.append(el("span", "hub-chip-k", label + " "), el("strong", null, val));
    chips.append(c);
  }
  strip.append(chips);
}

// ── 1 · the journey ─────────────────────────────────────────────────────────

function buildJourney(s) {
  const steps = [
    {
      id: "plug", title: "Plug it in",
      body: "Use the XIAO's own USB-C — the small board inside the case, not the kit's " +
        "reachable power port. If the device chooser stays empty later, you're on the wrong port.",
      done: () => !!state.transport,
    },
    {
      id: "connect", title: "Connect — or boot the twin",
      body: "Web Serial needs Chrome or Edge. No board, no cable, wrong browser? The twin " +
        "is the full firmware pipeline in this page — same console, same drills.",
      done: () => !!state.transport,
    },
    {
      id: "hear", title: "Hear it speak",
      body: "The bench asks for the knob snapshot; the first [cfg] reply proves the tuning " +
        "console is alive and syncs every slider below to what the chip actually holds.",
      done: () => state.synced,
    },
    {
      id: "prove", title: "Prove it senses",
      body: "Run the drills — auto first, then the guided ones. A full green wall means the " +
        "radar, the parser, the FSMs and the timings all work in your actual room.",
      done: () => state.engine && state.engine.summary().pass >= (state.engine.drills.length - 1),
    },
    {
      id: "settle", title: "Calibrate, then mount it",
      body: "Mark your zone edges so the bands mean what you mean, then let the placement " +
        "studio grade the spot before you commit to screws.",
      done: () => !!(state.calib.plan && state.calib.applied),
    },
  ];

  const rail = el("ol", "rdev-journey");
  s.append(rail);

  ui.renderJourney = () => {
    rail.innerHTML = "";
    let firstOpen = null;
    steps.forEach((st, i) => {
      const li = el("li", "rdev-step");
      const done = st.done();
      if (done) li.classList.add("rdev-step-done");
      else if (firstOpen === null) { firstOpen = i; li.classList.add("rdev-step-now"); }
      const dot = el("span", "rdev-step-dot", done ? "✓" : String(i + 1));
      const body = el("div", "rdev-step-body");
      body.append(el("strong", null, st.title), el("p", "muted", st.body));
      if (st.id === "connect" && !done) body.append(connectControls());
      li.append(dot, body);
      rail.append(li);
    });
  };
}

function connectControls() {
  const row = el("div", "flash-row rdev-connect");
  const hasSerial = "serial" in navigator;
  const usb = el("button", "primary", "🔌 connect the flashed Sense");
  usb.disabled = !hasSerial;
  usb.addEventListener("click", async () => {
    try {
      const t = await connectUsb();
      setTransport(t);
    } catch (e) {
      ui.tuneVerdict({ ok: false, text: "no port opened (" + String(e.message || e) + ")" });
    }
  });
  const twinBtn = el("button", "ghost", "🐣 boot the twin — presence build");
  twinBtn.addEventListener("click", () => setTransport(bootTwin(false)));
  const twinWb = el("button", "ghost", "🫀 boot the twin — wellbeing build");
  twinWb.addEventListener("click", () => setTransport(bootTwin(true)));
  row.append(usb, twinBtn, twinWb);
  if (!hasSerial) {
    row.append(el("p", "fineprint muted",
      "This browser has no Web Serial — use Chrome or Edge for the live cable " +
      "(or the desktop Flasher app), and the twin right here meanwhile."));
  }
  return row;
}

// ── 2 · the bench ───────────────────────────────────────────────────────────

function buildBench(s) {
  const head = el("div", "rdev-bench-head");
  const srcBadge = el("span", "flash-passport-chip", "no cable, no twin — start above");
  srcBadge.setAttribute("role", "status");
  const discBtn = el("button", "ghost small", "✕ disconnect");
  discBtn.hidden = true;
  discBtn.addEventListener("click", () => setTransport(null));
  head.append(srcBadge, discBtn);
  s.append(head);

  const status = el("div", "flash-sense-status", "◌ listening for the radar…");
  status.setAttribute("role", "status");
  s.append(status);
  const rawLine = el("div", "flash-sense-rawline");
  rawLine.hidden = true;
  s.append(rawLine);

  const aura = document.createElement("canvas");
  aura.className = "flash-sense-aura";
  aura.width = 640; aura.height = 340;
  s.append(aura);

  // Twin scene levers — only shown when the twin is on the cable.
  const scenePanel = buildScenePanel();
  s.append(scenePanel.box);

  // The tuning suite (same contract as the Nursery's bench: reconcile only
  // to [cfg], enable nothing before the first sync).
  const tune = el("div", "flash-sense-tune");
  const tuneHead = el("div", "flash-sense-vitals-head");
  const tuneBadge = el("span", "flash-passport-chip", "syncing with the board…");
  tuneBadge.setAttribute("role", "status");
  tuneHead.append(el("strong", null, "Tuning suite — every knob, live"), tuneBadge);
  tune.append(tuneHead);
  const knobGrid = el("div", "flash-sense-knobs");
  tune.append(knobGrid);
  const tunectl = el("div", "flash-row flash-sense-tunectl");
  const resetBtn = el("button", "ghost", "↺ restore defaults");
  resetBtn.disabled = true;
  resetBtn.addEventListener("click", () => send("reset"));
  const streamSel = document.createElement("select");
  streamSel.className = "flash-sense-streamsel";
  [["500", "stream: 2×/s"], ["1000", "stream: 1×/s"], ["2000", "stream: every 2 s"], ["0", "stream: off"]]
    .forEach(([v, label]) => {
      const o = document.createElement("option");
      o.value = v; o.textContent = label;
      streamSel.append(o);
    });
  streamSel.value = "1000";
  streamSel.disabled = true;
  streamSel.addEventListener("change", () =>
    send(streamSel.value === "0" ? "stream off" : `stream ${streamSel.value}`));
  const rawLab = el("label", "flash-sense-rawtoggle");
  const rawChk = document.createElement("input");
  rawChk.type = "checkbox";
  rawChk.disabled = true;
  rawChk.addEventListener("change", () => send(rawChk.checked ? "raw on" : "raw off"));
  rawLab.append(rawChk, document.createTextNode(" bench detail (raw cm & bpm — stays on this cable)"));
  tunectl.append(resetBtn, streamSel, rawLab);
  tune.append(tunectl);
  s.append(tune);

  // The live console.
  const conWrap = el("details", "flash-displaybench-serial");
  conWrap.open = true;
  conWrap.append(el("summary", null, "the live console — every line the radar speaks"));
  const conCtl = el("div", "flash-row flash-sense-logctl");
  const pauseBtn = el("button", "ghost", "⏸ hold the scroll");
  let paused = false;
  pauseBtn.addEventListener("click", () => {
    paused = !paused;
    pauseBtn.textContent = paused ? "▶ follow again" : "⏸ hold the scroll";
  });
  const clearBtn = el("button", "ghost", "✕ clear");
  const cmdIn = document.createElement("input");
  cmdIn.type = "text";
  cmdIn.className = "rdev-cmd";
  cmdIn.placeholder = "type a console command — try: help";
  cmdIn.addEventListener("keydown", (e) => {
    if (e.key !== "Enter" || !cmdIn.value.trim()) return;
    logLine("> " + cmdIn.value.trim());
    send(cmdIn.value.trim());
    cmdIn.value = "";
  });
  conCtl.append(pauseBtn, clearBtn, cmdIn);
  conWrap.append(conCtl);
  const con = el("div", "flash-console flash-sense-log");
  clearBtn.addEventListener("click", () => { con.textContent = ""; });
  conWrap.append(con);
  s.append(conWrap);

  function logLine(text) {
    if (!text.trim()) return;
    con.append(el("div", "flash-senseline tone-" + senseLineTone(text), text));
    while (con.childElementCount > 400) con.firstElementChild.remove();
    if (!paused) con.scrollTop = con.scrollHeight;
  }

  const knobEls = new Map();
  function rebuildKnobs() {
    knobGrid.innerHTML = "";
    knobEls.clear();
    for (const k of knobTable(state.wellbeing)) {
      const row = el("label", "flash-sense-knob");
      row.append(el("span", "flash-sense-knob-name", k.name));
      const input = document.createElement("input");
      input.type = "range";
      input.min = String(k.lo);
      input.max = String(k.hi);
      input.step = k.unit === "cm" ? "10" : k.unit === "bpm" ? "1" : "50";
      input.value = String(k.def);
      input.disabled = !state.synced;
      const val = el("span", "flash-sense-knob-val", `${k.def} ${k.unit}`);
      input.addEventListener("input", () => { val.textContent = `${input.value} ${k.unit}`; });
      input.addEventListener("change", () => send(`set ${k.name} ${input.value}`));
      row.append(input, val);
      row.title = `${k.help} — default ${k.def} ${k.unit}, range ${k.lo}–${k.hi}`;
      knobGrid.append(row);
      knobEls.set(k.name, { input, val, unit: k.unit });
    }
  }
  rebuildKnobs();

  // ── render hooks ──
  ui.logLine = logLine;

  // Disarm every tuning control until the CURRENT cable's [cfg] sync — a
  // knob left enabled from the previous board must not be able to write
  // values onto the next one during the handshake.
  ui.resetTuning = () => {
    rebuildKnobs(); // inputs come back disabled while !state.synced
    resetBtn.disabled = true;
    streamSel.disabled = true;
    streamSel.value = "1000";
    rawChk.disabled = true;
    rawChk.checked = false;
  };

  ui.renderBenchHead = () => {
    const t = state.transport;
    discBtn.hidden = !t;
    scenePanel.box.hidden = !t || t.kind !== "twin";
    if (!state.synced) ui.resetTuning();
    if (!t) {
      srcBadge.textContent = "no cable, no twin — start above";
      srcBadge.className = "flash-passport-chip";
      tuneBadge.textContent = "syncing with the board…";
      tuneBadge.className = "flash-passport-chip";
    } else if (t.kind === "usb") {
      srcBadge.textContent = "LIVE — a real Sense on the cable";
      srcBadge.className = "flash-passport-chip flash-passport-ok";
    } else {
      srcBadge.textContent = "TWIN — emulated Sense (" + (state.wellbeing ? "wellbeing" : "presence") + " build)";
      srcBadge.className = "flash-passport-chip chip-dev";
    }
  };

  ui.syncKnobs = (cfg) => {
    rebuildKnobs();
    for (const [name, kui] of knobEls) {
      if (!Number.isFinite(cfg.values[name])) continue;
      kui.input.value = String(cfg.values[name]);
      kui.val.textContent = `${cfg.values[name]} ${kui.unit}`;
      kui.input.disabled = false;
    }
    resetBtn.disabled = false;
    streamSel.disabled = false;
    rawChk.disabled = false;
    if (Number.isFinite(cfg.stream)) {
      const v = String(cfg.stream);
      streamSel.value = [...streamSel.options].some((o) => o.value === v) ? v : "1000";
    }
    if (typeof cfg.raw === "boolean") rawChk.checked = cfg.raw;
    tuneBadge.textContent = "LIVE — knobs synced with the chip";
    tuneBadge.className = "flash-passport-chip flash-passport-ok";
  };

  ui.tuneVerdict = (v) => {
    tuneBadge.textContent = (v.ok ? "✓ " : "⚠ ") + v.text;
    tuneBadge.className = "flash-passport-chip " + (v.ok ? "flash-passport-ok" : "flash-passport-warn");
  };

  ui.renderStatus = () => {
    const m = state.model;
    status.className = "flash-sense-status flash-sense-" + m.presence;
    status.textContent =
      m.presence === "present"
        ? `● someone's here — ${m.count === "2+" ? "two or more" : m.count === "1" ? "one person" : "movement"} · ${m.range}` +
          (state.wellbeing && m.locked && m.breath ? ` · breathing ${m.breath} · heart ${m.heart} bpm` : "")
        : m.presence === "clear" ? "○ clear — the room is empty"
        : "◌ listening for the radar…";
    if (m.raw) {
      rawLine.hidden = false;
      rawLine.textContent =
        `bench detail · distance ${m.raw.dist_cm} cm · targets ${m.raw.count}` +
        (state.wellbeing ? ` · raw breath ${m.raw.breath} · raw heart ${m.raw.heart} bpm` : "");
    } else rawLine.hidden = true;
  };

  // Handshake honesty timer for USB (twin syncs instantly).
  setInterval(() => {
    if (state.transport && state.transport.kind === "usb" && !state.synced) {
      tuneBadge.textContent = "no tuning console yet — still booting, or reflash with the latest firmware";
      tuneBadge.className = "flash-passport-chip flash-passport-warn";
    }
  }, 5000);

  startAura(aura);
}

// The aura: bands, sweep, target — the Nursery's drawing, leaner.
function startAura(aura) {
  const actx = aura.getContext("2d");
  const calm = prefersCalm();
  const BAND_R = { near: 0.36, mid: 0.62, far: 0.88, unknown: 0.62 };
  let targetR = 0, targetA = 0;
  const t0 = performance.now();

  function draw(now) {
    const m = state.model;
    // The first rAF timestamp can precede the t0 captured above — a negative
    // t makes sweepT % 1 negative and arc() throws on the negative radius.
    const t = calm ? 0 : Math.max(0, now - t0) / 1000;
    const W = aura.width, H = aura.height;
    const cx = W / 2, cy = H - 14, R = H - 40;
    actx.clearRect(0, 0, W, H);
    const present = m.presence === "present";
    const stale = state.transport && m.lastSeenMs && now - m.lastSeenMs > 15000;

    for (const [name, r] of [["near", BAND_R.near], ["mid", BAND_R.mid], ["far", BAND_R.far]]) {
      const active = present && m.range === name;
      actx.beginPath();
      actx.arc(cx, cy, r * R, Math.PI, 2 * Math.PI);
      actx.lineWidth = active ? 5 : 1.5;
      actx.strokeStyle = active
        ? `hsl(140 90% 62% / ${0.75 + 0.25 * Math.sin(t * 4)})`
        : "rgba(160,180,200,0.25)";
      actx.stroke();
      actx.font = "600 11px ui-monospace, Menlo, monospace";
      actx.fillStyle = active ? "#7CFF9B" : "rgba(160,180,200,0.5)";
      actx.fillText(name, cx + r * R - 26, cy - 8);
    }
    const sweepT = (t * (present ? 0.55 : 0.22)) % 1;
    actx.beginPath();
    actx.arc(cx, cy, sweepT * R, Math.PI, 2 * Math.PI);
    actx.lineWidth = 2;
    actx.strokeStyle = `hsl(${present ? 140 : 205} 80% 60% / ${0.5 * (1 - sweepT)})`;
    actx.stroke();
    actx.beginPath();
    actx.arc(cx, cy, 6, 0, 2 * Math.PI);
    actx.fillStyle = stale ? "#f0a860" : present ? "#7CFF9B" : "#7db8e8";
    actx.fill();
    const wantR = present ? (BAND_R[m.range] || BAND_R.mid) * R : 0;
    targetR += (wantR - targetR) * 0.08;
    targetA += ((present ? 1 : 0) - targetA) * 0.1;
    if (targetA > 0.02) {
      const bob = Math.sin(t * 2.2) * 4;
      const tx = cx, ty = cy - targetR + bob;
      const glow = actx.createRadialGradient(tx, ty, 2, tx, ty, 26);
      glow.addColorStop(0, `hsl(140 90% 66% / ${0.9 * targetA})`);
      glow.addColorStop(1, "hsl(140 90% 66% / 0)");
      actx.fillStyle = glow;
      actx.beginPath(); actx.arc(tx, ty, 26, 0, 2 * Math.PI); actx.fill();
      actx.fillStyle = `hsl(140 90% 70% / ${targetA})`;
      actx.beginPath(); actx.arc(tx, ty, 7, 0, 2 * Math.PI); actx.fill();
      if (m.count === "2+") {
        actx.fillStyle = `hsl(140 90% 70% / ${0.8 * targetA})`;
        actx.beginPath(); actx.arc(tx + 22, ty + 6, 5, 0, 2 * Math.PI); actx.fill();
        actx.font = "700 12px ui-monospace, Menlo, monospace";
        actx.fillText("2+", tx + 32, ty + 10);
      }
    }
    if (stale) {
      actx.font = "600 12px ui-monospace, Menlo, monospace";
      actx.fillStyle = "#f0a860";
      actx.fillText("no radar lines lately — is it still plugged in?", 14, 18);
    }
    requestAnimationFrame(draw);
  }
  requestAnimationFrame(draw);
}

// The twin's scene levers — walk the virtual room without leaving the chair.
function buildScenePanel() {
  const box = el("div", "rdev-scene");
  box.hidden = true;
  const head = el("div", "flash-sense-vitals-head");
  head.append(el("strong", null, "The virtual room — drive the twin"),
    el("span", "flash-passport-chip chip-dev", "scene → wire bytes → the real parser"));
  box.append(head);

  const grid = el("div", "rdev-scene-grid");

  const distRow = el("label", "flash-sense-knob");
  distRow.append(el("span", "flash-sense-knob-name", "distance"));
  const dist = document.createElement("input");
  dist.type = "range"; dist.min = "0.3"; dist.max = "8"; dist.step = "0.1"; dist.value = "6.5";
  const distVal = el("span", "flash-sense-knob-val", "6.5 m (out of view)");
  dist.addEventListener("input", () => {
    const x = Number(dist.value);
    distVal.textContent = x.toFixed(1) + " m";
    if (state.twin) state.twin.setScene({ person: { x } });
  });
  distRow.append(dist, distVal);
  grid.append(distRow);

  const toggles = el("div", "flash-row rdev-scene-toggles");
  const mkToggle = (label, apply) => {
    const b = el("button", "ghost small", label);
    b.dataset.on = "0";
    b.addEventListener("click", () => {
      const on = b.dataset.on !== "1";
      b.dataset.on = on ? "1" : "0";
      b.classList.toggle("rdev-on", on);
      apply(on);
    });
    return b;
  };
  toggles.append(
    mkToggle("🚶 moving", (on) => state.twin && state.twin.setScene({ person: { moving: on } })),
    mkToggle("👥 second person", (on) => state.twin && state.twin.setScene({ secondPerson: on })),
    mkToggle("🌀 fan in view", (on) => state.twin && state.twin.setScene({ fan: on })),
    mkToggle("🔌 unplug the radar", (on) => state.twin && state.twin.setScene({ unplugged: on })),
  );
  grid.append(toggles);

  const scen = el("div", "flash-row rdev-scenarios");
  const mkScen = (label, fn) => {
    const b = el("button", "ghost small", label);
    b.addEventListener("click", () => {
      if (!state.twin) return;
      fn();
      // reflect the levers
      dist.value = String(state.twin.scene.person.x);
      distVal.textContent = Number(dist.value).toFixed(1) + " m";
      for (const t of toggles.querySelectorAll("button")) {
        const map = { "🚶 moving": state.twin.scene.person.moving, "👥 second person": state.twin.scene.secondPerson,
          "🌀 fan in view": state.twin.scene.fan, "🔌 unplug the radar": state.twin.scene.unplugged };
        const on = !!map[t.textContent];
        t.dataset.on = on ? "1" : "0";
        t.classList.toggle("rdev-on", on);
      }
    });
    return b;
  };
  scen.append(
    mkScen("walk in", () => state.twin.setScene({ unplugged: false, person: { x: 2.5, moving: true }, secondPerson: false })),
    mkScen("sit & rest", () => state.twin.setScene({ unplugged: false, person: { x: 1.2, moving: false, posture: "sitting", orientation: "facing" }, secondPerson: false, fan: false })),
    mkScen("leave", () => state.twin.setScene({ person: { x: 7.5, moving: false }, secondPerson: false })),
  );
  grid.append(scen);
  box.append(grid);
  box.append(el("p", "fineprint muted",
    "Every lever changes the scene; the scene generates real MR60 wire bytes; the bytes run " +
    "the firmware's ported parser and FSMs. Nothing here shortcuts the pipeline — that's why " +
    "the drills mean the same thing on the twin as on the cable."));
  return { box };
}

// ── 3 · the drills ──────────────────────────────────────────────────────────

function buildDrills(s) {
  const wall = el("div", "rdev-drills");
  s.append(wall);
  const note = el("p", "fineprint muted",
    "One drill runs at a time. Auto drills settle by themselves in seconds; guided drills " +
    "wait for the room to move — you. Nothing a drill sends changes your knobs.");
  s.append(note);

  let ticker = null;
  function ensureEngine() {
    if (state.engine) return state.engine;
    state.engine = new DrillEngine(drillsFor(state.wellbeing), send, state.wellbeing);
    state.engine.onSettle = (id, r) => {
      if (r.status === "pass") chirp("found");
      render();
      ui.renderJourney();
    };
    if (!ticker) ticker = setInterval(() => { if (state.engine) { state.engine.tick(performance.now()); render(); } }, 500);
    return state.engine;
  }

  function render() {
    wall.innerHTML = "";
    if (!state.transport) {
      wall.append(el("p", "muted", "Connect a Sense (or boot the twin) to arm the drills."));
      return;
    }
    const eng = ensureEngine();
    for (const d of eng.drills) {
      const row = el("div", "rdev-drill");
      const res = eng.results[d.id];
      const active = eng.active && eng.active.d.id === d.id;
      const stateChip = el("span", "rdev-drill-state " +
        (active ? "rdev-armed" : res ? (res.status === "pass" ? "rdev-pass" : "rdev-fail") : ""),
        active ? "● armed — " + d.ask
          : res ? (res.status === "pass" ? `✓ passed in ${(res.ms / 1000).toFixed(1)} s` : "✕ " + (res.reason || "failed"))
          : d.kind === "auto" ? "auto" : "guided");
      const body = el("div", "rdev-drill-body");
      body.append(el("strong", null, d.title), el("span", "muted", " — " + d.prove));
      const btn = el("button", active ? "danger small" : "ghost small", active ? "stop" : res ? "re-run" : "run");
      btn.addEventListener("click", () => {
        if (active) eng.disarm();
        else eng.arm(d.id, performance.now());
        render();
      });
      row.append(btn, body, stateChip);
      wall.append(row);
    }
    const sum = eng.summary();
    const bar = el("div", "rdev-drill-sum",
      `${sum.pass} passed · ${sum.fail} failed · ${sum.total - sum.done} to go` +
      (sum.pass === sum.total ? " — a full green wall 🎉" : ""));
    const runAuto = el("button", "primary small", eng.autoChain ? "… running" : "▶ run the auto drills");
    runAuto.disabled = !!eng.autoChain;
    runAuto.addEventListener("click", () => {
      if (eng.autoChain) return;
      const autos = eng.drills.filter((d) => d.kind === "auto").map((d) => d.id);
      const onSettleBase = eng.onSettle;
      eng.autoChain = true;
      const next = () => {
        const id = autos.shift();
        if (!id) { eng.onSettle = onSettleBase; eng.autoChain = false; render(); return; }
        eng.arm(id, performance.now());
        render();
      };
      eng.onSettle = (id, r) => { onSettleBase(id, r); next(); };
      next();
    });
    bar.append(runAuto);
    wall.append(bar);
  }

  ui.renderDrills = render;
  transportHooks.push(render);
  render();
}

// ── 4 · calibration ─────────────────────────────────────────────────────────

function buildCalibrate(s) {
  const wrap = el("div", "rdev-calib");
  s.append(wrap);

  const round = (xs) => (xs.length ? `${median(xs)} cm over ${xs.length} readings` : "—");

  function render() {
    wrap.innerHTML = "";
    const c = state.calib;
    if (!state.transport) {
      wrap.append(el("p", "muted", "Connect a Sense (or boot the twin) first — the wizard drives the live console."));
      return;
    }

    const steps = el("ol", "we2-guide-steps");
    steps.append(
      el("li", null, "The wizard flips on bench detail (raw cm — stays on this cable) and a fast stream."),
      el("li", null, "Stand at the edge of your CLOSE zone (arm's reach of the thing you care about). Hold still ~4 s."),
      el("li", null, "Walk to the far edge of the zone you want watched. Hold still ~4 s."),
      el("li", null, "Review the numbers, apply — they hit the live FSMs at once and are written to the chip's settings (best-effort NVS); the [cfg] echo is the confirmation of what it now holds."),
    );
    wrap.append(steps);

    const row = el("div", "flash-row");
    const startBtn = el("button", c.phase === "idle" || c.phase === "done" ? "primary" : "ghost", "① start — mark the near edge");
    startBtn.addEventListener("click", () => {
      c.phase = "near"; c.nearSamples = []; c.midSamples = []; c.plan = null; c.applied = false;
      send("raw on"); send("stream 500");
      render();
    });
    const midBtn = el("button", c.phase === "near" && c.nearSamples.length >= 5 ? "primary" : "ghost", "② now the far edge");
    midBtn.disabled = !(c.phase === "near" && c.nearSamples.length >= 5);
    midBtn.addEventListener("click", () => { c.phase = "mid"; render(); });
    const finishBtn = el("button", c.phase === "mid" && c.midSamples.length >= 5 ? "primary" : "ghost", "③ compute the plan");
    finishBtn.disabled = !(c.phase === "mid" && c.midSamples.length >= 5);
    finishBtn.addEventListener("click", () => {
      c.plan = calibPlan(c.nearSamples, c.midSamples);
      c.phase = "done";
      send("raw off"); send("stream 1000");
      render();
    });
    row.append(startBtn, midBtn, finishBtn);
    wrap.append(row);

    const facts = el("div", "wap-facts");
    for (const [k, v] of [
      ["near mark", round(c.nearSamples)],
      ["far mark", round(c.midSamples)],
      ["phase", c.phase === "near" ? "collecting the NEAR edge — hold still"
        : c.phase === "mid" ? "collecting the FAR edge — hold still"
        : c.phase === "done" ? "plan ready" : "not started"],
    ]) {
      const r = el("div", "wap-fact");
      r.append(el("span", "wap-fact-k", k), el("span", "wap-fact-v", v));
      facts.append(r);
    }
    wrap.append(facts);

    if (c.plan && c.plan.near_cm != null) {
      const planBox = el("div", "rdev-plan");
      planBox.append(el("strong", null,
        `Recommendation: near = ${c.plan.near_cm} cm, mid = ${c.plan.mid_cm} cm`));
      for (const n of c.plan.notes) planBox.append(el("p", "fineprint muted", n));
      const apply = el("button", "primary", c.applied ? "✓ applied to the chip" : "apply → set near & mid");
      apply.disabled = !!c.applied;
      apply.addEventListener("click", () => {
        send(`set near ${c.plan.near_cm}`);
        send(`set mid ${c.plan.mid_cm}`);
        c.applied = true;
        render();
        ui.renderJourney();
      });
      planBox.append(apply);
      planBox.append(el("p", "fineprint muted",
        "Then verify with the “three bands” drill above: walk the room and the bands should flip where you marked them."));
      wrap.append(planBox);
    }

    // Room presets — the same six rooms the flasher bakes at install time.
    if (state.presets && state.presets.length) {
      const pd = el("details", "rdev-presets");
      pd.append(el("summary", null, "or start from a room preset — the flasher's six rooms"));
      const pr = el("div", "flash-row");
      for (const p of state.presets) {
        const b = el("button", "ghost small", (p.icon ? p.icon + " " : "") + (p.title || p.id));
        b.title = p.blurb || "";
        b.addEventListener("click", () => {
          for (const [key, v] of Object.entries(p.values || {})) send(`set ${key} ${v}`);
        });
        pr.append(b);
      }
      pd.append(pr, el("p", "fineprint muted",
        "Each button speaks plain `set` commands — watch the console echo every one, then fine-tune."));
      wrap.append(pd);
    }

    // The knob playbook, straight from the drift-gated tuning guidance.
    const play = el("details", "rdev-playbook");
    play.append(el("summary", null, "when to raise what — the tuning playbook"));
    const grid = el("div", "wap-mqtt-ents");
    for (const k of state.data.tuning.knobs) {
      const rrow = el("div", "wap-ent");
      rrow.append(el("span", "wap-ent-comp", k.name), el("span", "wap-ent-name", k.does),
        el("span", "wap-ent-topic", (k.raise_when && k.raise_when !== "—" ? "raise: " + k.raise_when + " " : "") +
          (k.lower_when && k.lower_when !== "—" ? "· lower: " + k.lower_when : "")));
      grid.append(rrow);
    }
    play.append(grid);
    wrap.append(play);
  }

  ui.calibProgress = render;
  transportHooks.push(render);
  render();
}

// ── 5 · placement ───────────────────────────────────────────────────────────

function buildPlacement(s) {
  const d = state.data;

  // The three blessed geometries — verbatim, source-badged.
  const mountGrid = el("div", "rdev-mounts");
  for (const m of d.placement.mounts) {
    const card = el("div", "rdev-mount");
    card.append(el("strong", null, m.name));
    const facts = el("div", "wap-facts");
    for (const [k, v] of [["height", m.height], ["aim", m.aim], ["range", m.range], ["best for", m.best_for]]) {
      if (!v || v === "—") continue;
      const r = el("div", "wap-fact");
      r.append(el("span", "wap-fact-k", k), el("span", "wap-fact-v", v));
      facts.append(r);
    }
    card.append(facts, el("span", "sense-src sense-src-" + m.src, m.src));
    mountGrid.append(card);
  }
  s.append(mountGrid);

  // The interactive scorer.
  const lab = el("div", "rdev-place-lab");
  lab.append(el("h3", "wap-col-h", "Score a spot before you drill a hole"));
  const modeRow = el("div", "flash-row");
  let mode = "presence";
  const bPres = el("button", "primary small", "presence (wall/shelf)");
  const bVit = el("button", "ghost small", "vitals (bedside)");
  bPres.addEventListener("click", () => { mode = "presence"; bPres.className = "primary small"; bVit.className = "ghost small"; render(); });
  bVit.addEventListener("click", () => { mode = "vitals"; bVit.className = "primary small"; bPres.className = "ghost small"; render(); });
  modeRow.append(bPres, bVit);
  lab.append(modeRow);

  const scene = defaultScene();
  scene.person.x = 2.5;
  const sliders = el("div", "flash-sense-knobs");
  const mk = (label, min, max, step, get, set, fmt) => {
    const row = el("label", "flash-sense-knob");
    row.append(el("span", "flash-sense-knob-name", label));
    const input = document.createElement("input");
    input.type = "range"; input.min = String(min); input.max = String(max); input.step = String(step);
    input.value = String(get());
    const val = el("span", "flash-sense-knob-val", fmt(get()));
    input.addEventListener("input", () => { set(Number(input.value)); val.textContent = fmt(Number(input.value)); render(); });
    row.append(input, val);
    sliders.append(row);
  };
  mk("mount height", 0.2, 3, 0.1, () => scene.mountHeight, (v) => { scene.mountHeight = v; }, (v) => v.toFixed(1) + " m");
  mk("tilt down", 0, 60, 5, () => scene.tiltDeg, (v) => { scene.tiltDeg = v; }, (v) => v + "°");
  mk("person at", 0.3, 7, 0.1, () => scene.person.x, (v) => { scene.person.x = v; }, (v) => v.toFixed(1) + " m");
  mk("off to the side", 0, 4, 0.1, () => scene.person.y, (v) => { scene.person.y = v; }, (v) => v.toFixed(1) + " m");
  lab.append(sliders);

  const postRow = el("div", "flash-row");
  const posture = document.createElement("select");
  for (const p of ["standing", "sitting", "lying"]) {
    const o = document.createElement("option");
    o.value = p; o.textContent = p;
    posture.append(o);
  }
  posture.addEventListener("change", () => { scene.person.posture = posture.value; render(); });
  const orient = document.createElement("select");
  for (const p of ["facing", "side", "back"]) {
    const o = document.createElement("option");
    o.value = p; o.textContent = "chest " + p;
    orient.append(o);
  }
  orient.addEventListener("change", () => { scene.person.orientation = orient.value; render(); });
  postRow.append(posture, orient);
  lab.append(postRow);

  const verdict = el("div", "rdev-place-verdict");
  lab.append(verdict);
  s.append(lab);

  function render() {
    if (mode === "vitals") { scene.mount = "wall"; }
    const adv = placementAdvice(scene, state.hw, mode);
    verdict.innerHTML = "";
    const grade = el("span", "rdev-grade rdev-grade-" + adv.grade, adv.grade);
    const head = el("div", "rdev-place-head");
    head.append(grade, el("strong", null, adv.headline));
    verdict.append(head);
    const facts = el("div", "wap-facts");
    const rr = el("div", "wap-fact");
    rr.append(el("span", "wap-fact-k", "radar → chest"), el("span", "wap-fact-v",
      adv.view.r.toFixed(2) + " m · " + adv.view.offBoresightDeg.toFixed(0) + "° off boresight" +
      (adv.quality != null ? ` · vitals quality ${(adv.quality * 100).toFixed(0)}%` : "")));
    facts.append(rr);
    verdict.append(facts);
    if (adv.fixes.length) {
      const ul = el("ul", "rdev-fixes");
      for (const f of adv.fixes) ul.append(el("li", null, f));
      verdict.append(ul);
    }
  }
  render();

  // The keep-out list + radome rule — the honest half of placement.
  const avoid = el("div", "rdev-avoid");
  avoid.append(el("h3", "wap-col-h", "Keep out of the beam"));
  const grid = el("div", "wap-mqtt-ents");
  for (const a of d.placement.avoid) {
    const row = el("div", "wap-ent");
    row.append(el("span", "wap-ent-comp", "✕"), el("span", "wap-ent-name", a.what),
      el("span", "wap-ent-topic", a.why));
    grid.append(row);
  }
  avoid.append(grid);
  const rd = d.placement.radome;
  avoid.append(el("p", "fineprint muted", "Radome rule: " + rd.rule + " " + rd.why + " " + rd.repo_note));
  s.append(avoid);
}

// ── keep going ──────────────────────────────────────────────────────────────

function buildKeepGoing(mount) {
  const s = section(mount, "more", "keep going", "Where this goes next", null);
  const grid = el("div", "hub-links");
  const items = [
    ["The Nursery", "flash a Sense over USB in the browser — this bench's front door", "flash.html", true],
    ["The Sense — radar school", "the guided story: cone, FSMs, MQTT, the privacy chokepoint", "sense.html", true],
    ["The Sense Lab", "the deep bench: wire protocol, placement physics, the power lab", "senselab.html", true],
    ["The Hub", "Home Assistant — where the tuned knobs surface as number entities", "homeassistant.html", true],
    ["The design doc", "the whole plan — privacy classes, scenarios, the fall-detection sibling", "docs/canary_sense_mr60bha2_design.md", false],
  ];
  for (const [title, body, path, local] of items) {
    const a = el("a", "hub-link");
    a.href = local ? path : GH + path;
    if (!local) { a.target = "_blank"; a.rel = "noopener noreferrer"; }
    a.append(el("strong", null, title), el("span", "muted", body), el("code", "fineprint", path));
    grid.append(a);
  }
  s.append(grid);
}

main();
