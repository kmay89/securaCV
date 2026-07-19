// canary-local/assets/assembly.js — the assembly stage.
//
// A LEGO-instruction engine on top of the same zero-dependency WebGL viewer the
// board and enclosure labs use. It takes the REAL parts — printed enclosure
// STLs, the vendor board GLB, and procedurally-built fasteners/battery/magnet
// (simple shapes, sized from the BOM) — and choreographs them: an exploded view
// with leader lines, and a step-by-step build where each new part flies into
// place along its insertion axis with a little "click", the camera reframes the
// action, and the callout is the catalog's own assembly sentence.
//
// Honesty: the parts and the step text are real and drift-gated; the
// choreography (seated transforms, vectors, camera, which part per step) is
// authored in devices/assembly.json and validated to reference real files, the
// real README steps, and the real BOM fastener counts. Nothing here is a photo.

// ── column-major mat4 (matches scene3d) ─────────────────────────────────────
export const M = {
  id: () => new Float32Array([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]),
  mul(a, b) {
    const o = new Float32Array(16);
    for (let c = 0; c < 4; c++)
      for (let r = 0; r < 4; r++)
        o[c * 4 + r] = a[r] * b[c * 4] + a[4 + r] * b[c * 4 + 1] + a[8 + r] * b[c * 4 + 2] + a[12 + r] * b[c * 4 + 3];
    return o;
  },
  t(x, y, z) { const o = M.id(); o[12] = x; o[13] = y; o[14] = z; return o; },
  s(x, y = x, z = x) { const o = M.id(); o[0] = x; o[5] = y; o[10] = z; return o; },
  rx(a) { const c = Math.cos(a), s = Math.sin(a), o = M.id(); o[5] = c; o[6] = s; o[9] = -s; o[10] = c; return o; },
  ry(a) { const c = Math.cos(a), s = Math.sin(a), o = M.id(); o[0] = c; o[2] = -s; o[8] = s; o[10] = c; return o; },
  rz(a) { const c = Math.cos(a), s = Math.sin(a), o = M.id(); o[0] = c; o[1] = s; o[4] = -s; o[5] = c; return o; },
  // compose translate·rotX·rotY·rotZ·scale from a plain spec {pos,rot,scale}
  compose(sp = {}) {
    let m = M.id();
    const p = sp.pos || [0, 0, 0], r = sp.rot || [0, 0, 0], s = sp.scale;
    m = M.mul(M.t(p[0], p[1], p[2]), m);
    if (r[0]) m = M.mul(m, M.rx(r[0] * Math.PI / 180));
    if (r[1]) m = M.mul(m, M.ry(r[1] * Math.PI / 180));
    if (r[2]) m = M.mul(m, M.rz(r[2] * Math.PI / 180));
    if (s) m = M.mul(m, M.s(s[0], s[1], s[2]));
    return m;
  },
};

// ── tiny geometry kit for procedural parts ──────────────────────────────────
class MB {
  constructor() { this.pos = []; this.nrm = []; this.uv = []; this.idx = []; }
  v(p, n) { this.pos.push(p[0], p[1], p[2]); this.nrm.push(n[0], n[1], n[2]); this.uv.push(0, 0); return this.pos.length / 3 - 1; }
  quad(a, b, c, d) { this.idx.push(a, b, c, a, c, d); }
  tri(a, b, c) { this.idx.push(a, b, c); }
  out() { return { pos: this.pos, nrm: this.nrm, uv: this.uv, idx: this.idx }; }
}

// cylinder along +Z from z0..z1, centered on (cx,cy); optional caps
function cyl(m, cx, cy, r, z0, z1, seg = 28, capTop = true, capBot = true) {
  for (let i = 0; i < seg; i++) {
    const a0 = (i / seg) * Math.PI * 2, a1 = ((i + 1) / seg) * Math.PI * 2;
    const c0 = [Math.cos(a0), Math.sin(a0)], c1 = [Math.cos(a1), Math.sin(a1)];
    const p0 = [cx + r * c0[0], cy + r * c0[1]], p1 = [cx + r * c1[0], cy + r * c1[1]];
    m.quad(
      m.v([p0[0], p0[1], z1], [c0[0], c0[1], 0]), m.v([p0[0], p0[1], z0], [c0[0], c0[1], 0]),
      m.v([p1[0], p1[1], z0], [c1[0], c1[1], 0]), m.v([p1[0], p1[1], z1], [c1[0], c1[1], 0]));
    if (capTop) { const ctr = m.v([cx, cy, z1], [0, 0, 1]); m.tri(ctr, m.v([p0[0], p0[1], z1], [0, 0, 1]), m.v([p1[0], p1[1], z1], [0, 0, 1])); }
    if (capBot) { const ctr = m.v([cx, cy, z0], [0, 0, -1]); m.tri(ctr, m.v([p1[0], p1[1], z0], [0, 0, -1]), m.v([p0[0], p0[1], z0], [0, 0, -1])); }
  }
}
// cone (for countersunk head): r0 at z0 → r1 at z1
function cone(m, cx, cy, r0, r1, z0, z1, seg = 28) {
  for (let i = 0; i < seg; i++) {
    const a0 = (i / seg) * Math.PI * 2, a1 = ((i + 1) / seg) * Math.PI * 2;
    const c0 = [Math.cos(a0), Math.sin(a0)], c1 = [Math.cos(a1), Math.sin(a1)];
    const n0 = [c0[0], c0[1], (r0 - r1) / (z1 - z0)], n1 = [c1[0], c1[1], (r0 - r1) / (z1 - z0)];
    m.quad(
      m.v([cx + r1 * c0[0], cy + r1 * c0[1], z1], n0), m.v([cx + r0 * c0[0], cy + r0 * c0[1], z0], n0),
      m.v([cx + r0 * c1[0], cy + r0 * c1[1], z0], n1), m.v([cx + r1 * c1[0], cy + r1 * c1[1], z1], n1));
  }
}
// rounded box (a filled prism), z0..z1, half-extents hw,hh, corner r
function roundedBox(m, hw, hh, z0, z1, r = 1.2, seg = 4) {
  const pts = [];
  const cs = [[hw - r, hh - r, 0], [-(hw - r), hh - r, Math.PI / 2], [-(hw - r), -(hh - r), Math.PI], [hw - r, -(hh - r), 3 * Math.PI / 2]];
  for (const [cx, cy, a0] of cs) for (let i = 0; i <= seg; i++) { const a = a0 + i / seg * Math.PI / 2; pts.push([cx + r * Math.cos(a), cy + r * Math.sin(a)]); }
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

// ── procedural parts (mm; canonical frame, +Z up, seated by the transform) ──
// Each returns [{builder,color,gloss}]. Fasteners point their shaft down -Z so
// the "insert" animation drops them straight in.
const STEEL = [0.66, 0.68, 0.72], BRASS = [0.72, 0.58, 0.28], GOLD = [0.85, 0.72, 0.30];
const NICKEL = [0.80, 0.81, 0.84], BLACK = [0.10, 0.10, 0.12], WHITE = [0.90, 0.90, 0.88];
const LIPO = [0.20, 0.26, 0.52], RED = [0.75, 0.18, 0.15], CLEAR = [0.72, 0.82, 0.88];

export const PARTS = {
  // M2 machine/self-tap screw: head at z=0, shaft down to -len. csk = countersunk.
  screw({ len = 8, csk = false, color = STEEL } = {}) {
    const m = new MB();
    if (csk) { cone(m, 0, 0, 2.0, 1.0, -1.4, 0); cyl(m, 0, 0, 2.0, -0.2, 0, 24, true, false); }
    else { cyl(m, 0, 0, 2.0, 0, 1.5, 24); cyl(m, 0, 0, 0.5, 1.3, 1.9, 12); } // pan head + drive nub
    cyl(m, 0, 0, 1.0, -len, csk ? -1.0 : 0, 20, false, true); // shaft
    return [{ builder: m.out(), color, gloss: 0.55 }];
  },
  insert({ len = 4, color = BRASS } = {}) { const m = new MB(); cyl(m, 0, 0, 1.75, -len, 0, 22); return [{ builder: m.out(), color, gloss: 0.5 }]; },
  magnet({ d = 6, h = 2, color = NICKEL } = {}) { const m = new MB(); cyl(m, 0, 0, d / 2, 0, h, 32); return [{ builder: m.out(), color, gloss: 0.7 }]; },
  lightPipe({ d = 3, len = 8, color = CLEAR } = {}) { const m = new MB(); cyl(m, 0, 0, d / 2, 0, len, 20); return [{ builder: m.out(), color, gloss: 0.85 }]; },
  disc({ d = 12, t = 1, color = CLEAR } = {}) { const m = new MB(); cyl(m, 0, 0, d / 2, 0, t, 40); return [{ builder: m.out(), color, gloss: 0.9 }]; },
  vent({ d = 8, t = 1.2, color = BLACK } = {}) { const m = new MB(); cyl(m, 0, 0, d / 2, 0, t, 32); return [{ builder: m.out(), color, gloss: 0.4 }]; },
  // LiPo pouch + JST-PH connector + two short leads, laid flat (thin in Z)
  lipo({ w = 50, h = 34, t = 5, color = LIPO } = {}) {
    const body = new MB(); roundedBox(body, w / 2, h / 2, 0, t, 2);
    const con = new MB(); roundedBox(con, 3.0, 2.0, t * 0.2, t * 0.8, 0.6);
    const leadR = new MB(); roundedBox(leadR, 4, 0.5, t * 0.35, t * 0.55, 0.4);
    const leadB = new MB(); roundedBox(leadB, 4, 0.5, t * 0.55, t * 0.75, 0.4);
    // connector + leads sit off one short edge (+X)
    const off = M.t(w / 2 + 3, 0, 0), offR = M.t(w / 2 + 8, 1.2, 0), offB = M.t(w / 2 + 8, -1.2, 0);
    return [
      { builder: body.out(), color, gloss: 0.35 },
      { builder: con.out(), color: WHITE, gloss: 0.3, local: off },
      { builder: leadR.out(), color: RED, gloss: 0.3, local: offR },
      { builder: leadB.out(), color: BLACK, gloss: 0.3, local: offB },
    ];
  },
};

// ── easing ──────────────────────────────────────────────────────────────────
const easeInOut = (t) => (t < 0.5 ? 2 * t * t : 1 - Math.pow(-2 * t + 2, 2) / 2);
const easeOutBack = (t) => { const c1 = 1.70158, c3 = c1 + 1; return 1 + c3 * Math.pow(t - 1, 3) + c1 * Math.pow(t - 1, 2); };
const lerp = (a, b, t) => a + (b - a) * t;

// ── the assembly ────────────────────────────────────────────────────────────
export class Assembly {
  constructor(scene) {
    this.scene = scene;
    scene.autoSway = false;
    this.parts = [];
    this.leaders = [];      // scene meshes for leader lines
    this.explodeT = 0;
    this.mode = "explode";  // "explode" | "step"
    this.step = 0;
    this.anims = [];        // active tweens
    this._raf = null;
    this.onStep = null;     // callback(stepIndex)
  }

  // spec: { id, meshes:[{builder,color,gloss,local?}], seated (spec or mat4),
  //         explode:[x,y,z], insert:[x,y,z], step, name, qty, tip }
  add(spec) {
    const seated = spec.seated instanceof Float32Array ? spec.seated : M.compose(spec.seated);
    const part = {
      ...spec,
      seated,
      explode: spec.explode || [0, 0, 0],
      insert: spec.insert || [0, 0, 0],
      step: spec.step ?? 0,
      meshes: spec.meshes.map((mm) => ({
        sp: this.scene.addMesh(mm.builder, { color: mm.color, gloss: mm.gloss ?? 0.3, model: M.id() }),
        local: mm.local || M.id(),
      })),
      center: spec.center || [0, 0, 0],
    };
    this.parts.push(part);
    return part;
  }

  _apply(part, world) {
    for (const m of part.meshes) m.sp.model = M.mul(world, m.local);
  }
  _hide(part) { for (const m of part.meshes) m.sp.model = M.s(0, 0, 0); }

  // world transform for a part given explode t and an extra insertion offset
  _world(part, explodeT, insertF = 0) {
    const e = part.explode, i = part.insert;
    const dx = e[0] * explodeT + i[0] * insertF, dy = e[1] * explodeT + i[1] * insertF, dz = e[2] * explodeT + i[2] * insertF;
    return M.mul(M.t(dx, dy, dz), part.seated);
  }

  setExplode(t) {
    this.mode = "explode";
    this.explodeT = t;
    for (const p of this.parts) this._apply(p, this._world(p, t));
    this._rebuildLeaders(t);
  }

  _rebuildLeaders(t) {
    for (const l of this.leaders) this.scene.removePart(l);
    this.leaders = [];
    if (t < 0.02) return;
    const m = new MB();
    for (const p of this.parts) {
      const from = M.mul(this._world(p, 0), M.t(p.center[0], p.center[1], p.center[2]));
      const to = M.mul(this._world(p, t), M.t(p.center[0], p.center[1], p.center[2]));
      const a = [from[12], from[13], from[14]], b = [to[12], to[13], to[14]];
      // dashed segments
      const segs = 9;
      for (let s = 0; s < segs; s += 2) {
        const p0 = a.map((v, i) => lerp(v, b[i], s / segs)), p1 = a.map((v, i) => lerp(v, b[i], (s + 1) / segs));
        const i0 = m.v(p0, [0, 0, 1]), i1 = m.v(p1, [0, 0, 1]); m.idx.push(i0, i1);
      }
    }
    if (m.idx.length) this.leaders.push(this.scene.addMesh(m.out(), { color: [0.45, 0.5, 0.58], lines: true, unlit: true }));
  }

  // step player: reveal parts up to k, fly the step-k part(s) in
  showStep(k, { animate = true } = {}) {
    this.mode = "step";
    this.step = k;
    this._rebuildLeaders(0);
    this.anims = [];
    for (const p of this.parts) {
      if (p.step > k) { this._hide(p); continue; }
      if (p.step === k && animate) {
        this.anims.push({ part: p, kind: "insert", t0: performance.now(), dur: 620 });
        this._apply(p, this._world(p, 0, 1)); // start pulled out
      } else {
        this._apply(p, this._world(p, 0, 0)); // seated
      }
    }
    if (this.onStep) this.onStep(k);
  }

  tweenCamera(pose, dur = 560) {
    const s = this.scene;
    this.anims.push({ kind: "cam", t0: performance.now(), dur, from: { x: s.rot.x, y: s.rot.y, d: s.dist }, to: pose });
    this._camActive = true;
  }

  _frame() {
    const now = performance.now();
    this.anims = this.anims.filter((a) => {
      const t = Math.min(1, (now - a.t0) / a.dur), done = t >= 1;
      if (a.kind === "insert") {
        const f = 1 - easeOutBack(t); // insert offset 1→0 with a slight overshoot
        this._apply(a.part, this._world(a.part, 0, Math.max(0, f)));
      } else if (a.kind === "cam") {
        const e = easeInOut(t);
        this.scene.rot.x = lerp(a.from.x, a.to.x, e);
        this.scene.rot.y = lerp(a.from.y, a.to.y, e);
        this.scene.dist = lerp(a.from.d, a.to.d, e);
        if (done) this._camActive = false;
      }
      return !done;
    });
    this.scene.draw();
    this._raf = requestAnimationFrame(() => this._frame());
  }

  mount() { if (!this._raf) this._frame(); }
  unmount() { if (this._raf) cancelAnimationFrame(this._raf); this._raf = null; }
}
