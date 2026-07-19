// canary-local/assets/scene3d.js — the device as an object, not a photo.
//
// A deliberately small WebGL renderer (no three.js, no CDN — this repo
// ships pages that work with the ethernet cable unplugged). It builds
// each Canary's body procedurally from the same millimeter dimensions as
// the OpenSCAD enclosures in docs/hardware/enclosure/, and textures the
// glass with the LIVE emulator framebuffer — so the 3D card is not an
// illustration of the device, it IS the device, running.
//
// Lighting is a fixed two-key studio (soft top-left key + cool rim +
// fresnel) tuned for the Apple-pairing-card look: friendly, floating,
// slightly glossy, obviously tangible.

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
// corners, extruded ±h/2 in Z, with beveled front edge feel via normals.
export function roundedBox(w, h, d, r, seg = 6) {
  const m = new MeshBuilder();
  const hw = w / 2, hh = h / 2, hd = d / 2;
  const pts = [];
  const corners = [
    [hw - r, hh - r, 0], [-(hw - r), hh - r, Math.PI / 2],
    [-(hw - r), -(hh - r), Math.PI], [hw - r, -(hh - r), (3 * Math.PI) / 2],
  ];
  for (const [cx, cy, a0] of corners)
    for (let i = 0; i <= seg; i++) {
      const a = a0 + (i / seg) * (Math.PI / 2);
      pts.push([cx + r * Math.cos(a), cy + r * Math.sin(a)]);
    }
  const n = pts.length;
  // side wall
  for (let i = 0; i < n; i++) {
    const [x, y] = pts[i];
    const [x2, y2] = pts[(i + 1) % n];
    const nx = (y2 - y), ny = -(x2 - x);
    const len = Math.hypot(nx, ny) || 1;
    const a = m.vert([x, y, hd], [nx / len, ny / len, 0]);
    const b = m.vert([x, y, -hd], [nx / len, ny / len, 0]);
    const c = m.vert([x2, y2, -hd], [nx / len, ny / len, 0]);
    const dd = m.vert([x2, y2, hd], [nx / len, ny / len, 0]);
    m.quad(a, b, c, dd);
  }
  // front + back caps (fan from center)
  for (const z of [hd, -hd]) {
    const nz = z > 0 ? 1 : -1;
    const center = m.vert([0, 0, z], [0, 0, nz]);
    const ring = pts.map(([x, y]) => m.vert([x, y, z], [0, 0, nz]));
    for (let i = 0; i < n; i++) {
      const a = ring[i], b = ring[(i + 1) % n];
      if (nz > 0) m.tri(center, a, b);
      else m.tri(center, b, a);
    }
  }
  return m;
}

// Cylinder along Z (the watch drum / bezel), optional inner bore → ring.
export function cylinder(rOut, depth, seg = 64, rIn = 0) {
  const m = new MeshBuilder();
  const hd = depth / 2;
  for (let i = 0; i < seg; i++) {
    const a0 = (i / seg) * Math.PI * 2;
    const a1 = ((i + 1) / seg) * Math.PI * 2;
    const c0 = [Math.cos(a0), Math.sin(a0)], c1 = [Math.cos(a1), Math.sin(a1)];
    // outer wall
    {
      const a = m.vert([rOut * c0[0], rOut * c0[1], hd], [c0[0], c0[1], 0]);
      const b = m.vert([rOut * c0[0], rOut * c0[1], -hd], [c0[0], c0[1], 0]);
      const c = m.vert([rOut * c1[0], rOut * c1[1], -hd], [c1[0], c1[1], 0]);
      const d = m.vert([rOut * c1[0], rOut * c1[1], hd], [c1[0], c1[1], 0]);
      m.quad(a, b, c, d);
    }
    if (rIn > 0) {
      // inner wall (bore)
      const a = m.vert([rIn * c0[0], rIn * c0[1], hd], [-c0[0], -c0[1], 0]);
      const b = m.vert([rIn * c1[0], rIn * c1[1], hd], [-c1[0], -c1[1], 0]);
      const c = m.vert([rIn * c1[0], rIn * c1[1], -hd], [-c1[0], -c1[1], 0]);
      const d = m.vert([rIn * c0[0], rIn * c0[1], -hd], [-c0[0], -c0[1], 0]);
      m.quad(a, b, c, d);
    }
    // caps
    for (const z of [hd, -hd]) {
      const nz = z > 0 ? 1 : -1;
      if (rIn > 0) {
        const a = m.vert([rIn * c0[0], rIn * c0[1], z], [0, 0, nz]);
        const b = m.vert([rOut * c0[0], rOut * c0[1], z], [0, 0, nz]);
        const c = m.vert([rOut * c1[0], rOut * c1[1], z], [0, 0, nz]);
        const d = m.vert([rIn * c1[0], rIn * c1[1], z], [0, 0, nz]);
        if (nz > 0) m.quad(a, b, c, d);
        else m.quad(d, c, b, a);
      } else {
        const ctr = m.vert([0, 0, z], [0, 0, nz]);
        const a = m.vert([rOut * c0[0], rOut * c0[1], z], [0, 0, nz]);
        const b = m.vert([rOut * c1[0], rOut * c1[1], z], [0, 0, nz]);
        if (nz > 0) m.tri(ctr, a, b);
        else m.tri(ctr, b, a);
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

const FS = `
precision mediump float;
varying vec3 vN; varying vec3 vP; varying vec2 vUv;
varying vec3 vObj; varying vec3 vNl;
uniform vec3 uColor;
uniform float uGloss;      // 0 matte shell .. 1 glass
uniform float uUseTex;     // screen face samples the live framebuffer
uniform float uEmissive;   // screen glow (backlight level)
uniform float uClipZ;      // print guide: hide everything above this layer
uniform float uMinZ;       // part's plate level (local z)
uniform float uOverhangOn; // tint faces steeper than 45° pointing down
uniform float uUnlit;      // plate grid / layer contours: flat color
uniform sampler2D uTex;
void main() {
  if (vObj.z - uMinZ > uClipZ) discard;
  if (uUnlit > 0.5) { gl_FragColor = vec4(uColor, 1.0); return; }
  vec3 N = normalize(vN);
  vec3 V = normalize(-vP);
  vec3 key = normalize(vec3(-0.45, 0.75, 0.6));
  vec3 rim = normalize(vec3(0.6, 0.1, -0.8));
  float kd = max(dot(N, key), 0.0);
  float bounce = max(dot(N, normalize(vec3(0.2, -1.0, 0.25))), 0.0) * 0.18;
  float rimL = pow(1.0 - max(dot(N, V), 0.0), 2.6);
  vec3 base = uColor;
  if (uUseTex > 0.5) {
    vec3 tex = texture2D(uTex, vUv).rgb;
    base = tex * (0.25 + 0.75 * uEmissive);
    // panel off-glass reflection so a dark screen still reads as glass
    float sheen = pow(max(dot(reflect(-key, N), V), 0.0), 24.0);
    gl_FragColor = vec4(base + sheen * 0.10 + rimL * 0.03, 1.0);
    return;
  }
  vec3 lit = base * (0.30 + kd * 0.72 + bounce);
  float spec = pow(max(dot(reflect(-key, N), V), 0.0), mix(8.0, 64.0, uGloss)) * mix(0.06, 0.5, uGloss);
  vec3 col = lit + vec3(spec) + rimL * vec3(0.10, 0.11, 0.13);
  // Overhang guide: local faces steeper than 45° pointing at the plate,
  // above the first layers, would need support in this orientation.
  if (uOverhangOn > 0.5 && normalize(vNl).z < -0.707 && vObj.z > uMinZ + 0.45) {
    col = mix(col, vec3(0.92, 0.28, 0.2), 0.7);
  }
  gl_FragColor = vec4(col, 1.0);
}`;

// ── scene ───────────────────────────────────────────────────────────────
export class DeviceScene {
  /**
   * @param canvas 3D canvas
   * @param screenSource <canvas> the emulator draws into (or null)
   */
  constructor(canvas, screenSource) {
    this.canvas = canvas;
    this.src = screenSource;
    const gl = canvas.getContext("webgl", {
      antialias: true,
      alpha: true,
      premultipliedAlpha: true,
    });
    this.gl = gl;
    this.prog = this._program(VS, FS);
    this.u = {};
    for (const n of ["uProj", "uView", "uModel", "uColor", "uGloss", "uUseTex",
                     "uEmissive", "uTex", "uClipZ", "uMinZ", "uOverhangOn", "uUnlit"])
      this.u[n] = gl.getUniformLocation(this.prog, n);
    this.overhangOn = false;
    this.clipZ = 1e9;
    this.viewY = 0; // vertical look-at offset (plate scenes sit above y=0)
    this.a = {
      pos: gl.getAttribLocation(this.prog, "aPos"),
      nrm: gl.getAttribLocation(this.prog, "aNrm"),
      uv: gl.getAttribLocation(this.prog, "aUv"),
    };
    this.parts = [];
    this.rot = { x: -0.28, y: 0.55 }; // presentation pose
    this.home = { x: -0.28, y: 0.55 };
    this.vel = { x: 0, y: 0 };
    this.t = 0;
    this.dist = 150;
    this.glow = 1;
    this.dirtySerial = -1;
    this.tex = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, this.tex);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, 1, 1, 0, gl.RGBA, gl.UNSIGNED_BYTE,
                  new Uint8Array([0, 0, 0, 255]));
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    this._wireOrbit();
    this._raf = null;
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

  addMesh(builder, { color = [0.5, 0.5, 0.5], gloss = 0.2, screen = false,
                     model = M4.ident(), lines = false, unlit = false,
                     clippable = false, minZ = 0 } = {}) {
    const gl = this.gl;
    const part = {
      model,
      color,
      gloss,
      screen,
      lines,
      unlit,
      clippable,
      minZ,
      count: builder.idx.length,
      vbo: gl.createBuffer(),
      nbo: gl.createBuffer(),
      ubo: gl.createBuffer(),
      ibo: gl.createBuffer(),
    };
    gl.bindBuffer(gl.ARRAY_BUFFER, part.vbo);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array(builder.pos), gl.STATIC_DRAW);
    gl.bindBuffer(gl.ARRAY_BUFFER, part.nbo);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array(builder.nrm), gl.STATIC_DRAW);
    gl.bindBuffer(gl.ARRAY_BUFFER, part.ubo);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array(builder.uv), gl.STATIC_DRAW);
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, part.ibo);
    gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, new Uint16Array(builder.idx), gl.STATIC_DRAW);
    this.parts.push(part);
    return part;
  }

  clearParts() {
    for (const p of this.parts) {
      this.gl.deleteBuffer(p.vbo);
      this.gl.deleteBuffer(p.nbo);
      this.gl.deleteBuffer(p.ubo);
      this.gl.deleteBuffer(p.ibo);
    }
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

  start() {
    if (this._raf) return;
    const step = () => {
      this._raf = requestAnimationFrame(step);
      this.draw();
    };
    step();
  }
  stop() {
    if (this._raf) cancelAnimationFrame(this._raf);
    this._raf = null;
  }

  draw() {
    const gl = this.gl;
    const dpr = Math.min(2, window.devicePixelRatio || 1);
    const W = Math.round(this.canvas.clientWidth * dpr);
    const H = Math.round(this.canvas.clientHeight * dpr);
    if (this.canvas.width !== W || this.canvas.height !== H) {
      this.canvas.width = W;
      this.canvas.height = H;
    }
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

    // live screen texture
    if (this.src) {
      gl.bindTexture(gl.TEXTURE_2D, this.tex);
      try {
        gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, this.src);
      } catch { /* canvas not ready yet */ }
    }

    const proj = M4.persp(0.62, W / H, 5, 2000);
    const view = M4.translate(0, -this.viewY, -this.dist);
    const spin = M4.mul(M4.rotX(this.rot.x), M4.rotY(this.rot.y));

    gl.useProgram(this.prog);
    gl.uniformMatrix4fv(this.u.uProj, false, proj);
    gl.uniformMatrix4fv(this.u.uView, false, view);
    gl.uniform1i(this.u.uTex, 0);
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, this.tex);

    for (const p of this.parts) {
      gl.uniformMatrix4fv(this.u.uModel, false, M4.mul(spin, p.model));
      gl.uniform3fv(this.u.uColor, p.color);
      gl.uniform1f(this.u.uGloss, p.gloss);
      gl.uniform1f(this.u.uUseTex, p.screen ? 1 : 0);
      gl.uniform1f(this.u.uEmissive, this.glow);
      gl.uniform1f(this.u.uClipZ, p.clippable ? this.clipZ : 1e9);
      gl.uniform1f(this.u.uMinZ, p.minZ || 0);
      gl.uniform1f(this.u.uOverhangOn, this.overhangOn && p.clippable ? 1 : 0);
      gl.uniform1f(this.u.uUnlit, p.unlit ? 1 : 0);
      gl.bindBuffer(gl.ARRAY_BUFFER, p.vbo);
      gl.vertexAttribPointer(this.a.pos, 3, gl.FLOAT, false, 0, 0);
      gl.enableVertexAttribArray(this.a.pos);
      gl.bindBuffer(gl.ARRAY_BUFFER, p.nbo);
      gl.vertexAttribPointer(this.a.nrm, 3, gl.FLOAT, false, 0, 0);
      gl.enableVertexAttribArray(this.a.nrm);
      gl.bindBuffer(gl.ARRAY_BUFFER, p.ubo);
      gl.vertexAttribPointer(this.a.uv, 2, gl.FLOAT, false, 0, 0);
      gl.enableVertexAttribArray(this.a.uv);
      gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, p.ibo);
      gl.drawElements(p.lines ? gl.LINES : gl.TRIANGLES, p.count,
                      gl.UNSIGNED_SHORT, 0);
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
    const spin = M4.mul(M4.rotX(this.rot.x), M4.rotY(this.rot.y));
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

  removePart(part) {
    const i = this.parts.indexOf(part);
    if (i < 0) return;
    for (const b of ["vbo", "nbo", "ubo", "ibo"]) this.gl.deleteBuffer(part[b]);
    this.parts.splice(i, 1);
  }
}

// ── device bodies (dimensions: docs/hardware/enclosure/*.scad) ──────────
const SHELL = [0.16, 0.17, 0.19];      // matte printed shell (graphite)
const SHELL_LIGHT = [0.90, 0.87, 0.80]; // bone/eggshell variant
const CANARY = [1.0, 0.83, 0.31];       // #FFD44F feather yellow
const GLASS_EDGE = [0.05, 0.05, 0.06];

export function buildWatchStation(scene, { light = false } = {}) {
  // canary_watch_station.scad: drum Ø52, drum_h≈14.8, bezel 7 (Σ≈21.8),
  // aperture Ø34, stand tilt 25°.
  scene.clearParts();
  const shell = light ? SHELL_LIGHT : SHELL;
  const drum = cylinder(26, 14.8, 72);
  const bezel = cylinder(26, 7, 72, 17);
  const glassRing = cylinder(17.4, 1.2, 72, 17);
  const screen = screenPlane(34, 34, true);
  const tilt = (25 * Math.PI) / 180;
  const lean = M4.rotX(-tilt);

  const at = (m, dz) => M4.mul(lean, M4.mul(m, M4.translate(0, 0, dz)));
  scene.addMesh(drum, { color: shell, gloss: 0.22, model: at(M4.ident(), -3.6) });
  scene.addMesh(bezel, { color: shell, gloss: 0.3, model: at(M4.ident(), 7.4) });
  scene.addMesh(glassRing, { color: GLASS_EDGE, gloss: 0.75, model: at(M4.ident(), 10.4) });
  scene.addMesh(screen, { screen: true, model: at(M4.ident(), 11.05) });
  // stand wedge, its pocket under the leaning drum
  const st = wedge(64, 47, 26);
  scene.addMesh(st, {
    color: shell, gloss: 0.18,
    model: M4.mul(M4.translate(0, -30, 2), M4.ident()),
  });
  scene.dist = 165;
}

export function buildDash(scene, { light = false } = {}) {
  // canary_dash_display.scad: shell 113.7 × 73.6 × 16.0 (frame 13.6 +
  // back 2.4), view window 101.3 × 61.2, r_out 5, stand 25°.
  scene.clearParts();
  const shell = light ? SHELL_LIGHT : SHELL;
  const tiltM = M4.rotX((-25 * Math.PI) / 180);
  const body = roundedBox(113.7, 73.6, 16, 5);
  const glass = screenPlane(101.3, 61.2, false);
  const bezl = roundedBox(104.5, 64.4, 1.4, 2.4);
  scene.addMesh(body, { color: shell, gloss: 0.22, model: tiltM });
  scene.addMesh(bezl, { color: GLASS_EDGE, gloss: 0.7, model: M4.mul(tiltM, M4.translate(0, 0, 7.6)) });
  scene.addMesh(glass, { screen: true, model: M4.mul(tiltM, M4.translate(0, 0, 8.45)) });
  const st = wedge(120, 78, 40);
  scene.addMesh(st, { color: shell, gloss: 0.18, model: M4.translate(0, -48, -6) });
  scene.dist = 260;
}

// Sensing canaries (no glass): simplified true-to-scad bodies so every
// family member gets a card. Dimensions from canary_*_enclosure.scad.
export function buildVision(scene) {
  scene.clearParts();
  const body = roundedBox(46, 46, 22, 6);
  const lensBarrel = cylinder(9, 6, 48);
  const lensGlass = cylinder(6.5, 1.5, 48);
  scene.addMesh(body, { color: SHELL_LIGHT, gloss: 0.25 });
  scene.addMesh(lensBarrel, { color: [0.1, 0.1, 0.11], gloss: 0.5, model: M4.translate(0, 6, 12) });
  scene.addMesh(lensGlass, { color: [0.02, 0.03, 0.05], gloss: 0.95, model: M4.translate(0, 6, 15.4) });
  const led = cylinder(1.6, 1.2, 24);
  scene.addMesh(led, { color: CANARY, gloss: 0.9, model: M4.translate(12, -12, 11.6) });
  scene.dist = 130;
}

export function buildWap(scene) {
  scene.clearParts();
  const body = roundedBox(58, 38, 20, 5);
  scene.addMesh(body, { color: SHELL_LIGHT, gloss: 0.25 });
  // vent slots implied by darker inset panel
  const inset = roundedBox(44, 24, 1.4, 3);
  scene.addMesh(inset, { color: [0.35, 0.36, 0.38], gloss: 0.15, model: M4.translate(0, 0, 10) });
  const led = cylinder(1.6, 1.4, 24);
  scene.addMesh(led, { color: CANARY, gloss: 0.9, model: M4.translate(20, 11, 10.2) });
  scene.dist = 135;
}

export function buildSense(scene) {
  scene.clearParts();
  // radar radome: soft rounded puck standing on edge
  const body = cylinder(24, 16, 64);
  scene.addMesh(body, { color: SHELL_LIGHT, gloss: 0.3 });
  const dome = cylinder(19, 2.5, 64);
  scene.addMesh(dome, { color: [0.82, 0.79, 0.72], gloss: 0.45, model: M4.translate(0, 0, 9) });
  const led = cylinder(1.4, 1.4, 24);
  scene.addMesh(led, { color: CANARY, gloss: 0.9, model: M4.translate(0, -17, 8.4) });
  scene.dist = 120;
}

export const BUILDERS = {
  "canary-display-watch": buildWatchStation,
  "canary-display-dash": buildDash,
  "canary-vision": buildVision,
  "canary-wap": buildWap,
  "canary-sense": buildSense,
};
