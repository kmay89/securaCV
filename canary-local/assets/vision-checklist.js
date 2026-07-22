// The two-port Vision completion checklist — one shared card so the ESP32 done
// screen (flash.js) and the camera-module done screen (we2-flash.js) render the
// SAME "did you do both ports?" guardrail. Kept in its own module because
// flash.js already imports we2-flash.js; a shared helper here avoids an import
// cycle. All wording lives in core.visionChecklistModel (pure, host-tested);
// this is just the thin DOM.

import { visionChecklistModel } from "./flash-core.js";

/**
 * Build the checklist card.
 * @param {{esp32?:boolean, we2?:boolean}} parts  which ports are flashed so far.
 * @param {object} [opts]
 * @param {Document} [opts.document]     injectable for host tests (default: global).
 * @param {() => void} [opts.onFlashOther]  click handler for the "do the other
 *   port" button; the button is shown only when the pair is incomplete AND this
 *   is provided (so a screen with no route to the other port omits it cleanly).
 * @param {string} [opts.otherLabel]     override the button text.
 * @returns {HTMLElement}
 */
export function visionChecklistCard(parts, opts = {}) {
  const doc = opts.document || (typeof document !== "undefined" ? document : null);
  if (!doc) throw new Error("visionChecklistCard needs a document");
  const m = visionChecklistModel(parts);

  const el = (tag, cls, text) => {
    const n = doc.createElement(tag);
    if (cls) n.className = cls;
    if (text != null) n.textContent = text;
    return n;
  };

  const card = el("div", "vision-checklist" + (m.done ? " is-done" : ""));
  card.append(el("div", "vision-checklist-title", m.title));

  const list = el("ul", "vision-checklist-rows");
  for (const row of m.rows) {
    const li = el("li", "vision-row " + (row.done ? "is-done" : "is-todo"));
    li.append(el("span", "vision-row-mark", row.done ? "✓" : "○"));
    li.append(el("span", "vision-row-label", row.label));
    list.append(li);
  }
  card.append(list);
  card.append(el("p", "vision-checklist-status", m.status));

  if (!m.done && opts.onFlashOther) {
    const label = opts.otherLabel ||
      (m.nextPart === "we2" ? "Load the camera module’s model →" : "Flash the Vision firmware →");
    const cta = el("button", "primary vision-checklist-cta", label);
    cta.addEventListener("click", opts.onFlashOther);
    card.append(cta);
  }
  return card;
}
