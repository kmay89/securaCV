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
import { productSummary } from "./catalog-browse.js";

const el = (tag, cls, text) => {
  const n = document.createElement(tag);
  if (cls) n.className = cls;
  if (text != null) n.textContent = text;
  return n;
};

const ENC_BASE = "../docs/hardware/enclosure/";
// first clause of a BOM description — enough to shop by; the full sheet
// is one link away. ALT rows say what the alternative IS, not just "ALT".
const shortDesc = (d) => {
  const alt = /^ALT\b[^:]*:\s*(.*)$/.exec(d || "");
  if (alt) return "alt: " + alt[1].split(/[,;(]/)[0].trim();
  return (d || "").split(/[,;(]/)[0].trim();
};
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
  viewGen: 0,
  meshCache: new Map(),   // url → {mesh,bbox,triangles,volume}
  volumes: new Map(),     // file → cm³ (filled as meshes load)
};

// ── config resolution ───────────────────────────────────────────────────
const wsDev = () => state.data.devices[state.dev];
// A user-facing boolean toggle: driven by the manifest's `audience` tag, not
// the `opt_` name prefix (the leaky signal catalog.json's audience split
// replaced). Enum options (mount_style) are user too but ride their own select,
// so they stay out of the boolean option vector.
const isUserToggle = (o) => o.audience === "user" && !o.enum;
const hasConfigurator = () => (wsDev().options || []).some((o) => o.audience === "user");

// The catalog manifest's summary for the current device's enclosure (by scad),
// or null offline / when the scad isn't in the manifest.
function catalogFacts() {
  const p = state.catalogByScad?.get(wsDev().scad);
  return p ? productSummary(p) : null;
}

function optionVector() {
  const v = {};
  for (const o of wsDev().options || []) {
    if (isUserToggle(o)) v[o.id] = !!state.options[o.id];
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

// what ticking this option adds to the cart (primary parts; ALTs are
// either/or choices and don't stack)
// ref → row lookup, built once per BOM (this runs for every option card
// on every re-render — no need to rebuild a Map each time)
function bomByRef(bom) {
  if (!bom._byRef) bom._byRef = new Map(bom.rows.map((r) => [r.ref, r]));
  return bom._byRef;
}

function optionCost(o) {
  const bom = state.build.devices?.[state.dev]?.bom;
  if (!bom || !o.bom) return 0;
  const byRef = bomByRef(bom);
  return o.bom.filter((ref) => !/-ALT/.test(ref))
    .reduce((s, ref) => s + (byRef.get(ref)?.usd || 0), 0);
}

// a package's indicative all-in parts price (core + its ticked options).
// Union the refs BEFORE summing — options share parts (camera and seal
// both want ADH1) and per-option sums would double-count them, making
// the card disagree with the deduped checklist/gather totals.
function packagePrice(p) {
  const bom = state.build.devices?.[state.dev]?.bom;
  if (!bom || !p.options) return null;
  const byRef = bomByRef(bom);
  const opts = wsDev().options || [];
  const refs = new Set();
  for (const [id, on] of Object.entries(p.options)) {
    if (!on) continue;
    for (const ref of opts.find((x) => x.id === id)?.bom || []) {
      if (!/-ALT/.test(ref)) refs.add(ref);
    }
  }
  let sum = bom.required_usd || 0;
  for (const ref of refs) sum += byRef.get(ref)?.usd || 0;
  return sum;
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
// A Downloads folder full of these must still read at a glance, so the
// name says everything: device, package-or-options, what the file IS.
//   canary-wap_battery-weather_openscad-params.json
//   canary-wap_custom-gps-batt-seal_openscad-params.json
// The Apple-configurator register: every option leads with what it DOES
// for the home, then the honest mechanics underneath. Curated page copy —
// the technical truth stays generated (consequence, refs, flags, prices).
const BENEFIT = {
  opt_camera: "eyes for the witness — semantic events, never frames",
  opt_buzzer: "a voice: chirps, alerts, the community whistle",
  opt_led: "a heartbeat you can see across the room",
  opt_battery: "keeps witnessing when the power doesn't",
  opt_gps: "pins the where to the when",
  opt_tamper: "knows the moment its lid is lifted",
  opt_touch: "an invisible button through solid plastic",
  opt_antenna: "reach for the far corner of the yard",
  opt_seal: "rain happens — seal it for the outdoors",
  opt_mount: "give it a permanent post on the wall",
  opt_batt: "untethered on the nightstand",
  opt_lux: "knows dark from day",
  opt_vent: "lets the room's air in to sense",
};
const GROUP_INTRO = {
  "Peripherals": "What's fitted inside",
  "Weather sealing": "Where it will live",
  "Mounting": "How it hangs",
};

const OPT_ABBR = {
  opt_camera: "cam", opt_buzzer: "buz", opt_led: "led", opt_battery: "batt",
  opt_gps: "gps", opt_tamper: "tamp", opt_touch: "touch", opt_antenna: "ant",
  opt_seal: "seal", opt_mount: "mount", opt_batt: "batt", opt_lux: "lux",
  opt_vent: "vent",
};
export function specName(devId, match, vector) {
  const what = match?.exact && match.pkg
    ? match.pkg.id.replace(/_/g, "-")
    : "custom-" + (Object.entries(vector).filter(([, v]) => v)
        .map(([k]) => OPT_ABBR[k] || k.replace(/^opt_/, "")).join("-") || "bare");
  return `${devId}_${what}_openscad-params`;
}

function parameterSetJson(name) {
  const params = { preset: "custom" };
  for (const [k, v] of Object.entries(optionVector())) params[k] = String(v);
  if ((wsDev().options || []).some((o) => o.id === "mount_style")) {
    params.mount_style = state.mountStyle;
  }
  return {
    parameterSets: { [name]: params },
    fileFormatVersion: "1",
  };
}

function downloadParameterSet() {
  const name = specName(state.dev, matchPackage(), optionVector());
  const blob = new Blob([JSON.stringify(parameterSetJson(name), null, 2) + "\n"],
    { type: "application/json" });
  const a = document.createElement("a");
  a.href = URL.createObjectURL(blob);
  a.download = `${name}.json`;
  a.click();
  URL.revokeObjectURL(a.href);
}

// ── coming-soon honesty: never an empty pane, always a door ─────────────
function issueUrl(title, body) {
  return "https://github.com/kmay89/securaCV/issues/new?title=" +
    encodeURIComponent(title) + "&body=" + encodeURIComponent(body);
}

function soonCard(text, reqTitle, reqBody) {
  const card = el("div", "ws-soon");
  card.append(el("p", "body", text));
  const a = el("a", "ws-request", "→ request it (opens a GitHub issue)");
  a.href = issueUrl(reqTitle, reqBody);
  a.target = "_blank";
  a.rel = "noopener";
  card.append(a);
  return card;
}

// ── 3D viewport ─────────────────────────────────────────────────────────
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
const PREVIEW_BASE = "enclosures/preview/";

// the standardized hero pose: classic isometric (35.26° elevation, 45°
// azimuth) — every part, every package, the same flattering angle
const ISO = { x: -0.615, y: 0.785 };

function partUrl(part) {
  return part.preview_mesh
    ? PREVIEW_BASE + part.file.replace(/^preview\//, "")
    : ENC_BASE + part.file;
}

async function loadPartMesh(part) {
  const url = partUrl(part);
  // cache the PROMISE so concurrent callers (viewport + checklist) share
  // one fetch; a failure evicts itself so a later attempt can retry
  if (!state.meshCache.has(url)) {
    const p = (async () => {
      const r = await fetch(url);
      if (!r.ok) throw new Error(`${url}: HTTP ${r.status}`);
      const buf = await r.arrayBuffer();
      const parsed = parseSTL(buf);
      parsed.volume = meshVolumeCm3(parsed.mesh);
      parsed.bytes = buf.byteLength;
      state.volumes.set(part.file, parsed.volume);
      return parsed;
    })();
    p.catch(() => state.meshCache.delete(url));
    state.meshCache.set(url, p);
  }
  return state.meshCache.get(url);
}

// solo = index into the selection to inspect alone; null = the ensemble
async function showSelectionIn3D(note, solo = null) {
  const scene = state.scene;
  // rapid toggles overlap these async runs — only the newest may touch
  // the scene, or a slow earlier load repaints over the current choice
  const gen = ++state.viewGen;
  const parts = selectedParts();
  const metas = [];
  for (const part of parts) {
    try { metas.push({ part, ...(await loadPartMesh(part)) }); }
    catch { /* a missing mesh shows in the parts list; the scene stays honest */ }
  }
  if (gen !== state.viewGen || scene !== state.scene) return;
  for (const p of state.sceneParts) scene.removePart(p);
  state.sceneParts = [];
  state.viewMetas = metas;
  renderInspectChips(metas, solo);
  if (!metas.length) {
    note.textContent = "no committed meshes for this selection yet";
    renderSpecStrip(null);
    return;
  }
  scene.home.x = ISO.x;
  scene.home.y = ISO.y;
  const shown = solo != null && metas[solo] ? [metas[solo]] : metas;
  const gap = 8;
  const totalW = shown.reduce((s, m) => s + m.bbox.size[0], 0) + gap * (shown.length - 1);
  let cursor = 0, maxR = 30;
  shown.forEach((m, i) => {
    const cx = cursor - totalW / 2 + m.bbox.size[0] / 2;
    const model = placeFloat(cx, -(m.bbox.size[2] / 2), m.bbox.center);
    const colorIdx = solo != null ? solo : i;
    state.sceneParts.push(scene.addMesh(m.mesh, {
      color: FIL_COLORS[colorIdx % FIL_COLORS.length], gloss: 0.18, model,
    }));
    cursor += m.bbox.size[0] + gap;
    maxR = Math.max(maxR, Math.hypot(totalW / 2, m.bbox.size[1], m.bbox.size[2]));
  });
  scene.viewY = 0;
  scene.dist = Math.max(60, maxR * (shown.length === 1 ? 2.6 : 3.0));
  if (solo != null && metas[solo]) {
    renderSpecStrip(metas[solo]);
    note.textContent = "";
  } else {
    renderSpecStrip(null);
    const grams = metas.reduce((s, m) => s + m.volume, 0) * PETG_G_CM3;
    note.textContent = `${metas.length} part${metas.length > 1 ? "s" : ""} · solid-PETG ceiling ≈ ${Math.round(grams)} g`;
  }
}

// part chips under the viewport — tap to inspect one part alone
function renderInspectChips(metas, solo) {
  const box = state.viewport?.chips;
  if (!box) return;
  box.innerHTML = "";
  if (metas.length < 2 && solo == null) return;
  const all = el("button", "ws-chip" + (solo == null ? " on" : ""), "all");
  all.addEventListener("click", () => showSelectionIn3D(state.viewport.meshNote, null));
  box.append(all);
  metas.forEach((m, i) => {
    const c = el("button", "ws-chip" + (solo === i ? " on" : ""), m.part.name);
    c.addEventListener("click", () => showSelectionIn3D(state.viewport.meshNote, i));
    box.append(c);
  });
}

// the trust strip: real numbers from the actual mesh, Printables-style
function renderSpecStrip(meta) {
  const box = state.viewport?.specs;
  if (!box) return;
  box.innerHTML = "";
  if (!meta) { box.hidden = true; return; }
  box.hidden = false;
  const { bbox, triangles, volume, bytes, part } = meta;
  const dims = bbox.size.map((v) => Math.round(v * 10) / 10).join(" × ") + " mm";
  box.append(
    el("span", "ws-spec", dims),
    el("span", "ws-spec", `${volume.toFixed(1)} cm³ solid`),
    el("span", "ws-spec", `≈ ${Math.round(volume * PETG_G_CM3)} g ceiling`),
    el("span", "ws-spec", `${triangles.toLocaleString()} triangles`),
    el("span", "ws-spec", `${(bytes / 1024).toFixed(0)} KB STL`),
    part.preview_mesh
      ? el("span", "chip", "preview mesh — not yet print-validated")
      : el("span", "chip chip-live", "print-validated STL"),
  );
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

  // catalog facts (read from the manifest, not retyped): environment rating —
  // design intent, never "verified" — plus flavors, options and see-also.
  const cf = catalogFacts();
  if (cf) {
    const bits = [];
    if (cf.envLabel && cf.envLabel !== "unrated")
      bits.push(`${cf.envLabel} · target`);
    if (cf.flavors) bits.push(`${cf.flavors} flavor${cf.flavors === 1 ? "" : "s"}`);
    // Count the options THIS workshop actually exposes, not the catalog's full
    // user-option inventory — the workshop only builds devices with a BOM +
    // firmware, so a "5 options" claim beside a package it can't configure
    // (e.g. the Dash) would mislead. Zero → the line just omits it.
    const nOpts = (d.options || []).filter((o) => o.audience === "user").length;
    if (nOpts) bits.push(`${nOpts} option${nOpts === 1 ? "" : "s"}`);
    if (cf.alternatives.length)
      bits.push("see also " + cf.alternatives.map((a) => a.replace(/_/g, " ")).join(", "));
    if (bits.length) {
      const line = el("p", "ws-catfacts muted");
      line.append(el("span", "ws-catfacts-cap", "From the catalog: "),
        document.createTextNode(bits.join(" · ")));
      left.append(line);
    }
  }

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
    const fromUsd = packagePrice(p);
    if (fromUsd != null) {
      card.append(el("span", "ws-from", `from ~$${fromUsd.toFixed(0)} in parts`));
    }
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
    left.append(el("h3", null, "…then build yours, option by option"));
    const groups = new Map();
    for (const o of d.options) {
      if (!groups.has(o.group)) groups.set(o.group, []);
      groups.get(o.group).push(o);
    }
    for (const [g, opts] of groups) {
      const box = el("div", "ws-optgroup");
      const groupName = g || "Options";
      const gh = el("h4", null, GROUP_INTRO[groupName] || groupName);
      gh.append(el("span", "ws-groupsub", " · " + groupName.toLowerCase()));
      box.append(gh);
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
        const on = !!state.options[o.id];
        const row = el("label", "ws-opt" + (on ? " ws-picked" : ""));
        const cb = document.createElement("input");
        cb.type = "checkbox";
        cb.checked = on;
        cb.addEventListener("change", () => {
          state.options[o.id] = cb.checked;
          state.pkg = null;
          update();
        });
        const body = el("span", "ws-optbody");
        body.append(el("strong", null, o.label));
        if (BENEFIT[o.id]) body.append(el("span", "ws-benefit", BENEFIT[o.id]));
        if (o.consequence) {
          body.append(el("span", "ws-conseq", "the case adds: " + o.consequence));
        }
        const tags = el("span", "ws-tags b-only");
        for (const ref of o.bom || []) tags.append(el("code", "ws-ref", ref));
        for (const f of o.fw || []) tags.append(el("code", "ws-flag", f));
        if (tags.childNodes.length) body.append(tags);
        const cost = optionCost(o);
        const price = el("span", "ws-delta",
          cost > 0 ? `+ $${cost.toFixed(2)}` : (on ? "included" : ""));
        row.append(cb, body, price);
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
  if (!hasConfigurator()) {
    const reg = state.registry.devices.find((x) => x.id === state.dev);
    left.append(soonCard(
      "This case doesn't expose tick-box options yet — its .scad ships " +
      "fixed variants (each package above is one). Options land here the " +
      "moment the scad grows them; nothing to rewrite, the page reads the file.",
      `workshop: printed-case option request for ${reg?.name || state.dev}`,
      "Which option would you print if the case offered it? (e.g. solar hood, " +
      "PoE cutout, VESA boss)\n\nDevice: " + state.dev));
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
    const full = el("button", "ws-fullbtn", "⛶");
    full.title = "inspect fullscreen (Esc to leave)";
    full.addEventListener("click", () => {
      if (document.fullscreenElement) document.exitFullscreen();
      else stage3d.requestFullscreen?.();
    });
    // the dock lives in normal flow under the canvas — overlays fight
    // each other the moment either wraps (learned the hard way)
    const chips = el("div", "ws-chips");
    const specs = el("div", "ws-specs");
    specs.hidden = true;
    const dock = el("div", "ws-dock");
    const dockRow = el("div", "ws-dockrow");
    dockRow.append(chips, meshNote);
    dock.append(dockRow, specs);
    stage3d.append(cv, ribbon, full, dock);
    state.scene = new DeviceScene(cv, null);
    state.scene.start();
    state.sceneParts = [];
    cv.__scene = state.scene;
    state.viewport = { stage3d, cv, ribbon, meshNote, chips, specs };
  }
  const { stage3d, ribbon, meshNote } = state.viewport;
  right.append(stage3d);
  const hint = el("p", "muted fineprint ws-viewhint",
    "drag to orbit · scroll or pinch to zoom · tap a part chip to inspect it alone · ⛶ fullscreen");
  right.append(hint);

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
  showSelectionIn3D(meshNote);
  renderChecklist(check);
}

function renderChecklist(box) {
  const d = wsDev();
  box.innerHTML = "";
  const reg = state.registry.devices.find((x) => x.id === state.dev);
  const m = matchPackage();
  box.append(el("h3", null,
    `Your ${reg?.name || state.dev}` +
    (m.pkg ? ` — ${m.exact ? m.pkg.label : "custom"}` : "")));

  const parts = selectedParts();

  // volumes stream in after the first paint — refresh just this panel
  // when they land (a full update() would flicker the 3D viewport)
  const missing = parts.filter((p) => !state.volumes.has(p.file));
  if (missing.length) {
    Promise.all(missing.map((p) => loadPartMesh(p).catch(() => null)))
      .then((r) => {
        if (r.some(Boolean) && box.isConnected && state.stage === "configure") {
          renderChecklist(box);
        }
      });
  }

  // the running-total ticker: price, prints, plastic — live as you tick
  const bom = state.build.devices?.[state.dev]?.bom;
  if (bom || parts.length) {
    const tick = el("div", "ws-ticker");
    if (bom) {
      const total = (requiredBom().usd || 0) + optionBomRows().usd;
      tick.append(el("strong", "ws-tickbig", `$${total.toFixed(2)}`),
        el("span", "muted", " in parts"));
    }
    if (parts.length) {
      tick.append(el("span", "ws-ticksep", "·"),
        el("strong", "ws-tickbig", String(parts.length)),
        el("span", "muted", parts.length === 1 ? " print" : " prints"));
      const grams = parts.reduce((s, p) => s + (state.volumes.get(p.file) || 0), 0) * PETG_G_CM3;
      if (grams > 0) {
        tick.append(el("span", "ws-ticksep", "·"),
          el("strong", "ws-tickbig", `≤ ${Math.round(grams)} g`),
          el("span", "muted", " of plastic"));
      }
    }
    box.append(tick);
  }
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

  if (hasConfigurator()) {
    const dl = el("button", "ws-dl");
    dl.textContent = "⤓ Download your OpenSCAD parameter set";
    dl.addEventListener("click", downloadParameterSet);
    box.append(dl);
    const how = el("p", "muted fineprint",
      `Saves as ${specName(state.dev, matchPackage(), optionVector())}.json — ` +
      `in OpenSCAD open ${d.scad}, Window ▸ Customizer ▸ import that file, ` +
      "and your exact spec appears as a preset.");
    box.append(how);
  }
  const explorer = el("p", "muted fineprint");
  const a = el("a", null, "every dimension, in the parameter explorer →");
  a.href = `fleet.html#${state.dev}`;
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
    Promise.all(missing.map((p) => loadPartMesh(p).catch(() => null)))
      .then((r) => { if (r.some(Boolean) && state.stage === "print") update(); });
  }

  const list = el("div", "ws-printlist");
  for (const p of parts) {
    const row = el("div", "ws-printrow");
    const head = el("div", "ws-printhead");
    head.append(el("strong", null, p.name),
      el("span", "chip chip-dim", p.material || "PETG / ASA"));
    head.append(p.preview_mesh
      ? el("span", "chip", "preview — not yet print-validated")
      : el("span", "chip chip-live", "print-validated"));
    const vol = state.volumes.get(p.file);
    if (vol) head.append(el("span", "muted", `solid ≈ ${Math.round(vol * PETG_G_CM3)} g`));
    const a = el("a", "ws-stl", "STL ⤓");
    a.href = partUrl(p); a.download = p.file.replace(/^preview\//, "");
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
  link.href = `fleet.html#${state.dev}`;
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
    root.append(soonCard(
      n || "BOM pending for this device — it lands here automatically the "
        + "moment a bom_*.csv exists.",
      `hardware: BOM request for ${state.dev}`,
      "The workshop's Gather stage is waiting on a docs/hardware BOM CSV "
      + "for this device.\n\nDevice: " + state.dev));
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
    root.append(soonCard(
      "No written assembly walk-through for this device yet — the enclosure "
      + "catalog carries per-case steps for WAP and Vision today, and any "
      + "§Assembly section added to its README appears here automatically.",
      `docs: assembly steps request for ${state.dev}`,
      "The workshop's Assemble stage is waiting on a '## Assembly' section "
      + "in docs/hardware/enclosure/README.md for this device.\n\nDevice: "
      + state.dev));
  }

  const tpls = (wsDev().templates || []).map((t) => [t, state.data.templates[t]]);
  if (!tpls.length) {
    root.append(soonCard(
      "No 1:1 drill template for this device yet — templates render from "
      + "canary_templates_2d.scad and appear here the moment one exists.",
      `hardware: drill template request for ${state.dev}`,
      "The workshop's Assemble stage would love a paper install template "
      + "(canary_templates_2d.scad mode) for this device.\n\nDevice: " + state.dev));
  }
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
    a2.href = `fleet.html#${state.dev}`;
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
  const on = (wsDev().options || []).filter((o) => state.options[o.id] && isUserToggle(o));
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
  a.href = `fleet.html#${state.dev}`;
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
  else for (const o of d.options || []) if (isUserToggle(o)) state.options[o.id] = o.default;
}

async function boot() {
  const [data, registry, enclosures, build, catalog] = await Promise.all([
    loadJson("devices/workshop.json"),
    loadJson("devices/registry.json"),
    loadJson("devices/enclosures.json"),
    loadJson("devices/build.json"),
    loadJson("devices/catalog.json").catch(() => null),
  ]);
  state.data = data; state.registry = registry;
  state.enclosures = enclosures; state.build = build;
  // the catalog manifest, indexed by scad basename — lets the workshop show the
  // env rating / flavors / alternatives the SCADs declare, not just its options
  state.catalogByScad = new Map(
    ((catalog && catalog.products) || []).map((p) => [p.scad, p]));

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
