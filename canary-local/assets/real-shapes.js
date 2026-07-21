// canary-local/assets/real-shapes.js — the device cards, from the real files.
//
// Upgrades a display card's procedural approximation to the ACTUAL parts:
// the OpenSCAD-rendered preview meshes in enclosures/preview/ (drum, bezel
// and stand for the watch; frame, back and stand for the dash — the same
// geometry gen_enclosures.py renders from docs/hardware/enclosure/*.scad).
// Each STL is modeled in its PRINT orientation (z = 0 is the build plate),
// so assembly re-orients per part: the bezel and frame print face-down
// (flip to face the viewer), the stands print flat (tip upright), the drum
// prints back-plate-down (its z is already the device axis).
//
// The procedural builder has already run when this fires — if a fetch
// fails (offline copy without the preview dir), the card simply keeps the
// approximation. Real shapes are an upgrade, never a dependency.

import { parseSTL } from "./stl.js";
import { M4, screenPlane } from "./scene3d.js";
import { activeFinish } from "./finishes.js";

const PREVIEW = "enclosures/preview/";
const ENC = "../docs/hardware/enclosure/";   // print-validated library (same base the enclosure lab uses)
// One finish family across ALL five cards (the gallery used to split dark
// graphite displays vs bone witnesses — same product line, same filament).
// The active finish (finishes.js) drives both tones; these helpers read it
// per-build so a finish swap re-lines every real-shape card.
const shell = () => activeFinish().shell;    // primary printed shell
const shell2 = () => activeFinish().shell2;  // stands & rear covers (depth cue)
const GLASS_EDGE = [0.05, 0.05, 0.06];

// Caches the PROMISE, not the mesh: the card grid and an open sheet load
// the same files concurrently, and one request should serve both. A
// failed load evicts itself so a later card can retry (review catch).
const cache = new Map(); // url → Promise<{mesh, bbox}>
function load(file, base = PREVIEW) {
  const url = base + file;
  if (cache.has(url)) return cache.get(url);
  const p = (async () => {
    try {
      const res = await fetch(url);
      if (!res.ok) throw new Error(`HTTP ${res.status}`);
      return parseSTL(await res.arrayBuffer());
    } catch (err) {
      cache.delete(url);
      throw err;
    }
  })();
  cache.set(url, p);
  return p;
}

// The finish role a placed part animates with, INFERRED from its colour so a
// caller can't silently mislabel one: shell2()/shell() return the stable
// per-finish arrays, so an identity match names the role. Anything else (a
// literal colour) gets no role and keeps its colour through the showcase.
function roleFor(color, explicit) {
  if (explicit !== undefined) return explicit;
  const f = activeFinish();
  return color === f.shell2 ? "shell2" : color === f.shell ? "shell" : null;
}

// Center a mesh on its bbox, then pre-rotate (print → device orientation),
// then place. Baking center+pre into the model matrix keeps the mesh data
// shared between cards (the cache holds one copy per file).
function place(scene, parsed, { pre = M4.ident(), at = M4.ident(), color = shell(), gloss = 0.22, role }) {
  const c = parsed.bbox.center;
  const model = M4.mul(at, M4.mul(pre, M4.translate(-c[0], -c[1], -c[2])));
  scene.addMesh(parsed.mesh, { color, gloss, model, role: roleFor(color, role) });
}

const rotXpi = M4.rotX(Math.PI);          // face-down print → face forward
const standUp = M4.rotX(-Math.PI / 2);    // flat print → upright (z → y)

// ── seated placement, derived instead of eyeballed ──────────────────────
// Both display products are posed in the STAND's print frame (z up, +y =
// stand rear), then the whole group is turned upright once:
//   world = G · T(devicePos − standCenter) · R_device · T(−meshCenter)
// so every part's seat comes from the SCAD's own cradle geometry, not from
// per-part hand offsets. G = standUp (print z → screen y) + a vertical trim.
function seatPart(scene, parsed, { G, D, R = M4.ident(), color = shell(), gloss = 0.22, role }) {
  const c = parsed.bbox.center;
  const model = M4.mul(G, M4.mul(M4.translate(D[0], D[1], D[2]),
    M4.mul(R, M4.translate(-c[0], -c[1], -c[2]))));
  scene.addMesh(parsed.mesh, { color, gloss, model, role: roleFor(color, role) });
}

// canary_watch_station.scad (v0.2) — the drum sinks pocket_dep = 11 into the
// stand's divot, a cylindrical recess bored NORMAL to the 25°-reclined face.
// The seat comes from the scad's own echo: DRUM SEAT pos [0, 3.03, 27.36]
// rot [65, 0, 0] — drum +z along the pocket axis a = (0, −sin65°, cos65°),
// no azimuth flip (the USB slot at 270° lands in the stand's chin slot).
// Drum spans 0…21 along a; the snap bezel's face caps it at 23.2.
async function realWatch(scene) {
  const [drum, bezel, stand] = await Promise.all([
    load("canary_watch_station_drum.stl"),
    load("canary_watch_station_bezel.stl"),
    load("canary_watch_station_stand.stl"),
  ]);
  scene.clearParts();
  const G = M4.mul(M4.translate(0, -14, 0), standUp);   // upright + vertical trim
  const cS = stand.bbox.center;                          // ≈ (0, 4.07, 29.8)
  const A = 65 * Math.PI / 180;                          // pocket axis = Rx(65°)·ẑ
  const Ra = M4.rotX(A);
  const p0 = [0, 3.03 - cS[1], 27.36 - cS[2]];           // scad's drum seat, stand-centred
  const a = [0, -Math.sin(A), Math.cos(A)];
  const along = (s) => [p0[0], p0[1] + a[1] * s, p0[2] + a[2] * s];
  seatPart(scene, stand, { G, D: [0, 0, 0], color: shell2(), gloss: 0.18 });
  seatPart(scene, drum, { G, D: along(10.5), R: Ra, gloss: 0.22 });           // drum centre at s=10.5
  seatPart(scene, bezel, { G, D: along(20.1), R: M4.mul(Ra, rotXpi), gloss: 0.3 }); // face-down print → face out
  scene.addMesh(screenPlane(37.5, 37.5, true), {         // the Ø37.7 glass behind the Ø39.4 aperture
    screen: true,
    model: M4.mul(G, M4.mul(M4.translate(...along(20.5)), Ra)),
  });
  scene.dist = 185;
}

// canary_dash_display.scad — the panel sits in the stand's channel, back
// against the 25° fin. Bottom-rear edge at (y −5.19, z 4) with the back
// plane on the fin face (the SCAD's own derivation); module centre works
// out to (0, 4.04, 42.73) in the stand frame for the 118.1 × 78 × 16 body.
// (As committed, the 6 mm rear rail's top corner pokes ~2.4 mm into that
// plane — flagged in the scad; the overlap is buried inside the channel.)
async function realDash(scene) {
  const [frame, back, stand] = await Promise.all([
    load("canary_dash_display_frame.stl"),
    load("canary_dash_display_back.stl"),
    load("canary_dash_display_stand.stl"),
  ]);
  scene.clearParts();
  const G = M4.mul(M4.translate(0, -16, 0), standUp);
  const cS = stand.bbox.center;                          // (0, 0, 21.87)
  const A = 65 * Math.PI / 180;                          // recline: module ẑ = Rx(65°)·ẑ
  const Ra = M4.rotX(A);
  const C = [0, 4.04 - cS[1], 42.73 - cS[2]];            // module centre (derivation above)
  const w = [0, -Math.cos(25 * Math.PI / 180), Math.sin(25 * Math.PI / 180)]; // face normal
  const off = (s) => [C[0], C[1] + w[1] * s, C[2] + w[2] * s];
  seatPart(scene, stand, { G, D: [0, 0, 0], color: shell2(), gloss: 0.18 });
  seatPart(scene, frame, { G, D: off(1.2), R: M4.mul(Ra, rotXpi), gloss: 0.22 });  // face-down print → face out
  seatPart(scene, back, { G, D: off(-6.8), R: Ra, color: shell2(), gloss: 0.22 });
  scene.addMesh(screenPlane(101.3, 61.2, false), {       // glass behind the 2.5 mm bezel lip
    screen: true,
    model: M4.mul(G, M4.mul(M4.translate(...off(5.55)), Ra)),
  });
  scene.dist = 270;
}

// The witnesses: their PRINT-VALIDATED shells (the exact STLs a builder
// slices), closed as two-part boxes. Print orientation convention across
// the library: fronts/lids print A-face-down (flip to face the viewer),
// backs/bases print outer-face-down (already facing away after centering).
// Depths stack from the parts' own bounding boxes — no invented numbers.
async function realTwoPart(scene, frontFile, backFile, { color = shell(), dist = 130, nest = 1.6, extras = null } = {}) {
  const [front, back] = await Promise.all([load(frontFile, ENC), load(backFile, ENC)]);
  scene.clearParts();
  const fz = front.bbox.size[2], bz = back.bbox.size[2];
  // the front's lip nests INTO the back by ~nest mm — flush-stacking the
  // two bboxes showed a phantom seam gap no real build has
  place(scene, back, { at: M4.translate(0, 0, -fz / 2 + nest / 2), color: shell2(), gloss: 0.25 });
  place(scene, front, { pre: rotXpi, at: M4.translate(0, 0, bz / 2 - nest / 2), color, gloss: 0.25 });
  if (extras) await extras();
  scene.dist = dist;
}

const REAL = {
  "canary-display-watch": realWatch,
  "canary-display-dash": realDash,
  "canary-vision": (scene) =>
    realTwoPart(scene, "canary_vision_enclosure_xiao_indoor_front.stl",
                "canary_vision_enclosure_xiao_indoor_back.stl", { dist: 140 }),
  "canary-wap": (scene) =>
    realTwoPart(scene, "canary_wap_enclosure_compact_lid.stl",
                "canary_wap_enclosure_compact_base.stl", { dist: 150 }),
  "canary-sense": (scene) =>
    realTwoPart(scene, "canary_sense_front.stl", "canary_sense_back.stl",
                { dist: 140 }),
};

/** Swap a card to its real printed geometry. Fire-and-forget for most
 * callers; resolves true/false (real/approximation kept) for callers that
 * add props afterwards — clearParts() would wipe anything added earlier. */
export function upgradeRealShape(scene, devId) {
  const fn = REAL[devId];
  if (!fn) return Promise.resolve(false);
  return fn(scene).then(() => true).catch(() => false);
}
