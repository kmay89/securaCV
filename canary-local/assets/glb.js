// canary-local/assets/glb.js — glTF-binary → scene3d parts, zero deps.
//
// The board viewer's STL: a minimal GLB reader for exactly the subset a CAD
// export (cascadio STEP→GLB, or a KiCad glTF export) emits — indexed
// triangles, f32 POSITION + NORMAL, u16/u32 indices, a node TRS/matrix tree,
// and pbrMetallicRoughness.baseColorFactor per material. Positions are baked
// to world space and scaled to millimeters (glTF ships meters), so the parts
// drop straight into the same DeviceScene the printed STL parts use.
//
// Same spirit as stl.js: no three.js, no CDN — these pages work with the
// ethernet cable unplugged (Invariant IV). Heavy STEP→GLB tessellation lives
// in the generator (tools/gen_boards.py); the browser only reads the small
// committed .glb, exactly as it only reads committed .stl for enclosures.
//
// Returns { parts:[{pos,nrm,uv,idx,color:[r,g,b]}], bbox:{min,max,size,center},
//           triangles } — parts grouped by material color, then split so no
// part exceeds 65535 vertices (DeviceScene indexes with Uint16).

const CT = { 5120: Int8Array, 5121: Uint8Array, 5122: Int16Array,
             5123: Uint16Array, 5125: Uint32Array, 5126: Float32Array };
const NUMC = { SCALAR: 1, VEC2: 2, VEC3: 3, VEC4: 4, MAT4: 16 };
const MAXV = 65535; // Uint16 index ceiling; buckets split on a triangle bound

export function parseGLB(bufferOrView, { scale = 1000 } = {}) {
  // Accept a raw ArrayBuffer or any ArrayBufferView (Node Buffer, Uint8Array)
  // zero-copy — Node callers pass readFileSync's Buffer straight in.
  const isView = ArrayBuffer.isView(bufferOrView);
  const buffer = isView ? bufferOrView.buffer : bufferOrView;
  const byteOffset = isView ? bufferOrView.byteOffset : 0;
  const byteLength = bufferOrView.byteLength;
  const dv = new DataView(buffer, byteOffset, byteLength);
  if (dv.getUint32(0, true) !== 0x46546c67) throw new Error("not a GLB");
  const len = dv.getUint32(8, true);
  let off = 12, json = null, bin = null;
  while (off < len) {
    const clen = dv.getUint32(off, true), ctype = dv.getUint32(off + 4, true);
    const body = off + 8;
    if (ctype === 0x4e4f534a) json = JSON.parse(new TextDecoder().decode(new Uint8Array(buffer, byteOffset + body, clen)));
    else if (ctype === 0x004e4942) bin = { off: byteOffset + body, len: clen };
    off = body + clen + ((4 - (clen % 4)) % 4);
  }
  if (!json || !bin) throw new Error("GLB missing JSON or BIN chunk");
  const g = json;
  const bufOff = (i) => (i === 0 ? bin.off : (() => { throw new Error("external buffers unsupported"); })());

  const readAccessor = (idx) => {
    const a = g.accessors[idx];
    const bv = g.bufferViews[a.bufferView];
    const comps = NUMC[a.type];
    const Ctor = CT[a.componentType];
    const base = bufOff(bv.buffer) + (bv.byteOffset || 0) + (a.byteOffset || 0);
    const stride = bv.byteStride || comps * Ctor.BYTES_PER_ELEMENT;
    const strideComps = stride / Ctor.BYTES_PER_ELEMENT;
    const out = new Float64Array(a.count * comps);
    // one typed-array view over the whole accessor range, not one per vertex
    const view = new Ctor(buffer, base, (a.count - 1) * strideComps + comps);
    if (stride === comps * Ctor.BYTES_PER_ELEMENT) {
      out.set(view); // tightly packed → native copy (+ widen to f64)
    } else {
      for (let i = 0; i < a.count; i++) {
        const start = i * strideComps;
        for (let c = 0; c < comps; c++) out[i * comps + c] = view[start + c];
      }
    }
    return { data: out, comps, count: a.count };
  };

  // node → local matrix (column-major, glTF convention)
  const nodeMatrix = (n) => {
    if (n.matrix) return n.matrix.slice();
    const t = n.translation || [0, 0, 0];
    const r = n.rotation || [0, 0, 0, 1];
    const s = n.scale || [1, 1, 1];
    const [x, y, z, w] = r;
    const x2 = x + x, y2 = y + y, z2 = z + z;
    const xx = x * x2, xy = x * y2, xz = x * z2;
    const yy = y * y2, yz = y * z2, zz = z * z2;
    const wx = w * x2, wy = w * y2, wz = w * z2;
    return [
      (1 - (yy + zz)) * s[0], (xy + wz) * s[0], (xz - wy) * s[0], 0,
      (xy - wz) * s[1], (1 - (xx + zz)) * s[1], (yz + wx) * s[1], 0,
      (xz + wy) * s[2], (yz - wx) * s[2], (1 - (xx + yy)) * s[2], 0,
      t[0], t[1], t[2], 1,
    ];
  };
  const mul = (a, b) => {
    const o = new Array(16);
    for (let c = 0; c < 4; c++) for (let r = 0; r < 4; r++)
      o[c * 4 + r] = a[r] * b[c * 4] + a[4 + r] * b[c * 4 + 1] + a[8 + r] * b[c * 4 + 2] + a[12 + r] * b[c * 4 + 3];
    return o;
  };
  const tp = (m, x, y, z) => [
    m[0] * x + m[4] * y + m[8] * z + m[12],
    m[1] * x + m[5] * y + m[9] * z + m[13],
    m[2] * x + m[6] * y + m[10] * z + m[14],
  ];
  const tn = (m, x, y, z) => { // direction (upper 3×3), normalized
    const nx = m[0] * x + m[4] * y + m[8] * z;
    const ny = m[1] * x + m[5] * y + m[9] * z;
    const nz = m[2] * x + m[6] * y + m[10] * z;
    const l = Math.hypot(nx, ny, nz) || 1;
    return [nx / l, ny / l, nz / l];
  };

  const matColor = (mi) => {
    const m = mi != null ? g.materials?.[mi] : null;
    const f = m?.pbrMetallicRoughness?.baseColorFactor || [0.8, 0.8, 0.8, 1];
    return [f[0], f[1], f[2]];
  };
  const key = (c) => c.map((v) => Math.round(v * 255)).join(",");
  const buckets = new Map(); // colorKey → {color, pos, nrm}

  const bb = { min: [Infinity, Infinity, Infinity], max: [-Infinity, -Infinity, -Infinity] };
  const grow = (p) => { for (let i = 0; i < 3; i++) { if (p[i] < bb.min[i]) bb.min[i] = p[i]; if (p[i] > bb.max[i]) bb.max[i] = p[i]; } };

  const emitPrim = (prim, world) => {
    if (prim.mode != null && prim.mode !== 4) return; // triangles only
    const P = readAccessor(prim.attributes.POSITION);
    const N = prim.attributes.NORMAL != null ? readAccessor(prim.attributes.NORMAL) : null;
    const idx = prim.indices != null ? readAccessor(prim.indices).data
                                     : Float64Array.from({ length: P.count }, (_, i) => i);
    const col = matColor(prim.material);
    const k = key(col);
    let bkt = buckets.get(k);
    if (!bkt) buckets.set(k, (bkt = { color: col, pos: [], nrm: [] }));
    for (let t = 0; t < idx.length; t += 3) {
      for (let e = 0; e < 3; e++) {
        const vi = idx[t + e];
        // Full node tree FIRST (the meter→mm scale can live on an
        // intermediate node, composed with rotations), then scale the
        // finished world point — uniform scale-at-the-end is safe.
        const wp = tp(world, P.data[vi * 3], P.data[vi * 3 + 1], P.data[vi * 3 + 2]);
        wp[0] *= scale; wp[1] *= scale; wp[2] *= scale;
        grow(wp);
        bkt.pos.push(wp[0], wp[1], wp[2]);
        if (N) { const wn = tn(world, N.data[vi * 3], N.data[vi * 3 + 1], N.data[vi * 3 + 2]); bkt.nrm.push(wn[0], wn[1], wn[2]); }
        else bkt.nrm.push(0, 0, 1);
      }
    }
  };

  const walk = (ni, parent) => {
    const n = g.nodes[ni];
    const world = mul(parent, nodeMatrix(n));
    if (n.mesh != null) for (const prim of g.meshes[n.mesh].primitives) emitPrim(prim, world);
    for (const c of n.children || []) walk(c, world);
  };
  const scene = g.scenes[g.scene || 0];
  const I = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1];
  for (const ni of scene.nodes) walk(ni, I);

  // group buckets into parts, splitting any bucket over the Uint16 ceiling.
  // Geometry is non-indexed (3 verts per triangle) and MAXV is a multiple of
  // 3, so every split lands on a triangle boundary.
  const parts = [];
  let tris = 0;
  for (const b of buckets.values()) {
    const total = b.pos.length / 3;
    for (let start = 0; start < total; start += MAXV) {
      const n = Math.min(MAXV, total - start);
      const idx = new Array(n);
      for (let i = 0; i < n; i++) idx[i] = i;
      parts.push({
        pos: b.pos.slice(start * 3, (start + n) * 3),
        nrm: b.nrm.slice(start * 3, (start + n) * 3),
        uv: new Array(n * 2).fill(0),
        idx,
        color: b.color,
      });
      tris += n / 3;
    }
  }
  const size = bb.max.map((m, i) => m - bb.min[i]);
  const center = bb.min.map((m, i) => m + size[i] / 2);
  return { parts, bbox: { ...bb, size, center }, triangles: tris };
}
