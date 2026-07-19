// canary-local/assets/house.js — the Canary House renderer.
//
// Draws an isometric cutaway home from house-data.js (rooms as data,
// perches as data), animates each Canary's sensing modality the way it
// actually works — camera cones that keep their pixels, WiFi-field
// ripples, radar arcs, a breathing wave, display glows — and walks a
// visitor through so the witness feed can show the ONLY thing a flock
// ever says out loud: small signed claims.
//
// Pure SVG + CSS animation; no libraries, works offline like the rest
// of canary.local.
import {
  WALL_H, SLAB_T, FLOORS, ROOMS, DIVIDERS, DIVIDER_H,
  SENSE_COPY, PLACEMENTS, WALK,
  placementInfo, chooserHash, flockSummary,
} from "./house-data.js";

const SVG = "http://www.w3.org/2000/svg";
const UX = 30, UY = 15, UZ = 24; // iso projection scales (px per unit)

const px = (x, y, z = 0) => [(x - y) * UX, (x + y) * UY - z * UZ];
const pts = (list) => list.map((p) => px(...p).join(",")).join(" ");
const rad = (deg) => (deg * Math.PI) / 180;

function el(tag, attrs = {}, cls) {
  const n = document.createElementNS(SVG, tag);
  for (const [k, v] of Object.entries(attrs)) n.setAttribute(k, v);
  if (cls) n.setAttribute("class", cls);
  return n;
}
function html(tag, cls, text) {
  const n = document.createElement(tag);
  if (cls) n.className = cls;
  if (text != null) n.textContent = text;
  return n;
}

const floorOf = (room) => FLOORS.find((f) => f.id === room.floor) || FLOORS[0];
const roomOf = (id) => ROOMS.find((r) => r.id === id);
const worldOf = (p) => {
  const room = roomOf(p.room);
  const z = room && !room.outside ? floorOf(room).z : 0;
  return { x: p.at.x, y: p.at.y, z: z + p.at.z };
};

// ── primitive builders ─────────────────────────────────────────────────

// A box drawn dollhouse-style: top face + the two viewer-facing sides.
function box(g, x0, y0, x1, y1, z0, h, fills, cls) {
  const [ft, fl, fr] = fills;
  const grp = el("g", {}, cls);
  grp.append(
    el("polygon", { points: pts([[x0, y1, z0 + h], [x1, y1, z0 + h], [x1, y1, z0], [x0, y1, z0]]), fill: fl }),
    el("polygon", { points: pts([[x1, y0, z0 + h], [x1, y1, z0 + h], [x1, y1, z0], [x1, y0, z0]]), fill: fr }),
    el("polygon", { points: pts([[x0, y0, z0 + h], [x1, y0, z0 + h], [x1, y1, z0 + h], [x0, y1, z0 + h]]), fill: ft }),
  );
  g.append(grp);
  return grp;
}

// A standing wall from plan segment (x0,y0)→(x1,y1), raised h from z0.
function wall(g, x0, y0, x1, y1, z0, h, fill, cls) {
  const q = el("polygon", {
    points: pts([[x0, y0, z0], [x1, y1, z0], [x1, y1, z0 + h], [x0, y0, z0 + h]]),
    fill,
  }, cls);
  g.append(q);
  return q;
}

// Iso ellipse (a circle in plan) centred on a world point.
function planRing(cx, cy, z, r, attrs, cls) {
  const [sx, sy] = px(cx, cy, z);
  const g = el("g", { transform: `translate(${sx},${sy})` }, cls);
  g.append(el("ellipse", { cx: 0, cy: 0, rx: (r * UX * 1.414).toFixed(1), ry: (r * UY * 1.414).toFixed(1), ...attrs }));
  return g;
}

// A wedge (sector) in the floor plane: centre, aim degrees, half-angle,
// radius — projected point-by-point so it sits properly in iso.
function planWedge(cx, cy, z, aim, half, r, attrs, cls) {
  const p = [px(cx, cy, z)];
  for (let a = aim - half; a <= aim + half; a += 6) {
    p.push(px(cx + r * Math.cos(rad(a)), cy + r * Math.sin(rad(a)), z));
  }
  return el("polygon", { points: p.map((q) => q.join(",")).join(" "), ...attrs }, cls);
}

// An arc (stroke only) in the floor plane.
function planArc(cx, cy, z, aim, half, r, attrs, cls) {
  let d = "";
  for (let a = aim - half; a <= aim + half; a += 6) {
    const [sx, sy] = px(cx + r * Math.cos(rad(a)), cy + r * Math.sin(rad(a)), z);
    d += (d ? " L" : "M") + sx.toFixed(1) + " " + sy.toFixed(1);
  }
  return el("path", { d, fill: "none", ...attrs }, cls);
}

// ── the scene ──────────────────────────────────────────────────────────

const stage = document.getElementById("stage");
const svg = el("svg", { id: "house-svg", role: "img" });
svg.setAttribute("aria-label",
  "Isometric cutaway of a two-storey home showing where each Canary witness lives and what it senses");
const world = el("g", { id: "world" });
svg.append(world);
stage.append(svg);

const layers = {};
for (const id of ["yard", "ground", "upper", "fx", "person"]) {
  layers[id] = el("g", { id: `layer-${id}` });
  world.append(layers[id]);
}

function drawYard() {
  const g = layers.yard;
  const yard = roomOf("yard");
  // lawn
  box(g, yard.x0, yard.y0, yard.x1, yard.y1, -0.55, 0.4,
    ["#131a16", "#0a0e0c", "#0d1210"], "lawn");
  // faint survey grid on the lawn — quiet, technical, intentional
  for (let gx = Math.ceil(yard.x0); gx <= Math.floor(yard.x1); gx++) {
    const [ax, ay] = px(gx, yard.y0, -0.14), [bx, by] = px(gx, yard.y1, -0.14);
    g.append(el("line", { x1: ax, y1: ay, x2: bx, y2: by,
      stroke: "rgba(210,230,215,0.03)", "stroke-width": 0.8 }));
  }
  for (let gy = Math.ceil(yard.y0); gy <= Math.floor(yard.y1); gy++) {
    const [ax, ay] = px(yard.x0, gy, -0.14), [bx, by] = px(yard.x1, gy, -0.14);
    g.append(el("line", { x1: ax, y1: ay, x2: bx, y2: by,
      stroke: "rgba(210,230,215,0.03)", "stroke-width": 0.8 }));
  }
  // the house's grounding shadow
  g.append(planRing(5.1, 3.3, -0.13, 6.4, { fill: "url(#groundShadow)" }, "house-shadow"));
  // walkway from the street to the porch
  box(g, 5.65, 8.5, 6.95, 10.0, -0.18, 0.1, ["#20242a", "#14161a", "#181b20"]);
  // porch slab
  const porch = roomOf("porch");
  box(g, porch.x0, porch.y0, porch.x1, porch.y1, -0.15, 0.18, ["#232529", "#151619", "#191a1e"]);
  // chain-link fence along the property line (the Fence Guard's perch)
  const FX = 12.15, FY0 = 1.6, FY1 = 8.6, FH = 1.25;
  for (let fy = FY0; fy <= FY1 + 0.01; fy += (FY1 - FY0) / 4) {
    wall(g, FX, fy, FX + 0.05, fy + 0.05, 0, FH + 0.1, "#3a3e46", "fence-post");
  }
  for (const fz of [0.18, FH]) {
    const [ax, ay] = px(FX, FY0, fz), [bx2, by2] = px(FX, FY1, fz);
    g.append(el("line", { x1: ax, y1: ay, x2: bx2, y2: by2, stroke: "#34383f", "stroke-width": 1.2 }));
  }
  for (let fy = FY0; fy < FY1 - 0.4; fy += 0.45) {
    const [ax, ay] = px(FX, fy, 0.18), [bx2, by2] = px(FX, Math.min(fy + 0.9, FY1), FH);
    const [cx2, cy2] = px(FX, Math.min(fy + 0.9, FY1), 0.18), [dx2, dy2] = px(FX, fy, FH);
    g.append(
      el("line", { x1: ax, y1: ay, x2: bx2, y2: by2, stroke: "rgba(150,160,172,0.13)", "stroke-width": 0.8 }),
      el("line", { x1: cx2, y1: cy2, x2: dx2, y2: dy2, stroke: "rgba(150,160,172,0.13)", "stroke-width": 0.8 }),
    );
  }
  // the shade tree the Fence Guard hides under (solar likes cool cells)
  {
    const [sx, sy] = px(12.35, 6.9, 0.16);
    const t = el("g", { transform: `translate(${sx},${sy})` });
    t.append(
      el("ellipse", { cx: -14, cy: 4, rx: 34, ry: 12, fill: "rgba(0,0,0,0.32)" }), // cast shade
      el("rect", { x: -1.6, y: -26, width: 3.2, height: 28, fill: "#241d15" }),
      el("ellipse", { cx: 0, cy: -34, rx: 24, ry: 15, fill: "#152015" }),
      el("ellipse", { cx: -10, cy: -26, rx: 16, ry: 10, fill: "#1a2519" }),
      el("ellipse", { cx: 11, cy: -27, rx: 14, ry: 9, fill: "#111c12" }),
    );
    g.append(t);
  }
  // bushes
  for (const [bx, by, r] of [[-1.1, 7.6, 0.8], [11.6, 8.2, 0.65], [-1.3, 1.2, 0.7]]) {
    const [sx, sy] = px(bx, by, 0.16);
    const b = el("g", { transform: `translate(${sx},${sy})` });
    b.append(el("ellipse", { cx: 0, cy: 0, rx: r * UX, ry: r * UY, fill: "#111813" }));
    b.append(el("ellipse", { cx: 0, cy: -r * 9, rx: r * UX * 0.8, ry: r * UY * 0.9, fill: "#162015" }));
    b.append(el("ellipse", { cx: r * 6, cy: -r * 14, rx: r * UX * 0.5, ry: r * UY * 0.6, fill: "#1a2519" }));
    g.append(b);
  }
}

function drawFloor(floorId) {
  const g = layers[floorId];
  const floor = FLOORS.find((f) => f.id === floorId);
  const rooms = ROOMS.filter((r) => r.floor === floorId && !r.outside);
  const x1 = Math.max(...rooms.map((r) => r.x1));
  const y1 = Math.max(...rooms.map((r) => r.y1));
  const z = floor.z;

  // slab
  box(g, 0, 0, x1, y1, z - SLAB_T, SLAB_T, ["#26282e", "#141519", "#1a1b20"], "slab");
  // back walls (x=0 and y=0), with warm windows
  wall(g, 0, 0, 0, y1, z, WALL_H, "#212329", "wall");
  wall(g, 0, 0, x1, 0, z, WALL_H, "#1a1c21", "wall");
  const windows = floorId === "ground"
    ? [["x", 1.4, 2.6], ["x", 3.2, 4.4], ["y", 5.6, 6.8], ["y", 8.2, 9.4]]
    : [["x", 1.2, 2.4], ["x", 3.0, 4.2], ["y", 6.2, 7.4], ["y", 8.4, 9.6]];
  for (const [axis, a0, a1] of windows) {
    const attrs = { fill: "#453714", stroke: "#66521f", "stroke-width": 0.8, filter: "url(#warmGlow)" };
    const w = axis === "x"
      ? el("polygon", { points: pts([[0, a0, z + 1.1], [0, a1, z + 1.1], [0, a1, z + 2.3], [0, a0, z + 2.3]]), ...attrs }, "window")
      : el("polygon", { points: pts([[a0, 0, z + 1.1], [a1, 0, z + 1.1], [a1, 0, z + 2.3], [a0, 0, z + 2.3]]), ...attrs }, "window");
    g.append(w);
  }
  // ghost dividers
  for (const d of DIVIDERS.filter((d) => d.floor === floorId)) {
    wall(g, d.x0, d.y0, d.x1, d.y1, z, DIVIDER_H, "rgba(96,100,112,0.42)", "divider");
  }
}

function drawLabels() {
  for (const r of ROOMS.filter((r) => !r.outside)) {
    const z = floorOf(r).z;
    const [sx, sy] = px((r.x0 + r.x1) / 2, (r.y0 + r.y1) / 2, z);
    // type is styled inline so a cached stylesheet can never blow it up
    const t = el("text", {
      x: sx, y: sy + 4, "text-anchor": "middle",
      fill: "#7c7f88", "font-size": "10", "font-weight": "600",
      "letter-spacing": "1.6", opacity: "0.85",
    }, "room-label");
    t.textContent = r.label.toUpperCase();
    layers[r.floor].append(t);
  }
}

function drawFurniture() {
  const g0 = layers.ground, g1 = layers.upper, zU = FLOORS[1].z;
  const F = ["#2e3037", "#1b1c21", "#222329"];
  const S = ["#38332b", "#211e19", "#2a2620"]; // soft/warm pieces
  box(g0, 1.0, 2.7, 2.1, 5.0, 0, 0.62, S);          // sofa
  box(g0, 2.7, 3.1, 3.5, 4.3, 0, 0.38, F);          // coffee table
  box(g0, 4.85, 0.05, 9.7, 0.65, 0, 0.9, F);        // kitchen counter
  box(g0, 6.3, 1.5, 8.3, 2.5, 0, 0.88, F);          // island
  box(g0, 7.9, 3.8, 9.7, 6.2, 0, 1.15, ["#1c1f24", "#101216", "#14161a"]); // car
  box(g1, 1.4, 0.4, 3.6, 3.1, zU, 0.55, S);         // bed
  box(g1, 1.6, 0.55, 2.2, 1.1, zU + 0.55, 0.16, ["#2e2b26", "#1a1815", "#211f1a"]); // pillow
  box(g1, 7.35, 1.15, 8.45, 2.35, zU, 0.72, S);     // crib
  box(g1, 0.7, 4.5, 2.3, 5.3, zU, 0.72, F);         // desk
  // front door frame on the entry's open face — the doorbell's perch
  const g = layers.ground;
  wall(g, 5.55, 6.5, 6.55, 6.5, 0, 2.25, "#2a2c33", "doorframe");
  wall(g, 5.62, 6.5, 6.48, 6.5, 0.05, 2.1, "#54401b", "doorlight")
    .setAttribute("filter", "url(#warmGlow)");
  // LoRa relay pole in the yard
  const relay = PLACEMENTS.find((p) => p.id === "relay");
  if (relay) {
    const { x, y } = relay.at;
    wall(layers.yard, x, y, x + 0.06, y + 0.06, 0, 2.6, "#33363e", "pole");
    box(layers.yard, x - 0.32, y - 0.32, x + 0.38, y + 0.38, 2.6, 0.12,
      ["#3a3f4a", "#20232a", "#282c34"]); // solar roof
  }
  // the Fence Guard concept: a small sealed box on the chain-link,
  // wearing a tilted solar sliver — drawn ghost-quiet, it's a teaser
  const guard = PLACEMENTS.find((p) => p.id === "fence-guard");
  if (guard) {
    const { x, y, z } = guard.at;
    box(layers.yard, x - 0.14, y - 0.16, x + 0.14, y + 0.16, z - 0.24, 0.42,
      ["#252b2c", "#151a1b", "#1b2122"], "guard-body");
    box(layers.yard, x - 0.2, y - 0.24, x + 0.24, y + 0.28, z + 0.28, 0.07,
      ["#2c3a44", "#181f26", "#20282f"], "guard-solar");
  }
}

// ── sensing fields ─────────────────────────────────────────────────────

function fieldFor(p) {
  const w = worldOf(p);
  const g = el("g", { "data-field": p.id }, `field field-${p.sense}`);
  if (p.sense === "camera") {
    // a true view frustum: apex at the lens, base swept on the floor —
    // light falling from the camera, not a puddle beside it
    const zf = w.z - p.at.z;
    const beam = [px(w.x, w.y, w.z)];
    for (let a = p.aim - 24; a <= p.aim + 24; a += 6) {
      beam.push(px(w.x + p.range * Math.cos(rad(a)), w.y + p.range * Math.sin(rad(a)), zf));
    }
    g.append(el("polygon", {
      points: beam.map((q) => q.join(",")).join(" "), fill: "url(#coneFill)",
    }, "cone"));
    for (let i = 1; i <= 3; i++) {
      const a = planArc(w.x, w.y, w.z - p.at.z, p.aim, 22, (p.range * i) / 3.2,
        { stroke: "#ffd44f", "stroke-width": 1.1 }, "scan");
      a.style.animationDelay = `${i * 0.5}s`;
      g.append(a);
    }
  } else if (p.sense === "wifi" || p.sense === "lora" || p.sense === "mesh") {
    const n = p.sense === "wifi" ? 3 : 2;
    const stroke = p.sense === "lora" ? "#7fb7d8" : p.sense === "mesh" ? "#6fd6c3" : "#ffd44f";
    for (let i = 0; i < n; i++) {
      const ring = planRing(w.x, w.y, w.z - p.at.z, p.range,
        p.sense === "wifi"
          ? { fill: "none", stroke, "stroke-width": 1.2 }
          : { fill: "none", stroke, "stroke-width": 1, "stroke-dasharray": "5 7", "stroke-opacity": 0.55 },
        "ripple");
      ring.style.animationDelay = `${(i * (p.sense === "wifi" ? 1.3 : 2.4)).toFixed(1)}s`;
      g.append(ring);
    }
    if (p.sense === "mesh") {
      // the mesh handshake: a marching dashed arc to the relay's pole —
      // "to their field", literally
      const relay = PLACEMENTS.find((q) => q.id === "relay");
      if (relay) {
        const [ax, ay] = px(w.x, w.y, w.z + 0.35);
        const [bx, by] = px(relay.at.x, relay.at.y, 2.7);
        const [mx, my] = [(ax + bx) / 2, Math.min(ay, by) - 34];
        g.append(el("path", {
          d: `M ${ax.toFixed(1)} ${ay.toFixed(1)} Q ${mx.toFixed(1)} ${my.toFixed(1)} ${bx.toFixed(1)} ${by.toFixed(1)}`,
          fill: "none", stroke: "#6fd6c3", "stroke-width": 1.1,
          "stroke-dasharray": "4 7", "stroke-opacity": 0.6,
        }, "mesh-link"));
      }
    }
  } else if (p.sense === "radar" || p.sense === "breath") {
    const zf = w.z - p.at.z;
    for (let i = 1; i <= 3; i++) {
      const a = planArc(w.x, w.y, zf, p.aim, 30, (p.range * i) / 3,
        { stroke: p.sense === "breath" ? "#8fd3a8" : "#c9a4f0", "stroke-width": 1.3 }, "sweep");
      a.style.animationDelay = `${i * 0.45}s`;
      g.append(a);
    }
    if (p.sense === "breath") {
      // the breathing line: a small wave near the crib, drawing itself
      const [sx, sy] = px(w.x + Math.cos(rad(p.aim)) * 1.4, w.y + Math.sin(rad(p.aim)) * 1.4, w.z + 0.4);
      let d = `M ${sx - 26} ${sy}`;
      for (let i = 0; i < 3; i++) d += ` q 4 -9 9 0 q 4 8 9 0`;
      g.append(el("path", { d, fill: "none", stroke: "#8fd3a8", "stroke-width": 1.4 }, "breathline"));
    }
  } else if (p.sense === "display") {
    const [sx, sy] = px(w.x, w.y, w.z);
    const halo = el("g", { transform: `translate(${sx},${sy})` }, "halo");
    halo.append(el("circle", { cx: 0, cy: 0, r: 20, fill: "url(#haloFill)" }));
    g.append(halo);
  }
  return g;
}

// ── markers ────────────────────────────────────────────────────────────

const GLYPHS = {
  camera: "M-3.5 -2.5 h7 v5 h-7 z M2 -0.2 a2 2 0 1 0 0.01 0", // lens box
  wifi: "M-4 1.5 q4 -6 8 0 M-2.2 2.6 q2.2 -3.2 4.4 0",
  radar: "M-3.5 2.5 q3.5 -7 7 0 M-1.8 2.5 q1.8 -3.6 3.6 0",
  breath: "M-4.5 0.8 q1.5 -5 3 0 q1.5 5 3 0 q1.5 -5 3 0",
  display: "M-4 -2.8 h8 v5 h-8 z",
  lora: "M0 3 v-5 M-3 -2 q3 -3.4 6 0",
  mesh: "M-3.5 2.5 l3.5 -5.5 l3.5 5.5 z M0 -3 v-2", // mast + uplink
};

function markerFor(p, info) {
  const w = worldOf(p);
  const [sx, sy] = px(w.x, w.y, w.z);
  const g = el("g", {
    transform: `translate(${sx},${sy})`,
    tabindex: 0, role: "button",
    "data-marker": p.id,
    "aria-label": `${info.title} — ${p.spot}. ${p.headline}`,
  }, "marker");
  // presentation attrs are inline so the marker reads even with no CSS;
  // house.css repeats these values and adds hover/selection states.
  // Teaser concepts wear teal and a dashed ring — visibly not-yet-real.
  const tone = info.teaser ? "#6fd6c3" : "#ffd44f";
  if (info.teaser) g.classList.add("teaser");
  g.append(
    el("ellipse", { cx: 0, cy: 12, rx: 10, ry: 4.5, fill: "rgba(0,0,0,0.5)" }, "marker-shadow"),
    el("circle", { cx: 0, cy: 0, r: 12.5, fill: "none", stroke: info.teaser ? "rgba(111,214,195,0.45)" : "rgba(255,212,79,0.55)", "stroke-width": 1.2 }, "marker-pulse"),
    el("circle", { cx: 0, cy: 0, r: 9, fill: "#141414", stroke: tone, "stroke-width": 1.6,
      ...(info.teaser ? { "stroke-dasharray": "3 2.5" } : {}), filter: "url(#glow)" }, "marker-body"),
    el("path", { d: GLYPHS[p.sense] || GLYPHS.display, transform: "scale(0.95)",
      fill: "none", stroke: tone, "stroke-width": 1.5,
      "stroke-linecap": "round", "stroke-linejoin": "round" }, "marker-glyph"),
  );
  if (!info.teaser && info.status !== "released") {
    g.append(el("circle", { cx: 7.5, cy: -7.5, r: 3, fill: "#fb8c00", stroke: "#000", "stroke-width": 0.8 }, "marker-dev"));
  }
  return g;
}

// ── defs ───────────────────────────────────────────────────────────────

const defs = el("defs");
defs.innerHTML = `
  <radialGradient id="haloFill">
    <stop offset="0%" stop-color="rgba(255,212,79,0.5)"/>
    <stop offset="100%" stop-color="rgba(255,212,79,0)"/>
  </radialGradient>
  <radialGradient id="coneFill">
    <stop offset="0%" stop-color="rgba(255,212,79,0.30)"/>
    <stop offset="100%" stop-color="rgba(255,212,79,0.02)"/>
  </radialGradient>
  <radialGradient id="poolWarm">
    <stop offset="0%" stop-color="rgba(255,212,79,0.16)"/>
    <stop offset="65%" stop-color="rgba(255,212,79,0.05)"/>
    <stop offset="100%" stop-color="rgba(255,212,79,0)"/>
  </radialGradient>
  <radialGradient id="groundShadow">
    <stop offset="0%" stop-color="rgba(0,0,0,0.6)"/>
    <stop offset="70%" stop-color="rgba(0,0,0,0.28)"/>
    <stop offset="100%" stop-color="rgba(0,0,0,0)"/>
  </radialGradient>
  <filter id="glow" x="-60%" y="-60%" width="220%" height="220%">
    <feGaussianBlur stdDeviation="2.2" result="b"/>
    <feMerge><feMergeNode in="b"/><feMergeNode in="SourceGraphic"/></feMerge>
  </filter>
  <filter id="warmGlow" x="-120%" y="-120%" width="340%" height="340%">
    <feGaussianBlur stdDeviation="4.5" result="b"/>
    <feMerge><feMergeNode in="b"/><feMergeNode in="SourceGraphic"/></feMerge>
  </filter>`;
svg.prepend(defs);

// ── build ──────────────────────────────────────────────────────────────

drawYard();
drawFloor("ground");
drawFloor("upper");
// dashed guides tying the exploded storeys back together
for (const [cx, cy] of [[0, 0], [10, 0], [0, 6.5], [10, 6.5]]) {
  const [x1s, y1s] = px(cx, cy, WALL_H + 0.15);
  const [x2s, y2s] = px(cx, cy, FLOORS[1].z - SLAB_T - 0.15);
  layers.upper.append(el("line", {
    x1: x1s, y1: y1s, x2: x2s, y2: y2s,
    stroke: "rgba(140,144,156,0.35)", "stroke-width": 1, "stroke-dasharray": "3 5",
  }));
}
drawFurniture();
drawLabels();

const state = {
  on: new Set(PLACEMENTS.map((p) => p.id)),
  selected: null,
  walking: false,
};

const fieldEls = {}, markerEls = {};
for (const p of PLACEMENTS) {
  const info = placementInfo(p);
  if (!info) continue;
  const room = roomOf(p.room);
  const layer = room.outside ? layers.fx : layers[room.floor];
  const f = fieldFor(p);
  const m = markerFor(p, info);
  // a soft pool of light under every sensing witness — the Hue trick
  // (real witnesses only; concepts don't get to light the floor yet)
  if (!p.teaser && p.sense !== "display" && p.sense !== "lora") {
    const w = worldOf(p);
    f.prepend(planRing(w.x, w.y, w.z - p.at.z, 1.7, { fill: "url(#poolWarm)" }, "pool"));
  }
  layer.append(f, m);
  fieldEls[p.id] = f;
  markerEls[p.id] = m;
  m.addEventListener("click", () => select(p.id));
  m.addEventListener("keydown", (e) => {
    if (e.key === "Enter" || e.key === " ") { e.preventDefault(); select(p.id); }
  });
}

// visitor
const person = el("g", { id: "visitor" }, "visitor hidden");
const personInner = el("g", {}, "visitor-inner");
personInner.append(
  el("path", { d: "M-4.5 2 q0 -9 4.5 -9 q4.5 0 4.5 9 z", fill: "#e8e8ec" }, "visitor-body"),
  el("circle", { cx: 0, cy: -10.5, r: 3.6, fill: "#e8e8ec" }, "visitor-head"),
);
person.append(
  el("ellipse", { cx: 0, cy: 3, rx: 7, ry: 3.2, fill: "rgba(0,0,0,0.55)" }, "visitor-shadow"),
  personInner,
);
layers.person.append(person);

// frame the scene
requestAnimationFrame(() => {
  const bb = world.getBBox();
  svg.setAttribute("viewBox",
    `${(bb.x - 16).toFixed(0)} ${(bb.y - 20).toFixed(0)} ${(bb.width + 32).toFixed(0)} ${(bb.height + 36).toFixed(0)}`);
});

// ── side panel: flock list, details, summary ───────────────────────────

const panel = document.getElementById("panel");
const feedList = document.getElementById("feed-list");
const GH = "https://github.com/kmay89/securaCV/blob/main/";

function statusChip(status) {
  if (status === "coming-soon") return html("span", "chip chip-soon", "coming soon · concept");
  const released = status === "released";
  return html("span", `chip ${released ? "chip-live" : "chip-dev"}`,
    released ? "released · print-validated" : "in development");
}

function renderPanel() {
  panel.innerHTML = "";
  if (state.selected) return renderDetails(PLACEMENTS.find((p) => p.id === state.selected));

  const s = flockSummary([...state.on]);
  const head = html("div", "flock-head");
  head.append(html("h2", null, "Your flock"));
  head.append(html("p", "flock-count",
    `${s.witnesses} witness${s.witnesses === 1 ? "" : "es"} · ${s.displays} display${s.displays === 1 ? "" : "s"}` +
    (s.infra ? ` · ${s.infra} relay` : "") +
    (s.soon ? ` · +${s.soon} coming soon` : "")));
  head.append(html("p", "flock-honest",
    s.total === 0 ? "Nothing perched yet — tap a marker or switch one on below." :
    `${s.released} released today, ${s.indev} in development — statuses never hidden.`));
  panel.append(head);

  for (const floorId of ["ground", "upper", "outside"]) {
    const perches = PLACEMENTS.filter((p) => {
      const r = roomOf(p.room);
      return floorId === "outside" ? r.outside : (!r.outside && r.floor === floorId);
    });
    if (!perches.length) continue;
    panel.append(html("h3", "flock-floor",
      floorId === "ground" ? "Ground floor" : floorId === "upper" ? "Upstairs" : "Outside"));
    for (const p of perches) {
      const info = placementInfo(p);
      const row = html("button", "perch" + (state.on.has(p.id) ? " on" : ""));
      row.setAttribute("aria-pressed", state.on.has(p.id));
      const dot = html("span", `perch-dot dot-${p.sense}`);
      const body = html("span", "perch-body");
      body.append(html("span", "perch-name", info.title));
      body.append(html("span", "perch-where", `${roomOf(p.room).label} · ${p.spot}`));
      row.append(dot, body);
      if (info.teaser) row.append(html("span", "perch-soon", "coming soon"));
      else if (info.status !== "released") row.append(html("span", "perch-dev", "in dev"));
      row.addEventListener("click", () => toggle(p.id));
      row.addEventListener("mouseenter", () => markerEls[p.id]?.classList.add("hint"));
      row.addEventListener("mouseleave", () => markerEls[p.id]?.classList.remove("hint"));
      panel.append(row);
    }
  }

  const doors = html("div", "flock-doors");
  const a1 = html("a", "primary small door", "refine in the chooser →");
  a1.href = "choose.html";
  const a2 = html("a", "door", "spec builds in the Workshop →");
  a2.href = "workshop.html";
  const a3 = html("a", "door", "meet them live →");
  a3.href = "index.html";
  doors.append(a1, a2, a3);
  panel.append(doors);
}

function renderDetails(p) {
  const info = placementInfo(p);
  const back = html("button", "back", "← whole flock");
  back.addEventListener("click", () => select(null));
  panel.append(back);

  const head = html("div", "det-head");
  head.append(html("h2", null, info.title));
  head.append(statusChip(info.status));
  panel.append(head);
  panel.append(html("p", "det-where", `${roomOf(p.room).label} — ${p.spot}`));
  panel.append(html("p", "det-headline", p.headline));

  const sense = html("div", "det-sense");
  sense.append(html("h4", null, `How it senses — ${info.sense.label}`));
  sense.append(html("p", null, info.sense.how));
  sense.append(html("h4", null, "What leaves the device"));
  sense.append(html("p", "det-emits", info.sense.emits));
  panel.append(sense);

  for (const n of info.notes) {
    const od = html("p", "ondevice");
    od.append(html("strong", null, "Honesty: "), document.createTextNode(n));
    panel.append(od);
  }

  const doors = html("div", "flock-doors");
  if (info.teaser) {
    const req = html("a", "primary small door", "→ request it (opens a GitHub issue)");
    req.href = "https://github.com/kmay89/securaCV/issues/new?title=" +
      encodeURIComponent(`Concept request: ${info.title} (Meshtastic fence guard)`) +
      "&body=" + encodeURIComponent(
        "Seen in the Canary House teaser. What I'd want from a chain-link-mounted, " +
        "solar-fed Meshtastic perimeter witness:\n\n- fence length / terrain:\n- mesh distance to the nearest node:\n- shade situation:\n");
    req.target = "_blank"; req.rel = "noopener";
    const rel = html("button", "door door-btn", "the relay it would mesh with →");
    rel.addEventListener("click", () => select("relay"));
    doors.append(req, rel);
    panel.append(doors);
    return;
  }
  const a1 = html("a", "primary small door", "find this one in the chooser →");
  a1.href = "choose.html" + chooserHash(p.answers);
  const a2 = html("a", "door", "meet it live →");
  a2.href = `index.html#${info.device}`;
  const a3 = html("a", "door", "spec it →");
  a3.href = `workshop.html#${info.device}`;
  const a4 = html("a", "door", "enclosure (.scad)");
  a4.href = GH + "docs/hardware/enclosure/" + (info.enclosure || "");
  a4.target = "_blank"; a4.rel = "noopener";
  doors.append(a1, a2, a3);
  if (info.enclosure) doors.append(a4);
  panel.append(doors);
}

function toggle(id) {
  state.on.has(id) ? state.on.delete(id) : state.on.add(id);
  applyVisibility();
  renderPanel();
}
function applyVisibility() {
  for (const p of PLACEMENTS) {
    const off = !state.on.has(p.id);
    fieldEls[p.id]?.classList.toggle("off", off);
    markerEls[p.id]?.classList.toggle("off", off);
  }
}
function select(id) {
  state.selected = state.selected === id ? null : id;
  for (const [pid, m] of Object.entries(markerEls)) {
    m.classList.toggle("sel", pid === state.selected);
    fieldEls[pid]?.classList.toggle("sel", pid === state.selected);
  }
  renderPanel();
}

// ── the walkthrough + witness feed ─────────────────────────────────────

function feed(text, tone) {
  const li = html("li", "feed-item" + (tone ? ` feed-${tone}` : ""));
  li.textContent = text;
  feedList.prepend(li);
  while (feedList.children.length > 6) feedList.lastChild.remove();
}

const PINGS = {
  camera: "person: yes · 0.93 · ✓ signed — no image left the device",
  wifi: "presence: yes · field disturbance · ✓ signed",
  radar: "presence: yes · range 2.1 m · ✓ signed",
  breath: "breathing: steady · 14/min · ✓ signed",
  display: "new witness event on the glass",
  lora: "relaying signed claims onward →",
};

function chipAt(p, text) {
  const w = worldOf(p);
  const [sx, sy] = px(w.x, w.y, w.z + 0.9);
  const pt = svg.createSVGPoint();
  pt.x = sx; pt.y = sy;
  const ctm = svg.getScreenCTM();
  if (!ctm) return;
  const sp = pt.matrixTransform(ctm);
  const host = stage.getBoundingClientRect();
  const chip = html("div", "event-chip", text);
  chip.style.left = `${sp.x - host.left}px`;
  chip.style.top = `${sp.y - host.top}px`;
  stage.append(chip);
  setTimeout(() => chip.remove(), 2600);
}

const walkBtn = document.getElementById("walk-btn");
let walkRaf = null;

function startWalk() {
  if (state.walking) return;
  state.walking = true;
  walkBtn.disabled = true;
  walkBtn.textContent = "…visitor walking";
  person.classList.remove("hidden");
  feedList.innerHTML = "";
  feed("visitor approaching — the flock is listening", "sys");

  const speed = matchMedia("(prefers-reduced-motion: reduce)").matches ? 5.5 : 1.7; // units/s
  const tripped = new Set();
  let seg = 0, t = 0, pauseLeft = 0, last = performance.now();

  const step = (now) => {
    const dt = Math.min((now - last) / 1000, 0.08);
    last = now;
    if (pauseLeft > 0) { pauseLeft -= dt; walkRaf = requestAnimationFrame(step); return; }
    const a = WALK[seg], b = WALK[seg + 1];
    if (!b) return endWalk();
    const len = Math.hypot(b.x - a.x, b.y - a.y);
    t += (dt * speed) / len;
    if (t >= 1) { t = 0; seg += 1; pauseLeft = b.pause || 0; }
    const cx = a.x + (b.x - a.x) * Math.min(t, 1);
    const cy = a.y + (b.y - a.y) * Math.min(t, 1);
    const [sx, sy] = px(cx, cy, 0.55);
    person.setAttribute("transform", `translate(${sx.toFixed(1)},${sy.toFixed(1)})`);

    for (const p of PLACEMENTS) {
      if (tripped.has(p.id) || !state.on.has(p.id)) continue;
      if (p.teaser) continue; // concepts don't witness — that would be theater
      const room = roomOf(p.room);
      if (!room.outside && room.floor !== "ground") continue; // visitor stays downstairs
      if (Math.hypot(p.at.x - cx, p.at.y - cy) <= p.range) {
        tripped.add(p.id);
        const info = placementInfo(p);
        markerEls[p.id].classList.add("tripped");
        setTimeout(() => markerEls[p.id].classList.remove("tripped"), 1800);
        chipAt(p, PINGS[p.sense]);
        feed(`${roomOf(p.room).label} · ${info.title} — ${PINGS[p.sense]}`,
          p.sense === "display" || p.sense === "lora" ? "show" : "witness");
      }
    }
    walkRaf = requestAnimationFrame(step);
  };
  walkRaf = requestAnimationFrame(step);
}

function endWalk() {
  cancelAnimationFrame(walkRaf);
  state.walking = false;
  person.classList.add("hidden");
  walkBtn.disabled = false;
  walkBtn.textContent = "▶ walk a visitor through";
  feed("walk complete — every claim above is the ENTIRE story the flock told. No pixels, no audio, no identities.", "sys");
}

walkBtn.addEventListener("click", startWalk);

renderPanel();
applyVisibility();
