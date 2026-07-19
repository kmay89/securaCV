// canary-local/assets/print-guide.js — how this part wants to be printed.
//
// A guide, not a slicer: every visual here is honest geometry or quoted
// repo guidance, never an invented toolpath.
//
//  · The build-plate view places each part exactly as modeled — the
//    .scad sources put z=0 ON the plate in the documented print
//    orientation ("prints face-down", "open-face-up", "flat, no
//    supports"), so "as modeled" IS the recommendation.
//  · The layer scrub really slices the mesh: the shader clips above the
//    current layer and the bright contour is the true cross-section at
//    that height (plane–triangle intersection, sliceSegments below).
//  · Overhang tint = face normals steeper than 45° pointing at the
//    plate, above the first layers — the classic needs-support signal.
//    The catalog's parts are designed support-free; if you see red
//    after reorienting a part in your slicer, that's the lesson.
//  · The settings card quotes docs/hardware/enclosure/README.md
//    (§Suggested print settings) and each part's own orientation note.

// ── slicing (pure; Node-tested) ─────────────────────────────────────────
// Intersect every triangle with the plane z = sliceZ; return flat
// [x,y,z, x,y,z, ...] positions, one segment (2 points) per crossing tri.
export function sliceSegments(mesh, sliceZ) {
  const { pos, idx } = mesh;
  const out = [];
  for (let t = 0; t < idx.length; t += 3) {
    const pts = [idx[t], idx[t + 1], idx[t + 2]].map((i) => [
      pos[i * 3], pos[i * 3 + 1], pos[i * 3 + 2],
    ]);
    const hits = [];
    for (let e = 0; e < 3; e++) {
      const a = pts[e], b = pts[(e + 1) % 3];
      const da = a[2] - sliceZ, db = b[2] - sliceZ;
      if ((da > 0 && db > 0) || (da < 0 && db < 0)) continue;
      const denom = da - db;
      if (Math.abs(denom) < 1e-9) continue; // edge lies in plane; skip (neighbors supply it)
      const s = da / denom;
      const pnt = [a[0] + (b[0] - a[0]) * s, a[1] + (b[1] - a[1]) * s, sliceZ];
      // A vertex exactly on the plane is hit by both of its edges —
      // dedupe or the duplicate eats the real segment (contour gaps).
      if (!hits.some((h) => Math.hypot(h[0] - pnt[0], h[1] - pnt[1]) < 1e-5)) {
        hits.push(pnt);
      }
    }
    if (hits.length >= 2) {
      out.push(...hits[0], ...hits[1]);
    }
  }
  return out;
}

export function segmentsBuilder(flatPositions) {
  const n = flatPositions.length / 3;
  return {
    pos: flatPositions,
    nrm: new Array(flatPositions.length).fill(0),
    uv: new Array(n * 2).fill(0),
    idx: Array.from({ length: n }, (_, i) => i),
  };
}

// ── build plate (grid every 10 mm, like a slicer's bed) ─────────────────
export function plateBuilder(halfW, halfD) {
  const posL = [];
  const step = 10;
  for (let x = -Math.floor(halfW / step) * step; x <= halfW; x += step) {
    posL.push(x, -halfD, 0, x, halfD, 0);
  }
  for (let y = -Math.floor(halfD / step) * step; y <= halfD; y += step) {
    posL.push(-halfW, y, 0, halfW, y, 0);
  }
  return segmentsBuilder(posL);
}

export function plateSlabBuilder(halfW, halfD) {
  // A thin dark slab just under the grid so the plate reads as a surface.
  const z = -0.6;
  const pos = [
    -halfW, -halfD, z,  halfW, -halfD, z,  halfW, halfD, z,
    -halfW, -halfD, z,  halfW, halfD, z,  -halfW, halfD, z,
  ];
  return {
    pos,
    nrm: [0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1],
    uv: new Array(12).fill(0),
    idx: [0, 1, 2, 3, 4, 5],
  };
}
