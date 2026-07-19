// canary-local/assets/assembly-rules.js — how an assembly is ALLOWED to go together.
//
// The Assemble tab used to trust whatever order assembly.json authored. This
// module encodes the drafting-room rules a real work instruction follows, so
// choreography that violates them fails CI instead of teaching a wrong build:
//
//   1. A support (stand/bracket) is the base (step 0) or the finale — never
//      mid-build.
//   2. Internal components (boards, cells, magnets, pipes) install before the
//      shell that encloses them closes.
//   3. Seals (gaskets) seat before their mating shell closes.
//   4. Fasteners drive LAST among the parts they join — after every shell,
//      seal and internal part — and sit OUTERMOST in the exploded view
//      (|explode| strictly greater than the shells they clamp), the way a
//      proper exploded drawing stacks its hardware at the far end of the
//      assembly axis.
//   5. External accessories (shields, covers that clip on outside) come after
//      the fasteners.
//
// It also owns item numbering (balloons ↔ parts list, one item per distinct
// part, in build order — the ASME Y14-style pairing the lab renders) and the
// caliper readout: every dimension in mm, decimal inches, and nearest-1/64
// fractional inches. DOM-free on purpose (Node-tested).

// ── classification ──────────────────────────────────────────────────────────
// A part may declare `class` explicitly; otherwise it is inferred from what
// it IS (source/proc) and what it is called. Classes:
//   support · internal · shell · seal · fastener · external
const FASTENER_PROCS = new Set(["screw", "insert"]);
const INTERNAL_PROCS = new Set(["lipo", "magnet", "lightPipe", "disc", "vent", "pcb", "lcdPanel"]);

export function classifyPart(p) {
  if (p.class) return p.class;
  if (p.source === "proc" && FASTENER_PROCS.has(p.part)) return "fastener";
  if (p.source === "proc" && INTERNAL_PROCS.has(p.part)) return "internal";
  if (p.source === "board") return "internal";
  const name = `${p.id} ${p.name || ""} ${p.file || ""}`.toLowerCase();
  if (/gasket|o-ring|oring|seal/.test(name)) return "seal";
  if (/stand|cradle|bracket|mount(?!ed)/.test(name)) return "support";
  if (/shield|visor|hood|clip-on/.test(name)) return "external";
  if (p.source === "stl") return "shell";
  return "internal";
}

const mag = (v) => Math.hypot(...(v || [0, 0, 0]));

// ── the order gate ──────────────────────────────────────────────────────────
// device: one entry of assembly.json's `devices`. Returns { ok, violations }.
export function validateDevice(device) {
  const violations = [];
  const parts = (device.parts || []).map((p) => ({ p, cls: classifyPart(p) }));
  const of = (cls) => parts.filter((x) => x.cls === cls);

  const shells = of("shell");
  const lastShellStep = Math.max(...shells.map((x) => x.p.step), -1);
  const fasteners = of("fastener");
  const firstFastenerStep = fasteners.length
    ? Math.min(...fasteners.map((x) => x.p.step)) : Infinity;

  // 1. supports open or close the build
  for (const { p } of of("support")) {
    const maxStep = Math.max(...parts.map((x) => x.p.step));
    if (p.step !== 0 && p.step !== maxStep) {
      violations.push(`${p.id}: a support must be the base (step 0) or the finale, not step ${p.step}`);
    }
  }
  // 2. internals before the enclosure closes
  for (const { p } of of("internal")) {
    if (lastShellStep >= 0 && p.step > lastShellStep) {
      violations.push(`${p.id}: internal part installs at step ${p.step}, after the last shell closes (step ${lastShellStep})`);
    }
  }
  // 3. seals before the closing shell
  for (const { p } of of("seal")) {
    if (lastShellStep >= 0 && p.step >= lastShellStep) {
      violations.push(`${p.id}: the seal must seat before the shell that compresses it (step ${lastShellStep})`);
    }
  }
  // 4. fasteners after everything they clamp, outermost in the explode
  const maxClampedExplode = Math.max(
    ...parts.filter((x) => x.cls === "shell" || x.cls === "seal" || x.cls === "internal")
      .map((x) => mag(x.p.explode)), 0);
  for (const { p } of fasteners) {
    if (p.step <= lastShellStep) {
      violations.push(`${p.id}: fasteners drive after the shells they join (step ${p.step} ≤ ${lastShellStep})`);
    }
    for (const { p: q, } of parts.filter((x) => x.cls === "internal" || x.cls === "seal")) {
      if (p.step <= q.step) {
        violations.push(`${p.id}: fastener at step ${p.step} but ${q.id} installs at step ${q.step}`);
      }
    }
    if (mag(p.explode) <= maxClampedExplode) {
      violations.push(`${p.id}: fasteners sit outermost in the exploded view (|explode| ${mag(p.explode).toFixed(0)} ≤ ${maxClampedExplode.toFixed(0)})`);
    }
  }
  // 5. externals after the fasteners
  for (const { p } of of("external")) {
    if (p.step < firstFastenerStep) {
      violations.push(`${p.id}: external accessory fits after the fasteners (step ${p.step} < ${firstFastenerStep})`);
    }
  }
  return { ok: violations.length === 0, violations };
}

// ── item numbering: balloons ↔ parts list ───────────────────────────────────
// One item per distinct part name, numbered in build order (instanced
// fasteners collapse to a single item with their qty) — the balloon on the
// drawing and the row in the list share the number.
export function itemize(device) {
  const seen = new Map(); // name → item
  const items = [];
  for (const p of [...(device.parts || [])].sort((a, b) => a.step - b.step)) {
    if (seen.has(p.name)) continue;
    const item = {
      n: items.length + 1,
      name: p.name,
      ref: p.ref || null,
      qty: p.qty || 1,
      cls: classifyPart(p),
      partId: p.id,
      step: p.step,
    };
    seen.set(p.name, item);
    items.push(item);
  }
  return items;
}

// ── the caliper: mm · decimal inch · fractional inch ────────────────────────
export const UNIT_MODES = ["all", "mm", "in"];
const MM_PER_IN = 25.4;

// nearest 1/64, reduced — "2 3/64″", "13/32″", "3″"
export function fracInch(mm) {
  const inches = mm / MM_PER_IN;
  let n = Math.round(inches * 64), d = 64;
  while (n % 2 === 0 && n > 0 && d > 1) { n /= 2; d /= 2; }
  const whole = Math.floor(n / d);
  const num = n - whole * d;
  if (num === 0) return `${whole}`;
  return whole ? `${whole} ${num}/${d}` : `${num}/${d}`;
}

export function fmtLen(mm, mode = "all") {
  const inch = mm / MM_PER_IN;
  const mmS = `${mm.toFixed(1)} mm`;
  const inS = `${inch.toFixed(3)}″`;
  const frS = `${fracInch(mm)}″`;
  if (mode === "mm") return mmS;
  if (mode === "in") return `${inS} · ${frS}`;
  return `${mmS} · ${inS} · ${frS}`;
}

// W × H × D triple — mm block, inch block, fraction block, kept in axis order
export function fmtDims(dims, mode = "all") {
  const [w, h, d] = dims;
  const mmS = `${w.toFixed(1)} × ${h.toFixed(1)} × ${d.toFixed(1)} mm`;
  const inS = `${(w / MM_PER_IN).toFixed(3)} × ${(h / MM_PER_IN).toFixed(3)} × ${(d / MM_PER_IN).toFixed(3)} in`;
  const frS = `${fracInch(w)} × ${fracInch(h)} × ${fracInch(d)} in`;
  if (mode === "mm") return mmS;
  if (mode === "in") return `${inS}  ·  ${frS}`;
  return `${mmS}  ·  ${inS}  ·  ${frS}`;
}
