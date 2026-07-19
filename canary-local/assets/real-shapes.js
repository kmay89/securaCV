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

const PREVIEW = "enclosures/preview/";
const ENC = "../docs/hardware/enclosure/";   // print-validated library (same base the enclosure lab uses)
const SHELL = [0.16, 0.17, 0.19];
const SHELL_LIGHT = [0.90, 0.87, 0.80];

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

// Center a mesh on its bbox, then pre-rotate (print → device orientation),
// then place. Baking center+pre into the model matrix keeps the mesh data
// shared between cards (the cache holds one copy per file).
function place(scene, parsed, { pre = M4.ident(), at = M4.ident(), color = SHELL, gloss = 0.22 }) {
  const c = parsed.bbox.center;
  const model = M4.mul(at, M4.mul(pre, M4.translate(-c[0], -c[1], -c[2])));
  scene.addMesh(parsed.mesh, { color, gloss, model });
}

const rotXpi = M4.rotX(Math.PI);          // face-down print → face forward
const standUp = M4.rotX(-Math.PI / 2);    // flat print → upright (z → y)

// canary_watch_station.scad: drum Ø52 × 14.8 back-down; bezel Ø52 × ~7
// face-down; stand upright with the 25° pocket. Device envelope matches
// the procedural card (back −10.9 … front +10.9, lean 25°).
async function realWatch(scene) {
  const [drum, bezel, stand] = await Promise.all([
    load("canary_watch_station_drum.stl"),
    load("canary_watch_station_bezel.stl"),
    load("canary_watch_station_stand.stl"),
  ]);
  const lean = M4.rotX((-25 * Math.PI) / 180);
  const at = (dz) => M4.mul(lean, M4.translate(0, 0, dz));
  scene.clearParts();
  place(scene, drum, { at: at(-3.5), gloss: 0.22 });
  place(scene, bezel, { pre: rotXpi, at: at(7.4), gloss: 0.3 });
  scene.addMesh(screenPlane(34, 34, true), { screen: true, model: at(10.2) });
  place(scene, stand, { pre: standUp, at: M4.translate(0, -26, -2), gloss: 0.18 });
  scene.dist = 165;
}

// canary_dash_display.scad: frame 113.7 × 73.6 × 13.6 face-down; back
// × 2.4 outer-face-down (already outward after centering); stand flat.
async function realDash(scene) {
  const [frame, back, stand] = await Promise.all([
    load("canary_dash_display_frame.stl"),
    load("canary_dash_display_back.stl"),
    load("canary_dash_display_stand.stl"),
  ]);
  const tilt = M4.rotX((-25 * Math.PI) / 180);
  const at = (dz) => M4.mul(tilt, M4.translate(0, 0, dz));
  scene.clearParts();
  place(scene, frame, { pre: rotXpi, at: at(1.2), gloss: 0.22 });
  place(scene, back, { at: at(-6.8), gloss: 0.22 });
  // Glass sits behind the frame's 2.5 mm lip — recessed, like the part.
  scene.addMesh(screenPlane(101.3, 61.2, false), { screen: true, model: at(6.6) });
  place(scene, stand, { pre: standUp, at: M4.translate(0, -44, -6), gloss: 0.18 });
  scene.dist = 260;
}

// The witnesses: their PRINT-VALIDATED shells (the exact STLs a builder
// slices), closed as two-part boxes. Print orientation convention across
// the library: fronts/lids print A-face-down (flip to face the viewer),
// backs/bases print outer-face-down (already facing away after centering).
// Depths stack from the parts' own bounding boxes — no invented numbers.
async function realTwoPart(scene, frontFile, backFile, { color = SHELL_LIGHT, dist = 130, extras = null } = {}) {
  const [front, back] = await Promise.all([load(frontFile, ENC), load(backFile, ENC)]);
  scene.clearParts();
  const fz = front.bbox.size[2], bz = back.bbox.size[2];
  place(scene, back, { at: M4.translate(0, 0, -fz / 2), color, gloss: 0.25 });
  place(scene, front, { pre: rotXpi, at: M4.translate(0, 0, bz / 2), color, gloss: 0.25 });
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

/** Fire-and-forget: swap a card to its real printed geometry. */
export function upgradeRealShape(scene, devId) {
  const fn = REAL[devId];
  if (!fn) return;
  fn(scene).catch(() => {}); // keep the approximation on any failure
}
