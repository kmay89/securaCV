// canary-local/assets/flash.js — the browser flasher.
//
// Plug a Canary into a Chromium browser over USB-C and it flashes itself:
// detect the chip, read what firmware is already on it, take a one-click
// backup, write a signed release image, verify the bytes against the chip's
// own MD5, and watch it boot — all offline, nothing phones anywhere. The
// whole thing is built around one true promise: you cannot brick the board
// from here (the ESP32's first-stage bootloader is mask ROM), and the UI
// never lets you reach for an image that isn't meant for the silicon in hand.
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
  backup: null,         // { bytes, name } once taken
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

  const layout = el("div", "flash-layout");
  const flow = el("div", "flash-flow");
  flow.id = "flash-flow";
  const rail = renderRail();
  layout.append(flow, rail);
  mount.append(layout);

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

// The flow is a single swappable panel; the rail persists.
function setPhase(node) {
  const flow = $("#flash-flow");
  flow.innerHTML = "";
  flow.append(node);
  flow.scrollIntoView({ behavior: "smooth", block: "nearest" });
}

// ── the persistent "you can't mess up" rail ─────────────────────────────────
function renderRail() {
  const rail = el("aside", "flash-rail");
  const nb = state.catalog.no_brick;

  const promise = el("section", "flash-card flash-promise");
  promise.append(el("div", "flash-promise-badge", "🛟"));
  promise.append(el("h3", null, nb.headline));
  promise.append(el("p", "muted", nb.why));
  const ul = el("ul", "flash-checklist");
  nb.points.forEach((pt) => {
    const li = el("li");
    li.append(el("span", "flash-check", "✓"), document.createTextNode(pt));
    ul.append(li);
  });
  promise.append(ul);
  rail.append(promise);

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
  rail.append(help);

  const privacy = el("p", "fineprint flash-privacy");
  privacy.textContent =
    "Everything runs in your browser. The flasher engine is served from this " +
    "site, not a CDN; the only network call is fetching the signed firmware " +
    "image you choose. Nothing about your board leaves this page.";
  rail.append(privacy);
  return rail;
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
  const nudge = el("p", "fineprint flash-hidden");
  nudge.textContent =
    "Taking a while? Put it in download mode: hold BOOT (B), tap RESET (R), " +
    "release BOOT — then reconnect.";
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
  const steps = el("ol", "flash-steps");
  [
    "Hold the BOOT (B) button down.",
    "While holding BOOT, tap RESET (R) once.",
    "Let go of BOOT.",
    "Click “Try again”.",
  ].forEach((s) => steps.append(el("li", null, s)));
  box.append(steps);
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

// ── read what firmware is already on the board (best-effort) ────────────────
async function readCurrentFirmware() {
  state.current = null;
  const { esploader } = state.session;
  try {
    const ptBytes = await esploader.readFlash(0x8000, 0xc00);
    const { apps } = core.parsePartitionTable(ptBytes);
    const app = core.pickAppPartition(apps);
    if (!app) { state.current = { unknown: true }; return; }
    const desc = await esploader.readFlash(app.offset + core.APP_DESC_OFFSET, 256);
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
  hello.append(el("div", "flash-chip-emoji", "🐤"));
  const h = el("h2", null, `Say hello to your ${info.label || state.chip}`);
  hello.append(h);
  const facts = el("div", "flash-facts");
  facts.append(fact("Chip", state.chipDesc || state.chip));
  if (state.mac) facts.append(fact("ID (MAC)", core.formatMac(state.mac)));
  if (state.flashBytes) facts.append(fact("Flash", core.formatBytes(state.flashBytes)));
  hello.append(facts);

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
  hello.append(cur);
  wrap.append(hello);

  // Backup offer.
  wrap.append(renderBackupCard());

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

function renderBackupCard() {
  const card = el("section", "flash-card flash-backup");
  const head = el("div", "flash-backup-head");
  head.append(el("span", "flash-backup-emoji", "💾"));
  const t = el("div");
  t.append(el("h3", null, "Take a safety copy first"));
  t.append(el("p", "muted",
    "Save everything currently on the board to a file. If you ever want to " +
    "put it back exactly as it was, you can — this is your undo button."));
  head.append(t);
  card.append(head);

  if (state.backup) {
    const done = el("p", "flash-backup-done");
    done.append(el("span", "flash-check", "✓"));
    done.append(document.createTextNode(`Backed up — ${state.backup.name} (${core.formatBytes(state.backup.bytes.length)}) saved to your downloads.`));
    card.append(done);
    const restore = el("button", "ghost small", "restore this backup instead");
    restore.addEventListener("click", () => startFlash({ localBytes: state.backup.bytes, label: "your backup", isBackup: true }));
    card.append(restore);
  } else {
    const btn = el("button", "ghost", "Back up what’s on it now");
    btn.addEventListener("click", onBackup);
    card.append(btn);
    if (!state.flashBytes) {
      card.append(el("p", "fineprint", "(Flash size unknown, so backup is unavailable on this board — flashing is still perfectly safe.)"));
      btn.disabled = true;
    }
  }
  return card;
}

async function onBackup() {
  if (state.busy || !state.flashBytes) return;
  state.busy = true;
  const { esploader } = state.session;
  const box = progressCard("Backing up your Canary", "Reading every byte off the board. Nothing is changed.");
  setPhase(box.card);
  try {
    const total = state.flashBytes;
    const bytes = await esploader.readFlash(0, total, (_pkt, prog, tot) => {
      box.set(prog / tot, `${core.formatBytes(prog)} of ${core.formatBytes(tot)}`);
    });
    const stamp = macStamp();
    const name = `canary-${stamp}-backup.bin`;
    downloadBytes(bytes, name);
    state.backup = { bytes, name };
    state.busy = false;
    setPhase(phaseConnected());
  } catch (e) {
    state.busy = false;
    setPhase(errorRetry("Backup didn’t finish", e, phaseConnected));
  }
}

function renderPicker() {
  const card = el("section", "flash-card flash-picker");
  card.append(el("h3", null, "Choose what to flash"));

  const matches = core.productsForChip(state.catalog, state.chip);
  const info = core.chipInfo(state.catalog, state.chip) || {};
  card.append(el("p", "muted",
    `Only firmware built for your ${info.label || state.chip} is shown — the ` +
    `flasher won’t offer an image meant for a different board.`));

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

  // Advanced: local file + erase toggle.
  const adv = el("details", "flash-advanced");
  adv.append(el("summary", null, "Advanced"));
  const local = el("div", "flash-local");
  local.append(el("p", "muted",
    "Flash a firmware file from your computer (a .bin you built, or one for " +
    "an air-gapped install). We can’t check a personal file’s signature, but " +
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
  const btn = el("button", "primary small flash-pick", "Flash this");
  btn.dataset.for = p.id;
  btn.disabled = true; // enabled when manifest confirms an image exists
  btn.addEventListener("click", () => onPick(p));
  row.append(btn);
  return row;
}

// ── release manifest (lazy) ─────────────────────────────────────────────────
function ensureManifest() {
  if (state.manifest) { refreshManifestState(); return; }
  fetch(state.catalog.manifest_url, { cache: "no-store" })
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
      ? "The published release manifest didn’t validate, so official images are hidden. You can still flash a local file under Advanced."
      : "No signed firmware release is published yet. When the maintainer cuts one, the official images appear here automatically. Until then, use Advanced → flash a local file.";
    banner.append(note);
    return;
  }
  // Manifest present: fill versions + enable buttons for available products.
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
  const buf = await file.arrayBuffer();
  const bytes = new Uint8Array(buf);
  startFlash({ localBytes: bytes, label: file.name, isLocal: true });
}

function phaseConfirm(product, entry) {
  const box = el("section", "flash-card flash-confirm");
  box.append(el("h2", null, `Flash ${product.name}?`));
  const sum = el("div", "flash-summary");
  sum.append(fact("Firmware", `${product.name} · v${entry.version}`));
  sum.append(fact("For chip", entry.chipFamily));
  sum.append(fact("Size", core.formatBytes(entry.size)));
  sum.append(fact("Verified by", "SHA-256 before · chip MD5 after"));
  box.append(sum);

  if (!state.backup) {
    const warn = el("p", "flash-note flash-note-soft");
    warn.append(el("strong", null, "No backup yet. "));
    warn.append(document.createTextNode("You can still go back to any signed release, but a backup captures this exact board. "));
    const b = el("button", "ghost small", "back up first");
    b.addEventListener("click", () => { onBackup(); });
    warn.append(b);
    box.append(warn);
  }

  const eraseOn = $("#flash-erase-all") && $("#flash-erase-all").checked;
  const isSensor = product && product.provisioning === "usb-secrets";
  const settingsLine = isSensor
    ? " It comes up with the network baked into this firmware."
    : " It’ll bring up its own setup WiFi afterwards, ready to configure — that’s expected.";
  const promise = el("p", "flash-reassure");
  promise.append(el("span", "flash-shield", "🛟"));
  promise.append(document.createTextNode(
    (eraseOn
      ? "Full erase selected — the board is wiped completely, then written fresh."
      : "This writes a clean install." ) +
    settingsLine +
    " Safe to interrupt at any point: unplug mid-flash and nothing breaks, you just run it again."));
  box.append(promise);

  const row = el("div", "flash-row");
  const go = el("button", "primary flash-go", `Flash it${eraseOn ? " (with full erase)" : ""}`);
  go.addEventListener("click", () => startFlash({ entry, product, eraseAll: !!eraseOn }));
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

  const box = progressCard(`Flashing ${label}`, "Getting the image ready…");
  setPhase(box.card);

  try {
    // 1) Obtain the image bytes.
    let bytes;
    if (opts.localBytes) {
      bytes = opts.localBytes;
    } else {
      box.stage("Downloading the signed image");
      const buf = await fetch(opts.entry.factory, { cache: "no-store" }).then((r) => {
        if (!r.ok) throw new Error("download failed (HTTP " + r.status + ")");
        return r.arrayBuffer();
      });
      bytes = new Uint8Array(buf);
      // 2) Verify SHA-256 against the manifest BEFORE writing a byte.
      box.stage("Checking the image is authentic (SHA-256)");
      const digest = await crypto.subtle.digest("SHA-256", buf);
      const got = core.hex(new Uint8Array(digest));
      if (got.toLowerCase() !== opts.entry.sha256) {
        throw new Error("Downloaded image failed its checksum — refusing to flash it. " +
          "Nothing was written. (Try again; if it persists the release may be mid-update.)");
      }
    }

    // 3) Write, with live progress + automatic chip MD5 verification.
    if (eraseAll) {
      box.stage("Erasing the whole chip");
      await esploader.eraseFlash();
    }
    box.stage("Writing firmware");
    const data = core.bytesToBinaryString(bytes);
    const started = Date.now();
    await esploader.writeFlash({
      fileArray: [{ data, address: 0 }],
      flashSize: "keep",
      flashMode: "keep",
      flashFreq: "keep",
      eraseAll: false, // regions being written are erased as needed
      compress: true,
      reportProgress: (_i, written, total) => {
        const dt = Date.now() - started;
        box.set(written / total,
          `${core.formatBytes(written)} of ${core.formatBytes(total)} · ${core.throughput(written, dt)}`);
      },
      calculateMD5Hash: (image) => md5Raw(image),
    });

    box.stage("Verified — the chip holds exactly what we sent ✓");
    box.set(1, "");

    // 4) Reset into the freshly-flashed app.
    try { await esploader.after("hard_reset"); } catch {}

    state.busy = false;
    setPhase(phaseDone(opts));
  } catch (e) {
    state.busy = false;
    setPhase(flashError(e, opts));
  }
}

function flashError(e, opts) {
  const msg = String(e && e.message ? e.message : e);
  const box = errorBox("The flash didn’t complete",
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
    : "Flashed — your Canary is awake"));

  const product = opts.product;
  if (product) {
    const note = state.catalog.products.find((p) => p.id === product.id);
    const p = el("p", "muted", note ? note.provisioning_note : "");
    box.append(p);
  } else {
    box.append(el("p", "muted", "It rebooted into the firmware you just wrote. If it doesn’t light up, tap the RESET button once."));
  }

  const row = el("div", "flash-row");
  const watch = el("button", "primary", "Watch it boot →");
  watch.addEventListener("click", () => watchBoot(box));
  const again = el("button", "ghost", "Flash another");
  again.addEventListener("click", () => onDisconnect().then(() => setPhase(phaseConnect())));
  row.append(watch, again);
  box.append(row);

  const consoleWrap = el("div", "flash-console-wrap flash-hidden");
  consoleWrap.id = "flash-console-wrap";
  consoleWrap.append(el("div", "flash-console-head", "serial console · 115200"));
  const con = el("pre", "flash-console");
  con.id = "flash-console";
  consoleWrap.append(con);
  box.append(consoleWrap);
  return box;
}

// Open the port at console baud and stream the boot log. Best-effort: native
// USB boards re-enumerate on reset, so if the held port won't open we ask for
// it again. Any failure degrades to a friendly note — never blocks anything.
async function watchBoot(doneBox) {
  const wrap = $("#flash-console-wrap");
  const con = $("#flash-console");
  wrap.classList.remove("flash-hidden");
  con.textContent = "";

  // Release esptool's hold on the port first.
  let port = state.session && state.session.port;
  try { if (state.session) await state.session.transport.disconnect(); } catch {}
  state.session = null;

  const openConsole = async (p) => {
    await p.open({ baudRate: state.catalog.console_baud || 115200 });
    // Toggle reset lines so we catch the banner from the top (harmless if the
    // board ignores them).
    try { await p.setSignals({ dataTerminalReady: false, requestToSend: true }); await sleep(120);
          await p.setSignals({ dataTerminalReady: true, requestToSend: false }); } catch {}
    const reader = p.readable.getReader();
    const dec = new TextDecoder();
    let buf = "";
    const deadline = Date.now() + 20000;
    let celebrated = false;
    while (Date.now() < deadline) {
      const { value, done } = await reader.read();
      if (done) break;
      buf += dec.decode(value, { stream: true });
      con.textContent = buf.slice(-4000);
      con.scrollTop = con.scrollHeight;
      if (!celebrated && /securacv|canary|chirp|witness|boot/i.test(buf)) {
        celebrated = true;
        const ok = el("p", "flash-console-ok");
        ok.append(el("span", "flash-check", "✓"), document.createTextNode("It’s talking — your Canary is alive."));
        wrap.append(ok);
        confettiBurst();
      }
    }
    try { reader.releaseLock(); } catch {}
    try { await p.close(); } catch {}
  };

  try {
    if (!port) throw new Error("no port");
    await openConsole(port);
  } catch (e) {
    // Re-enumerated or lost — offer a fresh pick.
    con.textContent = "";
    const p = el("p", "fineprint");
    p.append(document.createTextNode("The board reconnected as a new USB device (normal for these chips). "));
    const b = el("button", "ghost small", "pick it again to watch →");
    b.addEventListener("click", async () => {
      try { const np = await navigator.serial.requestPort(); await openConsole(np); }
      catch {}
    });
    p.append(b);
    con.parentElement.append(p);
  }
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
