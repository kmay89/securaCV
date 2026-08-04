// canary-local/assets/finishes.js — the filament finish, as data.
//
// One curated set of printed-shell finishes, shared by every 3D surface on
// the site: the card gallery, the open device sheet, the assembly lab, and
// the WAP plug-in scene. Picking a finish re-lines all of them at once.
//
// Each finish is TWO-TONE on purpose — a bold primary body with a darker
// secondary for stands / rear covers / vent insets — because a single
// saturated color reads cheap, while a body + graphite accent reads
// designed. Colors are linear-ish RGB tuned for scene3d's shader (ambient
// 0.30 + diffuse), not raw hex. `swatch` is the CSS hex for the picker dot.
//
// FUNCTIONAL part colors (camera glass, lens barrel, solar sliver, screen
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
/** True once the user has actually chosen a finish (so we stop showcasing). */
export function hasUserChoice() { return !!stored(); }

// `committed` is the logical finish builders paint with and tests read; it is
// the persisted choice or the default. `shown`/`prev` drive the on-screen
// cross-fade (the ambient showcase and manual picks both animate through it),
// so the committed finish and the pixels can differ for ~1.5 s at a time.
let committed = byId(stored()) || FINISHES[0];
let shown = committed;   // the finish being faded TO (what the picker marks)
let prevShown = committed; // the finish being faded FROM
let fadeStart = -1e9, fadeDur = 1500;
const now = () => (typeof performance !== "undefined" ? performance.now() : 0);
const listeners = new Set();

/** The finish every builder should paint with at build time (and tests read). */
export function activeFinish() { return committed; }
/** The finish the picker should highlight right now (tracks the showcase). */
export function shownFinish() { return shown; }

const ROLE_KEY = { shell: "shell", shell2: "shell2", gasket: "gasket", beacon: "beacon" };
const smooth = (t) => t * t * (3 - 2 * t);
const lerp3 = (a, b, t) => [a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t];

/**
 * The live RGB for a filament role, mid-cross-fade — read per-frame by the
 * renderer for role-tagged parts. Returns null for non-filament roles so
 * functional parts (glass, lens) keep their literal color.
 */
export function finishColor(role) {
  const key = ROLE_KEY[role];
  if (!key) return null;
  const t = smooth(Math.max(0, Math.min(1, (now() - fadeStart) / fadeDur)));
  return t >= 1 ? shown[key] : lerp3(prevShown[key], shown[key], t);
}

function notify(f) {
  for (const fn of listeners) { try { fn(f); } catch (e) { console.error("finish listener", e); } }
}

// Begin a cross-fade to `f`. `commit` persists it as the user's real choice.
function fadeTo(f, { commit } = {}) {
  if (commit) { committed = f; persist(f.id); }
  if (f === shown) return;
  prevShown = shown;
  shown = f;
  fadeStart = now();
  notify(f);
}

/** Switch the finish (a manual pick): commits, persists, cross-fades. */
export function setFinish(id) {
  const f = byId(id);
  if (!f) return committed;
  stopFinishShowcase();
  fadeTo(f, { commit: true });
  if (f === committed && f === shown) notify(f); // first pick of the default still repaints the picker
  return committed;
}

/** Subscribe to finish changes (manual or showcase); returns an unsubscribe. */
export function onFinishChange(fn) { listeners.add(fn); return () => listeners.delete(fn); }

// ── the ambient showcase ────────────────────────────────────────────────
// While it runs, the models cross-fade slowly through the whole palette in
// sync — a calm "you can make it yours" demo. It stops for good the moment
// the user picks a swatch (setFinish), and never starts if they already have
// a saved choice or asked for reduced motion.
let cycleTimer = null;
export function startFinishShowcase({ hold = 4600 } = {}) {
  stopFinishShowcase();
  let i = FINISHES.indexOf(shown);
  const step = () => {
    i = (i + 1) % FINISHES.length;
    fadeTo(FINISHES[i]); // no commit — a preview, not their choice
    cycleTimer = setTimeout(step, hold + fadeDur);
  };
  cycleTimer = setTimeout(step, hold);
}
export function stopFinishShowcase() {
  if (cycleTimer) { clearTimeout(cycleTimer); cycleTimer = null; }
}
export function showcaseRunning() { return cycleTimer !== null; }

/**
 * A small swatch picker (browser-only). Its highlight + label follow the
 * SHOWN finish, so it moves gently along with the ambient showcase; a click
 * commits that swatch and ends the showcase. `onPick` fires after a manual
 * pick. Returns the row element.
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
      const on = f === shown;
      b.classList.toggle("on", on);
      b.setAttribute("aria-checked", on ? "true" : "false");
    };
    b.addEventListener("click", () => {
      setFinish(f.id);            // commits + stops the showcase
      row.classList.remove("cycling");
      dots.forEach((d) => d());
      if (onPick) onPick(committed);
    });
    dots.push(paint);
    paint();
    row.append(b);
  }
  const label = document.createElement("span");
  label.className = "finish-label";
  const setLabel = () => { label.textContent = shown.name; };
  setLabel();
  // while the showcase runs, a hairline ring pulses under the shown swatch
  row.classList.toggle("cycling", showcaseRunning());
  onFinishChange(() => {
    dots.forEach((d) => d());
    setLabel();
    row.classList.toggle("cycling", showcaseRunning());
  });
  return row;
}
