// canary-local/assets/stl.js — STL → scene3d mesh.
//
// Parses the enclosure library's STL files (binary or ASCII) into the
// same {pos, nrm, uv, idx} shape scene3d's MeshBuilder produces, so the
// printed parts render in the exact viewer the device cards use. STL is
// a triangle soup with per-face normals — no dedup needed for display.
// Also returns the bounding box so the lab can center and frame parts of
// any size, from a 15-minute clip coupon to the field case.

export function parseSTL(buffer) {
  const bytes = new Uint8Array(buffer);
  // ASCII files start with "solid" AND contain "facet" early; binary
  // files may also start with "solid", so sniff for the keyword.
  const head = new TextDecoder().decode(bytes.slice(0, 512));
  if (head.trimStart().startsWith("solid") && head.includes("facet")) {
    return parseAscii(new TextDecoder().decode(bytes));
  }
  return parseBinary(buffer);
}

function finish(pos, nrm) {
  const idx = [];
  for (let i = 0; i < pos.length / 3; i++) idx.push(i);
  const bb = {
    min: [Infinity, Infinity, Infinity],
    max: [-Infinity, -Infinity, -Infinity],
  };
  for (let i = 0; i < pos.length; i += 3) {
    for (let a = 0; a < 3; a++) {
      const v = pos[i + a];
      if (v < bb.min[a]) bb.min[a] = v;
      if (v > bb.max[a]) bb.max[a] = v;
    }
  }
  const size = bb.max.map((m, i) => m - bb.min[i]);
  const center = bb.min.map((m, i) => m + size[i] / 2);
  return {
    mesh: { pos, nrm, uv: new Array((pos.length / 3) * 2).fill(0), idx },
    bbox: { ...bb, size, center },
    triangles: idx.length / 3,
  };
}

function parseBinary(buffer) {
  const dv = new DataView(buffer);
  const n = dv.getUint32(80, true);
  const pos = [];
  const nrm = [];
  let off = 84;
  for (let t = 0; t < n; t++) {
    let nx = dv.getFloat32(off, true);
    let ny = dv.getFloat32(off + 4, true);
    let nz = dv.getFloat32(off + 8, true);
    off += 12;
    const vs = [];
    for (let v = 0; v < 3; v++) {
      vs.push([
        dv.getFloat32(off, true),
        dv.getFloat32(off + 4, true),
        dv.getFloat32(off + 8, true),
      ]);
      off += 12;
    }
    off += 2; // attribute byte count
    if (nx === 0 && ny === 0 && nz === 0) {
      [nx, ny, nz] = faceNormal(vs);
    }
    for (const v of vs) {
      pos.push(v[0], v[1], v[2]);
      nrm.push(nx, ny, nz);
    }
  }
  return finish(pos, nrm);
}

function parseAscii(text) {
  const pos = [];
  const nrm = [];
  const re =
    /facet\s+normal\s+(\S+)\s+(\S+)\s+(\S+)[\s\S]*?vertex\s+(\S+)\s+(\S+)\s+(\S+)\s+vertex\s+(\S+)\s+(\S+)\s+(\S+)\s+vertex\s+(\S+)\s+(\S+)\s+(\S+)/g;
  let m;
  while ((m = re.exec(text))) {
    const f = m.slice(1).map(Number);
    let [nx, ny, nz] = f.slice(0, 3);
    const vs = [f.slice(3, 6), f.slice(6, 9), f.slice(9, 12)];
    if (nx === 0 && ny === 0 && nz === 0) [nx, ny, nz] = faceNormal(vs);
    for (const v of vs) {
      pos.push(v[0], v[1], v[2]);
      nrm.push(nx, ny, nz);
    }
  }
  return finish(pos, nrm);
}

function faceNormal([a, b, c]) {
  const u = [b[0] - a[0], b[1] - a[1], b[2] - a[2]];
  const v = [c[0] - a[0], c[1] - a[1], c[2] - a[2]];
  const n = [
    u[1] * v[2] - u[2] * v[1],
    u[2] * v[0] - u[0] * v[2],
    u[0] * v[1] - u[1] * v[0],
  ];
  const l = Math.hypot(...n) || 1;
  return [n[0] / l, n[1] / l, n[2] / l];
}
