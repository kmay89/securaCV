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

// ── estimator (pure; Node-tested) ───────────────────────────────────────────
//
// Same rule as everything else here: honest geometry or quoted repo guidance,
// never an invented number. So the estimate splits cleanly in two:
//
//   · MEASURED  — filament mass/length/cost come from the STL's own solid
//     volume (signed-tetrahedron sum, exact for a watertight mesh) and the
//     repo's documented density (README §Engineering & materials: "PETG at
//     1.27 g/cm³ … ×0.84 for ASA"). meshVolume on the committed WAP-compact
//     pair reproduces the README's "≈ 12 g/pair" mass budget — same method,
//     so the number is checkable, not asserted (see tests/print_estimate.test.js).
//   · MODELED  — print time and energy come from a transparent physical model
//     (volumetric flow → time, average duty-cycle power → energy) with every
//     assumption named below and surfaced in the UI. These carry an honest
//     ± band; for exact figures, slice in your slicer. This is NOT a slicer
//     and never claims to be — it plans no toolpath.
//
// The machine profile is the user's rig: an Ender 3 V2 (220×220×250 bed) with
// a Micro Swiss 45° direct-drive + all-metal hotend and linear-rail mod. The
// direct drive raises the flow ceiling over the stock Bowden/PTFE setup; the
// power figures are typical *average* draws for the 24 V board holding temp,
// not nameplate peaks (the bed and hotend duty-cycle hard once warm).

export const FILAMENT_DIA = 1.75;                       // mm, standard
const FIL_AREA = Math.PI * (FILAMENT_DIA / 2) ** 2;     // mm² cross-section

export const MATERIALS = {
  // density: g/cm³ (PETG/ASA per the README mass budget). price_kg: typical
  // spool $/kg. speed: default outer/infill print speed (mm/s) the Micro Swiss
  // DD comfortably holds for this material on an Ender 3 V2.
  PLA:  { label: "PLA",       density: 1.24, price_kg: 22, speed: 55, temp: 210, note: "indoor / bench only — creeps ~55 °C" },
  PETG: { label: "PETG",      density: 1.27, price_kg: 25, speed: 45, temp: 235, note: "default; tough, sheltered-outdoor ok" },
  ASA:  { label: "ASA",       density: 1.07, price_kg: 30, speed: 45, temp: 245, note: "UV-stable outdoor; print hot, draft-free" },
  TPU:  { label: "TPU 90–95A", density: 1.21, price_kg: 30, speed: 25, temp: 230, note: "gaskets — 100 % infill, slow" },
};

export const MACHINE = {
  id: "ender3v2-microswiss-dd",
  name: "Ender 3 V2 · Micro Swiss direct-drive + linear rails",
  bed_mm: [220, 220, 250],
  nozzle_mm: 0.4,
  // Average electrical draw while printing (steady state, no enclosure). The
  // bed dominates and duty-cycles; these are holding averages, not peaks.
  power: {
    bed_w: 55,                  // ~180 W peak, ~55 W avg holding 60–70 °C
    hotend_w: 20,               // 40 W cartridge, ~20 W avg holding 210–245 °C
    motors_electronics_w: 18,   // board + 4 steppers + part fan + display
  },
  heatup_wh: 12,                // one-time warm-up energy before motion starts
  heatup_s: 180,               // bed 60→ + nozzle to temp ≈ 3 min before layer 1
  // Volumetric flow ceilings (mm³/s) for the Micro Swiss DD all-metal hotend.
  // Material-limited, not motor-limited — this is what actually caps speed.
  max_flow_mm3_s: { PLA: 13, PETG: 10, ASA: 10, TPU: 3.5 },
  // Wall-clock reality: pure extrusion time under-counts, because short
  // perimeters never reach cruise speed (acceleration/jerk), travel and
  // z-hops don't extrude, and cooling forces minimum layer times. These two
  // knobs are calibrated so the catalog's quoted coupon prints (~15 min clip,
  // ~25 min fit coupon) land inside the model's band.
  motion_efficiency: 0.60,      // sustained fraction of nominal flow
  travel_overhead: 0.12,        // +12 % non-extruding motion
};

export const DEFAULT_SETTINGS = {
  layer_height_mm: 0.2,         // README §Suggested print settings
  line_width_mm: 0.44,          // ~1.1 × a 0.4 mm nozzle
  walls: 3,                     // README suggested (security spec uses 4)
  top_bottom_layers: 5,
  infill_pct: 25,               // README "20–30 %" midpoint
};

export const ELECTRICITY_DEFAULT = 0.17;  // $/kWh — US residential average

// Signed solid volume of a triangle soup, mm³. Exact for a closed mesh:
// Σ (v0 · (v1 × v2)) / 6 over every triangle. abs() so winding can't flip it.
export function meshVolume(mesh) {
  const { pos, idx } = mesh;
  let v6 = 0;
  for (let t = 0; t < idx.length; t += 3) {
    const a = idx[t] * 3, b = idx[t + 1] * 3, c = idx[t + 2] * 3;
    const ax = pos[a], ay = pos[a + 1], az = pos[a + 2];
    const bx = pos[b], by = pos[b + 1], bz = pos[b + 2];
    const cx = pos[c], cy = pos[c + 1], cz = pos[c + 2];
    v6 += ax * (by * cz - bz * cy)
        - ay * (bx * cz - bz * cx)
        + az * (bx * cy - by * cx);
  }
  return Math.abs(v6) / 6;
}

// Total surface area of the mesh, mm² — the perimeter-shell basis.
export function meshArea(mesh) {
  const { pos, idx } = mesh;
  let area = 0;
  for (let t = 0; t < idx.length; t += 3) {
    const a = idx[t] * 3, b = idx[t + 1] * 3, c = idx[t + 2] * 3;
    const ux = pos[b] - pos[a], uy = pos[b + 1] - pos[a + 1], uz = pos[b + 2] - pos[a + 2];
    const vx = pos[c] - pos[a], vy = pos[c + 1] - pos[a + 1], vz = pos[c + 2] - pos[a + 2];
    const cx = uy * vz - uz * vy, cy = uz * vx - ux * vz, cz = ux * vy - uy * vx;
    area += Math.hypot(cx, cy, cz) / 2;
  }
  return area;
}

// Which real material a part prints in. Gaskets are functionally TPU no matter
// what shell plastic you picked; everything else follows the shell selection.
export function materialForPart(part, shellMaterial = "PETG") {
  if (part && /tpu/i.test(part.material || "")) return "TPU";
  return MATERIALS[shellMaterial] ? shellMaterial : "PETG";
}

// The estimate for one part. `geom` = {volume, area, height} in mm/mm²/mm³.
// Returns measured filament figures + a modeled time/energy with a band.
export function estimatePart(geom, opts = {}) {
  const machine = opts.machine || MACHINE;
  const settings = { ...DEFAULT_SETTINGS, ...(opts.settings || {}) };
  const matId = opts.material || "PETG";
  const mat = MATERIALS[matId] || MATERIALS.PETG;
  const elec = opts.electricity ?? ELECTRICITY_DEFAULT;
  const infill = matId === "TPU" ? 1 : Math.max(0, Math.min(1, settings.infill_pct / 100));

  const vSolid = Math.max(0, geom.volume);              // mm³ — the README basis
  const area = Math.max(0, geom.area);

  // Deposited plastic ≤ solid: perimeters/skins print solid, the bulk interior
  // fills at `infill`. Shell depth is the greater of the vertical perimeter
  // band and the horizontal top/bottom skin. Thin-walled parts (every Canary
  // shell) are almost all perimeter, so infill barely trims them — which is
  // exactly why the README's mass budget uses solid volume. Chunky parts
  // (posts, the field case) get the real infill discount.
  const shellDepth = Math.max(
    settings.walls * settings.line_width_mm,
    settings.top_bottom_layers * settings.layer_height_mm,
  );
  const vShell = Math.min(vSolid, area * shellDepth);
  const vDeposited = vShell + (vSolid - vShell) * infill;

  const gramsSolid = (vSolid / 1000) * mat.density;     // 100 % basis (README)
  const grams = (vDeposited / 1000) * mat.density;      // realistic, infilled
  const lengthMm = vDeposited / FIL_AREA;

  // Time: extrusion-limited. Nominal volumetric flow = line × layer × speed,
  // capped at the hotend's material flow ceiling, then derated for real motion.
  const nominalFlow = settings.line_width_mm * settings.layer_height_mm * mat.speed;
  const flowCeil = machine.max_flow_mm3_s[matId] ?? machine.max_flow_mm3_s.PETG;
  const effFlow = Math.min(nominalFlow, flowCeil) * machine.motion_efficiency;
  const extrusionS = vDeposited / effFlow;
  const timeS = extrusionS * (1 + machine.travel_overhead) + machine.heatup_s;

  // Energy: average printing draw across the run, plus the warm-up bump.
  const p = machine.power;
  const avgW = p.bed_w + p.hotend_w + p.motors_electronics_w;
  const energyWh = (avgW * timeS) / 3600 + machine.heatup_wh;
  const energyKWh = energyWh / 1000;

  const filamentCost = (grams / 1000) * mat.price_kg;
  const energyCost = energyKWh * elec;

  return {
    material: matId,
    volumeMm3: vSolid,
    depositedMm3: vDeposited,
    grams,
    gramsSolid,
    lengthMm,
    lengthM: lengthMm / 1000,
    timeS,
    timeLowS: timeS * 0.75,     // honest uncertainty band on the model
    timeHighS: timeS * 1.4,
    energyKWh,
    filamentCost,
    energyCost,
    cost: filamentCost + energyCost,
  };
}

// Aggregate a whole printed set (base + lid + gasket …). Parts print one at a
// time on one machine, so totals sum — that's the real bench time and spend to
// make the set. `parts` = [{ geom, part }]. Returns { parts:[…], total }.
export function estimateSet(parts, opts = {}) {
  const rows = parts.map(({ geom, part }) => ({
    part,
    est: estimatePart(geom, { ...opts, material: materialForPart(part, opts.shellMaterial) }),
  }));
  const total = rows.reduce((acc, r) => {
    acc.grams += r.est.grams;
    acc.gramsSolid += r.est.gramsSolid;
    acc.lengthM += r.est.lengthM;
    acc.timeS += r.est.timeS;
    acc.timeLowS += r.est.timeLowS;
    acc.timeHighS += r.est.timeHighS;
    acc.energyKWh += r.est.energyKWh;
    acc.filamentCost += r.est.filamentCost;
    acc.energyCost += r.est.energyCost;
    acc.cost += r.est.cost;
    return acc;
  }, {
    grams: 0, gramsSolid: 0, lengthM: 0, timeS: 0, timeLowS: 0, timeHighS: 0,
    energyKWh: 0, filamentCost: 0, energyCost: 0, cost: 0,
  });
  return { rows, total };
}

// h/m formatting for a duration in seconds.
export function fmtDuration(s) {
  const m = Math.round(s / 60);
  if (m < 60) return `${m} min`;
  const h = Math.floor(m / 60);
  return `${h} h ${String(m % 60).padStart(2, "0")} min`;
}

// ── build cost: the honest "how much to make this" ─────────────────────────
// The enclosure is ~$1; the device is dominated by its electronics. Sum the
// device BOM (build.json rows: {usd, qty, required}) so the bench can show real
// total cost, not just filament. Firm numbers — these are the repo's own
// priced BOM. See docs/strategy/18-unit-economics-and-production-scale.md.
export function partsCost(rows) {
  let required = 0, optional = 0, count = 0;
  for (const r of rows || []) {
    const usd = Number(r.usd);
    if (!Number.isFinite(usd)) continue;
    const q = Number.isFinite(parseFloat(r.qty)) ? parseFloat(r.qty) : 1;
    if (r.required) { required += usd * q; count += q; } else { optional += usd * q; }
  }
  return { required, optional, count };
}

// A deliberately generic comparator — NOT a claim about any named product.
// A typical cloud camera monetizes a subscription; a Canary is one-time with
// nothing recurring and nothing leaving the house. Numbers are stated
// assumptions, adjustable by the caller.
export const CLOUD_CAM = { hardware: 40, monthly: 8, label: "typical cloud camera" };

// 3-year total cost of ownership: a Canary is its build/buy cost, once; the
// cloud comparator is hardware + subscription × months.
export function tco({ unitCost, years = 3, cloud = CLOUD_CAM } = {}) {
  return {
    years,
    canary: unitCost,
    cloud: cloud.hardware + cloud.monthly * 12 * years,
    cloudLabel: cloud.label,
  };
}

// A starting slicer config, generated from the SAME settings the estimate uses
// so the two can't drift. Emitted in the Slic3r/PrusaSlicer config-bundle
// format (`[print:…]` / `[filament:…]` / `[printer:…]`), which PrusaSlicer,
// OrcaSlicer and SuperSlicer all import via "Import Config". A starting point,
// not a tuned profile — the fit coupon still tunes tolerances for your printer.
export function slicerConfigIni({ machine = MACHINE, settings = DEFAULT_SETTINGS, material = "PETG" } = {}) {
  const s = { ...DEFAULT_SETTINGS, ...settings };
  const mat = MATERIALS[material] || MATERIALS.PETG;
  const [w, d, h] = machine.bed_mm;
  const bedTemp = material === "PLA" ? 60 : 75;
  const L = [
    `# SecuraCV Canary — slicer config (${mat.label})`,
    `# Generated for a ${machine.nozzle_mm} mm nozzle from the print guide's own settings.`,
    `# Starting point for PrusaSlicer / OrcaSlicer / SuperSlicer — import as a config.`,
    ``,
    `[print:SecuraCV Canary ${s.layer_height_mm}mm ${s.walls}-wall]`,
    `layer_height = ${s.layer_height_mm}`,
    `first_layer_height = ${s.layer_height_mm}`,
    `perimeters = ${s.walls}`,
    `top_solid_layers = ${s.top_bottom_layers}`,
    `bottom_solid_layers = ${s.top_bottom_layers}`,
    `fill_density = ${s.infill_pct}%`,
    `fill_pattern = gyroid`,
    `top_fill_pattern = monotonic`,
    `bottom_fill_pattern = monotonic`,
    `extrusion_width = ${s.line_width_mm}`,
    ``,
    `[filament:SecuraCV ${mat.label}]`,
    `filament_type = ${material}`,
    `filament_diameter = ${FILAMENT_DIA}`,
    `temperature = ${mat.temp}`,
    `first_layer_temperature = ${mat.temp}`,
    `bed_temperature = ${bedTemp}`,
    `first_layer_bed_temperature = ${bedTemp}`,
    ``,
    `[printer:${machine.name}]`,
    `printer_technology = FFF`,
    `nozzle_diameter = ${machine.nozzle_mm}`,
    `bed_shape = 0x0,${w}x0,${w}x${d},0x${d}`,
    `max_print_height = ${h}`,
    ``,
  ];
  return L.join("\n");
}
