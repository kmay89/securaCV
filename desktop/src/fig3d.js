// fig3d.js — the figure slot's 3D upgrade, shared by BOTH flashers.
//
// Upgrades a product row's flat isometric figure to a small turntable of the
// committed device model (models/<figure-id>.glb, generated from the same
// massing the SVG figure is drawn from — one geometry, two renderings). The
// upgrade is progressive and never a dependency: a failed fetch (an offline
// copy without models/) keeps the drawing, exactly the real-shapes rule.
//
// This file is a CLASSIC script (no modules) exposing one global,
// window.SCV_FIG3D, because the two flashers load scripts differently (the
// browser flasher is an ES-module page, the desktop Flasher is a classic
// script) and the SAME BYTES must serve both — the copies live at
// canary-local/assets/fig3d.js and desktop/src/fig3d.js, and
// canary-local/tests/device_models.test.js asserts they never drift.
//
// Rendering is a zero-dependency canvas-2D painter: orthographic turntable,
// flat facets, painter's-algorithm sort — the fleet-figure language in 3D,
// no WebGL context per row (a picker can show a dozen rows; 2D contexts are
// free, WebGL contexts are not). Spin is hover/focus-only and respects
// prefers-reduced-motion (a static three-quarter pose is still 3D).
//
// The GLB subset read here is exactly what tools/figures/gen_device_glbs.mjs
// emits: one binary buffer, u16 indices, f32 POSITION/NORMAL, one primitive
// per mesh, pbrMetallicRoughness.baseColorFactor, identity nodes.
(() => {
  "use strict";

  function parseGlb(buf) {
    const dv = new DataView(buf);
    if (dv.getUint32(0, true) !== 0x46546c67) throw new Error("not a GLB");
    const total = dv.getUint32(8, true);
    let off = 12, json = null, bin = null;
    while (off < total) {
      const clen = dv.getUint32(off, true);
      const ctype = dv.getUint32(off + 4, true);
      const body = off + 8;
      if (ctype === 0x4e4f534a) {
        json = JSON.parse(new TextDecoder().decode(new Uint8Array(buf, body, clen)));
      } else if (ctype === 0x004e4942) {
        bin = body;
      }
      off = body + clen + ((4 - (clen % 4)) % 4);
    }
    if (!json || bin === null) throw new Error("GLB missing chunks");
    const acc = (i) => {
      const a = json.accessors[i];
      const v = json.bufferViews[a.bufferView];
      const base = bin + (v.byteOffset || 0) + (a.byteOffset || 0);
      const comps = { SCALAR: 1, VEC3: 3 }[a.type];
      const Ctor = { 5123: Uint16Array, 5125: Uint32Array, 5126: Float32Array }[a.componentType];
      return new Ctor(buf, base, a.count * comps);
    };
    const parts = [];
    let lo = [Infinity, Infinity, Infinity], hi = [-Infinity, -Infinity, -Infinity];
    for (const mesh of json.meshes || []) {
      for (const p of mesh.primitives || []) {
        const pos = acc(p.attributes.POSITION);
        const nrm = acc(p.attributes.NORMAL);
        const idx = acc(p.indices);
        const mat = json.materials?.[p.material]?.pbrMetallicRoughness?.baseColorFactor || [0.7, 0.7, 0.7, 1];
        for (let i = 0; i < pos.length; i += 3) {
          for (let k = 0; k < 3; k++) {
            if (pos[i + k] < lo[k]) lo[k] = pos[i + k];
            if (pos[i + k] > hi[k]) hi[k] = pos[i + k];
          }
        }
        parts.push({ pos, nrm, idx, color: mat.slice(0, 3) });
      }
    }
    return { parts, lo, hi };
  }

  const TILT = -0.42;          // camera pitch, radians (the figures' own angle)
  const POSE = -0.55;          // resting yaw — the three-quarter product pose
  const LIGHT = (() => {       // fixed view-space key light
    const l = [0.35, 0.75, 0.65];
    const n = Math.hypot(...l);
    return l.map((v) => v / n);
  })();

  function render(ctx, model, w, h, yaw) {
    const { parts, lo, hi } = model;
    const cx = (lo[0] + hi[0]) / 2, cyy = (lo[1] + hi[1]) / 2, cz = (lo[2] + hi[2]) / 2;
    const ext = Math.max(hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]) || 1;
    const s = (0.86 * Math.min(w, h)) / ext;
    const sy = Math.sin(yaw), cy = Math.cos(yaw);
    const sp = Math.sin(TILT), cp = Math.cos(TILT);
    const tris = [];
    for (const part of parts) {
      const { pos, nrm, idx, color } = part;
      const n = pos.length / 3;
      const vx = new Float64Array(n), vy = new Float64Array(n), vz = new Float64Array(n);
      const nz = new Float64Array(n), shade = new Float64Array(n);
      for (let i = 0; i < n; i++) {
        const x = pos[i * 3] - cx, y = pos[i * 3 + 1] - cyy, z = pos[i * 3 + 2] - cz;
        const x1 = x * cy + z * sy, z1 = -x * sy + z * cy;
        const y2 = y * cp - z1 * sp, z2 = y * sp + z1 * cp;
        vx[i] = w / 2 + x1 * s;
        vy[i] = h / 2 - y2 * s;
        vz[i] = z2;
        const a = nrm[i * 3], b = nrm[i * 3 + 1], c = nrm[i * 3 + 2];
        const a1 = a * cy + c * sy, c1 = -a * sy + c * cy;
        const b2 = b * cp - c1 * sp, c2 = b * sp + c1 * cp;
        nz[i] = c2;
        shade[i] = Math.max(0, a1 * LIGHT[0] + b2 * LIGHT[1] + c2 * LIGHT[2]);
      }
      for (let t = 0; t < idx.length; t += 3) {
        const a = idx[t], b = idx[t + 1], c = idx[t + 2];
        // Backface cull in screen space (CCW = facing the camera).
        const area = (vx[b] - vx[a]) * (vy[c] - vy[a]) - (vy[b] - vy[a]) * (vx[c] - vx[a]);
        if (area >= 0) continue; // y is flipped on screen, so front faces are CW here
        const k = 0.42 + 0.58 * shade[a];
        tris.push({
          z: (vz[a] + vz[b] + vz[c]) / 3,
          x1: vx[a], y1: vy[a], x2: vx[b], y2: vy[b], x3: vx[c], y3: vy[c],
          f: `rgb(${Math.round(color[0] * 255 * k)},${Math.round(color[1] * 255 * k)},${Math.round(color[2] * 255 * k)})`,
        });
      }
    }
    tris.sort((p, q) => p.z - q.z);
    ctx.clearRect(0, 0, w, h);
    ctx.lineWidth = 1;
    for (const t of tris) {
      ctx.fillStyle = t.f;
      ctx.strokeStyle = t.f; // hairline stroke closes anti-aliasing seams
      ctx.beginPath();
      ctx.moveTo(t.x1, t.y1);
      ctx.lineTo(t.x2, t.y2);
      ctx.lineTo(t.x3, t.y3);
      ctx.closePath();
      ctx.fill();
      ctx.stroke();
    }
  }

  const reduced = typeof matchMedia === "function" &&
    matchMedia("(prefers-reduced-motion: reduce)").matches;

  async function upgrade(el, url) {
    if (!el || el.dataset.fig3d === "done") return;
    el.dataset.fig3d = "done";
    let model;
    try {
      const res = await fetch(url);
      if (!res.ok) return; // no model committed for this figure — keep the SVG
      model = parseGlb(await res.arrayBuffer());
      if (!model.parts.length) return;
    } catch {
      return; // offline copy without models/ — the drawing was already right
    }
    const rect = el.getBoundingClientRect();
    const cssW = Math.max(24, rect.width || 46);
    const cssH = Math.max(24, rect.height || 46);
    const dpr = Math.min(3, (typeof devicePixelRatio === "number" && devicePixelRatio) || 1);
    const canvas = document.createElement("canvas");
    canvas.className = "fig3d";
    canvas.width = Math.round(cssW * dpr);
    canvas.height = Math.round(cssH * dpr);
    canvas.setAttribute("aria-hidden", "true");
    const ctx = canvas.getContext("2d");
    if (!ctx) return;
    ctx.scale(dpr, dpr);
    render(ctx, model, cssW, cssH, POSE);
    el.appendChild(canvas);
    el.classList.add("has3d");

    if (reduced) return; // a still three-quarter pose is the whole story
    let yaw = POSE;
    let spinning = false;
    let raf = 0;
    const tick = () => {
      raf = 0;
      if (!spinning) {
        // ease home to the pose, then rest
        const d = ((POSE - yaw + Math.PI * 3) % (Math.PI * 2)) - Math.PI;
        if (Math.abs(d) < 0.02) {
          yaw = POSE;
          render(ctx, model, cssW, cssH, yaw);
          return;
        }
        yaw += d * 0.18;
      } else {
        yaw += 0.035;
      }
      render(ctx, model, cssW, cssH, yaw);
      raf = requestAnimationFrame(tick);
    };
    const start = () => {
      spinning = true;
      if (!raf) raf = requestAnimationFrame(tick);
    };
    const stop = () => {
      spinning = false;
      if (!raf) raf = requestAnimationFrame(tick);
    };
    el.addEventListener("pointerenter", start);
    el.addEventListener("pointerleave", stop);
    el.addEventListener("focusin", start);
    el.addEventListener("focusout", stop);
  }

  function upgradeAll(root) {
    for (const el of (root || document).querySelectorAll("[data-fig3d-id]")) {
      upgrade(el, `models/${el.dataset.fig3dId}.glb`);
    }
  }

  window.SCV_FIG3D = { upgrade, upgradeAll };
})();
