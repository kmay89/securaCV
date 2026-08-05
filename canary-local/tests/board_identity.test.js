// canary-local/tests/board_identity.test.js — the flasher's board identity panel.
//
// Proves the "which board am I holding?" panel is drawn honestly from the
// catalogs: every flashable product resolves (via boards.json.device_board)
// to a board with a labeled pinout, each pin classifies into a vendor-legend
// class, and products with an enclosure surface the product they become. Pure
// logic + a tiny DOM shim for the builder — no browser needed.

const { test } = require("node:test");
const assert = require("node:assert");
const { readFileSync } = require("node:fs");
const { join } = require("node:path");

const ROOT = join(__dirname, "..");
const read = (p) => JSON.parse(readFileSync(join(ROOT, p), "utf8"));
const flash = read("devices/flash.json");
const boards = read("devices/boards.json");
const enclosures = read("devices/enclosures.json");

const mod = () => import("../assets/board-identity.js");

// A DOM shim just wide enough for buildIdentityPanel: createElement + append +
// className/textContent/style/href, with a querySelectorAll over the tree.
function fakeDoc() {
  const make = (tag) => ({
    tag, className: "", textContent: "", style: {}, children: [], attrs: {},
    set href(v) { this.attrs.href = v; }, get href() { return this.attrs.href; },
    set target(v) { this.attrs.target = v; }, set rel(v) { this.attrs.rel = v; },
    append(...ns) { this.children.push(...ns); },
  });
  return { createElement: make };
}
function walk(node, out = []) {
  if (!node) return out;
  out.push(node);
  for (const c of node.children || []) walk(c, out);
  return out;
}
const byClass = (root, cls) => walk(root).filter((n) => String(n.className).split(" ").includes(cls));

test("classifyPin: the vendor-legend classes are picked from label/pin/gpio", async () => {
  const { classifyPin } = await mod();
  assert.strictEqual(classifyPin({ label: "Touch", pin: "I2C", gpio: "CST816S" }), "touch");
  assert.strictEqual(classifyPin({ label: "5V power in", pin: "VIN" }), "power");
  assert.strictEqual(classifyPin({ label: "GND", pin: "GND" }), "gnd");
  assert.strictEqual(classifyPin({ label: "sensor", pin: "SDA", gpio: "GPIO8" }), "i2c");
  assert.strictEqual(classifyPin({ label: "Display SPI", pin: "D8", gpio: "GPIO7" }), "spi");
  assert.strictEqual(classifyPin({ label: "console", pin: "TX", gpio: "GPIO43" }), "uart");
  assert.strictEqual(classifyPin({ label: "VBAT sense", pin: "A0", gpio: "GPIO1" }), "analog");
  assert.strictEqual(classifyPin({ label: "Chirp", pin: "D1", gpio: "GPIO2" }), "digital");
});

test("shortDeviceId: strips the securacv- prefix to the catalog key", async () => {
  const { shortDeviceId } = await mod();
  assert.strictEqual(shortDeviceId("securacv-canary-display-watch"), "canary-display-watch");
  assert.strictEqual(shortDeviceId("securacv-canary-wap"), "canary-wap");
});

test("boardForProduct: mapped products resolve; unmapped skip gracefully (no throw)", async () => {
  const { boardForProduct, shortDeviceId } = await mod();
  // The boards we currently ship CAD + pinout for (incl. both displays) must
  // each resolve to a named board. (canary-sense / the base canary have no
  // device_board entry yet — the panel skips them; a gen_boards.py follow-up.)
  for (const id of [
    "securacv-canary-display-watch", "securacv-canary-display-dash",
    "securacv-canary-wap", "securacv-canary-vision",
  ]) {
    const b = boardForProduct(boards, id);
    assert.ok(b && b.name, `${id}: should resolve to a named board`);
  }
  // Every product either resolves or returns null (the panel then no-ops) —
  // never throws, and never a half-built board.
  for (const p of flash.products) {
    const b = boardForProduct(boards, p.id);
    const mapped = (boards.device_board[shortDeviceId(p.id)] || []).length > 0;
    if (mapped) assert.ok(b, `${p.id}: mapped in device_board but did not resolve`);
    if (b) assert.ok(b.name, `${p.id}: resolved board must have a name`);
  }
});

test("the display products carry a labeled pinout to match in-hand", async () => {
  const { boardForProduct } = await mod();
  for (const id of ["securacv-canary-display-watch", "securacv-canary-display-dash"]) {
    const b = boardForProduct(boards, id);
    assert.ok(b && Array.isArray(b.pinout) && b.pinout.length,
      `${id}: display board should have a pinout to label`);
  }
});

test("enclosureForProduct: display + camera products surface the product they become", async () => {
  const { enclosureForProduct } = await mod();
  const watch = enclosureForProduct(enclosures, "securacv-canary-display-watch");
  assert.ok(watch && watch.name, "watch has an enclosure/product frame");
  assert.strictEqual(watch.device, "canary-display-watch");
});

test("buildIdentityPanel: renders the white card with color-coded pin rows", async () => {
  const { buildIdentityPanel } = await mod();
  const watch = flash.products.find((p) => p.id === "securacv-canary-display-watch");
  const doc = fakeDoc();
  const panel = buildIdentityPanel(doc, watch, boards, enclosures);
  assert.ok(panel, "panel built for the display watch");
  // the labeled card exists with a heading
  assert.strictEqual(byClass(panel, "flash-identity-card").length, 1);
  assert.ok(walk(panel).some((n) => n.tag === "h3" && n.textContent));
  // one color-coded class tag per pinout entry, each with a background color
  const board = boards.boards[(boards.device_board["canary-display-watch"] || [])
    .find((bid) => (boards.boards[bid] || {}).pinout)];
  const rows = byClass(panel, "flash-identity-cls");
  assert.strictEqual(rows.length, board.pinout.length, "a class tag per pin");
  assert.ok(rows.every((r) => r.style && r.style.background), "each class tag is tinted");
  // the enclosure/product frame names what you're building
  assert.strictEqual(byClass(panel, "flash-identity-product").length, 1);
  // and a jump to the live 3D
  assert.ok(walk(panel).some((n) => n.tag === "a" && /boards\.html/.test(n.attrs.href || "")));
});
