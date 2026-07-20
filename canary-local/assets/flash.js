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
async function boot() {
  const mount = $("#flash");
  mount.innerHTML = "";

  if (!("serial" in navigator)) {
    mount.append(renderUnsupported());
    return;
  }

  try {
    state.catalog = await fetch("devices/flash.json").then((r) => r.json());
  } catch (e) {
    mount.append(errorBox("Couldn’t load the flasher catalog",
      "Reload the page. If it keeps happening, the guided path still works.", true));
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

  const privacy = el("p", "fineprint flash-privacy");
  privacy.textContent =
    "Everything runs in your browser. The flasher engine is served from this " +
    "site, not a CDN; the only network call is fetching the signed firmware " +
    "image you choose. Nothing about your board leaves this page.";
  wrap.append(privacy);
  return wrap;
}

// ── phase: unsupported browser ──────────────────────────────────────────────
function renderUnsupported() {
  const box = el("section", "flash-card flash-unsupported");
  box.append(el("div", "flash-big-emoji", "🧭"));
  box.append(el("h2", null, "This browser can’t talk to USB boards — but you’re one hop away"));
  box.append(el("p", "muted",
    "In-browser flashing uses Web Serial, which today lives in Chromium " +
    "browsers: Chrome, Edge, Brave, Opera, or Arc on a computer (not iPhone " +
    "or iPad — Apple doesn’t allow it). Safari and Firefox can’t do it yet."));
  const row = el("div", "flash-row");
  const guide = el("a", "primary", "Use the guided flash instead →");
  guide.href = LESSON;
  row.append(guide);
  box.append(row);
  box.append(el("p", "fineprint",
    "Same firmware, same safety — just a few more steps in a terminal. When " +
    "you’re next at a Chromium browser, come back and it’s two clicks."));
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
  return box;
}

async function onConnect() {
  if (state.busy) return;
  let port;
  try {
    port = await navigator.serial.requestPort();
  } catch (e) {
    // User dismissed the chooser — not an error, just nudge.
    return;
  }
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
  const box = errorBox(
    "Couldn’t reach the board",
    "That almost always means it isn’t in download mode yet — no harm done.",
    false);
  box.append(downloadModeSteps());
  box.append(el("p", "muted", "Then click “Try again”."));
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

  const list = el("div", "flash-products");
  matches.forEach((p) => list.append(productRow(p)));
  if (!matches.length) {
    list.append(el("p", "muted", "No published SecuraCV product targets this chip yet."));
  }
  card.append(list);
  refreshManifestState(); // fills versions/availability once manifest lands

  // The best-practice promise: a safety copy happens by itself.
  if (state.flashBytes) {
    const bk = el("p", "flash-autobackup fineprint");
    bk.append(el("span", "flash-check", "✓"));
    bk.append(document.createTextNode(
      " Before anything is written, a full copy of the board is saved to your " +
      "downloads automatically — your undo button, no clicks needed."));
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
  // manifestOverrideUrl). Otherwise fall back to the signed release.
  const override = core.manifestOverrideUrl(location.search, location.origin);
  state.manifestOverride = !!override;
  return override || state.catalog.manifest_url;
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

  const row = el("div", "flash-row");
  const go = el("button", "primary flash-go", `Install it${eraseOn ? " (with full erase)" : ""}`);
  go.addEventListener("click", () => startFlash({ entry, product, eraseAll: !!eraseOn, skipBackup: !!skipBackup }));
  const cancel = el("button", "ghost", "not yet");
  cancel.addEventListener("click", () => setPhase(phaseConnected()));
  row.append(go, cancel);
  box.append(row);
  return box;
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
    let bytes;
    if (opts.localBytes) {
      bytes = opts.localBytes;
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
    }

    // 3) Write, with live progress + automatic chip MD5 verification.
    if (eraseAll) {
      nextStep("erasing the whole chip");
      await esploader.eraseFlash();
    }
    nextStep("writing firmware");
    const data = core.bytesToBinaryString(bytes);
    const eta = core.makeEtaTracker(bytes.length);
    await esploader.writeFlash({
      fileArray: [{ data, address: 0 }],
      flashSize: "keep",
      flashMode: "keep",
      flashFreq: "keep",
      eraseAll: false, // regions being written are erased as needed
      compress: true,
      reportProgress: (_i, written, total) => {
        const p = eta.feed(written, performance.now());
        box.set(p.frac, progressMeta(written, total, p));
      },
      calculateMD5Hash: (image) => md5Raw(image),
    });

    box.stage("Verified — the chip holds exactly what we sent ✓");
    box.set(1, "");

    // 4) Reset into the freshly-flashed app.
    try { await esploader.after("hard_reset"); } catch {}

    state.busy = false;
    setPhase(phaseDone({ ...opts, backupName, backupFailed }));
  } catch (e) {
    state.busy = false;
    setPhase(flashError(e, opts));
  }
}

function flashError(e, opts) {
  const msg = String(e && e.message ? e.message : e);
  const box = errorBox("The install didn’t complete",
    "Your board is fine — remember, it can’t be bricked from here. Here’s what to try:", false);
  const steps = el("ul", "flash-steps");
  [
    "Unplug the board, plug it back in, and click Try again.",
    "If it won’t connect: hold BOOT, tap RESET, release BOOT, then reconnect.",
    "Use a USB-C data cable (not charge-only).",
  ].forEach((s) => steps.append(el("li", null, s)));
  box.append(steps);
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
  if (product) {
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
      ` Safety copy saved to your downloads as ${opts.backupName} — restore it any time from Advanced.`));
    box.append(bk);
  } else if (opts.backupFailed) {
    box.append(el("p", "fineprint",
      "The safety copy couldn’t be read off this board first — common when a board " +
      "is corrupted, and exactly why the rescue carried on without it."));
  }

  const row = el("div", "flash-row");
  const watch = el("button", "primary", "Watch it boot →");
  watch.addEventListener("click", () => openMonitor({ celebrate: true, skipReset: true }));
  const again = el("button", "ghost", "Set up another board");
  again.addEventListener("click", () => onDisconnect().then(() => setPhase(phaseConnect())));
  row.append(watch, again);
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
    "flash bytes, so a file from any past or future version restores the same way."));
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

// One consistent meta line under every progress bar: bytes, speed, and an
// honest smoothed time estimate (blank until the rate settles).
function progressMeta(done, total, p) {
  let s = `${core.formatBytes(done)} of ${core.formatBytes(total)}`;
  if (p.kbps > 1) s += ` · ${p.kbps.toFixed(0)} KB/s`;
  const left = core.formatDuration(p.etaSeconds);
  if (left) s += ` · ${left} left`;
  return s;
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
  };
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
