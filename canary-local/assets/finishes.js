// canary-local/assets/finishes.js — the filament finish, as data.
//
// One curated set of printed-shell finishes, shared by every 3D surface on
// the site: the card gallery, the open device sheet, the assembly lab, and
// the WAP plug-in scene. Picking a finish re-lines all of them at once.
//
// Each finish is TWO-TONE on purpose — a bold primary body with a darker
// secondary for stands / rear covers / vent insets — because a single
// saturated colour reads cheap, while a body + graphite accent reads
// designed. Colours are linear-ish RGB tuned for scene3d's shader (ambient
// 0.30 + diffuse), not raw hex. `swatch` is the CSS hex for the picker dot.
//
// FUNCTIONAL part colours (camera glass, lens barrel, solar sliver, screen
// bezel) are NOT filament and stay literal in the builders — the finish only
// moves the printed shell, which is the honest thing a filament swap does.
//
// DOM-free + storage-guarded so the Node test suite can import the module
// graph (wap-ui → scene3d → finishes) without a browser.

export const FINISHES = [
  {
    id: "canary", name: "Canary", swatch: "#f2b705",
    shell: [0.98, 0.78, 0.16],   // bold warm canary body
    shell2: [0.15, 0.16, 0.19],  // graphite stand / back / vent inset
    gasket: [0.08, 0.08, 0.09],
    beacon: [1.0, 0.97, 0.86],   // the status LED reads white-hot on yellow
    dark: false,
  },
  {
    id: "walnut", name: "Walnut", swatch: "#6b4423",
    shell: [0.42, 0.26, 0.13],   // warm walnut brown
    shell2: [0.24, 0.15, 0.08],  // deeper walnut for the secondary
    gasket: [0.10, 0.07, 0.05],
    beacon: [1.0, 0.83, 0.31],   // canary LED pops on brown
    dark: true,
  },
  {
    id: "graphite", name: "Graphite", swatch: "#24262b",
    shell: [0.17, 0.18, 0.21],   // near-black with a soft sheen
    shell2: [0.10, 0.11, 0.13],
    gasket: [0.07, 0.07, 0.08],
    beacon: [1.0, 0.83, 0.31],
    dark: true,
  },
  {
    id: "bone", name: "Bone", swatch: "#e8e4d8",
    shell: [0.90, 0.87, 0.80],   // the classic eggshell (kept as an option)
    shell2: [0.74, 0.71, 0.64],
    gasket: [0.09, 0.09, 0.10],
    beacon: [1.0, 0.83, 0.31],
    dark: false,
  },
];

const KEY = "scv-finish";
const byId = (id) => FINISHES.find((f) => f.id === id);

// storage may be absent (Node import graph) or blocked (private mode)
function stored() {
  try { return localStorage.getItem(KEY); } catch { return null; }
}
function persist(id) {
  try { localStorage.setItem(KEY, id); } catch { /* ignore */ }
}

let active = byId(stored()) || FINISHES[0];
const listeners = new Set();

/** The finish every builder should paint with right now. */
export function activeFinish() { return active; }

/** Switch the active finish; notifies subscribers (which re-render). */
export function setFinish(id) {
  const f = byId(id);
  if (!f || f === active) return active;
  active = f;
  persist(id);
  for (const fn of listeners) { try { fn(f); } catch (e) { console.error("finish listener", e); } }
  return active;
}

/** Subscribe to finish changes; returns an unsubscribe. */
export function onFinishChange(fn) { listeners.add(fn); return () => listeners.delete(fn); }

/**
 * A small swatch picker (browser-only). `onPick` fires AFTER the active
 * finish is set, so callers just re-render. Returns the row element.
 */
export function buildFinishPicker(onPick) {
  const row = document.createElement("div");
  row.className = "finish-picker";
  row.setAttribute("role", "radiogroup");
  row.setAttribute("aria-label", "Filament finish");
  const dots = [];
  for (const f of FINISHES) {
    const b = document.createElement("button");
    b.className = "finish-swatch";
    b.type = "button";
    b.title = f.name;
    b.style.setProperty("--sw", f.swatch);
    b.setAttribute("role", "radio");
    b.setAttribute("aria-label", f.name);
    const paint = () => {
      const on = f === active;
      b.classList.toggle("on", on);
      b.setAttribute("aria-checked", on ? "true" : "false");
    };
    b.addEventListener("click", () => {
      setFinish(f.id);
      dots.forEach((d) => d());
      if (onPick) onPick(active);
    });
    dots.push(paint);
    paint();
    row.append(b);
  }
  const label = document.createElement("span");
  label.className = "finish-label";
  const setLabel = () => { label.textContent = active.name; };
  setLabel();
  onFinishChange(() => { dots.forEach((d) => d()); setLabel(); });
  row.append(label);
  return row;
}
