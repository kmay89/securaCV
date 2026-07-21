// canary-local/assets/slicer.js — optional real-slicer bridge (Kiri:Moto).
//
// The print-guide estimate (print-guide.js) is always-on, offline, and exact
// on filament mass. The one thing a transparent model can only *approximate*
// is print TIME — real toolpath time depends on acceleration, travel and
// cooling that only a slicer computes. This module is the seam for handing
// that one number to a real slicer when one is available, WITHOUT weakening
// any of the lab's promises:
//
//   · Offline / "nothing phones anywhere" — the engine is loaded ONLY from a
//     locally vendored path (assets/vendor/kiri/), exactly like esptool-js.
//     Nothing here ever reaches out to grid.space or any CDN. If the engine
//     isn't vendored, this module quietly reports "unavailable".
//   · Fails gracefully — every path is feature-detected, timed out, and
//     wrapped; any failure (not vendored, load error, engine throw, timeout,
//     unparseable output) returns null and the UI keeps the honest estimate.
//   · Doesn't rot — the device/process mapping and the g-code time parse are
//     pure and Node-tested against Kiri:Moto's real field names and its actual
//     output format (`; --- print time: Ns ---`, from src/kiri/mode/fdm/work/
//     export.js). If a future Kiri build changes the contract, the parse
//     fails closed → fallback, never a wrong number.
//
// Vendoring the engine is a deliberate, documented step — see
// assets/vendor/kiri/README.md and tools/vendor_kiri.sh. Kiri:Moto is a
// build-from-source app, not a single-file library, so its bundle is added by
// that step, not committed casually. Its slicing pool also needs
// SharedArrayBuffer (a cross-origin-isolated page), so if the engine loads but
// the page isn't isolated, the slice throws and we fall back — same as absent.

import { MACHINE, MATERIALS, DEFAULT_SETTINGS } from "./print-guide.js";

// Where a vendored engine would live. Resolved relative to this module so it
// works wherever the site is served from — and it is always same-origin.
export const ENGINE_URL = new URL("./vendor/kiri/engine.js", import.meta.url);

const SLICE_TIMEOUT_MS = 25000;

// ── pure: our machine/material → Kiri:Moto FDM settings ─────────────────────
// Field names verified against GridSpace/grid-apps src/kiri/mode/fdm.

export function kiriDevice(machine = MACHINE) {
  const [w, d, h] = machine.bed_mm;
  return {
    deviceName: machine.name,
    bedWidth: w,
    bedDepth: d,
    maxHeight: h,
    extruders: [{ extNozzle: machine.nozzle_mm, extFilament: 1.75 }],
    gcodeFan: [],
    gcodePre: [],
    gcodePost: [],
    originCenter: true,
  };
}

export function kiriProcess(settings = DEFAULT_SETTINGS, materialId = "PETG") {
  const s = { ...DEFAULT_SETTINGS, ...settings };
  const mat = MATERIALS[materialId] || MATERIALS.PETG;
  const infill = materialId === "TPU" ? 1 : Math.max(0, Math.min(1, s.infill_pct / 100));
  return {
    sliceHeight: s.layer_height_mm,
    sliceShells: s.walls,
    sliceFillSparse: infill,
    sliceTopLayers: s.top_bottom_layers,
    sliceBottomLayers: s.top_bottom_layers,
    outputFeedrate: mat.speed,
    firstLayerRate: Math.round(mat.speed * 0.5),
    outputTemp: mat.temp,
    outputBedTemp: materialId === "PLA" ? 60 : 75,
    outputOriginCenter: true,
  };
}

// Kiri:Moto's FDM exporter writes exactly `; --- print time: <sec>s ---`
// (src/kiri/mode/fdm/work/export.js). Parse that; also accept a couple of
// slicer-agnostic fallbacks. Returns seconds, or null if nothing sane is found.
export function parseGcodeTime(gcode) {
  if (typeof gcode !== "string" || !gcode) return null;
  const patterns = [
    /;\s*-*\s*print time:\s*([\d.]+)\s*s/i,            // Kiri:Moto FDM
    /;\s*estimated printing time[^\d]*(?:(\d+)h)?\s*(?:(\d+)m)?\s*(?:(\d+)s)?/i, // Prusa/Cura style
  ];
  const m0 = gcode.match(patterns[0]);
  if (m0) {
    const s = parseFloat(m0[1]);
    return Number.isFinite(s) && s > 0 ? s : null;
  }
  const m1 = gcode.match(patterns[1]);
  if (m1 && (m1[1] || m1[2] || m1[3])) {
    const s = (+m1[1] || 0) * 3600 + (+m1[2] || 0) * 60 + (+m1[3] || 0);
    return s > 0 ? s : null;
  }
  return null;
}

// A slice result is trustworthy only if it's a positive, finite, plausible
// number of seconds. Anything else fails closed.
export function validSliceSeconds(v) {
  return Number.isFinite(v) && v > 0 && v < 1000 * 3600;
}

// ── browser: load the vendored engine (cached), feature-detected ────────────
let _enginePromise;
export function loadEngine() {
  if (_enginePromise) return _enginePromise;
  _enginePromise = (async () => {
    try {
      // ESM bundle exporting a factory …
      const mod = await import(/* @vite-ignore */ ENGINE_URL.href);
      const factory = mod?.newEngine || mod?.default?.newEngine || mod?.kiri?.newEngine;
      if (typeof factory === "function") return factory;
    } catch { /* not an ESM bundle, or not vendored — try the global */ }
    // …or a classic script that exposes `kiri` on the global.
    if (typeof globalThis !== "undefined" && globalThis.kiri?.newEngine) {
      return globalThis.kiri.newEngine.bind(globalThis.kiri);
    }
    return null;
  })();
  return _enginePromise;
}

export async function slicerAvailable() {
  return (await loadEngine()) != null;
}

// Slice one STL's real print time. Resolves to seconds, or null on ANY failure
// (not vendored, load/slice error, timeout, unparseable). Never throws to the
// caller — the estimate is always there to fall back on.
export async function sliceSeconds(stlArrayBuffer, opts = {}) {
  const factory = await loadEngine();
  if (!factory) return null;
  const device = kiriDevice(opts.machine);
  const process = kiriProcess(opts.settings, opts.material);
  const run = (async () => {
    const eng = factory();
    eng.setMode?.("FDM");
    eng.setDevice?.(device);
    eng.setProcess?.(process);
    await eng.parse(new Uint8Array(stlArrayBuffer));
    await eng.slice();
    await eng.prepare();
    const gcode = await eng.export();
    const t = parseGcodeTime(gcode);
    return validSliceSeconds(t) ? t : null;
  })();
  const timeout = new Promise((res) => setTimeout(() => res(null), opts.timeoutMs || SLICE_TIMEOUT_MS));
  try {
    return await Promise.race([run, timeout]);
  } catch {
    return null;
  }
}
