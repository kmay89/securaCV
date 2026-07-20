// canary-local/assets/sense-ui.js — the Canary Sense bench widgets.
//
// Staged surfaces for the canary-sense device (XIAO ESP32-C6 + MR60BHA2
// 60 GHz mmWave), all fed from the drift-gated devices/sense.json
// (tools/gen_sense.py parses the firmware):
//
//   · buildDevice    — the radome in 3D + the kit's real wiring facts + the
//                      WS2812 LED grammar (green/blue/amber straight from
//                      main.cpp's led_for_presence()).
//   · buildProvision — the USB provisioning walkthrough: secrets.h, the two
//                      build flavors, the NVS policy that makes identity
//                      survive OTA — and the Track B stock-ESPHome on-ramp.
//   · buildSerial    — the USB-CDC console: the real boot scenes + net
//                      bring-up log, then the live [presence]/[vitals] lines.
//   · buildRadarLab  — the placement lab: a top-down room with the real
//                      80° cone and the firmware's own range gates; drag
//                      people (and a cat, and a fan) through it and watch
//                      the ACTUAL presence FSM — same thresholds, mirrored
//                      in JS — debounce, clear, and suppress vitals.
//   · buildMqtt      — an MQTT-explorer view: retained topics fill on
//                      connect, signed events stream on transitions.
//   · buildEntities  — the HA discovery set, flavor-gated exactly like the
//                      firmware compiles it (BPM entities absent, not hidden).
//   · buildPlacement / buildTuning — the max-capability playbook, each claim
//                      labeled with where it comes from (repo / seeed / community).
//
// Everything is wired through a tiny shared bus (see sense.js) so one action
// ripples across all surfaces at once, the way it would on a real bench.
// Nothing is faked past what the firmware strings say; where a claim comes
// from outside the repo, its source badge says so to your face.

import { DeviceScene, BUILDERS } from "./scene3d.js";
import { upgradeRealShape } from "./real-shapes.js";
import { withId, mqttApply } from "./wap-ui.js";

const el = (tag, cls, text) => {
  const n = document.createElement(tag);
  if (cls) n.className = cls;
  if (text != null) n.textContent = text;
  return n;
};
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const alive = (node) => document.body.contains(node);

// ── DOM-free cores (exported; pinned in tests/sense.test.js) ──────────────

// Flatten the serial data into one ordered list of {cls,text} console lines:
// the banner scenes, the tagged net bring-up log, then the ready scene —
// the exact order main.cpp's setup() prints them.
export function bootLines(serial) {
  const out = [];
  for (const t of serial.banner || []) out.push({ cls: "wap-b", text: t });
  for (const s of serial.boot || []) {
    const kind = (serial.tags || {})[s.tag] || "prov";
    const cls = { done: "ok", wifi: "net", mqtt: "net", witness: "prov", heap: "faint" }[kind] || "prov";
    out.push({ cls: "wap-" + cls, text: `${s.tag} ${s.text}`.trim() });
  }
  for (const t of serial.ready || []) out.push({ cls: "wap-b", text: t });
  return out;
}

// The privacy chokepoint's range gate, mirrored: raw centimetres in, coarse
// band out. Same comparisons as mr60_presence.cpp's band_of().
export function rangeBandOf(cm, cfg) {
  if (!(cm > 0)) return "unknown";
  if (cm <= cfg.near_cm) return "near";
  if (cm <= cfg.mid_cm) return "mid";
  return "far";
}

// Bucketed occupant count — the 0/1/2+ vocabulary, never a track log.
export function countBucketOf(n) {
  return n <= 0 ? "0" : n === 1 ? "1" : "2+";
}

// A JS mirror of the firmware's PresenceFSM (mr60_presence): debounce into
// Present, timeout into Clear, stall into Unknown — deadline checks first,
// so a silent radar (frame == null) still drives the stall. Thresholds come
// straight from sense.json (the firmware's own CS_* constants).
export function makePresenceFSM(cfg) {
  let state = "unknown", lastFrame = 0, targetSince = 0, targetGone = 0, rawTarget = false;
  let count = "0", range = "unknown";
  return {
    get state() { return state; },
    get count() { return count; },
    get range() { return range; },
    reset(now) { state = "unknown"; lastFrame = now; rawTarget = false; count = "0"; range = "unknown"; },
    tick(frame, now) {
      const before = state, beforeCount = count;
      // deadline first: silence past the stall window is Unknown, always
      if (now - lastFrame >= cfg.stall_ms) { state = "unknown"; range = "unknown"; }
      if (frame) {
        lastFrame = now;
        if (frame.hasTarget) {
          if (!rawTarget) { rawTarget = true; targetSince = now; }
          if (state !== "present" && now - targetSince >= cfg.debounce_ms) state = "present";
          if (state === "unknown" && now - targetSince < cfg.debounce_ms) { /* stay until debounced */ }
          count = countBucketOf(frame.count);
          range = rangeBandOf(frame.distanceCm, cfg);
        } else {
          if (rawTarget) { rawTarget = false; targetGone = now; }
          if (state === "present") {
            if (now - targetGone >= cfg.clear_ms) { state = "clear"; count = "0"; range = "unknown"; }
          } else {
            state = "clear"; count = "0"; range = "unknown";
          }
        }
      }
      return {
        state, count, range,
        stateChanged: state !== before,
        countChanged: count !== beforeCount,
        stalled: state === "unknown" && before !== "unknown",
      };
    },
  };
}

// The vitals lock mirror (mr60_vitals): plausible vitals must sustain
// lock_ms with EXACTLY one target; anything else drains toward lost. The
// single-target rule is the firmware's hard suppress, not advice.
export function makeVitalsFSM(cfg) {
  let lock = "unknown", validSince = 0, lastValid = 0, wasValid = false;
  return {
    get lock() { return lock; },
    reset(now) { lock = "unknown"; wasValid = false; validSince = lastValid = now; },
    tick(plausible, singleTarget, now) {
      const before = lock;
      const valid = plausible && singleTarget;
      if (valid) {
        if (!wasValid) { wasValid = true; validSince = now; }
        lastValid = now;
        if (lock !== "locked" && now - validSince >= cfg.lock_ms) lock = "locked";
      } else {
        wasValid = false;
        if (lock === "locked" && now - lastValid >= cfg.lost_ms) lock = "lost";
        if (lock === "unknown" && now - lastValid >= cfg.lost_ms) lock = "lost";
      }
      return { lock, changed: lock !== before };
    },
  };
}

// Presence state → the WS2812 colour main.cpp shows for it.
export function ledFor(state, fsm) {
  const s = (fsm.presence.states || []).find((x) => x.name.toLowerCase() === state);
  return s ? { led: s.led, rgb: s.rgb } : { led: "amber", rgb: [24, 8, 0] };
}

const STATE_PILL = { present: "wap-pill-presence", clear: "wap-pill-quiet", unknown: "wap-pill-off" };
const LED_CSS = { green: "#3ddc84", blue: "#4f8cff", amber: "#e0a03c", white: "#f2f2f2" };

// ── source badges — where a claim comes from ──────────────────────────────
const SRC_LABEL = { repo: "repo · CI-gated", seeed: "Seeed wiki", esphome: "ESPHome docs", community: "community" };
function srcBadge(src) {
  return el("span", "sense-src sense-src-" + (src || "repo"), SRC_LABEL[src] || src);
}

// ── the device: radome in 3D + wiring facts + LED grammar ─────────────────
export function buildDevice(data, bus) {
  const wrap = el("div", "wap-plug sense-device");
  const stage = el("div", "wap-plug-stage");
  const cv = el("canvas", "wap-plug-3d");
  cv.setAttribute("aria-label", "Interactive 3D render of the Canary Sense radome");
  stage.append(cv, el("div", "asmlab-hint", "drag to orbit · pinch or scroll to zoom"));
  const scene = new DeviceScene(cv, null);
  BUILDERS["canary-sense"](scene);
  // the printed RADOME shells, if the preview meshes are present; the
  // stylised radome stays otherwise — same graceful path as the family cards
  upgradeRealShape(scene, "canary-sense");
  scene.start();

  const side = el("div", "wap-plug-side");
  side.append(el("h3", "wap-col-h", "the kit, wired the way it ships"));
  side.append(el("p", "muted",
    "The RADOME shell: the front over the antenna is a thin, flat membrane — 60 GHz " +
    "transparency demands it (the air gap is computed and asserted in the OpenSCAD model). " +
    "Inside, the radar talks to the host over one UART:"));
  const facts = el("div", "wap-facts");
  for (const [k, v] of [
    ["Radar", data.radar.module + " · " + data.radar.band],
    ["Brain", data.radar.soc],
    ["Host", data.radar.host],
    ["Link", data.radar.link],
    ["Extras", data.radar.peripherals.join("  ·  ")],
  ]) {
    const row = el("div", "wap-fact");
    row.append(el("span", "wap-fact-k", k), el("span", "wap-fact-v", v));
    facts.append(row);
  }
  side.append(facts);

  // LED grammar — the state colours from main.cpp, live on the bench
  const ledRow = el("div", "sense-leds");
  const dots = {};
  for (const s of data.fsm.presence.states) {
    const d = el("div", "sense-led");
    const dot = el("i");
    dot.style.background = LED_CSS[s.led];
    d.append(dot, el("strong", null, s.name), el("span", "muted fineprint", s.meaning));
    dots[s.name.toLowerCase()] = dot;
    ledRow.append(d);
  }
  side.append(el("h3", "wap-col-h", "the LED speaks presence"), ledRow);
  bus.on("state", ({ state }) => {
    for (const k in dots) dots[k].classList.toggle("on", k === state);
  });

  const priv = el("p", "ondevice wap-note");
  priv.append(el("strong", null, "The privacy story: "), document.createTextNode(data.radar.privacy));
  side.append(priv);

  wrap.append(stage, side);
  return wrap;
}

// ── provisioning: Track A (USB seeds NVS) + Track B (stock ESPHome) ───────
export function buildProvision(data, bus) {
  const p = data.provisioning;
  const wrap = el("div", "sense-prov");

  const two = el("div", "wap-two");
  const left = el("div", "wap-two-col");
  left.append(el("h3", "wap-col-h", "1 · fill in secrets.h (it never leaves your machine)"));
  const sec = el("div", "wap-facts sense-secrets");
  for (const f of p.secrets_fields) {
    const row = el("div", "wap-fact");
    row.append(el("span", "wap-fact-k", "#define " + f.macro), el("span", "wap-fact-v muted", f.hint));
    sec.append(row);
  }
  left.append(sec);

  left.append(el("h3", "wap-col-h", "2 · pick a flavor"));
  const flav = el("div", "sense-flavors");
  for (const f of p.envs) {
    const c = el("div", "card sense-flavor");
    c.append(el("strong", null, f.label), el("code", "fineprint", f.id), el("span", "muted", f.note));
    flav.append(c);
  }
  left.append(flav);

  const right = el("div", "wap-two-col");
  right.append(el("h3", "wap-col-h", "3 · flash once over USB-C"));
  const term = el("div", "wap-cmds");
  for (const s of p.steps) {
    term.append(el("div", "wap-cmd-note", "# " + s.title + " — " + s.detail));
    term.append(el("div", "wap-cmd", "$ " + s.cmd));
  }
  right.append(term);

  const nvs = el("div", "sense-nvs");
  nvs.append(el("h4", "wap-flash-h", "Why once is enough — the NVS policy"));
  const ul = el("ul", "wap-btn-gestures");
  for (const line of p.nvs_policy) ul.append(el("li", null, line));
  nvs.append(ul);
  right.append(nvs);

  two.append(left, right);
  wrap.append(two);  // p.intro is the section lede — sense.js renders it once

  // Track B — the honest on-ramp card
  const tb = el("details", "fix sense-trackb");
  tb.append(el("summary", null, p.trackb.title));
  tb.append(el("p", "muted", p.trackb.body));
  const paths = el("div", "sense-flavors");
  for (const path of p.trackb.paths) {
    const c = el("div", "card sense-flavor");
    c.append(el("strong", null, path.name), el("span", "muted", path.how));
    paths.append(c);
  }
  tb.append(paths);
  const honest = el("p", "ondevice wap-note");
  honest.append(el("strong", null, "Honesty rule: "), document.createTextNode(p.trackb.honesty));
  tb.append(honest);
  wrap.append(tb);
  return wrap;
}

// ── the serial console ────────────────────────────────────────────────────
export function buildSerial(data, bus) {
  const wrap = el("div", "wap-term");
  const bar = el("div", "hub-term-bar");
  const dots = el("span", "hub-term-dots");
  dots.append(el("i"), el("i"), el("i"));
  bar.append(dots, el("span", "hub-term-title", "USB-CDC · 115200 8N1"),
    el("span", "hub-term-sim", "real firmware strings · staged boot"));
  const scroll = el("div", "wap-term-scroll");
  const controls = el("div", "hub-term-controls");
  const btnPwr = el("button", "primary small", "⏻ power on");
  const btnSkip = el("button", "ghost small", "skip to ready");
  const hint = el("span", "muted fineprint",
    "boots from devices/sense.json — the firmware's own boot log; " + data.serial.port_note);
  controls.append(btnPwr, btnSkip, hint);
  wrap.append(bar, scroll, controls);

  const lines = bootLines(data.serial);
  let booting = false, booted = false;

  const put = (cls, text) => {
    const l = el("div", "wap-line " + cls);
    l.textContent = text;
    scroll.append(l);
    scroll.scrollTop = scroll.scrollHeight;
    return l;
  };

  async function boot(instant) {
    if (booting || booted) return;
    booting = true; btnPwr.disabled = true; btnSkip.disabled = instant;
    scroll.innerHTML = "";
    bus.emit("power");
    for (const ln of lines) {
      put(ln.cls, ln.text);
      if (/Connected IP=/.test(ln.text)) bus.emit("wifi");
      if (/\[MQTT\] Connected\./.test(ln.text)) bus.emit("mqtt");
      if (!instant) {
        if (!alive(scroll)) { booting = false; return; }
        await sleep(ln.text.trim() === "" ? 8 : 24);
      }
    }
    booted = true; booting = false;
    btnSkip.disabled = true;
    put("wap-faint", "");
    put("wap-faint", "(steady state now: [presence]/[vitals] transitions and a [health] line every 5 s — drive them from the lab and the sandbox)");
    bus.emit("ready");
  }

  // one live line per lab/sandbox signal — the exact strings main.cpp prints
  bus.on("serial", ({ text, kind }) => {
    if (!booted) return;
    put("wap-" + (kind || "ok"), text);
  });
  bus.on("mqtt", () => { if (booted) return; }); // phase handled in boot stream

  btnPwr.addEventListener("click", () => boot(false));
  btnSkip.addEventListener("click", () => { if (!booting) boot(true); else booted = true; });
  bus.on("flash", () => { if (!booted && !booting) boot(false); });
  return wrap;
}

// ── the placement lab — the real cone, the real FSM, your furniture ───────
export function buildRadarLab(data, bus) {
  const cfg = data.fsm.presence;
  const vcfg = data.fsm.vitals;
  const wrap = el("div", "sense-lab");

  const stage = el("div", "sense-lab-stage");
  const cv = el("canvas", "sense-lab-canvas");
  cv.setAttribute("aria-label", "Top-down radar placement lab: drag people, a cat and a fan through the real 80-degree cone");
  stage.append(cv, el("div", "asmlab-hint", "drag the people · toggle the troublemakers"));

  const side = el("div", "sense-lab-side");

  // live readout
  const pill = el("div", "wap-pill wap-pill-off", "Unknown");
  const ledDot = el("i", "sense-lab-led");
  const pillRow = el("div", "sense-lab-pillrow");
  pillRow.append(pill, ledDot);
  const facts = el("div", "wap-facts");
  const rows = {};
  for (const k of ["Occupants", "Range band", "Vitals", "Publishes"]) {
    const row = el("div", "wap-fact");
    const v = el("span", "wap-fact-v", "—");
    row.append(el("span", "wap-fact-k", k), v);
    rows[k] = v;
    facts.append(row);
  }
  const toggles = el("div", "sense-lab-toggles");
  const mk = (id, label, hint) => {
    const b = el("button", "chip sense-toggle");
    b.append(el("strong", null, label), el("span", "muted fineprint", hint));
    b.dataset.id = id;
    toggles.append(b);
    return b;
  };
  const tPerson2 = mk("p2", "＋ second person", "watch 1 → 2+ and vitals refuse");
  const tCat = mk("cat", "🐈 the cat", "60 GHz sees any moving mass — honest");
  const tFan = mk("fan", "🌀 a fan", "the #1 community false-positive");
  const tWell = mk("well", "🫁 wellbeing build", "arm the breathing/heart lock");
  const tStill = mk("still", "🧘 hold still", "vitals need a still, single target");

  side.append(el("h3", "wap-col-h", "what the firmware decides"), pillRow, facts,
    el("h3", "wap-col-h", "troublemakers"), toggles,
    el("p", "fineprint muted",
      `Every threshold here is the real one: ${cfg.debounce_ms} ms debounce, ${cfg.clear_ms} ms clear, ` +
      `${cfg.stall_ms} ms stall, bands at ${cfg.near_cm}/${cfg.mid_cm} cm — mirrored from ` +
      `configs/canary-sense/*/config.h and drift-gated in CI.`));

  wrap.append(stage, side);

  // ---- the simulated room --------------------------------------------------
  // World coords in metres, device at (0,0) facing +x, cone ±40°. The canvas
  // shows 0..6.5 m of x and −3..3 m of y.
  const FOV = (data.radar.fov_deg * Math.PI / 180) / 2;   // half-angle
  const MAXR = data.radar.presence_max_m;
  const VITR = data.radar.vitals_range_m;
  const world = {
    // person ① starts OUTSIDE the cone (dist > 6 m) — walking them in is the
    // first lesson, and it keeps the bench Clear until the visitor acts
    people: [{ x: 5.9, y: 1.6, on: true }, { x: 4.6, y: -1.4, on: false }],
    cat: { on: false, t: 0 },
    fan: { on: false, x: 2.2, y: -1.7 },
    still: false, wellbeing: false,
    drag: null, stalled: false,
  };
  const fsm = makePresenceFSM(cfg);
  const vfsm = makeVitalsFSM(vcfg);
  fsm.reset(performance.now());
  vfsm.reset(performance.now());

  const inCone = (x, y) => {
    const d = Math.hypot(x, y);
    return d <= MAXR && d >= 0.1 && Math.abs(Math.atan2(y, x)) <= FOV ? d : 0;
  };

  function radarFrame(now) {
    if (world.stalled) return null;
    const targets = [];
    for (const p of world.people) if (p.on) { const d = inCone(p.x, p.y); if (d) targets.push(d); }
    if (world.cat.on) {
      // the cat wanders an ellipse through the room; small radar cross-section
      // → an intermittent target (the flicker IS the phenomenon to learn)
      const cx = 3 + 2.2 * Math.cos(world.cat.t), cy = 1.8 * Math.sin(world.cat.t * 1.3);
      const d = inCone(cx, cy);
      if (d && Math.sin(now / 260) > -0.35) targets.push(d);
      world.cat.pos = [cx, cy];
    }
    if (world.fan.on) {
      const d = inCone(world.fan.x, world.fan.y);
      // a fan is sustained motion at a FIXED range — exactly why it fools radar
      if (d && Math.sin(now / 90) > -0.85) targets.push(d);
    }
    if (!targets.length) return { hasTarget: false, count: 0, distanceCm: 0 };
    return { hasTarget: true, count: targets.length, distanceCm: Math.min(...targets) * 100 };
  }

  let lastState = "unknown", lastCount = "0", lastPub = "—";
  function publish(event, extra) {
    lastPub = event;
    bus.emit("labevent", { event, ...extra });
  }

  function tick(now) {
    world.cat.t += 0.006;
    const frame = radarFrame(now);
    const ev = fsm.tick(frame, now);

    // vitals: exactly one person-scale target, close, still, wellbeing build
    const singlePerson = world.people.filter((p) => p.on && inCone(p.x, p.y)).length === 1
      && !(world.cat.on && world.cat.pos && inCone(...world.cat.pos));
    const near = world.people.some((p) => p.on && inCone(p.x, p.y) && Math.hypot(p.x, p.y) <= VITR);
    const plausible = world.wellbeing && world.still && near && fsm.state === "present";
    const vev = vfsm.tick(plausible, singlePerson && fsm.count === "1", now);

    if (ev.stateChanged) {
      const s = fsm.state;
      bus.emit("state", { state: s });
      bus.emit("serial", { text: `[presence] -> ${s}${ev.stalled ? " (radar stall)" : ""}`, kind: ev.stalled ? "warn" : "ok" });
      if (s === "present") publish("presence_detected", { presence: "present", occupants: fsm.count, range: fsm.range });
      else if (s === "clear" && lastState === "present") publish("presence_cleared", { presence: "clear", occupants: "0", range: "unknown" });
      lastState = s;
    }
    if (ev.countChanged && fsm.state === "present" && fsm.count !== lastCount) {
      publish("occupancy_changed", { presence: "present", occupants: fsm.count, range: fsm.range });
      lastCount = fsm.count;
    }
    if (vev.changed) {
      bus.emit("serial", { text: `[vitals] breathing ${vfsm.lock === "locked" ? "locked" : "lost"}`, kind: "ok" });
      bus.emit("labvitals", { locked: vfsm.lock === "locked" });
    }

    // readouts
    const stateName = fsm.state[0].toUpperCase() + fsm.state.slice(1);
    pill.className = "wap-pill " + (STATE_PILL[fsm.state] || "wap-pill-off");
    pill.textContent = stateName;
    const led = ledFor(fsm.state, data.fsm);
    ledDot.style.background = LED_CSS[led.led];
    rows["Occupants"].textContent = fsm.count + (fsm.count === "2+" ? "  (bucketed — never a track log)" : "");
    rows["Range band"].textContent = fsm.range + (fsm.range !== "unknown" ? "  (raw cm never publish)" : "");
    rows["Vitals"].textContent = !world.wellbeing ? "presence-only build — compiled out"
      : vfsm.lock === "locked" ? "breathing locked · 14 / 68 bpm (P1)"
        : (fsm.count === "2+" ? "suppressed — 2+ targets (code rule)" : "no lock (" + vfsm.lock + ")");
    rows["Publishes"].textContent = lastPub;

    draw(now);
    if (alive(cv)) requestAnimationFrame(tick);
  }

  // ---- rendering -----------------------------------------------------------
  const DPR = Math.min(2, window.devicePixelRatio || 1);
  function fit() {
    const w = cv.clientWidth || 620, h = Math.max(300, Math.round(w * 0.62));
    cv.width = w * DPR; cv.height = h * DPR;
  }
  const X = (m) => (cv.width * 0.08) + m * (cv.width * 0.86 / 6.5);
  const Y = (m) => cv.height / 2 + m * (cv.width * 0.86 / 6.5);
  const M = (px, py) => [((px * DPR) - cv.width * 0.08) / (cv.width * 0.86 / 6.5),
                         ((py * DPR) - cv.height / 2) / (cv.width * 0.86 / 6.5)];

  function draw(now) {
    const ctx = cv.getContext("2d");
    const w = cv.width, h = cv.height;
    ctx.clearRect(0, 0, w, h);

    // the cone + the firmware's range gates
    const bands = [
      [cfg.near_cm / 100, "rgba(120,220,160,.16)"],
      [cfg.mid_cm / 100, "rgba(120,190,255,.12)"],
      [MAXR, "rgba(255,255,255,.06)"],
    ];
    let r0 = 0;
    for (const [r, fill] of bands) {
      ctx.beginPath();
      ctx.moveTo(X(r0 * Math.cos(FOV)), Y(r0 * Math.sin(FOV)));
      ctx.arc(X(0), Y(0), (X(r) - X(0)), -FOV, FOV);
      ctx.arc(X(0), Y(0), (X(r0) - X(0)), FOV, -FOV, true);
      ctx.closePath();
      ctx.fillStyle = fill;
      ctx.fill();
      r0 = r;
    }
    // vitals ring
    ctx.beginPath();
    ctx.arc(X(0), Y(0), X(VITR) - X(0), -FOV, FOV);
    ctx.strokeStyle = "rgba(255,180,120,.55)";
    ctx.setLineDash([6 * DPR, 5 * DPR]);
    ctx.lineWidth = 1.4 * DPR;
    ctx.stroke();
    ctx.setLineDash([]);
    // labels
    ctx.fillStyle = "rgba(255,255,255,.5)";
    ctx.font = `${11 * DPR}px ui-monospace, monospace`;
    ctx.fillText("near", X(cfg.near_cm / 200) - 10 * DPR, Y(0) - 6 * DPR);
    ctx.fillText("mid", X((cfg.near_cm / 100 + cfg.mid_cm / 100) / 2) - 8 * DPR, Y(0) - 6 * DPR);
    ctx.fillText("far", X((cfg.mid_cm / 100 + MAXR) / 2) - 6 * DPR, Y(0) - 6 * DPR);
    ctx.fillStyle = "rgba(255,180,120,.8)";
    ctx.fillText("vitals ≤" + VITR + " m", X(VITR) - 34 * DPR, Y(-0.15) - 14 * DPR);

    // sweep, while the radar is alive
    if (!world.stalled) {
      const a = -FOV + ((now / 1800) % 1) * 2 * FOV;
      ctx.beginPath();
      ctx.moveTo(X(0), Y(0));
      ctx.lineTo(X(MAXR * Math.cos(a)), Y(MAXR * Math.sin(a)));
      ctx.strokeStyle = "rgba(120,220,160,.25)";
      ctx.lineWidth = 1.2 * DPR;
      ctx.stroke();
    }

    // the device on the wall
    ctx.fillStyle = world.stalled ? "#7a5c2e" : "#e8c15a";
    ctx.beginPath();
    ctx.arc(X(0), Y(0), 7 * DPR, 0, Math.PI * 2);
    ctx.fill();
    ctx.fillStyle = "rgba(255,255,255,.55)";
    ctx.fillText("Canary Sense", X(0) - 14 * DPR, Y(0) + 22 * DPR);

    // people
    for (const [i, p] of world.people.entries()) {
      if (!p.on) continue;
      const d = inCone(p.x, p.y);
      ctx.beginPath();
      ctx.arc(X(p.x), Y(p.y), 9 * DPR, 0, Math.PI * 2);
      ctx.fillStyle = d ? "rgba(120,220,160,.9)" : "rgba(255,255,255,.35)";
      ctx.fill();
      ctx.fillStyle = "#10131a";
      ctx.font = `${10 * DPR}px system-ui`;
      ctx.fillText(i === 0 ? "①" : "②", X(p.x) - 5 * DPR, Y(p.y) + 3.5 * DPR);
    }
    // cat
    if (world.cat.on && world.cat.pos) {
      const [cx, cy2] = world.cat.pos;
      ctx.font = `${13 * DPR}px system-ui`;
      ctx.fillText("🐈", X(cx) - 7 * DPR, Y(cy2) + 5 * DPR);
    }
    // fan
    if (world.fan.on) {
      ctx.font = `${13 * DPR}px system-ui`;
      ctx.save();
      ctx.translate(X(world.fan.x), Y(world.fan.y));
      ctx.rotate((now / 300) % (Math.PI * 2));
      ctx.fillText("🌀", -7 * DPR, 5 * DPR);
      ctx.restore();
    }
  }

  // ---- interactions --------------------------------------------------------
  cv.addEventListener("pointerdown", (e) => {
    const rect = cv.getBoundingClientRect();
    const [mx, my] = M(e.clientX - rect.left, e.clientY - rect.top);
    let best = null, bd = 0.45;
    for (const [i, p] of world.people.entries()) {
      if (!p.on) continue;
      const d = Math.hypot(p.x - mx, p.y - my);
      if (d < bd) { bd = d; best = i; }
    }
    if (world.fan.on && Math.hypot(world.fan.x - mx, world.fan.y - my) < bd) best = "fan";
    if (best != null) { world.drag = best; cv.setPointerCapture(e.pointerId); }
  });
  cv.addEventListener("pointermove", (e) => {
    if (world.drag == null) return;
    const rect = cv.getBoundingClientRect();
    const [mx, my] = M(e.clientX - rect.left, e.clientY - rect.top);
    const t = world.drag === "fan" ? world.fan : world.people[world.drag];
    t.x = Math.max(0.15, Math.min(6.4, mx));
    t.y = Math.max(-2.25, Math.min(2.25, my));
    world.still = false; tStill.classList.remove("on");
  });
  const drop = () => { world.drag = null; };
  cv.addEventListener("pointerup", drop);
  cv.addEventListener("pointercancel", drop);

  const bindToggle = (btn, fn) => btn.addEventListener("click", () => {
    btn.classList.toggle("on");
    fn(btn.classList.contains("on"));
  });
  bindToggle(tPerson2, (on) => { world.people[1].on = on; });
  bindToggle(tCat, (on) => { world.cat.on = on; });
  bindToggle(tFan, (on) => { world.fan.on = on; });
  bindToggle(tWell, (on) => { world.wellbeing = on; });
  bindToggle(tStill, (on) => { world.still = on; });

  // the sandbox can drive the lab too (stall, identify)
  bus.on("labcmd", ({ cmd }) => {
    if (cmd === "stall") { world.stalled = true; setTimeout(() => { world.stalled = false; }, 8000); }
    if (cmd === "still") { world.still = true; world.wellbeing = true; tStill.classList.add("on"); tWell.classList.add("on"); }
    if (cmd === "person2") { world.people[1].on = true; tPerson2.classList.add("on"); }
    if (cmd === "clearall") {
      world.people[0].x = 5.9; world.people[0].y = 1.6;
      world.people[1].on = false; tPerson2.classList.remove("on");
      world.cat.on = false; tCat.classList.remove("on");
      world.fan.on = false; tFan.classList.remove("on");
    }
    if (cmd === "walkin") { world.people[0].x = 3.0; world.people[0].y = 0.4; }
    if (cmd === "near") { world.people[0].x = 1.1; world.people[0].y = 0.1; }
  });

  fit();
  window.addEventListener("resize", fit);
  requestAnimationFrame(tick);
  return wrap;
}

// ── the MQTT explorer ─────────────────────────────────────────────────────
export function buildMqtt(data, bus) {
  const m = data.mqtt, id = data.device.id_example;
  const wrap = el("div", "wap-mqtt");
  const bar = el("div", "hub-term-bar");
  bar.append(el("span", "hub-term-title", "MQTT · " + m.broker_uri),
    el("span", "hub-term-sim", "topics + payloads from mqtt_mgr.cpp"));
  wrap.append(bar);

  const cols = el("div", "wap-mqtt-cols");
  const retainedCol = el("div", "wap-mqtt-col");
  retainedCol.append(el("div", "wap-mqtt-h", "retained topics"));
  const retained = el("div", "wap-mqtt-tree");
  retainedCol.append(retained);
  const streamCol = el("div", "wap-mqtt-col");
  streamCol.append(el("div", "wap-mqtt-h", "live — " + withId(m.topic_pattern.replace("<suffix>", "events"), id)));
  const stream = el("div", "wap-mqtt-stream");
  streamCol.append(stream);
  cols.append(retainedCol, streamCol);
  wrap.append(cols);

  const note = el("p", "ondevice wap-note");
  note.append(el("strong", null, "How to read this: "), document.createTextNode(
    "the topic tree and payloads are the exact strings mqtt_mgr.cpp publishes; only events and the " +
    "identify echo are non-retained. " + m.offline_note + "."));
  wrap.append(note);

  const store = {};
  function renderRetained() {
    retained.innerHTML = "";
    const keys = Object.keys(store).sort();
    if (!keys.length) { retained.append(el("p", "muted fineprint", "nothing retained yet — the device isn't connected.")); return; }
    for (const k of keys) {
      const row = el("div", "wap-mqtt-row");
      row.append(el("code", "wap-mqtt-topic", k), el("code", "wap-mqtt-payload", store[k]));
      retained.append(row);
    }
  }
  function pushStream(topic, payload, cls) {
    const row = el("div", "wap-mqtt-ev " + (cls || ""));
    row.append(el("code", "wap-mqtt-topic", topic), el("code", "wap-mqtt-payload", payload));
    stream.prepend(row);
    while (stream.children.length > 7) stream.lastChild.remove();
  }
  renderRetained();

  let chainLen = 313;
  bus.on("mqtt", () => {
    const seq = [];
    for (const t of m.topics) {
      if (!t.retained) continue;
      seq.push({ topic: withId(m.topic_pattern.replace("<suffix>", t.suffix), id), payload: t.payload, retain: true });
    }
    (async () => {
      for (const msg of seq) {
        await sleep(150); if (!alive(wrap)) return;
        mqttApply(store, msg); renderRetained();
      }
      pushStream(m.discovery.prefix + "/…/config", m.discovery.counts.default + " announced (retained)", "disc");
    })();
  });
  bus.on("labevent", (e) => {
    chainLen += 1;
    const evTopic = withId(m.topic_pattern.replace("<suffix>", "events"), id);
    pushStream(evTopic,
      `{"event":"${e.event}","presence":"${e.presence}","occupants":"${e.occupants}","range":"${e.range}","seq":${chainLen},"signed":true,"fp":"${data.device.fp_example}"}`,
      "live");
    const chTopic = withId(m.topic_pattern.replace("<suffix>", "chain"), id);
    mqttApply(store, { topic: chTopic, payload: `{"v":1,"length":${chainLen},"latest_hash":"…","alg":"ed25519","sig":"…"}`, retain: true });
    renderRetained();
  });
  bus.on("sandboxpub", ({ pubs }) => {
    for (const pub of pubs || []) {
      const topic = withId(m.topic_pattern.replace("<suffix>", pub.suffix), id);
      const retain = pub.suffix !== "events" && pub.suffix !== "identify";
      if (retain) { mqttApply(store, { topic, payload: pub.payload, retain: true }); renderRetained(); }
      pushStream(topic, pub.payload, retain ? "" : "live");
    }
  });
  return wrap;
}

// ── the HA discovery set (flavor-gated, like the firmware compiles it) ────
export function buildEntities(data) {
  const m = data.mqtt;
  const wrap = el("div", "sense-ents");
  const tabs = el("nav", "subtabs");
  const grid = el("div", "wap-mqtt-ents");
  const note = el("p", "fineprint muted");

  const FLAVORS = [
    ["default", "presence-only", (e) => e.flavor === "default"],
    ["wellbeing", "wellbeing (+P1 opt-in)", () => true],
  ];
  function render(which) {
    const [, , filter] = FLAVORS.find((f) => f[0] === which);
    grid.innerHTML = "";
    let n = 0;
    for (const e of m.discovery.entities) {
      if (!filter(e)) continue;
      n++;
      const row = el("div", "wap-ent" + (e.flavor !== "default" ? " sense-ent-well" : ""));
      row.append(el("span", "wap-ent-comp", e.component.replace("_", " ")),
        el("span", "wap-ent-name", e.name + (e.unit ? " (" + e.unit + ")" : "")),
        el("code", "wap-ent-topic", "…/" + e.state_topic + (e.note ? "  — " + e.note : "")));
      grid.append(row);
    }
    note.textContent = which === "default"
      ? n + " entities. " + m.discovery.gating_note + "."
      : n + " entities. " + m.discovery.counts.wellbeing + ". Vitals entities are highlighted.";
  }
  for (const [id, label] of FLAVORS) {
    const b = el("button", "tab" + (id === "default" ? " on" : ""), label);
    b.addEventListener("click", () => {
      for (const x of tabs.children) x.classList.remove("on");
      b.classList.add("on");
      render(id);
    });
    tabs.append(b);
  }
  render("default");
  wrap.append(tabs, grid, note,
    el("p", "muted fineprint", m.discovery.note + " — config topics on " + m.discovery.config_topic));
  return wrap;
}

// ── placement — mounts, the radome rule, what to keep out of the beam ─────
export function buildPlacement(data) {
  const p = data.placement;
  const wrap = el("div", "sense-place");

  const mounts = el("div", "sense-mounts");
  for (const mnt of p.mounts) {
    const c = el("div", "card sense-mount");
    const h = el("strong", null, mnt.name);
    c.append(h, srcBadge(mnt.src));
    const facts = el("div", "wap-facts");
    for (const [k, v] of [["Height", mnt.height], ["Aim", mnt.aim], ["Range", mnt.range], ["Best for", mnt.best_for]]) {
      const row = el("div", "wap-fact");
      row.append(el("span", "wap-fact-k", k), el("span", "wap-fact-v", v));
      facts.append(row);
    }
    c.append(facts);
    mounts.append(c);
  }
  wrap.append(mounts);

  const radome = el("p", "ondevice wap-note");
  radome.append(el("strong", null, "The radome rule: "),
    document.createTextNode(p.radome.rule + " — " + p.radome.why + ". " + p.radome.repo_note));
  wrap.append(radome);

  wrap.append(el("h3", "wap-col-h", "keep out of the beam"));
  const avoid = el("div", "sense-avoid");
  for (const a of p.avoid) {
    const d = el("details", "fix");
    const s = el("summary");
    s.append(document.createTextNode(a.what + " "), srcBadge(a.src));
    d.append(s, el("p", "muted", a.why));
    avoid.append(d);
  }
  wrap.append(avoid);
  return wrap;
}

// ── tuning — the knobs, the error taxonomy, the playbook ──────────────────
export function buildTuning(data) {
  const t = data.tuning;
  const wrap = el("div", "sense-tune");
  wrap.append(el("p", "hub-lede", t.goal));

  wrap.append(el("h3", "wap-col-h", "the knobs (all host-side, all versioned)"));
  const knobs = el("div", "sense-knobs");
  for (const k of t.knobs) {
    const c = el("div", "card sense-knob");
    const top = el("div", "sense-knob-top");
    top.append(el("code", null, k.name), el("strong", "sense-knob-val", String(k.value)), srcBadge(k.src));
    c.append(top, el("p", "muted", k.does));
    if (k.raise_when !== "—") c.append(kv("raise it when", k.raise_when));
    if (k.lower_when !== "—") c.append(kv("lower it when", k.lower_when));
    knobs.append(c);
  }
  wrap.append(knobs);

  wrap.append(el("h3", "wap-col-h", "the error taxonomy — least error means knowing your enemy"));
  const errs = el("div", "sense-errs");
  for (const e2 of t.errors) {
    const d = el("details", "fix sense-err sense-err-" + e2.kind.replace("-", ""));
    const s = el("summary");
    s.append(el("span", "sense-err-kind", e2.kind), document.createTextNode(" " + e2.cause + " "), srcBadge(e2.src));
    d.append(s, el("p", "muted", e2.reality), kv("the fix", e2.fix));
    errs.append(d);
  }
  wrap.append(errs);

  wrap.append(el("h3", "wap-col-h", "the placement-first playbook"));
  const ol = el("ol", "wap-ritual");
  for (const step of t.playbook) ol.append(el("li", null, step));
  wrap.append(ol, el("p", "fineprint muted", t.sources_note));
  return wrap;

  function kv(k, v) {
    const p2 = el("p", "fineprint sense-kv");
    p2.append(el("strong", null, k + ": "), document.createTextNode(v));
    return p2;
  }
}

// ── use cases + the capability table ──────────────────────────────────────
export function buildUseCases(data) {
  const wrap = el("div", "sense-uses");
  const grid = el("div", "sense-mounts");
  for (const u of data.use_cases) {
    const c = el("div", "card sense-mount");
    c.append(el("strong", null, u.title), el("span", "muted fineprint", u.where));
    c.append(el("p", "muted", u.how));
    const why = el("p", "fineprint sense-kv");
    why.append(el("strong", null, "why radar: "), document.createTextNode(u.why));
    c.append(why);
    grid.append(c);
  }
  wrap.append(grid);

  const tbl = el("div", "sense-captable");
  const head = el("div", "sense-caprow sense-caphead");
  for (const h of ["capability", "camera (vision)", "WiFi-CSI (WAP)", "60 GHz radar (Sense)"]) head.append(el("span", null, h));
  tbl.append(head);
  for (const r of data.capabilities) {
    const row = el("div", "sense-caprow");
    row.append(el("span", null, r.cap), el("span", "muted", r.vision), el("span", "muted", r.wap),
      el("strong", null, r.sense));
    tbl.append(row);
  }
  wrap.append(el("h3", "wap-col-h", "three physics, one family"), tbl,
    el("p", "fineprint muted", "from the design doc's own comparison — CSI and radar fail independently, which is exactly the multi-witness corroboration model."));
  return wrap;
}

// ── the sandbox cards ─────────────────────────────────────────────────────
export function buildSandbox(data, bus) {
  const wrap = el("div");
  const pad = el("div", "wap-sandbox");
  const readout = el("p", "muted wap-sandbox-read",
    "Power the device on in the console above, or just tap a card — the bench will bring it online for you.");

  const LABCMD = { walk: "walkin", approach: "near", sit: "still", second: "person2", leave: "clearall", stall: "stall" };

  for (const sc of data.sandbox) {
    const card = el("button", "card wap-sand-card");
    card.append(el("strong", null, sc.label), el("span", "muted", sc.blurb));
    if (sc.ha) card.append(el("code", "fineprint wap-sand-ha", sc.ha));
    card.addEventListener("click", () => {
      if (!bus.has("mqtt")) { bus.emit("power"); bus.emit("wifi"); bus.emit("mqtt"); bus.emit("ready"); }
      if (LABCMD[sc.id]) bus.emit("labcmd", { cmd: LABCMD[sc.id] });
      if (sc.serial && !LABCMD[sc.id]) bus.emit("serial", { text: sc.serial, kind: "ok" });
      if (sc.id === "identify" || sc.id === "lights") bus.emit("sandboxpub", { pubs: sc.mqtt });
      if (sc.state) bus.emit("state", { state: sc.state.toLowerCase() });
      readout.textContent = "▶ " + sc.label + " — " + sc.blurb + (sc.ha ? "  →  " + sc.ha : "");
    });
    pad.append(card);
  }
  wrap.append(pad, readout);
  return wrap;
}
