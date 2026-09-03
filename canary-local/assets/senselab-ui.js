// canary-local/assets/senselab-ui.js — the Sense Lab's surfaces.
//
// DOM widget builders for senselab.html, all driven by devices/senselab.json (the
// generated, drift-gated data) + the DOM-free cores in sense-sim.js. Layout:
//
//   buildProtocol   the wire — frame table, [BENCH] assumptions, checksum
//   buildStage      the placement bench — top-down + side view, drag the
//                   person, mount/height/tilt, live FoV + vitals quality
//   buildConsole    the pipeline — UART hex → decoded frames → FSM states →
//                   privacy chokepoint → signed witness events
//   buildKnobs      FSM tunables (the CS_* values as live sliders) + flavor
//   buildPowerLab   heat → useful computation: rails, duty cycles, claims/J
//   buildGlassBridge  stage this witness on the real display firmware (wasm)
//
// Everything numeric comes from sense.json; nothing is hardcoded here.
// The DOM-free scene/quality/power math lives in sense-sim.js (tested).

import {
  radarView, vitalsQuality, powerModel, TYPE_NAMES,
} from "./sense-sim.js";

const $ = (sel, root = document) => root.querySelector(sel);
const el = (tag, cls, text) => {
  const n = document.createElement(tag);
  if (cls) n.className = cls;
  if (text != null) n.textContent = text;
  return n;
};
const svgel = (tag, attrs = {}) => {
  const n = document.createElementNS("http://www.w3.org/2000/svg", tag);
  // `style` goes through the CSSOM: the page's CSP has no 'unsafe-inline' for
  // styles, and setAttribute("style") is exactly what that blocks.
  for (const [k, v] of Object.entries(attrs)) k === "style" ? (n.style.cssText = v) : n.setAttribute(k, v);
  return n;
};
const fmt = (x, d = 1) => (Math.round(x * 10 ** d) / 10 ** d).toFixed(d);

// ============================================================================
// §protocol — the wire, said out loud
// ============================================================================

export function buildProtocol(data) {
  const wrap = el("div", "slab-proto");

  const facts = el("div", "wap-facts");
  for (const [k, v] of [
    ["Link", `UART${data.pins.uart_num} · TX GPIO${data.pins.radar_tx} → radar · RX GPIO${data.pins.radar_rx} ← radar · ${data.pins.baud} 8N1`],
    ["Frame", `SOF 0x01 · id/len/type big-endian · payload little-endian · XOR-fold ~invert checksums ×2`],
    ["Radar", `${data.hardware.soc} — the IQ data never crosses this link; scalars only`],
  ]) {
    const row = el("div", "wap-fact");
    row.append(el("span", "wap-fact-k", k), el("span", "wap-fact-v", v));
    facts.append(row);
  }
  wrap.append(facts);

  const tbl = el("table", "slab-frames pin-table");
  const thead = el("thead");
  const hr = el("tr");
  for (const h of ["type id", "frame", "payload"]) hr.append(el("th", null, h));
  thead.append(hr);
  tbl.append(thead);
  const tb = el("tbody");
  for (const f of data.protocol.frames) {
    const tr = el("tr");
    tr.append(el("td", "slab-mono", f.type_hex));
    tr.append(el("td", null, f.name));
    tr.append(el("td", "muted", f.payload));
    tb.append(tr);
  }
  tbl.append(tb);
  wrap.append(tbl);

  const bench = el("div", "slab-bench");
  bench.append(el("h4", null, "The [BENCH] list — assumptions this lab lets you rehearse"));
  const ul = el("ul");
  for (const b of data.protocol.bench) ul.append(el("li", null, b));
  bench.append(ul);
  bench.append(el("p", "fineprint",
    "Quoted verbatim from firmware/common/sensors/mmwave_mr60/mr60_uart.h — the open " +
    "hardware-bench items. The stream below is built on exactly these assumptions, " +
    "so when the bench proves one wrong, this page and the firmware change together."));
  wrap.append(bench);
  return wrap;
}

// ============================================================================
// §stage — the placement bench
// ============================================================================
//
// scene state is owned by the page driver (sense.js); this builds the two
// views + controls and calls onChange() whenever the user moves something.

const STAGE = { W: 460, H: 380, SCALE: 52, MX: 30 }; // px per meter, margins

export function buildStage(data, scene, onChange) {
  const hw = data.hardware;
  const wrap = el("div", "slab-stage");

  // ---- controls ----
  const ctl = el("div", "slab-stage-ctl");

  const mountRow = el("div", "slab-ctl-row");
  mountRow.append(el("span", "slab-ctl-k", "mount"));
  for (const m of data.placement.mounts) {
    const b = el("button", "ghost small" + (scene.mount === m.id ? " on" : ""), m.label);
    b.dataset.mount = m.id;
    b.addEventListener("click", () => {
      scene.mount = m.id;
      scene.mountHeight = m.height_m;
      scene.tiltDeg = m.tilt_deg;
      if (m.person) Object.assign(scene.person, m.person);
      onChange();
    });
    mountRow.append(b);
  }
  ctl.append(mountRow);

  const sliders = [
    ["height", "mount height", 0.3, 3.0, 0.05, "m",
      () => scene.mountHeight, (v) => { scene.mountHeight = v; }],
    ["tilt", "tilt (down)", 0, 60, 1, "°",
      () => scene.tiltDeg, (v) => { scene.tiltDeg = v; }],
  ];
  for (const [id, label, min, max, step, unit, get, set] of sliders) {
    const row = el("div", "slab-ctl-row");
    row.append(el("span", "slab-ctl-k", label));
    const s = el("input");
    s.type = "range"; s.min = min; s.max = max; s.step = step; s.value = get();
    s.id = "slab-stage-" + id;
    const out = el("span", "slab-ctl-v", fmt(get(), step < 1 ? 2 : 0) + " " + unit);
    s.addEventListener("input", () => {
      set(parseFloat(s.value));
      out.textContent = fmt(get(), step < 1 ? 2 : 0) + " " + unit;
      onChange();
    });
    row.append(s, out);
    ctl.append(row);
  }

  const person = el("div", "slab-ctl-row");
  person.append(el("span", "slab-ctl-k", "person"));
  for (const [key, opts] of [
    ["posture", ["standing", "sitting", "lying"]],
    ["orientation", ["facing", "side", "back"]],
  ]) {
    const sel = el("select", "slab-sel");
    for (const o of opts) {
      const op = el("option", null, o);
      op.value = o;
      if (scene.person[key] === o) op.selected = true;
      sel.append(op);
    }
    sel.dataset.pkey = key;
    sel.addEventListener("change", () => { scene.person[key] = sel.value; onChange(); });
    person.append(sel);
  }
  ctl.append(person);

  const toggles = el("div", "slab-ctl-row slab-toggles");
  for (const [key, label] of [
    ["moving", "moving"], ["secondPerson", "second person"], ["fan", "fan in room"],
  ]) {
    const b = el("button", "ghost small" + (scene[key] || scene.person[key] ? " on" : ""), label);
    b.dataset.toggle = key;
    b.addEventListener("click", () => {
      if (key === "moving") scene.person.moving = !scene.person.moving;
      else scene[key] = !scene[key];
      b.classList.toggle("on");
      onChange();
    });
    toggles.append(b);
  }
  ctl.append(toggles);

  // ---- the two views ----
  const views = el("div", "slab-views");
  const top = svgel("svg", { viewBox: `0 0 ${STAGE.W} ${STAGE.H}`, class: "slab-view", "aria-label": "top-down view" });
  const side = svgel("svg", { viewBox: `0 0 ${STAGE.W} ${STAGE.H}`, class: "slab-view", "aria-label": "side view" });
  const topBox = el("div", "slab-view-box"); topBox.append(el("div", "slab-view-h", "from above"), top);
  const sideBox = el("div", "slab-view-box"); sideBox.append(el("div", "slab-view-h", "from the side"), side);
  views.append(topBox, sideBox);

  // ---- quality meter ----
  const meter = el("div", "slab-quality");
  const meterBar = el("div", "slab-qbar");
  const meterFill = el("div", "slab-qfill");
  meterBar.append(meterFill);
  const meterLabel = el("div", "slab-qlabel");
  const meterWhy = el("ul", "slab-qwhy");
  meter.append(el("h4", null, "Vitals signal quality at this placement"), meterBar, meterLabel, meterWhy);

  wrap.append(ctl, views, meter);

  // ---- drag the person (top view: x/y; side view: x only) ----
  function attachDrag(svg, mapPoint) {
    let dragging = false;
    const toScene = (evt) => {
      const r = svg.getBoundingClientRect();
      const px = ((evt.clientX - r.left) / r.width) * STAGE.W;
      const py = ((evt.clientY - r.top) / r.height) * STAGE.H;
      return mapPoint(px, py);
    };
    svg.addEventListener("pointerdown", (e) => { dragging = true; svg.setPointerCapture(e.pointerId); toScene(e); onChange(); });
    svg.addEventListener("pointermove", (e) => { if (dragging) { toScene(e); onChange(); } });
    svg.addEventListener("pointerup", () => { dragging = false; });
  }
  attachDrag(top, (px, py) => {
    scene.person.x = Math.max(0.15, (px - STAGE.MX) / STAGE.SCALE);
    scene.person.y = (py - STAGE.H / 2) / STAGE.SCALE;
  });
  attachDrag(side, (px) => {
    scene.person.x = Math.max(0.15, (px - STAGE.MX) / STAGE.SCALE);
  });

  // ---- redraw ----
  function draw() {
    const view = radarView(scene, hw);
    const vq = vitalsQuality(scene, hw);
    drawTop(top, scene, hw, view);
    drawSide(side, scene, hw, view);

    const q = vq.quality;
    meterFill.style.width = (q * 100).toFixed(0) + "%";
    meterFill.className = "slab-qfill " + (q > 0.65 ? "q-ok" : q > 0.3 ? "q-warn" : "q-bad");
    meterLabel.textContent = view.detected
      ? `radial ${fmt(view.r, 2)} m · ${fmt(view.offBoresightDeg, 0)}° off boresight · quality ${(q * 100).toFixed(0)} %`
      : "not in the detection sector";
    meterWhy.innerHTML = "";
    for (const rr of vq.reasons) meterWhy.append(el("li", null, rr));
    if (!vq.reasons.length && q > 0.65) meterWhy.append(el("li", "slab-q-good", "good geometry — this is a bench-grade vitals placement"));
    // sync sliders that presets may have moved
    $("#slab-stage-height", ctl).value = scene.mountHeight;
    $("#slab-stage-tilt", ctl).value = scene.tiltDeg;
  }

  wrap.redraw = draw;
  draw();
  return wrap;
}

function drawTop(svg, scene, hw, view) {
  svg.innerHTML = "";
  const ox = STAGE.MX, oy = STAGE.H / 2, S = STAGE.SCALE;
  // floor grid, 1 m
  for (let m = 0; m <= 8; m++) {
    svg.append(svgel("line", { x1: ox + m * S, y1: 8, x2: ox + m * S, y2: STAGE.H - 8, class: "slab-grid" }));
    if (m && m <= 6 && m % 2 === 0) {
      const t = svgel("text", { x: ox + m * S, y: STAGE.H - 12, class: "slab-grid-t" });
      t.textContent = m + " m";
      svg.append(t);
    }
  }
  const half = (hw.fov_deg / 2) * (Math.PI / 180);
  // detection sector (azimuth cut) — ceiling mount shows footprint circle
  if (scene.mount === "ceiling") {
    const foot = Math.tan(half) * scene.mountHeight;
    svg.append(svgel("circle", { cx: ox + scene.tiltDeg * 0, cy: oy, r: Math.min(foot * S, 300), class: "slab-fov", transform: `translate(${Math.sin(scene.tiltDeg * Math.PI / 180) * scene.mountHeight * S},0)` }));
  } else {
    const r = hw.presence_max_m * S;
    const x1 = ox + Math.cos(-half) * r, y1 = oy + Math.sin(-half) * r;
    const x2 = ox + Math.cos(half) * r, y2 = oy + Math.sin(half) * r;
    svg.append(svgel("path", { d: `M ${ox} ${oy} L ${x1} ${y1} A ${r} ${r} 0 0 1 ${x2} ${y2} Z`, class: "slab-fov" }));
    // range bands (near/mid) + vitals envelope
    for (const [cm, cls] of [[scene.cfg.near_cm, "slab-band-near"], [scene.cfg.mid_cm, "slab-band-mid"]]) {
      const rr = (cm / 100) * S;
      svg.append(svgel("path", { d: arcPath(ox, oy, rr, -half, half), class: "slab-band " + cls }));
    }
    svg.append(svgel("path", { d: arcPath(ox, oy, hw.vitals_max_m * S, -half, half), class: "slab-vitals-ring" }));
  }
  // radar mark
  svg.append(svgel("rect", { x: ox - 7, y: oy - 10, width: 8, height: 20, rx: 3, class: "slab-radar" }));
  // person
  const px = ox + scene.person.x * S, py = oy + scene.person.y * S;
  const g = svgel("g", { class: "slab-person" + (view.detected ? " on" : "") });
  g.append(svgel("circle", { cx: px, cy: py, r: 9 }));
  if (view.detected && !scene.person.moving) {
    for (let i = 1; i <= 2; i++) {
      g.append(svgel("circle", { cx: px, cy: py, r: 9 + i * 7, class: "slab-breath-ring", style: `animation-delay:${i * 0.6}s` }));
    }
  }
  svg.append(g);
  if (scene.secondPerson) {
    svg.append(svgel("circle", { cx: px + 0.7 * S, cy: py + 0.5 * S, r: 9, class: "slab-person2" }));
  }
  if (scene.fan) {
    const fx = ox + 2.2 * S, fy = oy - 1.6 * S;
    const fan = svgel("g", { class: "slab-fan" });
    fan.append(svgel("circle", { cx: fx, cy: fy, r: 8 }));
    fan.append(svgel("line", { x1: fx - 10, y1: fy, x2: fx + 10, y2: fy, class: "slab-fan-blade" }));
    svg.append(fan);
  }
}

function drawSide(svg, scene, hw, view) {
  svg.innerHTML = "";
  const ox = STAGE.MX, S = STAGE.SCALE;
  const floorY = STAGE.H - 40;
  const zy = (z) => floorY - z * S; // meters above floor → px
  svg.append(svgel("line", { x1: 8, y1: floorY, x2: STAGE.W - 8, y2: floorY, class: "slab-floor" }));
  // wall or ceiling line
  if (scene.mount === "ceiling") {
    svg.append(svgel("line", { x1: 8, y1: zy(scene.mountHeight), x2: STAGE.W - 8, y2: zy(scene.mountHeight), class: "slab-grid" }));
  } else {
    svg.append(svgel("line", { x1: ox - 10, y1: 10, x2: ox - 10, y2: floorY, class: "slab-grid" }));
  }
  const half = (hw.fov_deg / 2) * (Math.PI / 180);
  const tilt = (scene.tiltDeg * Math.PI) / 180;
  const rz = zy(scene.mountHeight);
  // beam edges (elevation cut around the tilted boresight)
  const boreAng = scene.mount === "ceiling" ? Math.PI / 2 - tilt : tilt; // below horizontal
  const rmax = hw.presence_max_m * S;
  const edge = (a) => {
    const dx = Math.cos(a) * rmax, dz = Math.sin(a) * rmax;
    return [ox + (scene.mount === "ceiling" ? Math.sin(tilt) * rmax : dx), rz + (scene.mount === "ceiling" ? Math.cos(a - Math.PI / 2 + tilt) * rmax : dz)];
  };
  if (scene.mount === "ceiling") {
    const spread = Math.tan(half) * scene.mountHeight * S;
    const cx = ox + Math.sin(tilt) * scene.mountHeight * S;
    svg.append(svgel("path", { d: `M ${ox} ${rz} L ${cx - spread} ${floorY} L ${cx + spread} ${floorY} Z`, class: "slab-fov" }));
  } else {
    const [x1, y1] = edge(boreAng - half), [x2, y2] = edge(boreAng + half);
    svg.append(svgel("path", { d: `M ${ox} ${rz} L ${x1} ${Math.min(y1, floorY)} L ${x2} ${Math.min(y2, floorY)} Z`, class: "slab-fov" }));
  }
  // radar
  svg.append(svgel("rect", { x: ox - 8, y: rz - 8, width: 10, height: 16, rx: 3, class: "slab-radar" }));
  // person silhouette (posture-aware)
  const px = ox + scene.person.x * S;
  const g = svgel("g", { class: "slab-person" + (view.detected ? " on" : "") });
  const post = scene.person.posture;
  if (post === "lying") {
    g.append(svgel("rect", { x: px - 26, y: zy(0.35), width: 52, height: 12, rx: 6 }));
    g.append(svgel("circle", { cx: px + 32, cy: zy(0.3), r: 7 }));
  } else {
    const h = post === "sitting" ? 1.25 : 1.72;
    g.append(svgel("rect", { x: px - 7, y: zy(h * 0.82), width: 14, height: (h * 0.82 - 0.35) * S, rx: 7 }));
    g.append(svgel("circle", { cx: px, cy: zy(h * 0.92), r: 8 }));
  }
  // chest displacement callout at chest height
  g.append(svgel("circle", { cx: px, cy: zy(view.chestH), r: 3.5, class: "slab-chest" }));
  svg.append(g);
}

function arcPath(cx, cy, r, a1, a2) {
  const x1 = cx + Math.cos(a1) * r, y1 = cy + Math.sin(a1) * r;
  const x2 = cx + Math.cos(a2) * r, y2 = cy + Math.sin(a2) * r;
  return `M ${x1} ${y1} A ${r} ${r} 0 0 1 ${x2} ${y2}`;
}

// ============================================================================
// §console — the pipeline, live
// ============================================================================

export function buildConsole() {
  const wrap = el("div", "slab-console");

  const cols = el("div", "slab-con-cols");

  const hexBox = el("div", "slab-con-col");
  hexBox.append(el("h4", null, "UART bytes (GPIO17 ← radar)"));
  const hex = el("div", "slab-hex wap-term");
  const hexScroll = el("div", "wap-term-scroll slab-hex-scroll");
  hex.append(hexScroll);
  hexBox.append(hex);

  const frameBox = el("div", "slab-con-col");
  frameBox.append(el("h4", null, "Decoded frames → FSMs"));
  const frames = el("div", "slab-framelog wap-term");
  const frameScroll = el("div", "wap-term-scroll slab-hex-scroll");
  frames.append(frameScroll);
  frameBox.append(frames);

  cols.append(hexBox, frameBox);
  wrap.append(cols);

  // chokepoint strip: raw in → coarse out
  const choke = el("div", "slab-choke");
  const rawSide = el("div", "slab-choke-side");
  rawSide.append(el("div", "slab-choke-h", "the radar said (read + dropped)"));
  const rawVals = el("div", "slab-choke-vals slab-mono");
  rawSide.append(rawVals);
  const wall = el("div", "slab-choke-wall");
  wall.append(el("span", null, "privacy"), el("span", null, "chokepoint"));
  const outSide = el("div", "slab-choke-side");
  outSide.append(el("div", "slab-choke-h", "the witness says (full vocabulary)"));
  const outVals = el("div", "slab-choke-vals slab-mono");
  outSide.append(outVals);
  choke.append(rawSide, wall, outSide);
  wrap.append(choke);

  // witness event log
  const evBox = el("div", "slab-events");
  evBox.append(el("h4", null, "Signed witness events (securacv/<id>/events)"));
  const evLog = el("div", "slab-evlog");
  evBox.append(evLog);
  wrap.append(evBox);

  const MAXROWS = 60;
  const trim = (node) => { while (node.childNodes.length > MAXROWS) node.removeChild(node.firstChild); };

  wrap.pushHex = (bytes, names) => {
    if (!bytes.length) return;
    const row = el("div", "wap-line");
    let s = "";
    for (let i = 0; i < bytes.length && i < 64; i++) s += bytes[i].toString(16).padStart(2, "0") + " ";
    if (bytes.length > 64) s += "…";
    row.textContent = s.trim();
    row.title = names.join(" + ");
    hexScroll.append(row); trim(hexScroll);
    hexScroll.scrollTop = hexScroll.scrollHeight;
  };

  wrap.pushFrame = (f) => {
    const row = el("div", "wap-line");
    const kindName = f.kind === 1 ? "Presence" : f.kind === 2 ? "Vitals" : "?";
    row.textContent =
      `${kindName.padEnd(8)} target=${f.has_target ? 1 : 0} n=${f.target_count} ` +
      `d=${f.distance_cm}cm br=${f.breath_rate} hr=${f.heart_rate}`;
    row.className = "wap-line " + (f.kind === 2 ? "slab-f-vitals" : "slab-f-presence");
    frameScroll.append(row); trim(frameScroll);
    frameScroll.scrollTop = frameScroll.scrollHeight;
  };

  wrap.pushStall = () => {
    const row = el("div", "wap-line wap-warn", "… no frames — stall deadline running");
    if (frameScroll.lastChild && frameScroll.lastChild.textContent === row.textContent) return;
    frameScroll.append(row); trim(frameScroll);
    frameScroll.scrollTop = frameScroll.scrollHeight;
  };

  wrap.setChokepoint = (raw, out) => {
    rawVals.innerHTML = "";
    for (const [k, v, dropped] of raw) {
      const r = el("div", "slab-choke-row" + (dropped ? " dropped" : ""));
      r.append(el("span", "slab-choke-k", k), el("span", null, String(v)));
      if (dropped) r.append(el("span", "slab-choke-x", "✕ dropped here"));
      rawVals.append(r);
    }
    outVals.innerHTML = "";
    for (const [k, v] of out) {
      const r = el("div", "slab-choke-row out");
      r.append(el("span", "slab-choke-k", k), el("span", null, String(v)));
      outVals.append(r);
    }
  };

  wrap.pushEvent = (ev) => {
    const row = el("div", "slab-ev card");
    const head = el("div", "slab-ev-head");
    head.append(el("strong", null, ev.name), el("span", "muted", ` seq ${ev.seq} · bucket ${ev.bucket}s`));
    head.append(el("span", "slab-ev-sig " + (ev.signed ? "cc-ok" : "cc-warn"), ev.signed ? "✓ Ed25519" : "unsigned"));
    row.append(head);
    const body = el("code", "fineprint slab-ev-canon", ev.canonical);
    row.append(body);
    if (ev.sig) row.append(el("code", "fineprint slab-ev-b64", "sig " + ev.sig.slice(0, 32) + "…"));
    evLog.prepend(row);
    while (evLog.childNodes.length > 8) evLog.removeChild(evLog.lastChild);
  };

  return wrap;
}

// ============================================================================
// §knobs — FSM tunables + build flavor
// ============================================================================

export function buildKnobs(data, state, onChange) {
  const wrap = el("div", "slab-knobs");

  const flav = el("div", "slab-ctl-row");
  flav.append(el("span", "slab-ctl-k", "build flavor"));
  for (const f of data.flavors) {
    const b = el("button", "ghost small" + (state.flavor === f.env ? " on" : ""), f.env.replace("canary-sense-", ""));
    b.dataset.flavor = f.env;
    b.title = f.vitals ? "vitals compiled in (-DCANARY_SENSE_VITALS)" : "presence-only — BPM entities provably absent";
    b.addEventListener("click", () => {
      state.flavor = f.env;
      for (const bb of flav.querySelectorAll("button")) bb.classList.toggle("on", bb === b);
      onChange({ rebuild: true });
    });
    flav.append(b);
  }
  const p1 = el("button", "ghost small" + (state.p1OptIn ? " on" : ""), "P1 opt-in (BPM entities)");
  p1.dataset.flavor = "p1";
  p1.addEventListener("click", () => { state.p1OptIn = !state.p1OptIn; p1.classList.toggle("on"); onChange({}); });
  flav.append(p1);
  wrap.append(flav);

  const grid = el("div", "slab-knob-grid");
  const KNOBS = [
    ["present_debounce_ms", "present debounce", 0, 3000, 50, "ms", "presence"],
    ["clear_timeout_ms", "clear timeout", 200, 10000, 100, "ms", "presence"],
    ["stall_timeout_ms", "radar stall", 1000, 20000, 250, "ms", "presence"],
    ["near_cm", "near band ≤", 50, 400, 10, "cm", "presence"],
    ["mid_cm", "mid band ≤", 100, 600, 10, "cm", "presence"],
    ["lock_confirm_ms", "vitals lock", 500, 15000, 250, "ms", "vitals"],
    ["lock_lost_ms", "vitals lost", 1000, 20000, 250, "ms", "vitals"],
  ];
  for (const [key, label, min, max, step, unit, group] of KNOBS) {
    const cfg = group === "presence" ? state.presenceCfg : state.vitalsCfg;
    const box = el("label", "slab-knob");
    box.append(el("span", "slab-ctl-k", label));
    const s = el("input");
    s.type = "range"; s.min = min; s.max = max; s.step = step; s.value = cfg[key];
    const out = el("span", "slab-ctl-v", cfg[key] + " " + unit);
    const def = group === "presence" ? data.fsm.presence[key] : data.fsm.vitals[key];
    s.addEventListener("input", () => {
      cfg[key] = parseInt(s.value, 10);
      out.textContent = cfg[key] + " " + unit + (cfg[key] !== def ? " *" : "");
      onChange({ recfg: true });
    });
    box.append(s, out);
    box.title = `firmware default ${def} ${unit} (CS_* in configs/canary-sense)`;
    grid.append(box);
  }
  wrap.append(grid);
  wrap.append(el("p", "fineprint",
    "Defaults are the firmware's own CS_* values (drift-gated from configs/canary-sense). " +
    "A * marks a knob you've moved off the shipped tuning — the point of a bench."));
  return wrap;
}

// ============================================================================
// §power lab — heat → useful computation
// ============================================================================

export function buildPowerLab(data, knobs, onChange) {
  const wrap = el("div", "slab-power");

  const ctl = el("div", "slab-stage-ctl");
  const rows = [
    ["heartbeatS", "status heartbeat", 1, 120, 1, "s"],
    ["eventsPerHour", "witness events", 0, 120, 1, "/h"],
    ["txPerEventMs", "TX burst per publish", 5, 200, 5, "ms"],
  ];
  for (const [key, label, min, max, step, unit] of rows) {
    const row = el("div", "slab-ctl-row");
    row.append(el("span", "slab-ctl-k", label));
    const s = el("input");
    s.type = "range"; s.min = min; s.max = max; s.step = step; s.value = knobs[key];
    const out = el("span", "slab-ctl-v", knobs[key] + " " + unit);
    s.addEventListener("input", () => {
      knobs[key] = parseInt(s.value, 10);
      out.textContent = knobs[key] + " " + unit;
      onChange();
    });
    row.append(s, out);
    ctl.append(row);
  }
  const togg = el("div", "slab-ctl-row slab-toggles");
  for (const [key, label] of [["modemSleep", "WiFi modem sleep"], ["ledOn", "status LED"], ["lux", "BH1750 lux"]]) {
    const b = el("button", "ghost small" + (knobs[key] ? " on" : ""), label);
    b.addEventListener("click", () => { knobs[key] = !knobs[key]; b.classList.toggle("on"); onChange(); });
    togg.append(b);
  }
  ctl.append(togg);
  wrap.append(ctl);

  const bars = el("div", "slab-pbars");
  const totals = el("div", "slab-ptotals");
  wrap.append(bars, totals);

  const noteBits = el("div", "slab-pnotes");
  for (const n of data.power.notes) noteBits.append(el("p", "fineprint", n));
  wrap.append(noteBits);

  wrap.update = () => {
    const m = powerModel(knobs, data.power.rails);
    bars.innerHTML = "";
    const max = Math.max(...m.parts.map((p) => p.mw));
    for (const p of m.parts) {
      const row = el("div", "slab-pbar");
      row.append(el("span", "slab-pbar-k", p.name));
      const track = el("div", "slab-pbar-track");
      const fill = el("div", "slab-pbar-fill");
      fill.style.width = Math.max(1.5, (p.mw / max) * 100) + "%";
      track.append(fill);
      row.append(track);
      row.append(el("span", "slab-pbar-v", fmt(p.mw, 1) + " mW"));
      row.title = p.note;
      bars.append(row);
    }
    totals.innerHTML = "";
    for (const [k, v] of [
      ["total draw", fmt(m.totalMw, 0) + " mW"],
      ["per day", fmt(m.perDayWh, 1) + " Wh"],
      ["per year", fmt(m.perYearKwh, 1) + " kWh"],
      ["signed claims", fmt(m.claimsPerHour, 0) + " /h"],
      ["claims per joule", m.claimsPerJoule.toExponential(2)],
      ["sensing share of heat", (m.sensingShare * 100).toFixed(0) + " %"],
    ]) {
      const c = el("span", "chip");
      c.append(el("span", "hub-chip-k", k + " "), el("strong", null, v));
      totals.append(c);
    }
    return m;
  };
  wrap.update();
  return wrap;
}

// ============================================================================
// §glass bridge — stage this witness on the real display firmware
// ============================================================================
//
// Lazily loads the committed wasm display build (the SAME firmware bytes the
// watch runs) and stages a canary-sense SimWitness on it. Live state from the
// bench is published through emu.publish() → the firmware's own MQTT
// dispatcher — so what appears on the glass is what the display firmware
// actually does with a radar sibling today (the honest gap included).

export function buildGlassBridge(data, getState) {
  const wrap = el("div", "slab-glass");
  const ribbon = el("p", "ondevice",
    "The glass below runs the real canary-display firmware compiled to WebAssembly. " +
    "This bench stages a canary-sense witness through the display's own MQTT dispatcher — " +
    "today the glass renders it generically (liveness, events, breathing ✓/—); the Canary Cards " +
    "schema (docs/standard/CANARY_CARDS.md) is the documented path to type-aware cards.");
  wrap.append(ribbon);

  const stage = el("div", "slab-glass-stage");
  const canvas = el("canvas", "slab-glass-canvas");
  canvas.width = 240; canvas.height = 240;
  const boot = el("button", "primary", "Boot the glass + join this witness");
  const status = el("p", "muted slab-glass-status", "The display artifact loads only when you ask (≈1 MB, from this repo — nothing phones anywhere).");
  stage.append(canvas, boot, status);
  wrap.append(stage);

  let emu = null, witness = null, started = false;

  async function bootGlass() {
    if (started) return;
    started = true;
    boot.disabled = true;
    status.textContent = "loading the wasm firmware…";
    try {
      await loadScript("emulator/dist/canary-display-watch.js");
      const shell = await import("./../emulator/web/emu-shell.js");
      emu = new shell.CanaryEmulator(window.createCanaryEmuWatch, {
        canvas,
        onDisplayReady: (w, h, round) => { if (round) canvas.classList.add("round"); },
        onBacklight: (glow) => { canvas.style.filter = `brightness(${Math.max(0.05, glow)})`; },
      });
      await emu.start({ provisioned: true, seed: 20260720 });
      const st = getState();
      witness = new shell.SimWitness({
        id: st.deviceId, deviceType: data.display.device_type_wire,
        name: "Sense Lab", room: "Bench", battery: null, rssi: -47,
      });
      await emu.addWitness(witness);
      emu.startFleetHeartbeat(15000);
      await publishState();
      status.textContent = "witness staged — move the person on the stage above and watch the glass.";
      boot.textContent = "glass is live";
    } catch (e) {
      status.textContent = "couldn't boot the glass here (" + e.message + ") — the committed artifact may be missing in this checkout.";
      boot.disabled = false;
      started = false;
    }
  }
  boot.addEventListener("click", bootGlass);

  async function publishState() {
    if (!emu || !witness) return;
    const st = getState();
    await emu.publish(witness.topic("state"), {
      presence: st.snapshot.presence,
      occupants: st.snapshot.count,
      range: st.snapshot.range,
      breathing_locked: !!st.snapshot.breathing_locked,
    }, { retained: true });
  }

  wrap.onEvent = async (name) => {
    if (!emu || !witness) return;
    await emu.witnessEvent(witness, name, { signed: true });
    await publishState();
  };
  wrap.onSnapshot = () => { publishState().catch(() => {}); };

  return wrap;
}

function loadScript(src) {
  return new Promise((resolve, reject) => {
    if (document.querySelector(`script[src="${src}"]`)) return resolve();
    const s = document.createElement("script");
    s.src = src;
    s.onload = resolve;
    s.onerror = () => reject(new Error("failed to load " + src));
    document.head.append(s);
  });
}

// ============================================================================
// Witness signing (WebCrypto Ed25519, same shape as emu-shell's SimWitness)
// ============================================================================

export class BenchSigner {
  constructor() { this.key = null; this.ready = false; }
  async init() {
    try {
      this.key = await crypto.subtle.generateKey("Ed25519", true, ["sign", "verify"]);
      this.ready = true;
    } catch { this.key = null; this.ready = false; }
    return this.ready;
  }
  async sign(canonical) {
    if (!this.key) return null;
    const sig = new Uint8Array(await crypto.subtle.sign(
      "Ed25519", this.key.privateKey, new TextEncoder().encode(canonical)));
    let b64 = btoa(String.fromCharCode(...sig));
    return b64.replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
  }
}
