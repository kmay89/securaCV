// canary-local/assets/minimal.js — the Nursery's quiet mode.
//
// The flasher teaches on purpose: reassurance, layer tours, lessons, help
// dots. Wonderful the first time; wallpaper the fortieth. Minimal mode keeps
// every control, every safety check and every honest number, and folds the
// teaching away — one tap brings the full story back, per screen, without
// changing the preference. OFF by default: the full story is the front door,
// quiet is a choice. The toggle persists per-browser (localStorage); every
// call is safe headless — no storage, no document, it just quietly says off.
//
// What "minimal" means here (flash.css owns the exact list):
//   kept   — every button, the journey bar, progress + live map + meta,
//            errors, the collapsed technical log, receipts, all <details>
//   folded — explainer prose, the reassurance strip, help dots, the layers
//            tour and the coach (those two skip rendering: they run timers)
//
// Vanilla ES module, zero dependencies (repo convention).

const KEY = "nursery.minimal";

export function minimalEnabled() {
  try { return localStorage.getItem(KEY) === "on"; } catch { return false; }
}

export function setMinimalEnabled(on) {
  try { localStorage.setItem(KEY, on ? "on" : "off"); } catch { /* private mode */ }
}

// The toggle chip the pages mount (flash.html's journey row and the settings
// dialog). Reflects and persists state; `onChange` lets the page re-dress
// itself the moment the choice is made.
export function minimalToggle(opts = {}, doc = typeof document === "undefined" ? null : document) {
  if (!doc) return null;
  const btn = doc.createElement("button");
  btn.type = "button";
  btn.className = "nursery-minimal-toggle";
  const sync = () => {
    const on = minimalEnabled();
    btn.textContent = on ? "🪶 minimal on" : "🪶 minimal off";
    btn.setAttribute("aria-pressed", String(on));
    btn.title = on
      ? "Just the essentials — click for the full story on every screen."
      : "Fold the explainers away — every control and check stays, the prose waits one click away.";
  };
  btn.addEventListener("click", () => {
    setMinimalEnabled(!minimalEnabled());
    sync();
    if (opts.onChange) { try { opts.onChange(minimalEnabled()); } catch { /* never break the page */ } }
  });
  sync();
  return btn;
}
