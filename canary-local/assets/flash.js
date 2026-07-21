// canary-local/assets/flash.js — the browser flasher.
//
// Plug a Canary into a Chromium browser over USB-C and it flashes itself:
// detect the chip, read what firmware is already on it, take a safety copy
// automatically, write a signed release image, verify the bytes against the
// chip's own MD5, and watch it boot — all offline, nothing phones anywhere.
// The whole thing is built around one true promise: you cannot brick the
// board from here (the ESP32's first-stage bootloader is mask ROM), and the
// UI never lets you reach for an image that isn't meant for the silicon in
// hand.
//
// Beyond flashing, the page doubles as a triage bench: a health check reads
// the board's partition map, both firmware slots, OTA history, crash-dump
// presence and the witness-chain counters straight off the flash (read-only,
// secrets never surfaced), and a serial monitor talks to the firmware's
// built-in command console.
//
// DOM-free logic + parsers live in flash-core.js (tested under node --test);
// the flashing engine is the vendored, self-hosted esptool-js. This file is
// the glue and the theatre.

import { ESPLoader, Transport } from "./vendor/esptool-js/bundle.js";
import { md5Raw } from "./vendor/md5/md5.js";
import * as core from "./flash-core.js";
import { phaseModule } from "./we2-flash.js";

const GH = "https://github.com/kmay89/securaCV/blob/main/";
const LESSON = "wap.html"; // the guided BOOT/RESET + PlatformIO/Arduino path

const $ = (sel, root = document) => root.querySelector(sel);
const el = (tag, cls, text) => {
  const n = document.createElement(tag);
  if (cls) n.className = cls;
  if (text != null) n.textContent = text;
  return n;
};
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const state = {
  catalog: null,
  manifest: null,       // release manifest, or {__missing:true}
  session: null,        // { port, transport, esploader }
  chip: null,           // "ESP32-S3"
  chipDesc: null,
  mac: null,
  flashBytes: null,
  current: null,        // { productName, version, projectName } | { unknown:true }
  backup: null,         // { bytes, name, mac } once taken this session
  report: null,         // last health-check result
  busy: false,
};

// ── boot ───────────────────────────────────────────────────────────────────
// ── global safety net ───────────────────────────────────────────────────────
// Every KNOWN failure path already renders its own card. This backstops the
// UNKNOWN ones: a stray unhandled promise rejection or uncaught error that
// escapes while an operation is in flight would otherwise leave a frozen
// progress bar. Instead we reset the busy flag and show a friendly, recoverable
// error card. Idle-time errors are left to surface in the console untouched.
let safetyNetInstalled = false;
function installSafetyNet() {
  if (safetyNetInstalled) return;
  safetyNetInstalled = true;
  const onFail = (err) => {
    if (!state.busy) return;
    state.busy = false;
    try { setPhase(flashError(err || new Error("unexpected error"), {})); } catch {}
  };
  window.addEventListener("unhandledrejection", (ev) => onFail(ev && ev.reason));
  window.addEventListener("error", (ev) => onFail(ev && (ev.error || ev.message)));
}

async function boot() {
  installSafetyNet();
  const mount = $("#flash");
  mount.innerHTML = "";

  if (!("serial" in navigator)) {
    mount.append(renderUnsupported());
    return;
  }

  try {
    const resp = await fetch("devices/flash.json");
    if (!resp.ok) throw new Error("HTTP " + resp.status);
    state.catalog = await resp.json();
  } catch (e) {
    mount.append(errorBox("Couldn’t load the flasher catalog",
      "Reload the page. If it keeps happening, the guided path still works.", true));
    return;
  }

  // A valid-JSON-but-malformed catalog would throw mid-render (renderReassurance
  // reads no_brick.points, the picker reads products[].chip). Catch it here with
  // a clear message instead of a blank page.
  const catErrs = core.validateCatalog(state.catalog);
  if (catErrs.length) {
    const box = errorBox("The flasher catalog looks incomplete",
      "The page loaded but its device list didn’t validate, so flashing is paused. " +
      "Reload; if it persists, the guided path still works.", true);
    const raw = el("details", "flash-rawerr");
    raw.append(el("summary", null, "Technical details"));
    raw.append(el("pre", null, catErrs.join("\n")));
    box.append(raw);
    const guide = el("a", "ghost", "Use the guided flash instead →");
    guide.href = LESSON;
    box.append(guide);
    mount.append(box);
    return;
  }

  const flow = el("div", "flash-flow");
  flow.id = "flash-flow";
  mount.append(flow);
  mount.append(renderReassurance());

  renderVersionStrip();
  setPhase(phaseConnect());
}

function renderVersionStrip() {
  const strip = $("#flash-versions");
  if (!strip) return;
  strip.innerHTML = "";
  const pill = (label, val) => {
    const p = el("span", "pill");
    p.append(el("strong", null, label + " "), document.createTextNode(val));
    return p;
  };
  strip.append(pill("firmware train", state.catalog.fw_train));
  strip.append(pill("engine", "esptool-js (vendored, offline)"));
}

// The flow is a single swappable panel; the reassurance strip persists below.
function setPhase(node) {
  const flow = $("#flash-flow");
  flow.innerHTML = "";
  flow.append(node);
  flow.scrollIntoView({ behavior: "smooth", block: "nearest" });
}

// ── the persistent "you can't mess up" strip (compact, below the flow) ──────
function renderReassurance() {
  const wrap = el("div", "flash-reassure-strip");
  const nb = state.catalog.no_brick;

  const promise = el("details", "flash-card flash-promise");
  const sum = el("summary");
  sum.append(el("span", "flash-promise-badge", "🛟"));
  sum.append(el("span", null, nb.headline));
  promise.append(sum);
  promise.append(el("p", "muted", nb.why));
  const ul = el("ul", "flash-checklist");
  nb.points.forEach((pt) => {
    const li = el("li");
    li.append(el("span", "flash-check", "✓"), document.createTextNode(pt));
    ul.append(li);
  });
  promise.append(ul);
  wrap.append(promise);

  const help = el("details", "flash-card flash-recovery");
  help.append(el("summary", null, "Board not showing up? Fixes →"));
  state.catalog.recovery.forEach((r) => {
    const row = el("div", "flash-recovery-row");
    row.append(el("p", "flash-recovery-when", r.when));
    row.append(el("p", "muted", r.do));
    help.append(row);
  });
  const lessonP = el("p", "fineprint");
  const a = el("a", "start-link", "Prefer the step-by-step guide with pictures →");
  a.href = LESSON;
  lessonP.append(a);
  help.append(lessonP);
  wrap.append(help);

  wrap.append(renderTrustCard());

  const privacy = el("p", "fineprint flash-privacy");
  privacy.textContent =
    "Everything runs in your browser. The flasher engine is served from this " +
    "site, not a CDN; the only network call is fetching the signed firmware " +
    "image you choose. Nothing about your board leaves this page.";
  wrap.append(privacy);
  return wrap;
}

// ── the trust card: what "secure" means here, honestly ──────────────────────
// For the skeptical reader. Every claim in here is mechanically checkable on
// this very page (receipts, health check, flash map) or documented in the
// repo's threat model — including the limits.
function renderTrustCard() {
  const card = el("details", "flash-card flash-trust");
  card.append(el("summary", null, "What does “secure” mean here? The honest version →"));

  const sec = (title, ...ps) => {
    const d = el("div", "flash-trust-sec");
    d.append(el("h3", null, title));
    ps.forEach((p) => d.append(el("p", "muted", p)));
    return d;
  };

  card.append(sec("Every byte is checked, and you can check the checker.",
    "Four separate guards, all verifiable: images come from signed releases; " +
    "your browser recomputes the SHA-256 fingerprint and compares it to the " +
    "published one before a single byte is written; after writing, the chip " +
    "itself recomputes a checksum over the written range; and the chip guard " +
    "refuses any image built for different silicon. After an install, “show " +
    "the receipts” lists the exact numbers, and the health check re-reads " +
    "reality straight off the chip."));

  card.append(sec("The board's crypto, in one breath.",
    "On first boot, the Canary mints its own Ed25519 identity keypair on the " +
    "chip. The private key is born there and never leaves — there is no " +
    "export function, and this page never reads it (the health check shows " +
    "presence only). Every witness record is hash-chained to the previous " +
    "one and signed with that key, so an edited or deleted record leaves a " +
    "visible seam. What you see here — counters, fingerprints, the chain " +
    "head — is the public half. Verifying evidence needs no one's " +
    "permission; forging it needs the key that never left the board."));

  card.append(sec("What's not at risk from this page.",
    "Bricking — the first-stage bootloader is mask ROM, physically read-only. " +
    "Key theft through the browser — the identity key is never transmitted, " +
    "here or anywhere. Phoning home — the page works offline; the only " +
    "network call fetches the signed image you chose."));

  card.append(sec("What is at risk, honestly.",
    "Physical possession. A standard dev board answers USB: anyone holding " +
    "it with a cable can read or rewrite its flash — that is exactly how " +
    "this page's own backup works, and pretending otherwise would be " +
    "theater. The project's Phase-2 provisioning closes this with flash " +
    "encryption and eFuse locks (then the flash reads back as ciphertext), " +
    "documented — limits included — in docs/security/THREAT_MODEL.md."));

  const bp = el("div", "flash-trust-sec");
  bp.append(el("h3", null, "Best practices, in four lines."));
  const ul = el("ul", "flash-checklist");
  [
    "Treat backup files like house keys — they contain everything on the board, identity key and saved WiFi included.",
    "Selling or giving a board away? Run Advanced → full erase first. It destroys the identity key and all settings.",
    "For a board deployed somewhere physically exposed, use Phase-2 secure provisioning (flash encryption + eFuse).",
    "After any install or restore, run the health check — fingerprints and witness counters, read off the chip.",
  ].forEach((t) => {
    const li = el("li");
    li.append(el("span", "flash-check", "✓"), document.createTextNode(t));
    ul.append(li);
  });
  bp.append(ul);
  card.append(bp);

  card.append(sec("So: “secure” here means…",
    "You can verify exactly what runs (open source, signed, checksummed at " +
    "every step). Nothing leaves (offline by construction). Everything is " +
    "reversible (backup and restore, forever). And the one real limit — " +
    "physical access — is stated out loud instead of hidden."));
  return card;
}

// ── phase: unsupported browser ──────────────────────────────────────────────
// A tiny Chrome-ish mark, drawn inline (nominative use — it just needs to be
// recognizable at a glance next to the arrow).
function chromeMark(size = 40) {
  const s = document.createElementNS("http://www.w3.org/2000/svg", "svg");
  s.setAttribute("viewBox", "0 0 48 48");
  s.setAttribute("width", size); s.setAttribute("height", size);
  s.innerHTML =
    '<circle cx="24" cy="24" r="22" fill="#fff"/>' +
    '<path d="M24 2a22 22 0 0 1 19.05 11H24a11 11 0 0 0-9.53 5.5z" fill="#EA4335"/>' +
    '<path d="M43.05 13A22 22 0 0 1 24 46l9.53-16.5A11 11 0 0 0 34 13z" fill="#FBBC05"/>' +
    '<path d="M24 46A22 22 0 0 1 4.96 13l9.51 16.5A11 11 0 0 0 24 35z" fill="#34A853"/>' +
    '<circle cx="24" cy="24" r="10" fill="#4285F4" stroke="#fff" stroke-width="2"/>';
  return s;
}

function renderUnsupported() {
  const b = core.detectBrowser(navigator.userAgent, navigator.maxTouchPoints || 0);
  const box = el("section", "flash-card flash-unsupported");

  box.append(el("h2", null, b.mobile
    ? `${b.label} can’t reach the board — you’ll need a computer with Chrome`
    : `${b.label} can’t talk to USB boards — you’re one hop away`));

  // The arrow: where you are → where it works.
  const hop = el("div", "flash-hop");
  const from = el("div", "flash-hop-item");
  from.append(el("span", "flash-hop-icon", b.icon));
  from.append(el("span", "flash-hop-label", b.label));
  const arrow = el("span", "flash-hop-arrow", "⟶");
  const to = el("div", "flash-hop-item flash-hop-to");
  const chromeIcon = el("span", "flash-hop-icon");
  chromeIcon.append(chromeMark(40));
  to.append(chromeIcon);
  to.append(el("span", "flash-hop-label",
    b.mobile ? "Chrome on a computer" : "Chrome (or Edge / Brave)"));
  hop.append(from, arrow, to);
  box.append(hop);

  box.append(el("p", "muted",
    "In-browser flashing uses Web Serial, which today only Chromium browsers " +
    "on a computer have: Chrome, Edge, Brave, Opera, Arc. Safari and Firefox " +
    "can’t do it yet."));

  // On a phone or tablet, installing Chrome HERE wouldn't help (mobile Chrome
  // has no Web Serial) — so the primary action is getting this link onto a
  // computer, and the Chrome install link only leads on desktops.
  const row = el("div", "flash-row flash-hop-actions");
  const copy = el("button", b.mobile ? "primary" : "ghost",
    b.mobile ? "Copy the link for your computer" : "Copy this page’s link");
  copy.addEventListener("click", async () => {
    try {
      await navigator.clipboard.writeText(location.href);
      copy.textContent = "✓ copied — open it in Chrome on your computer";
    } catch {
      copy.textContent = location.href; // worst case: show it to select by hand
    }
  });
  if (b.mobile) {
    row.append(copy);
    box.append(row);
    box.append(el("p", "fineprint",
      "No Chrome on that computer yet? It’s a free download at google.com/chrome — " +
      "or use Edge or Brave, already on most machines."));
  } else {
    const get = el("a", "primary", "Get Chrome (free) →");
    get.href = "https://www.google.com/chrome/";
    get.target = "_blank";
    get.rel = "noopener";
    row.append(get, copy);
    box.append(row);
  }

  // What the working setup looks like: board — cable — computer with Chrome.
  const rig = el("div", "flash-rig");
  const board = el("div", "flash-rig-item");
  board.append(el("span", "flash-rig-icon", "🐤"));
  board.append(el("span", "flash-rig-label", "your Canary"));
  const cable = el("div", "flash-rig-cable");
  cable.append(el("span", "flash-rig-plug"), el("span", "flash-rig-wire"), el("span", "flash-rig-plug"));
  const pc = el("div", "flash-rig-item");
  const pcIcon = el("span", "flash-rig-icon flash-rig-pc", "💻");
  const badge = el("span", "flash-rig-badge");
  badge.append(chromeMark(16));
  pcIcon.append(badge);
  pc.append(pcIcon);
  pc.append(el("span", "flash-rig-label", "computer running Chrome"));
  rig.append(board, cable, pc);
  box.append(rig);
  box.append(el("p", "fineprint",
    "USB-C data cable, any computer: Windows, Mac, Linux, or a Chromebook."));

  if (b.id === "ios") {
    box.append(el("p", "flash-note flash-note-soft",
      "Through the iPhone/iPad charging port? Sadly no — Apple requires every " +
      "browser on iOS and iPadOS (even the one named Chrome) to use Safari’s " +
      "engine underneath, and that engine isn’t allowed to talk to the port. " +
      "The cable would power the board, but no web page may speak to it."));
  } else if (b.id === "android") {
    box.append(el("p", "flash-note flash-note-soft",
      "On Android, Chrome doesn’t carry Web Serial either — it’s a desktop " +
      "feature. A laptop or desktop is the way."));
  }

  const row2 = el("div", "flash-row");
  const guide = el("a", "ghost", "No Chrome today? Use the guided flash instead →");
  guide.href = LESSON;
  row2.append(guide);
  box.append(row2);
  box.append(el("p", "fineprint",
    "Same firmware, same safety — just a few more steps in a terminal. When " +
    "you’re next at Chrome on a computer, come back and it’s two clicks."));
  return box;
}

// ── download mode: the BOOT/RESET gesture, one shared component ─────────────
// The flasher usually flips the board into download mode by itself over USB
// (DTR/RTS); the by-hand gesture is the fallback, and it's identical on every
// chip we ship (see catalog chips[].download_mode). Rendered everywhere the
// user might wonder "what state is my board in?".
const kbd = (t) => el("kbd", "flash-kbd", t);

function downloadModeSteps() {
  const wrap = el("div", "flash-dlmode");
  wrap.append(el("p", "flash-dlmode-title",
    "Download mode by hand — the “flash me” state:"));
  const ol = el("ol", "flash-steps flash-dlmode-steps");
  const li1 = el("li");
  li1.append(document.createTextNode("Hold the "), kbd("BOOT"),
    document.createTextNode(" button down (marked B)."));
  const li2 = el("li");
  li2.append(document.createTextNode("While holding it, tap "), kbd("RESET"),
    document.createTextNode(" once (marked R)."));
  const li3 = el("li");
  li3.append(document.createTextNode("Let go of "), kbd("BOOT"),
    document.createTextNode(" — the board sits quietly, waiting for firmware."));
  ol.append(li1, li2, li3);
  wrap.append(ol);
  wrap.append(el("p", "fineprint",
    "To leave download mode, just tap RESET (or unplug and replug) — the board boots its firmware again."));
  return wrap;
}

// A small badge stating which mode the board is in right now. We always know:
// if esptool has it, it's in download mode; if the monitor has it, it's
// running its firmware.
function modeBadge(mode) {
  const b = el("span", "flash-mode flash-mode-" + mode);
  b.append(el("span", "flash-mode-dot", "●"));
  b.append(document.createTextNode(mode === "download"
    ? "download mode — safe to flash, firmware paused"
    : "running its firmware"));
  return b;
}

// ── phase: connect ──────────────────────────────────────────────────────────
function phaseConnect() {
  const box = el("section", "flash-card flash-connect");
  box.append(el("div", "flash-big-emoji flash-plug", "🔌"));
  box.append(el("h2", null, "Plug in your Canary, then let’s meet it"));
  box.append(el("p", "muted",
    "Connect the board to this computer with a USB-C data cable. When you " +
    "click Connect, your browser asks which device — pick the one that " +
    "appears (often “USB JTAG/serial” or “USB Serial”)."));

  const btn = el("button", "primary flash-connect-btn", "Connect your Canary");
  btn.addEventListener("click", onConnect);
  box.append(btn);

  const hint = el("p", "fineprint flash-connect-hint",
    "Nothing is written yet — connecting only lets the page look at the board.");
  box.append(hint);

  // The BOOT/RESET gesture, one click away. Most boards never need it — the
  // flasher flips them into download mode itself — but knowing the move is
  // half the reassurance.
  const dl = el("details", "flash-dlmode-details");
  dl.append(el("summary", null, "Board won’t show up, or want to do it by hand? Download mode →"));
  dl.append(el("p", "muted",
    "Usually you don’t need this: when you click Connect, the flasher puts the " +
    "board into download mode on its own. If it can’t, do it by hand:"));
  dl.append(downloadModeSteps());
  box.append(dl);

  // ── the OTHER port: the Vision's camera module (a different chip, its
  // own engine — ROM bootloader + XMODEM instead of esptool) ──
  if (state.catalog.we2_module) {
    const mod = el("button", "flash-module-card");
    mod.append(el("span", "flash-module-icon", "📷"),
      el("strong", null, "Building a Canary Vision? Load the camera module’s brain here"),
      el("span", "muted",
        "The Grove Vision AI V2’s person-detection model, burned from this page over the " +
        "MODULE’s USB-C port — pinned, SHA-256-verified, with a live bench check after. " +
        "No vendor site, no account, no choices to get wrong."));
    mod.addEventListener("click", () => setPhase(phaseModule({
      catalog: state.catalog,
      setPhase,
      back: () => setPhase(phaseConnect()),
    })));
    box.append(mod);
  }
  return box;
}

async function onConnect() {
  if (state.busy || state.connecting) return;
  state.connecting = true;   // synchronous guard: a double-click can't open two choosers
  let port;
  try {
    port = await navigator.serial.requestPort();
  } catch (e) {
    // User dismissed the chooser — not an error, just nudge.
    state.connecting = false;
    return;
  }
  state.connecting = false;
  state.busy = true;
  const box = el("section", "flash-card flash-working");
  box.append(el("div", "flash-spinner", ""));
  const status = el("h2", null, "Waking up your Canary…");
  const detail = el("p", "muted", "Reaching the board’s bootloader. This takes a few seconds.");
  box.append(status, detail);
  // A gentle recovery nudge if it's slow (native-USB boards sometimes need
  // the download-mode gesture the flashing lesson teaches).
  const nudge = el("div", "flash-hidden");
  nudge.append(el("p", "fineprint", "Taking a while? Put it in download mode yourself, then reconnect:"));
  nudge.append(downloadModeSteps());
  box.append(nudge);
  setPhase(box);
  const nudgeTimer = setTimeout(() => nudge.classList.remove("flash-hidden"), 6000);

  const transport = new Transport(port, false);
  const esploader = new ESPLoader({
    transport,
    baudrate: state.catalog.flash_baud || 921600,
    romBaudrate: state.catalog.console_baud || 115200,
    terminal: makeTerminal(),
  });

  try {
    state.chipDesc = await esploader.main();
    state.chip = esploader.chip.CHIP_NAME;
    // A stalled read should fail fast and retry (we read in small chunks),
    // not sit out esptool-js's default 100 s silence window per packet.
    esploader.FLASH_READ_TIMEOUT = 15000;
    try { state.mac = await esploader.chip.readMac(esploader); } catch { state.mac = null; }
    try {
      const kb = await esploader.getFlashSize();
      state.flashBytes = kb ? kb * 1024 : null;
    } catch { state.flashBytes = null; }
    state.session = { port, transport, esploader };
    clearTimeout(nudgeTimer);
    state.busy = false;
    await readCurrentFirmware();     // best-effort; never throws out
    ensureManifest();                // kick off (async) manifest load
    setPhase(phaseConnected());
  } catch (e) {
    clearTimeout(nudgeTimer);
    state.busy = false;
    try { await transport.disconnect(); } catch {}
    setPhase(connectFailed(e));
  }
}

function connectFailed(e) {
  const v = core.classifyFlashError(e);
  const box = errorBox(
    v.title || "Couldn’t reach the board",
    v.kind === "not-in-download" || v.kind === "unknown"
      ? "That almost always means it isn’t in download mode yet — no harm done."
      : v.hint,
    false);
  // For a genuine "not in download mode" show the gesture; for a specific cause
  // (port busy, permissions, cable) the hint above already says what to do.
  if (v.kind === "not-in-download" || v.kind === "unknown") {
    box.append(downloadModeSteps());
    box.append(el("p", "muted", "Then click “Try again”."));
  }
  const retry = el("button", "primary", "Try again");
  retry.addEventListener("click", () => setPhase(phaseConnect()));
  box.append(retry);
  const raw = el("details", "flash-rawerr");
  raw.append(el("summary", null, "Technical details"));
  raw.append(el("pre", null, String(e && e.message ? e.message : e)));
  box.append(raw);
  return box;
}

// esptool writes its own log lines here; we surface them in a collapsible log.
let logSink = null;
function makeTerminal() {
  return {
    clean() { if (logSink) logSink.textContent = ""; },
    writeLine(data) { if (logSink) { logSink.textContent += data + "\n"; logSink.scrollTop = logSink.scrollHeight; } },
    write(data) { if (logSink) { logSink.textContent += data; logSink.scrollTop = logSink.scrollHeight; } },
  };
}

// ── chunked flash reads (the stall fix) ─────────────────────────────────────
// One big readFlash call streams the whole span against a ~4 MB un-acked
// window; a single lost byte stalls it (esptool-js #218 — exactly the
// "backup stuck at 1.8 MB" failure). Instead: many small reads, each its own
// command, each retried on its own if the stream hiccups.
async function readFlashChunked(esploader, offset, size, onProgress) {
  const out = new Uint8Array(size);
  const plan = core.planReadChunks(offset, size);
  let done = 0;
  for (const c of plan) {
    let attempt = 0;
    for (;;) {
      try {
        const part = await esploader.readFlash(c.offset, c.size, (_pkt, prog) => {
          if (onProgress) onProgress(Math.min(done + prog, size), size);
        });
        if (part.length < c.size) throw new Error(`short read at 0x${c.offset.toString(16)}`);
        out.set(part.subarray(0, c.size), c.offset - offset);
        break;
      } catch (e) {
        if (++attempt >= 3) throw e;
        // Drop any half-delivered packets before asking again.
        try { await esploader.transport.flushInput(); } catch {}
        await sleep(300 * attempt);
      }
    }
    done += c.size;
    if (onProgress) onProgress(done, size);
  }
  return out;
}

// ── read what firmware is already on the board (best-effort) ────────────────
async function readCurrentFirmware() {
  state.current = null;
  const { esploader } = state.session;
  try {
    const ptBytes = await readFlashChunked(esploader, 0x8000, 0xc00);
    const { apps } = core.parsePartitionTable(ptBytes);
    const app = core.pickAppPartition(apps);
    if (!app) { state.current = { unknown: true }; return; }
    const desc = await readFlashChunked(esploader, app.offset + core.APP_DESC_OFFSET, 256);
    const d = core.parseAppDescriptor(desc);
    if (!d) { state.current = { unknown: true }; return; }
    const product = core.matchProjectToProduct(state.catalog, d.projectName);
    state.current = {
      version: d.version,
      projectName: d.projectName,
      productName: product ? product.name : null,
      date: d.date,
    };
  } catch (e) {
    state.current = { unknown: true };
  }
}

// ── phase: connected — chip card + firmware picker ──────────────────────────
function phaseConnected() {
  const wrap = el("div", "flash-connected");

  // The chip hello card.
  const info = core.chipInfo(state.catalog, state.chip) || {};
  const hello = el("section", "flash-card flash-hello");
  const head = el("div", "flash-hello-head");
  head.append(el("div", "flash-chip-emoji", "🐤"));
  const ht = el("div");
  ht.append(el("h2", null, `Say hello to your ${info.label || state.chip}`));
  // Current-firmware readout.
  const cur = el("p", "flash-current");
  if (state.current && state.current.unknown) {
    cur.append(el("span", "flash-current-dot flash-dot-new", "●"));
    cur.append(document.createTextNode("This board looks brand new — no SecuraCV firmware read off it yet."));
  } else if (state.current) {
    cur.append(el("span", "flash-current-dot flash-dot-ok", "●"));
    const name = state.current.productName || state.current.projectName || "firmware";
    const ver = state.current.version ? ` ${state.current.version}` : "";
    cur.append(document.createTextNode(`Looks like it’s running ${name}${ver} right now.`));
  }
  ht.append(cur);
  ht.append(modeBadge("download"));
  head.append(ht);
  hello.append(head);

  const facts = el("div", "flash-facts");
  facts.append(fact("Chip", state.chipDesc || state.chip));
  if (state.mac) facts.append(fact("ID (MAC)", core.formatMac(state.mac)));
  if (state.flashBytes) facts.append(fact("Flash", core.formatBytes(state.flashBytes)));
  hello.append(facts);

  const tools = el("div", "flash-row flash-tools");
  const health = el("button", "ghost", "🩺 Health check");
  health.title = "Read the board’s story — firmware slots, crash dumps, witness chain. Nothing is changed.";
  health.addEventListener("click", runHealthCheck);
  const mon = el("button", "ghost", "🖥️ Serial monitor");
  mon.title = "Reboot into the firmware and talk to its built-in console.";
  mon.addEventListener("click", () => openMonitor());
  const rescue = el("button", "ghost", "🚑 Rescue");
  rescue.title = "Board acting wrong? Wipe it and write the newest signed firmware — back to known-good.";
  rescue.addEventListener("click", () => setPhase(phaseRescue()));
  tools.append(health, mon, rescue);
  hello.append(tools);
  const toolsNote = el("p", "fineprint",
    "Health check reads the board’s story without changing a byte. The serial " +
    "monitor is the board’s live voice over USB — watch it and send commands. " +
    "Rescue brings a misbehaving board back to a known-good state.");
  hello.append(toolsNote);
  wrap.append(hello);

  // Firmware picker (chip-guarded).
  wrap.append(renderPicker());

  const disconnect = el("button", "ghost small flash-disconnect", "disconnect");
  disconnect.addEventListener("click", onDisconnect);
  wrap.append(disconnect);
  return wrap;
}

function fact(label, val) {
  const f = el("div", "flash-fact");
  f.append(el("span", "flash-fact-label", label));
  f.append(el("span", "flash-fact-val", val));
  return f;
}

// ── the safety copy (runs automatically before every flash) ─────────────────
function haveBackupForThisBoard() {
  return !!(state.backup && state.backup.mac === state.mac);
}

// Standalone backup (from the health report). The auto-backup inside
// startFlash reuses takeBackup with the flash progress card.
async function takeBackup(box) {
  const { esploader } = state.session;
  const total = state.flashBytes;
  const eta = core.makeEtaTracker(total);
  const bytes = await readFlashChunked(esploader, 0, total, (prog, tot) => {
    const p = eta.feed(prog, performance.now());
    box.set(p.frac, progressMeta(prog, tot, p));
  });
  const name = `canary-${macStamp()}-backup.bin`;
  downloadBytes(bytes, name);
  state.backup = { bytes, name, mac: state.mac };
  return name;
}

async function onBackup() {
  if (state.busy || !state.flashBytes) return;
  state.busy = true;
  const box = progressCard("Backing up your Canary", "Reading every byte off the board. Nothing is changed.");
  setPhase(box.card);
  try {
    await takeBackup(box);
    state.busy = false;
    setPhase(phaseConnected());
  } catch (e) {
    state.busy = false;
    setPhase(errorRetry("Backup didn’t finish", e, phaseConnected));
  }
}

function renderPicker() {
  const card = el("section", "flash-card flash-picker");
  card.append(el("h3", null, "Choose the firmware to install"));

  const matches = core.productsForChip(state.catalog, state.chip);
  const info = core.chipInfo(state.catalog, state.chip) || {};
  card.append(el("p", "muted",
    `Installing firmware over USB is what “flashing” means — same thing, two ` +
    `words. Only firmware built for your ${info.label || state.chip} is shown ` +
    `— the flasher won’t offer an image meant for a different board.`));

  const manifestState = el("div", "flash-manifest-state");
  manifestState.id = "flash-manifest-state";
  card.append(manifestState);

  // Arrived from the checkup page with a board already in mind (?product=…)?
  // Lead with just that one — the chip guard still limits the set, so this only
  // ever narrows within what's valid, never widens it.
  const preferredId = core.preferredProductId(location.search);
  const focus = preferredId ? matches.find((p) => p.id === preferredId) : null;

  const list = el("div", "flash-products");
  const rows = matches.map((p) => { const r = productRow(p); list.append(r); return r; });
  if (!matches.length) {
    list.append(el("p", "muted", "No published SecuraCV product targets this chip yet."));
  }
  if (focus && matches.length > 1) {
    // Hide the others, but keep their rows in the DOM so refreshManifestState
    // still fills every version; one click reveals them.
    rows.forEach((r) => { if (r.dataset.id !== focus.id) r.style.display = "none"; });
    const note = el("p", "fineprint flash-focus-note");
    note.append(document.createTextNode(`Showing ${focus.name} — the firmware you picked. `));
    const more = el("button", "ghost small", `show the other ${matches.length - 1} for this chip`);
    more.addEventListener("click", () => { rows.forEach((r) => { r.style.display = ""; }); note.remove(); });
    note.append(more);
    card.append(note);
  }
  card.append(list);
  refreshManifestState(); // fills versions/availability once manifest lands

  // The best-practice promise: a safety copy happens by itself.
  if (state.flashBytes) {
    const bk = el("p", "flash-autobackup fineprint");
    bk.append(el("span", "flash-check", "✓"));
    bk.append(document.createTextNode(
      " Before anything is written, a full copy of the board is saved to your " +
      "downloads automatically — your undo button, no clicks needed. (Keep " +
      "backup files private: they contain the board's identity key.)"));
    card.append(bk);
  }

  // Advanced: local file, erase toggle, skip-backup, restore.
  const adv = el("details", "flash-advanced");
  adv.append(el("summary", null, "Advanced"));
  const local = el("div", "flash-local");
  local.append(el("p", "muted",
    "Install a firmware file from your computer (a .bin you built, or one for " +
    "an air-gapped setup). We can’t check a personal file’s signature, but " +
    "the board still can’t be bricked, and we verify the write against the chip."));
  const fileBtn = el("input");
  fileBtn.type = "file";
  fileBtn.accept = ".bin";
  fileBtn.className = "flash-file";
  fileBtn.addEventListener("change", onLocalFile);
  local.append(fileBtn);
  adv.append(local);

  const eraseWrap = el("label", "flash-erase");
  const erase = el("input");
  erase.type = "checkbox";
  erase.id = "flash-erase-all";
  eraseWrap.append(erase);
  eraseWrap.append(el("span", null,
    " Erase the entire chip first — an extra-clean start that also clears any " +
    "leftover data from a previous firmware. Use it if a board is misbehaving."));
  adv.append(eraseWrap);

  if (state.flashBytes) {
    const skipWrap = el("label", "flash-erase");
    const skip = el("input");
    skip.type = "checkbox";
    skip.id = "flash-skip-backup";
    skipWrap.append(skip);
    skipWrap.append(el("span", null,
      " Skip the automatic safety copy this time. Only worth it if you’re " +
      "reflashing a board you already backed up — the copy is what lets you " +
      "put everything back exactly as it was."));
    adv.append(skipWrap);
  }

  if (haveBackupForThisBoard()) {
    const restoreRow = el("div", "flash-local");
    restoreRow.append(el("p", "muted",
      `This session holds a backup of this exact board (${state.backup.name}).`));
    const restore = el("button", "ghost small", "restore that backup");
    restore.addEventListener("click", () =>
      startFlash({ localBytes: state.backup.bytes, label: "your backup", isBackup: true }));
    restoreRow.append(restore);
    adv.append(restoreRow);
  }

  const restoreFile = el("div", "flash-local");
  restoreFile.append(el("p", "muted",
    "Restore a backup file saved earlier (canary-…-backup.bin) — rewinds the " +
    "board to that exact moment, works with backups from any version."));
  const rf = el("input");
  rf.type = "file";
  rf.accept = ".bin";
  rf.className = "flash-file";
  rf.addEventListener("change", onRestoreFile);
  restoreFile.append(rf);
  adv.append(restoreFile);
  card.append(adv);
  return card;
}

function productRow(p) {
  const row = el("div", "flash-product");
  row.dataset.id = p.id;
  const left = el("div", "flash-product-main");
  left.append(el("div", "flash-product-name", p.name));
  left.append(el("div", "flash-product-tag muted", p.tagline));
  const ver = el("div", "flash-product-ver");
  ver.dataset.for = p.id;
  left.append(ver);
  row.append(left);
  const btn = el("button", "primary small flash-pick", "Install this");
  btn.dataset.for = p.id;
  btn.disabled = true; // enabled when manifest confirms an image exists
  btn.addEventListener("click", () => onPick(p));
  row.append(btn);
  return row;
}

// ── release manifest (lazy) ─────────────────────────────────────────────────
function activeManifestUrl() {
  // `?manifest=<url>` lets a self-hosted / air-gapped user point at their own
  // manifest — but only if it's same-origin or a private/LAN host (see
  // manifestOverrideUrl). `?channel=dev` switches to the rolling dev
  // prerelease manifest (a fixed first-party URL, never user-supplied).
  // Otherwise: the signed stable release.
  const override = core.manifestOverrideUrl(location.search, location.origin);
  state.manifestOverride = !!override;
  state.devChannel = !override && core.channelFromSearch(location.search) === "dev";
  if (override) return override;
  return state.devChannel ? core.DEV_FLASH_MANIFEST_URL : state.catalog.manifest_url;
}

function ensureManifest() {
  if (state.manifest) { refreshManifestState(); return; }
  fetch(activeManifestUrl(), { cache: "no-store" })
    .then((r) => (r.ok ? r.json() : Promise.reject(new Error("no release manifest (HTTP " + r.status + ")"))))
    .then((m) => {
      const errs = core.validateManifest(m);
      state.manifest = errs.length ? { __invalid: errs } : m;
    })
    .catch(() => { state.manifest = { __missing: true }; })
    .finally(refreshManifestState);
}

function refreshManifestState() {
  const banner = $("#flash-manifest-state");
  if (!banner) return;
  banner.innerHTML = "";
  const m = state.manifest;

  if (!m) {
    banner.append(el("p", "fineprint", "Checking for the latest signed release…"));
    return;
  }
  if (m.__missing || m.__invalid) {
    const note = el("p", "flash-note flash-note-soft");
    note.textContent = m.__invalid
      ? "The published release manifest didn’t validate, so official images are hidden. You can still install a local file under Advanced."
      : "No signed firmware release is published yet. When the maintainer cuts one, the official images appear here automatically. Until then, use Advanced → install a local file.";
    banner.append(note);
    return;
  }
  // Manifest present. Note when it's a self-hosted override, not the release.
  if (state.manifestOverride) {
    const note = el("p", "flash-note flash-note-soft");
    note.textContent = "Using a self-hosted firmware manifest from this page’s address bar, not the official signed release.";
    banner.append(note);
  }
  if (state.devChannel) {
    const note = el("p", "flash-note flash-note-soft");
    note.textContent = "DEV CHANNEL — these images come from the rolling dev prerelease, signed with the same key but not yet promoted to stable. Remove ?channel=dev from the address bar to go back to release firmware.";
    banner.append(note);
  }
  // Fill versions + enable buttons for available products.
  document.querySelectorAll(".flash-product").forEach((row) => {
    const id = row.dataset.id;
    const product = state.catalog.products.find((p) => p.id === id);
    const entry = core.manifestEntry(m, product, state.chip);
    const verEl = row.querySelector('[data-for="' + id + '"].flash-product-ver, .flash-product-ver');
    const btn = row.querySelector(".flash-pick");
    if (entry && !entry.error) {
      if (verEl) verEl.textContent = `v${entry.version} · ${core.formatBytes(entry.size)}`;
      if (btn) btn.disabled = false;
    } else {
      if (verEl) verEl.textContent = "not in this release";
      if (btn) { btn.disabled = true; btn.textContent = "unavailable"; }
    }
  });
}

// ── pick → confirm → flash ──────────────────────────────────────────────────
function onPick(product) {
  const entry = core.manifestEntry(state.manifest, product, state.chip);
  if (!entry || entry.error) {
    setPhase(errorRetry("That image isn’t available", new Error(entry && entry.error || "not in release"), phaseConnected));
    return;
  }
  setPhase(phaseConfirm(product, entry));
}

async function onLocalFile(ev) {
  const file = ev.target.files && ev.target.files[0];
  if (!file) return;
  const skip = $("#flash-skip-backup") && $("#flash-skip-backup").checked;
  const buf = await file.arrayBuffer();
  const bytes = new Uint8Array(buf);
  startFlash({ localBytes: bytes, label: file.name, isLocal: true, skipBackup: skip });
}

function phaseConfirm(product, entry) {
  // Read the Advanced toggles while the picker is still in the DOM.
  const eraseOn = $("#flash-erase-all") && $("#flash-erase-all").checked;
  const skipBackup = $("#flash-skip-backup") && $("#flash-skip-backup").checked;

  const box = el("section", "flash-card flash-confirm");
  box.append(el("h2", null, `Install ${product.name}?`));
  box.append(el("p", "muted", state.current && state.current.unknown
    ? "This is the one-time first setup — after it, the board is a Canary."
    : "This is the same “flash” process as first setup — the board just gets the new firmware."));
  const sum = el("div", "flash-summary");
  sum.append(fact("Firmware", `${product.name} · v${entry.version}`));
  sum.append(fact("For chip", entry.chipFamily));
  sum.append(fact("Size", core.formatBytes(entry.size)));
  sum.append(fact("Verified by", "SHA-256 before · chip MD5 after"));
  box.append(sum);

  const willBackup = state.flashBytes && !skipBackup && !haveBackupForThisBoard();
  const isSensor = product && product.provisioning === "usb-secrets";
  const settingsLine = isSensor
    ? " It comes up with the network baked into this firmware."
    : " It’ll bring up its own setup WiFi afterwards, ready to configure — that’s expected.";
  const promise = el("p", "flash-reassure");
  promise.append(el("span", "flash-shield", "🛟"));
  promise.append(document.createTextNode(
    (willBackup
      ? "First, a full safety copy of the board is saved to your downloads — your undo button. Then "
      : "") +
    (eraseOn
      ? (willBackup ? "the board is wiped completely and written fresh." : "Full erase selected — the board is wiped completely, then written fresh.")
      : (willBackup ? "a clean install is written." : "This writes a clean install.")) +
    settingsLine +
    " Safe to interrupt at any point: unplug mid-flash and nothing breaks, you just run it again."));
  box.append(promise);

  // WiFi (optional): fill it in and it's baked into the chip during the
  // install; leave it empty and nothing changes. Either way the setup
  // network is the safety net.
  let wifiUI = null;
  if (product && product.provisioning === "ap") {
    wifiUI = renderWifiFields(box);
  }

  const row = el("div", "flash-row");
  const go = el("button", "primary flash-go", `Install it${eraseOn ? " (with full erase)" : ""}`);
  go.addEventListener("click", () => {
    let wifi = null;
    if (wifiUI) {
      const r = wifiUI.credentials();
      if (!r.ok) return; // invalid input — the field showed why
      wifi = r.wifi;
      wifiUI.clear();    // never leave the password sitting in the DOM
    }
    startFlash({ entry, product, eraseAll: !!eraseOn, skipBackup: !!skipBackup, wifi });
  });
  const cancel = el("button", "ghost", "not yet");
  cancel.addEventListener("click", () => setPhase(phaseConnected()));
  row.append(go, cancel);
  box.append(row);
  return box;
}

// ── optional WiFi fields (confirm card) ─────────────────────────────────────
function renderWifiFields(box) {
  const sec = el("div", "flash-wifi");
  sec.append(el("h3", null, "WiFi (optional)"));

  const ssid = el("input"), pass = el("input");
  ssid.type = "text"; ssid.placeholder = "network name (SSID)"; ssid.autocomplete = "off";
  pass.type = "password"; pass.placeholder = "password";
  pass.autocomplete = "new-password";
  const showBtn = el("button", "ghost small", "show");
  showBtn.addEventListener("click", () => {
    pass.type = pass.type === "password" ? "text" : "password";
    showBtn.textContent = pass.type === "password" ? "show" : "hide";
  });
  const rowIn = el("div", "flash-wifi-inputs");
  rowIn.append(ssid, pass, showBtn);
  sec.append(rowIn);

  const err = el("p", "flash-note flash-note-soft flash-hidden");
  sec.append(err);
  sec.append(el("p", "fineprint",
    "Fill this in and it’s written into the chip during the install, so the " +
    "Canary joins your WiFi on its very first boot. If it can’t connect — " +
    "or you leave this empty — it simply broadcasts its own setup network " +
    "to connect to and finish setup there. What you type stays on this " +
    "page and goes only to the chip over the cable."));

  // Bonus for camera Canaries: the same fields can mint a standard WiFi QR
  // (generated right here, nothing sent anywhere) to show the lens later.
  const qrRow = el("div", "flash-row");
  const qrBtn = el("button", "ghost small", "…or make a WiFi QR code to show a camera Canary");
  const qrOut = el("div", "flash-wifi-qr");
  qrBtn.addEventListener("click", async () => {
    err.classList.add("flash-hidden");
    if (!ssid.value) {
      err.textContent = "Type the network name (and password) first, then make the QR.";
      err.classList.remove("flash-hidden");
      return;
    }
    try { core.buildNvsWifiImage(ssid.value, pass.value, 4096); }
    catch (e) {
      err.textContent = String(e.message || e);
      err.classList.remove("flash-hidden");
      return;
    }
    const { default: qrcode } = await import("./vendor/qrcode/qrcode.mjs");
    const qr = qrcode(0, "M");
    qr.addData(core.wifiQrString(ssid.value, pass.value));
    qr.make();
    qrOut.innerHTML = qr.createSvgTag({ cellSize: 5, margin: 3 });
    qrOut.append(el("p", "fineprint",
      "Generated on this page — nothing was sent anywhere. Print it or show " +
      "it on a phone; after install, hold it in front of the Canary’s camera."));
  });
  qrRow.append(qrBtn);
  sec.append(qrRow, qrOut);

  box.append(sec);
  return {
    credentials() {
      err.classList.add("flash-hidden");
      if (!ssid.value) return { ok: true, wifi: null }; // optional — skipped
      try {
        // The builder validates lengths; run it small just for the checks.
        core.buildNvsWifiImage(ssid.value, pass.value, 4096);
        return { ok: true, wifi: { ssid: ssid.value, pass: pass.value } };
      } catch (e) {
        err.textContent = String(e.message || e);
        err.classList.remove("flash-hidden");
        return { ok: false, wifi: null };
      }
    },
    clear() { pass.value = ""; },
  };
}

// ── the flash itself ────────────────────────────────────────────────────────
async function startFlash(opts) {
  if (state.busy) return;
  state.busy = true;
  const { esploader } = state.session;
  const eraseAll = !!opts.eraseAll;
  const label = opts.product ? `${opts.product.name} v${opts.entry.version}` : opts.label;

  const box = progressCard(`Installing ${label}`, "Getting the image ready…");
  setPhase(box.card);

  // The layers tour rides along for the whole install — backup included —
  // and its hex slide starts showing the real image bytes the moment the
  // download lands (imageBytesRef is filled in below).
  const imageBytesRef = { bytes: opts.localBytes || null };
  const logEl = box.card.querySelector(".flash-log");
  box.card.insertBefore(installStory(() => imageBytesRef.bytes || (state.backup && state.backup.bytes)), logEl);

  // Announce the whole journey up front — "step 2 of 4" is what makes the
  // bar predictable instead of a mystery that keeps restarting.
  const willBackup = state.flashBytes && !opts.isBackup && !opts.skipBackup && !haveBackupForThisBoard();
  const stepLabels = [];
  if (willBackup) stepLabels.push("safety copy");
  if (!opts.localBytes) stepLabels.push("download", "authenticity check");
  if (eraseAll) stepLabels.push("full erase");
  stepLabels.push("write + verify");
  let stepNo = 0;
  const nextStep = (text) => box.stage(`Step ${++stepNo} of ${stepLabels.length} — ${text}`);

  try {
    // 0) The automatic safety copy — best practice, no clicks. Skipped only
    //    when restoring a backup, when Advanced says so, or when this exact
    //    board was already copied this session. During a rescue a failing
    //    backup (common on a corrupted board) must not block the recovery.
    let backupName = null;
    let backupFailed = false;
    if (willBackup) {
      nextStep("saving a safety copy of the board first (nothing is changed)");
      try {
        backupName = await takeBackup(box);
      } catch (e) {
        if (!opts.rescue) throw e;
        backupFailed = true;
      }
      box.set(0, "");
    }

    // 1) Obtain the image bytes.
    let bytes, shaHex = null, shaSigned = false;
    if (opts.localBytes) {
      bytes = opts.localBytes;
      // Fingerprint the local file too, so the receipts can name exactly
      // what was written even when we can't vouch for its origin.
      try {
        shaHex = core.hex(new Uint8Array(await crypto.subtle.digest("SHA-256", bytes.slice().buffer)));
      } catch {}
    } else {
      nextStep("downloading the signed image");
      const buf = await fetch(opts.entry.factory, { cache: "no-store" }).then((r) => {
        if (!r.ok) throw new Error("download failed (HTTP " + r.status + ")");
        return r.arrayBuffer();
      });
      bytes = new Uint8Array(buf);
      // 2) Verify SHA-256 against the manifest BEFORE writing a byte.
      nextStep("checking the image is authentic (SHA-256)");
      const digest = await crypto.subtle.digest("SHA-256", buf);
      const got = core.hex(new Uint8Array(digest));
      if (got.toLowerCase() !== opts.entry.sha256) {
        throw new Error("Downloaded image failed its checksum — refusing to flash it. " +
          "Nothing was written. (Try again; if it persists the release may be mid-update.)");
      }
      shaHex = got.toLowerCase();
      shaSigned = true;
    }
    imageBytesRef.bytes = bytes;
    state.lastImage = bytes; // lets the done card replay the tour with real hex

    // 2.5) The change map: we hold the board's current bytes (safety copy)
    // and the image's bytes, so we can say exactly which regions this
    // install touches — before a single byte is written.
    let diff = null, settings = null;
    const oldBytes = state.backup && state.backup.mac === state.mac ? state.backup.bytes : null;
    if (oldBytes && !opts.isBackup) {
      try {
        diff = core.diffInstall(oldBytes, bytes);
        settings = core.settingsVerdict(diff, hadSavedWifi(oldBytes));
      } catch { diff = null; }
    }

    // 2.7) WiFi pre-provisioning: build a minimal valid NVS image carrying
    // the typed credentials and write it into the image's own settings
    // region, in the same pass as the firmware. If we can't locate that
    // region, the install continues and the setup network takes over —
    // never block a flash on a convenience.
    let wifiFile = null, wifiSsid = null;
    if (opts.wifi && !opts.isBackup) {
      try {
        const { entries } = core.parsePartitionTable(
          bytes.subarray(0x8000, Math.min(0x8c00, bytes.length)));
        const nvs = entries.find(core.isNvsPart);
        if (!nvs) throw new Error("no settings region in this image");
        const nvsImg = core.buildNvsWifiImage(opts.wifi.ssid, opts.wifi.pass, nvs.size);
        wifiFile = { data: core.bytesToBinaryString(nvsImg), address: nvs.offset };
        wifiSsid = opts.wifi.ssid;
      } catch (e) {
        box.stage("Couldn’t bake the WiFi (" + String(e.message || e) +
          ") — continuing; use the setup network instead");
        await sleep(1200);
      }
    }

    // 3) Write, with live progress + automatic chip MD5 verification —
    // the map lights up region by region as the write cursor passes.
    if (eraseAll) {
      nextStep("erasing the whole chip");
      await esploader.eraseFlash();
    }
    nextStep(wifiFile ? "writing firmware + your WiFi settings" : "writing firmware");
    const liveMap = diff ? box.attachMap(diff.rows, bytes.length, state.flashBytes || bytes.length) : null;
    const data = core.bytesToBinaryString(bytes);
    const eta = core.makeEtaTracker(bytes.length);
    await esploader.writeFlash({
      fileArray: wifiFile ? [{ data, address: 0 }, wifiFile] : [{ data, address: 0 }],
      flashSize: "keep",
      flashMode: "keep",
      flashFreq: "keep",
      eraseAll: false, // regions being written are erased as needed
      compress: true,
      reportProgress: (_i, written, total) => {
        const p = eta.feed(written, performance.now());
        box.set(p.frac, progressMeta(written, total, p));
        if (liveMap) liveMap.update(p.frac);
      },
      calculateMD5Hash: (image) => md5Raw(image),
    });

    box.stage("Verified — the chip holds exactly what we sent ✓");
    box.set(1, "");
    if (liveMap) liveMap.update(1);

    // 4) Reset into the freshly-flashed app.
    try { await esploader.after("hard_reset"); } catch {}

    state.busy = false;
    setPhase(phaseDone({ ...opts, backupName, backupFailed, diff, settings,
      shaHex, shaSigned, bytesWritten: bytes.length, wifiSsid, wifi: null }));
  } catch (e) {
    state.busy = false;
    setPhase(flashError(e, opts));
  }
}

function flashError(e, opts) {
  const msg = String(e && e.message ? e.message : e);
  const v = core.classifyFlashError(e);
  const box = errorBox(v.title || "The install didn’t complete",
    "Your board is fine — remember, it can’t be bricked from here. " +
    (v.kind === "unknown" ? "Here’s what to try:" : v.hint), false);
  // For a recognized cause the hint above is the fix; for the generic case,
  // fall back to the classic three-step checklist.
  if (v.kind === "unknown") {
    const steps = el("ul", "flash-steps");
    [
      "Unplug the board, plug it back in, and click Try again.",
      "If it won’t connect: hold BOOT, tap RESET, release BOOT, then reconnect.",
      "Use a USB-C data cable (not charge-only).",
    ].forEach((s) => steps.append(el("li", null, s)));
    box.append(steps);
  }
  const row = el("div", "flash-row");
  const retry = el("button", "primary", "Try again");
  retry.addEventListener("click", async () => {
    // Re-establish the session cleanly, then return to the picker.
    await onDisconnect(true);
    setPhase(phaseConnect());
  });
  row.append(retry);
  box.append(row);
  const raw = el("details", "flash-rawerr");
  raw.append(el("summary", null, "Technical details"));
  raw.append(el("pre", null, msg));
  box.append(raw);
  return box;
}

// ── phase: done — celebration + watch it boot ───────────────────────────────
function phaseDone(opts) {
  const box = el("section", "flash-card flash-done");
  confettiBurst();
  box.append(el("div", "flash-done-bird", "🎉"));
  box.append(el("h2", null, opts.isBackup ? "Restored — your Canary is back to that copy"
    : "Installed — your Canary is awake"));

  const product = opts.product;
  if (opts.wifiSsid) {
    const w = el("p", "muted");
    w.append(el("span", "flash-check", "✓"));
    w.append(document.createTextNode(
      ` Your WiFi is baked in — the Canary should join “${opts.wifiSsid}” on its very first boot. ` +
      `No setup network needed (it still appears if the join fails, as the fallback).`));
    box.append(w);
  } else if (product) {
    const note = state.catalog.products.find((p) => p.id === product.id);
    const p = el("p", "muted", note ? note.provisioning_note : "");
    box.append(p);
  } else {
    box.append(el("p", "muted", "It rebooted into the firmware you just wrote. If it doesn’t light up, tap the RESET button once."));
  }

  if (opts.backupName) {
    const bk = el("p", "fineprint");
    bk.append(el("span", "flash-check", "✓"));
    bk.append(document.createTextNode(
      ` Safety copy saved to your downloads as ${opts.backupName} — restore it any time from Advanced. ` +
      `Keep the file private: it holds everything that was on the board, including its identity key ` +
      `and any saved WiFi. Treat it like a spare house key.`));
    box.append(bk);
  } else if (opts.backupFailed) {
    box.append(el("p", "fineprint",
      "The safety copy couldn’t be read off this board first — common when a board " +
      "is corrupted, and exactly why the rescue carried on without it."));
  }

  // What actually changed — the byte-verified answer, region by region.
  if (opts.diff) {
    const sec = el("div", "flash-report-sec");
    sec.append(el("h3", null, "What this install changed"));
    if (opts.wifiSsid) {
      sec.append(el("p", "flash-note flash-note-kept",
        "The settings region was written fresh — with your WiFi already inside it."));
    } else if (opts.settings) {
      sec.append(el("p", opts.settings.kept ? "flash-note flash-note-kept" : "flash-note flash-note-soft",
        opts.settings.text));
    }
    if (opts.diff.layoutChanged) {
      sec.append(el("p", "fineprint", "The storage layout itself changed with this install — the map below is the new one."));
    }
    opts.diff.rows.forEach((r) => {
      let text, tone = null;
      if (r.verdict === "identical") { text = "unchanged — byte-for-byte the same"; tone = "ok"; }
      else if (r.verdict === "untouched") { text = "not touched by this install"; tone = "ok"; }
      else if (r.verdict === "wiped") { text = "reset to factory-fresh"; }
      else if (r.before || r.after) { text = `updated: ${r.before || "empty"} → ${r.after || "?"}`; }
      else { text = `updated (${r.changedPct || 1}% of its bytes changed)`; }
      sec.append(reportRow(`${r.label} · ${plainRegionName(r)}`, text, tone));
    });
    box.append(sec);
  } else if (opts.backupName || opts.backupFailed) {
    box.append(el("p", "fineprint",
      "No change map this time — it needs the safety copy to compare against."));
  }

  // The receipts: for the skeptic who (rightly) wants proof, the exact
  // numbers behind "verified" — nothing here is a vibe, it's all checkable.
  if (opts.shaHex || opts.diff) {
    const rec = el("details", "flash-receipts");
    rec.append(el("summary", null, "show the receipts — every byte accounted for"));
    const list = el("div", "flash-report-sec flash-receipts-body");
    if (opts.shaHex) {
      list.append(reportRow("SHA-256 of the image",
        el("code", null, opts.shaHex.slice(0, 32) + "…"), "ok"));
      list.append(el("p", "fineprint", opts.shaSigned
        ? "Computed in your browser from the downloaded bytes and matched against the fingerprint published in the signed release — before anything was written. You can recompute it yourself: download the same release asset and run sha256sum."
        : "Computed in your browser from your local file, so you can pin down exactly what was written. We can't vouch for a personal file's origin — that part is on you."));
    }
    if (opts.bytesWritten) {
      list.append(reportRow("Written and read back",
        `${opts.bytesWritten.toLocaleString()} bytes — the chip itself recomputed a checksum (MD5) over the written range and it matched`, "ok"));
    }
    if (opts.diff && state.flashBytes) {
      const named = opts.diff.rows.reduce((a, r) => a + r.size, 0);
      list.append(reportRow("Every byte accounted for",
        `${state.flashBytes.toLocaleString()} bytes of flash = ${opts.diff.rows.length} named regions ` +
        `(${named.toLocaleString()} bytes) + ${(state.flashBytes - named).toLocaleString()} bytes of unused space — nothing unmapped`, "ok"));
    }
    list.append(el("p", "fineprint",
      "And the deeper checks live one click away: the health check re-reads the partition map, " +
      "firmware fingerprints and witness-chain state straight off the chip, any time."));
    rec.append(list);
    box.append(rec);
  }

  const row = el("div", "flash-row");
  const watch = el("button", "primary", "Watch it boot →");
  watch.addEventListener("click", () => openMonitor({ celebrate: true, skipReset: true }));
  const again = el("button", "ghost", "Set up another board");
  again.addEventListener("click", () => onDisconnect().then(() => setPhase(phaseConnect())));
  const tour = el("button", "ghost", "replay the layers tour");
  let tourEl = null;
  tour.addEventListener("click", () => {
    if (tourEl) { tourEl.remove(); tourEl = null; return; }
    tourEl = installStory(() => state.lastImage);
    box.append(tourEl);
  });
  row.append(watch, again, tour);
  box.append(row);
  return box;
}

// ── rescue: back to known-good, for any firmware past or future ─────────────
// The rescue path leans only on things that never change: the mask-ROM
// bootloader, the chip family we detect from the silicon, and the signed
// release manifest (regenerated for every future firmware release). So a
// board messed up by ANY firmware — including ones that don't exist yet —
// recovers the same way: best-effort safety copy, full erase, newest signed
// image for the chip in hand.
function phaseRescue() {
  const box = el("section", "flash-card flash-rescue");
  box.append(el("h2", null, "Rescue this board"));
  box.append(el("p", "muted",
    "For a Canary that’s acting wrong and you just want it back to known-good. " +
    "Three steps: a safety copy is attempted first (a corrupted board may not " +
    "give one — the rescue continues anyway), the whole chip is wiped, and the " +
    "newest signed firmware for your exact chip is written and verified. This " +
    "works the same for every future firmware release — the flasher always " +
    "fetches the latest signed image for the silicon in hand."));

  const matches = core.productsForChip(state.catalog, state.chip);
  const preferred = core.pickRescueProduct(
    state.catalog, state.chip, state.current && state.current.projectName);

  let chosen = preferred || matches[0] || null;
  if (matches.length > 1) {
    const list = el("div", "flash-products");
    matches.forEach((p) => {
      const lab = el("label", "flash-rescue-choice");
      const r = el("input");
      r.type = "radio"; r.name = "rescue-product";
      r.checked = chosen && p.id === chosen.id;
      r.addEventListener("change", () => { chosen = p; });
      lab.append(r);
      const t = el("div");
      t.append(el("div", "flash-product-name", p.name));
      t.append(el("div", "flash-product-tag muted", p.tagline));
      lab.append(t);
      list.append(lab);
    });
    box.append(list);
    if (preferred) box.append(el("p", "fineprint",
      `${preferred.name} is pre-selected because that’s what the board says it was running.`));
  } else if (chosen) {
    box.append(el("p", "flash-current", `Firmware: ${chosen.name} — ${chosen.tagline}`));
  }

  const entry = chosen && core.manifestEntry(state.manifest, chosen, state.chip);
  const row = el("div", "flash-row");
  if (chosen && entry && !entry.error) {
    const go = el("button", "primary", `Rescue with ${chosen.name} v${entry.version}`);
    go.addEventListener("click", () => {
      const e2 = core.manifestEntry(state.manifest, chosen, state.chip);
      if (!e2 || e2.error) return;
      startFlash({ entry: e2, product: chosen, eraseAll: true, rescue: true });
    });
    row.append(go);
  } else {
    box.append(el("p", "flash-note flash-note-soft",
      state.manifest && (state.manifest.__missing || state.manifest.__invalid)
        ? "No signed release is reachable right now, so the one-click rescue is unavailable — but restoring a backup file below always works."
        : "Still checking for the latest signed release… come back to this card in a moment."));
  }
  const cancel = el("button", "ghost", "not yet");
  cancel.addEventListener("click", () => setPhase(phaseConnected()));
  row.append(cancel);
  box.append(row);

  // Restoring a saved backup file: raw bytes, so it can never go stale.
  const restore = el("div", "flash-local flash-rescue-restore");
  restore.append(el("h3", null, "…or put back a backup you saved earlier"));
  restore.append(el("p", "muted",
    "Every install here saves a full copy of the board to your downloads " +
    "(canary-…-backup.bin). Restoring one rewinds the board to that exact " +
    "moment — firmware, settings, witness chain, everything. Backups are raw " +
    "flash bytes, so a file from any past or future version restores the same " +
    "way. That completeness cuts both ways: a backup holds the board's " +
    "identity key and saved WiFi, so store it like a house key."));
  const file = el("input");
  file.type = "file";
  file.accept = ".bin";
  file.className = "flash-file";
  file.addEventListener("change", onRestoreFile);
  restore.append(file);
  box.append(restore);
  return box;
}

async function onRestoreFile(ev) {
  const f = ev.target.files && ev.target.files[0];
  if (!f) return;
  const check = core.validateBackupFile(f.size, state.flashBytes, f.name);
  if (!check.ok) {
    setPhase(errorRetry("That file can’t be restored here", new Error(check.reason), phaseConnected));
    return;
  }
  const buf = await f.arrayBuffer();
  startFlash({ localBytes: new Uint8Array(buf), label: f.name, isBackup: true, warn: check.warn });
}

// ── the health check (triage without changing a byte) ───────────────────────
async function runHealthCheck() {
  if (state.busy || !state.session) return;
  state.busy = true;
  const { esploader } = state.session;
  const box = progressCard("Reading your board’s story", "Partition map, firmware slots, crash dumps, witness chain — read-only, nothing is changed.");
  setPhase(box.card);

  const report = {
    generatedAt: new Date().toISOString(),
    chip: state.chipDesc || state.chip,
    mac: state.mac ? core.formatMac(state.mac) : null,
    flashBytes: state.flashBytes,
  };

  try {
    // 1) Partition table — everything else hangs off it.
    box.stage("Reading the partition map");
    box.set(0.1, "");
    const ptBytes = await readFlashChunked(esploader, 0x8000, 0xc00);
    const { entries, apps } = core.parsePartitionTable(ptBytes);
    report.partitions = entries.map((e) => ({
      label: e.label, kind: core.partitionKind(e),
      offset: e.offset, size: e.size,
    }));

    if (!entries.length) {
      report.blank = true;
    } else {
      // 2) Every app slot's descriptor — what's installed, when it was built.
      box.stage("Reading the firmware in each slot");
      box.set(0.3, "");
      report.slots = [];
      for (const app of apps) {
        let desc = null;
        try {
          const d = await readFlashChunked(esploader, app.offset + core.APP_DESC_OFFSET, 256);
          desc = core.parseAppDescriptor(d);
        } catch {}
        report.slots.push({
          label: app.label || core.partitionKind(app),
          subtype: app.subtype,
          empty: !desc,
          project: desc ? desc.projectName : null,
          version: desc ? desc.version : null,
          built: desc && desc.date ? `${desc.date} ${desc.time || ""}`.trim() : null,
          idf: desc ? desc.idfVer : null,
        });
      }

      // 3) otadata — which slot boots, how many updates this board has seen.
      const otaPart = entries.find(core.isOtaDataPart);
      const otaSlots = apps.filter((a) => a.subtype >= 0x10 && a.subtype < 0x20);
      if (otaPart) {
        box.stage("Reading update history (otadata)");
        box.set(0.5, "");
        try {
          const ob = await readFlashChunked(esploader, otaPart.offset, Math.min(otaPart.size, 0x2000));
          report.ota = core.parseOtaData(ob, otaSlots.length);
          if (report.ota && !report.ota.fresh && otaSlots[report.ota.activeOta]) {
            const activeLabel = otaSlots[report.ota.activeOta].label;
            const slot = report.slots.find((s) => s.label === activeLabel);
            if (slot) slot.active = true;
          } else if (report.ota && report.ota.fresh) {
            // Factory default boot: factory partition if present, else ota_0.
            const first = apps.find((a) => a.subtype === 0x00) || otaSlots[0];
            const slot = first && report.slots.find((s) => s.label === first.label);
            if (slot) slot.active = true;
          }
        } catch {}
      }

      // 4) Coredump — has this board ever crashed hard?
      const cd = entries.find(core.isCoredumpPart);
      if (cd) {
        box.stage("Checking for stored crash dumps");
        box.set(0.65, "");
        try {
          const cb = await readFlashChunked(esploader, cd.offset, 16);
          report.coredump = core.parseCoredumpHeader(cb, cd.size);
        } catch {}
      }

      // 5) NVS — the witness chain's fast-boot cache lives here. Values are
      //    only pulled for the chain head; secrets stay as presence-only.
      const nvs = entries.find(core.isNvsPart);
      if (nvs) {
        box.stage("Reading witness-chain state (NVS)");
        box.set(0.8, "");
        try {
          const nb = await readFlashChunked(esploader, nvs.offset, nvs.size);
          const items = core.parseNvs(nb, [core.WITNESS_CHAIN_BLOB_KEY]);
          report.witness = core.witnessSummary(items);
        } catch {}
      }

      const wl = entries.find(core.isWitnessLogPart);
      if (wl) report.witnessLog = { label: wl.label, size: wl.size };
    }

    box.set(1, "");
    state.report = report;
    state.busy = false;
    setPhase(renderReport(report));
  } catch (e) {
    state.busy = false;
    setPhase(errorRetry("The health check didn’t finish", e, phaseConnected));
  }
}

function reportRow(label, value, tone) {
  const row = el("div", "flash-report-row" + (tone ? ` flash-report-${tone}` : ""));
  row.append(el("span", "flash-report-label", label));
  const v = el("span", "flash-report-val");
  if (value instanceof Node) v.append(value); else v.textContent = value;
  row.append(v);
  return row;
}

function renderReport(r) {
  const box = el("section", "flash-card flash-report");
  box.append(el("h2", null, "Board report"));
  box.append(el("p", "muted", "Read straight off the flash — nothing was changed, and nothing left this page."));

  const facts = el("div", "flash-facts");
  facts.append(fact("Chip", r.chip));
  if (r.mac) facts.append(fact("ID (MAC)", r.mac));
  if (r.flashBytes) facts.append(fact("Flash", core.formatBytes(r.flashBytes)));
  box.append(facts);

  if (r.blank) {
    box.append(el("p", "flash-note flash-note-soft",
      "No partition table found — this board looks blank (or was fully erased). Installing any firmware below sets it up from scratch."));
  }

  // Firmware slots.
  if (r.slots && r.slots.length) {
    const sec = el("div", "flash-report-sec");
    sec.append(el("h3", null, "Firmware on the board"));
    r.slots.forEach((s) => {
      const badge = s.active ? " — running now" : "";
      const text = s.empty
        ? "empty"
        : `${s.project || "unknown"} ${s.version || ""}`.trim() +
          (s.built ? ` · built ${s.built}` : "");
      sec.append(reportRow(s.label + badge, text, s.active ? "ok" : null));
    });
    const note = el("p", "fineprint",
      "“Built” is the firmware’s own compile stamp — the closest thing a board keeps to “when it was last flashed”.");
    sec.append(note);
    box.append(sec);
  }

  // Update history.
  if (r.ota) {
    const sec = el("div", "flash-report-sec");
    sec.append(el("h3", null, "Update history"));
    if (r.ota.fresh) {
      sec.append(reportRow("Over-the-air updates", "none yet — still on its first-flashed firmware"));
    } else {
      sec.append(reportRow("Over-the-air updates", `${r.ota.updatesSeen} slot switch${r.ota.updatesSeen === 1 ? "" : "es"} recorded`));
      sec.append(reportRow("Boot state", r.ota.stateText, r.ota.pendingVerify ? "warn" : null));
      if (r.ota.pendingVerify) {
        sec.append(el("p", "fineprint",
          "“Pending verify” means the last update hasn’t confirmed itself yet — if the board reboots before it does, it rolls back. Usually it just needs one good boot."));
      }
    }
    box.append(sec);
  }

  // Health.
  const health = el("div", "flash-report-sec");
  health.append(el("h3", null, "Health"));
  if (r.coredump) {
    if (r.coredump.present) {
      health.append(reportRow("Crash dump", `found (${core.formatBytes(r.coredump.size)}) — the board hard-crashed at some point and kept the evidence`, "warn"));
      health.append(el("p", "fineprint",
        "Not an emergency: the dump is from an earlier fault, and flashing fresh firmware (or a full erase) clears it. If a board keeps crashing, this is the breadcrumb to mention in a bug report."));
    } else {
      health.append(reportRow("Crash dump", "none stored — no hard crashes on record", "ok"));
    }
  }
  if (r.witness && r.witness.boots != null) {
    health.append(reportRow("Lifetime boots", String(r.witness.boots)));
  }
  if (r.witness && r.witness.tamper != null && r.witness.tamper !== 0) {
    health.append(reportRow("Tamper flag", `set (${r.witness.tamper}) — the firmware recorded a tamper event`, "warn"));
  }
  if (!r.coredump && (!r.witness || r.witness.boots == null)) {
    health.append(el("p", "muted", "Nothing health-related readable on this layout."));
  }
  box.append(health);

  // Witness chain.
  if (r.witness) {
    const sec = el("div", "flash-report-sec");
    sec.append(el("h3", null, "Witness chain"));
    if (r.witness.seq != null) {
      sec.append(reportRow("Records chained", String(r.witness.seq), "ok"));
    }
    if (r.witness.chainHeadFp) {
      sec.append(reportRow("Chain head", el("code", null, r.witness.chainHeadFp + "…")));
    }
    sec.append(reportRow("Device identity", r.witness.provisioned
      ? "provisioned — this board has its signing key" : "not provisioned yet",
      r.witness.provisioned ? "ok" : null));
    sec.append(reportRow("WiFi settings", r.witness.wifiConfigured ? "stored on the board" : "none stored"));
    sec.append(el("p", "fineprint",
      "Counters and fingerprints only — keys, passwords and tokens are never read out of the board, here or anywhere."));
    box.append(sec);
  } else if (!r.blank) {
    const sec = el("div", "flash-report-sec");
    sec.append(el("h3", null, "Witness chain"));
    sec.append(el("p", "muted", "No SecuraCV witness state found — normal for a board that hasn’t run Canary firmware yet."));
    box.append(sec);
  }
  if (r.witnessLog) {
    box.append(el("p", "fineprint",
      `This board also carries a dedicated ${core.formatBytes(r.witnessLog.size)} tamper-evident witness_log partition.`));
  }

  // The flash map: the chip's whole address space, drawn to scale from the
  // partition table we just read off the board. Click any region to see its
  // actual first bytes — real hex from the real chip, reading only.
  if (r.partitions && r.partitions.length && r.flashBytes) {
    const det = el("details", "flash-report-map");
    det.append(el("summary", null, "flash map — what lives where on the chip"));
    det.append(el("p", "fineprint",
      "Every byte on the chip has an address; the board’s own partition table " +
      "says what each region is for. Click a region to peek at its actual " +
      "bytes — looking never changes anything."));

    const bar = el("div", "flash-map");
    const peek = el("div", "flash-hexpeek flash-hidden");

    const openPeek = async (label, kind, offset, size) => {
      peek.classList.remove("flash-hidden");
      peek.innerHTML = "";
      peek.append(el("p", "flash-hexpeek-title",
        `${label || kind} · 0x${offset.toString(16)} · ${core.formatBytes(size)}`));
      const body = el("div", "flash-hexdump", "reading 256 bytes…");
      peek.append(body);
      try {
        const bytes = await readFlashChunked(state.session.esploader, offset, 256);
        body.textContent = "";
        core.hexDumpLines(bytes, offset).forEach((l) => {
          const line = el("div", "flash-hexline");
          line.append(el("span", "flash-hexaddr", l.addr));
          line.append(el("span", "flash-hexbytes", l.hex));
          line.append(el("span", "flash-hexascii", l.ascii));
          body.append(line);
        });
        peek.append(el("p", "fineprint", "What this looks like: " + core.sniffRegion(bytes) + "."));
      } catch (e) {
        body.textContent = "couldn’t read that region right now — reconnect and try again";
      }
    };

    const kindClass = (kind) => {
      if (kind.startsWith("app")) return "app";
      if (kind.includes("nvs")) return "nvs";
      if (kind.includes("otadata")) return "ota";
      if (kind.includes("coredump")) return "core";
      if (/fat|spiffs|littlefs/.test(kind)) return "fs";
      return "data";
    };

    let covered = 0;
    const addSeg = (label, kind, offset, size, cls) => {
      const seg = el("button", "flash-map-seg flash-map-" + cls);
      seg.style.width = Math.max(1.2, (size / r.flashBytes) * 100) + "%";
      seg.title = `${label || kind} · 0x${offset.toString(16)} · ${core.formatBytes(size)}`;
      seg.addEventListener("click", () => openPeek(label, kind, offset, size));
      bar.append(seg);
    };
    r.partitions.forEach((p) => {
      addSeg(p.label, p.kind, p.offset, p.size, /witness/i.test(p.label || "") ? "witness" : kindClass(p.kind));
      covered = Math.max(covered, p.offset + p.size);
    });
    if (covered < r.flashBytes) {
      addSeg("unused space", "free", covered, r.flashBytes - covered, "free");
    }
    det.append(bar);

    const legend = el("div", "flash-map-legend");
    r.partitions.forEach((p) => {
      const item = el("button", "flash-map-key");
      item.append(el("span", "flash-map-dot flash-map-" +
        (/witness/i.test(p.label || "") ? "witness" : kindClass(p.kind))));
      item.append(document.createTextNode(`${p.label || p.kind} · ${core.formatBytes(p.size)}`));
      item.addEventListener("click", () => openPeek(p.label, p.kind, p.offset, p.size));
      legend.append(item);
    });
    if (covered < r.flashBytes) {
      const item = el("span", "flash-map-key");
      item.append(el("span", "flash-map-dot flash-map-free"));
      item.append(document.createTextNode(`unused · ${core.formatBytes(r.flashBytes - covered)}`));
      legend.append(item);
    }
    det.append(legend);
    det.append(peek);
    box.append(det);
  } else if (r.partitions && r.partitions.length) {
    // No flash size known: fall back to the plain table.
    const det = el("details", "flash-report-map");
    det.append(el("summary", null, "storage map (partitions)"));
    const tbl = el("div", "flash-report-table");
    r.partitions.forEach((p) => {
      const row = el("div", "flash-report-trow");
      row.append(el("code", null, p.label || "—"));
      row.append(el("span", "muted", p.kind));
      row.append(el("code", null, "0x" + p.offset.toString(16)));
      row.append(el("span", null, core.formatBytes(p.size)));
      tbl.append(row);
    });
    det.append(tbl);
    box.append(det);
  }

  // If anything above looked off, point straight at the way out.
  const worrying = r.blank || (r.coredump && r.coredump.present) ||
    (r.ota && r.ota.pendingVerify) ||
    (r.witness && r.witness.tamper != null && r.witness.tamper !== 0);
  if (worrying && !r.blank) {
    const fix = el("p", "flash-note flash-note-soft");
    fix.append(document.createTextNode(
      "Something above looks off? The Rescue button wipes the board and writes the newest signed firmware — back to known-good in one go. "));
    const b = el("button", "ghost small", "🚑 open Rescue");
    b.addEventListener("click", () => setPhase(phaseRescue()));
    fix.append(b);
    box.append(fix);
  }

  const row = el("div", "flash-row");
  const back = el("button", "primary", "Back to the flasher");
  back.addEventListener("click", () => setPhase(phaseConnected()));
  const save = el("button", "ghost", "Save report (.json)");
  save.addEventListener("click", () => {
    const blob = new TextEncoder().encode(JSON.stringify(r, null, 2));
    downloadBytes(blob, `canary-${macStamp()}-report.json`);
  });
  const bk = el("button", "ghost", "💾 Save a full backup");
  bk.addEventListener("click", onBackup);
  row.append(back, save, bk);
  box.append(row);
  return box;
}

// ── the serial monitor (talk to the firmware's own console) ─────────────────
// The firmware answers single-key commands on its USB console ('h' prints the
// full menu). We reboot the board out of download mode, reopen the port at
// console baud, and give the user a live console with a send box.
const MONITOR_CMDS = [
  ["h", "help"], ["i", "identity"], ["s", "status"], ["t", "time"],
  ["w", "wifi"], ["m", "system"], ["b", "battery"],
];

async function openMonitor(opts = {}) {
  if (state.busy) return;
  let port = state.session && state.session.port;
  if (state.session) {
    // Leave the bootloader: reset into the app, then let go of the port.
    if (!opts.skipReset) { try { await state.session.esploader.after("hard_reset"); } catch {} }
    try { await state.session.transport.disconnect(); } catch {}
    state.session = null;
  }
  setPhase(phaseMonitor(port, opts));
}

function phaseMonitor(port, opts = {}) {
  const box = el("section", "flash-card flash-monitor");
  box.append(el("h2", null, "Serial monitor"));
  box.append(modeBadge("running"));
  box.append(el("p", "muted",
    "This is the board’s own voice: a live text feed the firmware prints over " +
    "the USB cable, plus single-key commands you can send back. It’s how you " +
    "check on a Canary without touching anything — and nothing you type here " +
    "can break the board."));
  const status = el("p", "muted flash-mon-status", "Opening the console…");
  box.append(status);

  const con = el("pre", "flash-console flash-console-tall");
  box.append(con);

  const chips = el("div", "flash-mon-chips");
  MONITOR_CMDS.forEach(([ch, label]) => {
    const b = el("button", "ghost small", `${label} (${ch})`);
    b.addEventListener("click", () => send(ch));
    chips.append(b);
  });
  box.append(chips);

  const inRow = el("div", "flash-mon-input");
  const input = el("input");
  input.type = "text";
  input.placeholder = "type a command — the firmware answers single keys; h shows its menu";
  input.addEventListener("keydown", (ev) => {
    if (ev.key === "Enter") { send(input.value + "\n"); input.value = ""; }
  });
  const sendBtn = el("button", "primary small", "Send");
  sendBtn.addEventListener("click", () => { send(input.value + "\n"); input.value = ""; });
  inRow.append(input, sendBtn);
  box.append(inRow);

  // Baud rate: pre-picked from the firmware catalog, changeable for the
  // curious, self-correcting when the output doesn't decode as text.
  const defaultBaud = state.catalog.console_baud || 115200;
  const baudRow = el("label", "flash-mon-baud");
  baudRow.append(el("span", "flash-fact-label", "baud"));
  const baudSel = el("select");
  const bauds = core.CONSOLE_BAUDS.includes(defaultBaud)
    ? core.CONSOLE_BAUDS : [defaultBaud, ...core.CONSOLE_BAUDS];
  bauds.forEach((b) => {
    const o = el("option", null, b === defaultBaud ? `${b} (picked for your Canary)` : String(b));
    o.value = String(b);
    baudSel.append(o);
  });
  baudSel.value = String(defaultBaud);
  baudRow.append(baudSel);
  box.append(baudRow);

  const row = el("div", "flash-row");
  const done = el("button", "ghost", "← back to the flasher");
  row.append(done);
  box.append(row);
  box.append(el("p", "fineprint",
    "The right speed is already chosen for you (on native-USB Canaries the " +
    "number barely matters). Nothing here leaves your computer. To flash " +
    "again, go back — the flasher returns the board to download mode itself " +
    "(or: hold BOOT, tap RESET, release BOOT)."));

  // — wire it up —
  const mon = { alive: true, port: null, reader: null, writer: null, manualBaud: false, session: 0 };
  let celebrated = !opts.celebrate;

  function send(text) {
    if (!mon.writer || !text) return;
    mon.writer.write(new TextEncoder().encode(text)).catch(() => {});
  }

  async function stopStreams() {
    try { mon.reader && await mon.reader.cancel(); } catch {}
    try { mon.reader && mon.reader.releaseLock(); } catch {}
    try { mon.writer && mon.writer.releaseLock(); } catch {}
    mon.reader = mon.writer = null;
    try { mon.port && await mon.port.close(); } catch {}
  }

  // One open-read session at a given baud. Resolves "garbage" if the first
  // stretch of output doesn't decode as text (wrong baud), "ended" otherwise.
  async function pumpOnce(p, baud) {
    const mySession = ++mon.session;
    await p.open({ baudRate: baud });
    mon.port = p;
    try { mon.writer = p.writable.getWriter(); } catch {}
    mon.reader = p.readable.getReader();
    status.textContent = `Connected at ${baud} — the board is talking. Press a command, or h for its menu.`;
    const dec = new TextDecoder();
    let buf = "", sample = "", judged = false;
    while (mon.alive && mySession === mon.session) {
      const { value, done: fin } = await mon.reader.read();
      if (fin) break;
      const text = dec.decode(value, { stream: true });
      if (!judged) {
        sample = (sample + text).slice(0, 600);
        if (core.looksLikeGarbage(sample)) {
          if (!mon.manualBaud) { await stopStreams(); return "garbage"; }
          judged = true; // user chose this baud on purpose; show it raw
        } else if (sample.length >= 600) judged = true;
      }
      buf = (buf + text).slice(-16000);
      con.textContent = buf;
      con.scrollTop = con.scrollHeight;
      if (!celebrated && /securacv|canary|chirp|witness|boot/i.test(buf)) {
        celebrated = true;
        status.textContent = "✓ It’s talking — your Canary is alive.";
        confettiBurst();
      }
    }
    return "ended";
  }

  // Try the chosen baud; if the text is soup, walk the candidates until one
  // decodes (announcing each hop), so the user never stares at � garbage.
  async function pump(p) {
    let baud = Number(baudSel.value);
    const tried = new Set();
    for (;;) {
      tried.add(baud);
      const verdict = await pumpOnce(p, baud);
      if (verdict !== "garbage" || !mon.alive) return;
      const next = core.CONSOLE_BAUDS.find((b) => !tried.has(b));
      if (!next) {
        status.textContent = "None of the usual speeds decoded as clean text — showing it raw at " + defaultBaud + ".";
        mon.manualBaud = true; // stop re-judging
        baudSel.value = String(defaultBaud);
        await pumpOnce(p, defaultBaud);
        return;
      }
      status.textContent = `That didn’t look like text at ${baud} — trying ${next}…`;
      con.textContent = "";
      baudSel.value = String(next);
      baud = next;
    }
  }

  baudSel.addEventListener("change", async () => {
    mon.manualBaud = true;
    const p = mon.port || port;
    await stopStreams();
    con.textContent = "";
    try { await pumpOnce(p, Number(baudSel.value)); } catch {}
  });

  done.addEventListener("click", async () => {
    mon.alive = false;
    await stopStreams();
    setPhase(phaseConnect());
  });

  (async () => {
    try {
      if (!port) throw new Error("no port");
      await pump(port);
    } catch (e) {
      if (!mon.alive) return;
      // Native-USB boards re-enumerate after reset — offer a fresh pick.
      status.textContent = "The board reconnected as a new USB device (normal for these chips).";
      const b = el("button", "ghost small", "pick it again to keep watching →");
      b.addEventListener("click", async () => {
        b.remove();
        try {
          const np = await navigator.serial.requestPort();
          await pump(np);
        } catch {}
      });
      status.after(b);
    }
  })();

  return box;
}

// Did the board have a saved WiFi before this install? Read from the safety
// copy's NVS region — presence only, never the value.
function hadSavedWifi(oldBytes) {
  try {
    const { entries } = core.parsePartitionTable(oldBytes.subarray(0x8000, 0x8c00));
    const nvs = entries.find(core.isNvsPart);
    if (!nvs) return false;
    const items = core.parseNvs(oldBytes.subarray(nvs.offset, nvs.offset + nvs.size));
    const s = core.witnessSummary(items);
    return !!(s && s.wifiConfigured);
  } catch { return false; }
}

// One consistent meta line under every progress bar: bytes, speed, and an
// honest smoothed time estimate (blank until the rate settles).
function progressMeta(done, total, p) {
  let s = `${core.formatBytes(done)} of ${core.formatBytes(total)}`;
  if (p.kbps > 1) s += ` · ${p.kbps.toFixed(0)} KB/s`;
  const left = core.formatDuration(p.etaSeconds);
  if (left) s += ` · ${left} left`;
  return s;
}

// ── the layers tour ─────────────────────────────────────────────────────────
// While the bytes are being written, walk the user down the whole stack —
// from "what you're holding" to electrons tunneling through glass — so the
// machine they just trusted stops being a black box. Every claim is true of
// this exact board; the hex slide shows the actual bytes being installed.
function installStory(getBytes) {
  const root = el("div", "flash-story");
  const stage = el("div", "flash-story-stage");
  const dots = el("div", "flash-story-dots");
  root.append(el("p", "flash-story-kicker", "while it installs — what this thing actually is"));
  root.append(stage, dots);

  const S = []; // slides: {title, body, visual()}

  S.push({
    title: "What you're holding",
    body: "A watcher that can testify. The Canary senses presence, signs what it " +
      "saw with its own key, and chains every record to the one before — " +
      "evidence, not vibes. Every layer below exists to keep that one promise.",
    visual() {
      const v = el("div", "flash-sv flash-sv-bird");
      v.append(el("span", "flash-sv-bird-emoji", "🐤"));
      const ring = el("span", "flash-sv-bird-ring");
      v.append(ring);
      return v;
    },
  });

  S.push({
    title: "No — it's not a little Linux computer",
    body: "There's no operating system in the laptop sense. No apps, no accounts, " +
      "no background anything. One program — the one being written right now — " +
      "plus a matchbox-sized scheduler juggling the few tasks that program " +
      "creates. Nothing runs here that isn't in this image. The whole machine " +
      "is knowable.",
    visual() {
      const v = el("div", "flash-sv flash-sv-stack");
      [["the firmware — one program, yours", "app"],
       ["a tiny scheduler (FreeRTOS) it brings along", "ota"],
       ["silicon", "data"]].forEach(([t, c], i) => {
        const layer = el("div", "flash-sv-layer flash-map-" + c);
        layer.textContent = t;
        if (i === 0) layer.classList.add("flash-sv-layer-glow");
        v.append(layer);
      });
      return v;
    },
  });

  S.push({
    title: "Its whole life is one loop",
    body: "This is the real shape of the firmware's main loop, simplified but not " +
      "romanticized. Around and around, tens of thousands of times a second, " +
      "for years. No scenes, no sessions — just a heartbeat.",
    visual() {
      const v = el("div", "flash-sv flash-sv-loop");
      const pre = el("pre", "flash-sv-code");
      pre.innerHTML = "";
      ["for (;;) {              // forever",
       "  sense();              // feel the room",
       "  witness();            // sign + chain what happened",
       "  chirp();              // tell your Home Assistant",
       "  listen();             // answer h / i / s on the console",
       "}"].forEach((line) => pre.append(el("div", "flash-sv-codeline", line)));
      v.append(pre);
      const c = el("p", "fineprint flash-sv-counter");
      c.dataset.count = "0";
      c.textContent = "loops since this slide appeared: 0";
      v.append(c);
      return v;
    },
  });

  S.push({
    title: "The bytes ARE the program",
    body: "This hex isn't a file describing the program — it is the program, the " +
      "exact bytes being placed on the chip right now. The two processor cores " +
      "eat them directly, 240 million ticks a second, no interpreter in " +
      "between. What you verify is what runs.",
    visual() {
      const v = el("div", "flash-sv flash-sv-hex");
      const pre = el("pre", "flash-sv-code flash-sv-hexlines");
      pre.textContent = "…";
      v.append(pre);
      return v;
    },
  });

  S.push({
    title: "Where an “if” actually lives",
    body: "Every if in the code compiles down to gates — arrangements of " +
      "microscopic switches doing pure boolean logic. This board carries " +
      "hundreds of millions of them. A thought, in hardware: IF motion AND " +
      "armed → witness.",
    visual() {
      const v = el("div", "flash-sv flash-sv-gate");
      const a = el("span", "flash-sv-in", "motion");
      const b = el("span", "flash-sv-in", "armed");
      const g = el("span", "flash-sv-and", "AND");
      const o = el("span", "flash-sv-out", "witness()");
      v.append(a, b, g, o);
      v.dataset.role = "gate";
      return v;
    },
  });

  S.push({
    title: "Underneath: electrons behind glass",
    body: "Writing a byte means pushing a few thousand electrons through a wall " +
      "of glass thinner than anything you've ever seen — quantum tunneling; " +
      "they cross without quite passing through. Trapped electrons read as 0, " +
      "an empty cell as 1. Unplug it, and they stay put for decades. The chip " +
      "itself is melted sand, grown into a flawless crystal, with metal roads " +
      "narrower than a virus.",
    visual() {
      const v = el("div", "flash-sv flash-sv-cell");
      const box2 = el("div", "flash-sv-cellbox");
      for (let i = 0; i < 5; i++) box2.append(el("span", "flash-sv-electron"));
      v.append(el("div", "flash-sv-cellwall"));
      v.append(box2);
      v.append(el("p", "fineprint", "the glass wall · electrons tunneling in · held for decades"));
      return v;
    },
  });

  S.push({
    title: "And when it speaks: waves",
    body: "A chirp is electrons sloshing up and down an antenna 2.4 billion times " +
      "a second. The ripple crossing your room is the same physics as light — " +
      "a color your eyes can't see. Boolean logic, riding a wave.",
    visual() {
      const v = el("div", "flash-sv flash-sv-wave");
      for (let i = 0; i < 3; i++) {
        const r = el("span", "flash-sv-ring");
        r.style.animationDelay = i * 0.9 + "s";
        v.append(r);
      }
      v.append(el("span", "flash-sv-antenna", "📡"));
      return v;
    },
  });

  S.push({
    title: "All of it, checkable",
    body: "Sand → electrons → gates → bytes → one loop → one promise. Every layer " +
      "verifiable from this page: the receipts count the bytes, the health " +
      "check reads them back, the monitor lets the loop speak for itself. Not " +
      "magic — just physics you can audit.",
    visual() {
      const v = el("div", "flash-sv flash-sv-ladder");
      ["sand", "electrons", "gates", "bytes", "loop", "witness"].forEach((w, i) => {
        const s = el("span", "flash-sv-rung");
        s.textContent = w;
        s.style.animationDelay = i * 0.35 + "s";
        v.append(s);
        if (i < 5) v.append(el("span", "flash-sv-rung-arrow", "→"));
      });
      return v;
    },
  });

  // — deck mechanics: auto-advance, dots, self-cleaning animations —
  let idx = -1, autoTimer = null;
  const show = (i) => {
    idx = (i + S.length) % S.length;
    stage.innerHTML = "";
    const s = S[idx];
    const slide = el("div", "flash-story-slide");
    slide.append(s.visual());
    slide.append(el("h3", null, s.title));
    slide.append(el("p", "muted", s.body));
    stage.append(slide);
    [...dots.children].forEach((d, j) => d.classList.toggle("flash-story-dot-on", j === idx));
  };
  S.forEach((_, i) => {
    const d = el("button", "flash-story-dot");
    d.setAttribute("aria-label", `layer ${i + 1}`);
    d.addEventListener("click", () => { show(i); resetAuto(); });
    dots.append(d);
  });
  const resetAuto = () => {
    if (autoTimer) clearInterval(autoTimer);
    autoTimer = setInterval(() => {
      if (!root.isConnected) { clearInterval(autoTimer); return; }
      show(idx + 1);
    }, 9500);
  };
  show(0);
  resetAuto();

  // One shared ticker animates whatever the current slide needs: the loop
  // counter, the live hex, the gate's boolean truth.
  let hexLine = 0;
  const ticker = setInterval(() => {
    if (!root.isConnected) { clearInterval(ticker); return; }
    const counter = root.querySelector(".flash-sv-counter");
    if (counter) {
      const n = (parseInt(counter.dataset.count, 10) || 0) + 5800 + Math.floor(Math.random() * 900);
      counter.dataset.count = String(n);
      counter.textContent = "loops since this slide appeared: " + n.toLocaleString();
    }
    const hexEl = root.querySelector(".flash-sv-hexlines");
    if (hexEl) {
      const bytes = getBytes && getBytes();
      if (bytes && bytes.length > 0x400) {
        const base = 0x10000 + (hexLine % 24) * 16;
        if (base + 48 < bytes.length) {
          const lines = core.hexDumpLines(bytes.subarray(base, base + 48), base);
          hexEl.textContent = lines.map((l) => `${l.addr}  ${l.hex}`).join("\n");
        }
        hexLine++;
      }
    }
    const gate = root.querySelector('[data-role="gate"]');
    if (gate && Math.random() > 0.4) {
      const ins = gate.querySelectorAll(".flash-sv-in");
      const a = Math.random() > 0.4, b = Math.random() > 0.4;
      ins[0].classList.toggle("flash-sv-hot", a);
      ins[1].classList.toggle("flash-sv-hot", b);
      gate.querySelector(".flash-sv-out").classList.toggle("flash-sv-hot", a && b);
    }
  }, 250);

  return root;
}

// ── shared UI bits ──────────────────────────────────────────────────────────
function progressCard(title, subtitle) {
  const card = el("section", "flash-card flash-progress");
  card.append(el("h2", null, title));
  const stageEl = el("p", "flash-stage muted", subtitle || "");
  card.append(stageEl);
  const bar = el("div", "flash-bar");
  const fill = el("div", "flash-bar-fill");
  bar.append(fill);
  card.append(bar);
  const meta = el("p", "flash-progress-meta fineprint", "");
  card.append(meta);
  const reassure = el("p", "flash-reassure-lite fineprint",
    "Safe to interrupt — you can’t brick it. If anything stops, just start again.");
  card.append(reassure);
  // expose esptool log
  const log = el("details", "flash-log");
  log.append(el("summary", null, "show technical log"));
  const pre = el("pre");
  logSink = pre;
  log.append(pre);
  card.append(log);
  return {
    card,
    stage(s) { stageEl.textContent = s; },
    set(frac, metaText) {
      const pct = Math.max(0, Math.min(1, frac || 0)) * 100;
      fill.style.width = pct.toFixed(1) + "%";
      if (metaText != null) meta.textContent = metaText;
    },
    // The live write map: the chip's regions, lighting up as the write
    // cursor passes through them — Arduino's console line, but visual.
    attachMap(rows, imageLen, flashLen) {
      const wrap = el("div", "flash-livemap");
      const barEl = el("div", "flash-map");
      const segs = [];
      rows.forEach((r) => {
        const seg = el("span", "flash-map-seg flash-map-pending flash-map-" + liveKindClass(r));
        seg.style.width = Math.max(1.2, (r.size / flashLen) * 100) + "%";
        seg.title = `${r.label} · ${core.formatBytes(r.size)}`;
        barEl.append(seg);
        segs.push({ r, seg });
      });
      if (flashLen > imageLen) {
        const rest = el("span", "flash-map-seg flash-map-free");
        rest.style.flex = "1";
        rest.title = "not touched by this install";
        barEl.append(rest);
      }
      const label = el("p", "fineprint flash-livemap-label", "");
      wrap.append(barEl, label);
      bar.before(wrap);
      return {
        update(frac) {
          const addr = Math.min(1, Math.max(0, frac)) * imageLen;
          let activeRow = null;
          segs.forEach(({ r, seg }) => {
            if (r.offset >= imageLen) return; // never written
            if (addr >= r.offset + r.size) {
              seg.classList.remove("flash-map-pending", "flash-map-active");
            } else if (addr > r.offset) {
              seg.classList.add("flash-map-active");
              seg.classList.remove("flash-map-pending");
              activeRow = r;
            }
          });
          if (activeRow) {
            label.textContent = `now writing: ${activeRow.label} — ${plainRegionName(activeRow)}`;
          } else if (frac >= 1) {
            label.textContent = "every region written and verified ✓";
          }
        },
      };
    },
  };
}

// Plain-language names for regions in the live map and the change list.
function plainRegionName(r) {
  if (r.kind && r.kind.includes("bootloader")) return "the board's startup code and map";
  if (r.type === 0x00) return "the firmware itself";
  if (r.type === 0x01 && r.subtype === 0x02) return "your settings (WiFi, identity, witness chain)";
  if (r.type === 0x01 && r.subtype === 0x00) return "the boot selector";
  if (r.type === 0x01 && r.subtype === 0x03) return "crash-dump space";
  if (/witness/i.test(r.label || "")) return "the tamper-evident witness log";
  if (/fat|spiffs|littlefs/.test(r.kind || "")) return "file storage";
  return "on-board data";
}

function liveKindClass(r) {
  if (/witness/i.test(r.label || "")) return "witness";
  if (r.type === 0x00) return "app";
  if (r.type === 0x01 && r.subtype === 0x02) return "nvs";
  if (r.type === 0x01 && r.subtype === 0x00) return "ota";
  if (r.type === 0x01 && r.subtype === 0x03) return "core";
  if (/fat|spiffs|littlefs/.test(r.kind || "")) return "fs";
  return "data";
}

function errorBox(title, subtitle, fatal) {
  const box = el("section", "flash-card flash-error");
  box.append(el("div", "flash-big-emoji", fatal ? "😕" : "🛟"));
  box.append(el("h2", null, title));
  if (subtitle) box.append(el("p", "muted", subtitle));
  return box;
}

function errorRetry(title, e, backPhase) {
  const box = errorBox(title, String(e && e.message ? e.message : e), false);
  const b = el("button", "primary", "Back");
  b.addEventListener("click", () => setPhase(backPhase()));
  box.append(b);
  return box;
}

async function onDisconnect(silent) {
  try { if (state.session) await state.session.transport.disconnect(); } catch {}
  state.session = null;
  state.chip = state.mac = state.flashBytes = state.current = null;
  state.report = null;
  state.busy = false;
  if (!silent) { /* caller decides next phase */ }
}

// ── helpers: downloads, stamps, confetti ────────────────────────────────────
function downloadBytes(bytes, name) {
  const blob = new Blob([bytes], { type: "application/octet-stream" });
  const url = URL.createObjectURL(blob);
  const a = el("a");
  a.href = url;
  a.download = name;
  document.body.append(a);
  a.click();
  a.remove();
  setTimeout(() => URL.revokeObjectURL(url), 4000);
}

function macStamp() {
  const mac = (state.mac || "").replace(/[^0-9a-fA-F]/g, "").slice(-6).toLowerCase();
  return mac || "canary";
}

function confettiBurst() {
  const layer = el("div", "flash-confetti");
  document.body.append(layer);
  const bits = ["🐤", "✨", "🎉", "🟡", "💛"];
  for (let i = 0; i < 26; i++) {
    const c = el("span", "flash-confetti-bit", bits[i % bits.length]);
    c.style.left = ((i / 26) * 100).toFixed(1) + "%";
    c.style.animationDelay = (i % 6 * 60) + "ms";
    c.style.setProperty("--drift", (((i * 37) % 100) - 50) + "px");
    layer.append(c);
  }
  setTimeout(() => layer.remove(), 2600);
}

boot();
