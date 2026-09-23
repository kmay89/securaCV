// canary-local/assets/body-dims.js — the Body row's numbers, from the CAD.
//
// The Lab's device sheet used to print a caliper row from registry.json's
// hand-typed `body_mm` — a third copy of numbers the fleet-figure ledger
// already derives, and one that had drifted: the Dash row said
// 113.7 x 73.6 x 16.0 while its case (canary_dash_display.scad) had grown
// corner screw lobes, a thicker back and dock pads. It now reads the device's
// figure in canary-local/devices/figures.json (gen_figures.mjs, generated from
// the CAD and CI-gated), says where the millimeters came from, and reads the
// stand recline off the case's own Customizer knob. Pure functions, so the
// tests exercise exactly what the page runs.

// The device's fleet figure: `device.<id>` where one exists, else the one
// device figure drawn for it (the ledger's `device`).
export function deviceFigure(ledger, devId) {
  const figs = ledger?.figures;
  if (!Array.isArray(figs)) return null;
  return figs.find((f) => f.id === `device.${devId}`)
    || figs.find((f) => f.role === "device" && f.device === devId)
    || null;
}

// Where a figure's millimeters came from, said next to them (figures.json
// dims_source) — a sketch must never read like a measurement.
export const DIMS_SOURCE = {
  "stl": "measured off the committed STLs",
  "assembled-cad": "measured off the CAD · not print-validated",
  "board-cad": "the vendor board, not a case",
  "sketch": "sketch · an estimate",
};

// The stand recline, read off the case's own Customizer knob: the registry
// names the knob (`stand_knob`), enclosures.json carries the .scad literal
// (for the Watch, the literal its manifest owns and gen_cad_params.py proves).
export function standTilt(dev, enclosures) {
  if (!dev.stand_knob || !dev.enclosure) return null;
  const scad = enclosures?.scads?.[dev.enclosure.split("/").pop()];
  for (const g of scad?.groups || []) {
    for (const p of g.params || []) {
      if (p.name !== dev.stand_knob) continue;
      const v = Number(p.default);
      return Number.isFinite(v) ? v : null;
    }
  }
  return null;
}

// The Body row's text, from the figure's envelope — round where the glass is
// round and the case is as wide as it is tall (the Watch's drum). `f` formats
// one length in the viewer's unit mode.
export function bodyText(dev, fig, tilt, f) {
  const e = fig.envelope_mm;
  const round = !!dev.glass?.round && Math.abs(e.w - e.h) < 0.05;
  const size = round
    ? `Ø ${f(e.w)}  ×  ${f(e.d)} deep`
    : `${f(e.w)}  ×  ${f(e.h)}  ×  ${f(e.d)}`;
  const stand = tilt != null ? ` · stand ${tilt}°` : "";
  const src = DIMS_SOURCE[fig.dims_source] || fig.dims_source;
  return `${size}${stand} · ${src}`;
}
