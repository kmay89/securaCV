// Pack the box before you tape it. A port of the website's scripts/box-fit.mjs
// (the canonical method, kept in lockstep: tests/fixtures/boxfit_expected.json
// is that script's own output on eighteen orders, and tests/packing.test.mjs
// holds this file to it). The data is the website's shipping.json — the box
// catalog with inner dimensions, each SKU's packed parcel, the fill factor,
// the dimensional-weight divisor and the Ground Advantage rate tiers — read
// like the catalog, never retyped here.
//
// The method, plainly: First-Fit Decreasing (largest parcel first, into the
// first box it fits); a box "fits" a set when every parcel fits by sorted
// dimensions, the summed volume is under the box's usable volume (inner
// volume × fill_efficiency, the honest stand-in for the air between parcels)
// and the weight is under the box max; the plan is the cheaper of "the whole
// order in one box" and "open boxes greedily by cost per packed volume", ties
// to fewer boxes; a box is priced by billable weight — the greater of actual
// and dimensional weight (volume ÷ divisor, in pounds) — the way the carrier
// bills it, and a flat-rate box by its flat price. Costs are estimates for a
// representative zone; the label prints the real number.

const dims = (p) => [p.l, p.w, p.h].sort((a, b) => b - a);
const volume = (p) => p.l * p.w * p.h;
const round2 = (n) => Math.round(n * 100) / 100;

export function dimWeightOz(box, ship) {
  // Dimensional weight applies only above a minimum volume when the data says so (USPS: one
  // cubic foot, 1728 in³, for Ground Advantage and Priority); without the field it always applies,
  // which is what the website's tool did before the field existed.
  const min = Number(ship.dim_weight_min_cubic_in) || 0;
  if (volume(box.inner) < min) return 0;
  return (volume(box.inner) / ship.dim_weight_divisor) * 16;
}
export function billableOz(box, actualOz, ship) { return box.pricing === 'flat' ? actualOz : Math.max(actualOz, dimWeightOz(box, ship)); }
export function boxCost(box, actualOz, ship) {
  if (box.pricing === 'flat') return box.flat_usd;
  const oz = billableOz(box, actualOz, ship);
  for (const tier of ship.ground_advantage_rate_tiers) if (oz <= tier.max_oz) return tier.usd;
  return ship.ground_advantage_rate_tiers.at(-1).usd;
}
export function parcelFitsBox(parcel, box) { const p = dims(parcel), b = dims(box.inner); return p[0] <= b[0] && p[1] <= b[1] && p[2] <= b[2]; }
export function setFitsBox(parcels, box, ship) {
  const usable = volume(box.inner) * ship.fill_efficiency;
  if (parcels.reduce((a, p) => a + p.weight_oz, 0) > box.max_weight_oz) return false;
  if (parcels.reduce((a, p) => a + volume(p), 0) > usable) return false;
  return parcels.every((p) => parcelFitsBox(p, box));
}
function fillBox(parcels, box, ship) {
  const usable = volume(box.inner) * ship.fill_efficiency; let vol = 0, wt = 0; const placed = [], leftover = [];
  for (const p of parcels) { if (parcelFitsBox(p, box) && vol + volume(p) <= usable && wt + p.weight_oz <= box.max_weight_oz) { placed.push(p); vol += volume(p); wt += p.weight_oz; } else leftover.push(p); }
  return { placed, leftover };
}
const parcelWeight = (parcels) => parcels.reduce((a, p) => a + p.weight_oz, 0);
function packedParcel(box, parcels, ship) {
  const oz = parcelWeight(parcels); const usable = volume(box.inner) * ship.fill_efficiency;
  return { box: box.id, box_name: box.name, service: box.service, inner: box.inner, items: parcels.map((p) => p.id), weight_oz: round2(oz), billable_oz: round2(billableOz(box, oz, ship)), fill: round2(parcels.reduce((a, p) => a + volume(p), 0) / usable), cost_est: round2(boxCost(box, oz, ship)) };
}
function singleBoxPlan(items, ship) { const fit = ship.boxes.filter((b) => setFitsBox(items, b, ship)).map((b) => packedParcel(b, items, ship)).sort((x, y) => x.cost_est - y.cost_est)[0]; return fit ? [fit] : null; }
function greedyPlan(items, ship) {
  const parcels = []; let remaining = items;
  while (remaining.length) {
    let best = null;
    for (const box of ship.boxes) {
      const { placed } = fillBox(remaining, box, ship); if (!placed.length) continue;
      const packedVol = placed.reduce((a, p) => a + volume(p), 0); const efficiency = boxCost(box, parcelWeight(placed), ship) / packedVol;
      if (!best || efficiency < best.efficiency - 1e-9 || (Math.abs(efficiency - best.efficiency) < 1e-9 && placed.length > best.placed.length)) best = { box, placed, efficiency };
    }
    parcels.push(packedParcel(best.box, best.placed, ship));
    const packedSet = new Set(best.placed); remaining = remaining.filter((p) => !packedSet.has(p));
  }
  return parcels;
}
const planCost = (parcels) => parcels.reduce((a, p) => a + p.cost_est, 0);

// A flat list of parcels → the cheapest set of boxes, plus what no box can ever hold.
export function pack(parcels, ship) {
  const out = { parcels: [], oversize: [], total_cost_est: 0 };
  // A parcel is admitted only when some box can take it ALONE by all three tests — sides, weight
  // AND usable volume (inner volume × fill factor); one that passes the first two but not the
  // third would never be placed by fillBox() and would wedge the greedy loop.
  let items = [];
  for (const p of parcels) { if (ship.boxes.some((b) => parcelFitsBox(p, b) && p.weight_oz <= b.max_weight_oz && volume(p) <= volume(b.inner) * ship.fill_efficiency)) items.push(p); else out.oversize.push({ item: p.id, reason: 'no box in the catalog fits this parcel by size or weight' }); }
  if (!items.length) return out;
  items = items.slice().sort((a, b) => volume(b) - volume(a));
  const candidates = [singleBoxPlan(items, ship), greedyPlan(items, ship)].filter(Boolean).sort((a, b) => { const d = planCost(a) - planCost(b); return Math.abs(d) > 1e-9 ? d : a.length - b.length; });
  out.parcels = candidates[0]; out.total_cost_est = round2(planCost(candidates[0]));
  return out;
}

// {sku: qty} → one parcel per unit. A line past MAX_UNITS is refused rather than planned: a
// thousand parcels is a typo or an attack on the planner, never a box.
export const MAX_UNITS = 500;
export function explode(lines, ship) {
  const parcels = [], missing = [], tooMany = [];
  for (const [id, qty] of Object.entries(lines)) {
    const spec = ship.parcels[id]; if (!spec) { missing.push(id); continue; }
    if (qty > MAX_UNITS) { tooMany.push(id); continue; }
    for (let i = 0; i < qty; i++) parcels.push({ id, ...spec });
  }
  return { parcels, missing, tooMany };
}

// An order's lines → the boxes it ships in, what each weighs and roughly costs, and every reason
// a human should look first. Never throws: an order the data cannot plan comes back with
// `warnings` and no boxes, and the bench says so.
export function packOrder(ship, items) {
  const lines = {}; for (const it of items || []) lines[it.sku] = (lines[it.sku] || 0) + (Number(it.qty) || 0);
  if (!ship) return { boxes: [], warnings: ['no shipping data (shipping.json) — weigh and size the box by hand'], total_oz: null, total_cost_est: null, pieces: 0, service: null };
  const { parcels, missing, tooMany } = explode(lines, ship);
  const p = pack(parcels, ship); const warnings = [];
  for (const m of missing) warnings.push(`${m} has no parcel in shipping.json — its size and weight are not in this plan`);
  for (const t of tooMany) warnings.push(`${t} × ${lines[t]}: more than ${MAX_UNITS} units — plan that by hand`);
  for (const o of p.oversize) warnings.push(`${o.item}: ${o.reason}`);
  if (p.parcels.length > 1) warnings.push(`${p.parcels.length} boxes — one order, ${p.parcels.length} labels, the same tracking link cannot carry both`);
  return { boxes: p.parcels, warnings, total_oz: round2(p.parcels.reduce((a, b) => a + b.weight_oz, 0)), total_cost_est: p.parcels.length ? p.total_cost_est : null, pieces: parcels.length, service: p.parcels[0]?.service || null, complete: !missing.length && !tooMany.length && !p.oversize.length };
}

// The scale check: a box that weighs past tolerance from its plan usually holds the wrong count.
export function weightCheck(plan, measuredOz, { tolerance = 1.5 } = {}) {
  if (measuredOz == null || !Number.isFinite(measuredOz)) return { checked: false };
  const expected = plan?.total_oz; if (expected == null || !plan.boxes?.length || !plan.complete) return { checked: false, reason: 'no complete plan to check against' };
  const delta = round2(measuredOz - expected);
  return { checked: true, expected, measured: measuredOz, delta, ok: Math.abs(delta) <= tolerance, tolerance };
}

// The data's own sanity: a box needs inner dimensions and a max weight, a parcel its three sides
// and a weight, the tiers ascending. A file that fails here is refused at load, not at the bench.
export function normalizeShipping(j) {
  if (!j || typeof j !== 'object') throw new Error('shipping.json is not an object');
  const num = (v, what) => { const n = Number(v); if (!Number.isFinite(n) || n <= 0) throw new Error(`shipping.json: ${what} must be a positive number`); return n; };
  const boxes = (Array.isArray(j.boxes) ? j.boxes : []).map((b) => ({ id: String(b.id), name: String(b.name || b.id), service: String(b.service || ''), pricing: b.pricing === 'flat' ? 'flat' : 'weight', soft: !!b.soft, inner: { l: num(b.inner?.l, `box ${b.id} inner.l`), w: num(b.inner?.w, `box ${b.id} inner.w`), h: num(b.inner?.h, `box ${b.id} inner.h`) }, max_weight_oz: num(b.max_weight_oz, `box ${b.id} max_weight_oz`), flat_usd: b.pricing === 'flat' ? num(b.flat_usd, `box ${b.id} flat_usd`) : null }));
  if (!boxes.length) throw new Error('shipping.json has no boxes');
  const parcels = {}; for (const [id, p] of Object.entries(j.parcels || {})) parcels[id] = { l: num(p.l, `parcel ${id} l`), w: num(p.w, `parcel ${id} w`), h: num(p.h, `parcel ${id} h`), weight_oz: num(p.weight_oz, `parcel ${id} weight_oz`), soft: !!p.soft };
  const tiers = (Array.isArray(j.ground_advantage_rate_tiers) ? j.ground_advantage_rate_tiers : []).map((t) => ({ max_oz: num(t.max_oz, 'a rate tier max_oz'), usd: num(t.usd, 'a rate tier usd') }));
  if (!tiers.length) throw new Error('shipping.json has no ground_advantage_rate_tiers');
  for (let i = 1; i < tiers.length; i++) if (tiers[i].max_oz <= tiers[i - 1].max_oz) throw new Error('shipping.json rate tiers must ascend by max_oz');
  return { fill_efficiency: num(j.fill_efficiency ?? 0.78, 'fill_efficiency'), dim_weight_divisor: num(j.dim_weight_divisor ?? 166, 'dim_weight_divisor'), dim_weight_min_cubic_in: j.dim_weight_min_cubic_in == null ? 0 : num(j.dim_weight_min_cubic_in, 'dim_weight_min_cubic_in'), boxes, parcels, ground_advantage_rate_tiers: tiers, cost_zone_note: j.cost_zone_note || '' };
}
