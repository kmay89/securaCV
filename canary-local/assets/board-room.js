// canary-local/assets/board-room.js — the Board Room: every board, as the real thing.
//
// The Enclosure Lab's electronic sibling. Every vendor board GLB in the catalog
// (devices/boards.json) spins in the same zero-dependency WebGL viewer, but
// here the pins speak: 3D flags hang off the REAL pad geometry — anchors
// derived from the committed mesh's own pad islands (tools/pin_anchors.mjs),
// never eyeballed — and the "Wire it" mode builds a harness LEGO-instruction
// style: schematic peripherals fly up around the board and every wire lands on
// the exact castellated pad the firmware config names (devices/wiring.json).
//
// Honesty split, same as the Assembly tab: pads, pin functions and wire
// ENDPOINTS are real and drift-gated; peripheral shapes are schematic
// stand-ins sized from datasheets, and wire routing is staged for legibility.
// A `planned` pin/wire is defined in firmware config but not driven yet.
//
// Forward plan carried by the data, not this file: builds in wiring.json are
// permutations (many per device), and each connection's `signal`/`dir` is the
// binding point for live pin emulation — the wasm emulator's GPIO bus lighting
// these same flags. See canary-local/README.md §The Board Room.

import { DeviceScene, M4 } from "./scene3d.js";
import { parseGLB } from "./glb.js";
import { glossFor, hexOf } from "./board-lab.js";

const el = (tag, cls, text) => {
  const n = document.createElement(tag);
  if (cls) n.className = cls;
  if (text != null) n.textContent = text;
  return n;
};
const GH = "https://github.com/kmay89/securaCV/blob/main/";
const SVGNS = "http://www.w3.org/2000/svg";

// ── pure helpers (exported for tests/boardroom.test.js) ─────────────────────

// pinout rows → flag specs: one flag per anchored pad, labeled by the pin
// token that pad carries ("D6 / D7" with two anchors → a "D6" and a "D7" flag).
export function flagSpecs(board) {
  const flags = [];
  (board.pinout || []).forEach((row, ri) => {
    const anchors = row.anchors || (row.anchor ? [row.anchor] : []);
    if (!anchors.length) return;
    const names = String(row.pin || "").split("/").map((s) => s.trim()).filter(Boolean);
    anchors.forEach((a, i) => flags.push({
      text: anchors.length > 1 ? (names[i] || names[0] || row.label) : (names[0] || row.label),
      label: row.label,
      row: ri,
      planned: row.status === "planned",
      anchor: a,
    }));
  });
  return flags;
}

// board pads map minus comments → [{name, anchor}]
export function padEntries(board) {
  return Object.entries(board.pads || {})
    .filter(([k]) => !k.startsWith("_"))
    .map(([name, anchor]) => ({ name, anchor }));
}

// resolve a wiring `to` pin name to its mesh anchor: pads first, then any
// pinout row whose pin field carries the token (paired with its anchors).
export function resolvePin(board, name) {
  if (board.pads && board.pads[name] && !name.startsWith("_")) return board.pads[name];
  for (const row of board.pinout || []) {
    const names = String(row.pin || "").split("/").map((s) => s.trim());
    const i = names.indexOf(name);
    if (i < 0) continue;
    const anchors = row.anchors || (row.anchor ? [row.anchor] : []);
    if (anchors.length) return anchors[Math.min(i, anchors.length - 1)];
  }
  return null;
}

export function buildsFor(wiring, boardId) {
  return (wiring?.builds || []).filter((b) => b.board === boardId);
}

// quadratic bezier wire path: pad → lifted midpoint → peripheral pin
export function wirePoints(p0, p2, lift = 0, segs = 26) {
  const mid = [(p0[0] + p2[0]) / 2, Math.max(p0[1], p2[1]) + lift, (p0[2] + p2[2]) / 2];
  const pts = [];
  for (let i = 0; i <= segs; i++) {
    const t = i / segs, u = 1 - t;
    pts.push([
      u * u * p0[0] + 2 * u * t * mid[0] + t * t * p2[0],
      u * u * p0[1] + 2 * u * t * mid[1] + t * t * p2[1],
      u * u * p0[2] + 2 * u * t * mid[2] + t * t * p2[2],
    ]);
  }
  return pts;
}

// ── tiny Y-up geometry kit for schematic peripherals ────────────────────────
class MB {
  constructor() { this.pos = []; this.nrm = []; this.uv = []; this.idx = []; }
  v(p, n) { this.pos.push(p[0], p[1], p[2]); this.nrm.push(n[0], n[1], n[2]); this.uv.push(0, 0); return this.pos.length / 3 - 1; }
  quad(a, b, c, d) { this.idx.push(a, b, c, a, c, d); }
  tri(a, b, c) { this.idx.push(a, b, c); }
}
// axis-aligned box: x∈[-w/2,w/2], y∈[y0,y0+h], z∈[-d/2,d/2]
function box(w, h, d, y0 = 0) {
  const m = new MB(), hw = w / 2, hd = d / 2, y1 = y0 + h;
  const F = [
    [[+hw, y0, -hd], [+hw, y1, -hd], [+hw, y1, +hd], [+hw, y0, +hd], [1, 0, 0]],
    [[-hw, y0, +hd], [-hw, y1, +hd], [-hw, y1, -hd], [-hw, y0, -hd], [-1, 0, 0]],
    [[-hw, y1, -hd], [-hw, y1, +hd], [+hw, y1, +hd], [+hw, y1, -hd], [0, 1, 0]],
    [[-hw, y0, +hd], [-hw, y0, -hd], [+hw, y0, -hd], [+hw, y0, +hd], [0, -1, 0]],
    [[-hw, y0, +hd], [+hw, y0, +hd], [+hw, y1, +hd], [-hw, y1, +hd], [0, 0, 1]],
    [[+hw, y0, -hd], [-hw, y0, -hd], [-hw, y1, -hd], [+hw, y1, -hd], [0, 0, -1]],
  ];
  for (const [a, b, c, d2, n] of F) m.quad(m.v(a, n), m.v(b, n), m.v(c, n), m.v(d2, n));
  return m;
}
// cylinder, axis Y, y∈[y0,y0+h]
function tube(r, h, y0 = 0, seg = 26) {
  const m = new MB(), y1 = y0 + h;
  const ct = m.v([0, y1, 0], [0, 1, 0]), cb = m.v([0, y0, 0], [0, -1, 0]);
  for (let i = 0; i < seg; i++) {
    const a0 = (i / seg) * Math.PI * 2, a1 = ((i + 1) / seg) * Math.PI * 2;
    const c0 = [Math.cos(a0), Math.sin(a0)], c1 = [Math.cos(a1), Math.sin(a1)];
    m.quad(
      m.v([r * c0[0], y1, r * c0[1]], [c0[0], 0, c0[1]]), m.v([r * c0[0], y0, r * c0[1]], [c0[0], 0, c0[1]]),
      m.v([r * c1[0], y0, r * c1[1]], [c1[0], 0, c1[1]]), m.v([r * c1[0], y1, r * c1[1]], [c1[0], 0, c1[1]]));
    m.tri(ct, m.v([r * c0[0], y1, r * c0[1]], [0, 1, 0]), m.v([r * c1[0], y1, r * c1[1]], [0, 1, 0]));
    m.tri(cb, m.v([r * c1[0], y0, r * c1[1]], [0, -1, 0]), m.v([r * c0[0], y0, r * c0[1]], [0, -1, 0]));
  }
  return m;
}
// cylinder, axis X, x∈[x0,x0+len], centered at (y,z)=(cy,0)
function tubeX(r, len, x0, cy, seg = 20) {
  const m = new MB(), x1 = x0 + len;
  const ca = m.v([x1, cy, 0], [1, 0, 0]), cb = m.v([x0, cy, 0], [-1, 0, 0]);
  for (let i = 0; i < seg; i++) {
    const a0 = (i / seg) * Math.PI * 2, a1 = ((i + 1) / seg) * Math.PI * 2;
    const c0 = [Math.cos(a0), Math.sin(a0)], c1 = [Math.cos(a1), Math.sin(a1)];
    const p = (c, x) => [x, cy + r * c[1], r * c[0]];
    m.quad(
      m.v(p(c0, x1), [0, c0[1], c0[0]]), m.v(p(c0, x0), [0, c0[1], c0[0]]),
      m.v(p(c1, x0), [0, c1[1], c1[0]]), m.v(p(c1, x1), [0, c1[1], c1[0]]));
    m.tri(ca, m.v(p(c0, x1), [1, 0, 0]), m.v(p(c1, x1), [1, 0, 0]));
    m.tri(cb, m.v(p(c1, x0), [-1, 0, 0]), m.v(p(c0, x0), [-1, 0, 0]));
  }
  return m;
}

// schematic peripheral bodies (mm, Y-up, sitting on y=0). Pin coordinates in
// devices/wiring.json `peripherals[*].pins` are the solder points ON these
// shapes — change a body here, update its pins there.
const PCB_GREEN = [0.13, 0.35, 0.2], PCB_BLACK = [0.1, 0.1, 0.12];
const BRASS = [0.75, 0.6, 0.28], CERAMIC = [0.88, 0.86, 0.8], GLASS = [0.75, 0.83, 0.87];
const TAN = [0.78, 0.66, 0.45], LIPO = [0.2, 0.26, 0.52], WHITE = [0.92, 0.92, 0.9];
export const PERIPHERAL_PARTS = {
  piezo: () => [
    { builder: tube(7, 0.6, 0), color: BRASS, gloss: 0.6 },
    { builder: tube(4.5, 0.5, 0.6), color: CERAMIC, gloss: 0.35 },
  ],
  reed: () => [
    { builder: tubeX(1.25, 14, -7, 1.3), color: GLASS, gloss: 0.85 },
    { builder: tubeX(0.3, 4, -11, 1.3), color: [0.7, 0.71, 0.73], gloss: 0.6 },
    { builder: tubeX(0.3, 4, 7, 1.3), color: [0.7, 0.71, 0.73], gloss: 0.6 },
  ],
  ws2812: () => [
    { builder: box(10, 1.6, 10), color: PCB_BLACK, gloss: 0.3 },
    { builder: box(5, 1.1, 5, 1.6), color: WHITE, gloss: 0.7 },
  ],
  gnss: () => [
    { builder: box(16, 1.8, 16), color: PCB_GREEN, gloss: 0.3 },
    { builder: box(12, 3.4, 12, 1.8), color: TAN, gloss: 0.25 },
    { builder: tube(1.4, 0.8, 5.2), color: [0.7, 0.71, 0.73], gloss: 0.6 },
  ],
  divider: () => [
    { builder: box(12, 1.6, 8), color: PCB_GREEN, gloss: 0.3 },
    { builder: box(5, 2, 2.4, 1.6), color: TAN, gloss: 0.35, local: M4.translate(-2.6, 0, 0) },
    { builder: box(5, 2, 2.4, 1.6), color: TAN, gloss: 0.35, local: M4.translate(2.6, 0, 0) },
  ],
  lipo: () => [
    { builder: box(30, 6, 20), color: LIPO, gloss: 0.35 },
    { builder: box(4, 2.4, 5, 1.6), color: WHITE, gloss: 0.3, local: M4.translate(16.5, 0, 0) },
  ],
  // an isolated-input field loop: a small DC field supply (box) in series with a
  // dry contact (glass reed), the two leads landing on DI COM and DI0. The board
  // GND is deliberately NOT in this loop — the DI side is optocoupled.
  field_loop: () => [
    { builder: box(15, 5, 12), color: LIPO, gloss: 0.35, local: M4.translate(-8, 0, 0) },      // field supply
    { builder: box(4, 2.4, 5, 1.6), color: WHITE, gloss: 0.3, local: M4.translate(-1, 0, 0) },  // supply terminals
    { builder: tubeX(1.2, 9, 9, 1.3), color: GLASS, gloss: 0.85 },                               // dry contact (reed)
    { builder: tubeX(0.3, 3, 5, 1.3), color: [0.7, 0.71, 0.73], gloss: 0.6 },                    // contact lead
    { builder: tubeX(0.3, 3, 13, 1.3), color: [0.7, 0.71, 0.73], gloss: 0.6 },                   // contact lead
  ],
};

// compose translate·rotY (degrees) — peripherals only spin about Y in v1
const placeMat = (pos, rot) =>
  M4.mul(M4.translate(pos[0], pos[1], pos[2]), M4.rotY(((rot?.[1] || 0) * Math.PI) / 180));
const applyMat = (m, p) => [
  m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12],
  m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13],
  m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14],
];

function lineBuilder(pts) {
  const m = { pos: [], nrm: [], uv: [], idx: [] };
  pts.forEach((p, i) => {
    m.pos.push(p[0], p[1], p[2]); m.nrm.push(0, 1, 0); m.uv.push(0, 0);
    if (i) m.idx.push(i - 1, i);
  });
  return m;
}

// ── the room ────────────────────────────────────────────────────────────────
export function buildBoardRoom(mount, boardsData, wiring) {
  const wrap = el("div", "broom");
  mount.append(wrap);
  if (!boardsData?.boards || !Object.keys(boardsData.boards).length) {
    wrap.append(el("p", "muted", "Board catalog unavailable — run tools/gen_boards.py."));
    return wrap;
  }

  const pills = el("div", "pills");
  const modeRow = el("div", "subtabs broom-modes");
  const bBoard = el("button", "tab on", "the board");
  const bWire = el("button", "tab", "wire it");
  modeRow.append(bBoard, bWire);
  const padsToggle = el("label", "broom-pads-toggle");
  const padsCb = document.createElement("input");
  padsCb.type = "checkbox";
  padsToggle.append(padsCb, document.createTextNode(" every pad"));
  const tools = el("div", "broom-tools");
  tools.append(modeRow, padsToggle);

  const stage = el("div", "broom-stage");
  const cv = el("canvas", "broom-3d");
  const overlay = el("div", "broom-overlay");
  const svg = document.createElementNS(SVGNS, "svg");
  svg.setAttribute("class", "broom-leaders");
  overlay.append(svg);
  const legend = el("div", "broom-legend", "drag to orbit · scroll to zoom · pinch on touch");
  stage.append(cv, overlay, legend);
  const info = el("div", "broom-info");
  wrap.append(pills, tools, stage, info);

  const scene = new DeviceScene(cv, null);
  scene.start();
  cv.__scene = scene; // test/debug handle
  const iobs = new IntersectionObserver(() => {
    if (!document.body.contains(cv)) { scene.stop(); iobs.disconnect(); }
  });
  iobs.observe(cv);

  const room = {
    bid: null, board: null, parsed: null,
    mode: "board",            // "board" | "wire"
    build: null,              // active wiring build (wire mode)
    step: -1,                 // -1 = whole harness
    flags: [],                // [{spec, el, line, world:[x,y,z], ext}]
    wireParts: [],            // [{conn, part, color}]
    periParts: new Map(),     // peri id → engine meshes
    glbCache: new Map(),
  };

  async function loadParsed(board) {
    if (room.glbCache.has(board.glb)) return room.glbCache.get(board.glb);
    const buf = await (await fetch(board.glb)).arrayBuffer();
    const parsed = parseGLB(buf);
    room.glbCache.set(board.glb, parsed);
    return parsed;
  }

  // stage the board meshes with a given world offset (returns bbox)
  function stageBoard(parsed, offset) {
    const model = M4.translate(offset[0], offset[1], offset[2]);
    for (const p of parsed.parts) {
      scene.addMesh({ pos: p.pos, nrm: p.nrm, uv: p.uv, idx: p.idx },
        { color: p.color, gloss: glossFor(hexOf(p.color)), model });
    }
  }

  // ── flags overlay ──
  function setFlags(specs, offset) {
    for (const f of room.flags) { f.el.remove(); f.line.remove(); }
    room.flags = [];
    let i = 0;
    for (const spec of specs) {
      const d = el("div", "pin-flag" + (spec.planned ? " planned" : "") + (spec.minor ? " minor" : ""));
      d.textContent = spec.text;
      d.title = spec.label ? `${spec.label} — ${spec.text}` : spec.text;
      if (spec.row != null) {
        d.addEventListener("pointerenter", () => rowHi(spec.row, true));
        d.addEventListener("pointerleave", () => rowHi(spec.row, false));
      }
      const line = document.createElementNS(SVGNS, "line");
      line.setAttribute("class", "pin-leader" + (spec.planned ? " planned" : "") + (spec.minor ? " minor" : ""));
      svg.append(line);
      overlay.append(d);
      room.flags.push({
        spec, el: d, line,
        world: [spec.anchor[0] + offset[0], spec.anchor[1] + offset[1], spec.anchor[2] + offset[2]],
        ext: spec.minor ? 22 : (i++ % 2 ? 58 : 38), // alternate reach → fewer collisions
      });
    }
  }

  function rowHi(ri, on) {
    info.querySelectorAll(`.pin-row[data-row="${ri}"]`).forEach((r) => r.classList.toggle("hi", on));
    for (const f of room.flags) if (f.spec.row === ri) f.el.classList.toggle("on", on);
  }

  let rafId = null;
  function layoutTick() {
    rafId = null;
    if (!document.body.contains(cv)) return;
    const W = cv.clientWidth, H = cv.clientHeight;
    if (svg.getAttribute("width") != W) { svg.setAttribute("width", W); svg.setAttribute("height", H); }
    const pc = scene.project(0, 0, 0);
    for (const f of room.flags) {
      const p = scene.project(f.world[0], f.world[1], f.world[2]);
      const behind = p.depth > pc.depth + 4;
      let dx = p.x - pc.x, dy = p.y - pc.y;
      const l = Math.hypot(dx, dy) || 1;
      dx /= l; dy /= l;
      const lx = p.x + dx * f.ext, ly = p.y + dy * f.ext;
      f.el.style.transform = `translate(${lx.toFixed(1)}px, ${ly.toFixed(1)}px) translate(-50%, -50%)`;
      f.el.classList.toggle("back", behind);
      f.el.style.zIndex = String(Math.max(1, Math.round(1000 - p.depth)));
      f.line.setAttribute("x1", p.x.toFixed(1)); f.line.setAttribute("y1", p.y.toFixed(1));
      f.line.setAttribute("x2", lx.toFixed(1)); f.line.setAttribute("y2", ly.toFixed(1));
      f.line.classList.toggle("back", behind);
    }
    rafId = requestAnimationFrame(layoutTick);
  }
  function startOverlay() { if (!rafId) rafId = requestAnimationFrame(layoutTick); }

  // ── board mode ──
  function showBoardMode() {
    const b = room.board, parsed = room.parsed;
    scene.clearParts();
    const c = parsed.bbox.center;
    const offset = [-c[0], -c[1], -c[2]];
    stageBoard(parsed, offset);
    const pose = b.pose || { rx: -0.5, ry: 0.7, dist_factor: 2.7 };
    scene.dist = Math.max(...parsed.bbox.size) * (pose.dist_factor || 2.7);
    scene.rot = { x: pose.rx, y: pose.ry };
    scene.home = { x: pose.rx, y: pose.ry };
    scene.viewY = 0;
    scene.autoSway = true;

    const specs = flagSpecs(b);
    if (padsCb.checked) {
      const used = new Set(specs.map((s) => s.text));
      for (const p of padEntries(b)) {
        if (!used.has(p.name)) specs.push({ text: p.name, anchor: p.anchor, minor: true });
      }
    }
    setFlags(specs, offset);
    renderBoardInfo();
  }

  function renderBoardInfo() {
    const b = room.board;
    info.innerHTML = "";
    const ribbon = el("div", "board-ribbon");
    ribbon.append(el("strong", null, b.name),
      el("span", "muted", `  ${b.vendor}${b.mpn ? " · " + b.mpn : ""}`));
    info.append(ribbon);
    const prov = el("p", "ondevice board-prov");
    prov.append(el("strong", null, "What this is: "), document.createTextNode(b.provenance));
    info.append(prov);
    const dims = b.dims_mm;
    info.append(el("p", "muted boardlab-facts",
      `${dims[0]} × ${dims[1]} × ${dims[2]} mm · ${b.triangles.toLocaleString()} triangles · ${b.parts} parts, from the vendor STEP`));
    info.append(el("p", "body", b.blurb));

    if (b.pinout?.length) {
      info.append(el("h4", null, "Firmware pin map"));
      const tbl = el("div", "pin-table");
      let anyPlanned = false, anyFlagless = false;
      b.pinout.forEach((p, ri) => {
        const planned = p.status === "planned";
        anyPlanned = anyPlanned || planned;
        const hasFlag = !!(p.anchor || p.anchors);
        anyFlagless = anyFlagless || !hasFlag;
        const row = el("div", "pin-row" + (planned ? " pin-planned" : ""));
        row.dataset.row = String(ri);
        const label = el("span", "pin-label", p.label);
        if (planned) label.append(el("span", "pin-tag", "planned"));
        row.append(label, el("code", "pin-pin", p.pin),
          el("code", "pin-gpio", p.gpio || "—"), el("span", "pin-use muted", p.use));
        if (hasFlag) {
          row.classList.add("has-flag");
          row.addEventListener("pointerenter", () => rowHi(ri, true));
          row.addEventListener("pointerleave", () => rowHi(ri, false));
        }
        tbl.append(row);
      });
      info.append(tbl);
      const notes = [];
      if (anyPlanned) notes.push("“planned” — defined in firmware config but not yet driven or read by this build.");
      if (anyFlagless) notes.push("Rows without a 3D flag: that feature couldn't be pinpointed on the vendor mesh with confidence, so no flag is hung — the table is still the truth.");
      if (notes.length) info.append(el("p", "muted fineprint", notes.join(" ")));
    }

    const links = el("p", "muted fineprint boardlab-links");
    const dl = el("a", null, "download .glb");
    dl.href = b.glb; dl.download = room.bid + ".glb";
    links.append(dl, document.createTextNode(" · source: "));
    if (b.source_step) {
      const step = el("a", null, "vendor STEP");
      step.href = GH + b.source_step; step.target = "_blank"; step.rel = "noopener";
      links.append(step);
    } else {
      links.append(el("span", null, "procedural model"));  // no vendor CAD
    }
    if (b.doc) {
      const doc = el("a", null, "vendor docs ↗");
      doc.href = b.doc; doc.target = "_blank"; doc.rel = "noopener";
      links.append(document.createTextNode(" · "), doc);
    }
    info.append(links);
  }

  // ── wire mode ──
  function showWireMode() {
    const b = room.board, parsed = room.parsed, build = room.build;
    scene.clearParts();
    room.wireParts = [];
    room.periParts.clear();
    // tabletop frame: the board's underside sits on y=0 with the peripherals
    const bb = parsed.bbox;
    const offset = [-bb.center[0], -bb.min[1], -bb.center[2]];
    stageBoard(parsed, offset);

    // peripherals
    const periPin = {}; // "id.pin" → world point
    for (const pp of build.peripherals) {
      const cat = wiring.peripherals[pp.ref];
      const mat = placeMat(pp.pos, pp.rot);
      const meshes = (PERIPHERAL_PARTS[cat.part] || (() => []))().map((mm) => {
        const model = mm.local ? M4.mul(mat, mm.local) : mat;
        return scene.addMesh(mm.builder, { color: mm.color, gloss: mm.gloss ?? 0.3, model });
      });
      room.periParts.set(pp.id, { meshes, mats: meshes.map((m) => m.model), step: pp.step ?? 0, cat, spec: pp });
      for (const [pin, local] of Object.entries(cat.pins || {})) {
        periPin[pp.id + "." + pin] = applyMat(mat, local);
      }
    }

    // wires + wired-pad flags
    const wireFlags = [];
    const flaggedPads = new Set();
    build.connections.forEach((conn) => {
      const anchor = resolvePin(b, conn.to);
      const from = periPin[conn.from[0] + "." + conn.from[1]];
      if (!anchor || !from) return; // tests gate this; belt-and-braces at runtime
      const p0 = [anchor[0] + offset[0], anchor[1] + offset[1], anchor[2] + offset[2]];
      const span = Math.hypot(from[0] - p0[0], from[2] - p0[2]);
      const pts = wirePoints(p0, from, Math.max(5, span * 0.22));
      const color = wiring.colors[conn.color] || [0.8, 0.8, 0.3];
      const part = scene.addMesh(lineBuilder(pts), { color: color.slice(), lines: true, unlit: true });
      room.wireParts.push({ conn, part, color });
      if (!flaggedPads.has(conn.to)) {
        flaggedPads.add(conn.to);
        wireFlags.push({ text: conn.to, anchor, planned: conn.status === "planned" });
      }
    });
    setFlags(wireFlags, offset);

    // frame from full extent
    let span = Math.max(bb.size[0], bb.size[2]);
    for (const pp of build.peripherals) span = Math.max(span, Math.hypot(pp.pos[0], pp.pos[2]) * 2 + 24);
    const f = build.frame || {};
    scene.dist = span * (f.pad || 1.4);
    scene.rot = { x: f.rx ?? -0.62, y: f.ry ?? 0.5 };
    scene.home = { ...scene.rot };
    scene.viewY = 4;
    scene.autoSway = false;

    renderWireInfo();
    setStep(room.step);
  }

  const dim = (c) => c.map((x) => x * 0.16 + 0.06);
  function setStep(k) {
    room.step = k;
    const last = (room.build.steps?.length || 1) - 1;
    for (const [, peri] of room.periParts) {
      const on = k < 0 || peri.step <= k;
      peri.meshes.forEach((m, i) => { m.model = on ? peri.mats[i] : M4.scale(0, 0, 0); });
    }
    for (const w of room.wireParts) {
      const ws = w.conn.step ?? 0;
      if (k >= 0 && ws > k) { w.part.model = M4.scale(0, 0, 0); continue; }
      w.part.model = M4.ident();
      w.part.color = (k < 0 || ws === k) ? w.color.slice() : dim(w.color);
    }
    info.querySelectorAll(".wire-step-card").forEach((c) => {
      c.classList.toggle("on", Number(c.dataset.step) === k);
    });
    const counter = info.querySelector(".wire-counter");
    if (counter) counter.textContent = k < 0 ? "whole harness" : `step ${k + 1} / ${last + 1}`;
  }

  function renderWireInfo() {
    const build = room.build, b = room.board;
    info.innerHTML = "";
    const ribbon = el("p", "ondevice asm-prov");
    ribbon.append(el("strong", null, "How to read this: "), document.createTextNode(
      "pads and pin functions are the drift-gated catalog; every wire ends on the exact pad the firmware config names. " +
      "Peripheral bodies are schematic stand-ins sized from datasheets, and the wire routing is staged for legibility — the endpoints are the claim."));
    info.append(ribbon);

    info.append(el("h4", null, build.title));
    if (build.note) info.append(el("p", "body", build.note));

    // player
    const nav = el("div", "asm-nav wire-nav");
    const prev = el("button", "ghost", "‹ back");
    const all = el("button", "ghost", "whole harness");
    const next = el("button", "primary", "next ›");
    const counter = el("span", "muted wire-counter");
    nav.append(prev, all, counter, next);
    info.append(nav);
    const last = (build.steps?.length || 1) - 1;
    prev.addEventListener("click", () => setStep(room.step <= 0 ? -1 : room.step - 1));
    next.addEventListener("click", () => setStep(room.step >= last ? last : room.step + 1));
    all.addEventListener("click", () => setStep(-1));

    // step cards with their connections
    (build.steps || []).forEach((s, i) => {
      const card = el("div", "wire-step-card");
      card.dataset.step = String(i);
      card.append(el("h5", null, `${i + 1}. ${s.title}`));
      if (s.note) card.append(el("p", "muted", s.note));
      const list = el("div", "wire-list");
      for (const conn of build.connections.filter((c) => (c.step ?? 0) === i)) {
        const row = el("div", "wire-row" + (conn.status === "planned" ? " pin-planned" : ""));
        const chip = el("i", "wire-chip");
        const col = wiring.colors[conn.color] || [0.8, 0.8, 0.3];
        chip.style.background = `rgb(${col.map((x) => Math.round(x * 255)).join(",")})`;
        const peri = build.peripherals.find((p) => p.id === conn.from[0]);
        const periName = wiring.peripherals[peri?.ref]?.name || conn.from[0];
        row.append(chip,
          el("span", "wire-from", `${periName} ${conn.from[1]}`),
          el("span", "wire-arrow", "→"),
          el("code", "pin-pin", conn.to));
        if (conn.status === "planned") row.append(el("span", "pin-tag", "planned"));
        if (conn.note) row.append(el("span", "wire-note muted", conn.note));
        list.append(row);
      }
      card.append(list);
      card.addEventListener("click", () => setStep(i));
      info.append(card);
    });

    const periNote = el("p", "muted fineprint");
    const names = build.peripherals.map((p) => wiring.peripherals[p.ref]?.name).filter(Boolean);
    periNote.textContent = "On the bench: " + names.join(" · ") +
      ". Signals (chirp, tamper, gnss_uart…) are named on each wire in devices/wiring.json — the hook where the firmware emulator's GPIO state will light these pins live.";
    info.append(periNote);
  }

  // ── mode + board switching ──
  function setMode(m) {
    if (m === "wire" && !room.build) m = "board";
    room.mode = m;
    bBoard.classList.toggle("on", m === "board");
    bWire.classList.toggle("on", m === "wire");
    padsToggle.hidden = m !== "board";
    // mid-fetch the stage holds no (or the PREVIOUS board's) mesh — remember
    // the chosen mode and let showBoard() render it when the parse lands
    if (!room.parsed) return;
    if (m === "board") showBoardMode(); else showWireMode();
  }
  bBoard.addEventListener("click", () => setMode("board"));
  bWire.addEventListener("click", () => setMode("wire"));
  padsCb.addEventListener("change", () => { if (room.mode === "board" && room.parsed) showBoardMode(); });

  async function showBoard(bid) {
    room.bid = bid;
    room.board = boardsData.boards[bid];
    room.parsed = null; // guards setMode/toggle from rendering a stale mesh
    room.step = -1;
    const builds = buildsFor(wiring, bid);
    room.build = builds[0] || null;
    bWire.disabled = !room.build;
    bWire.title = room.build ? "" : "no wiring harness authored for this board yet";
    setFlags([], [0, 0, 0]);
    info.innerHTML = "";
    info.append(el("p", "muted", "loading the vendor mesh…"));
    let parsed;
    try {
      parsed = await loadParsed(room.board);
    } catch {
      info.innerHTML = "";
      info.append(el("p", "muted", "board mesh unavailable — run tools/gen_boards.py"));
      return;
    }
    if (room.bid !== bid || !document.body.contains(cv)) return; // moved on mid-fetch
    room.parsed = parsed;
    setMode(room.mode === "wire" && room.build ? "wire" : "board");
    startOverlay();
  }

  let first = null;
  for (const [bid, b] of Object.entries(boardsData.boards)) {
    const btn = el("button", "pill");
    btn.textContent = b.name;
    btn.title = (b.devices || []).join(" · ");
    btn.addEventListener("click", () => {
      for (const x of pills.children) x.classList.remove("on");
      btn.classList.add("on");
      showBoard(bid);
    });
    pills.append(btn);
    first ||= btn;
  }
  first?.click();
  return wrap;
}
