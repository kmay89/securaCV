// canary-local/assets/board-identity.js — "which board am I holding?"
//
// A per-product identity panel for the flasher: the labeled reference that
// tells you WHAT the board is and WHERE to look on it (a clean card of its
// pins/features, colour-coded the way the vendor pinouts are), framed by the
// enclosure it becomes — the finished product — with a jump to the live 3D
// model in the Board Room. Everything is drawn from the honest catalogs
// (devices/boards.json + devices/enclosures.json), no scraped images.
//
// Pure DOM, zero dependencies, same-origin fetch only (CSP-safe). Every entry
// point is defensive: if the catalogs or a field are missing, the panel
// quietly renders what it can (or nothing) rather than throwing into the flow.

// The vendor-legend function classes (Seeed's colour language), used to tint
// each pin/feature so a physical board is easy to match.
export const PIN_CLASSES = {
  touch:   { label: "Touch",   color: "#f2a9b6" },
  digital: { label: "Digital", color: "#9fd67f" },
  analog:  { label: "Analog",  color: "#f0a95a" },
  gpio:    { label: "GPIO",    color: "#9bb0c9" },
  i2c:     { label: "I²C",     color: "#8a8f98" },
  power:   { label: "Power",   color: "#e0574d" },
  gnd:     { label: "GND",     color: "#2b3640" },
  spi:     { label: "SPI",     color: "#f2d03b" },
  uart:    { label: "UART",    color: "#4bb3e6" },
  other:   { label: "Feature", color: "#6f7a86" },
};

// Classify a pinout entry into a legend class from its label / pin / gpio text.
// Order matters: the most specific buses win before the generic D#/A#/GPIO.
export function classifyPin(entry) {
  const hay = `${entry.label || ""} ${entry.pin || ""} ${entry.gpio || ""}`.toUpperCase();
  const has = (...w) => w.some((x) => hay.includes(x));
  if (has("TOUCH")) return "touch";
  if (has("GND")) return "gnd";
  // Note: an A# analog pin that merely *senses* battery voltage (e.g.
  // "VBAT sense" on A0) is analog, not a power rail — so classify A#/D# and
  // the buses before the power-rail keywords below reach a sense pin.
  if (/\bA\d/.test(hay) || has("ANALOG", "ADC")) return "analog";
  if (has("5V", "3V3", "VIN", "VOUT", "BAT+", "BAT-", "POWER", "DC")) return "power";
  if (has("I2C", "IIC", "SDA", "SCL")) return "i2c";
  if (has("SPI", "MOSI", "MISO", "SCK", "MISO")) return "spi";
  if (has("UART", "TX", "RX", "RS485", "RS-485")) return "uart";
  if (/\bD\d/.test(hay) || has("DIGITAL")) return "digital";
  if (has("GPIO")) return "gpio";
  return "other";
}

// The short device id the catalogs key on = flash.json product id minus the
// `securacv-` prefix (e.g. securacv-canary-display-watch → canary-display-watch).
export function shortDeviceId(productId) {
  return String(productId || "").replace(/^securacv-/, "");
}

// Resolve product → the board(s) that make it, from boards.json.device_board.
// Returns the first board that carries a pinout (the one worth labelling).
export function boardForProduct(boards, productId) {
  if (!boards || !boards.boards) return null;
  const short = shortDeviceId(productId);
  const ids = (boards.device_board && boards.device_board[short]) || [];
  for (const id of ids) {
    const b = boards.boards[id];
    if (b && Array.isArray(b.pinout) && b.pinout.length) return { id, ...b };
  }
  // fall back to the first known board even without a pinout
  for (const id of ids) if (boards.boards[id]) return { id, ...boards.boards[id] };
  return null;
}

// Resolve product → the enclosure it becomes, from enclosures.json.sets.
// Prefer a set that carries a preview; otherwise the first for this device.
export function enclosureForProduct(enclosures, productId) {
  if (!enclosures || !Array.isArray(enclosures.sets)) return null;
  const short = shortDeviceId(productId);
  const mine = enclosures.sets.filter((s) => s.device === short);
  if (!mine.length) return null;
  return mine.find((s) => s.preview) || mine[0];
}

function dimsLabel(dims) {
  if (!Array.isArray(dims) || dims.length < 2) return null;
  const [w, , d] = dims.length >= 3 ? dims : [dims[0], 0, dims[1]];
  // A round board (watch) reads as a diameter; rectangles as W×D.
  if (Math.abs(w - d) < 0.6) return `Ø${Math.round(w)} mm`;
  return `${Math.round(w)}×${Math.round(d)} mm`;
}

let _cat = null;
async function catalogs() {
  if (_cat) return _cat;
  const grab = async (p) => {
    try { const r = await fetch(p, { cache: "no-store" }); return r.ok ? await r.json() : null; }
    catch { return null; }
  };
  const [boards, enclosures] = await Promise.all([
    grab("devices/boards.json"), grab("devices/enclosures.json"),
  ]);
  _cat = { boards, enclosures };
  return _cat;
}

function el(tag, cls, text) {
  const n = document.createElement(tag);
  if (cls) n.className = cls;
  if (text != null) n.textContent = text;
  return n;
}

// Build the identity panel DOM for a product record (from flash.json) using
// pre-loaded catalogs. Exported pure so it's unit-testable with a DOM shim.
export function buildIdentityPanel(doc, product, boards, enclosures) {
  const board = boardForProduct(boards, product.id);
  if (!board) return null; // nothing honest to show
  const enclosure = enclosureForProduct(enclosures, product.id);

  const mk = (tag, cls, text) => {
    const n = doc.createElement(tag);
    if (cls) n.className = cls;
    if (text != null) n.textContent = text;
    return n;
  };

  const sec = mk("section", "flash-identity");

  // ── the enclosure / product frame (what you're building) ──
  if (enclosure) {
    const frame = mk("div", "flash-identity-frame");
    frame.append(mk("span", "flash-identity-kicker", "You're building"));
    frame.append(mk("span", "flash-identity-product", enclosure.name || product.name));
    if (enclosure.note) frame.append(mk("span", "flash-identity-note", enclosure.note));
    sec.append(frame);
  }

  // ── the white "what it is" card ──
  const card = mk("div", "flash-identity-card");
  card.append(mk("h3", null, board.name || product.board_label || product.name));
  const facts = [board.vendor, product.chip, dimsLabel(board.dims_mm)].filter(Boolean).join(" · ");
  if (facts) card.append(mk("div", "flash-identity-facts", facts));
  card.append(mk("p", "flash-identity-lead",
    "Match it to the board in your hand — here's what each labelled part is and where to look."));

  const list = mk("div", "flash-identity-pins");
  for (const p of board.pinout || []) {
    const cls = classifyPin(p);
    const row = mk("div", "flash-identity-pin");
    const tag = mk("span", "flash-identity-cls");
    tag.textContent = PIN_CLASSES[cls].label;
    tag.style.background = PIN_CLASSES[cls].color;
    tag.style.color = (cls === "gnd") ? "#fff" : "#161616";
    row.append(tag);
    const body = mk("span", "flash-identity-pin-body");
    body.append(mk("b", null, p.label || p.pin || "—"));
    const where = [p.pin, p.gpio].filter(Boolean).join(" · ");
    if (where) body.append(mk("span", "flash-identity-where", " " + where));
    if (p.use) body.append(mk("span", "flash-identity-use", p.use));
    row.append(body);
    if (p.status === "planned") row.append(mk("span", "flash-identity-planned", "planned"));
    list.append(row);
  }
  card.append(list);

  // Live 3D lives in the Board Room (our own CAD, rendered with no deps).
  const more = mk("div", "flash-identity-more");
  const a = mk("a", "ghost small", "See it in 3D — spin the real model →");
  a.href = "boards.html";
  a.target = "_blank"; a.rel = "noopener";
  more.append(a);
  card.append(more);

  sec.append(card);
  return sec;
}

// Mount the identity panel for a product into `container`. Async: loads the
// catalogs (cached) then appends. No-op on any failure.
export async function mountBoardIdentity(container, product) {
  if (!container || !product) return null;
  try {
    const { boards, enclosures } = await catalogs();
    const panel = buildIdentityPanel(document, product, boards, enclosures);
    if (panel) container.append(panel);
    return panel;
  } catch { return null; }
}
