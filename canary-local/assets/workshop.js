// canary-local/assets/workshop.js — the production workshop.
//
// A Tesla-style configurator with a hardware-project conscience. The
// journey: Configure → Print → Gather → Assemble → Flash → Your build.
// Every pane renders GENERATED data (devices/workshop.json + friends),
// which CI regenerates from the enclosure README, the .scad customizer,
// the BOM CSVs and firmware/configs — so the page cannot quietly drift
// from the hardware it describes.
//
// Honesty rules the 3D viewport: the committed STLs are print-validated
// renders of the scad's curated presets. When your option ticks match a
// preset, you're looking at exactly your case. When they don't, the
// viewport shows the NEAREST preset and says so — and the "parameter
// set" download always encodes your exact choices for OpenSCAD, because
// the scad is the configurator; this page is its honest showroom.

import { DeviceScene } from "./scene3d.js";
import { parseSTL } from "./stl.js";

const el = (tag, cls, text) => {
  const n = document.createElement(tag);
  if (cls) n.className = cls;
  if (text != null) n.textContent = text;
  return n;
};

const ENC_BASE = "../docs/hardware/enclosure/";
// first clause of a BOM description — enough to shop by; the full sheet
// is one link away
const shortDesc = (d) => (d || "").split(/[,;(]/)[0].trim();
const GH = "https://github.com/kmay89/securaCV/blob/main/";
const PETG_G_CM3 = 1.27; // typical PETG density — used only for the solid-weight ceiling

const STAGES = [
  ["configure", "Configure"],
  ["print", "Print plan"],
  ["gather", "Gather parts"],
  ["assemble", "Assemble"],
  ["flash", "Flash & setup"],
  ["build", "Your build"],
];

const DEVICE_BLURB = {
  "canary-wap": "the witness — pocketable, portable, the full option sheet",
  "canary-vision": "the eyes — semantic events, never frames",
  "canary-sense": "the feel — radar presence, no camera, no mic",
  "canary-display-watch": "the glance — desk glass for one room",
  "canary-display-dash": "the dashboard — the whole house at a glance",
};

// ── data ────────────────────────────────────────────────────────────────
async function loadJson(path) {
  const r = await fetch(path);
  if (!r.ok) throw new Error(`${path}: HTTP ${r.status}`);
  return r.json();
}

const state = {
  data: null,        // workshop.json
  registry: null,
  enclosures: null,
  build: null,
  dev: null,         // device id
  options: {},       // opt_* → bool (wap-style configurators)
  mountStyle: "keyhole",
  pkg: null,         // explicitly selected package id (non-configurator devices)
  addons: {},        // addon id → bool
  flavor: null,      // firmware flavor
  stage: "configure",
  mode: "building",  // "building" | "dreaming"
  scene: null,
  sceneParts: [],
  meshCache: new Map(),   // url → {mesh,bbox,triangles,volume}
  volumes: new Map(),     // file → cm³ (filled as meshes load)
};

// ── config resolution ───────────────────────────────────────────────────
const wsDev = () => state.data.devices[state.dev];
const hasConfigurator = () => (wsDev().options || []).some((o) => o.id.startsWith("opt_"));

function optionVector() {
  const v = {};
  for (const o of wsDev().options || []) {
    if (o.id.startsWith("opt_")) v[o.id] = !!state.options[o.id];
  }
  return v;
}

// nearest package by option distance; exact match wins at distance 0
function matchPackage() {
  const d = wsDev();
  if (!d.packages?.length) return { pkg: null, exact: true, distance: 0 };
  if (!hasConfigurator() || !d.packages[0].options) {
    const pkg = d.packages.find((p) => p.id === state.pkg) || d.packages[0];
    return { pkg, exact: true, distance: 0 };
  }
  const v = optionVector();
  let best = null, bestDist = Infinity;
  for (const p of d.packages) {
    let dist = 0;
    for (const k of Object.keys(v)) if (!!p.options[k] !== v[k]) dist++;
    if (dist < bestDist) { best = p; bestDist = dist; }
  }
  return { pkg: best, exact: bestDist === 0, distance: bestDist };
}

function setById(id) {
  return state.enclosures.sets.find((s) => s.id === id);
}

// the parts you'd actually print for the current selection
function selectedParts() {
  const { pkg } = matchPackage();
  const parts = [];
  if (pkg) {
    const st = setById(pkg.set);
    parts.push(...(st?.parts || pkg.parts || []));
  }
  for (const a of wsDev().addons || []) {
    const enabled = state.addons[a.id] && (!a.when || state.options[a.when]);
    if (enabled) {
      const st = setById(a.set);
      parts.push(...(st?.parts || a.parts || []));
    }
  }
  return parts;
}

// BOM rows implied by the current option ticks (deduped, alts marked)
function optionBomRows() {
  const bom = state.build.devices?.[state.dev]?.bom;
  if (!bom) return { rows: [], usd: 0 };
  const byRef = new Map(bom.rows.map((r) => [r.ref, r]));
  const picked = new Map();
  for (const o of wsDev().options || []) {
    if (!state.options[o.id] || !o.bom) continue;
    for (const ref of o.bom) {
      const row = byRef.get(ref);
      if (row && !picked.has(ref)) picked.set(ref, { ...row, because: o.label });
    }
  }
  const rows = [...picked.values()];
  const usd = rows.filter((r) => !/-ALT/.test(r.ref)).reduce((s, r) => s + (r.usd || 0), 0);
  return { rows, usd };
}

function requiredBom() {
  const bom = state.build.devices?.[state.dev]?.bom;
  if (!bom) return { rows: [], usd: 0 };
  const rows = bom.rows.filter((r) => r.required);
  return { rows, usd: bom.required_usd };
}

// firmware flags implied by ticked options, resolved against the flavor
function impliedFlags() {
  const fw = wsDev().firmware;
  const flavor = state.flavor || Object.keys(fw.flavors)[0];
  const flags = fw.flavors[flavor] || {};
  const out = [];
  for (const o of wsDev().options || []) {
    if (!state.options[o.id] || !o.fw) continue;
    for (const f of o.fw) {
      out.push({ flag: f, because: o.label, ...(flags[f] || { on: false }) });
    }
  }
  return { flavor, flags, implied: out };
}

// ── OpenSCAD parameter-set download (native customizer preset) ──────────
function parameterSetJson() {
  const params = { preset: "custom" };
  for (const [k, v] of Object.entries(optionVector())) params[k] = String(v);
  if ((wsDev().options || []).some((o) => o.id === "mount_style")) {
    params.mount_style = state.mountStyle;
  }
  return {
    parameterSets: { "my-canary": params },
    fileFormatVersion: "1",
  };
}

function downloadParameterSet() {
  const scad = wsDev().scad;
  const blob = new Blob([JSON.stringify(parameterSetJson(), null, 2) + "\n"],
    { type: "application/json" });
  const a = document.createElement("a");
  a.href = URL.createObjectURL(blob);
  // named after the scad: dropped beside it, OpenSCAD's customizer
  // offers "my-canary" in its preset dropdown automatically
  a.download = scad.replace(/\.scad$/, ".json");
  a.click();
  URL.revokeObjectURL(a.href);
}

// ── 3D viewport ─────────────────────────────────────────────────────────
async function loadMesh(file) {
  const url = ENC_BASE + file;
  if (state.meshCache.has(url)) return state.meshCache.get(url);
  const buf = await (await fetch(url)).arrayBuffer();
  const parsed = parseSTL(buf);
  parsed.volume = meshVolumeCm3(parsed.mesh);
  state.meshCache.set(url, parsed);
  state.volumes.set(file, parsed.volume);
  return parsed;
}

// signed tetrahedron sum — exact for the closed manifolds OpenSCAD emits
export function meshVolumeCm3(mesh) {
  const { pos, idx } = mesh;
  let six = 0;
  for (let t = 0; t < idx.length; t += 3) {
    const a = idx[t] * 3, b = idx[t + 1] * 3, c = idx[t + 2] * 3;
    const ax = pos[a], ay = pos[a + 1], az = pos[a + 2];
    const bx = pos[b], by = pos[b + 1], bz = pos[b + 2];
    const cx = pos[c], cy = pos[c + 1], cz = pos[c + 2];
    six += ax * (by * cz - bz * cy) - ay * (bx * cz - bz * cx) + az * (bx * cy - by * cx);
  }
  return Math.abs(six / 6) / 1000; // mm³ → cm³
}

const FIL_COLORS = [[0.93, 0.83, 0.31], [0.62, 0.66, 0.7], [0.85, 0.4, 0.32], [0.42, 0.62, 0.5]];

async function showSelectionIn3D(viewport, note) {
  const scene = state.scene;
  for (const p of state.sceneParts) scene.removePart(p);
  state.sceneParts = [];
  const parts = selectedParts();
  const metas = [];
  for (const part of parts) {
    try { metas.push({ part, ...(await loadMesh(part.file)) }); }
    catch { /* a missing mesh shows in the parts list; the scene stays honest */ }
  }
  if (!metas.length) { note.textContent = "no committed meshes for this selection yet"; return; }
  const gap = 8;
  const totalW = metas.reduce((s, m) => s + m.bbox.size[0], 0) + gap * (metas.length - 1);
  let cursor = 0, maxR = 40;
  metas.forEach((m, i) => {
    const cx = cursor - totalW / 2 + m.bbox.size[0] / 2;
    const model = placeFloat(cx, -(m.bbox.size[2] / 2), m.bbox.center);
    state.sceneParts.push(scene.addMesh(m.mesh, {
      color: FIL_COLORS[i % FIL_COLORS.length], gloss: 0.18, model,
    }));
    cursor += m.bbox.size[0] + gap;
    maxR = Math.max(maxR, Math.hypot(totalW / 2, m.bbox.size[1], m.bbox.size[2]));
  });
  scene.viewY = 0;
  scene.dist = Math.max(60, maxR * 3.0);
  const grams = metas.reduce((s, m) => s + m.volume, 0) * PETG_G_CM3;
  note.textContent = `${metas.length} part${metas.length > 1 ? "s" : ""} · solid-PETG ceiling ≈ ${Math.round(grams)} g`;
}

// scad Z-up → viewer Y-up (same math the enclosure lab uses)
function placeFloat(cx, cy, pivot) {
  const c = Math.cos(-Math.PI / 2), s = Math.sin(-Math.PI / 2);
  const R = new Float32Array([1, 0, 0, 0, 0, c, s, 0, 0, -s, c, 0, 0, 0, 0, 1]);
  const C = new Float32Array([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -pivot[0], -pivot[1], -pivot[2], 1]);
  const Tm = new Float32Array([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, cx, cy, 0, 1]);
  return mul(Tm, mul(R, C));
}
function mul(a, b) {
  const o = new Float32Array(16);
  for (let col = 0; col < 4; col++)
    for (let r = 0; r < 4; r++)
      o[col * 4 + r] =
        a[r] * b[col * 4] + a[4 + r] * b[col * 4 + 1] +
        a[8 + r] * b[col * 4 + 2] + a[12 + r] * b[col * 4 + 3];
  return o;
}

// ── stage: CONFIGURE ────────────────────────────────────────────────────
function renderConfigure(root) {
  const d = wsDev();
  const grid = el("div", "ws-configure");

  // left column: packages + options
  const left = el("div", "ws-left");

  left.append(el("h3", null, "Start from a package"));
  const pkgRow = el("div", "ws-pkgs");
  for (const p of d.packages || []) {
    const card = el("button", "ws-pkg");
    if (p.preview) {
      const img = document.createElement("img");
      img.src = ENC_BASE + p.preview;
      img.alt = p.label;
      img.loading = "lazy";
      card.append(img);
    }
    card.append(el("strong", null, p.label));
    if (p.dims_mm) card.append(el("span", "ws-dims", p.dims_mm));
    card.append(el("span", "ws-contents", p.contents || ""));
    if (p.status === "in-development") card.append(el("span", "chip", "in development"));
    card.addEventListener("click", () => {
      state.pkg = p.id;
      if (p.options) {
        state.options = { ...p.options };
        if (!p.options.opt_seal) state.addons = {};
      }
      update();
    });
    pkgRow.append(card);
  }
  left.append(pkgRow);

  if (hasConfigurator()) {
    left.append(el("h3", null, "…then tick what your home needs"));
    const groups = new Map();
    for (const o of d.options) {
      if (!groups.has(o.group)) groups.set(o.group, []);
      groups.get(o.group).push(o);
    }
    for (const [g, opts] of groups) {
      const box = el("div", "ws-optgroup");
      box.append(el("h4", null, g));
      for (const o of opts) {
        if (o.enum) {
          const row = el("label", "ws-opt ws-opt-enum b-only");
          row.append(el("span", "ws-optlabel", "style"));
          const sel = document.createElement("select");
          for (const v of o.enum) sel.append(new Option(v, v));
          sel.value = state.mountStyle;
          sel.addEventListener("change", () => { state.mountStyle = sel.value; update(); });
          row.append(sel);
          box.append(row);
          continue;
        }
        const row = el("label", "ws-opt");
        const cb = document.createElement("input");
        cb.type = "checkbox";
        cb.checked = !!state.options[o.id];
        cb.addEventListener("change", () => {
          state.options[o.id] = cb.checked;
          state.pkg = null;
          update();
        });
        const body = el("span", "ws-optbody");
        body.append(el("strong", null, o.label));
        if (o.consequence) body.append(el("span", "ws-conseq", "→ " + o.consequence));
        const tags = el("span", "ws-tags b-only");
        for (const ref of o.bom || []) tags.append(el("code", "ws-ref", ref));
        for (const f of o.fw || []) tags.append(el("code", "ws-flag", f));
        if (tags.childNodes.length) body.append(tags);
        row.append(cb, body);
        box.append(row);
      }
      left.append(box);
    }
    for (const a of d.addons || []) {
      const gated = a.when && !state.options[a.when];
      const row = el("label", "ws-opt ws-addon" + (gated ? " ws-gated" : ""));
      const cb = document.createElement("input");
      cb.type = "checkbox";
      cb.disabled = gated;
      cb.checked = !!state.addons[a.id] && !gated;
      cb.addEventListener("change", () => { state.addons[a.id] = cb.checked; update(); });
      const body = el("span", "ws-optbody");
      body.append(el("strong", null, a.label));
      body.append(el("span", "ws-conseq", a.blurb + (gated ? " (needs weather sealing)" : "")));
      row.append(cb, body);
      left.append(row);
    }
  } else if ((d.addons || []).length) {
    for (const a of d.addons) {
      const row = el("label", "ws-opt ws-addon");
      const cb = document.createElement("input");
      cb.type = "checkbox";
      cb.checked = !!state.addons[a.id];
      cb.addEventListener("change", () => { state.addons[a.id] = cb.checked; update(); });
      const body = el("span", "ws-optbody");
      body.append(el("strong", null, a.label), el("span", "ws-conseq", a.blurb));
      row.append(cb, body);
      left.append(row);
    }
  }

  // right column: 3D + live checklist. The viewport node (and its WebGL
  // context) persists across re-renders — a context per option toggle
  // would exhaust the browser's context pool.
  const right = el("div", "ws-right");
  if (!state.viewport) {
    const stage3d = el("div", "ws-stage");
    const cv = document.createElement("canvas");
    cv.className = "ws-canvas";
    cv.width = 640; cv.height = 420;
    const ribbon = el("div", "ws-ribbon");
    const meshNote = el("div", "ws-meshnote muted");
    stage3d.append(cv, ribbon, meshNote);
    state.scene = new DeviceScene(cv, null);
    state.scene.start();
    state.sceneParts = [];
    cv.__scene = state.scene;
    state.viewport = { stage3d, cv, ribbon, meshNote };
  }
  const { stage3d, ribbon, meshNote } = state.viewport;
  right.append(stage3d);

  const check = el("div", "ws-check");
  right.append(check);

  grid.append(left, right);
  root.append(grid);

  // fill the live pieces
  const m = matchPackage();
  if (m.pkg) {
    ribbon.textContent = m.exact
      ? `print-validated preset: ${m.pkg.label}${m.pkg.dims_mm ? " · " + m.pkg.dims_mm : ""}`
      : `custom combo — showing nearest preset “${m.pkg.label}” (${m.distance} option${m.distance > 1 ? "s" : ""} differ); your exact case renders from the .scad`;
    ribbon.className = "ws-ribbon" + (m.exact ? " ws-exact" : " ws-custom");
  } else {
    ribbon.textContent = "no package yet";
  }
  showSelectionIn3D(stage3d, meshNote);
  renderChecklist(check);
}

function renderChecklist(box) {
  const d = wsDev();
  box.innerHTML = "";
  box.append(el("h3", null, "Your build so far"));

  const parts = selectedParts();
  const ul = el("ul", "ws-partlist");
  for (const p of parts) {
    const li = el("li");
    li.append(el("strong", null, p.name), el("span", "muted", " — " + (p.print_note || "")));
    ul.append(li);
  }
  if (state.options.opt_seal && !parts.some((p) => /gasket/i.test(p.file))) {
    const li = el("li", "ws-warn");
    li.textContent = "weather sealing ticked — pick the weather package (or render a custom gasket from the .scad)";
    ul.append(li);
  }
  box.append(el("h4", null, `Case parts to print (${parts.length})`), ul);

  for (const a of d.always || []) {
    box.append(el("p", "muted fineprint", `${a.label}: ${a.note}`));
  }

  const ob = optionBomRows();
  if (ob.rows.length) {
    const ul2 = el("ul", "ws-bomlist");
    for (const r of ob.rows) {
      const li = el("li" , /-ALT/.test(r.ref) ? "ws-alt" : null);
      li.append(
        el("code", "ws-ref b-only", r.ref),
        el("span", null, " " + shortDesc(r.desc)),
        el("span", "muted", r.usd ? ` · $${r.usd.toFixed(2)}` : ""),
        el("span", "ws-because", ` ← ${r.because}`),
      );
      ul2.append(li);
    }
    box.append(el("h4", null, `Parts your options add (~$${ob.usd.toFixed(2)})`), ul2);
  }

  const fl = impliedFlags();
  if (fl.implied.length) {
    const ul3 = el("ul", "ws-flaglist b-only");
    for (const f of fl.implied) {
      const li = el("li", f.on ? "ws-on" : "ws-off");
      li.append(
        el("code", "ws-flag", f.flag),
        el("span", null, f.on ? ` already ON in ${fl.flavor}` : ` OFF in ${fl.flavor} — flip it when you flash`),
        el("span", "ws-because", ` ← ${f.because}`),
      );
      ul3.append(li);
    }
    box.append(el("h4", "b-only", "Firmware this implies"), ul3);
  }

  const dl = el("button", "ws-dl");
  dl.textContent = "⤓ Download your OpenSCAD parameter set";
  dl.title = "drop it next to " + d.scad + " — the customizer picks it up as a preset";
  dl.addEventListener("click", downloadParameterSet);
  if (hasConfigurator()) box.append(dl);
  const explorer = el("p", "muted fineprint");
  const a = el("a", null, "every dimension, in the parameter explorer →");
  a.href = `index.html#${state.dev}`;
  explorer.append(a);
  box.append(explorer);
}

// ── stage: PRINT ────────────────────────────────────────────────────────
function renderPrint(root) {
  const parts = selectedParts();
  const d = wsDev();
  const ps = state.enclosures.print_settings;

  const intro = el("p", "body",
    "Parts print exactly as modeled — z=0 is the build plate, in the " +
    "documented orientation, no supports by design.");
  root.append(intro);

  if (d.coupon) {
    const c = el("p", "ondevice");
    c.append(el("strong", null, "Print this first: "),
      document.createTextNode(`${d.coupon.name} — ${d.coupon.print_note || "15-minute fit tuner for your printer"} `));
    const a = el("a", null, "STL ⤓");
    a.href = ENC_BASE + d.coupon.file; a.download = d.coupon.file;
    c.append(a);
    root.append(c);
  }

  // volumes fill in as meshes load; if the user jumped straight here,
  // fetch what's missing and repaint once
  const missing = parts.filter((p) => !state.volumes.has(p.file));
  if (missing.length) {
    Promise.all(missing.map((p) => loadMesh(p.file).catch(() => null)))
      .then((r) => { if (r.some(Boolean) && state.stage === "print") update(); });
  }

  const list = el("div", "ws-printlist");
  for (const p of parts) {
    const row = el("div", "ws-printrow");
    const head = el("div", "ws-printhead");
    head.append(el("strong", null, p.name),
      el("span", "chip chip-dim", p.material || "PETG / ASA"));
    const vol = state.volumes.get(p.file);
    if (vol) head.append(el("span", "muted", `solid ≈ ${Math.round(vol * PETG_G_CM3)} g`));
    const a = el("a", "ws-stl", "STL ⤓");
    a.href = ENC_BASE + p.file; a.download = p.file;
    head.append(a);
    row.append(head, el("p", "muted", p.print_note || ""));
    list.append(row);
  }
  root.append(list);

  const card = el("div", "ws-settings");
  card.append(el("h4", null, "Suggested settings (from the enclosure README)"));
  const ulS = el("ul");
  ulS.append(
    el("li", null, `layer ${ps.layer_height_mm} mm · ${ps.walls} walls · ${ps.infill_pct}% infill`),
    el("li", null, ps.material),
    el("li", null, `gaskets: ${ps.gasket_material}`),
    el("li", null, ps.orientation),
  );
  card.append(ulS);
  root.append(card);

  const note = el("p", "muted fineprint",
    "The gram figures are a ceiling: solid volume × PETG density, computed " +
    "from the actual mesh. Your slicer's estimate at the settings above will " +
    "land well under it. For the layer-by-layer view, open the print bench " +
    "on the device sheet.");
  const link = el("a", null, "open the print bench →");
  link.href = `index.html#${state.dev}`;
  note.append(" ", link);
  root.append(note);
}

// ── stage: GATHER ───────────────────────────────────────────────────────
function renderGather(root) {
  const req = requiredBom();
  const ob = optionBomRows();
  const bom = state.build.devices?.[state.dev]?.bom;

  if (!bom) {
    const n = state.build.devices?.[state.dev]?.bom_note;
    root.append(el("p", "ondevice", n || "BOM pending for this device."));
    return;
  }

  const total = req.usd + ob.usd;
  const head = el("div", "ws-gathertotal");
  head.append(el("strong", null, `$${total.toFixed(2)}`),
    el("span", "muted", ` for your configuration — $${req.usd.toFixed(2)} core + $${ob.usd.toFixed(2)} options (indicative, from the BOM's own prices)`));
  root.append(head);

  const mkList = (rows, title) => {
    root.append(el("h4", null, title));
    const ul = el("ul", "ws-bomlist");
    for (const r of rows) {
      const li = el("li", /-ALT/.test(r.ref) ? "ws-alt" : null);
      li.append(el("code", "ws-ref b-only", r.ref),
        el("span", null, ` ${shortDesc(r.desc)}`),
        el("span", "muted", ` ×${r.qty}${r.usd ? ` · $${r.usd.toFixed(2)}` : ""}`));
      if (r.because) li.append(el("span", "ws-because", ` ← ${r.because}`));
      ul.append(li);
    }
    root.append(ul);
  };
  mkList(req.rows, "Core (always)");
  if (ob.rows.length) mkList(ob.rows, "Because of your options");

  const copy = el("button", "ws-dl", "⧉ Copy shopping list");
  copy.addEventListener("click", () => {
    const lines = [...req.rows, ...ob.rows].map(
      (r) => `${r.qty}× ${r.ref}  ${shortDesc(r.desc)}${r.mpn ? `  (${r.mfr} ${r.mpn})` : ""}`);
    navigator.clipboard?.writeText(lines.join("\n"));
    copy.textContent = "✓ copied";
    setTimeout(() => (copy.textContent = "⧉ Copy shopping list"), 1500);
  });
  root.append(copy);

  const recipes = wsDev().recipes || [];
  if (recipes.length) {
    root.append(el("h4", null, "Reference builds (the BOM's own summary rows)"));
    const row = el("div", "ws-recipes");
    for (const rc of recipes) {
      const c = el("div", "ws-recipe");
      c.append(el("strong", null, rc.label),
        rc.usd ? el("span", "ws-dims", `$${rc.usd.toFixed(2)}`) : "",
        el("span", "muted b-only", rc.formula));
      if (rc.note) c.append(el("span", "ws-conseq", rc.note));
      row.append(c);
    }
    root.append(row);
  }

  const src = el("p", "muted fineprint");
  const a = el("a", null, bom.source);
  a.href = GH + bom.source; a.target = "_blank"; a.rel = "noopener";
  src.append("Full sheet with MPNs, distributors, lifecycle: ", a);
  root.append(src);
}

// ── stage: ASSEMBLE ─────────────────────────────────────────────────────
function renderAssemble(root) {
  const asm = state.build.devices?.[state.dev]?.assembly;
  if (asm) {
    root.append(el("h4", null, "Assembly"));
    const ol = el("ol", "asm-steps");
    for (const s of asm.steps) ol.append(el("li", null, s));
    root.append(ol);
  } else {
    root.append(el("p", "muted",
      "Assembly for this device lives in its docs — the enclosure catalog carries per-case steps for WAP and Vision today."));
  }

  const tpls = (wsDev().templates || []).map((t) => [t, state.data.templates[t]]);
  if (tpls.length) {
    root.append(el("h4", null, "Drill templates — print on paper, 100% scale"));
    root.append(el("p", "ondevice", state.data.calibration_note));
    const row = el("div", "ws-templates");
    for (const [file, t] of tpls) {
      const card = el("div", "ws-template");
      const img = document.createElement("img");
      img.src = ENC_BASE + file; img.alt = t.label; img.loading = "lazy";
      const a = el("a", "ws-stl", "SVG ⤓");
      a.href = ENC_BASE + file; a.download = file;
      card.append(img, el("strong", null, t.label), el("span", "muted", t.note), a);
      row.append(card);
    }
    root.append(row);
  }

  if (state.options.opt_seal) {
    const p = el("p", "muted fineprint");
    const a = el("a", null, "field ratings: what “sealed” honestly survives →");
    a.href = GH + "docs/hardware/enclosure/field_ratings.md";
    a.target = "_blank"; a.rel = "noopener";
    p.append(a);
    root.append(p);
  }
}

// ── stage: FLASH ────────────────────────────────────────────────────────
function renderFlash(root) {
  const fw = wsDev().firmware;
  const flavors = Object.keys(fw.flavors);
  const fl = impliedFlags();

  const pick = el("div", "ws-flavors");
  for (const f of flavors) {
    const b = el("button", "pill" + (f === fl.flavor ? " on" : ""), f);
    b.addEventListener("click", () => { state.flavor = f; update(); });
    pick.append(b);
  }
  root.append(el("h4", null, `Firmware: ${fw.project}`), pick);

  if (fl.implied.length) {
    root.append(el("h4", null, "What your options need"));
    const ul = el("ul", "ws-flaglist");
    for (const f of fl.implied) {
      const li = el("li", f.on ? "ws-on" : "ws-off");
      li.append(el("code", "ws-flag", f.flag),
        el("span", null, f.on
          ? ` — ON in the ${fl.flavor} flavor${f.note ? ` (${f.note})` : ""}`
          : ` — OFF in ${fl.flavor}: set it to 1 in config.h when you build`),
        el("span", "ws-because", ` ← ${f.because}`));
      ul.append(li);
    }
    root.append(ul);
  }

  root.append(el("h4", "b-only", `Every flag in ${fl.flavor}`));
  const ul2 = el("ul", "ws-flaglist ws-allflags b-only");
  for (const [name, v] of Object.entries(fl.flags)) {
    const li = el("li", v.on ? "ws-on" : "ws-off");
    li.append(el("code", "ws-flag", name),
      el("span", "muted", ` ${v.on ? "1" : "0"}${v.note ? " — " + v.note : ""}`));
    ul2.append(li);
  }
  root.append(ul2);

  const src = el("p", "muted fineprint");
  const a = el("a", null, `${fw.source}/`);
  a.href = GH + fw.source; a.target = "_blank"; a.rel = "noopener";
  src.append("Flags live in ", a, " — one folder per flavor, flashing docs beside them.");
  root.append(src);

  if (state.dev.startsWith("canary-display")) {
    const p = el("p", "ondevice");
    const a2 = el("a", null, "practice on the live emulator →");
    a2.href = `index.html#${state.dev}`;
    p.append(el("strong", null, "Before it even ships: "),
      document.createTextNode("this device's real firmware runs in your browser — pair it, poke it, cut its network. "), a2);
    root.append(p);
  }
}

// ── stage: BUILD CARD ───────────────────────────────────────────────────
function renderBuildCard(root) {
  const d = wsDev();
  const reg = state.registry.devices.find((x) => x.id === state.dev);
  const m = matchPackage();
  const parts = selectedParts();
  const req = requiredBom();
  const ob = optionBomRows();
  const fl = impliedFlags();

  const card = el("div", "ws-card");
  card.append(el("h3", null, `${reg?.name || state.dev} — your spec`));
  if (m.pkg) {
    card.append(el("p", "body",
      (m.exact ? `Package: ${m.pkg.label}` : `Custom (nearest: ${m.pkg.label})`)
      + (m.pkg.dims_mm ? ` · ${m.pkg.dims_mm}` : "")));
  }
  const on = (wsDev().options || []).filter((o) => state.options[o.id] && o.id.startsWith("opt_"));
  if (on.length) {
    card.append(el("p", "muted", "Options: " + on.map((o) => o.label).join(" · ")));
  }
  card.append(el("p", "muted", `Prints: ${parts.map((p) => p.name).join(" · ") || "—"}`));
  if (state.build.devices?.[state.dev]?.bom) {
    card.append(el("p", "body",
      `Parts budget ≈ $${(req.usd + ob.usd).toFixed(2)} (core $${req.usd.toFixed(2)} + options $${ob.usd.toFixed(2)})`));
  }
  if (fl.implied.length) {
    card.append(el("p", "muted b-only",
      `Firmware (${fl.flavor}): ` + fl.implied.map((f) => `${f.flag}${f.on ? "" : "→1"}`).join(" · ")));
  }
  root.append(card);

  const acts = el("div", "ws-cardacts");
  if (hasConfigurator()) {
    const dl = el("button", "ws-dl", "⤓ OpenSCAD parameter set");
    dl.addEventListener("click", downloadParameterSet);
    acts.append(dl);
  }
  const pr = el("button", "ws-dl", "⎙ Print this build card");
  pr.addEventListener("click", () => window.print());
  acts.append(pr);
  root.append(acts);

  const next = el("p", "muted fineprint",
    "Confidence is the point: everything above came from the same repo the " +
    "hardware ships from. When the printed parts are in your hand, the " +
    "device sheet walks pairing and fixing.");
  const a = el("a", null, `open ${reg?.name || "the device"} on canary.local →`);
  a.href = `index.html#${state.dev}`;
  next.append(" ", a);
  root.append(next);
}

// ── frame: device strip, stage rail, mode toggle ────────────────────────
const RENDERERS = {
  configure: renderConfigure, print: renderPrint, gather: renderGather,
  assemble: renderAssemble, flash: renderFlash, build: renderBuildCard,
};

function update() {
  const main = document.getElementById("workshop");
  main.innerHTML = "";
  document.body.className = state.mode === "dreaming" ? "ws-dreaming" : "ws-building";

  // device strip
  const strip = el("div", "ws-devices");
  for (const rd of state.registry.devices) {
    if (!state.data.devices[rd.id]) continue;
    const b = el("button", "ws-device" + (rd.id === state.dev ? " on" : ""));
    b.append(el("strong", null, rd.name), el("span", "muted", DEVICE_BLURB[rd.id] || ""));
    b.addEventListener("click", () => { pickDevice(rd.id); update(); });
    strip.append(b);
  }
  main.append(strip);

  // mode toggle
  const mode = el("div", "ws-mode");
  for (const [id, label] of [["dreaming", "Just dreaming"], ["building", "I'm building it"]]) {
    const b = el("button", "pill" + (state.mode === id ? " on" : ""), label);
    b.addEventListener("click", () => { state.mode = id; update(); });
    mode.append(b);
  }
  mode.title = "dreaming hides part numbers and flags; building shows the whole sheet";
  main.append(mode);

  // stage rail
  const rail = el("nav", "ws-rail");
  STAGES.forEach(([id, label], i) => {
    const b = el("button", "ws-stagebtn" + (id === state.stage ? " on" : ""));
    b.append(el("span", "ws-stageno", String(i + 1)), document.createTextNode(label));
    b.addEventListener("click", () => { state.stage = id; update(); });
    rail.append(b);
  });
  main.append(rail);

  const stage = el("section", "ws-stagebody ws-stage-" + state.stage);
  main.append(stage);
  RENDERERS[state.stage](stage);

  // deep-linkable without polluting history
  history.replaceState(null, "",
    `#${state.dev}${state.mode === "dreaming" ? ".dreaming" : ""}`);
}

function pickDevice(id) {
  state.dev = id;
  state.stage = "configure"; // a new device restarts the spec
  state.options = {};
  state.addons = {};
  state.pkg = null;
  state.flavor = null;
  state.scene?.stop?.();
  state.scene = null;
  state.viewport = null;
  state.sceneParts = [];
  const d = state.data.devices[id];
  const pkg = d.packages?.find((p) => p.id === d.default_package) || d.packages?.[0];
  state.pkg = pkg?.id || null;
  if (pkg?.options) state.options = { ...pkg.options };
  else for (const o of d.options || []) if (o.id.startsWith("opt_")) state.options[o.id] = o.default;
}

async function boot() {
  const [data, registry, enclosures, build] = await Promise.all([
    loadJson("devices/workshop.json"),
    loadJson("devices/registry.json"),
    loadJson("devices/enclosures.json"),
    loadJson("devices/build.json"),
  ]);
  state.data = data; state.registry = registry;
  state.enclosures = enclosures; state.build = build;

  const [hashDev, hashMode] = location.hash.replace(/^#/, "").split(".");
  pickDevice(data.devices[hashDev] ? hashDev : "canary-wap");
  if (hashMode === "dreaming") state.mode = "dreaming";
  update();
}

// Node imports this module for the pure helpers (meshVolumeCm3); only a
// real page boots the UI.
if (typeof document !== "undefined") {
  boot().catch((e) => {
    const main = document.getElementById("workshop");
    main.append(el("p", "muted", `The workshop could not load its data (${e.message}). ` +
      "A bare checkout serves everything from canary-local/ — check the console."));
  });
}
