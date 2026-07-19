// canary-local/assets/hub-parts.js — the Raspberry Pi, as an object.
//
// Procedural bodies for The Hub's assembly stage, in the same spirit as
// assembly.js's fastener kit: simple shapes at honest sizes. The board is
// the published Raspberry Pi 4B mechanical drawing (85 × 56 PCB, 3.5 mm
// hole inset, 58 × 49 hole grid); connector placement is representative,
// not a CAD import — the stage's honesty ribbon says so. The case is a
// deliberately generic two-part sketch: the guide is "any vented case",
// so the model refuses to impersonate a specific product.
//
// Every part builder returns [{ builder, color, gloss, local? }], the
// contract assembly-lab.js established for PARTS — The Hub's stage speaks
// the same language so the two guides feel like one product.

import { M } from "./assembly.js";

// ── tiny geometry kit (same MB idiom as assembly.js) ────────────────────
class MB {
  constructor() { this.pos = []; this.nrm = []; this.uv = []; this.idx = []; }
  v(p, n) { this.pos.push(p[0], p[1], p[2]); this.nrm.push(n[0], n[1], n[2]); this.uv.push(0, 0); return this.pos.length / 3 - 1; }
  quad(a, b, c, d) { this.idx.push(a, b, c, a, c, d); }
  tri(a, b, c) { this.idx.push(a, b, c); }
  out() { return { pos: this.pos, nrm: this.nrm, uv: this.uv, idx: this.idx }; }
}

function cyl(m, cx, cy, r, z0, z1, seg = 28, capTop = true, capBot = true) {
  const ctrTop = capTop ? m.v([cx, cy, z1], [0, 0, 1]) : -1;
  const ctrBot = capBot ? m.v([cx, cy, z0], [0, 0, -1]) : -1;
  for (let i = 0; i < seg; i++) {
    const a0 = (i / seg) * Math.PI * 2, a1 = ((i + 1) / seg) * Math.PI * 2;
    const c0 = [Math.cos(a0), Math.sin(a0)], c1 = [Math.cos(a1), Math.sin(a1)];
    const p0 = [cx + r * c0[0], cy + r * c0[1]], p1 = [cx + r * c1[0], cy + r * c1[1]];
    m.quad(
      m.v([p0[0], p0[1], z1], [c0[0], c0[1], 0]), m.v([p0[0], p0[1], z0], [c0[0], c0[1], 0]),
      m.v([p1[0], p1[1], z0], [c1[0], c1[1], 0]), m.v([p1[0], p1[1], z1], [c1[0], c1[1], 0]));
    if (capTop) m.tri(ctrTop, m.v([p0[0], p0[1], z1], [0, 0, 1]), m.v([p1[0], p1[1], z1], [0, 0, 1]));
    if (capBot) m.tri(ctrBot, m.v([p1[0], p1[1], z0], [0, 0, -1]), m.v([p0[0], p0[1], z0], [0, 0, -1]));
  }
}

// axis-aligned rounded prism centered on (0,0), z0..z1 (r=0 → plain box)
function rbox(m, hw, hh, z0, z1, r = 1.2, seg = 4) {
  const pts = [];
  if (r <= 0) { pts.push([hw, hh], [-hw, hh], [-hw, -hh], [hw, -hh]); }
  else {
    const cs = [[hw - r, hh - r, 0], [-(hw - r), hh - r, Math.PI / 2], [-(hw - r), -(hh - r), Math.PI], [hw - r, -(hh - r), 3 * Math.PI / 2]];
    for (const [cx, cy, a0] of cs) for (let i = 0; i <= seg; i++) { const a = a0 + i / seg * Math.PI / 2; pts.push([cx + r * Math.cos(a), cy + r * Math.sin(a)]); }
  }
  const n = pts.length;
  for (let i = 0; i < n; i++) {
    const [x, y] = pts[i], [x2, y2] = pts[(i + 1) % n];
    const nx = y2 - y, ny = -(x2 - x), l = Math.hypot(nx, ny) || 1;
    m.quad(m.v([x, y, z1], [nx / l, ny / l, 0]), m.v([x, y, z0], [nx / l, ny / l, 0]), m.v([x2, y2, z0], [nx / l, ny / l, 0]), m.v([x2, y2, z1], [nx / l, ny / l, 0]));
  }
  for (const z of [z1, z0]) {
    const nz = z === z1 ? 1 : -1, ctr = m.v([0, 0, z], [0, 0, nz]);
    const ring = pts.map(([x, y]) => m.v([x, y, z], [0, 0, nz]));
    for (let i = 0; i < n; i++) { const a = ring[i], b = ring[(i + 1) % n]; nz > 0 ? m.tri(ctr, a, b) : m.tri(ctr, b, a); }
  }
}

const box = (hw, hh, z0, z1, r = 0) => { const m = new MB(); rbox(m, hw, hh, z0, z1, r); return m.out(); };

// ── palette ─────────────────────────────────────────────────────────────
const PCB = [0.05, 0.32, 0.17];        // solder-mask green
const METAL = [0.74, 0.76, 0.79];      // shielded connector cans
const BLACK = [0.09, 0.09, 0.1];       // header/SoC plastic
const CHIP = [0.14, 0.14, 0.16];       // silicon packages
const GOLD = [0.82, 0.68, 0.28];       // pin field hint
const SHELL = [0.17, 0.18, 0.2];       // matte printed shell (graphite)
const SHELL_IN = [0.13, 0.14, 0.16];
const CABLE = [0.16, 0.16, 0.18];
const WHITE = [0.88, 0.88, 0.86];
const CANARY = [1.0, 0.83, 0.31];

// ── the parts ───────────────────────────────────────────────────────────
export const PARTS = {
  // Raspberry Pi 4 Model B. PCB 85 × 56 × 1.4, z = 0..1.4 board-local;
  // components up (+Z), microSD slot underside at the -X edge.
  piBoard() {
    const parts = [];
    const pcb = new MB(); rbox(pcb, 42.5, 28, 0, 1.4, 3, 5);
    parts.push({ builder: pcb.out(), color: PCB, gloss: 0.38 });
    const at = (x, y, z = 1.4) => M.t(x, y, z);
    // ethernet jack + double USB stacks along the +X edge
    parts.push({ builder: box(10.5, 8, 0, 13.5, 0.6), color: METAL, gloss: 0.6, local: at(35, -18) });
    parts.push({ builder: box(8.5, 6.5, 0, 13, 0.6), color: METAL, gloss: 0.6, local: at(37, 0) });
    parts.push({ builder: box(8.5, 6.5, 0, 13, 0.6), color: METAL, gloss: 0.6, local: at(37, 18) });
    // 40-pin GPIO header along the +Y edge: black body + gold pin field
    parts.push({ builder: box(25.5, 2.5, 0, 8.5, 0.4), color: BLACK, gloss: 0.25, local: at(-10, 24.8) });
    parts.push({ builder: box(24.8, 1.8, 8.5, 9.1, 0.3), color: GOLD, gloss: 0.7, local: at(-10, 24.8) });
    // SoC + RAM
    parts.push({ builder: box(7.5, 7.5, 0, 2.4, 0.8), color: CHIP, gloss: 0.55, local: at(-6.5, 4.5) });
    parts.push({ builder: box(5, 7.5, 0, 1.6, 0.8), color: CHIP, gloss: 0.5, local: at(13, 4.5) });
    // USB-C power + 2× micro-HDMI along the -Y edge
    parts.push({ builder: box(4.5, 3.6, 0, 3.2, 0.8), color: METAL, gloss: 0.65, local: at(-30, -26.2) });
    parts.push({ builder: box(3.6, 3.4, 0, 3, 0.6), color: METAL, gloss: 0.6, local: at(-18, -26.3) });
    parts.push({ builder: box(3.6, 3.4, 0, 3, 0.6), color: METAL, gloss: 0.6, local: at(-6, -26.3) });
    // microSD slot can, underside at the -X edge
    parts.push({ builder: box(6.5, 7, -1.6, 0, 0.4), color: METAL, gloss: 0.5, local: at(-38, 0, 0) });
    return parts;
  },

  // microSD card: 15 × 11 × 1, black body, label face up
  microSd() {
    return [
      { builder: box(7.5, 5.5, 0, 1, 0.6), color: BLACK, gloss: 0.35 },
      { builder: box(5.6, 4.2, 1, 1.15, 0.3), color: WHITE, gloss: 0.25 },
      { builder: box(6.2, 4.6, -0.12, 0, 0.2), color: GOLD, gloss: 0.7 },
    ];
  },

  // finned heatsink for the SoC, 15 × 15 base
  heatsink() {
    const parts = [{ builder: box(7.5, 7.5, 0, 2, 0.6), color: METAL, gloss: 0.5 }];
    for (let i = 0; i < 4; i++)
      parts.push({ builder: box(1.1, 7.5, 2, 7, 0.3), color: METAL, gloss: 0.5, local: M.t(-5.4 + i * 3.6, 0, 0) });
    return parts;
  },

  // generic vented two-part case, bottom: floor + walls + board posts
  caseBase() {
    const parts = [];
    const floor = new MB(); rbox(floor, 47, 31.5, -2, 0, 6, 6);
    parts.push({ builder: floor.out(), color: SHELL, gloss: 0.2 });
    // walls (plain slabs; corners overlap — a sketch, not a print file)
    parts.push({ builder: box(1.4, 31, 0, 18), color: SHELL, gloss: 0.2, local: M.t(-45.6, 0, 0) });
    parts.push({ builder: box(1.4, 31, 0, 18), color: SHELL, gloss: 0.2, local: M.t(45.6, 0, 0) });
    parts.push({ builder: box(44.2, 1.4, 0, 18), color: SHELL, gloss: 0.2, local: M.t(0, 30.1, 0) });
    parts.push({ builder: box(44.2, 1.4, 0, 18), color: SHELL, gloss: 0.2, local: M.t(0, -30.1, 0) });
    // board posts at the Pi's real hole grid (58 × 49, 3.5 mm inset)
    for (const [x, y] of [[-39, -24.5], [-39, 24.5], [19, -24.5], [19, 24.5]]) {
      const post = new MB(); cyl(post, 0, 0, 2.4, 0, 4.4, 20);
      parts.push({ builder: post.out(), color: SHELL_IN, gloss: 0.18, local: M.t(x, y, 0) });
    }
    return parts;
  },

  // the lid: slab + inner lip + a small canary dot, because it's family
  caseLid() {
    return [
      { builder: box(47, 31.5, 0, 2.2, 6), color: SHELL, gloss: 0.22 },
      { builder: box(43.5, 28.5, -1.4, 0, 4), color: SHELL_IN, gloss: 0.18 },
      (() => { const m = new MB(); cyl(m, 0, 0, 2.2, 2.2, 2.7, 24); return { builder: m.out(), color: CANARY, gloss: 0.6, local: M.t(34, -20, 0) }; })(),
    ];
  },

  // RJ45 plug + cable stub, pointing -X into the jack
  ethPlug() {
    const boot = new MB(); cyl(boot, 0, 0, 3, 0, 7, 22);
    const cable = new MB(); cyl(cable, 0, 0, 2.2, 0, 16, 18);
    return [
      { builder: box(8, 6, 0, 9, 0.8), color: [0.72, 0.78, 0.83], gloss: 0.75 },
      { builder: boot.out(), color: CABLE, gloss: 0.3, local: M.mul(M.t(8, 0, 4.5), M.ry(90 * Math.PI / 180)) },
      { builder: cable.out(), color: CABLE, gloss: 0.3, local: M.mul(M.t(14, 0, 4.5), M.ry(90 * Math.PI / 180)) },
    ];
  },

  // USB-C plug + cable stub, pointing +Y into the power port
  psuPlug() {
    const cable = new MB(); cyl(cable, 0, 0, 1.8, 0, 16, 18);
    return [
      { builder: box(5, 6.5, 0, 3.4, 1.4), color: BLACK, gloss: 0.4 },
      { builder: box(3.9, 1.6, 0.6, 2.8, 0.7), color: METAL, gloss: 0.7, local: M.t(0, 7.2, 0) },
      { builder: cable.out(), color: CABLE, gloss: 0.3, local: M.mul(M.t(0, -6.5, 1.7), M.rx(90 * Math.PI / 180)) },
    ];
  },
};
