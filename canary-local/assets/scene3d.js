// canary-local/assets/scene3d.js — the device as an object, not a photo.
//
// A deliberately small WebGL renderer (no three.js, no CDN — this repo
// ships pages that work with the ethernet cable unplugged). The display
// line draws its committed fleet-figure model (models/<figure>.glb, massed
// from the CAD — see "the display line, from the fleet figures" below), the
// witnesses a procedural body until real-shapes.js swaps in their STLs, and
// the glass is textured with the LIVE emulator framebuffer — so the 3D card
// is not an illustration of the device, it IS the device, running.
//
// Lighting is a physically-based studio: GGX softboxes (warm key, cool
// window fill, rim strip) with area-light lobe widening, hemisphere
// bounce, a graded environment for reflections, real fresnel, linear
// light through an ACES-style curve — plus chamfered edges for the
// highlights to catch on and a view-space contact shadow that grounds
// the floating product. Tuned for the Apple-product-page look while
// staying a single zero-dependency file.
//
// GPU lifecycle (2026-09-27): a page holds ONE WebGL context, however many
// DeviceScenes it mounts. Every scene renders through a shared context on
// a detached canvas and blits its frame into its own <canvas> (a 2D
// surface), so fleet.html's 28 cards plus a sheet no longer ask Chromium
// for 29 contexts — it caps live contexts near 16 and loses the oldest,
// which blanked the first cards. A scene retains each part's source
// arrays beside its GL buffers: start() gates the frame loop on an
// IntersectionObserver (plus document visibility), an off-screen card
// stops its loop and sheds its buffers (_releaseGL), and the next draw
// re-uploads them (_acquireGL). A lost context (webglcontextlost is
// preventDefault-ed, restore is asked for) rebuilds programs, textures and
// buffers from the same retained data on webglcontextrestored, so an
// eviction never leaves a blank card. The public surface is unchanged:
// new DeviceScene(canvas, src), addMesh/clearParts/removePart, start/stop,
// draw, onTick, project, scene.gl (the shared context); dispose() is new.
// A scene is in the page's set only while started or drawing; stop() sheds
// its GPU objects and leaves the set, so a stop()-only teardown leaks nothing.
// Shader compile/link failures still throw from the first constructor on
// the page (tests/render_probe.mjs relies on it). Between passes no vertex
// attribute stays enabled — an enabled array whose buffer a later
// clearParts() deleted was the "drawArrays: no buffer is bound to enabled
// attribute" the shadow quad tripped on after a real-shape swap.

import { activeFinish, finishColor } from "./finishes.js";
import { parseGLB } from "./glb.js";
import { deviceFigure } from "./body-dims.js";

// ── tiny mat4 ───────────────────────────────────────────────────────────
export const M4 = {
  ident: () => new Float32Array([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]),
  mul(a, b) {
    const o = new Float32Array(16);
    for (let c = 0; c < 4; c++)
      for (let r = 0; r < 4; r++)
        o[c * 4 + r] =
          a[r] * b[c * 4] + a[4 + r] * b[c * 4 + 1] +
          a[8 + r] * b[c * 4 + 2] + a[12 + r] * b[c * 4 + 3];
    return o;
  },
  persp(fovY, aspect, near, far) {
    const f = 1 / Math.tan(fovY / 2);
    const o = new Float32Array(16);
    o[0] = f / aspect; o[5] = f;
    o[10] = (far + near) / (near - far); o[11] = -1;
    o[14] = (2 * far * near) / (near - far);
    return o;
  },
  translate(x, y, z) {
    const o = M4.ident();
    o[12] = x; o[13] = y; o[14] = z;
    return o;
  },
  rotX(a) {
    const c = Math.cos(a), s = Math.sin(a), o = M4.ident();
    o[5] = c; o[6] = s; o[9] = -s; o[10] = c;
    return o;
  },
  rotY(a) {
    const c = Math.cos(a), s = Math.sin(a), o = M4.ident();
    o[0] = c; o[2] = -s; o[8] = s; o[10] = c;
    return o;
  },
  rotZ(a) {
    const c = Math.cos(a), s = Math.sin(a), o = M4.ident();
    o[0] = c; o[1] = s; o[4] = -s; o[5] = c;
    return o;
  },
  scale(x, y, z) {
    const o = M4.ident();
    o[0] = x; o[5] = y; o[10] = z;
    return o;
  },
};

// ── geometry builders (positions + normals + uv, indexed) ───────────────
class MeshBuilder {
  constructor() {
    this.pos = [];
    this.nrm = [];
    this.uv = [];
    this.idx = [];
  }
  vert(p, n, uv = [0, 0]) {
    this.pos.push(...p);
    this.nrm.push(...n);
    this.uv.push(...uv);
    return this.pos.length / 3 - 1;
  }
  quad(a, b, c, d) {
    this.idx.push(a, b, c, a, c, d);
  }
  tri(a, b, c) {
    this.idx.push(a, b, c);
  }
}

// Rounded-rectangle prism (the dash shell): outline sampled with rounded
// corners, extruded ±h/2 in Z — with real 45° chamfers where wall meets
// cap, because a highlight needs an edge to catch on. bevel: 0 restores
// the old sharp box (print-exact silhouettes).
export function roundedBox(w, h, d, r, seg = 6, bevel) {
  const m = new MeshBuilder();
  const hw = w / 2, hh = h / 2, hd = d / 2;
  const b = bevel === undefined ? Math.min(1.1, d * 0.16, Math.max(r * 0.9, 0.3)) : bevel;
  const outline = (rr) => {
    const pts = [];
    const corners = [
      [hw - r, hh - r, 0], [-(hw - r), hh - r, Math.PI / 2],
      [-(hw - r), -(hh - r), Math.PI], [hw - r, -(hh - r), (3 * Math.PI) / 2],
    ];
    for (const [cx, cy, a0] of corners)
      for (let i = 0; i <= seg; i++) {
        const a = a0 + (i / seg) * (Math.PI / 2);
        pts.push([cx + rr * Math.cos(a), cy + rr * Math.sin(a), Math.cos(a), Math.sin(a)]);
      }
    return pts;
  };
  const outer = outline(r);                                  // wall outline (+2D normal)
  const inner = b > 0 ? outline(Math.max(r - b, 0.02)) : outer; // cap outline, inset
  const n = outer.length;
  const zWall = b > 0 ? hd - b : hd;
  // side wall
  for (let i = 0; i < n; i++) {
    const [x, y] = outer[i];
    const [x2, y2] = outer[(i + 1) % n];
    const nx = (y2 - y), ny = -(x2 - x);
    const len = Math.hypot(nx, ny) || 1;
    const a = m.vert([x, y, zWall], [nx / len, ny / len, 0]);
    const bb = m.vert([x, y, -zWall], [nx / len, ny / len, 0]);
    const c = m.vert([x2, y2, -zWall], [nx / len, ny / len, 0]);
    const dd = m.vert([x2, y2, zWall], [nx / len, ny / len, 0]);
    m.quad(a, bb, c, dd);
  }
  // chamfer rings (45°: outline normal tipped toward the cap)
  if (b > 0) {
    for (const s of [1, -1]) {
      for (let i = 0; i < n; i++) {
        const [ox, oy, cx, cy] = outer[i];
        const [ox2, oy2, cx2, cy2] = outer[(i + 1) % n];
        const [ix, iy] = inner[i];
        const [ix2, iy2] = inner[(i + 1) % n];
        const nrm = (cx_, cy_) => {
          const l = Math.hypot(cx_, cy_, 1) || 1;
          return [cx_ / l, cy_ / l, s / l];
        };
        const a = m.vert([ox, oy, s * zWall], nrm(cx, cy));
        const bb = m.vert([ix, iy, s * hd], nrm(cx, cy));
        const c = m.vert([ix2, iy2, s * hd], nrm(cx2, cy2));
        const dd = m.vert([ox2, oy2, s * zWall], nrm(cx2, cy2));
        if (s > 0) m.quad(a, bb, c, dd);
        else m.quad(dd, c, bb, a);
      }
    }
  }
  // front + back caps (fan from center over the inset outline)
  for (const z of [hd, -hd]) {
    const nz = z > 0 ? 1 : -1;
    const center = m.vert([0, 0, z], [0, 0, nz]);
    const ring = inner.map(([x, y]) => m.vert([x, y, z], [0, 0, nz]));
    for (let i = 0; i < n; i++) {
      const a = ring[i], bb = ring[(i + 1) % n];
      if (nz > 0) m.tri(center, a, bb);
      else m.tri(center, bb, a);
    }
  }
  return m;
}

// Cylinder along Z (the watch drum / bezel), optional inner bore → ring.
// Outer rims carry a 45° chamfer (bevel: 0 restores sharp rims).
export function cylinder(rOut, depth, seg = 64, rIn = 0, bevel) {
  const m = new MeshBuilder();
  const hd = depth / 2;
  const b = bevel === undefined
    ? Math.min(0.9, depth * 0.16, Math.max(rOut - rIn, rOut) * 0.12)
    : bevel;
  const zWall = b > 0 ? hd - b : hd;
  const rCap = b > 0 ? rOut - b : rOut;
  for (let i = 0; i < seg; i++) {
    const a0 = (i / seg) * Math.PI * 2;
    const a1 = ((i + 1) / seg) * Math.PI * 2;
    const c0 = [Math.cos(a0), Math.sin(a0)], c1 = [Math.cos(a1), Math.sin(a1)];
    // outer wall
    {
      const a = m.vert([rOut * c0[0], rOut * c0[1], zWall], [c0[0], c0[1], 0]);
      const b2 = m.vert([rOut * c0[0], rOut * c0[1], -zWall], [c0[0], c0[1], 0]);
      const c = m.vert([rOut * c1[0], rOut * c1[1], -zWall], [c1[0], c1[1], 0]);
      const d = m.vert([rOut * c1[0], rOut * c1[1], zWall], [c1[0], c1[1], 0]);
      m.quad(a, b2, c, d);
    }
    // rim chamfers
    if (b > 0) {
      for (const s of [1, -1]) {
        const nrm = (c) => {
          const l = Math.SQRT2;
          return [c[0] / l, c[1] / l, s / l];
        };
        const a = m.vert([rOut * c0[0], rOut * c0[1], s * zWall], nrm(c0));
        const b2 = m.vert([rCap * c0[0], rCap * c0[1], s * hd], nrm(c0));
        const c = m.vert([rCap * c1[0], rCap * c1[1], s * hd], nrm(c1));
        const d = m.vert([rOut * c1[0], rOut * c1[1], s * zWall], nrm(c1));
        if (s > 0) m.quad(a, b2, c, d);
        else m.quad(d, c, b2, a);
      }
    }
    if (rIn > 0) {
      // inner wall (bore)
      const a = m.vert([rIn * c0[0], rIn * c0[1], hd], [-c0[0], -c0[1], 0]);
      const b = m.vert([rIn * c1[0], rIn * c1[1], hd], [-c1[0], -c1[1], 0]);
      const c = m.vert([rIn * c1[0], rIn * c1[1], -hd], [-c1[0], -c1[1], 0]);
      const d = m.vert([rIn * c0[0], rIn * c0[1], -hd], [-c0[0], -c0[1], 0]);
      m.quad(a, b, c, d);
    }
    // caps (out to the chamfer's inner edge)
    for (const z of [hd, -hd]) {
      const nz = z > 0 ? 1 : -1;
      if (rIn > 0) {
        const a = m.vert([rIn * c0[0], rIn * c0[1], z], [0, 0, nz]);
        const b2 = m.vert([rCap * c0[0], rCap * c0[1], z], [0, 0, nz]);
        const c = m.vert([rCap * c1[0], rCap * c1[1], z], [0, 0, nz]);
        const d = m.vert([rIn * c1[0], rIn * c1[1], z], [0, 0, nz]);
        if (nz > 0) m.quad(a, b2, c, d);
        else m.quad(d, c, b2, a);
      } else {
        const ctr = m.vert([0, 0, z], [0, 0, nz]);
        const a = m.vert([rCap * c0[0], rCap * c0[1], z], [0, 0, nz]);
        const b2 = m.vert([rCap * c1[0], rCap * c1[1], z], [0, 0, nz]);
        if (nz > 0) m.tri(ctr, a, b2);
        else m.tri(ctr, b2, a);
      }
    }
  }
  return m;
}

// Screen plane with UVs (rect or disc), facing +Z.
export function screenPlane(w, h, round, seg = 64) {
  const m = new MeshBuilder();
  if (!round) {
    const a = m.vert([-w / 2, h / 2, 0], [0, 0, 1], [0, 0]);
    const b = m.vert([-w / 2, -h / 2, 0], [0, 0, 1], [0, 1]);
    const c = m.vert([w / 2, -h / 2, 0], [0, 0, 1], [1, 1]);
    const d = m.vert([w / 2, h / 2, 0], [0, 0, 1], [1, 0]);
    m.quad(a, b, c, d);
  } else {
    const r = w / 2;
    const ctr = m.vert([0, 0, 0], [0, 0, 1], [0.5, 0.5]);
    const ring = [];
    for (let i = 0; i <= seg; i++) {
      const a = (i / seg) * Math.PI * 2;
      ring.push(
        m.vert([r * Math.cos(a), r * Math.sin(a), 0], [0, 0, 1],
               [0.5 + 0.5 * Math.cos(a), 0.5 - 0.5 * Math.sin(a)])
      );
    }
    for (let i = 0; i < seg; i++) m.tri(ctr, ring[i], ring[i + 1]);
  }
  return m;
}

// ── the glass turns, so the device does (sweep A47) ─────────────────────
// A display's glass can turn: the dash worn portrait (480x800 from its
// 800x480 panel, in software) or the nightlight stood on its edge (320x180
// from its 180x320 panel, in hardware). The emulator's canvas is the glass
// as the person in front of it reads it, so it turns too (F184, F204); the
// 3D model's screen plane is the panel as it sits in the case. Textured as
// is, a turned canvas would stretch across the unturned plane. So when the
// canvas's long side disagrees with the panel's own (the glass turned), the
// model turns a quarter turn clockwise as the viewer sees it — the way both
// firmwares' turn 1 stands the device (lv_disp_rot_t 90 on the dash,
// Orient::R90 on the nightlight) — and the plane samples the canvas turned a
// quarter turn the other way, so the glass reads upright on the turned body.
// The canvas cannot tell turn 1 from turn 3 (both read upright), so a device
// worn the other way shows the same upright glass on a body turned the same
// way. The panel's own shape is the registry card's glass (app.js hands it
// over with the canvas): a glass that never turns never turns its model.

/** Quarter turns clockwise the model takes for this glass: 1 when the
 * canvas (`src`: {width, height}) is tall and the panel it shows
 * (`panel`: {w, h}, its native scan) wide, or the reverse; 0 otherwise (a
 * square or round glass, an unturned glass, or nothing to compare). */
export function glassTurn(src, panel) {
  if (!src || !panel) return 0;
  const tall = src.height > src.width, wide = src.width > src.height;
  return (tall && panel.w > panel.h) || (wide && panel.h > panel.w) ? 1 : 0;
}

/** The model's own turn for `turn` quarter turns clockwise (about the
 * viewer's axis, +Z toward them). */
export function turnModel(turn) {
  return turn ? M4.rotZ(-Math.PI / 2) : M4.ident();
}

/** The texture a turned plane samples, from a `w` x `h` canvas: its size and
 * the 2D transform (setTransform's a, b, c, d, e, f) that draws the canvas
 * into it a quarter turn counterclockwise — canvas (x, y) at texture
 * (y, w - x) — so that on the plane turned clockwise it reads upright. */
export function turnTexture(turn, w, h) {
  return turn ? { w: h, h: w, m: [0, -1, 1, 0, 0, w] } : { w, h, m: [1, 0, 0, 1, 0, 0] };
}

// Wedge stand (25° recline cradle, simplified silhouette of the printed
// part): a triangular prism under the device.
export function wedge(wid, dep, hgt) {
  const m = new MeshBuilder();
  const hw = wid / 2;
  // five faces of a right triangular prism, apex at back-top
  const A = [-hw, 0, dep / 2], B = [hw, 0, dep / 2];
  const C = [hw, 0, -dep / 2], D = [-hw, 0, -dep / 2];
  const E = [-hw, hgt, -dep / 2], F = [hw, hgt, -dep / 2];
  const slopeN = normalOf(A, B, F);
  m.quad(m.vert(A, [0, -1, 0]), m.vert(B, [0, -1, 0]), m.vert(C, [0, -1, 0]), m.vert(D, [0, -1, 0]));
  m.quad(m.vert(A, slopeN), m.vert(E, slopeN), m.vert(F, slopeN), m.vert(B, slopeN));
  m.quad(m.vert(D, [0, 0, -1]), m.vert(C, [0, 0, -1]), m.vert(F, [0, 0, -1]), m.vert(E, [0, 0, -1]));
  m.tri(m.vert(A, [-1, 0, 0]), m.vert(D, [-1, 0, 0]), m.vert(E, [-1, 0, 0]));
  m.tri(m.vert(B, [1, 0, 0]), m.vert(F, [1, 0, 0]), m.vert(C, [1, 0, 0]));
  return m;
}

function normalOf(a, b, c) {
  const u = [b[0] - a[0], b[1] - a[1], b[2] - a[2]];
  const v = [c[0] - a[0], c[1] - a[1], c[2] - a[2]];
  const n = [
    u[1] * v[2] - u[2] * v[1],
    u[2] * v[0] - u[0] * v[2],
    u[0] * v[1] - u[1] * v[0],
  ];
  const l = Math.hypot(...n) || 1;
  return n.map((x) => x / l);
}

// ── shaders ─────────────────────────────────────────────────────────────
const VS = `
attribute vec3 aPos; attribute vec3 aNrm; attribute vec2 aUv;
uniform mat4 uProj, uView, uModel;
varying vec3 vN; varying vec3 vP; varying vec2 vUv;
varying vec3 vObj; varying vec3 vNl;
void main() {
  vec4 wp = uModel * vec4(aPos, 1.0);
  vP = wp.xyz;
  vN = mat3(uModel) * aNrm;
  vObj = aPos;   // part-local (print) space: z rises off the build plate
  vNl = aNrm;    // local normal — overhang math is view-independent
  vUv = aUv;
  gl_Position = uProj * uView * wp;
}`;

// Physically-based studio shading. The rig is a photo studio, not a math
// demo: a large warm key softbox up-left, a tall cool fill window right,
// a rim strip behind, hemisphere bounce, and a graded environment for
// reflections — all analytic, evaluated in linear light and graded
// through an ACES-style filmic curve. Area lights are approximated by
// widening GGX roughness with each source's angular radius: the cheap
// trick that makes highlights read as *softboxes*, not points.
const FS = `
#ifdef GL_OES_standard_derivatives
#extension GL_OES_standard_derivatives : enable
#endif
#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif
varying vec3 vN; varying vec3 vP; varying vec2 vUv;
varying vec3 vObj; varying vec3 vNl;
uniform vec3 uColor;
uniform float uGloss;      // 0 matte shell .. 1 glass (legacy knob → roughness/F0)
uniform float uMetal;      // 0 dielectric .. 1 metal
uniform float uUseTex;     // screen face samples the live framebuffer
uniform float uEmissive;   // screen glow (backlight level)
uniform float uClipZ;      // print guide: hide everything above this layer
uniform float uMinZ;       // part's plate level (local z)
uniform float uOverhangOn; // tint faces steeper than 45° pointing down
uniform float uUnlit;      // plate grid / layer contours: flat color
uniform sampler2D uTex;

vec3 srgb2lin(vec3 c) { return pow(c, vec3(2.2)); }
vec3 aces(vec3 x) {
  return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}
float dGGX(float NoH, float a) {
  float a2 = a * a;
  float d = NoH * NoH * (a2 - 1.0) + 1.0;
  return a2 / max(3.14159 * d * d, 1e-4);
}
float vSmith(float NoV, float NoL, float a) {
  float k = a * 0.5 + 1e-3;
  return 0.25 / max((NoV * (1.0 - k) + k) * (NoL * (1.0 - k) + k), 1e-4);
}
vec3 fresnel(float u, vec3 f0) { return f0 + (1.0 - f0) * pow(1.0 - u, 5.0); }

// one softbox: direction, linear color·intensity, angular radius
vec3 softbox(vec3 N, vec3 V, vec3 L, vec3 tint, float radius, float rough, vec3 f0, vec3 albedo) {
  vec3 H = normalize(L + V);
  float NoL = dot(N, L);
  float wrap = clamp((NoL + radius) / (1.0 + radius), 0.0, 1.0); // area wrap
  if (wrap <= 0.0) return vec3(0.0);
  float a = clamp(rough + radius * 0.85, 0.03, 1.0);             // source size widens lobe
  float NoV = max(dot(N, V), 1e-3);
  float spec = dGGX(max(dot(N, H), 0.0), a) * vSmith(NoV, max(NoL, 1e-3), a);
  vec3 F = fresnel(max(dot(H, V), 0.0), f0);
  vec3 diff = albedo * (1.0 - F) * (1.0 - uMetal);
  return (diff + spec * F) * tint * wrap;
}

// graded studio environment for reflections: bright soft ceiling, cool
// horizon band, falling to a dark floor — what glossy shells "see"
vec3 envLight(vec3 R) {
  float h = clamp(R.y * 0.5 + 0.5, 0.0, 1.0);
  vec3 floorC = vec3(0.030, 0.032, 0.036);
  vec3 horizon = vec3(0.16, 0.18, 0.21);
  vec3 ceil = vec3(0.95, 0.97, 1.02);
  vec3 env = mix(floorC, horizon, smoothstep(0.0, 0.55, h));
  env = mix(env, ceil, smoothstep(0.55, 1.0, h) * smoothstep(0.55, 1.0, h));
  // the key softbox itself, visible in sharp reflections
  float box = smoothstep(0.93, 0.995, dot(R, normalize(vec3(-0.35, 0.85, 0.45))));
  return env + box * vec3(1.4);
}

void main() {
  if (vObj.z - uMinZ > uClipZ) discard;
  if (uUnlit > 0.5) { gl_FragColor = vec4(uColor, 1.0); return; }
  vec3 N = normalize(vN);
  vec3 V = normalize(-vP);
  float NoV = max(dot(N, V), 1e-3);

  // legacy gloss knob → PBR params
  float rough = clamp(1.0 - uGloss, 0.06, 1.0);
  rough *= rough; // perceptual → alpha-ish
#ifdef GL_OES_standard_derivatives
  // specular AA: fine geometry (bevels, STL facets) shimmers unless the
  // lobe widens with normal variance
  vec3 dx = dFdx(N), dy = dFdy(N);
  rough = clamp(rough + (dot(dx, dx) + dot(dy, dy)) * 0.8, 0.06, 1.0);
#endif

  if (uUseTex > 0.5) {
    // the glass path: live framebuffer under real fresnel + studio streak
    vec3 tex = srgb2lin(texture2D(uTex, vUv).rgb);
    vec3 emit = tex * (0.06 + 1.35 * uEmissive);
    vec3 R = reflect(-V, N);
    vec3 f0 = vec3(0.045);
    vec3 F = fresnel(NoV, f0);
    vec3 refl = envLight(R) * F * 1.1;
    vec3 col = aces(emit + refl);
    gl_FragColor = vec4(pow(col, vec3(1.0 / 2.2)), 1.0);
    return;
  }

  vec3 albedo = srgb2lin(uColor);
  vec3 f0 = mix(vec3(0.04 + 0.03 * uGloss), albedo, uMetal);

  // the rig (linear light)
  vec3 col = vec3(0.0);
  col += softbox(N, V, normalize(vec3(-0.38, 0.80, 0.46)), vec3(1.50, 1.45, 1.36), 0.34, rough, f0, albedo); // warm key
  col += softbox(N, V, normalize(vec3(0.78, 0.22, 0.42)),  vec3(0.34, 0.38, 0.47), 0.22, rough, f0, albedo); // cool window fill
  col += softbox(N, V, normalize(vec3(0.25, 0.35, -0.90)), vec3(0.36, 0.40, 0.48), 0.16, rough, f0, albedo); // rim strip
  // hemisphere bounce (sky / warm floor card)
  float hemi = N.y * 0.5 + 0.5;
  vec3 irr = mix(vec3(0.10, 0.093, 0.085), vec3(0.235, 0.25, 0.28), hemi);
  col += albedo * irr * (1.0 - uMetal * 0.85);
  // environment reflection, fresnel-weighted, stronger when glossy
  vec3 R = reflect(-V, N);
  vec3 F = fresnel(NoV, f0);
  col += envLight(R) * F * mix(0.10, 0.85, uGloss) * (1.0 - rough * 0.6);

  // Overhang guide: local faces steeper than 45° pointing at the plate,
  // above the first layers, would need support in this orientation.
  if (uOverhangOn > 0.5 && normalize(vNl).z < -0.707 && vObj.z > uMinZ + 0.45) {
    col = mix(col, srgb2lin(vec3(0.92, 0.28, 0.2)), 0.7);
  }
  col = aces(col);
  gl_FragColor = vec4(pow(col, vec3(1.0 / 2.2)), 1.0);
}`;

// the contact shadow: a soft ellipse on a view-space ground plane — the
// "floating product photo" grounding. Squared falloff, darker core.
const SHADOW_VS = `
attribute vec2 aPos;
uniform mat4 uProj;
uniform vec3 uCenter;   // view-space center of the ellipse
uniform vec2 uRadii;    // x/z radii (view units)
varying vec2 vQ;
void main() {
  vQ = aPos;
  vec3 p = uCenter + vec3(aPos.x * uRadii.x, 0.0, aPos.y * uRadii.y);
  gl_Position = uProj * vec4(p, 1.0);
}`;
const SHADOW_FS = `
precision mediump float;
varying vec2 vQ;
uniform float uAlpha;
void main() {
  float d = length(vQ);
  float a = uAlpha * pow(clamp(1.0 - d, 0.0, 1.0), 1.8);
  a += uAlpha * 0.55 * pow(clamp(1.0 - d * 2.6, 0.0, 1.0), 2.0); // dense core
  // premultiplied output — the canvas composites premultiplied alpha
  gl_FragColor = vec4(vec3(0.02, 0.025, 0.035) * a, a);
}`;

// ── the GPU: one shared context, every scene a viewport in it ───────────
// The Lab mounts a DeviceScene per card (fleet.html: one per registry
// device, plus the open sheet). Each used to own a WebGL context; Chromium
// keeps ~16 alive and silently loses the oldest, so the first cards went
// blank. Now every scene on a page renders through ONE context on a
// detached canvas — its frame lands in the card's own <canvas> through a
// 2D blit — so the page holds one context for any number of viewers.
// A scene retains its part data, so its buffers can be shed while it is
// off-screen and re-uploaded on the way back, and a lost context (a GPU
// reset, an eviction by some other library's contexts) rebuilds from the
// same retained data when it is restored.
let sharedGPU = null;

class SharedGPU {
  constructor() {
    this.canvas = null;
    this.gl = null;
    this.lost = false;
    this.gen = 0;           // bumped whenever every GL object became invalid
    this.scenes = new Set();
    this.software = false;
    this._loseExt = null;
    this._restoreAt = 0;
  }

  static get() {
    if (!sharedGPU) sharedGPU = new SharedGPU();
    return sharedGPU;
  }

  /** The live context, creating it (and compiling the shaders — loudly:
   * a compile/link failure throws) on first use; null while it is lost. */
  ensure() {
    if (!this.gl) this._acquire();
    return this.lost ? null : this.gl;
  }

  _acquire() {
    const canvas = document.createElement("canvas");
    const gl = canvas.getContext("webgl", {
      antialias: true,
      alpha: true,
      premultipliedAlpha: true,
    });
    if (!gl) throw new Error("scene3d: WebGL is not available");
    canvas.addEventListener("webglcontextlost", (e) => {
      e.preventDefault();       // ask the browser to restore it
      this.lost = true;
      this._restoreAt = 0;
    });
    canvas.addEventListener("webglcontextrestored", () => {
      this.lost = false;
      this.gen++;               // every buffer, texture and program is gone
      this._build();            // throws loudly if the shaders no longer compile
    });
    this.canvas = canvas;
    this.gl = gl;
    this.lost = false;
    this.gen++;
    this._loseExt = gl.getExtension("WEBGL_lose_context");
    try {
      const dbg = gl.getExtension("WEBGL_debug_renderer_info");
      const renderer = dbg ? gl.getParameter(dbg.UNMASKED_RENDERER_WEBGL) : "";
      this.software = /swiftshader|llvmpipe|software|angle \(google/i.test(renderer);
    } catch { this.software = false; }
    this._build();
  }

  // programs, locations, the shadow quad — everything that is not per-scene
  _build() {
    const gl = this.gl;
    // specular anti-aliasing needs screen-space normal derivatives
    gl.getExtension("OES_standard_derivatives");
    this.prog = this._program(VS, FS);
    this.u = {};
    for (const n of ["uProj", "uView", "uModel", "uColor", "uGloss", "uMetal", "uUseTex",
                     "uEmissive", "uTex", "uClipZ", "uMinZ", "uOverhangOn", "uUnlit"])
      this.u[n] = gl.getUniformLocation(this.prog, n);
    this.a = {
      pos: gl.getAttribLocation(this.prog, "aPos"),
      nrm: gl.getAttribLocation(this.prog, "aNrm"),
      uv: gl.getAttribLocation(this.prog, "aUv"),
    };
    // contact-shadow pass (own tiny program + unit quad)
    this.sprog = this._program(SHADOW_VS, SHADOW_FS);
    this.su = {
      proj: gl.getUniformLocation(this.sprog, "uProj"),
      center: gl.getUniformLocation(this.sprog, "uCenter"),
      radii: gl.getUniformLocation(this.sprog, "uRadii"),
      alpha: gl.getUniformLocation(this.sprog, "uAlpha"),
    };
    this.sa = gl.getAttribLocation(this.sprog, "aPos");
    this.squad = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, this.squad);
    gl.bufferData(gl.ARRAY_BUFFER,
      new Float32Array([-1, -1, 1, -1, 1, 1, -1, -1, 1, 1, -1, 1]), gl.STATIC_DRAW);
  }

  _program(vs, fs) {
    const gl = this.gl;
    const mk = (type, srcCode) => {
      const s = gl.createShader(type);
      gl.shaderSource(s, srcCode);
      gl.compileShader(s);
      if (!gl.getShaderParameter(s, gl.COMPILE_STATUS))
        throw new Error(gl.getShaderInfoLog(s));
      return s;
    };
    const p = gl.createProgram();
    gl.attachShader(p, mk(gl.VERTEX_SHADER, vs));
    gl.attachShader(p, mk(gl.FRAGMENT_SHADER, fs));
    gl.linkProgram(p);
    if (!gl.getProgramParameter(p, gl.LINK_STATUS))
      throw new Error(gl.getProgramInfoLog(p));
    return p;
  }

  /** Lost and not coming back on its own: ask for it, at most once a second. */
  tryRestore() {
    const now = Date.now();
    if (!this.lost || !this._loseExt || now - this._restoreAt < 1000) return;
    this._restoreAt = now;
    try { this._loseExt.restoreContext(); } catch { /* the browser will say no */ }
  }

  /** Grow the backing store to hold a W×H frame (never shrinks: a resize
   * reallocates the drawing buffer, and cards of many sizes share it). */
  fit(W, H) {
    const c = this.canvas;
    if (c.width < W || c.height < H) {
      c.width = Math.max(c.width, W);
      c.height = Math.max(c.height, H);
    }
  }

  /** The last scene is gone: hand the context back to the browser. The next
   * scene starts from a fresh canvas (a canvas keeps its one context for
   * life, lost or not, so the old one is dropped rather than reused). */
  release() {
    const gl = this.gl;
    if (!gl) return;
    if (!this.lost) {
      try {
        gl.deleteBuffer(this.squad);
        gl.deleteProgram(this.prog);
        gl.deleteProgram(this.sprog);
        if (this._loseExt) this._loseExt.loseContext();
      } catch { /* already gone */ }
    }
    this.canvas = null;
    this.gl = null;
    this.lost = false;
    this._loseExt = null;
    this.prog = this.sprog = this.squad = null;
  }
}

// ── scene ───────────────────────────────────────────────────────────────
export class DeviceScene {
  /**
   * @param canvas 3D canvas
   * @param screenSource <canvas> the emulator draws into (or null)
   */
  constructor(canvas, screenSource) {
    this.canvas = canvas;
    this.src = screenSource;
    this._gpu = SharedGPU.get();
    this._gpu.ensure();            // first scene on the page: throws on shader failure
    // NOT registered yet: a scene joins the page's set when it starts its
    // loop or draws, and leaves it when it stops — so a card torn down with
    // stop() (the Board, Assemble, Enclosure, Hub and Board Room tabs) holds
    // nothing on the GPU and nothing holds it (Codex on #1737)
    // the card shows the shared context's frame through its own 2D surface
    this.ctx2d = canvas.getContext("2d");
    this.shadow = null; // {y, rx, rz, alpha} in world units, or null
    this.overhangOn = false;
    this.clipZ = 1e9;
    this.viewY = 0; // vertical look-at offset (plate scenes sit above y=0)
    this.parts = [];
    // bumped by every clearParts(): an async build (a figure model still in
    // flight) checks it before adding parts, so a later build that already
    // owns the scene — a real-shape upgrade, a rebuild — is never doubled
    this.buildGen = 0;
    this.rot = { x: -0.28, y: 0.55 }; // presentation pose
    this.home = { x: -0.28, y: 0.55 };
    this.vel = { x: 0, y: 0 };
    this.t = 0;
    this.dist = 150;
    this.glow = 1;
    this.dirtySerial = -1;
    this._tex = null;    // { tex, gen }: the live-screen texture, per context generation
    // The live glass's own panel ({w, h}, its native scan: app.js sets it
    // with the canvas) and the quarter turns the model takes when the
    // canvas has turned from it (A47).
    this.glass = null;
    this.turn = 0;
    this._turned = null; // the turned canvas a turned plane samples
    this._wireOrbit();
    this._raf = null;
    // lifecycle: the loop runs only while start() was called AND the card is
    // in (or near) the viewport AND the page is visible — off-screen cards
    // cost nothing and hold no GPU memory
    this._running = false;
    this._inView = true;       // until an IntersectionObserver says otherwise
    this._pageVisible = typeof document === "undefined" || document.visibilityState !== "hidden";
    this._io = null;
    this._onVis = null;
    // Adaptive resolution: the studio shading is real work, and software
    // rasterizers (headless CI, weak iGPUs) pay for every fragment. Track
    // an EMA of frame time and scale the backing store down until the
    // scene is fluid again — sharpness costs nothing on a real GPU and
    // fluidity beats sharpness everywhere else. A software renderer is
    // known at birth, so it starts cheap instead of discovering it.
    this.resScale = this._gpu.software ? 0.4 : 1;
    this._ft = 16;
    this._lastT = 0;
    this._cool = 0;
  }

  /** The shared WebGL context this scene draws through (null while lost). */
  get gl() { return this._gpu.lost ? null : this._gpu.gl; }

  /** How many contexts and loops the page holds — what a probe asserts on. */
  static stats() {
    const g = sharedGPU;
    let running = 0;
    if (g) for (const s of g.scenes) if (s._raf) running++;
    return {
      contexts: g && g.gl && !g.lost ? 1 : 0,
      scenes: g ? g.scenes.size : 0,
      running,
    };
  }

  // The floating-product grounding: a soft ellipse below the object, in
  // view space (the object spins; its shadow shouldn't). Opt-in per scene.
  setContactShadow({ y = -30, rx = 40, rz = 30, alpha = 0.34 } = {}) {
    this.shadow = { y, rx, rz, alpha };
  }
  clearContactShadow() { this.shadow = null; }

  addMesh(builder, { color = [0.5, 0.5, 0.5], gloss = 0.2, metal = 0, screen = false,
                     model = M4.ident(), lines = false, unlit = false,
                     clippable = false, minZ = 0, role = null } = {}) {
    const pos = builder.pos instanceof Float32Array ? builder.pos : new Float32Array(builder.pos);
    const nv = pos.length / 3;
    // every enabled attribute must read a buffer big enough for the draw: a
    // mesh that carries no normals or uvs (an STL, a line list) gets zeros,
    // never an empty buffer under an enabled attribute
    const fill = (arr, per) => {
      const want = nv * per;
      if (arr && arr.length >= want) return arr instanceof Float32Array ? arr : new Float32Array(arr);
      const out = new Float32Array(want);
      if (arr && arr.length) out.set(arr instanceof Float32Array ? arr : new Float32Array(arr));
      return out;
    };
    const part = {
      model,
      color,
      // role-tagged filament parts ("shell"/"shell2"/"gasket"/"beacon") read
      // their live color from the active finish each frame, so a finish swap
      // or the ambient showcase cross-fades them with no geometry rebuild.
      role,
      gloss,
      metal,
      screen,
      lines,
      unlit,
      clippable,
      minZ,
      count: builder.idx.length,
      // retained source data: the buffers below are re-uploaded from these
      // after a release (off-screen) or a context loss
      src: {
        pos,
        nrm: fill(builder.nrm, 3),
        uv: fill(builder.uv, 2),
        idx: builder.idx instanceof Uint16Array ? builder.idx : new Uint16Array(builder.idx),
      },
      vbo: null,
      nbo: null,
      ubo: null,
      ibo: null,
      gen: -1,   // the context generation the buffers belong to
    };
    this.parts.push(part);
    return part;
  }

  _upload(part) {
    const gl = this._gpu.gl;
    part.vbo = gl.createBuffer();
    part.nbo = gl.createBuffer();
    part.ubo = gl.createBuffer();
    part.ibo = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, part.vbo);
    gl.bufferData(gl.ARRAY_BUFFER, part.src.pos, gl.STATIC_DRAW);
    gl.bindBuffer(gl.ARRAY_BUFFER, part.nbo);
    gl.bufferData(gl.ARRAY_BUFFER, part.src.nrm, gl.STATIC_DRAW);
    gl.bindBuffer(gl.ARRAY_BUFFER, part.ubo);
    gl.bufferData(gl.ARRAY_BUFFER, part.src.uv, gl.STATIC_DRAW);
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, part.ibo);
    gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, part.src.idx, gl.STATIC_DRAW);
    part.gen = this._gpu.gen;
  }

  _free(part) {
    const g = this._gpu;
    if (part.gen === g.gen && g.gl && !g.lost) {
      for (const b of ["vbo", "nbo", "ubo", "ibo"]) g.gl.deleteBuffer(part[b]);
    }
    part.vbo = part.nbo = part.ubo = part.ibo = null;
    part.gen = -1;
  }

  /** Make sure this scene's GPU objects exist on the live context — the
   * context itself, and every retained part's buffers. */
  _acquireGL() {
    const gl = this._gpu.ensure();
    if (!gl) return null;
    for (const p of this.parts) if (p.gen !== this._gpu.gen) this._upload(p);
    if (!this._tex || this._tex.gen !== this._gpu.gen) {
      const tex = gl.createTexture();
      gl.bindTexture(gl.TEXTURE_2D, tex);
      gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, 1, 1, 0, gl.RGBA, gl.UNSIGNED_BYTE,
                    new Uint8Array([0, 0, 0, 255]));
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
      this._tex = { tex, gen: this._gpu.gen };
    }
    return gl;
  }

  /** Shed this scene's GPU objects (the parts stay, retained; the next draw
   * re-uploads them). The context itself is shared and stays until the last
   * scene is disposed. */
  _releaseGL() {
    for (const p of this.parts) this._free(p);
    const g = this._gpu;
    if (this._tex && this._tex.gen === g.gen && g.gl && !g.lost) g.gl.deleteTexture(this._tex.tex);
    this._tex = null;
  }

  clearParts() {
    this.buildGen++;
    for (const p of this.parts) this._free(p);
    this.parts = [];
  }

  setGlow(g) { this.glow = g; }

  _wireOrbit() {
    const cv = this.canvas;
    // multi-pointer: one finger orbits, two fingers pinch-zoom
    const active = new Map(); // pointerId → {x, y}
    let lx = 0, ly = 0, pinchD = 0;
    const pinchDist = () => {
      const [a, b] = [...active.values()];
      return Math.hypot(a.x - b.x, a.y - b.y);
    };
    cv.addEventListener("pointerdown", (e) => {
      active.set(e.pointerId, { x: e.clientX, y: e.clientY });
      cv.setPointerCapture(e.pointerId);
      if (active.size === 1) { lx = e.clientX; ly = e.clientY; }
      if (active.size === 2) pinchD = pinchDist();
    });
    cv.addEventListener("pointermove", (e) => {
      if (!active.has(e.pointerId)) return;
      active.set(e.pointerId, { x: e.clientX, y: e.clientY });
      if (active.size >= 2) {
        // exactly two pinch; a third finger parks the gesture (no jitter)
        if (active.size === 2) {
          const d = pinchDist();
          if (pinchD > 0 && d > 0) {
            this.dist = Math.min(2000, Math.max(30, this.dist * (pinchD / d)));
          }
          pinchD = d;
        }
        return;
      }
      const dx = e.clientX - lx, dy = e.clientY - ly;
      lx = e.clientX; ly = e.clientY;
      this.rot.y += dx * 0.008;
      this.rot.x += dy * 0.006;
      this.rot.x = Math.max(-1.2, Math.min(0.7, this.rot.x));
      this.vel = { x: 0, y: dx * 0.0009 }; // fling inertia
    });
    const end = (e) => {
      active.delete(e.pointerId);
      // returning from pinch to one finger: re-anchor the orbit
      if (active.size === 1) {
        const p = [...active.values()][0];
        lx = p.x; ly = p.y;
      }
      pinchD = 0;
    };
    cv.addEventListener("pointerup", end);
    cv.addEventListener("pointercancel", end);
    // mobile browsers can seize a captured pointer (scroll/gesture
    // takeover) without firing pointerup — drop it or a later single
    // finger reads as a phantom pinch
    cv.addEventListener("lostpointercapture", end);
    cv.addEventListener("wheel", (e) => {
      e.preventDefault();
      this.dist = Math.min(2000, Math.max(30, this.dist * Math.exp(e.deltaY * 0.0011)));
    }, { passive: false });
  }

  /** Run the frame loop — while the card is on screen. Idempotent. */
  start() {
    if (this._running) return;
    this._running = true;
    this._gpu.scenes.add(this);
    this._watch();
    this._syncLoop();
  }
  /** Pause the loop and give back every GPU object this scene holds (its
   * parts stay retained, so start() or a hand-driven draw() re-uploads
   * them), and leave the page's set: a card torn down with stop() holds
   * nothing on the GPU and nothing holds it. The page's one context stays
   * (a hand-driven draw() right after stop() must still be synchronous —
   * a lost context comes back only asynchronously); dispose() is what
   * gives it back when the last scene goes. */
  stop() {
    this._running = false;
    this._unwatch();
    this._syncLoop();
    this._releaseGL();
    this._gpu.scenes.delete(this);
  }
  /** stop(), drop the retained parts, and — when this was the last scene
   * on the page — release the context: a disposed scene is done. */
  dispose() {
    this.stop();
    this.parts = [];
    this.buildGen++;
    if (this._gpu.scenes.size === 0) this._gpu.release();
  }

  _watch() {
    if (this._io || typeof IntersectionObserver === "undefined") return;
    this._io = new IntersectionObserver((entries) => {
      for (const e of entries) {
        if (e.target !== this.canvas) continue;
        this._inView = e.isIntersecting;
        if (!this._inView) this._releaseGL(); // off-screen: hold nothing on the GPU
      }
      this._syncLoop();
    }, { rootMargin: "25%" });
    this._io.observe(this.canvas);
    this._onVis = () => {
      this._pageVisible = document.visibilityState !== "hidden";
      this._syncLoop();
    };
    document.addEventListener("visibilitychange", this._onVis);
  }
  _unwatch() {
    if (this._io) { this._io.disconnect(); this._io = null; }
    if (this._onVis) { document.removeEventListener("visibilitychange", this._onVis); this._onVis = null; }
    this._inView = true;
  }
  _syncLoop() {
    const want = this._running && this._inView && this._pageVisible;
    if (want && !this._raf) {
      const step = () => {
        this._raf = requestAnimationFrame(step);
        if (this.onTick) this.onTick(); // per-frame hook: cable rigs, LEDs, prop animation
        this.draw();
      };
      step();
    } else if (!want && this._raf) {
      cancelAnimationFrame(this._raf);
      this._raf = null;
    }
  }

  draw() {
    this._gpu.scenes.add(this);    // a hand-driven scene counts while it draws
    const gl = this._acquireGL();
    if (!gl) { this._gpu.tryRestore(); return; } // context lost: skip the frame, ask for it back
    const gpu = this._gpu;
    const now = (typeof performance !== "undefined" ? performance.now() : 0);
    if (this._lastT) {
      this._ft += (Math.min(now - this._lastT, 100) - this._ft) * 0.1;
      if (this._cool-- <= 0) {
        if (this._ft > 30 && this.resScale > 0.3) {
          this.resScale = Math.max(0.3, this.resScale * 0.75); // shed pixels fast
          this._cool = 10;
        } else if (this._ft < 17.5 && this.resScale < 1) {
          this.resScale = Math.min(1, this.resScale / 0.9);    // recover slowly
          this._cool = 90;
        }
      }
    }
    this._lastT = now;
    const dpr = Math.min(2, window.devicePixelRatio || 1) * this.resScale;
    const W = Math.max(2, Math.round(this.canvas.clientWidth * dpr));
    const H = Math.max(2, Math.round(this.canvas.clientHeight * dpr));
    if (this.canvas.width !== W || this.canvas.height !== H) {
      this.canvas.width = W;
      this.canvas.height = H;
    }
    gpu.fit(W, H);
    gl.viewport(0, 0, W, H);
    gl.clearColor(0, 0, 0, 0);
    gl.enable(gl.DEPTH_TEST);
    gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);

    // motion: fling inertia decays, then the object breathes around its
    // presentation pose (a pairing card floats; it doesn't turn its back)
    this.t += 1 / 60;
    this.rot.y += this.vel.y;
    this.vel.y *= 0.97;
    if (Math.abs(this.vel.y) < 0.0004) {
      this.vel.y = 0;
      // Idle "breathing" sway for the floating pairing cards. The assembly
      // stage sets autoSway = false so it can hold an exact per-step pose.
      if (this.autoSway !== false) {
        const sway = this.home.y + Math.sin(this.t * 0.5) * 0.22;
        const bob = this.home.x + Math.sin(this.t * 0.35 + 1.3) * 0.05;
        this.rot.y += (sway - this.rot.y) * 0.02;
        this.rot.x += (bob - this.rot.x) * 0.02;
      }
    }

    // live screen texture — turned with the glass when the canvas's shape
    // disagrees with the model's screen (A47: the model turns to match)
    const tex = this._tex.tex;
    this.turn = glassTurn(this.src, this.glass);
    if (this.src) {
      gl.bindTexture(gl.TEXTURE_2D, tex);
      try {
        gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE,
                      this.turn ? this._turnedSource() : this.src);
      } catch { /* canvas not ready yet */ }
    }

    const proj = M4.persp(0.62, W / H, 5, 2000);
    const view = M4.translate(0, -this.viewY, -this.dist);
    const spin = this._spin();
    const { u, a, su, sa } = gpu;

    // the grounding shadow, under everything, blended, no depth write
    if (this.shadow) {
      gl.useProgram(gpu.sprog);
      gl.enable(gl.BLEND);
      gl.blendFunc(gl.ONE, gl.ONE_MINUS_SRC_ALPHA); // premultiplied
      gl.depthMask(false);
      gl.uniformMatrix4fv(su.proj, false, proj);
      gl.uniform3f(su.center, 0, this.shadow.y - this.viewY, -this.dist);
      gl.uniform2f(su.radii, this.shadow.rx, this.shadow.rz);
      gl.uniform1f(su.alpha, this.shadow.alpha);
      gl.bindBuffer(gl.ARRAY_BUFFER, gpu.squad);
      gl.vertexAttribPointer(sa, 2, gl.FLOAT, false, 0, 0);
      gl.enableVertexAttribArray(sa);
      gl.drawArrays(gl.TRIANGLES, 0, 6);
      gl.disableVertexAttribArray(sa);
      gl.depthMask(true);
      gl.disable(gl.BLEND);
    }

    gl.useProgram(gpu.prog);
    gl.uniformMatrix4fv(u.uProj, false, proj);
    gl.uniformMatrix4fv(u.uView, false, view);
    gl.uniform1i(u.uTex, 0);
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, tex);
    gl.enableVertexAttribArray(a.pos);
    gl.enableVertexAttribArray(a.nrm);
    gl.enableVertexAttribArray(a.uv);

    for (const p of this.parts) {
      gl.uniformMatrix4fv(u.uModel, false, M4.mul(spin, p.model));
      gl.uniform3fv(u.uColor, (p.role && finishColor(p.role)) || p.color);
      gl.uniform1f(u.uGloss, p.gloss);
      gl.uniform1f(u.uMetal, p.metal || 0);
      gl.uniform1f(u.uUseTex, p.screen ? 1 : 0);
      gl.uniform1f(u.uEmissive, this.glow);
      gl.uniform1f(u.uClipZ, p.clippable ? this.clipZ : 1e9);
      gl.uniform1f(u.uMinZ, p.minZ || 0);
      gl.uniform1f(u.uOverhangOn, this.overhangOn && p.clippable ? 1 : 0);
      gl.uniform1f(u.uUnlit, p.unlit ? 1 : 0);
      gl.bindBuffer(gl.ARRAY_BUFFER, p.vbo);
      gl.vertexAttribPointer(a.pos, 3, gl.FLOAT, false, 0, 0);
      gl.bindBuffer(gl.ARRAY_BUFFER, p.nbo);
      gl.vertexAttribPointer(a.nrm, 3, gl.FLOAT, false, 0, 0);
      gl.bindBuffer(gl.ARRAY_BUFFER, p.ubo);
      gl.vertexAttribPointer(a.uv, 2, gl.FLOAT, false, 0, 0);
      gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, p.ibo);
      gl.drawElements(p.lines ? gl.LINES : gl.TRIANGLES, p.count,
                      gl.UNSIGNED_SHORT, 0);
    }
    // leave no attribute enabled between passes: an enabled array whose
    // buffer a later clearParts() deletes is exactly the "no buffer is bound
    // to enabled attribute" the shadow quad's drawArrays used to trip on
    gl.disableVertexAttribArray(a.pos);
    gl.disableVertexAttribArray(a.nrm);
    gl.disableVertexAttribArray(a.uv);

    // the frame is in the shared context's bottom-left W×H; the card shows it
    // (same task, so the drawing buffer is still intact)
    const c2 = this.ctx2d;
    if (c2) {
      c2.clearRect(0, 0, W, H);
      c2.drawImage(gpu.canvas, 0, gpu.canvas.height - H, W, H, 0, 0, W, H);
    }
  }

  // World-space point (caller applies any part/model transform first) → CSS
  // pixel position on this canvas, using the SAME camera as the draw loop
  // (persp 0.62 / view translate / orbit spin). The Board Room hangs its HTML
  // pin flags and wire labels on this; depth is camera-space distance so
  // callers can fade or stack flags front-to-back.
  project(x, y, z) {
    const W = this.canvas.clientWidth || 1, H = this.canvas.clientHeight || 1;
    const proj = M4.persp(0.62, W / H, 5, 2000);
    const spin = this._spin();
    const wx = spin[0] * x + spin[4] * y + spin[8] * z;
    const wy = spin[1] * x + spin[5] * y + spin[9] * z;
    const wz = spin[2] * x + spin[6] * y + spin[10] * z;
    const vx = wx, vy = wy - this.viewY, vz = wz - this.dist;
    const cw = -vz; // proj[11] = -1
    if (cw <= 1e-3) return { x: -1e4, y: -1e4, depth: Infinity, visible: false };
    const cx = proj[0] * vx, cy = proj[5] * vy;
    return {
      x: (cx / cw * 0.5 + 0.5) * W,
      y: (0.5 - cy / cw * 0.5) * H,
      depth: cw,
      visible: true,
    };
  }

  // The orbit pose, then the model's own turn for its glass (A47).
  _spin() {
    return M4.mul(M4.mul(M4.rotX(this.rot.x), M4.rotY(this.rot.y)), turnModel(this.turn));
  }

  // The live canvas drawn a quarter turn counterclockwise into a canvas of
  // its own (turnTexture), for the turned plane to sample.
  _turnedSource() {
    const t = turnTexture(this.turn, this.src.width, this.src.height);
    if (!this._turned) this._turned = document.createElement("canvas");
    const c = this._turned;
    if (c.width !== t.w || c.height !== t.h) { c.width = t.w; c.height = t.h; }
    const g = c.getContext("2d");
    g.setTransform(...t.m);
    g.drawImage(this.src, 0, 0);
    return c;
  }

  removePart(part) {
    const i = this.parts.indexOf(part);
    if (i < 0) return;
    this._free(part);
    this.parts.splice(i, 1);
  }
}

// ── device bodies (dimensions: docs/hardware/enclosure/*.scad) ──────────
// Printed-shell colors come from the active finish (finishes.js); only the
// functional non-filament parts (glass, lens, solar, radome) stay literal.

// ── the display line, from the fleet figures ────────────────────────────
// The displays have no committed STLs, so their cards used to be hand
// meshes: a Watch drum typed from the v0.1 CAD (Ø52 × 21.8, a size the
// measured board could never seat), a Dash box typed as 113.7 × 73.6 × 16
// that had lost its corner lobes, its back and its dock pads — and three
// nightstand boards BORROWED that Dash mesh while two more fell through to
// the WAP's witness box. The fleet figures already carry every one of them,
// massed from the CAD (the Watch and the Dash are measured off it,
// gen_assembled_dims.py) or from their panel records, and honest about
// which (figures.json `dims_source`: a sketch stays a sketch, a prototype
// stays a prototype); tools/figures/gen_device_glbs.mjs commits each as a
// small plain GLB. The cards read those: one geometry for the picker's
// figure, the flasher's turntable and this card, and a fourth hand-typed
// copy of the numbers is gone. URLs resolve against THIS module, so any
// page that imports it (and the render probe) finds the same files.
const MODELS = new URL("../models/", import.meta.url);
const LEDGER = new URL("../devices/figures.json", import.meta.url);

const figureCache = new Map(); // figure id → Promise<parsed GLB>
function loadFigureModel(figId) {
  if (figureCache.has(figId)) return figureCache.get(figId);
  const p = fetch(new URL(`${figId}.glb`, MODELS))
    .then((r) => {
      if (!r.ok) throw new Error(`HTTP ${r.status}`);
      return r.arrayBuffer();
    })
    .then((buf) => parseGLB(buf))
    .catch((err) => {
      figureCache.delete(figId); // a failed load evicts itself; a later card retries
      throw err;
    });
  figureCache.set(figId, p);
  return p;
}
let ledgerP = null;
function loadLedger() {
  ledgerP = ledgerP || fetch(LEDGER)
    .then((r) => {
      if (!r.ok) throw new Error(`HTTP ${r.status}`);
      return r.json();
    })
    .catch((err) => {
      ledgerP = null;
      throw err;
    });
  return ledgerP;
}

// How the Lab paints a figure's materials (gen_device_glbs.mjs names them by
// role): printed parts take the active finish, the lit face becomes the live
// glass (see placeFigure), and everything else — glass, lens, the canary
// accent — keeps the figure's own color.
export const FIGURE_PAINT = {
  "printed shell": { role: "shell", gloss: 0.24 },
  "secondary printed part": { role: "shell2", gloss: 0.2 },
  "dark printed part": { gloss: 0.2 },
  "glass / screen": { gloss: 0.75 },
  "canary accent": { gloss: 0.3 },
};
export const SCREEN_MATERIAL = "lit screen";

function frameFigure(scene, size) {
  const big = Math.max(size[0], size[1], size[2]);
  scene.setContactShadow({
    y: -size[1] / 2 - 1.5,
    rx: Math.max(16, size[0] * 0.62),
    rz: Math.max(14, size[2] * 0.9 + 10),
    alpha: 0.3,
  });
  scene.dist = 60 + big * 1.8;
}

// glTF frame (+Y up, +Z toward the viewer, mm after parseGLB) is the card's
// frame, so a figure only needs centering.
function placeFigure(scene, model, { round }) {
  const c = model.bbox.center;
  const center = M4.translate(-c[0], -c[1], -c[2]);
  for (const part of model.parts) {
    if (part.name === SCREEN_MATERIAL) {
      // the lit face IS the glass: a screen plane over its footprint, on its
      // front face, textured with the live framebuffer once a sheet has one
      const lo = [Infinity, Infinity, Infinity], hi = [-Infinity, -Infinity, -Infinity];
      for (let i = 0; i < part.pos.length; i += 3) {
        for (let k = 0; k < 3; k++) {
          lo[k] = Math.min(lo[k], part.pos[i + k]);
          hi[k] = Math.max(hi[k], part.pos[i + k]);
        }
      }
      scene.addMesh(screenPlane(hi[0] - lo[0], hi[1] - lo[1], round), {
        screen: true,
        model: M4.translate((lo[0] + hi[0]) / 2 - c[0], (lo[1] + hi[1]) / 2 - c[1], hi[2] - c[2]),
      });
      continue;
    }
    const paint = FIGURE_PAINT[part.name] || {};
    scene.addMesh(part, {
      color: part.color, gloss: paint.gloss ?? 0.3, role: paint.role ?? null, model: center,
    });
  }
  frameFigure(scene, model.bbox.size);
}

// The twelve edges of a box, as a line list — how an IDEA stands in: a ghost,
// no fill, never a product-looking body (docs/design/FLEET_FIGURES.md §6).
function boxEdges(w, h, d) {
  const m = new MeshBuilder();
  const v = [];
  for (const x of [-w / 2, w / 2]) for (const y of [-h / 2, h / 2]) for (const z of [-d / 2, d / 2])
    v.push(m.vert([x, y, z], [0, 0, 1]));
  // corner index = 4·xi + 2·yi + zi
  for (const [a, b] of [[0, 1], [2, 3], [4, 5], [6, 7], [0, 2], [1, 3], [4, 6], [5, 7],
                        [0, 4], [1, 5], [2, 6], [3, 7]]) m.idx.push(v[a], v[b]);
  return m;
}

// No committed model for this figure (an idea, or a sketch the 3D tier does
// not build): stand in with its ENVELOPE from the ledger — a ghost for an
// idea, a plain slab otherwise — and say so on the console. Never another
// device's body: a card that borrows the WAP's box is a picture of the wrong
// thing.
function figureStandIn(scene, fig, devId) {
  const e = fig.envelope_mm;
  if (fig.confidence === "idea") {
    scene.addMesh(boxEdges(e.w, e.h, e.d), { color: [0.55, 0.58, 0.64], lines: true, unlit: true });
  } else {
    console.warn(`scene3d: "${devId}" has no committed figure model — standing in with `
      + `${fig.id}'s ${fig.dims_source} envelope`);
    scene.addMesh(roundedBox(e.w, e.h, e.d, Math.min(4, e.w / 4, e.h / 4)), {
      color: activeFinish().shell2, role: "shell2", gloss: 0.18,
    });
  }
  frameFigure(scene, [e.w, e.h, e.d]);
}

/** A builder that draws fleet figure `figId` from its committed GLB. Returns
 * the load promise (the render probe waits on it; the page need not). */
export function buildFromFigure(figId, { round = false } = {}) {
  return (scene) => {
    scene.clearParts();
    const gen = scene.buildGen;
    return loadFigureModel(figId)
      .then((model) => {
        if (scene.buildGen === gen) placeFigure(scene, model, { round });
      })
      .catch(() => loadLedger().then((led) => {
        const fig = led.figures.find((f) => f.id === figId);
        if (!fig) throw new Error(`${figId} is not in the ledger`);
        if (scene.buildGen === gen) figureStandIn(scene, fig, figId);
      }))
      .catch((err) => console.warn(`scene3d: ${figId}: no model and no ledger entry (${err.message})`));
  };
}

// Sensing canaries (no glass): simplified true-to-scad bodies so every
// family member gets a card. Dimensions from canary_*_enclosure.scad.
export function buildVision(scene) {
  scene.clearParts();
  const f = activeFinish();
  const body = roundedBox(46, 46, 22, 6);
  const lensBarrel = cylinder(9, 6, 48);
  const lensGlass = cylinder(6.5, 1.5, 48);
  scene.addMesh(body, { color: f.shell, role: "shell", gloss: 0.25 });
  scene.addMesh(lensBarrel, { color: [0.1, 0.1, 0.11], gloss: 0.5, model: M4.translate(0, 6, 12) });
  scene.addMesh(lensGlass, { color: [0.02, 0.03, 0.05], gloss: 0.95, model: M4.translate(0, 6, 15.4) });
  const led = cylinder(1.6, 1.2, 24);
  scene.addMesh(led, { color: f.beacon, role: "beacon", gloss: 0.9, model: M4.translate(12, -12, 11.6) });
    scene.setContactShadow({ y: -26, rx: 34, rz: 27, alpha: 0.30 });
  scene.dist = 130;
}

export function buildWap(scene) {
  scene.clearParts();
  const f = activeFinish();
  const body = roundedBox(58, 38, 20, 5);
  scene.addMesh(body, { color: f.shell, role: "shell", gloss: 0.25 });
  // vent slots implied by a recessed secondary-finish inset panel
  const inset = roundedBox(44, 24, 1.4, 3);
  scene.addMesh(inset, { color: f.shell2, role: "shell2", gloss: 0.15, model: M4.translate(0, 0, 10) });
  const led = cylinder(1.6, 1.4, 24);
  scene.addMesh(led, { color: f.beacon, role: "beacon", gloss: 0.9, model: M4.translate(20, 11, 10.2) });
    scene.setContactShadow({ y: -22, rx: 40, rz: 27, alpha: 0.30 });
  scene.dist = 135;
}

export function buildSense(scene) {
  scene.clearParts();
  const f = activeFinish();
  // radar radome: soft rounded puck standing on edge
  const body = cylinder(24, 16, 64);
  scene.addMesh(body, { color: f.shell, role: "shell", gloss: 0.3 });
  // the radome cap prints in the secondary finish (radar-transparent PETG)
  const dome = cylinder(19, 2.5, 64);
  scene.addMesh(dome, { color: f.shell2, role: "shell2", gloss: 0.45, model: M4.translate(0, 0, 9) });
  const led = cylinder(1.4, 1.4, 24);
  scene.addMesh(led, { color: f.beacon, role: "beacon", gloss: 0.9, model: M4.translate(0, -17, 8.4) });
    scene.setContactShadow({ y: -27, rx: 32, rz: 26, alpha: 0.30 });
  scene.dist = 120;
}

// Registry id → builder. The display line reads its fleet figure (the
// manifest's `figure`, the same id deviceFigure() resolves); the witnesses
// keep their procedural bodies until real-shapes.js swaps in the committed
// print-validated STLs. An idea has no entry here, ever: builderFor() draws
// every concept as its figure's ghost — the Fence Guard used to keep a solid
// hand-modeled body of its own, the one idea on the page that looked like
// something you could buy (tests/scene_figures.test.js now refuses that).
export const FIGURE_BUILDERS = {
  "canary-display-watch": ["device.canary-display-watch", { round: true }],
  "canary-display-dash": ["device.canary-display-dash", {}],
  "canary-display-nightstand-s3": ["device.canary-display-nightstand", {}],
  "canary-display-nightstand-c6": ["device.canary-display-nightstand-c6", {}],
  "canary-display-touch169": ["device.canary-display-touch169", {}],
  "canary-display-dash7": ["device.canary-display-dash7", {}],
  // one board, one case, two products: the bedside 7" draws the Dash 7's slab
  "canary-display-nightstand7": ["device.canary-display-dash7", {}],
  "canary-display-amoled241": ["device.canary-display-amoled241", {}],
  "canary-nightlight": ["device.canary-nightlight", {}],
};

export const BUILDERS = {
  ...Object.fromEntries(Object.entries(FIGURE_BUILDERS)
    .map(([id, [fig, opts]]) => [id, buildFromFigure(fig, opts)])),
  "canary-vision": buildVision,
  "canary-wap": buildWap,
  "canary-sense": buildSense,
};

/** The builder for any registry device: its own, else its fleet figure's
 * envelope resolved through the ledger (a ghost for an idea, a slab for a
 * figure the 3D tier does not build — every figure that HAS a committed
 * model is routed above, so nothing here fetches a model that is not there;
 * the Lab's probes fail a page on any 4xx) — never another device's body. */
export function builderFor(devId) {
  if (BUILDERS[devId]) return BUILDERS[devId];
  return (scene) => {
    scene.clearParts();
    const gen = scene.buildGen;
    return loadLedger()
      .then((led) => {
        if (scene.buildGen !== gen) return undefined;
        const fig = deviceFigure(led, devId);
        if (!fig) {
          console.warn(`scene3d: no fleet figure draws "${devId}" — its card stays empty `
            + "rather than borrowing another device's body");
          return undefined;
        }
        figureStandIn(scene, fig, devId);
        return undefined;
      })
      .catch((err) => console.warn(`scene3d: "${devId}": the figure ledger did not load (${err.message})`));
  };
}
