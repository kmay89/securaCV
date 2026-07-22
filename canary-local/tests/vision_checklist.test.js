// Host tests for assets/vision-checklist.js — the shared two-port completion
// card. A tiny injected fake `document` (the renderer takes opts.document for
// exactly this) lets us prove the DOM without a browser: the right rows are
// checked, the status reads correctly, the "do the other port" button appears
// only when the pair is incomplete AND a route is provided, and clicking it
// fires the caller's handler.

const { test } = require("node:test");
const assert = require("node:assert");

const mod = () => import("../assets/vision-checklist.js");

// Minimal document: enough of the surface visionChecklistCard touches.
function makeDoc() {
  const createElement = (tag) => ({
    tag,
    className: "",
    textContent: "",
    children: [],
    listeners: {},
    append(...kids) { this.children.push(...kids); },
    addEventListener(type, fn) { (this.listeners[type] = this.listeners[type] || []).push(fn); },
  });
  return { createElement };
}
const allText = (n) =>
  (n.textContent || "") + n.children.map(allText).map((t) => " " + t).join("");
const findAll = (n, pred, out = []) => {
  if (pred(n)) out.push(n);
  n.children.forEach((c) => findAll(c, pred, out));
  return out;
};
const button = (card) => findAll(card, (n) => /vision-checklist-cta/.test(n.className))[0] || null;

test("incomplete pair: shows what's left + a working 'other port' button", async () => {
  const { visionChecklistCard } = await mod();
  let clicked = 0;
  const card = visionChecklistCard({ esp32: true }, {
    document: makeDoc(),
    onFlashOther: () => { clicked++; },
  });
  const text = allText(card);
  assert.match(text, /two flashes/i);      // the title
  assert.match(text, /1 of 2/);            // the count
  assert.match(text, /camera module/i);    // the next port

  // Exactly one row is checked (the ESP32 firmware), the other still open.
  const marks = findAll(card, (n) => /vision-row-mark/.test(n.className)).map((n) => n.textContent);
  assert.deepStrictEqual(marks, ["✓", "○"]);

  // The button routes to the other port and fires the caller's handler.
  const cta = button(card);
  assert.ok(cta, "expected a 'flash the other port' button");
  assert.match(cta.textContent, /camera module/i);
  cta.listeners.click.forEach((fn) => fn());
  assert.strictEqual(clicked, 1);
});

test("camera module first: the button points at the ESP32 firmware", async () => {
  const { visionChecklistCard } = await mod();
  const card = visionChecklistCard({ we2: true }, { document: makeDoc(), onFlashOther() {} });
  const marks = findAll(card, (n) => /vision-row-mark/.test(n.className)).map((n) => n.textContent);
  assert.deepStrictEqual(marks, ["○", "✓"]);       // firmware open, camera checked
  assert.match(button(card).textContent, /Vision firmware/i);
});

test("both done: the celebration, no button even if a route is given", async () => {
  const { visionChecklistCard } = await mod();
  const card = visionChecklistCard({ esp32: true, we2: true }, {
    document: makeDoc(),
    onFlashOther() { throw new Error("must not render a button when done"); },
  });
  assert.match(card.className, /is-done/);
  assert.match(allText(card), /fully set up/i);
  const marks = findAll(card, (n) => /vision-row-mark/.test(n.className)).map((n) => n.textContent);
  assert.deepStrictEqual(marks, ["✓", "✓"]);
  assert.strictEqual(button(card), null);
});

test("no route provided: no button, even when incomplete", async () => {
  const { visionChecklistCard } = await mod();
  const card = visionChecklistCard({ esp32: true }, { document: makeDoc() });
  assert.strictEqual(button(card), null);
});
