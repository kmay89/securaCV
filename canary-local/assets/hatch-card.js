// The post-flash "first flight" card — the catalog's hatch moment (kicker +
// title + body + ordered steps), the same structure (and the same catalog
// copy) the desktop Flasher's #hatch-card renders (desktop/src/index.html,
// app.js showHatchCard), so the two frontends tell the same story. WHEN it
// appears follows the desktop's receipt gate: serial_receipt:false products
// get it right on the done card; products whose firmware answers `j` earn it
// when that live receipt lands in the monitor; and the two-port Vision earns
// it when its pair completes — on whichever screen the pair finishes.
//
// One shared builder because those screens live in BOTH flash.js and
// we2-flash.js, and flash.js already imports we2-flash.js — a helper here
// avoids an import cycle (the same reason vision-checklist.js exists). The
// wording itself is flash-core.js:hatchMoment (pure, host-tested); this is
// just the thin DOM.

/**
 * Build the hatch-moment card.
 * @param {{kicker:string,title:string,body:string,steps:string[]}} moment
 *   from flash-core.js:hatchMoment(product) — callers guard against null.
 * @param {object} [opts]
 * @param {Document} [opts.document]  injectable for host tests (default: global).
 * @returns {HTMLElement}
 */
export function hatchMomentCard(moment, opts = {}) {
  const doc = opts.document || (typeof document !== "undefined" ? document : null);
  if (!doc) throw new Error("hatchMomentCard needs a document");
  const el = (tag, cls, text) => {
    const n = doc.createElement(tag);
    if (cls) n.className = cls;
    if (text != null) n.textContent = text;
    return n;
  };
  const card = el("div", "flash-nextstep flash-hatch-moment");
  card.append(el("div", "flash-nextstep-kicker", moment.kicker));
  card.append(el("div", "flash-nextstep-title", moment.title));
  card.append(el("p", "flash-nextstep-body", moment.body));
  const ol = el("ol", "flash-steps");
  moment.steps.forEach((s) => ol.append(el("li", null, s)));
  card.append(ol);
  return card;
}
