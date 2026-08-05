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
// the glue and the theater.

import { ESPLoader, Transport } from "./vendor/esptool-js/bundle.js";
import { md5Raw } from "./vendor/md5/md5.js";
import * as core from "./flash-core.js";
import { phaseModule } from "./we2-flash.js";
import { wifiMemory } from "./wifi-memory.js";
import { visionSession } from "./vision-session.js";
import { visionChecklistCard } from "./vision-checklist.js";
import { mintCertificate } from "./hatchery.js";
import { chirp, chirpToggle } from "./chirp.js";
import { mountBoardIdentity } from "./board-identity.js";
import * as intake from "./intake.js";

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

// ── the Nursery roster: the session's hatchlings, so a batch never loses
// its place. sessionStorage only (gone when the tab closes), public facts
// only (product, version, preset names, MAC) — never credentials.
const ROSTER_KEY = "nursery.roster.v1";
function loadRoster() {
  try { return JSON.parse(sessionStorage.getItem(ROSTER_KEY)) || []; } catch { return []; }
}
function saveRoster(list) {
  try { sessionStorage.setItem(ROSTER_KEY, JSON.stringify(list)); } catch { /* private mode */ }
}

const state = {
  catalog: null,
  roster: loadRoster(),   // this session's hatchlings
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
  ble: null,            // { device, characteristic } while a BLE console is open
  // Which manifest the picker is reading. `devChannel` is STICKY session state,
  // not a re-read of the URL: `?channel=dev` only seeds it (boot), and the
  // Advanced toggle owns it from then on — the Lab app has no address bar, so
  // a URL-only dev channel is unreachable there (see onDevChannelToggle).
  devChannel: false,
  manifestOverride: false, // a ?manifest= self-hosted URL is in force
  advancedOpen: false,     // keep Advanced open across a picker re-render
  // Customs, for a board we've never met (see intake.js). `heldBoot` is what
  // the user told us on the connect card, not something we measured — on a
  // native-USB board the ROM and a stock firmware present the same descriptor,
  // so the port genuinely can't tell us.
  heldBoot: false,
  intake: null,            // the read-only intake scan, once connected
  ptRead: null,            // "ok" | "failed" — did the partition sector read at all
  // The user's explicit "this board is already mine, keep its data". The ONLY
  // thing besides our own session roster that waives the first-contact erase;
  // the board's own claim about what firmware it runs never does, because on
  // an untrusted board that claim is attacker-controlled.
  ownerClaimed: false,
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
  mountJourney(flow);
  mount.append(renderReassurance());

  // The footer's opener stays hidden until the catalog (and its about block)
  // actually loaded — a dead settings button would be worse than none.
  const footBtn = $("#foot-settings");
  if (footBtn && state.catalog && state.catalog.about) {
    footBtn.hidden = false;
    footBtn.addEventListener("click", openSettings);
  }

  // `?channel=dev` SEEDS the channel; the Advanced toggle owns it afterwards.
  state.devChannel = core.channelFromSearch(location.search) === "dev";

  renderVersionStrip();
  watchForUnplug();
  setPhase(phaseConnect());
}

// Parity with the desktop Flasher's live watcher (its 1 Hz port poll drops the
// "Connected" bar when the board is really gone): the browser hands us the
// same fact as a `disconnect` event. Without this, unplugging while parked on
// the connected card left "Say hello to your ESP32-S3" up — chip details and
// all — for a board that no longer exists. Scope is deliberately narrow: the
// serial-monitor phase manages its own disconnects (it owns the port then and
// state.session is null), and a mid-operation unplug already surfaces through
// that operation's own error path, so this only covers the idle,
// session-holding phases.
function watchForUnplug() {
  navigator.serial.addEventListener("disconnect", async (ev) => {
    if (!state.session || ev.target !== state.session.port) return;
    if (state.busy || state.connecting) return; // the in-flight step reports its own failure
    await onDisconnect();
    const box = phaseConnect();
    box.prepend(el("p", "muted",
      "Your Canary was unplugged — plug it back in and reconnect to carry on."));
    setPhase(box);
  });
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

// ── the journey bar: where am I on the path? ────────────────────────────────
// Five calm waypoints above the flow. Phases opt in via node.dataset.step;
// a phase without one (sub-cards, tools, the module flow) keeps the bar
// where it was — the bar never guesses.
const JOURNEY = ["Connect", "Meet it", "Choose", "Install", "First flight"];
let journeyEl = null;
function renderJourney(step) {
  if (!journeyEl) return;
  journeyEl.querySelectorAll(".flash-journey-step").forEach((n, i) => {
    n.classList.toggle("flash-journey-on", i + 1 === step);
    n.classList.toggle("flash-journey-done", i + 1 < step);
    n.setAttribute("aria-current", i + 1 === step ? "step" : "false");
  });
}
function mountJourney(before) {
  journeyEl = el("nav", "flash-journey");
  journeyEl.setAttribute("aria-label", "Nursery progress");
  JOURNEY.forEach((label, i) => {
    if (i) journeyEl.append(el("span", "flash-journey-sep", "·"));
    const s = el("span", "flash-journey-step");
    s.append(el("span", "flash-journey-dot"), document.createTextNode(label));
    journeyEl.append(s);
  });
  const side = el("span", "flash-journey-side");
  side.append(chirpToggle());
  const gear = el("button", "flash-settings-open", "⚙︎");
  gear.type = "button";
  gear.title = "About & settings — version, legal, sound, and what this page remembers";
  gear.setAttribute("aria-label", "About and settings");
  gear.addEventListener("click", openSettings);
  side.append(gear);
  const jh = helpDot("journey");
  if (jh) side.append(jh);
  journeyEl.append(side);
  before.parentNode.insertBefore(journeyEl, before);
  renderJourney(1);
}

// ── the settings & about panel: one tidy home for the meta ─────────────────
// Version, provenance, legal, sound, lessons, and the page's local memory —
// everything that isn't the flashing itself, in one native <dialog>. Facts
// come from catalog.about (parsed from LICENSE + vendor provenance at
// generation time), so the credits can't drift from the files of record.
function openSettings() {
  const about = (state.catalog && state.catalog.about) || null;
  if (!about) return;
  const old = $("#nursery-settings");
  if (old) old.remove(); // rebuild fresh each open — toggles reflect NOW
  const dlg = el("dialog", "flash-settings");
  dlg.id = "nursery-settings";
  dlg.setAttribute("aria-label", "About & settings");

  const head = el("div", "flash-settings-head");
  head.append(el("h2", null, "⚙︎ About & settings"));
  const close = el("button", "ghost small", "close ✕");
  close.addEventListener("click", () => dlg.close());
  head.append(close);
  dlg.append(head);

  const sec = (title) => {
    const s = el("section", "flash-settings-sec");
    s.append(el("h3", null, title));
    dlg.append(s);
    return s;
  };

  // ── about ──
  const sAbout = sec("The Nursery");
  sAbout.append(el("p", "muted", about.product + " — where Canaries hatch: " +
    "first flash, updates, triage, and the live benches, all in the browser."));
  sAbout.append(el("p", "fineprint", about.privacy));
  const src = el("p", "fineprint");
  const srcA = el("a", null, "source & issues on GitHub →");
  srcA.href = about.source; srcA.target = "_blank"; srcA.rel = "noopener";
  src.append(srcA);
  sAbout.append(src);

  // ── versions & provenance ──
  const sVer = sec("Versions & provenance");
  const facts = el("div", "flash-facts");
  facts.append(fact("Firmware train", state.catalog.fw_train));
  const engine = (about.vendors || []).find((v) => v.name === "esptool-js");
  if (engine) facts.append(fact("Flashing engine", engine.package + " (vendored, offline)"));
  sVer.append(facts);
  sVer.append(el("p", "fineprint",
    "The device catalog on this page is generated from the firmware tree " +
    "(tools/gen_flash.py) and drift-checked in CI — the facts here can’t be " +
    "typed wrong, only parsed wrong loudly."));

  // ── sound & lessons ──
  const sPref = sec("Sound & lessons");
  const prefRow = el("div", "flash-row");
  prefRow.append(chirpToggle());
  if (coachDismissed()) {
    const coachBtn = el("button", "ghost small", "☕ bring back the lessons");
    coachBtn.addEventListener("click", () => {
      try { sessionStorage.removeItem(COACH_KEY); } catch { /* private mode */ }
      coachBtn.textContent = "☕ lessons will ride the next install ✓";
      coachBtn.disabled = true;
    });
    prefRow.append(coachBtn);
  } else {
    prefRow.append(el("span", "fineprint", "☕ lessons ride every install (hide them with the × on the card)"));
  }
  sPref.append(prefRow);
  sPref.append(el("p", "fineprint",
    "Motion follows your system’s reduce-motion setting automatically — " +
    "nothing to configure here."));

  // ── this page's memory ──
  const sData = sec("What this page remembers");
  const dataRow = el("div", "flash-row");
  const forget = el("button", "ghost small", "forget saved WiFi");
  forget.addEventListener("click", () => {
    wifiMemory.forget();
    forget.textContent = "saved WiFi forgotten ✓";
    forget.disabled = true;
  });
  const clearRoster = el("button", "ghost small",
    state.roster.length ? `clear the session roster (${state.roster.length})` : "session roster is empty");
  clearRoster.disabled = !state.roster.length;
  clearRoster.addEventListener("click", () => {
    state.roster = [];
    saveRoster(state.roster);
    clearRoster.textContent = "roster cleared ✓";
    clearRoster.disabled = true;
  });
  dataRow.append(forget, clearRoster);
  sData.append(dataRow);
  sData.append(el("p", "fineprint",
    "Both live only in this browser: the WiFi copy so a batch provisions " +
    "without retyping, the roster so a batch keeps its place (public facts " +
    "only — never credentials). Neither ever leaves this page."));

  // ── legal ──
  const sLegal = sec("Legal");
  sLegal.append(el("p", "muted", `${about.copyright} · ${about.license.name}`));
  const legalList = el("div", "flash-settings-vendors");
  (about.vendors || []).forEach((v) => {
    const row = el("div", "flash-knob");
    const lab = el("span", "flash-knob-label");
    const a = el("a", null, v.package);
    a.href = v.file; a.target = "_blank"; a.rel = "noopener";
    lab.append(a);
    row.append(lab, el("span", "flash-knob-val", v.license));
    legalList.append(row);
  });
  sLegal.append(legalList);
  sLegal.append(el("p", "fineprint",
    "Vendored into this site on purpose — no CDNs, no third-party requests. " +
    "Each credit links its provenance file; the whole page ships under " +
    `${about.license.name} (LICENSE in the repository).`));

  dlg.addEventListener("click", (ev) => { if (ev.target === dlg) dlg.close(); });
  document.body.append(dlg);
  dlg.showModal();
}

// One check, used everywhere motion is decorative rather than informative.
function prefersCalm() {
  try { return matchMedia("(prefers-reduced-motion: reduce)").matches; } catch { return false; }
}

// The flow is a single swappable panel; the reassurance strip persists below.
function setPhase(node) {
  const flow = $("#flash-flow");
  flow.innerHTML = "";
  flow.append(node);
  if (node.dataset && node.dataset.step) renderJourney(Number(node.dataset.step));
  // Land screen-reader / keyboard focus on the new phase's heading, so a
  // phase change is announced instead of silently swapping content.
  const h = node.querySelector("h2, h3");
  if (h) {
    h.setAttribute("tabindex", "-1");
    try { h.focus({ preventScroll: true }); } catch { /* older engines */ }
  }
  flow.scrollIntoView({ behavior: prefersCalm() ? "auto" : "smooth", block: "nearest" });
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

  // Already have a Canary running? Prove it's on over the air — a Bluetooth
  // check that stands apart from flashing (it writes nothing), reachable any
  // time, not just after a flash.
  const bt = el("details", "flash-card flash-ble-entry");
  bt.append(el("summary", null, "🔵 Already have a Canary? Test it over Bluetooth →"));
  bt.append(el("p", "muted",
    "Skip the cable: connect to a running Canary’s Bluetooth console and read " +
    "its live snapshot — proof it’s powered, healthy, and reachable even when " +
    "WiFi is down. Chromium browser on a computer or Android."));
  const btBtn = el("button", "ghost small", "Open the Bluetooth check");
  btBtn.addEventListener("click", () => setPhase(phaseBluetoothCheck()));
  bt.append(btBtn);
  wrap.append(bt);

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

// ── boards whose flashing port isn't the one you can see ────────────────────
// Catalog-driven (products[].access, from gen_flash.py's BOARD_ACCESS), and
// rendered BEFORE Connect on purpose: on the 60 GHz radar kit the port that
// flashes is the XIAO's, inside the case, while the reachable one is power.
// Plug into the reachable port and the device chooser stays empty — which
// reads as "my board is dead" instead of "wrong port". Every family without
// an access block plugs in and works, and gets no card at all.
function accessCards() {
  const products = (state.catalog && state.catalog.products) || [];
  const seen = new Set();
  const out = [];
  for (const p of products) {
    const a = p.access;
    if (!a || seen.has(p.family)) continue;
    seen.add(p.family);
    const d = el("details", "flash-access");
    d.dataset.family = p.family;
    d.append(el("summary", null, `${p.name.replace(/ · .*$/, "")} — ${a.headline}`));
    const g = el("p", null);
    g.append(document.createTextNode("Flash into "));
    g.append(el("strong", null, a.flash_port));
    g.append(document.createTextNode("."));
    d.append(g);
    const ol = el("ol", "flash-steps");
    for (const s of a.steps) ol.append(el("li", null, s));
    d.append(ol);
    const other = el("p", "muted");
    other.append(el("strong", null, `That other port — ${a.other_port}: `));
    other.append(document.createTextNode(a.other_effect));
    d.append(other);
    if (a.enclosure_note) d.append(el("p", "muted", a.enclosure_note));
    if (a.reassembly) d.append(el("p", "fineprint", a.reassembly));
    if (a.doc) {
      const p2 = el("p", "fineprint");
      const link = el("a", null, "The full port guide →");
      link.href = a.doc;
      link.target = "_blank";
      link.rel = "noopener";
      p2.append(link);
      d.append(p2);
    }
    out.push(d);
  }
  return out;
}

// ── cold start: the gesture that keeps an unknown board off your desktop ────
// This is deliberately BEFORE the Connect button, because it is the only
// control here that has to happen before the cable goes in. Everything else
// the flasher does is a check after the fact; this one prevents.
//
// It asks rather than detects, and says so: the ROM's download mode and a
// stock hwcdc firmware present the SAME USB descriptor on a native-USB board
// (docs/browser_flasher.md § What the Canary is called over USB), so there is
// nothing to measure. Claiming otherwise would be a comforting lie.
function coldStartCard() {
  const card = el("section", "flash-coldstart");
  card.append(el("h3", null, "🛡 New board from a shop or marketplace? Do this first"));
  card.append(el("p", null,
    "It arrives running somebody else's firmware, and these chips have native " +
    "USB — so that firmware can introduce itself to your computer as anything " +
    "it likes, including a keyboard, the moment you plug it in. No web page can " +
    "step in front of that; your operating system finishes with the device " +
    "before this page even learns it exists."));
  card.append(el("p", null,
    "One move stops it, and it's the chip's own silicon doing the stopping:"));

  const ol = el("ol", "flash-steps flash-coldstart-steps");
  const li1 = el("li");
  li1.append(document.createTextNode("Unplug the board, if it's plugged in."));
  const li2 = el("li");
  li2.append(document.createTextNode("Press and hold "), kbd("BOOT"),
    document.createTextNode(" (marked B)."));
  const li3 = el("li");
  li3.append(document.createTextNode("Still holding it, plug the USB-C cable in. Then let go."));
  ol.append(li1, li2, li3);
  card.append(ol);
  card.append(el("p", "fineprint",
    "The chip comes up in its mask-ROM download mode and the firmware it shipped " +
    "with never runs a single instruction. That's not a promise this page is " +
    "making — it's unerasable ROM, the same property that makes the board " +
    "impossible to brick from here."));

  // What the user did — carried into the intake report, honestly labeled as
  // their answer rather than our measurement.
  const note = el("p", "fineprint flash-coldstart-note", "");
  const row = el("div", "flash-row flash-coldstart-row");
  const held = el("button", "ghost small", "✓ I held BOOT while plugging in");
  const hot = el("button", "ghost small", "It's already plugged in");
  const mark = (isCold) => {
    state.heldBoot = isCold;
    held.classList.toggle("flash-chosen", isCold);
    hot.classList.toggle("flash-chosen", !isCold);
    note.textContent = isCold
      ? "Good — its own firmware never got to run."
      : "Noted. Whatever shipped on the chip has already booted once; the erase " +
        "below still removes it, but next time hold BOOT on the way in.";
  };
  held.addEventListener("click", () => mark(true));
  hot.addEventListener("click", () => mark(false));
  row.append(held, hot);
  card.append(row, note);
  return card;
}

// ── phase: connect ──────────────────────────────────────────────────────────
function phaseConnect() {
  const box = el("section", "flash-card flash-connect");
  box.dataset.step = "1";
  box.append(el("div", "flash-big-emoji flash-plug", "🔌"));
  box.append(el("h2", null, "Plug in your Canary, then let’s meet it"));
  box.append(el("p", "muted",
    "Connect the board to this computer with a USB-C data cable. When you " +
    "click Connect, your browser asks which device — pick the one that " +
    "appears (often “USB JTAG/serial” or “USB Serial”)."));

  // ── the one move that actually protects the computer ──
  // A board bought unflashed arrives running somebody else's firmware, and an
  // ESP32-S3 has native USB: in TinyUSB mode that firmware can enumerate as a
  // KEYBOARD and type at the desktop the instant the cable goes in. No page
  // can intercept that — the OS finishes enumerating before Web Serial exists.
  // Holding BOOT is what stops it, in mask ROM: the chip lands in download
  // mode and the resident image never executes an instruction.
  box.append(coldStartCard());

  // Which port to plug into, for the boards where that is a real question.
  // Before the button, because the cable goes in before the button is clicked.
  for (const card of accessCards()) box.append(card);

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

// Sync the ROM and open an esptool session on an already-granted port —
// the baud ladder from the original connect flow, extracted so the live
// voice can hand the port back to the bootloader without a new chooser.
// statusCb narrates ("Trying a gentler speed…"); throws the last error.
async function openEsptool(port, statusCb) {
  const top = state.catalog.flash_baud || core.FLASH_BAUDS[0];
  // Start at the ceiling — lowered a rung by any prior write-time failure so
  // the retry's large transfer runs at a gentler speed — and step down. Keep
  // the slowest rung so the list is never empty.
  const ceil = state.baudCeiling || top;
  const baudList = [top, ...core.FLASH_BAUDS.filter((b) => b !== top)].filter((b) => b <= ceil);
  if (!baudList.length) baudList.push(core.FLASH_BAUDS[core.FLASH_BAUDS.length - 1]);
  let esploader = null, transport = null, lastErr = null;
  for (let i = 0; i < baudList.length; i++) {
    const baud = baudList[i];
    if (i > 0 && statusCb) statusCb(`Trying a gentler speed (${baud})…`);
    transport = new Transport(port, false);
    esploader = new ESPLoader({
      transport, baudrate: baud,
      romBaudrate: state.catalog.console_baud || 115200,
      terminal: makeTerminal(),
    });
    try {
      state.chipDesc = await esploader.main();
      state.usedBaud = baud;
      lastErr = null;
      break;
    } catch (e) {
      lastErr = e;
      esploader = null;
      try { await transport.disconnect(); } catch {}
    }
  }
  if (!esploader) throw lastErr || new Error("could not connect");
  return { esploader, transport };
}

// ── the live voice: the board's own serial console, before anything is
// written ─────────────────────────────────────────────────────────────────
// When the connect reads find real firmware on the board, the flasher lets
// it BOOT and streams its console right on the hello card — the board talks
// first, flashing comes second. Any bootloader action (install, health
// check, backup, rescue) calls ensureSession(), which quietly hands the
// port back to the bootloader. No chooser, no gestures.
async function startVoice(consoleEl, onIdentity) {
  // Re-entering the connected phase while a voice is already running (e.g.
  // "not yet" from the confirm card): re-bind the LIVE stream to the fresh
  // console element instead of leaving it talking to a detached node.
  if (state.voice) {
    const v0 = state.voice;
    v0.consoleEl = consoleEl;
    v0.onIdentity = onIdentity;
    consoleEl.textContent = v0.buf || "listening…";
    if (v0.identity && onIdentity) onIdentity(v0.identity);
    return;
  }
  if (!state.session) return;
  const { port } = state.session;
  // Claim the voice slot SYNCHRONOUSLY, before any await — a concurrent
  // second call must see it and take the re-bind path above, not race the
  // port open.
  const v = { port, alive: true, reader: null, writer: null, identity: null,
              buf: "", consoleEl, onIdentity };
  state.voice = v;
  try { await state.session.esploader.after("hard_reset"); } catch {}
  try { await state.session.transport.disconnect(); } catch {}
  state.session = null;
  if (state.voice !== v || !v.alive) return; // stopVoice won the race — it owns cleanup
  try {
    await port.open({ baudRate: state.catalog.console_baud || 115200 });
    if (state.voice !== v || !v.alive) { // superseded mid-open: nothing may own this port
      try { await port.close(); } catch {}
      return;
    }
    v.reader = port.readable.getReader();
    try { v.writer = port.writable.getWriter(); } catch { /* read-only is fine */ }
  } catch (e) {
    // Never strand an owner-less open port: close it so ensureSession (or a
    // fresh Connect) can take it back.
    try { v.reader && v.reader.releaseLock(); } catch {}
    try { await port.close(); } catch {}
    state.voice = null;
    v.consoleEl.textContent = "(the console didn’t open — " + String(e.message || e) +
      " — installing still works)";
    return;
  }
  // Ask for the signed self-manifest once the app settles: health, boots,
  // temperature — the live story, before a byte is written. Twice, in case
  // the first lands mid-boot.
  const ask = () => {
    if (!state.voice || v.identity || !v.writer) return;
    try { v.writer.write(new TextEncoder().encode("j\n")); } catch { /* quiet build */ }
  };
  setTimeout(ask, 900);
  setTimeout(ask, 2600);
  const dec = new TextDecoder();
  (async () => {
    try {
      for (;;) {
        const { value, done } = await v.reader.read();
        if (done || !v.alive) break;
        // Everything goes through v.* so a phase re-render can re-bind the
        // console element and identity callback mid-stream.
        v.buf = (v.buf + dec.decode(value, { stream: true })).slice(-8000);
        v.consoleEl.textContent = v.buf;
        v.consoleEl.scrollTop = v.consoleEl.scrollHeight;
        if (!v.identity) {
          const m = core.parseSelfManifest(v.buf);
          if (m) { v.identity = m; if (v.onIdentity) v.onIdentity(m); }
        }
      }
    } catch { /* unplug/re-enumeration — ensureSession or reconnect recovers */ }
  })();
}

async function stopVoice() {
  const v = state.voice;
  if (!v) return null;
  state.voice = null;
  v.alive = false;
  try { v.reader && await v.reader.cancel(); } catch {}
  try { v.reader && v.reader.releaseLock(); } catch {}
  try { v.writer && v.writer.releaseLock(); } catch {}
  try { await v.port.close(); } catch {}
  return v.port;
}

// Guarantee an esptool session for a bootloader action. If the live voice
// holds the port, it is closed and the SAME port re-synced — the flasher
// flips the board back into download mode itself, no gesture needed.
async function ensureSession(statusCb) {
  if (state.session) return true;
  const port = await stopVoice();
  if (!port) return false;
  try {
    const { esploader, transport } = await openEsptool(port, statusCb);
    esploader.FLASH_READ_TIMEOUT = 15000;
    state.session = { port, transport, esploader };
    return true;
  } catch {
    return false;
  }
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
  box.dataset.step = "1";
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

  // The USB identity, read once — used for a driver hint if it won't connect.
  state.portInfo = (port.getInfo && port.getInfo()) || {};

  // Baud ladder: try the fast speed, and step down automatically on failure.
  // esptool syncs the ROM at 115200 and only then switches to the flash speed;
  // flaky cables, unpowered hubs, and long USB runs sync fine but choke the
  // high-speed transfer. Walking down heals "it won't connect" silently
  // instead of dead-ending.
  let opened;
  try {
    opened = await openEsptool(port, (t) => { detail.textContent = t; });
  } catch (e) {
    clearTimeout(nudgeTimer);
    state.busy = false;
    setPhase(connectFailed(e || new Error("could not connect"), state.portInfo));
    return;
  }
  clearTimeout(nudgeTimer);
  const { esploader, transport } = opened;

  try {
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
    state.busy = false;
    // Escalation from a failed install: reconnect, then jump straight to the
    // clean-install rescue flow the user asked for.
    if (state.resumeRescue) { state.resumeRescue = false; setPhase(phaseRescue()); return; }
    await readCurrentFirmware();     // best-effort; never throws out
    await readPassport();            // the counters worth showing at hello
    await runIntake();               // customs — read-only, before anything is written
    ensureManifest();                // kick off (async) manifest load
    chirp("hello");                  // the Nursery says hi (only if invited)
    setPhase(phaseConnected());
  } catch (e) {
    state.busy = false;
    try { await transport.disconnect(); } catch {}
    setPhase(connectFailed(e, state.portInfo));
  }
}

function connectFailed(e, pinfo) {
  const v = core.classifyFlashError(e);
  const box = errorBox(
    v.title || "Couldn’t reach the board",
    v.kind === "not-in-download" || v.kind === "unknown"
      ? "That almost always means it isn’t in download mode yet — no harm done."
      : v.hint,
    false);
  // The commonest *confusing* failure: the Vision's CAMERA MODULE plugged into
  // the ESP32 flow. It's a different chip (WE2 behind a CH343) that never speaks
  // esptool, so the sync "fails" — recognize it by its USB id and hand the user
  // to the right engine instead of dead-ending on generic download-mode advice.
  // identifyPort matches vid AND pid, so a CH340 ESP32 board (same WCH vendor,
  // different product) is never mistaken for the module. Host-tested.
  if (core.identifyPort(pinfo || state.portInfo, state.catalog) === "we2" &&
      state.catalog.we2_module) {
    const card = el("div", "flash-note flash-note-soft");
    card.append(el("strong", null, "📷 That’s the Vision’s camera module — not an ESP32 board."));
    card.append(el("p", null,
      "Nothing’s wrong: it flashes a different way (its own engine, over the " +
      "module’s USB-C port). Let’s load its person-detection model instead."));
    box.append(card);
    const row = el("div", "flash-row");
    const go = el("button", "primary", "Load the camera module’s model →");
    go.addEventListener("click", () => setPhase(phaseModule({
      catalog: state.catalog, setPhase, back: () => setPhase(phaseConnect()),
    })));
    const back = el("button", "ghost", "Back");
    back.addEventListener("click", () => setPhase(phaseConnect()));
    row.append(go, back);
    box.append(row);
    return box; // skip the generic download-mode / driver advice — wrong for the module
  }
  // For a genuine "not in download mode" show the gesture; for a specific cause
  // (port busy, permissions, cable) the hint above already says what to do.
  if (v.kind === "not-in-download" || v.kind === "unknown") {
    box.append(downloadModeSteps());
    box.append(el("p", "muted", "Then click “Try again”."));
  }
  // If it's a bridge-chip board that may be missing its driver, link it.
  appendDriverHint(box, pinfo || state.portInfo);
  const row = el("div", "flash-row");
  const retry = el("button", "primary", "Try again");
  retry.addEventListener("click", () => setPhase(phaseConnect()));
  row.append(retry, diagnosticReportButton(() => ({ stage: "connect", error: String(e && e.message ? e.message : e) })));
  box.append(row);
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

// ── self-healing: the "I'm stuck" escape hatch ──────────────────────────────
// One click turns a stuck moment into a paste-able report for Discussions.
// Public-only by construction (buildDiagnosticReport takes safe facts only —
// never WiFi credentials or keys).
const hex4 = (n) => (n == null ? null : "0x" + n.toString(16).padStart(4, "0"));

function gatherDiagnostics(extra = {}) {
  const b = core.detectBrowser(navigator.userAgent || "", navigator.maxTouchPoints || 0);
  const pi = state.portInfo || {};
  let usb;
  if (pi.usbVendorId != null) {
    const bridge = core.usbBridgeInfo(pi.usbVendorId, pi.usbProductId);
    usb = `${hex4(pi.usbVendorId)}:${hex4(pi.usbProductId) || "?"} ` +
      (bridge ? `(${bridge.name})` : "(native USB)");
  }
  let logTail;
  try { logTail = (logSink && logSink.textContent) || undefined; } catch {}
  return core.buildDiagnosticReport({
    browser: b.label,
    platform: navigator.platform || undefined,
    webSerial: "serial" in navigator,
    catalogVersion: state.catalog && state.catalog.fw_train,
    chip: state.chip,
    chipDesc: state.chipDesc,
    mac: state.mac ? core.formatMac(state.mac) : undefined,
    flashBytes: state.flashBytes || undefined,
    usb,
    baud: state.usedBaud,
    logTail,
    ...extra,
  });
}

function diagnosticReportButton(extra) {
  const btn = el("button", "ghost small flash-report-btn", "Copy a diagnostic report");
  btn.addEventListener("click", async () => {
    const report = gatherDiagnostics(typeof extra === "function" ? extra() : (extra || {}));
    try {
      await navigator.clipboard.writeText(report);
      btn.textContent = "Copied ✓ — paste it into a Discussion for help";
    } catch {
      // No clipboard permission — show it to select by hand.
      const ta = el("textarea", "flash-report-ta");
      ta.value = report; ta.readOnly = true; ta.rows = 12;
      btn.replaceWith(ta); ta.focus(); ta.select();
    }
  });
  return btn;
}

// Add a bridge-chip driver hint to an error card when the board uses a
// USB-serial bridge that may be missing its OS driver (native-USB boards
// return null → nothing shown).
function appendDriverHint(box, pinfo) {
  const bridge = pinfo && core.usbBridgeInfo(pinfo.usbVendorId, pinfo.usbProductId);
  if (!bridge) return;
  const d = el("p", "flash-note flash-note-soft");
  d.append(document.createTextNode(bridge.note + " "));
  const a = el("a", "start-link", `Get the ${bridge.name} driver →`);
  a.href = bridge.driverUrl; a.target = "_blank"; a.rel = "noopener noreferrer";
  d.append(a);
  box.append(d);
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
  state.pt = null;
  // Did the partition sector actually come off the chip? "ok" means we got
  // bytes (whatever was in them); "failed" means the read threw. The intake
  // check needs these apart — an unread chip must never render as an empty
  // one, or missing evidence becomes the cleanest verdict on the page.
  state.ptRead = "failed";
  const { esploader } = state.session;
  try {
    const ptBytes = await readFlashChunked(esploader, 0x8000, 0xc00);
    state.ptRead = "ok";
    const { entries, apps } = core.parsePartitionTable(ptBytes);
    // An erased chip reads back as 0xFF with no valid table — that's a
    // successfully-read blank, not a partition map, so leave state.pt null.
    if (entries && entries.length) state.pt = { entries, apps };
    // The verdict must judge the slot the bootloader actually runs, so read
    // otadata (best-effort) before choosing which descriptor to trust.
    let otadata = null;
    try {
      const otaPart = entries.find(core.isOtaDataPart);
      const otaSlots = apps.filter((a) => a.subtype >= 0x10 && a.subtype < 0x20);
      if (otaPart && otaSlots.length) {
        const ob = await readFlashChunked(esploader, otaPart.offset, Math.min(otaPart.size, 0x2000));
        otadata = core.parseOtaData(ob, otaSlots.length);
      }
    } catch {}
    const app = core.pickBootedAppPartition(apps, otadata);
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
      built: d.date ? `${d.date} ${d.time || ""}`.trim() : null,
      idf: d.idfVer || null,
    };
  } catch (e) {
    state.current = { unknown: true };
  }
}

// ── customs: the read-only intake check, before a byte is written ───────────
// Everything here is a READ. The eFuse pass is a plain register read at
// block 0 — the flasher still never issues a burn, which is what the
// un-brickable promise rests on. Any probe may fail (an old board, a chip
// with no verified table, a flaky cable); a probe that fails is reported as
// "not checked" and never as "checked and clean".
async function runIntake() {
  state.intake = null;
  if (!state.session) return;
  const { esploader } = state.session;
  const macStr = state.mac ? core.formatMac(state.mac) : null;
  const rosterHit = core.rosterFind(state.roster, macStr);

  // 1. Security eFuses — the one thing a full erase can never undo, because
  //    eFuses are one-way. A previous owner's lockdown lives here.
  let efuses = null;
  try {
    const base = esploader.chip && esploader.chip.EFUSE_BASE;
    if (Number.isFinite(base)) {
      const words = [];
      for (const addr of intake.efuseBlock0Addrs(base)) {
        words.push(await esploader.readReg(addr));
      }
      efuses = intake.readSecurityEfuses(state.chip, words);
    }
  } catch { efuses = null; }

  // 2. Is the flash the size its SPI id claims? A relabeled part wraps its
  //    address lines, so reading AT a candidate capacity returns offset zero
  //    again. Probing "declared − 4 KB" would not do it: on a 4 MB die
  //    pretending to be 16 MB that address wraps to 0x3FF000, the top of the
  //    real part, which holds something other than the head.
  let alias = { level: "unknown", label: "Flash size not checked" };
  try {
    const declared = state.flashBytes;
    if (declared && declared > 0x2000) {
      const head = await readFlashChunked(esploader, 0, 0x1000);
      const probes = [];
      for (const at of intake.flashAliasCandidates(declared)) {
        probes.push({ atBytes: at, bytes: await readFlashChunked(esploader, at, 0x1000) });
      }
      alias = intake.flashAliasVerdict({ declaredBytes: declared, head, probes });
    }
  } catch { /* leave it "not checked" — never invent a clean result */ }

  const cold = intake.coldBootVerdict({
    heldBoot: state.heldBoot,
    hadResidentFirmware: !!(state.current && !state.current.unknown),
  });
  const shipped = intake.shippedWith({
    projectName: state.current && state.current.projectName,
    // A partition sector we READ and found empty is a blank chip; one we
    // couldn't read at all is unknown. state.ptRead keeps those apart —
    // `!state.pt` alone would call a failed read the cleanest possible result.
    blank: state.ptRead === "ok" && !state.pt,
    read: state.ptRead,
  });
  const mac = intake.macChecks(macStr);
  const dup = intake.duplicateMacCheck(state.roster, macStr);
  // Only OUR history waives the mandatory erase — never the board's own
  // account of itself, which an untrusted image controls. `ownerClaimed` is
  // the user's explicit "this is my board" on the confirm card.
  const firstContact = intake.isFirstContact({ rosterHit, ownerClaimed: state.ownerClaimed });

  const findings = [cold, shipped, mac, dup, alias, ...intake.efuseFindings(efuses)];
  state.intake = {
    efuses, alias, cold, shipped, mac, dup, firstContact,
    verdict: intake.intakeVerdict(findings),
  };
}

// ── the rest of the board's passport (read-only, best-effort, seconds) ──────
// The same probes the full health check runs, trimmed to the counters worth
// showing the moment the board says hello: how many updates it has seen, its
// lifetime boots, the witness-record count, the tamper flag, and whether it
// has ever hard-crashed. Every read is optional — a brand-new board simply
// has no story yet.
async function readPassport() {
  state.passport = null;
  if (!state.pt || !state.session) return;
  const { esploader } = state.session;
  const { entries, apps } = state.pt;
  const passport = {};
  try {
    const otaPart = entries.find(core.isOtaDataPart);
    const otaSlots = apps.filter((a) => a.subtype >= 0x10 && a.subtype < 0x20);
    if (otaPart) {
      const ob = await readFlashChunked(esploader, otaPart.offset, Math.min(otaPart.size, 0x2000));
      passport.otadata = core.parseOtaData(ob, otaSlots.length);
    }
  } catch {}
  try {
    const cd = entries.find(core.isCoredumpPart);
    if (cd) {
      const cb = await readFlashChunked(esploader, cd.offset, 16);
      passport.coredump = core.parseCoredumpHeader(cb, cd.size);
    }
  } catch {}
  try {
    const nvs = entries.find(core.isNvsPart);
    if (nvs) {
      const nb = await readFlashChunked(esploader, nvs.offset, nvs.size);
      passport.witness = core.witnessSummary(core.parseNvs(nb));
    }
  } catch {}
  state.passport = passport;
}

// ── phase: connected — chip card + firmware picker ──────────────────────────
function phaseConnected() {
  const wrap = el("div", "flash-connected");
  wrap.dataset.step = "2";

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
    const built = state.current.built ? ` (built ${state.current.built})` : "";
    cur.append(document.createTextNode(`Looks like it’s running ${name}${ver}${built} right now.`));
  }
  ht.append(cur);
  // Batch guard: if THIS exact board already hatched this session, say so
  // before anything else — the repeated-actions trap is plugging in a board
  // you already did and not being sure.
  const prev = core.rosterFind(state.roster, state.mac ? core.formatMac(state.mac) : null);
  if (prev) {
    const again = el("p", "flash-role flash-already");
    again.append(document.createTextNode(
      `🐣 Already hatched this session — #${prev.n}, ${prev.product}` +
      `${prev.version ? " v" + prev.version : ""}` +
      `${prev.preset ? ", dialed " + prev.preset : ""}. ` +
      `Plug in the next board, or reflash this one freely.`));
    ht.append(again);
  }
  const role = renderRoleLine();
  if (role) ht.append(role);
  ht.append(modeBadge("download"));
  head.append(ht);
  hello.append(head);

  const facts = el("div", "flash-facts");
  facts.append(fact("Chip", state.chipDesc || state.chip, "chip"));
  if (state.mac) facts.append(fact("ID (MAC)", core.formatMac(state.mac), "mac"));
  if (state.flashBytes) facts.append(fact("Flash", core.formatBytes(state.flashBytes), "flash_size"));
  hello.append(facts);

  // The passport strip: the board's story so far, read without changing a
  // byte while we said hello — updates seen, lifetime boots, witness records,
  // crash history. A brand-new board simply has no rows yet.
  const story = core.passportRows(state.passport || {});
  if (story.length) {
    const strip = el("div", "flash-passport flash-passport-stagger");
    const HELP_FOR = { updates: "updates_seen", boots: "boots", seq: "witness_records",
                       tamper: "tamper_flag", crash: "crash_record" };
    story.forEach((r) => {
      const chip = el("span", `flash-passport-chip flash-passport-${r.tone || "ok"}`);
      chip.append(el("strong", null, r.label + " "));
      chip.append(document.createTextNode(r.value));
      const hd = HELP_FOR[r.id] && helpDot(HELP_FOR[r.id]);
      if (hd) chip.append(hd);
      strip.append(chip);
    });
    hello.append(strip);
  }
  if (state.current && !state.current.unknown) {
    const feel = el("p", "fineprint flash-feel");
    feel.append(document.createTextNode(
      "Those counters were read cold, without booting it. For the live story — " +
      "its self-check score and how it’s feeling right now — "));
    const ask = el("button", "ghost small", "🩺 ask the board itself");
    ask.addEventListener("click", () => openMonitor({ proveIdentity: true }));
    feel.append(ask);
    hello.append(feel);
  }

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
  tools.append(health, helpDot("health_check") || "", mon, helpDot("serial_monitor") || "",
               rescue, helpDot("rescue") || "");
  hello.append(tools);
  const toolsNote = el("p", "fineprint",
    "Health check reads the board’s story without changing a byte. The serial " +
    "monitor is the board’s live voice over USB — watch it and send commands. " +
    "Rescue brings a misbehaving board back to a known-good state.");
  hello.append(toolsNote);
  wrap.append(hello);

  // Customs: what the read-only intake check found, before anything is written.
  const customs = renderIntakeCard();
  if (customs) wrap.append(customs);

  // The live voice: a board that carries real firmware BOOTS and talks,
  // right here, before anything is written. (A brand-new board has nothing
  // to say — the bootloader keeps the port and this card doesn't appear.)
  if (state.current && !state.current.unknown) {
    const voice = el("section", "flash-card flash-voice");
    voice.append(el("h3", null, "🎙 Its live voice — before anything is written"));
    voice.append(el("p", "fineprint",
      "The flasher let the board boot; this is its own serial console, live " +
      "over the cable. Install below whenever you’re ready — the flasher " +
      "flips it back into download mode by itself."));
    const idSlot = el("div", "flash-voice-id");
    voice.append(idSlot);
    const vcon = el("pre", "flash-console flash-voice-console", "listening…");
    voice.append(vcon);
    // A Sense on the wire gets its bench one click away — the console is its
    // voice, but the radar bench is its senses.
    const curProd = core.matchProjectToProduct(state.catalog, state.current.projectName);
    const curRole = curProd && core.productRole(curProd.id);
    if (curRole === "sense") {
      const rb = el("button", "ghost small", "👋 open the radar bench — feel it sense");
      rb.addEventListener("click", () => openSenseBench(curProd));
      voice.append(rb);
    } else if (curRole === "wap") {
      const rb = el("button", "ghost small", "🌊 open the field bench — feel the room");
      rb.addEventListener("click", () => openWapBench(curProd));
      voice.append(rb);
    }
    wrap.append(voice);
    startVoice(vcon, (m) => {
      idSlot.innerHTML = "";
      const chips = el("div", "flash-passport");
      const hv = core.healthVerdict(m.health);
      const scored = typeof m.health === "number" && Number.isFinite(m.health);
      const hchip = el("span", `flash-passport-chip flash-passport-${hv.level === "ok" ? "ok" : "warn"}`);
      hchip.append(el("strong", null, "Self-check "),
        document.createTextNode(scored ? `${hv.icon} ${m.health}/100` : hv.label));
      const hHelp = helpDot("self_check");
      if (hHelp) hchip.append(hHelp);
      chips.append(hchip);
      if (typeof m.temp_c === "number" && Number.isFinite(m.temp_c) &&
          m.temp_c > -40 && m.temp_c < 150) {
        const warm = m.temp_c >= 70;
        const tchip = el("span", `flash-passport-chip${warm ? " flash-passport-warn" : ""}`);
        tchip.append(el("strong", null, "Heat "),
          document.createTextNode(`${Math.round(m.temp_c)} °C${warm ? " — give it air" : ""}`));
        const tHelp = helpDot("temperature");
        if (tHelp) tchip.append(tHelp);
        chips.append(tchip);
      }
      if (m.tamper) {
        chips.append(el("span", "flash-passport-chip flash-passport-warn",
          "tamper flag raised"));
      }
      idSlot.append(chips);
    });
  }

  // Firmware picker (chip-guarded).
  wrap.append(renderPicker());

  // "Which board am I holding?" — a labeled identity panel for each product
  // this chip can be, drawn from the honest boards/enclosures catalogs so the
  // user can match the board in their hand and see the product it becomes.
  const idWrap = el("div", "flash-identity-wrap");
  wrap.append(idWrap);
  try {
    const forChip = (state.catalog.products || []).filter((p) => p.chip === state.chip);
    const seen = new Set();
    for (const p of forChip) {
      const key = core.shortName ? core.shortName(p.id) : p.id;
      if (seen.has(key)) continue;
      seen.add(key);
      mountBoardIdentity(idWrap, p); // async, best-effort; no-op on any miss
    }
  } catch { /* identity is a nicety — never block the flow */ }

  // The session's progression — a batch never loses its place.
  const roster = renderRosterStrip();
  if (roster) wrap.append(roster);

  const disconnect = el("button", "ghost small flash-disconnect", "disconnect");
  disconnect.addEventListener("click", onDisconnect);
  wrap.append(disconnect);
  return wrap;
}

// ── the intake report ───────────────────────────────────────────────────────
// One card, in the order that matters: the verdict, then only the findings
// worth a sentence. A board where nothing looks wrong gets a single quiet
// line — the report has to stay cheap to read, or people stop reading it.
function renderIntakeCard() {
  const it = state.intake;
  if (!it) return null;
  const v = it.verdict;

  const card = el("section", `flash-card flash-intake flash-intake-${v.level}`);
  const head = el("div", "flash-intake-head");
  head.append(el("span", "flash-intake-icon",
    v.level === "stop" ? "⛔" : v.level === "attention" ? "⚠️" : "✅"));
  head.append(el("h3", null, v.headline));
  card.append(head);
  card.append(el("p", "fineprint",
    "Customs — read off the board without changing a byte: its security fuses, " +
    "whether the flash is the size it claims, and what it arrived running."));

  for (const f of v.findings) {
    const row = el("div", `flash-intake-finding flash-intake-${f.level}`);
    row.append(el("strong", null, f.label));
    if (f.detail) row.append(el("span", "flash-intake-detail", f.detail));
    card.append(row);
  }

  if (v.level === "stop") {
    card.append(el("p", "flash-intake-verdict",
      "Every one of those is a state a full erase cannot fix — eFuses burn one " +
      "way only. This board can't become a Canary. If you bought it as new, " +
      "that is worth a refund request: someone used it before you."));
  } else if (v.level === "clear") {
    card.append(el("p", "flash-intake-verdict",
      "Nothing we can check looks wrong. That isn't the same as proven safe — " +
      "no read over USB can see a modified circuit board — but the security " +
      "fuses are untouched, so nobody has locked this chip before you."));
  }

  // The eFuse table itself, folded away: the evidence behind the verdict, for
  // anyone who wants to see the reading rather than trust the summary.
  if (it.efuses && it.efuses.supported && Array.isArray(it.efuses.fields) && it.efuses.fields.length) {
    const det = el("details", "flash-intake-efuses");
    det.append(el("summary", null,
      it.efuses.virgin
        ? "Security fuses: all clear — show the reading"
        : "Security fuses: show the reading"));
    const list = el("div", "flash-intake-efuse-list");
    for (const f of it.efuses.fields) {
      const r = el("div", `flash-intake-efuse${f.burned ? " flash-intake-burned" : ""}`);
      r.append(el("span", "flash-intake-efuse-key", f.key));
      r.append(el("span", "flash-intake-efuse-val", f.burned ? `burned (${f.value})` : "not set"));
      list.append(r);
    }
    det.append(list);
    det.append(el("p", "fineprint",
      "Read straight out of eFuse block 0. Nothing here is ever written — the " +
      "flasher issues no burn command at all, which is why you cannot ruin a " +
      "board from this page."));
    det.append(el("p", "fineprint",
      "A factory-fresh chip reads every one of these as unset. Espressif does " +
      "program fuses at the factory, but in other blocks — so anything set here " +
      "was set by a person."));
    card.append(det);
  } else if (it.efuses && !it.efuses.supported) {
    card.append(el("p", "fineprint",
      `We don't have a verified fuse map for ${state.chip}, so that check was ` +
      "skipped rather than guessed at."));
  } else if (!it.efuses) {
    card.append(el("p", "fineprint",
      "The security fuses couldn't be read this time — reported as unchecked, " +
      "not as clean."));
  }

  return card;
}

function fact(label, val, topicId) {
  const f = el("div", "flash-fact");
  const lab = el("span", "flash-fact-label", label);
  if (topicId) {
    const hd = helpDot(topicId);
    if (hd) lab.append(hd);
  }
  f.append(lab);
  f.append(el("span", "flash-fact-val", val));
  return f;
}

// ── per-setting help: the ⓘ that teaches (catalog-driven) ───────────────────
// One small button per setting; a tap opens a calm card: what it is, when to
// touch it, what the default means. Copy comes from catalog.settings_help
// (generated from the firmware's own values), looked up via core.helpTopic —
// so a control without a topic simply has no dot, never a broken one.
// Only one ⓘ open at a time; Escape and any outside click close it —
// pro-grade popover manners, wired up once, lazily.
let openHelp = null; // { pop, btn }
function closeHelp() {
  if (!openHelp) return;
  openHelp.pop.classList.add("flash-hidden");
  openHelp.btn.setAttribute("aria-expanded", "false");
  openHelp = null;
}
let helpManagersInstalled = false;
function installHelpManagers() {
  if (helpManagersInstalled) return;
  helpManagersInstalled = true;
  document.addEventListener("click", (ev) => {
    if (openHelp && !ev.target.closest(".flash-help")) closeHelp();
  });
  document.addEventListener("keydown", (ev) => {
    if (ev.key === "Escape") closeHelp();
  });
}

function helpDot(topicId) {
  const t = core.helpTopic(state.catalog, topicId);
  if (!t) return null;
  installHelpManagers();
  const wrap = el("span", "flash-help");
  const btn = el("button", "flash-help-dot", "?");
  btn.type = "button";
  btn.setAttribute("aria-label", `About: ${t.label}`);
  btn.setAttribute("aria-expanded", "false");
  const pop = el("span", "flash-help-pop flash-hidden");
  pop.setAttribute("role", "note");
  pop.append(el("strong", "flash-help-title", t.label));
  pop.append(el("span", "flash-help-what", t.what));
  if (t.when) {
    const w = el("span", "flash-help-when");
    w.append(el("em", null, "When to touch it: "), document.createTextNode(t.when));
    pop.append(w);
  }
  if (t.default) {
    const d = el("span", "flash-help-default");
    d.append(el("em", null, "Default: "), document.createTextNode(t.default));
    pop.append(d);
  }
  btn.addEventListener("click", (ev) => {
    // The dot often lives inside a <label>: stop the click from toggling the
    // checkbox the label wraps.
    ev.preventDefault();
    ev.stopPropagation();
    const wasOpen = openHelp && openHelp.pop === pop;
    closeHelp();
    if (!wasOpen) {
      pop.classList.remove("flash-hidden");
      btn.setAttribute("aria-expanded", "true");
      openHelp = { pop, btn };
    }
  });
  wrap.append(btn, pop);
  return wrap;
}

// ── the Nursery roster strip: the session's progression, at a glance ────────
function renderRosterStrip() {
  if (!state.roster.length) return null;
  const sec = el("details", "flash-roster");
  if (state.roster.length <= 3) sec.open = true;
  const plural = state.roster.length === 1 ? "hatchling" : "hatchlings";
  const sum = el("summary", null,
    `🐣 This session’s nursery — ${state.roster.length} ${plural} so far`);
  const rh = helpDot("roster");
  if (rh) sum.append(rh);
  sec.append(sum);
  const list = el("div", "flash-roster-list");
  core.rosterLines(state.roster, Date.now()).forEach((l) => {
    const row = el("div", "flash-roster-row");
    row.append(el("span", "flash-roster-n", `#${l.n}`));
    const main = el("span", "flash-roster-main");
    main.append(el("strong", null, l.label));
    if (l.mac) main.append(document.createTextNode(` ${l.mac}`));
    if (l.extras) main.append(el("span", "flash-roster-extras", ` · ${l.extras}`));
    row.append(main);
    row.append(el("span", "flash-roster-ago", l.ago));
    list.append(row);
  });
  sec.append(list);
  sec.append(el("p", "fineprint",
    "Kept for this tab only, and only public facts — which firmware, which " +
    "dials, whether WiFi was baked (never the network itself)."));
  return sec;
}

// ── role: what this board IS, read from what it RUNS ────────────────────────
function renderRoleLine() {
  const cur = state.current;
  if (!cur || cur.unknown) return null;
  // A display build read off the wire: name it and offer the live glass.
  if (core.looksLikeDisplayProject(cur.projectName)) {
    const d = core.displayFor(state.catalog, cur.projectName) || core.displaysIn(state.catalog)[0];
    const p = el("p", "flash-role flash-role-display");
    p.append(el("span", null,
      `🖼 This board is a display — it SHOWS${d ? `. It looks like the ${d.name}.` : "."} `));
    if (d) {
      const b = el("button", "ghost small", "see its screen, live →");
      b.addEventListener("click", () => setPhase(phaseDisplayBench(d, phaseConnected)));
      p.append(b);
    }
    return p;
  }
  const prod = core.matchProjectToProduct(state.catalog, cur.projectName);
  if (!prod) return null;
  const role = prod.role || core.productRole(prod.id);
  const verb = {
    vision: "senses people through its camera — on-device, never raw video",
    sense: "senses presence by 60 GHz radar — no camera, no mic",
    wap: "senses presence in the WiFi field itself",
    canary: "senses, witnesses, and bridges to Home Assistant",
    display: "shows",
  }[role];
  return el("p", "flash-role", `🧭 A ${prod.name} — it ${verb}.`);
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
  state.busy = true; // BEFORE the slow re-sync — a second click must bounce off
  if (!state.session && !(await ensureSession())) {
    state.busy = false;
    setPhase(errorRetry("Couldn’t reach the bootloader for the backup",
      new Error("the board didn’t re-enter download mode — unplug, replug, then Connect again"),
      phaseConnect));
    return;
  }
  const box = progressCard("Backing up your Canary", "Reading every byte off the board. Nothing is changed.");
  setPhase(box.card);
  // Name the stage so the coach deals its safety-copy lessons here too, not
  // only during an install's embedded backup step.
  box.stage("Saving a safety copy — reading every byte off the board. Nothing is changed.");
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
  card.append(el("h3", null, "Install firmware — one click"));

  const matches = core.productsForChip(state.catalog, state.chip);
  const info = core.chipInfo(state.catalog, state.chip) || {};
  card.append(el("p", "muted",
    `Pick your firmware below and press Install this. That's the whole job — ` +
    `we back the board up first, write the new firmware, and check every byte ` +
    `landed, all on their own. Only firmware built for your ` +
    `${info.label || state.chip} is shown, so you can't pick a wrong one.`));

  const manifestState = el("div", "flash-manifest-state");
  manifestState.id = "flash-manifest-state";
  card.append(manifestState);

  // Selection is detection-led (the Arduino-IDE lesson, minus the 400-board
  // dropdown): everything the flasher can READ — the chip, the measured
  // flash size, the firmware already on the board, a ?product= ask — picks
  // the ONE right card and says why in plain words. The full line hides
  // behind a click, grouped by family with one plain-language question per
  // variant axis, so ten products never read as a wall of SKUs.
  const pick = core.smartPick(state.catalog, {
    chip: state.chip,
    chipLabel: info.label,
    flashBytes: state.flashBytes,
    currentProject: state.current && !state.current.unknown
      ? state.current.projectName : null,
    preferredId: core.preferredProductId(location.search),
  });
  const focus = pick && pick.product;

  const list = el("div", "flash-products");
  const rows = [];
  const headers = [];
  const fams = core.familiesIn(state.catalog);
  const grouped = new Set();
  for (const fam of fams) {
    const inFam = matches.filter((p) => p.family === fam.id);
    if (!inFam.length) continue;
    const head = el("div", "flash-family");
    head.append(el("div", "flash-family-name", fam.name));
    head.append(el("div", "flash-family-pitch muted", fam.pitch));
    if (inFam.length > 1 && fam.pick) {
      head.append(el("div", "flash-family-pick fineprint", fam.pick));
    }
    headers.push(head);
    list.append(head);
    for (const p of inFam) {
      grouped.add(p.id);
      const r = productRow(p);
      rows.push(r);
      list.append(r);
    }
  }
  // Defensive: a product outside every family still renders (catalog drift
  // must degrade to "ungrouped", never to "invisible").
  for (const p of matches.filter((x) => !grouped.has(x.id))) {
    const r = productRow(p);
    rows.push(r);
    list.append(r);
  }
  if (!matches.length) {
    list.append(el("p", "muted", "No published SecuraCV product targets this chip yet."));
  }
  if (focus && matches.length > 1) {
    // Hide the others, but keep their rows in the DOM so refreshManifestState
    // still fills every version; one click reveals the grouped browse.
    rows.forEach((r) => { if (r.dataset.id !== focus.id) r.style.display = "none"; });
    headers.forEach((h) => { h.style.display = "none"; });
    const note = el("p", "fineprint flash-focus-note");
    note.append(el("span", null,
      (pick.kind === "picked" ? "" : "🪄 ") + pick.why + " "));
    const more = el("button", "ghost small",
      `show all ${matches.length} for this chip (developer)`);
    more.addEventListener("click", () => {
      rows.forEach((r) => { r.style.display = ""; });
      headers.forEach((h) => { h.style.display = ""; });
      note.remove();
    });
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

  // Advanced: dev channel, local file, erase toggle, skip-backup, restore.
  const adv = el("details", "flash-advanced");
  // The dev toggle below re-renders the whole picker, which would otherwise
  // snap Advanced shut under the user's cursor. Remember whether it was open.
  adv.open = !!state.advancedOpen;
  adv.addEventListener("toggle", () => { state.advancedOpen = adv.open; });
  adv.append(el("summary", null, "Advanced options — you can skip all of this"));

  // Dev channel — the same Advanced control the desktop Flasher has had
  // (desktop/src/index.html #adv-dev). Parity is not cosmetic here: the Lab
  // app renders this page in a webview with NO address bar, so `?channel=dev`
  // was unreachable for every Lab user — the dev channel existed and could
  // not be switched on. The toggle can only ever mean DEV_FLASH_MANIFEST_URL;
  // it is not a way to point at an arbitrary manifest.
  const devWrap = el("label", "flash-erase");
  const devBox = el("input");
  devBox.type = "checkbox";
  devBox.id = "flash-dev-channel";
  devBox.checked = !!state.devChannel;
  // A ?manifest= override already replaced the manifest; offering a channel
  // switch that the override would silently outrank would be a lie.
  devBox.disabled = !!state.manifestOverride;
  devBox.addEventListener("change", () => onDevChannelToggle(devBox.checked));
  devWrap.append(devBox);
  devWrap.append(el("span", null,
    " Use the dev channel — install from the rolling " +
    "fw-dev-latest prerelease instead of the pinned stable release. Dev " +
    "images are cut ahead of stable and checked exactly the same way (chip " +
    "guard, SHA-256 against the manifest, and the release signature once the " +
    "signing ceremony lands). It's also where a board shows up first: a " +
    "product with no stable release yet is often already on dev."));
  adv.append(devWrap);
  // A disabled checkbox with no reason beside it reads as a broken control.
  if (devBox.disabled) {
    adv.append(el("p", "fineprint",
      "The dev channel is unavailable while this page is pointed at a " +
      "self-hosted manifest (?manifest=). Drop that from the address to use it."));
  }

  const local = el("div", "flash-local");
  const localP = el("p", "muted",
    "Install a firmware file from your computer (a .bin you built, or one for " +
    "an air-gapped setup). We can’t check a personal file’s signature, but " +
    "the board still can’t be bricked, and we verify the write against the chip.");
  const localHelp = helpDot("local_file");
  if (localHelp) localP.append(localHelp);
  local.append(localP);
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
    "leftover data from a previous firmware. Use it if a board is misbehaving. " +
    "(On a board the flasher is meeting for the first time this happens anyway, " +
    "ticked or not.)"));
  const eraseHelp = helpDot("erase_all");
  if (eraseHelp) eraseWrap.append(eraseHelp);
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
    const skipHelp = helpDot("skip_backup");
    if (skipHelp) skipWrap.append(skipHelp);
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
  const restoreP = el("p", "muted",
    "Restore a backup file saved earlier (canary-…-backup.bin) — rewinds the " +
    "board to that exact moment, works with backups from any version.");
  const restoreHelp = helpDot("restore_backup");
  if (restoreHelp) restoreP.append(restoreHelp);
  restoreFile.append(restoreP);
  const rf = el("input");
  rf.type = "file";
  rf.accept = ".bin";
  rf.className = "flash-file";
  rf.addEventListener("change", onRestoreFile);
  restoreFile.append(rf);
  adv.append(restoreFile);
  card.append(adv);

  const displays = displaysTeaser();
  if (displays) card.append(displays);
  return card;
}

// ── the displays: boards that SHOW, previewed by their own firmware ─────────
// The display products ARE installable rows now (they ride the release train
// like everything else — build_flash_manifest.py packages all six). This
// teaser is the extra thing only a screen can offer: booting the REAL
// firmware in the browser (the same WASM build fleet.html runs) so the glass
// is seen before it exists. 1:1 — framebuffer out, touch in, LVGL and all.

function displaysTeaser() {
  // Both display hosts are ESP32-S3 boards — on other silicon the teaser
  // would only be a detour.
  if (core.normalizeChip(state.chip) !== core.normalizeChip("ESP32-S3")) return null;
  const displays = core.displaysIn(state.catalog);
  if (!displays.length) return null;
  const sec = el("details", "flash-displays");
  const sum = el("summary", null, "Building a Canary with a screen? Meet the displays");
  sec.append(sum);
  const intro = el("p", "muted",
    "Two of the family SHOW instead of sense — and both flash right here " +
    "(they’re in the picker above, chip-guarded like everything else). " +
    "Before you commit, the REAL firmware boots in your browser, 1:1: try " +
    "the glass first.");
  const hd = helpDot("display_emulator");
  if (hd) intro.append(hd);
  sec.append(intro);
  displays.forEach((d) => {
    const row = el("div", "flash-product");
    const left = el("div", "flash-product-main");
    left.append(el("div", "flash-product-name", d.name));
    left.append(el("div", "flash-product-tag muted", `${d.tagline} · ${d.panel}`));
    row.append(left);
    const btn = el("button", "primary small", "boot its screen");
    btn.addEventListener("click", () => setPhase(phaseDisplayBench(d, phaseConnected)));
    row.append(btn);
    sec.append(row);
  });
  return sec;
}

// Where the glass actually boots: fleet.html#<id> opens that display's sheet
// and powers the WASM firmware there. It does NOT boot inside flash.html on
// purpose — this page ships a deliberately strict CSP with no wasm-unsafe-eval
// (pinned by tests/flash.test.js), and weakening the flasher's policy for a
// preview would be the wrong trade. One tab away, same 1:1 truth.
function displayBenchUrl(d) {
  return `fleet.html#${encodeURIComponent(d.id)}`;
}

function phaseDisplayBench(d, back) {
  const box = el("section", "flash-card flash-displaybench");
  box.dataset.step = "2";
  box.append(el("h2", null, `${d.name} — a board that SHOWS`));
  const sub = el("p", "muted",
    `${d.tagline} Its firmware (v${d.emulator.fw_version}, LVGL ${d.emulator.lvgl}) ` +
    `is compiled to run in the browser too — real C++, real LVGL, the exact pixels ` +
    `the ${d.panel} will show, with touch. That’s the honest preview: not a mockup, ` +
    `the firmware itself.`);
  const hd = helpDot("display_emulator");
  if (hd) sub.append(hd);
  box.append(sub);

  const facts = el("div", "flash-facts");
  facts.append(fact("Board", d.board));
  facts.append(fact("Panel", d.panel));
  if (d.shows && d.shows.length) facts.append(fact("It shows", d.shows.join(" · ")));
  box.append(facts);

  box.append(el("p", "fineprint", d.build_note +
    " (The emulator boots on the fleet page — the Nursery itself runs under a " +
    "stricter security policy that deliberately can’t execute it. Flashing the " +
    "real board happens right here: it’s in the picker.)"));

  const row = el("div", "flash-row");
  const go = el("a", "primary flash-go", "boot its screen — live, 1:1 →");
  go.href = displayBenchUrl(d);
  go.target = "_blank";
  go.rel = "noopener";
  const done = el("button", "ghost", "← back");
  done.addEventListener("click", () => setPhase(back()));
  row.append(go, done);
  box.append(row);
  return box;
}

// ── the radar bench: feel a freshly-hatched Sense sensing, live ─────────────
// The moment after the flash IS the product: walk past and presence flips,
// step through the bands and the aura follows, sit statue-still and the
// clear timeout breathes out. Everything drawn here comes off the board's
// own console lines ([sense]/[presence]/[vitals] — coarse fields only, the
// same ones MQTT carries). The wellbeing senses render as a clearly-labeled
// preview on the presence-only build — what's POSSIBLE, never faked as live.

async function openConsoleBench(product, makePhase) {
  if (state.busy || state.opening) return;
  state.opening = true; // synchronous — see openMonitor
  try {
    let port = state.session && state.session.port;
    if (!port && state.voice) port = await stopVoice();
    if (state.session) {
      try { await state.session.esploader.after("hard_reset"); } catch {}
      try { await state.session.transport.disconnect(); } catch {}
      state.session = null;
    }
    if (!port) { setPhase(phaseConnect()); return; }
    setPhase(makePhase(port, product));
  } finally {
    state.opening = false;
  }
}
const openSenseBench = (product) => openConsoleBench(product, phaseSenseBench);
const openWapBench = (product) => openConsoleBench(product, phaseWapBench);

function phaseSenseBench(port, product) {
  const wellbeing = /wellbeing/.test((product && product.id) || "");
  const box = el("section", "flash-card flash-sensebench");
  box.dataset.step = "5";
  const senseH = el("h2", null, "👋 The radar bench — hold still, then don’t");
  const shd = helpDot("radar_bench");
  if (shd) senseH.append(shd);
  box.append(senseH);
  box.append(el("p", "muted",
    "This is the radar’s own senses, live off the USB cable — the same coarse " +
    "truths it publishes (present/clear, 0/1/2+, near/mid/far), never raw " +
    "centimeters unless you flip the bench-detail switch below (that raw echo " +
    "stays on this cable). Walk past it. Stand in each band. Then sit " +
    "statue-still and feel the clear timeout breathe out."));

  const status = el("div", "flash-sense-status", "listening for the radar…");
  status.setAttribute("role", "status");
  box.append(status);
  // Bench-detail readout: raw scalars, only when the raw switch is on and the
  // firmware echoes them ([radar] … raw_dist=…). Hidden otherwise.
  const rawLine = el("div", "flash-sense-rawline");
  rawLine.hidden = true;
  box.append(rawLine);
  const calm = prefersCalm(); // decorative motion politely sits out

  const aura = document.createElement("canvas");
  aura.className = "flash-sense-aura";
  aura.width = 640; aura.height = 340;
  box.append(aura);

  // The wellbeing senses: LIVE numbers on the wellbeing build, an explicit
  // possibility preview on presence-only — the user asked to SEE what this
  // hardware can feel, so show it, labeled for exactly what it is.
  const vit = el("div", "flash-sense-vitals");
  const vitHead = el("div", "flash-sense-vitals-head");
  const vitBadge = el("span", "flash-passport-chip", wellbeing ? "waiting for a lock…" : "PREVIEW — simulated");
  vitBadge.setAttribute("role", "status");
  vitHead.append(el("strong", null, "Breathing & heartbeat"), vitBadge);
  vit.append(vitHead);
  const wave = document.createElement("canvas");
  wave.className = "flash-sense-wave";
  wave.width = 640; wave.height = 110;
  vit.append(wave);
  vit.append(el("p", "fineprint", wellbeing
    ? "The Wellbeing build senses breathing (6–30 bpm) and heart rate (40–130 bpm) " +
      "by radar — no camera, no mic, no contact. Vitals lock only when exactly ONE " +
      "person is present: sit alone in the near band, settle, and watch it latch."
    : "This build watches presence only — but the SAME radar underneath can feel " +
      "breathing and heartbeat. The waves below are a simulated preview of what the " +
      "Canary Sense · Wellbeing firmware senses here; flash that build and they go live."));
  box.append(vit);

  // ── the tuning suite: every runtime knob, live over this cable ────────────
  // Sliders speak the firmware's USB tuning console (`set <knob> <value>` →
  // clamped, applied to the live FSMs, saved to NVS). The `[cfg]` snapshot
  // line is the single source of truth: the UI reconciles to it on connect
  // and after every command, so what you see is what the chip holds.
  const dials = core.reflexDials(state.catalog, product);
  const tune = el("div", "flash-sense-tune");
  const tuneHead = el("div", "flash-sense-vitals-head");
  const tuneBadge = el("span", "flash-passport-chip", "syncing with the board…");
  tuneBadge.setAttribute("role", "status");
  tuneHead.append(el("strong", null, "Tuning suite — every knob, live"), tuneBadge);
  tune.append(tuneHead);
  tune.append(el("p", "fineprint",
    "Slide a knob and it lands on the chip immediately, saved so it survives " +
    "reboots — the same numbers Home Assistant tunes later over cfg/*/set. " +
    (wellbeing
      ? "The breath/heart bands decide what the vitals lock will believe: if a " +
        "resting heart rate sits outside heart_min…heart_max, the lock politely " +
        "refuses — widen the band and watch it latch."
      : "This presence-only build carries the presence knobs; flash the " +
        "Wellbeing build to tune breathing and heart-rate bands too.")));
  const knobEls = new Map(); // console token -> {input, val, unit}
  const knobGrid = el("div", "flash-sense-knobs");
  for (const k of (dials && dials.knobs) || []) {
    if (!k.console) continue;
    const row = el("label", "flash-sense-knob");
    row.append(el("span", "flash-sense-knob-name", k.console));
    const input = document.createElement("input");
    input.type = "range";
    input.min = String(k.bounds[0]);
    input.max = String(k.bounds[1]);
    input.step = k.unit === "cm" ? "10" : k.unit === "bpm" ? "1" : "50";
    input.value = String(k.value);
    input.disabled = true; // enabled on the first [cfg] sync
    const val = el("span", "flash-sense-knob-val", `${k.value} ${k.unit}`);
    input.addEventListener("input", () => { val.textContent = `${input.value} ${k.unit}`; });
    input.addEventListener("change", () => sendCmd(`set ${k.console} ${input.value}`));
    row.append(input, val);
    row.title = `${k.id} — default ${k.value} ${k.unit}, range ${k.bounds[0]}–${k.bounds[1]}`;
    knobGrid.append(row);
    knobEls.set(k.console, { input, val, unit: k.unit });
  }
  tune.append(knobGrid);

  const tunectl = el("div", "flash-row flash-sense-tunectl");
  const resetBtn = el("button", "ghost", "↺ restore defaults");
  resetBtn.disabled = true;
  resetBtn.addEventListener("click", () => sendCmd("reset"));
  const streamSel = document.createElement("select");
  streamSel.className = "flash-sense-streamsel";
  [["500", "stream: 2×/s"], ["1000", "stream: 1×/s"], ["2000", "stream: every 2 s"], ["0", "stream: off"]]
    .forEach(([v, label]) => {
      const o = document.createElement("option");
      o.value = v; o.textContent = label;
      streamSel.append(o);
    });
  streamSel.value = "1000";
  streamSel.disabled = true;
  streamSel.addEventListener("change", () =>
    sendCmd(streamSel.value === "0" ? "stream off" : `stream ${streamSel.value}`));
  const rawLab = el("label", "flash-sense-rawtoggle");
  const rawChk = document.createElement("input");
  rawChk.type = "checkbox";
  rawChk.disabled = true;
  rawChk.addEventListener("change", () => sendCmd(rawChk.checked ? "raw on" : "raw off"));
  rawLab.append(rawChk, document.createTextNode(" bench detail (raw cm & bpm — stays on this cable)"));
  tunectl.append(resetBtn, streamSel, rawLab);
  tune.append(tunectl);
  box.append(tune);

  const script = el("div", "flash-nextstep");
  script.append(el("div", "flash-nextstep-title", "Make it magic — the 60-second tour"));
  const ol = el("ol", "we2-guide-steps");
  [
    "Walk past it — presence flips to “someone’s here” within about a second, and the aura lights.",
    "Step closer, then away: near → mid → far, the ring follows you.",
    "Bring a friend — the count climbs to 2+ (and vitals politely refuse: one person only).",
    wellbeing ? "Sit alone, still, a couple of meters away — breathing locks, then the numbers appear."
              : "Sit statue-still — presence holds through the debounce, then clears when you leave.",
    "Everything you just felt publishes the same way to Home Assistant — this bench is the radar’s honest voice.",
  ].forEach((s) => ol.append(el("li", null, s)));
  script.append(ol);
  box.append(script);

  // The live console — classified line by line so it reads at a glance
  // ([radar] stream stays quiet, [tune] verdicts pop, errors glow). Open by
  // default: seeing what the board says IS the bench.
  const conWrap = el("details", "flash-displaybench-serial");
  conWrap.open = true;
  conWrap.append(el("summary", null, "the live console — every line the radar speaks"));
  const conCtl = el("div", "flash-row flash-sense-logctl");
  const pauseBtn = el("button", "ghost", "⏸ hold the scroll");
  let paused = false;
  pauseBtn.addEventListener("click", () => {
    paused = !paused;
    pauseBtn.textContent = paused ? "▶ follow again" : "⏸ hold the scroll";
  });
  const clearBtn = el("button", "ghost", "✕ clear");
  conCtl.append(pauseBtn, clearBtn);
  conWrap.append(conCtl);
  const con = el("div", "flash-console flash-sense-log");
  clearBtn.addEventListener("click", () => { con.textContent = ""; });
  conWrap.append(con);
  box.append(conWrap);

  function logLine(text) {
    if (!text.trim()) return;
    con.append(el("div", "flash-senseline tone-" + core.senseLineTone(text), text));
    while (con.childElementCount > 400) con.firstElementChild.remove();
    if (!paused) con.scrollTop = con.scrollHeight;
  }

  const row = el("div", "flash-row");
  const back = el("button", "ghost", "← back to the Nursery");
  row.append(back);
  const senseTwin = twinLink(product);
  if (senseTwin) row.append(senseTwin);
  // The full post-flash journey — drills, zone calibration, placement
  // scoring — lives on its own bench (works with this same cable, or the
  // emulated twin when the board stays in its box).
  const proving = el("a", "ghost small flash-twin", "🎯 the Proving Ground — drills, calibration, placement");
  proving.href = "radar-dev.html";
  row.append(proving);
  box.append(row);

  // ── the live model, fed by the console lines ──
  const model = {
    presence: "unknown", count: "0", range: "unknown",
    breath: null, heart: null, locked: false,
    lastSeenMs: 0, frameErrs: null, alive: true,
  };

  function setStatus() {
    const p = model.presence;
    status.className = "flash-sense-status flash-sense-" + p;
    status.textContent =
      p === "present"
        ? `● someone’s here — ${model.count === "2+" ? "two or more" : model.count === "1" ? "one person" : "movement"} · ${model.range}`
        : p === "clear" ? "○ clear — the room is empty"
        : "◌ listening for the radar…";
  }

  // reader loop (the voice engine pattern — attended bench, no supervisor)
  // plus a writer: the bench now TALKS to the tuning console (cfg/set/reset/
  // stream/raw). The `[cfg]` reply is the sync point for every control.
  let reader = null;
  let writer = null;
  let synced = false;
  const enc = new TextEncoder();
  async function sendCmd(cmd) {
    if (!writer) return;
    try { await writer.write(enc.encode(cmd + "\n")); } catch { /* mid-unplug */ }
  }

  function syncFromCfg(cfg) {
    synced = true;
    for (const [name, ui] of knobEls) {
      if (!Number.isFinite(cfg.values[name])) continue;
      ui.input.value = String(cfg.values[name]);
      ui.val.textContent = `${cfg.values[name]} ${ui.unit}`;
      ui.input.disabled = false;
    }
    resetBtn.disabled = false;
    streamSel.disabled = false;
    rawChk.disabled = false;
    if (Number.isFinite(cfg.stream)) {
      const v = String(cfg.stream);
      streamSel.value = [...streamSel.options].some((o) => o.value === v) ? v : "1000";
    }
    if (typeof cfg.raw === "boolean") rawChk.checked = cfg.raw;
    tuneBadge.textContent = "LIVE — knobs synced with the chip";
    tuneBadge.className = "flash-passport-chip flash-passport-ok";
  }

  (async () => {
    try {
      await port.open({ baudRate: state.catalog.console_baud || 115200 });
      if (!model.alive) { try { await port.close(); } catch {} return; } // back won the race
      reader = port.readable.getReader();
      try { writer = port.writable.getWriter(); } catch { writer = null; }
      if (!model.alive) {
        try { reader.releaseLock(); } catch {}
        try { writer && writer.releaseLock(); } catch {}
        try { await port.close(); } catch {}
        return;
      }
    } catch (e) {
      status.textContent = "The console didn’t open (" + String(e.message || e) + ") — unplug, replug, reconnect.";
      return;
    }
    // Handshake: ask for the knob snapshot (twice — the board may still be
    // booting), then be honest if this firmware predates the console.
    setTimeout(() => { if (model.alive && !synced) sendCmd("cfg"); }, 600);
    setTimeout(() => { if (model.alive && !synced) sendCmd("cfg"); }, 2500);
    setTimeout(() => {
      if (model.alive && !synced) {
        tuneBadge.textContent = "no tuning console — reflash with the latest firmware to tune live";
        tuneBadge.className = "flash-passport-chip flash-passport-warn";
      }
    }, 5000);
    const dec = new TextDecoder();
    let tail = "";
    try {
      for (;;) {
        const { value, done } = await reader.read();
        if (done || !model.alive) break;
        const text = dec.decode(value, { stream: true });
        tail += text;
        const lines = tail.split("\n");
        tail = lines.pop() || "";
        for (const line of lines) {
          logLine(line);
          const cfg = core.parseCfgLine(line);
          if (cfg) { syncFromCfg(cfg); continue; }
          const verdict = core.parseTuneLine(line);
          if (verdict) {
            tuneBadge.textContent = (verdict.ok ? "✓ " : "⚠ ") + verdict.text;
            tuneBadge.className = "flash-passport-chip " +
              (verdict.ok ? "flash-passport-ok" : "flash-passport-warn");
            continue;
          }
          const ev = core.parseSenseLine(line);
          if (!ev) continue;
          model.lastSeenMs = performance.now();
          if (ev.kind === "sense") {
            if (ev.presence === "present" && model.presence !== "present") chirp("found");
            model.presence = ev.presence; model.count = ev.count; model.range = ev.range;
          } else if (ev.kind === "presence") {
            if (ev.presence === "present" && model.presence !== "present") chirp("found");
            model.presence = ev.presence;
          } else if (ev.kind === "bpm") {
            model.breath = ev.breath; model.heart = ev.heart; model.locked = true;
            vitBadge.textContent = `LIVE · breathing ${ev.breath} bpm · heart ${ev.heart} bpm`;
            vitBadge.className = "flash-passport-chip flash-passport-ok";
          } else if (ev.kind === "vitals") {
            model.locked = ev.locked;
            if (!ev.locked && wellbeing) {
              vitBadge.textContent = "lock lost — settle again";
              vitBadge.className = "flash-passport-chip";
            }
          } else if (ev.kind === "health") {
            model.frameErrs = ev.frame_errs;
          } else if (ev.kind === "radar") {
            // The tuning console's periodic stream: the bench stays alive
            // even between change-gated lines — presence, bands, lock and
            // (wellbeing) live BPM all ride it.
            if (ev.presence === "present" && model.presence !== "present") chirp("found");
            model.presence = ev.presence; model.count = ev.count; model.range = ev.range;
            if (ev.lock) {
              const locked = ev.lock === "locked";
              if (locked && Number.isFinite(ev.breath) && ev.breath > 0) {
                model.breath = ev.breath; model.heart = ev.heart; model.locked = true;
                vitBadge.textContent = `LIVE · breathing ${ev.breath} bpm · heart ${ev.heart} bpm`;
                vitBadge.className = "flash-passport-chip flash-passport-ok";
              } else if (!locked && model.locked && wellbeing) {
                model.locked = false;
                vitBadge.textContent = ev.lock === "lost" ? "lock lost — settle again" : "waiting for a lock…";
                vitBadge.className = "flash-passport-chip";
              }
            }
            if (ev.raw) {
              rawLine.hidden = false;
              rawLine.textContent =
                `bench detail · distance ${ev.raw.dist_cm} cm · targets ${ev.raw.count}` +
                (wellbeing ? ` · raw breath ${ev.raw.breath} · raw heart ${ev.raw.heart} bpm` : "");
            } else if (!rawChk.checked) {
              rawLine.hidden = true;
            }
            if (Number.isFinite(ev.frame_errs)) model.frameErrs = ev.frame_errs;
          }
          setStatus();
        }
      }
    } catch { /* unplugged — the back button is right there */ }
  })();

  // ── the aura: bands, sweep, and the target ──
  const actx = aura.getContext("2d");
  const wctx = wave.getContext("2d");
  let raf = 0, t0 = performance.now();
  const BAND_R = { near: 0.36, mid: 0.62, far: 0.88, unknown: 0.62 };
  let targetR = 0, targetA = 0; // eased radius/alpha of the presence blob

  function draw(now) {
    if (!model.alive) return;
    // Under prefers-reduced-motion the informative state still renders and
    // updates — only the decorative time-based motion freezes. The first rAF
    // timestamp can precede t0 — clamp, or sweepT % 1 goes negative and
    // arc() throws on the negative radius.
    const t = calm ? 0 : Math.max(0, now - t0) / 1000;
    const W = aura.width, H = aura.height;
    const cx = W / 2, cy = H - 14, R = H - 40;
    actx.clearRect(0, 0, W, H);
    const present = model.presence === "present";
    const stale = model.lastSeenMs && now - model.lastSeenMs > 15000;

    // band arcs
    for (const [name, r] of [["near", BAND_R.near], ["mid", BAND_R.mid], ["far", BAND_R.far]]) {
      const active = present && model.range === name;
      actx.beginPath();
      actx.arc(cx, cy, r * R, Math.PI, 2 * Math.PI);
      actx.lineWidth = active ? 5 : 1.5;
      actx.strokeStyle = active
        ? `hsl(140 90% 62% / ${0.75 + 0.25 * Math.sin(t * 4)})`
        : "rgba(160,180,200,0.25)";
      actx.stroke();
      actx.font = "600 11px ui-monospace, Menlo, monospace";
      actx.fillStyle = active ? "#7CFF9B" : "rgba(160,180,200,0.5)";
      actx.fillText(name, cx + r * R - 26, cy - 8);
    }
    // the sweep — a quiet metronome when clear, eager when someone's there
    const sweepT = (t * (present ? 0.55 : 0.22)) % 1;
    const sweepR = sweepT * R;
    actx.beginPath();
    actx.arc(cx, cy, sweepR, Math.PI, 2 * Math.PI);
    actx.lineWidth = 2;
    actx.strokeStyle = `hsl(${present ? 140 : 205} 80% 60% / ${0.5 * (1 - sweepT)})`;
    actx.stroke();
    // the emitter
    actx.beginPath();
    actx.arc(cx, cy, 6, 0, 2 * Math.PI);
    actx.fillStyle = stale ? "#f0a860" : present ? "#7CFF9B" : "#7db8e8";
    actx.fill();
    // the target blob — eases into its band, breathes gently
    const wantR = present ? (BAND_R[model.range] || BAND_R.mid) * R : 0;
    targetR += (wantR - targetR) * 0.08;
    targetA += ((present ? 1 : 0) - targetA) * 0.1;
    if (targetA > 0.02) {
      const bob = Math.sin(t * 2.2) * 4;
      const tx = cx, ty = cy - targetR + bob;
      const glow = actx.createRadialGradient(tx, ty, 2, tx, ty, 26);
      glow.addColorStop(0, `hsl(140 90% 66% / ${0.9 * targetA})`);
      glow.addColorStop(1, "hsl(140 90% 66% / 0)");
      actx.fillStyle = glow;
      actx.beginPath(); actx.arc(tx, ty, 26, 0, 2 * Math.PI); actx.fill();
      actx.fillStyle = `hsl(140 90% 70% / ${targetA})`;
      actx.beginPath(); actx.arc(tx, ty, 7, 0, 2 * Math.PI); actx.fill();
      if (model.count === "2+") {
        actx.fillStyle = `hsl(140 90% 70% / ${0.8 * targetA})`;
        actx.beginPath(); actx.arc(tx + 22, ty + 6, 5, 0, 2 * Math.PI); actx.fill();
        actx.font = "700 12px ui-monospace, Menlo, monospace";
        actx.fillText("2+", tx + 32, ty + 10);
      }
    }
    if (stale) {
      actx.font = "600 12px ui-monospace, Menlo, monospace";
      actx.fillStyle = "#f0a860";
      actx.fillText("no radar lines lately — is it still plugged in?", 14, 18);
    }

    // ── the vitals waves ──
    const WW = wave.width, WH = wave.height;
    wctx.clearRect(0, 0, WW, WH);
    const live = wellbeing && model.locked && model.breath;
    const breathBpm = live ? model.breath : 14;
    const heartBpm = live ? model.heart : 64;
    const alpha = live ? 0.95 : 0.5;
    // breath: a slow full-height sine
    wctx.beginPath();
    for (let x = 0; x < WW; x++) {
      const ph = (t - (WW - x) / 90) * (breathBpm / 60) * 2 * Math.PI;
      const y = WH * 0.32 - Math.sin(ph) * WH * 0.2;
      x ? wctx.lineTo(x, y) : wctx.moveTo(x, y);
    }
    wctx.lineWidth = 2.5;
    wctx.strokeStyle = `hsl(175 80% 60% / ${alpha})`;
    wctx.stroke();
    // heart: a sharp pulse train
    wctx.beginPath();
    for (let x = 0; x < WW; x++) {
      const ph = ((t - (WW - x) / 90) * (heartBpm / 60)) % 1;
      const spike = ph < 0.08 ? Math.sin((ph / 0.08) * Math.PI) : ph < 0.16 ? -0.35 * Math.sin(((ph - 0.08) / 0.08) * Math.PI) : 0;
      const y = WH * 0.78 - spike * WH * 0.17;
      x ? wctx.lineTo(x, y) : wctx.moveTo(x, y);
    }
    wctx.lineWidth = 2;
    wctx.strokeStyle = `hsl(345 85% 64% / ${alpha})`;
    wctx.stroke();
    wctx.font = "600 11px ui-monospace, Menlo, monospace";
    wctx.fillStyle = `hsl(175 80% 60% / ${alpha})`;
    wctx.fillText(`breath ${live ? model.breath : "~" + breathBpm} bpm`, 10, 16);
    wctx.fillStyle = `hsl(345 85% 64% / ${alpha})`;
    wctx.fillText(`heart ${live ? model.heart : "~" + heartBpm} bpm`, 10, WH - 10);

    raf = requestAnimationFrame(draw);
  }
  raf = requestAnimationFrame(draw);
  setStatus();

  back.addEventListener("click", async () => {
    model.alive = false;
    cancelAnimationFrame(raf);
    try { reader && await reader.cancel(); } catch {}
    try { reader && reader.releaseLock(); } catch {}
    try { writer && writer.releaseLock(); } catch {}
    try { await port.close(); } catch {}
    setPhase(phaseConnect());
  });
  return box;
}

// ── the field bench: feel a freshly-hatched WAP feel the room ───────────────
// The WAP senses presence in the WiFi field itself — no camera, no mic, no
// radar module. Its console prints one coarse line per RF transition
// ([wap] … devices/confidence/dwell/stir); the bench turns that into a
// living field: ripples on every event, a stir meter for the CSI motion
// score, and the honest vocabulary underneath. Same attended-USB posture
// as the radar bench.
function phaseWapBench(port, product) {
  const box = el("section", "flash-card flash-sensebench");
  box.dataset.step = "5";
  const wapH = el("h2", null, "🌊 The field bench — the room’s WiFi, felt");
  const whd2 = helpDot("field_bench");
  if (whd2) wapH.append(whd2);
  box.append(wapH);
  box.append(el("p", "muted",
    "This Canary reads the WiFi field itself: phones announcing themselves, " +
    "and the way a moving body stirs the radio reflections (CSI). Below is " +
    "its live verdict off the USB cable — the same coarse vocabulary it " +
    "publishes: an event name, a device count, a confidence word, a dwell " +
    "class. Never a MAC, never a raw signal."));

  const status = el("div", "flash-sense-status", "listening for the field…");
  status.setAttribute("role", "status");
  box.append(status);
  const calm = prefersCalm();

  const field = document.createElement("canvas");
  field.className = "flash-sense-aura";
  field.width = 640; field.height = 300;
  box.append(field);

  // The stir meter: the CSI motion score, decaying between events.
  const meter = el("div", "we2-meter");
  const track = el("div", "we2-meter-track");
  const fill = el("div", "we2-meter-fill");
  track.append(fill);
  const meterLabel = el("div", "we2-meter-label", "field stir — how much a body is moving the radio");
  meter.append(track, meterLabel);
  box.append(meter);

  const script = el("div", "flash-nextstep");
  script.append(el("div", "flash-nextstep-title", "Make it magic — the 60-second tour"));
  const ol = el("ol", "we2-guide-steps");
  [
    "Walk into the room with your phone in your pocket — an arrival event fires and the field ripples.",
    "Leave the phone outside and walk back in: the CSI stir meter still moves — the field feels the BODY, not the phone.",
    "Stand statue-still and watch the stir settle; linger a couple of minutes and the dwell class climbs to “sustained”.",
    "Bring a second phone — the device count follows, as a count, never an identity.",
    "Everything here publishes the same way to Home Assistant — this bench is the field’s honest voice.",
  ].forEach((s) => ol.append(el("li", null, s)));
  script.append(ol);
  box.append(script);

  const conWrap = el("details", "flash-displaybench-serial");
  conWrap.append(el("summary", null, "the raw console under the magic"));
  const con = el("pre", "flash-console");
  conWrap.append(con);
  box.append(conWrap);

  const row = el("div", "flash-row");
  const back = el("button", "ghost", "← back to the Nursery");
  row.append(back);
  const wapTwin = twinLink(product);
  if (wapTwin) row.append(wapTwin);
  box.append(row);

  const model = {
    present: false, devices: 0, confidence: null, dwell: null,
    stir: 0, lastEvent: null, lastSeenMs: 0, alive: true,
  };
  const ripples = []; // {born, strength}

  function setStatus() {
    status.className = "flash-sense-status " +
      (model.present ? "flash-sense-present" : "flash-sense-clear");
    status.textContent = model.present
      ? `● someone stirs the field — ${model.devices} device${model.devices === 1 ? "" : "s"} heard` +
        `${model.confidence ? ` · ${model.confidence} confidence` : ""}${model.dwell ? ` · ${model.dwell}` : ""}`
      : model.lastEvent ? "○ the field is calm again"
      : "◌ listening for the field…";
  }

  let reader = null;
  (async () => {
    try {
      await port.open({ baudRate: state.catalog.console_baud || 115200 });
      if (!model.alive) { try { await port.close(); } catch {} return; } // back won the race
      reader = port.readable.getReader();
      if (!model.alive) {
        try { reader.releaseLock(); } catch {}
        try { await port.close(); } catch {}
        return;
      }
    } catch (e) {
      status.textContent = "The console didn’t open (" + String(e.message || e) + ") — unplug, replug, reconnect.";
      return;
    }
    const dec = new TextDecoder();
    let buf = "", tail = "";
    try {
      for (;;) {
        const { value, done } = await reader.read();
        if (done || !model.alive) break;
        const text = dec.decode(value, { stream: true });
        buf = (buf + text).slice(-8000);
        con.textContent = buf;
        con.scrollTop = con.scrollHeight;
        tail += text;
        const lines = tail.split("\n");
        tail = lines.pop() || "";
        for (const line of lines) {
          const ev = core.parseWapLine(line);
          if (!ev) continue;
          model.lastSeenMs = performance.now();
          model.lastEvent = ev.event;
          model.devices = ev.devices;
          model.confidence = ev.confidence;
          model.dwell = ev.dwell;
          model.stir = Math.max(model.stir, ev.stir);
          if (ev.present && !model.present) chirp("found");
          if (ev.present) model.present = true;
          if (ev.departed) model.present = false;
          ripples.push({ born: performance.now(), strength: Math.max(0.35, ev.stir / 100) });
          if (ripples.length > 8) ripples.shift();
          setStatus();
        }
      }
    } catch { /* unplugged — the back button is right there */ }
  })();

  const fctx = field.getContext("2d");
  let raf = 0, t0 = performance.now(), lastFrame = t0;
  function draw(now) {
    if (!model.alive) return;
    // dt-based decay: the stir reading is INFORMATIVE, so it must drain at
    // the same rate on every display — 60 Hz, 144 Hz, or reduced-motion.
    const dt = Math.min(0.1, Math.max(0, (now - lastFrame) / 1000));
    lastFrame = now;
    const t = calm ? 0 : (now - t0) / 1000;
    const W = field.width, H = field.height, cx = W / 2, cy = H / 2;
    fctx.clearRect(0, 0, W, H);
    // the ambient field: faint standing rings, breathing very slightly
    for (let i = 1; i <= 4; i++) {
      fctx.beginPath();
      fctx.arc(cx, cy, i * 34 + Math.sin(t * 0.8 + i) * 2, 0, 2 * Math.PI);
      fctx.lineWidth = 1;
      fctx.strokeStyle = `hsl(205 60% 55% / ${model.present ? 0.18 : 0.1})`;
      fctx.stroke();
    }
    // event ripples: expanding, fading, strength-scaled
    const nowMs = performance.now();
    for (const r of ripples) {
      const age = (nowMs - r.born) / 1000;
      if (age > 3) continue;
      const p = calm ? 0.6 : age / 3;
      fctx.beginPath();
      fctx.arc(cx, cy, 20 + p * (H / 2 - 10), 0, 2 * Math.PI);
      fctx.lineWidth = 2.5 * r.strength;
      fctx.strokeStyle = `hsl(${model.present ? 140 : 205} 85% 62% / ${(1 - p) * 0.8 * r.strength})`;
      fctx.stroke();
    }
    // the emitter
    fctx.beginPath();
    fctx.arc(cx, cy, 6, 0, 2 * Math.PI);
    fctx.fillStyle = model.present ? "#7CFF9B" : "#7db8e8";
    fctx.fill();
    // device count chips orbiting when present
    if (model.present && model.devices > 0) {
      for (let i = 0; i < Math.min(model.devices, 5); i++) {
        const a = (i / Math.min(model.devices, 5)) * 2 * Math.PI + t * 0.4;
        fctx.beginPath();
        fctx.arc(cx + Math.cos(a) * 60, cy + Math.sin(a) * 60, 4, 0, 2 * Math.PI);
        fctx.fillStyle = "hsl(140 90% 70% / 0.9)";
        fctx.fill();
      }
    }
    const stale = model.lastSeenMs && nowMs - model.lastSeenMs > 30000;
    if (stale) {
      fctx.font = "600 12px ui-monospace, Menlo, monospace";
      fctx.fillStyle = "#f0a860";
      fctx.fillText("no field lines lately — the WAP only speaks on transitions; walk past it", 14, 18);
    }
    // the stir meter, decaying gently between events (~9%/second everywhere)
    model.stir = Math.max(0, model.stir - 9 * dt);
    fill.style.width = Math.round(model.stir) + "%";
    fill.dataset.level = model.stir >= 55 ? "ok" : model.stir >= 25 ? "soft" : "faint";
    raf = requestAnimationFrame(draw);
  }
  raf = requestAnimationFrame(draw);
  setStatus();

  back.addEventListener("click", async () => {
    model.alive = false;
    cancelAnimationFrame(raf);
    try { reader && await reader.cancel(); } catch {}
    try { reader && reader.releaseLock(); } catch {}
    try { await port.close(); } catch {}
    setPhase(phaseConnect());
  });
  return box;
}

/* The picker slot: the board's own figure when the catalog carries one, a
 * neutral placeholder when it doesn't (docs/design/FLEET_FIGURES.md).
 *
 * Every row gets a slot of the SAME size either way. That is the whole trick:
 * the list never reflows as figures land for more of the fleet — a
 * placeholder simply becomes a drawing. The words stay primary; the figure is
 * there so you can recognize the board in your hand without parsing a model
 * number, which is the moment this picker actually gets used.
 *
 * The placeholder deliberately does NOT resemble any product. Drawing a
 * plausible-looking generic board would be worse than drawing nothing: the
 * one failure this whole system exists to prevent is somebody matching their
 * hardware against a picture of different hardware.
 */
function figureSlot(p) {
  const slot = el("div", "flash-fig");
  const f = p.figure;
  if (f && f.svg) {
    slot.innerHTML = f.svg;
    slot.title = f.shared
      ? `${f.title} — this board is also built as another product`
      : f.title;
    return slot;
  }
  slot.classList.add("is-placeholder");
  slot.innerHTML =
    '<svg viewBox="0 0 64 64" aria-hidden="true">' +
    '<rect x="12" y="18" width="40" height="28" rx="3"/>' +
    '<rect class="c" x="20" y="26" width="24" height="12" rx="1.5"/></svg>';
  slot.append(el("span", null, (p.chip || "").replace("ESP32-", "")));
  slot.title = "No drawing for this board yet";
  return slot;
}

function productRow(p) {
  const row = el("div", "flash-product");
  row.dataset.id = p.id;
  row.append(figureSlot(p));
  const left = el("div", "flash-product-main");
  left.append(el("div", "flash-product-name", p.name));
  left.append(el("div", "flash-product-tag muted", p.tagline));
  if (p.figure && p.figure.shared) {
    // One board, two flashable products. Say it rather than let the drawing
    // imply this row is the only thing that board becomes.
    left.append(el("div", "flash-product-note",
      "This board is also sold as another product — check the name, not just the picture."));
  }
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
  // manifestOverrideUrl); it outranks the channel. Otherwise the channel
  // decides: state.devChannel (seeded by `?channel=dev`, then owned by the
  // Advanced toggle) reads the rolling dev prerelease — a fixed first-party
  // URL, never user-supplied — and the default is the stable release.
  const override = core.manifestOverrideUrl(location.search, location.origin);
  state.manifestOverride = !!override;
  if (override) { state.devChannel = false; return override; }
  return state.devChannel ? core.DEV_FLASH_MANIFEST_URL : state.catalog.manifest_url;
}

// Every manifest fetch carries a generation stamp. Switching channels starts a
// second fetch without canceling the first, and the two can land in either
// order — a slow stable response arriving after a fast dev one would repaint
// the picker with the versions, SHA-256s and Install targets of the channel the
// UI says is OFF. That is the silent-wrong case this whole toggle exists to
// avoid, so a late response from a superseded generation is discarded outright.
let manifestGeneration = 0;

function ensureManifest() {
  if (state.manifest) { refreshManifestState(); return; }
  const gen = ++manifestGeneration;
  const stale = () => gen !== manifestGeneration;
  fetch(activeManifestUrl(), { cache: "no-store" })
    .then((r) => (r.ok ? r.json() : Promise.reject(new Error("no release manifest (HTTP " + r.status + ")"))))
    .then((m) => {
      if (stale()) return;
      const errs = core.validateManifest(m);
      state.manifest = errs.length ? { __invalid: errs } : m;
    })
    // Keep WHY it's missing. "No release yet" and "the release this page is
    // pinned to was never cut" look identical to a user otherwise, and the
    // second one is a maintainer bug that hid for a whole release cycle.
    .catch((err) => {
      if (stale()) return;
      state.manifest = { __missing: true, why: String((err && err.message) || err) };
    })
    .finally(() => { if (!stale()) refreshManifestState(); });
}

// Advanced → dev channel. Switching channels invalidates the loaded manifest
// wholesale (versions, SHA-256s and availability all belong to the OTHER
// release), so drop it and re-render the picker from scratch rather than
// leaving stale versions on rows the new channel may not even carry.
function onDevChannelToggle(on) {
  if (state.busy) return;              // never re-point mid-write
  state.devChannel = !!on;
  state.manifest = null;
  // Bump the generation before re-rendering: an in-flight fetch for the
  // previous channel is now stale, and must not repaint over the new one
  // whichever order the two responses arrive in (see ensureManifest).
  manifestGeneration++;
  setPhase(phaseConnected());          // repaints with "Checking…" in the banner
  ensureManifest();
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
      : state.devChannel
        ? "The dev channel has nothing published yet — no rolling prerelease has been cut. Turn the dev channel off under Advanced for the stable release, or install a local file."
        : "No signed firmware release is published yet. When the maintainer cuts one, the official images appear here automatically. Until then, try Advanced → dev channel (products often land there first), or install a local file.";
    banner.append(note);
    // Name the release we were pinned to. Every product reading "unavailable"
    // because a tag was bumped but never released is indistinguishable from
    // "no releases exist" without this line.
    const pinned = m.__missing && core.releaseTagFromManifestUrl(activeManifestUrl());
    if (pinned) {
      banner.append(el("p", "fineprint",
        `This page is pinned to firmware release ${pinned}` +
        (m.why ? ` — ${m.why}.` : ".") +
        " If that release exists, the images will appear on reload."));
    }
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
    // Say what's actually true of THESE images. Claiming "signed" while the
    // signing ceremony hasn't happened would be the one lie this banner exists
    // to prevent — the per-image line below reports the real verification.
    note.textContent = core.isRealPubkey(state.catalog.release_pubkey)
      ? "DEV CHANNEL — these images come from the rolling dev prerelease, signed with the same key but not yet promoted to stable. Turn it off under Advanced to go back to release firmware."
      : "DEV CHANNEL — these images come from the rolling dev prerelease, ahead of stable. No release signing key is in force yet, so they're verified by SHA-256 against the manifest, not by signature. Turn it off under Advanced to go back to release firmware.";
    banner.append(note);
  }
  // The headline answer, before any list: should THIS board be updated?
  // Compares what the board runs against the release for that same product.
  if (state.current && !state.current.unknown) {
    const curProd = core.matchProjectToProduct(state.catalog, state.current.projectName);
    const entry = curProd && core.manifestEntry(m, curProd, state.chip);
    if (entry && !entry.error) {
      const v = installVerdictFor(curProd, entry.version);
      const text =
        v.kind === "update"
          ? `⬆ An update is waiting — ${curProd.name} ${state.current.version} → ${entry.version}. One click below installs it.`
          : v.kind === "same"
            ? `✓ Up to date — this board already runs the latest ${curProd.name} (${entry.version}).`
            : v.kind === "downgrade"
              ? `This board runs ${curProd.name} ${state.current.version} — newer than the published ${entry.version}. Probably a dev build; installing the release would be a downgrade.`
              : null;
      if (text) {
        banner.append(el("p", `flash-headline flash-verdict-line flash-verdict-${v.kind}`, text));
      }
    }
  }

  // Fill versions + enable buttons for available products.
  document.querySelectorAll(".flash-product").forEach((row) => {
    const id = row.dataset.id;
    const product = state.catalog.products.find((p) => p.id === id);
    const entry = core.manifestEntry(m, product, state.chip);
    const verEl = row.querySelector('[data-for="' + id + '"].flash-product-ver, .flash-product-ver');
    const btn = row.querySelector(".flash-pick");
    if (entry && !entry.error) {
      if (verEl) {
        verEl.textContent = `v${entry.version} · ${core.formatBytes(entry.size)}`;
        // The verdict chip: what would installing THIS mean for THIS board —
        // an update, a downgrade, a reinstall, a role switch. Said up front,
        // not discovered after.
        const v = installVerdictFor(product, entry.version);
        if (v && v.kind !== "fresh" && v.kind !== "unknown") {
          verEl.append(" ", el("span", `flash-verdict flash-verdict-${v.kind}`, `${v.icon} ${v.label}`));
        }
      }
      if (btn) btn.disabled = false;
    } else {
      if (verEl) verEl.textContent = "not in this release";
      if (btn) { btn.disabled = true; btn.textContent = "unavailable"; }
    }
  });
}

// ── pick → confirm → flash ──────────────────────────────────────────────────
// The verdict for installing `product` at `version` on the board in hand.
function installVerdictFor(product, version) {
  const currentProduct = state.current && !state.current.unknown
    ? core.matchProjectToProduct(state.catalog, state.current.projectName)
    : null;
  return core.installVerdict({ current: state.current, currentProduct, product, version });
}

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
  // Factory-shape gate (core.localImageShape): everything here is written at
  // offset 0, so an app-only build would land on the bootloader and the
  // board wouldn't boot until a USB re-flash. Same refusal, same words, as
  // the desktop Flasher.
  const shape = core.localImageShape(bytes);
  if (!shape.factory) {
    ev.target.value = ""; // let the corrected file re-fire the picker
    setPhase(errorRetry("That file isn’t a factory image",
      new Error(`${shape.reason}. The flasher writes whole factory images at offset 0, so an app-only .bin would overwrite the bootloader and the board wouldn't boot. Merge one with firmware/scripts/make_factory.py or use dev_flash.sh <env> -f.`),
      phaseConnected));
    return;
  }
  // Advanced → local file goes straight to startFlash without passing through
  // phaseConfirm, so it has to apply the first-contact erase itself. It is
  // exactly the same board and exactly the same leftover partitions; picking
  // your own .bin isn't a statement about the board's history.
  startFlash({ localBytes: bytes, label: file.name, isLocal: true, skipBackup: skip,
    eraseAll: !!(state.intake && state.intake.firstContact) });
}

function phaseConfirm(product, entry) {
  // Read the Advanced toggles while the picker is still in the DOM.
  const skipBackup = $("#flash-skip-backup") && $("#flash-skip-backup").checked;
  // First contact with a board forces the full erase, toggle or no toggle. A
  // normal install writes only the regions it needs, so anything a previous
  // owner left in a partition we don't touch would ride straight through onto
  // a board the user now believes is theirs. This is the one case where the
  // Advanced checkbox isn't the user's to decide.
  const forcedErase = !!(state.intake && state.intake.firstContact);
  const eraseOn = forcedErase || !!($("#flash-erase-all") && $("#flash-erase-all").checked);

  const box = el("section", "flash-card flash-confirm");
  box.dataset.step = "3";

  // Customs said stop. Every "stop" is an eFuse a previous owner burned, and
  // eFuses only burn one way — so this is a dead end, not a warning to click
  // past. Say what it is and offer the way back, not a disabled button with
  // no explanation.
  if (state.intake && state.intake.verdict.level === "stop") {
    box.classList.add("flash-confirm-blocked");
    box.append(el("h2", null, "This board can't take firmware"));
    for (const f of state.intake.verdict.findings.filter((x) => x.level === "stop")) {
      const row = el("div", "flash-intake-finding flash-intake-stop");
      row.append(el("strong", null, f.label));
      if (f.detail) row.append(el("span", "flash-intake-detail", f.detail));
      box.append(row);
    }
    box.append(el("p", "flash-intake-verdict",
      "Nothing this page can do reaches that — eFuses burn one way only, and the " +
      "flasher never burns one. Writing firmware here would either be refused by " +
      "the chip or come back unreadable. If this board was sold to you as new, " +
      "it wasn't."));
    const back = el("button", "ghost", "← back");
    back.addEventListener("click", () => setPhase(phaseConnected()));
    box.append(back);
    return box;
  }

  box.append(el("h2", null, `Install ${product.name}?`));
  box.append(el("p", "muted", state.current && state.current.unknown
    ? "This is the one-time first setup — after it, the board is a Canary."
    : "This is the same “flash” process as first setup — the board just gets the new firmware."));
  const sum = el("div", "flash-summary");
  sum.append(fact("Firmware", `${product.name} · v${entry.version}`));
  sum.append(fact("For chip", entry.chipFamily));
  sum.append(fact("Size", core.formatBytes(entry.size)));
  const vpolicy = core.imageVerificationPolicy({
    keyReal: core.isRealPubkey(state.catalog.release_pubkey),
    hasSignature: !!entry.signature,
    selfHosted: !!state.manifestOverride,
  });
  sum.append(fact("Verified by",
    vpolicy === "verify" ? "Ed25519 signature + SHA-256 · chip MD5 after"
      : vpolicy === "require-signature" ? "⚠ unsigned build — this will be refused"
        : "SHA-256 before · chip MD5 after"));
  box.append(sum);

  // The verdict, in plain words: what this install IS for this board.
  const verdict = installVerdictFor(product, entry.version);
  if (verdict) {
    const vline = el("p", `flash-verdict-line flash-verdict-${verdict.kind}`);
    vline.append(el("strong", null, `${verdict.icon} ${verdict.label}. `));
    vline.append(document.createTextNode(verdict.detail));
    const vh = helpDot("verdict");
    if (vh) vline.append(vh);
    box.append(vline);
  }

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

  // Say WHY the erase isn't optional here, rather than silently ticking a box
  // the user can see is unticked in Advanced.
  if (forcedErase) {
    const why = el("div", "flash-reassure flash-forced-erase");
    const line = el("p");
    line.append(el("span", "flash-shield", "🧹"));
    line.append(document.createTextNode(
      "The full erase isn't optional this time: this is a board we haven't " +
      "written before, so we wipe the whole chip rather than only the parts " +
      "we're about to write. A normal install leaves untouched partitions " +
      "alone — fine for a Canary you already own, wrong for one that arrived " +
      "carrying somebody else's firmware."));
    why.append(line);
    // The board saying "I'm already a Canary" is NOT enough to skip this — on
    // an untrusted board that claim lives in writable flash. A human saying it
    // is, so this is the one way out, and it's a deliberate click.
    if (state.current && !state.current.unknown) {
      const claim = el("p", "fineprint");
      claim.append(document.createTextNode(
        "It reports itself as " +
        (state.current.productName || state.current.projectName || "SecuraCV firmware") +
        ", but that text sits in flash the board controls, so it can't be the thing " +
        "that decides. If this is genuinely your own Canary and you want to keep its " +
        "identity key and settings, say so: "));
      const mine = el("button", "ghost small", "it's mine — keep its data");
      mine.addEventListener("click", () => {
        state.ownerClaimed = true;
        if (state.intake) state.intake.firstContact = false;
        setPhase(phaseConfirm(product, entry));
      });
      claim.append(mine);
      why.append(claim);
    }
    box.append(why);
  }

  // WiFi (optional) — for EVERY board now: fill it in and it's baked into
  // the chip's settings region during the install, in whichever NVS scheme
  // this firmware actually reads (catalog wifi_nvs, derived from the
  // firmware source). Leave it empty and nothing changes.
  let wifiUI = null;
  if (product) {
    wifiUI = renderWifiFields(box, product);
  }

  // Flash-time dials (Vision): pick the room, or fine-tune — the same four
  // numbers Home Assistant tunes live later, baked in before first boot.
  let dialsUI = null;
  const dials = core.detectDials(state.catalog, product);
  if (dials) dialsUI = renderDetectDials(box, dials);

  // Flash-time reflexes (Sense): room presets + fine-tune for the radar's
  // seven NVS-backed live numbers — same posture as the Vision dials.
  let reflexUI = null;
  if (product && product.reflexes) reflexUI = renderReflexes(box, product);

  const row = el("div", "flash-row");
  const go = el("button", "primary flash-go", `Install it${eraseOn ? " (with full erase)" : ""}`);
  go.addEventListener("click", () => {
    // Double-click guard: a second pass would re-read the now-cleared
    // password field and remember an empty one over the real network.
    if (state.busy) return;
    go.disabled = true;
    let wifi = null, mqtt = null;
    if (wifiUI) {
      const r = wifiUI.credentials();
      if (!r.ok) { go.disabled = false; return; } // invalid input — the field showed why
      wifi = r.wifi; mqtt = r.mqtt;
      wifiUI.clear();    // never leave the password sitting in the DOM
    }
    const dialSel = dialsUI ? dialsUI.selection() : null;
    const reflexSel = reflexUI ? reflexUI.selection() : null;
    startFlash({ entry, product, eraseAll: !!eraseOn, skipBackup: !!skipBackup, wifi, mqtt,
      detect: dialSel ? dialSel.values : null,
      detectPreset: dialSel ? dialSel.presetTitle : null,
      reflex: reflexSel ? reflexSel.values : null,
      reflexPreset: reflexSel ? reflexSel.presetTitle : null });
  });
  const cancel = el("button", "ghost", "not yet");
  cancel.addEventListener("click", () => setPhase(phaseConnected()));
  row.append(go, cancel);
  box.append(row);
  return box;
}

// ── flash-time dials (Vision): room presets + fine-tune ─────────────────────
// The four runtime detection numbers (confidence floor, lost timeout, dwell
// threshold, target class) are NVS-backed in the firmware and tunable from
// Home Assistant later — so baking a room preset here is genuinely the same
// write, just earlier. Presets and bounds come from the catalog (which parsed
// them out of the firmware); "as it ships" writes nothing at all.
function renderDetectDials(box, dials) {
  const sec = el("div", "flash-dials");
  sec.append(el("h3", null, "Dial it in for its room (optional)"));
  sec.append(el("p", "fineprint", dials.note));

  const chosen = { preset: "ships", values: { ...dials.defaults } };
  const fmtS = (ms) => (ms / 1000) % 1 ? (ms / 1000).toFixed(2) + "s" : (ms / 1000) + "s";

  const grid = el("div", "flash-preset-grid");
  const cards = [];
  dials.presets.forEach((pr) => {
    const b = el("button", "flash-preset");
    b.type = "button";
    b.append(el("span", "flash-preset-icon", pr.icon));
    b.append(el("span", "flash-preset-title", pr.title));
    b.append(el("span", "flash-preset-blurb", pr.blurb));
    b.append(el("span", "flash-preset-vals",
      `confidence ${pr.values.score} · lost ${fmtS(pr.values.lost_ms)} · dwell ${fmtS(pr.values.dwell_ms)}`));
    b.addEventListener("click", () => {
      chosen.preset = pr.id;
      chosen.values = { ...dials.defaults, ...pr.values };
      sync();
    });
    grid.append(b);
    cards.push([b, pr]);
  });
  sec.append(grid);

  // Fine-tune: the same three numbers as sliders, each with its ⓘ.
  const adv = el("details", "flash-dials-adv");
  adv.append(el("summary", null, "fine-tune the dials"));
  const sliders = {};
  const mkSlider = (key, topic, unitFmt, step) => {
    const [lo, hi] = dials.bounds[key];
    const row = el("div", "flash-dial-row");
    const lab = el("span", "flash-dial-label");
    const t = core.helpTopic(state.catalog, topic);
    lab.append(document.createTextNode((t && t.label) || key));
    const hd = helpDot(topic);
    if (hd) lab.append(hd);
    const val = el("span", "flash-dial-val");
    const input = el("input");
    input.type = "range";
    input.min = String(lo); input.max = String(hi); input.step = String(step);
    input.addEventListener("input", () => {
      chosen.values[key] = Number(input.value);
      chosen.preset = "custom";
      sync({ skipSlider: key });
    });
    row.append(lab, input, val);
    adv.append(row);
    sliders[key] = { input, val, unitFmt };
  };
  mkSlider("score", "det_score", (v) => `${v} / 100`, 1);
  mkSlider("lost_ms", "det_lost", fmtS, 250);
  mkSlider("dwell_ms", "det_dwell", fmtS, 1000);
  sec.append(adv);

  function sync(opts = {}) {
    for (const [b, pr] of cards) {
      const on = chosen.preset === pr.id;
      b.classList.toggle("flash-preset-on", on);
      b.setAttribute("aria-pressed", String(on));
    }
    for (const [key, s] of Object.entries(sliders)) {
      if (opts.skipSlider !== key) s.input.value = String(chosen.values[key]);
      s.val.textContent = s.unitFmt(chosen.values[key]);
    }
  }
  sync();

  box.append(sec);
  return {
    selection() {
      // Exactly the shipped defaults → write nothing; the firmware IS that.
      const same = Object.entries(dials.defaults).every(([k, v]) => chosen.values[k] === v);
      if (same) return null;
      const pr = dials.presets.find((p) => p.id === chosen.preset);
      return {
        values: { ...chosen.values },
        presetTitle: pr && pr.id !== "ships" ? pr.title : null,
      };
    },
  };
}

// ── the radar build's reflexes (Sense): room presets + fine-tune ────────────
// The same posture as the Vision dials: seven NVS-backed live numbers, room
// presets first, sliders one click deeper, "as it ships" writes nothing —
// and Home Assistant can retune every knob later.
function renderReflexes(box, product) {
  const r = core.reflexDials(state.catalog, product) || product.reflexes;
  const runtime = r.applies === "runtime" && Array.isArray(r.presets);
  const sec = el("div", "flash-dials");
  sec.append(el("h3", null, runtime
    ? "Dial its reflexes for the room (optional)"
    : "This build’s reflexes — how it judges presence"));
  const note = el("p", "fineprint", r.note);
  const fh = helpDot("sense_flavor");
  if (fh) note.append(fh);
  sec.append(note);

  // Static fallback for an old catalog: the read-only knob list.
  if (!runtime) {
    r.knobs.forEach((k) => {
      const t = core.helpTopic(state.catalog, k.id);
      const row = el("div", "flash-knob");
      const lab = el("span", "flash-knob-label", (t && t.label) || k.id);
      const hd = helpDot(k.id);
      if (hd) lab.append(hd);
      row.append(lab, el("span", "flash-knob-val", `${k.value} ${k.unit}`));
      sec.append(row);
    });
    box.append(sec);
    return null;
  }

  const defaults = r.presets[0].values; // "ships" leads by construction
  const chosen = { preset: "ships", values: { ...defaults } };
  const fmtVal = (k, v) => k.unit === "ms"
    ? ((v / 1000) % 1 ? (v / 1000).toFixed(2) + "s" : (v / 1000) + "s")
    : `${v} ${k.unit}`;

  const grid = el("div", "flash-preset-grid");
  const cards = [];
  r.presets.forEach((pr) => {
    const b = el("button", "flash-preset");
    b.type = "button";
    b.append(el("span", "flash-preset-icon", pr.icon));
    b.append(el("span", "flash-preset-title", pr.title));
    b.append(el("span", "flash-preset-blurb", pr.blurb));
    const deb = r.knobs.find((k) => k.id === "present_debounce_ms");
    const clr = r.knobs.find((k) => k.id === "clear_timeout_ms");
    b.append(el("span", "flash-preset-vals",
      `debounce ${fmtVal(deb, pr.values.present_debounce_ms)} · clear ${fmtVal(clr, pr.values.clear_timeout_ms)}`));
    b.addEventListener("click", () => {
      chosen.preset = pr.id;
      chosen.values = { ...defaults, ...pr.values };
      sync();
    });
    grid.append(b);
    cards.push([b, pr]);
  });
  sec.append(grid);

  const adv = el("details", "flash-dials-adv");
  adv.append(el("summary", null, "fine-tune the reflexes"));
  const sliders = {};
  r.knobs.forEach((k) => {
    const [lo, hi] = k.bounds;
    const t = core.helpTopic(state.catalog, k.id);
    const row = el("div", "flash-dial-row");
    const lab = el("span", "flash-dial-label");
    lab.append(document.createTextNode((t && t.label) || k.id));
    const hd = helpDot(k.id);
    if (hd) lab.append(hd);
    const val = el("span", "flash-dial-val");
    const input = el("input");
    input.type = "range";
    input.min = String(lo); input.max = String(hi);
    input.step = String(k.unit === "cm" ? 10 : 50);
    input.addEventListener("input", () => {
      chosen.values[k.id] = Number(input.value);
      chosen.preset = "custom";
      sync({ skipSlider: k.id });
    });
    row.append(lab, input, val);
    adv.append(row);
    sliders[k.id] = { input, val, knob: k };
  });
  sec.append(adv);

  const lb = el("p", "fineprint");
  lb.append(document.createTextNode("Want to feel these live before committing? "));
  const a = el("a", null, "Open the Sense Lab →");
  a.href = r.lab;
  lb.append(a);
  sec.append(lb);

  function sync(opts = {}) {
    for (const [b, pr] of cards) {
      const on = chosen.preset === pr.id;
      b.classList.toggle("flash-preset-on", on);
      b.setAttribute("aria-pressed", String(on));
    }
    for (const [id, s] of Object.entries(sliders)) {
      if (opts.skipSlider !== id) s.input.value = String(chosen.values[id]);
      s.val.textContent = fmtVal(s.knob, chosen.values[id]);
    }
  }
  sync();

  box.append(sec);
  return {
    selection() {
      const same = Object.entries(defaults).every(([k, v]) => chosen.values[k] === v);
      if (same) return null;
      const pr = r.presets.find((p) => p.id === chosen.preset);
      return {
        values: { ...chosen.values },
        presetTitle: pr && pr.id !== "ships" ? pr.title : null,
      };
    },
  };
}

// ── optional WiFi fields (confirm card) ─────────────────────────────────────
function renderWifiFields(box, product) {
  const sec = el("div", "flash-wifi");
  const wh = el("h3", null, "WiFi (optional)");
  const whd = helpDot("wifi_bake");
  if (whd) wh.append(whd);
  sec.append(wh);

  // A visible "you already typed this" banner so remembering is OBVIOUS — not a
  // silent pre-fill the user re-types board after board out of doubt. (Its
  // refresh is defined once the fields exist, below, so it can hide the moment
  // they're edited away from the saved network — never claiming the wrong one.)
  const savedBanner = el("div", "flash-wifi-saved flash-hidden");
  sec.append(savedBanner);

  const ssid = el("input"), pass = el("input");
  ssid.type = "text"; ssid.placeholder = "network name (SSID)"; ssid.autocomplete = "off";
  // An SSID is an exact identifier: no auto-capitalized first letter, no
  // autocorrect "fixes", no red squiggle. (Attribute form — Safari reads
  // autocapitalize/autocorrect as content attributes, not IDL props.)
  ssid.setAttribute("autocapitalize", "off");
  ssid.setAttribute("autocorrect", "off");
  ssid.spellcheck = false;
  ssid.enterKeyHint = "next";
  // An EXISTING network's key, not an account credential: a text input masked
  // by CSS (-webkit-text-security via .pw-masked), never type=password — a
  // bare password field on an unfamiliar page reads as "sign-up" to the OS,
  // which then offers to INVENT a password no router has ever seen. Real users
  // accepted that suggestion and the join failed. Grab yours from the router
  // sticker / your phone, or let "Remember on this computer" recall it.
  // (Pattern: firmware/LESSONS_LEARNED.md "iOS offers to invent a password".)
  pass.type = "text"; pass.placeholder = "password";
  pass.classList.add("pw-masked");
  pass.autocomplete = "off";
  pass.setAttribute("autocapitalize", "none");
  pass.setAttribute("autocorrect", "off");
  pass.spellcheck = false;
  // Remember-across-boards: pre-fill from the home Wi-Fi we already know — this
  // tab's session, or a copy saved on this computer if the user opted in — so a
  // whole batch of Canaries provisions without re-typing it into each one.
  const savedWifi = wifiMemory.recall();
  if (savedWifi) { ssid.value = savedWifi.ssid; pass.value = savedWifi.pass; }
  // The banner claims "using your saved Wi-Fi X" only while the fields still hold
  // exactly that network; edit either field and it hides (returns if you undo the
  // edit), so it can never contradict what will actually be provisioned.
  const refreshBanner = () => {
    const bs = core.wifiBannerState(ssid.value, pass.value, wifiMemory.recall(), wifiMemory.isPersisted());
    savedBanner.classList.toggle("flash-hidden", !bs.show);
    savedBanner.textContent = "";
    if (bs.show) {
      savedBanner.append(el("span", "flash-wifi-saved-i", "✓"),
        el("strong", null, bs.headline), document.createTextNode(" — " + bs.detail));
    }
  };
  ssid.addEventListener("input", refreshBanner);
  pass.addEventListener("input", refreshBanner);
  const showBtn = el("button", "ghost small", "show");
  showBtn.addEventListener("click", () => {
    // Flip the masking CLASS, never the input type — switching to
    // type="password" re-summons the OS's password generator.
    const masked = pass.classList.toggle("pw-masked");
    showBtn.textContent = masked ? "show" : "hide";
  });
  const rowIn = el("div", "flash-wifi-inputs");
  rowIn.append(ssid, pass, showBtn);
  sec.append(rowIn);

  const err = el("p", "flash-note flash-note-soft flash-hidden");
  sec.append(err);
  const prov = (product && product.provisioning) || "usb-secrets";
  sec.append(el("p", "fineprint", {
    "ap":
      "Fill this in and it’s written into the chip during the install, so the " +
      "Canary joins your WiFi on its very first boot. If it can’t connect — " +
      "or you leave this empty — it simply broadcasts its own setup network " +
      "to connect to and finish setup there. What you type stays on this " +
      "page and goes only to the chip over the cable.",
    "on-glass":
      "Fill this in and it’s written into the chip’s settings region during " +
      "the install — the glass then skips its WiFi wizard and goes straight " +
      "to meeting your Canaries. Leave it empty and the on-screen wizard " +
      "walks you through it on first boot. What you type stays on this page " +
      "and goes only to the chip over the cable.",
    "usb-secrets":
      "Fill this in and it’s written into the chip’s settings region during " +
      "the install — the signed generic release then joins YOUR network on " +
      "first boot, no custom build needed. Leave it empty and the firmware " +
      "keeps its compiled defaults. What you type stays on this page and " +
      "goes only to the chip over the cable.",
  }[prov] || ""));

  // Type it once, provision a whole batch. By default the network is kept in
  // memory for this tab only (gone when you close it, never written to disk);
  // ticking "remember" also saves it in THIS browser on THIS computer so it
  // survives — local-only, never sent anywhere, Forget clears it instantly.
  const remRow = el("div", "flash-wifi-remember");
  const remLabel = el("label", "flash-wifi-remember-label");
  const rememberChk = el("input"); rememberChk.type = "checkbox";
  rememberChk.checked = wifiMemory.isPersisted();
  remLabel.append(rememberChk, document.createTextNode(" Remember on this computer"));
  const forgetBtn = el("button", "ghost small", "Forget saved Wi-Fi");
  const refreshForget = () => forgetBtn.classList.toggle("flash-hidden", !wifiMemory.recall());
  forgetBtn.addEventListener("click", () => {
    wifiMemory.forget();
    ssid.value = ""; pass.value = ""; rememberChk.checked = false;
    refreshForget(); refreshBanner();
  });
  // Ticking/unticking persists (or un-persists) immediately when a network is
  // already typed, so the checkbox is honest about what's saved right now.
  rememberChk.addEventListener("change", () => {
    if (ssid.value) { wifiMemory.remember({ ssid: ssid.value, pass: pass.value }, rememberChk.checked); }
    refreshForget(); refreshBanner();
  });
  remRow.append(remLabel, forgetBtn);
  sec.append(remRow);
  refreshForget(); refreshBanner();

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

  // Home Assistant / MQTT + a device id — the SAME NVS keys the native app bakes
  // (desktop provisioning.rs), so a board flashed here is addressable on your
  // broker out of the box instead of Wi-Fi-only. Optional; nothing leaves this
  // page.
  //
  // Gated on `broker_nvs` — a CAPABILITY of the firmware, read out of its own
  // runtime_config by gen_flash.py — and NOT on `provisioning`, which is a
  // different question. Conflating them hid these fields from every display:
  // displays are provisioned `on-glass`, yet canary-display reads
  // mqtt_host/port/user/pass from NVS on every boot, and its glass portal only
  // ever asked for WiFi. So a display could not be told which hub to talk to by
  // any route at all, and simply sat there unable to reach one.
  let mqttUI = null;
  if (product && product.broker_nvs === true) {
    const ha = el("div", "flash-mqtt");
    ha.append(el("h3", null, "Home Assistant / MQTT (optional)"));
    ha.append(el("p", "fineprint",
      "Bake in your broker and a device id and the Canary reports to Home Assistant on " +
      "its first boot — no extra setup. Leave blank to configure later. Written into the " +
      "chip’s settings exactly the way the native app does; it goes only to the chip."));
    const mk = (ph, kind) => {
      const i = el("input"); i.type = "text"; i.placeholder = ph;
      i.autocomplete = "off";
      if (kind === "password") {
        // A broker secret, not an account credential: masked text, same as the
        // Wi-Fi key above, so nothing ever offers to "generate" one.
        i.classList.add("pw-masked");
        i.setAttribute("autocapitalize", "none");
        i.setAttribute("autocorrect", "off");
        i.spellcheck = false;
      }
      return i;
    };
    const devId = mk("device id (e.g. canary_vision_ab12)");
    // Auto-suggest a UNIQUE per-device id for every board whose firmware
    // derives its MQTT topics from dev_id (broker_nvs) — displays included.
    // A display's glass setup only ever asks for Wi-Fi, and dev_id is sticky
    // forever once written — so with nothing seeded, first boot persists the
    // flavor's SHARED compiled id (canary_dash_001) and two of the same
    // display collide on the same topics, acting on each other's traffic.
    // The suggestion is visible and editable; clearing it writes nothing.
    if (product && product.id &&
        (product.provisioning === "usb-secrets" || product.broker_nvs === true)) {
      const fam = /display/.test(product.id) ? "canary_display"
        : /vision/.test(product.id) ? "canary_vision"
        : /sense/.test(product.id) ? "canary_sense" : "canary";
      const sfx = Array.from(crypto.getRandomValues(new Uint8Array(2)),
        (b) => b.toString(16).padStart(2, "0")).join("");
      devId.value = `${fam}_${sfx}`;
    }
    const host = mk("broker host"); host.value = "homeassistant.local";
    const port = mk("1883"); port.value = "1883"; port.inputMode = "numeric";
    const user = mk("username (optional)");
    const mpass = mk("password (optional)", "password");
    const row1 = el("div", "flash-wifi-inputs"); row1.append(devId);
    const row2 = el("div", "flash-wifi-inputs"); row2.append(host, port);
    const row3 = el("div", "flash-wifi-inputs"); row3.append(user, mpass);
    ha.append(row1, row2, row3);
    sec.append(ha);
    mqttUI = { values: () => ({
      deviceId: devId.value, mqttHost: host.value,
      mqttPort: port.value ? Number(port.value) : 1883,
      mqttUser: user.value, mqttPass: mpass.value,
    }) };
  }

  box.append(sec);
  return {
    credentials() {
      err.classList.add("flash-hidden");
      // Optional broker/identity (usb-secrets only) — validate up front so a bad
      // value stops the flash with a clear message, like the Wi-Fi fields do.
      let mqtt = null;
      if (mqttUI) {
        mqtt = mqttUI.values();
        try { core.mqttProvisioningToNvs(mqtt); }
        catch (e) {
          err.textContent = String(e.message || e);
          err.classList.remove("flash-hidden");
          return { ok: false, wifi: null, mqtt: null };
        }
      }
      if (!ssid.value) return { ok: true, wifi: null, mqtt }; // wifi optional — skipped
      try {
        // The builder validates lengths; run it small just for the checks.
        core.buildNvsWifiImage(ssid.value, pass.value, 4096);
        // Remember it for the next board (session always; disk if opted in).
        wifiMemory.remember({ ssid: ssid.value, pass: pass.value }, rememberChk.checked);
        return { ok: true, wifi: { ssid: ssid.value, pass: pass.value }, mqtt };
      } catch (e) {
        err.textContent = String(e.message || e);
        err.classList.remove("flash-hidden");
        return { ok: false, wifi: null, mqtt: null };
      }
    },
    clear() { pass.value = ""; },
  };
}

// ── the flash itself ────────────────────────────────────────────────────────
async function startFlash(opts) {
  if (state.busy) return;
  state.busy = true;
  // The live voice may hold the port — hand it back to the bootloader first.
  if (!state.session) {
    const okSession = await ensureSession();
    if (!okSession) {
      state.busy = false;
      setPhase(errorRetry("Couldn’t reach the bootloader again",
        new Error("the board didn’t re-enter download mode — unplug, replug, reconnect"),
        phaseConnect));
      return;
    }
  }
  const { esploader } = state.session;
  const eraseAll = !!opts.eraseAll;
  const label = opts.product ? `${opts.product.name} v${opts.entry.version}` : opts.label;

  const box = progressCard(`Installing ${label}`, "Getting the image ready…");
  box.card.dataset.step = "4";
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
    let bytes, shaHex = null, shaSigned = false, sigVerified = false, sigChecked = false;
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
      // 2b) Fail closed: once a REAL release key is pinned, an official manifest
      // MUST carry a valid Ed25519 signature. Verifying only "if a signature is
      // present" would let a tampered manifest strip the signature and re-point
      // an updated SHA-256 at a malicious image — the exact substitution this
      // check exists to stop. (imageVerificationPolicy encodes the fail-closed
      // rule; checksum-only is reserved for pre-key and self-hosted manifests.)
      const policy = core.imageVerificationPolicy({
        keyReal: core.isRealPubkey(state.catalog.release_pubkey),
        hasSignature: !!opts.entry.signature,
        selfHosted: !!state.manifestOverride,
      });
      if (policy === "require-signature") {
        throw new Error("This official release is missing its Ed25519 signature, but a " +
          "signing key is in force — refusing to flash it. A stripped signature can mean " +
          "a tampered release manifest. Nothing was written.");
      } else if (policy === "verify") {
        sigChecked = true;
        sigVerified = await core.verifyImageSignature(
          opts.entry.signature, state.catalog.release_pubkey,
          bytes.length, new Uint8Array(digest));
        if (!sigVerified) {
          throw new Error("This image isn’t signed by the SecuraCV release key — " +
            "refusing to flash it. Nothing was written. (To flash a build you " +
            "trust anyway, use Advanced → flash a local file.)");
        }
      }
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

    // 2.7) Settings pre-provisioning: one minimal valid NVS image can carry
    // the typed WiFi credentials and/or the chosen detection dials (the same
    // det_* keys Home Assistant tunes later), written into the image's own
    // settings region in the same pass as the firmware. If we can't locate
    // that region, the install continues — never block a flash on a
    // convenience.
    let wifiFile = null, wifiSsid = null, seededDials = null, seededReflex = null, bakedDeviceId = "";
    if ((opts.wifi || opts.mqtt || opts.detect || opts.reflex) && !opts.isBackup) {
      try {
        const { entries } = core.parsePartitionTable(
          bytes.subarray(0x8000, Math.min(0x8c00, bytes.length)));
        const nvs = entries.find(core.isNvsPart);
        if (!nvs) throw new Error("no settings region in this image");
        const dials = opts.detect ? core.detectDials(state.catalog, opts.product) : null;
        const dInts = opts.detect ? core.detectValuesToNvs(opts.detect, dials) : { u8: {}, u32: {} };
        const reflexes = opts.reflex ? core.reflexDials(state.catalog, opts.product) : null;
        const rInts = opts.reflex ? core.reflexValuesToNvs(opts.reflex, reflexes) : { u32: {} };
        // Broker + device id (usb-secrets) → the same NVS keys the native app writes.
        const prov = opts.mqtt ? core.mqttProvisioningToNvs(opts.mqtt) : { strings: {}, u16: {} };
        const nvsImg = core.buildNvsSeedImage(
          { wifi: opts.wifi || null,
            wifiScheme: (opts.product && opts.product.wifi_nvs) || "blob",
            strings: prov.strings, u16: prov.u16,
            u8: dInts.u8, u32: { ...dInts.u32, ...rInts.u32 } }, nvs.size);
        wifiFile = { data: core.bytesToBinaryString(nvsImg), address: nvs.offset };
        wifiSsid = opts.wifi ? opts.wifi.ssid : null;
        seededDials = opts.detect || null;
        seededReflex = opts.reflex || null;
        // Only a device id that was actually written to NVS may appear as the
        // certificate's Ring ID (this line is reached only on a successful bake).
        bakedDeviceId = prov.strings.dev_id || "";
      } catch (e) {
        box.stage("Couldn’t bake the settings (" + String(e.message || e) +
          ") — continuing; everything is still tunable after boot");
        await sleep(1200);
      }
    }

    // 3) Write, with live progress + automatic chip MD5 verification —
    // the map lights up region by region as the write cursor passes.
    if (eraseAll) {
      nextStep("erasing the whole chip");
      await esploader.eraseFlash();
    }
    nextStep(wifiFile ? "writing firmware + your settings" : "writing firmware");
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
    state.baudCeiling = null; // this speed worked — don't carry a cap forward
    // Two-port Vision: this ESP32 half is now flashed — record it (once, here at
    // the completion transition, not in the render) so the done screen and the
    // camera-module flow can insist on both ports.
    if (opts.product && !opts.isBackup && core.isVisionBoard(opts.product)) {
      visionSession.markDone("esp32");
    }
    // The Nursery roster: one hatchling per successful install, so a batch
    // session always knows which boards are done and what they got.
    if (opts.product && !opts.isBackup && !opts.isLocal) {
      state.roster = core.rosterAdd(state.roster, {
        t: Date.now(),
        mac: state.mac ? core.formatMac(state.mac) : null,
        product: opts.product.name,
        version: opts.entry ? opts.entry.version : null,
        preset: opts.detectPreset || opts.reflexPreset || null,
        wifi: !!wifiSsid,
      });
      saveRoster(state.roster);
    }
    setPhase(phaseDone({ ...opts, backupName, backupFailed, diff, settings,
      shaHex, shaSigned, sigVerified, sigChecked, bytesWritten: bytes.length,
      wifiSsid, seededDials, seededReflex, provDeviceId: bakedDeviceId, wifi: null }));
  } catch (e) {
    state.busy = false;
    // Self-heal write-time failures too: a flaky cable can sync at 921600 but
    // time out mid-write. Lower the baud ceiling a rung so the retry's connect
    // ladder writes at a gentler speed (the ladder alone only covers connect).
    const k = core.classifyFlashError(e).kind;
    if (state.usedBaud && ["not-in-download", "read-stall", "device-lost", "unknown"].includes(k)) {
      const lower = core.FLASH_BAUDS.find((b) => b < state.usedBaud);
      if (lower) state.baudCeiling = lower;
    }
    setPhase(flashError(e, opts));
  }
}

function flashError(e, opts) {
  chirp("oops"); // one low, round note — never an alarm
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
  // Escalate: if a plain install failed, offer a full-erase clean install as
  // the next self-heal step (reconnect → the rescue flow, which also restores
  // a backup if one exists). Skip when we're already erasing/rescuing.
  if (!opts.rescue && !opts.eraseAll) {
    const clean = el("button", "ghost", "Clean install (full erase) →");
    clean.addEventListener("click", async () => {
      await onDisconnect(true);
      state.resumeRescue = true;
      // Carry the product being installed so the rescue can't default to the
      // wrong firmware (the reconnect clears the read-back identity).
      state.resumeRescuePrefer = opts.product || null;
      setPhase(phaseConnect());
    });
    row.append(clean);
  }
  row.append(diagnosticReportButton(() => ({ stage: "install", error: msg })));
  box.append(row);
  const raw = el("details", "flash-rawerr");
  raw.append(el("summary", null, "Technical details"));
  raw.append(el("pre", null, msg));
  box.append(raw);
  return box;
}

// ── the Hatchery: a whimsical name + birth certificate, like the native app ──
// One shared spec (devices/hatch.json — the same file the native app embeds),
// fetched once; the assembly is the pure, host-tested hatchery.js. Names are
// whimsy — the device keeps its functional id. Never throws into the flash flow.
let _hatchSpec;                 // undefined = untried, null = unavailable, obj = loaded
const _hatchUsed = new Set();   // base names spent this session (so names stay fresh)
const _hatchFleet = [];         // this session's hatches, for the "Nth of its name" ordinal
async function loadHatchSpec() {
  if (_hatchSpec !== undefined) return _hatchSpec;
  try {
    _hatchSpec = await fetch("devices/hatch.json", { cache: "force-cache" })
      .then((r) => (r.ok ? r.json() : null));
  } catch { _hatchSpec = null; }
  return _hatchSpec;
}
async function renderHatchCert(slot, opts) {
  try {
    const spec = await loadHatchSpec();
    if (!spec) return;
    let cert = mintCertificate(spec, {
      product: opts.product, deviceId: opts.deviceId,
      fleet: _hatchFleet, usedBases: _hatchUsed, now: Date.now(),
    });
    if (!cert) return;
    _hatchUsed.add(cert.base);
    _hatchFleet.unshift({ base: cert.base, ringId: cert.ringId });

    const c = (spec.certificate) || {};
    const paint = () => {
      slot.textContent = "";
      const fig = el("figure", "flash-cert");
      fig.append(el("div", "flash-cert-kicker", c.kicker || "Certificate of Hatching"));
      if (c.intro) fig.append(el("p", "flash-cert-intro", c.intro));
      fig.append(el("div", "flash-cert-name", cert.name));
      fig.append(el("div", "flash-cert-lineage", cert.lineage));
      const meta = el("div", "flash-cert-meta");
      meta.append(el("span", null, "Species · " + cert.species),
                  el("span", null, "Ring · " + cert.ringId));
      fig.append(meta);
      if (cert.motto) fig.append(el("div", "flash-cert-motto", "“" + cert.motto + "”"));
      if (cert.craft) fig.append(el("div", "flash-cert-craft", cert.craft));
      if (c.foot) fig.append(el("div", "flash-cert-foot", c.foot));
      const reroll = el("button", "ghost small flash-cert-reroll", "🎲 new name");
      reroll.addEventListener("click", () => {
        const next = mintCertificate(spec, {
          product: opts.product, deviceId: opts.deviceId,
          fleet: _hatchFleet, usedBases: _hatchUsed, avoidBase: cert.base, now: cert.ts,
        });
        if (!next) return;
        next.ringId = cert.ringId;                 // same bird, new whimsy
        // Record the accepted base and replace (not stack) this hatch's fleet
        // entry, so a further reroll can't repeat a name and the next board
        // can't reuse this base while still calling itself "the First".
        _hatchUsed.add(next.base);
        if (_hatchFleet[0]) _hatchFleet[0] = { base: next.base, ringId: cert.ringId };
        cert = next;
        paint();
      });
      fig.append(reroll);
      slot.append(fig);
    };
    paint();
  } catch { /* the certificate is a delight, never a requirement */ }
}

// ── phase: done — celebration + watch it boot ───────────────────────────────
function phaseDone(opts) {
  const box = el("section", "flash-card flash-done");
  box.dataset.step = "5";
  confettiBurst();
  chirp("hatch");
  // The hatch: egg wiggles, cracks, and the chick appears. Pure CSS
  // (flash-hatch-* keyframes); under prefers-reduced-motion the chick
  // simply IS — the celebration stays, the motion politely doesn't.
  const hatch = el("div", "flash-hatch");
  hatch.setAttribute("aria-hidden", "true");
  hatch.append(el("span", "flash-hatch-egg", "🥚"),
               el("span", "flash-hatch-cracked", "🐣"),
               el("span", "flash-hatch-chick", "🐤"));
  box.append(hatch);
  const hatchNo = !opts.isBackup && !opts.isLocal && opts.product && state.roster.length
    ? state.roster[state.roster.length - 1].n : null;
  box.append(el("h2", null, opts.isBackup ? "Restored — your Canary is back to that copy"
    : hatchNo ? `Hatchling #${hatchNo} — your Canary is awake`
    : "Installed — your Canary is awake"));

  // A whimsical name + birth certificate for a real, fresh hatch — the same one
  // the native app mints. Async: the done card renders now; the certificate
  // appears once the shared hatch.json spec loads (or quietly not at all).
  if (hatchNo && opts.product && !opts.isBackup && !opts.isLocal) {
    const certSlot = el("div", "flash-cert-slot");
    box.append(certSlot);
    renderHatchCert(certSlot, {
      product: opts.product,
      deviceId: opts.provDeviceId || "", // only the id actually written to NVS becomes the Ring ID
    });
  }

  const product = opts.product;
  if (opts.wifiSsid) {
    const w = el("p", "muted");
    w.append(el("span", "flash-check", "✓"));
    w.append(document.createTextNode(
      ` Your WiFi is baked in — the Canary should join “${opts.wifiSsid}” on its very first boot. ` +
      `No setup network needed (it still appears if the join fails, as the fallback).`));
    box.append(w);
  }
  if (opts.seededDials) {
    const d = opts.seededDials;
    const fmtS = (ms) => (ms / 1000) % 1 ? (ms / 1000).toFixed(2) + "s" : (ms / 1000) + "s";
    const line = el("p", "muted");
    line.append(el("span", "flash-check", "✓"));
    line.append(document.createTextNode(
      ` Dialed in${opts.detectPreset ? ` for ${opts.detectPreset}` : ""} — ` +
      `confidence ${d.score}, lost ${fmtS(d.lost_ms)}, dwell ${fmtS(d.dwell_ms)} are ` +
      `baked into its settings. Home Assistant can retune all of them live, any time.`));
    box.append(line);
  }
  if (opts.seededReflex) {
    const rv = opts.seededReflex;
    const fmtS = (ms) => (ms / 1000) % 1 ? (ms / 1000).toFixed(2) + "s" : (ms / 1000) + "s";
    const line = el("p", "muted");
    line.append(el("span", "flash-check", "✓"));
    line.append(document.createTextNode(
      ` Reflexes dialed in${opts.reflexPreset ? ` for ${opts.reflexPreset}` : ""} — ` +
      `debounce ${fmtS(rv.present_debounce_ms)}, clear ${fmtS(rv.clear_timeout_ms)}, ` +
      `bands ${rv.range_near_cm}/${rv.range_mid_cm} cm — baked into its settings. ` +
      `Home Assistant can retune every one of them live.`));
    box.append(line);
  }
  // The ONE obvious next step for THIS board — tailored to how it sets up and
  // what it senses, so "it's alive" leads somewhere instead of dead-ending.
  if (product && !opts.isBackup && core.isVisionBoard(product)) {
    // A Vision is two ports. The generic "watch it prove itself" is premature
    // until the camera module has its model too — so here the two-port checklist
    // IS the next step: it shows what's done, what's left, and routes to the
    // other port. (visionSession was marked at the completion transition above.)
    box.append(visionChecklistCard(visionSession.parts(), {
      onFlashOther: async () => {
        // Release the ESP32 port first — the module flow opens its own transport,
        // and leaving state.session open would lock the host port until reload.
        await onDisconnect(true);
        setPhase(phaseModule({
          catalog: state.catalog,
          setPhase,
          back: () => setPhase(phaseConnect()),
        }));
      },
    }));
  } else if (product && !opts.isBackup) {
    const step = core.postFlashNextStep(product, { wifiJoined: !!opts.wifiSsid });
    const ns = el("div", "flash-nextstep");
    ns.append(el("div", "flash-nextstep-title", `Next — ${step.title}`));
    ns.append(el("p", "flash-nextstep-body", step.body));
    box.append(ns);
  } else if (!product) {
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
      if (opts.sigChecked) {
        list.append(reportRow("Ed25519 release signature", el("span", "flash-check", "✓ verified"), "ok"));
        list.append(el("p", "fineprint",
          "Verified in your browser against the release public key pinned in this " +
          "page — the same key the device checks. A swapped or tampered image would " +
          "have been refused before a single byte was written."));
      } else if (opts.shaSigned) {
        list.append(el("p", "fineprint",
          "This release isn't Ed25519-signed yet (the signing-key ceremony hasn't " +
          "happened), so it was verified by checksum only."));
      }
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

  // The radar wow: a freshly-hatched Sense proves itself on the live bench —
  // presence, bands, and the wellbeing senses — not in a text console.
  const doneRole = opts.product && !opts.isBackup ? core.productRole(opts.product.id) : null;

  // The session's progression, right where the next board gets plugged in.
  const rosterStrip = renderRosterStrip();
  if (rosterStrip) box.append(rosterStrip);

  // Prove it, two ways — the SAME shape for every Canary (parity is a
  // tested guarantee): one real proof over the cable/glass, one emulated
  // twin a click away. The catalog's prove block owns both.
  const proveSpec = opts.product && !opts.isBackup
    ? ((state.catalog.products.find((p) => p.id === opts.product.id) || {}).prove || null)
    : null;
  const proveReal = () => {
    const kind = proveSpec ? proveSpec.real.kind : "monitor";
    if (kind === "bench-radar") return openSenseBench(opts.product);
    if (kind === "bench-field") return openWapBench(opts.product);
    if (kind === "bench-camera") {
      // The camera bench lives on the MODULE's port — route through the
      // module flow (same hand-off the two-port checklist uses).
      return onDisconnect(true).then(() => setPhase(phaseModule({
        catalog: state.catalog, setPhase, back: () => setPhase(phaseConnect()),
      })));
    }
    // "glass" and "monitor": the console is the honest window either way.
    return openMonitor({ celebrate: true, skipReset: true, proveIdentity: true });
  };

  const row = el("div", "flash-row");
  const watch = el("button", "primary",
    proveSpec ? proveSpec.real.label : "Watch it boot & prove itself →");
  if (proveSpec) watch.title = proveSpec.real.how;
  watch.addEventListener("click", proveReal);
  const again = el("button", "ghost", "Set up another board");
  // A new board is a new bring-up: drop any in-progress two-port Vision pair so a
  // half-done Vision can't carry a stale flag into the next board (else its other
  // half could later read as "both done"). Guided continuation uses the checklist
  // CTA above, not this button, so it's unaffected.
  again.addEventListener("click", () => onDisconnect().then(() => { visionSession.reset(); setPhase(phaseConnect()); }));
  const tour = el("button", "ghost", "replay the layers tour");
  let tourEl = null;
  tour.addEventListener("click", () => {
    if (tourEl) { tourEl.remove(); tourEl = null; return; }
    tourEl = installStory(() => state.lastImage);
    box.append(tourEl);
  });
  row.append(watch);
  if (proveSpec) {
    const twin = el("a", "ghost flash-twin", proveSpec.emulated.label);
    twin.href = proveSpec.emulated.href;
    twin.target = "_blank";
    twin.rel = "noopener";
    twin.title = proveSpec.emulated.how;
    row.append(twin);
  }
  // AP-based Canaries (canary / WAP) run the read-only Bluetooth console, so
  // offer the over-the-air "it's on" check right after the USB flash — the same
  // proof, on the path that survives a WiFi outage.
  const p = opts.product;
  const bleCapable = p && !opts.isBackup &&
    (core.productRole(p) === "wap" || p.provisioning === "ap");
  if (bleCapable) {
    const bt = el("button", "ghost", "Check over Bluetooth →");
    bt.title = "Reach this Canary's read-only Bluetooth console and read its live snapshot";
    bt.addEventListener("click", () => setPhase(phaseBluetoothCheck(() => setPhase(phaseDone(opts)))));
    row.append(bt);
  }
  row.append(again, tour);
  box.append(row);
  return box;
}

// The emulated-twin link for a product, wherever a bench wants to offer it.
function twinLink(product) {
  const spec = product &&
    ((state.catalog.products.find((p) => p.id === product.id) || {}).prove || null);
  if (!spec) return null;
  const a = el("a", "ghost small flash-twin", spec.emulated.label);
  a.href = spec.emulated.href;
  a.target = "_blank";
  a.rel = "noopener";
  a.title = spec.emulated.how;
  return a;
}

// ── the Bluetooth check: prove a Canary is really on and talking, over the air
// The flash channel is USB (Web Serial); this is its companion — the same
// "prove it's alive" idea, but over Bluetooth. A Canary's WAP firmware runs a
// read-only BLE console (one GATT service, one snapshot characteristic; see
// flash-core.js BLE_CONSOLE + firmware .../ble_console.h), the very path that
// keeps working when WiFi is down. This phase answers three plain questions:
// is Bluetooth on, can the browser reach a Canary, and what is it reporting —
// no flashing, nothing written to the board.
async function stopBle() {
  const b = state.ble;
  state.ble = null;
  if (!b) return;
  try { b.characteristic && await b.characteristic.stopNotifications(); } catch { /* already gone */ }
  try {
    if (b.device && b.device.gatt && b.device.gatt.connected) b.device.gatt.disconnect();
  } catch { /* already gone */ }
}

function phaseBluetoothCheck(back) {
  const goBack = back || (() => setPhase(phaseConnect()));
  const box = el("section", "flash-card flash-ble");
  box.append(el("div", "flash-big-emoji", "🔵"));
  box.append(el("h2", null, "Is it on? Reach a Canary over Bluetooth"));
  box.append(el("p", "muted",
    "A separate check from flashing: a Canary keeps a read-only Bluetooth " +
    "“console” alive even when WiFi is down. This asks your browser to find " +
    "one, connect, and read its live snapshot — proof it’s on and talking. " +
    "Nothing is written to the board."));

  const status = el("p", "flash-ble-status muted", "");
  box.append(status);

  const support = core.bleSupport(navigator);
  if (!support.supported) {
    // Same shape as the Web Serial fallback: Chromium desktop / Android only.
    status.classList.remove("muted");
    box.append(errorBox("This browser can’t do Bluetooth here", support.reason));
    const native = el("p", "fineprint",
      "On iPhone or iPad, the SecuraCV app talks to a Canary over Bluetooth " +
      "natively — Safari has no Web Bluetooth. On a computer, use a Chromium " +
      "browser (Chrome, Edge, Brave).");
    box.append(native);
    const backBtn = el("button", "ghost", "← back");
    backBtn.addEventListener("click", () => stopBle().then(goBack));
    box.append(backBtn);
    return box;
  }

  const result = el("div", "flash-ble-result");
  box.append(result);

  const connectBtn = el("button", "primary", "Find & connect a Canary");
  const backBtn = el("button", "ghost", "← back");
  const row = el("div", "flash-row");
  row.append(connectBtn, backBtn);
  box.append(row);
  backBtn.addEventListener("click", () => stopBle().then(goBack));

  let connectedName = null;   // the board's advertised (branded) BLE name
  const renderSnapshot = (snap, live) => {
    result.innerHTML = "";
    const card = el("div", "flash-card flash-ble-card");
    card.append(el("h3", null,
      connectedName
        ? `✓ Connected to ${connectedName} — it’s on and talking`
        : "✓ Connected — your Canary is on and talking"));
    const rows = core.bleSnapshotRows(snap);
    if (rows.length) {
      const facts = el("div", "flash-facts");
      rows.forEach((r) => facts.append(fact(r.label, r.value)));
      card.append(facts);
    } else {
      card.append(el("p", "muted",
        "Connected, but the snapshot was empty — the board is reachable over " +
        "Bluetooth even so."));
    }
    card.append(el("p", "fineprint",
      live
        ? "Live — this updates on its own as the board pushes new snapshots."
        : "A single read. Reconnect for live updates."));
    result.append(card);
  };

  // Report whether the radio itself is switched on before asking for a device —
  // getAvailability() is the honest "is Bluetooth on" signal Web Serial lacks.
  const reportRadio = async () => {
    status.textContent = "Checking Bluetooth…";
    let available = true;
    try {
      if (navigator.bluetooth.getAvailability) {
        available = await navigator.bluetooth.getAvailability();
      }
    } catch { available = true; /* some engines don't implement it — don't block */ }
    status.textContent = available
      ? "Bluetooth is available on this device. Click below and pick your Canary."
      : "Bluetooth looks switched off (or blocked). Turn it on, then try again.";
    return available;
  };
  reportRadio();

  const connect = async () => {
    connectBtn.disabled = true;
    result.innerHTML = "";
    status.textContent = "Opening the Bluetooth chooser…";
    try {
      await stopBle(); // drop any prior console first
      // Discover by the Canary's ADVERTISED identity (branded name + pairing
      // service); the console service is reached over the connection. See
      // core.bleRequestOptions.
      const device = await navigator.bluetooth.requestDevice(core.bleRequestOptions());
      connectedName = device.name || null;
      status.textContent = `Connecting to ${connectedName || "the Canary"}…`;
      device.addEventListener("gattserverdisconnected", () => {
        if (state.ble && state.ble.device === device) {
          state.ble = null;
          status.textContent = "The Canary disconnected. Click to reconnect.";
          connectBtn.disabled = false;
          connectBtn.textContent = "Reconnect";
        }
      });
      const server = await device.gatt.connect();
      const service = await server.getPrimaryService(core.BLE_CONSOLE.serviceUuid);
      const chr = await service.getCharacteristic(core.BLE_CONSOLE.snapshotUuid);
      state.ble = { device, characteristic: chr };

      // One read now, then subscribe for the board's periodic pushes.
      const first = await chr.readValue();
      renderSnapshot(core.parseBleSnapshot(first), false);
      try {
        await chr.startNotifications();
        chr.addEventListener("characteristicvaluechanged", (ev) => {
          const snap = core.parseBleSnapshot(ev.target.value);
          if (snap) renderSnapshot(snap, true);
        });
        status.textContent = "Live — reading your Canary’s snapshot over Bluetooth.";
      } catch {
        status.textContent = "Connected (single read — live updates weren’t available).";
      }
      connectBtn.disabled = false;
      connectBtn.textContent = "Reconnect";
    } catch (e) {
      connectBtn.disabled = false;
      const name = (e && e.name) || "";
      if (name === "NotFoundError") {
        // User dismissed the chooser, or no Canary was advertising. On the XIAO
        // ESP32-S3 the #1 physical cause of "nothing shows up" is the external
        // u.FL antenna not being seated — Seeed is explicit that BLE may not
        // work at all without it — so name it before blaming range/power.
        status.textContent =
          "No Canary picked. Make sure it’s powered and nearby, and that its " +
          "external antenna is seated (on the XIAO ESP32-S3 the u.FL WiFi/BT " +
          "antenna must be attached or Bluetooth may not work at all). Its " +
          "console advertises for a bonded phone; pick it from the list.";
      } else if (name === "SecurityError" || name === "NotAllowedError") {
        result.innerHTML = "";
        result.append(errorBox("The console is bonded-only",
          "The board answered but its snapshot is readable by paired devices " +
          "only (that’s deliberate — same trust as the WiFi token). Pair this " +
          "device with the Canary in your OS Bluetooth settings, then try again."));
      } else {
        result.innerHTML = "";
        result.append(errorBox("Bluetooth connect failed",
          (e && e.message) || "Couldn’t reach the Canary. Check the external " +
          "antenna is seated (required for BLE on the XIAO ESP32-S3), move " +
          "closer, power-cycle it, and retry."));
      }
    }
  };
  connectBtn.addEventListener("click", connect);
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
  box.dataset.step = "3";
  box.append(el("h2", null, "Rescue this board"));
  box.append(el("p", "muted",
    "For a Canary that’s acting wrong and you just want it back to known-good. " +
    "Three steps: a safety copy is attempted first (a corrupted board may not " +
    "give one — the rescue continues anyway), the whole chip is wiped, and the " +
    "newest signed firmware for your exact chip is written and verified. This " +
    "works the same for every future firmware release — the flasher always " +
    "fetches the latest signed image for the silicon in hand."));

  const matches = core.productsForChip(state.catalog, state.chip);
  // A clean-install escalation from a failed flash carries the product the user
  // was installing — state.current is cleared by the reconnect, so without this
  // a WAP / Vision rescue would silently default to plain canary. Consume it.
  const carried = (state.resumeRescuePrefer &&
    matches.find((p) => p.id === state.resumeRescuePrefer.id)) || null;
  state.resumeRescuePrefer = null;
  const preferred = carried || core.pickRescueProduct(
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
      carried
        ? `${preferred.name} is pre-selected — it’s the firmware you were installing.`
        : `${preferred.name} is pre-selected because that’s what the board says it was running.`));
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
  if (state.busy) return;
  state.busy = true; // BEFORE the slow re-sync — a second click must bounce off
  if (!state.session && !(await ensureSession())) {
    state.busy = false;
    setPhase(errorRetry("Couldn’t reach the bootloader for the health check",
      new Error("the board didn’t re-enter download mode — unplug, replug, then Connect again"),
      phaseConnect));
    return;
  }
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
  box.dataset.step = "2";
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
  ["h", "help"], ["j", "self-manifest"], ["i", "identity"], ["s", "status"],
  ["t", "time"], ["w", "wifi"], ["m", "system"], ["b", "battery"],
];

async function openMonitor(opts = {}) {
  if (state.busy || state.opening) return;
  state.opening = true; // synchronous — a double-click can't spawn two port consumers
  try { return await openMonitorInner(opts); } finally { state.opening = false; }
}
async function openMonitorInner(opts = {}) {
  let port = state.session && state.session.port;
  if (!port && state.voice) port = await stopVoice(); // hand the voice's port to the full monitor
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
  box.dataset.step = "5";
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

  // Boot-log diagnosis: when a fatal signature scrolls past (brownout, panic,
  // no bootable app…), translate it into a plain-language cause + fix instead
  // of leaving the user to read raw panic text. Informational — the fix text
  // says what to do, and the "back to the flasher" button below re-flashes.
  const diagPanel = el("div", "flash-diag flash-hidden");
  box.append(diagPanel);
  let diagnosedSig = null;
  function maybeDiagnose(text) {
    const d = core.diagnoseBootLog(text);
    if (!d || d.signature === diagnosedSig) return;
    diagnosedSig = d.signature;
    diagPanel.innerHTML = "";
    diagPanel.classList.remove("flash-hidden");
    diagPanel.append(el("div", "flash-diag-emoji", d.action === "power" ? "🔌" : "🩺"));
    diagPanel.append(el("h3", null, d.means));
    diagPanel.append(el("p", "muted", d.fix));
  }

  // Post-flash proof: when the firmware answers `j` with its signed
  // self-manifest, render a verified-identity card — the flash proven from the
  // board's own mouth (the same self-verify as securacv.com/canary).
  const idCard = el("div", "flash-identity flash-hidden");
  box.append(idCard);
  let identityShown = false;
  function maybeIdentity(text) {
    if (identityShown) return;
    const m = core.parseSelfManifest(text);
    if (!m) return;
    identityShown = true;
    const signed = !!(m.pubkey || m.pubkey_fp);
    idCard.innerHTML = "";
    idCard.classList.remove("flash-hidden");
    idCard.append(el("div", "flash-identity-head",
      (signed ? "✓ " : "") + "Your Canary just proved itself"));
    // The self-check verdict, front and center — so a headless board (no screen)
    // SHOWS you it works instead of being a silent dud. Health IS its self-test.
    {
      // Always show the verdict — including "Self-check pending" when health is
      // null/unknown — so a headless board never falls back to no status at all.
      const v = core.healthVerdict(m.health);
      const scored = typeof m.health === "number" && Number.isFinite(m.health) &&
                     m.health >= 0 && m.health <= 100;
      const vb = el("div", `flash-selfcheck flash-selfcheck-${v.level}`);
      vb.append(el("span", "flash-selfcheck-icon", v.icon));
      vb.append(el("span", null, scored ? `${v.label} · ${m.health}/100` : v.label));
      idCard.append(vb);
    }
    const facts = el("div", "flash-facts");
    if (m.board) facts.append(fact("board", m.board));
    if (m.firmware) facts.append(fact("firmware", m.firmware));
    const fp = core.formatFingerprint(m.pubkey_fp || m.pubkey);
    if (fp) facts.append(fact("key fingerprint", fp));
    if (typeof m.boots === "number") facts.append(fact("boots", String(m.boots)));
    // Heat, straight from the chip's own sensor — firmwares that report
    // temp_c in the manifest get a live reading; older ones simply don't.
    if (typeof m.temp_c === "number" && Number.isFinite(m.temp_c) &&
        m.temp_c > -40 && m.temp_c < 150) {
      const warm = m.temp_c >= 70;
      facts.append(fact("temperature",
        `${Math.round(m.temp_c)} °C${warm ? " — running hot; give it air" : ""}`));
    }
    idCard.append(facts);
    if (m.tamper) {
      idCard.append(el("p", "flash-note flash-note-soft",
        "⚠ The board reports a tamper flag — expected if you’ve opened it; look into it if not."));
    }
    idCard.append(el("p", "fineprint", signed
      ? "Read straight from the running firmware over the cable — the same signed " +
        "identity securacv.com/canary shows. Nothing was sent anywhere."
      : "Read from the running firmware over the cable."));
    setStatus("✓ Identity confirmed — it’s running and answering.");
  }

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
  //
  // A PERSISTENT console. ESP32-S3 boards run native USB, so a reset (or the
  // firmware rebooting itself) makes the chip vanish and re-enumerate as a
  // fresh USB device — the old port handle goes dead. We never leave the user
  // staring at a frozen "connected" screen: when bytes stop we say so plainly
  // and grab the board again automatically the moment it reappears (the
  // `serial` connect event), and a one-tap reconnect is always in reach.
  const mon = {
    alive: true, port: null, reader: null, writer: null,
    manualBaud: false, session: 0, everByted: false,
    lastPort: null, lastVid: null, ambiguityNoted: false,
    forced: null, reopenSame: false, waiting: null,
  };
  let celebrated = !opts.celebrate;
  let quietTimer = null;

  function setStatus(t) { status.textContent = t; }

  function send(text) {
    if (!mon.writer || !text) return;
    mon.writer.write(new TextEncoder().encode(text)).catch(() => {});
  }

  // A reconnect button that lives right under the status line — hidden while
  // the stream is healthy, shown (persistently) whenever we're waiting.
  const reconnectBtn = el("button", "ghost small flash-hidden", "reconnect the board →");
  reconnectBtn.addEventListener("click", async () => {
    let np;
    try { np = await navigator.serial.requestPort(); } catch { return; }
    reconnectBtn.classList.add("flash-hidden");
    mon.everByted = false;
    if (mon.waiting) { mon.waiting(np); }              // we were waiting → use it now
    else { mon.forced = np; mon.session++; await stopStreams(); }  // interrupt & switch
  });
  status.after(reconnectBtn);

  async function stopStreams() {
    if (quietTimer) { clearTimeout(quietTimer); quietTimer = null; }
    try { mon.reader && await mon.reader.cancel(); } catch {}
    try { mon.reader && mon.reader.releaseLock(); } catch {}
    try { mon.writer && mon.writer.releaseLock(); } catch {}
    mon.reader = mon.writer = null;
    try { mon.port && await mon.port.close(); } catch {}
    mon.port = null;
  }

  // Re-find a granted port (no user gesture needed — only requestPort() is
  // gesture-gated; getPorts()/open() are not).
  //
  // Same board or no board — never "a" board. Web Serial grants persist
  // across sessions, so getPorts() can hold every board this user has EVER
  // approved on this site; the old `ports[0]` fallback could silently attach
  // the console to a Canary granted weeks ago (the desktop app had the same
  // class of bug, found in the same review). The ladder mirrors the native
  // one as far as Web Serial allows: the same port object wins outright; a
  // sole same-vendor candidate is the board back under a new identity (a
  // re-enumeration mints a new object); anything plural is a question only a
  // human can answer — and the reconnect button, which is a chooser, is
  // exactly that human answering.
  async function reacquire() {
    try {
      const ports = await navigator.serial.getPorts();
      if (!ports.length) return null;
      if (mon.lastPort && ports.includes(mon.lastPort)) return mon.lastPort;
      const vendor = mon.lastVid;
      const candidates = vendor == null ? ports : ports.filter((p) => {
        try { return p.getInfo().usbVendorId === vendor; } catch { return false; }
      });
      if (candidates.length === 1) return candidates[0];
      if (candidates.length > 1 && !mon.ambiguityNoted) {
        mon.ambiguityNoted = true;
        setStatus("More than one granted board could be yours — press “reconnect the " +
          "board” and pick it, or unplug the others.");
      }
      return null;
    } catch { return null; }
  }

  // Resolve once the board is physically back: the `connect` event fires on
  // re-enumeration; a slow poll confirms by actually opening the port (a
  // granted-but-unplugged port still shows up in getPorts(), so we must test).
  function waitForPort(msg) {
    setStatus(msg);
    reconnectBtn.classList.remove("flash-hidden");
    return new Promise((resolve) => {
      let settled = false;
      const finish = (p) => {
        // Always tear down — guarding on mon.alive here used to leak the
        // 1 Hz poll (which OPENS the port to test it) forever after leaving
        // the monitor mid-wait, fighting every later connect for the port.
        if (settled) return;
        settled = true; clearInterval(poll);
        navigator.serial.removeEventListener("connect", tryGrant);
        mon.waiting = null;
        resolve(mon.alive ? p : null);
      };
      const tryGrant = async () => {
        if (settled) return;
        const p = await reacquire();
        if (!p) return;
        try { await p.open({ baudRate: Number(baudSel.value) }); await p.close(); finish(p); } catch {}
      };
      const poll = setInterval(tryGrant, 1000);
      navigator.serial.addEventListener("connect", tryGrant);
      mon.waiting = finish;   // the reconnect button hands us a port directly
      tryGrant();
    });
  }

  // One open-read session at a given baud. Returns "garbage" (wrong baud),
  // "ended" (stream closed), or "error" (port went away).
  async function pumpOnce(p, baud) {
    const mySession = ++mon.session;
    await p.open({ baudRate: baud });
    mon.port = mon.lastPort = p;
    // Remember the board's USB vendor for reacquire(): a re-enumeration mints
    // a new port object, so the vendor is the one identity that survives.
    try { mon.lastVid = p.getInfo().usbVendorId ?? null; } catch { mon.lastVid = null; }
    mon.ambiguityNoted = false;
    try { mon.writer = p.writable.getWriter(); } catch {}
    mon.reader = p.readable.getReader();
    reconnectBtn.classList.add("flash-hidden");
    setStatus(`Connected at ${baud} — the board is talking. Press a command, or h for its menu.`);
    // Post-flash proof: ask the firmware to prove its signed identity (`j`).
    // Read-only and harmless; sent twice in case the first lands mid-boot.
    if (opts.proveIdentity && !mon.askedManifest) {
      mon.askedManifest = true;
      const ask = () => { if (mon.alive && !identityShown) send("j\n"); };
      setTimeout(ask, 600);
      setTimeout(ask, 2000);
    }
    // If nothing arrives soon, explain why (many builds stay silent until
    // asked) — but keep the connection; don't tear it down.
    if (quietTimer) clearTimeout(quietTimer);
    // Same diagnosis the native app gives (desktop/src-tauri/serial_monitor.rs,
    // SILENCE_SECS) — a console that connects and then shows nothing looks like
    // a dead board when the board is usually fine and the LINK is the problem,
    // and browser users deserve the same list of things worth trying rather
    // than the vague version. The one cause that is NOT shared: this side
    // hard-resets before opening, so "it booted before you attached" can't
    // happen here and isn't offered.
    if (!mon.everByted) quietTimer = setTimeout(() => {
      if (!mon.everByted && mon.alive)
        setStatus("Connected, but the board hasn’t said anything yet. That usually isn’t a " +
          "dead board: many builds only speak when asked — press h for its menu. It may also " +
          "be running firmware built without a serial console, or another program (the " +
          "desktop Flasher app, screen, PlatformIO) may be holding the port. " +
          "(If the board just reset, I’ll reconnect on my own.)");
    }, 4000);
    const dec = new TextDecoder();
    let buf = con.textContent || "", sample = "", judged = false;
    try {
      while (mon.alive && mySession === mon.session) {
        const { value, done: fin } = await mon.reader.read();
        if (fin) return "ended";
        mon.everByted = true;
        if (quietTimer) { clearTimeout(quietTimer); quietTimer = null; }
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
          setStatus("✓ It’s talking — your Canary is alive.");
          confettiBurst();
        }
        maybeDiagnose(buf);   // fatal signature → plain-language cause + fix
        maybeIdentity(buf);   // `j` self-manifest → verified-identity card
      }
    } catch { return "error"; }
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
      if (verdict !== "garbage" || !mon.alive) return verdict;
      const next = core.CONSOLE_BAUDS.find((b) => !tried.has(b));
      if (!next) {
        setStatus("None of the usual speeds decoded as clean text — showing it raw at " + defaultBaud + ".");
        mon.manualBaud = true; // stop re-judging
        baudSel.value = String(defaultBaud);
        return await pumpOnce(p, defaultBaud);
      }
      setStatus(`That didn’t look like text at ${baud} — trying ${next}…`);
      con.textContent = "";
      baudSel.value = String(next);
      baud = next;
    }
  }

  // The supervisor: keep the console alive across resets & re-enumerations.
  async function supervise(initialPort) {
    let p = initialPort;
    while (mon.alive) {
      if (mon.forced) { p = mon.forced; mon.forced = null; }
      if (!p) p = await waitForPort("Board reset — reconnecting automatically… (normal for these chips)");
      if (!mon.alive || !p) return;
      reconnectBtn.classList.add("flash-hidden");
      try { await pump(p); } catch {}
      if (!mon.alive) return;
      await stopStreams();
      p = mon.reopenSame ? p : null;   // a deliberate baud change keeps the same port
      mon.reopenSame = false;
      await sleep(150);
    }
  }

  // A USB unplug / re-enumeration of the CURRENT port: cancel the read so the
  // supervisor drops straight into reconnect instead of hanging.
  const onDisconnect = () => {
    if (!mon.alive) return;
    mon.session++;
    try { mon.reader && mon.reader.cancel(); } catch {}
  };
  navigator.serial.addEventListener("disconnect", onDisconnect);

  baudSel.addEventListener("change", async () => {
    mon.manualBaud = true;
    mon.everByted = false;
    mon.reopenSame = true;   // same port, new baud — no "reset" detour
    mon.session++;
    con.textContent = "";
    await stopStreams();     // cancels the blocked read so supervise reopens now
  });

  done.addEventListener("click", async () => {
    mon.alive = false;
    if (mon.waiting) mon.waiting(null);
    navigator.serial.removeEventListener("disconnect", onDisconnect);
    await stopStreams();
    setPhase(phaseConnect());
  });

  supervise(port);

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
// The coach: optional micro-lessons that ride every long wait. Stage-aware
// (a backup teaches backups, a write teaches NVS and slots), one lesson at
// a time, auto-advancing gently (never under prefers-reduced-motion), and
// dismissible for the session — learning is offered, never imposed.
const COACH_KEY = "nursery.coach";
function coachDismissed() {
  try { return sessionStorage.getItem(COACH_KEY) === "off"; } catch { return false; }
}
function attachCoach(card, afterEl) {
  if (coachDismissed() || !state.catalog || !Array.isArray(state.catalog.lessons) ||
      !state.catalog.lessons.length) return null;
  const box = el("aside", "flash-coach");
  box.setAttribute("aria-label", "Optional lesson while you wait");
  const head = el("div", "flash-coach-head");
  head.append(el("span", "flash-coach-kicker", "☕ while it works — a 20-second lesson"));
  const close = el("button", "flash-coach-close", "×");
  close.type = "button";
  close.title = "Hide the lessons for this session";
  close.setAttribute("aria-label", "Hide lessons for this session");
  head.append(close);
  box.append(head);
  const titleEl = el("div", "flash-coach-title");
  const bodyEl = el("p", "flash-coach-body");
  box.append(titleEl, bodyEl);
  const controls = el("div", "flash-coach-controls");
  const next = el("button", "ghost small", "another →");
  controls.append(next);
  box.append(controls);
  afterEl.after(box);

  const shown = [];
  let stageText = "";
  let timer = null;
  let alive = true;
  function show(lesson) {
    if (!lesson) { next.disabled = true; next.textContent = "that’s the deck ✓"; return; }
    shown.push(lesson.id);
    box.classList.remove("flash-coach-swap");
    void box.offsetWidth; // restart the swap animation
    box.classList.add("flash-coach-swap");
    titleEl.textContent = lesson.title;
    bodyEl.textContent = lesson.body;
  }
  function advance() {
    // Detachment retires the coach: setPhase() swaps cards without telling
    // us, and an immortal 14 s timer chain per progress card would pile up
    // fast in a batch session. A dry deck stops the chain too.
    if (!alive || !box.isConnected) {
      alive = false;
      if (timer) clearTimeout(timer);
      return;
    }
    const lesson = core.pickLesson(state.catalog, stageText, shown);
    show(lesson);
    if (lesson) arm();
  }
  function arm() {
    if (timer) clearTimeout(timer);
    // Auto-advance is decorative pacing — calm users page by hand.
    if (!prefersCalm()) timer = setTimeout(advance, 14000);
  }
  next.addEventListener("click", advance);
  close.addEventListener("click", () => {
    alive = false;
    if (timer) clearTimeout(timer);
    try { sessionStorage.setItem(COACH_KEY, "off"); } catch { /* private mode */ }
    box.remove();
  });
  advance();
  return {
    stage(s) {
      // A new stage brings its own lesson — but only if it actually has one
      // unseen; mid-lesson churn for nothing helps nobody.
      stageText = s || "";
      const staged = core.pickLesson(state.catalog, stageText, shown);
      if (staged && staged.stage !== "any" &&
          stageText.toLowerCase().includes(staged.stage)) {
        show(staged);
        arm();
      }
    },
    retire() { alive = false; if (timer) clearTimeout(timer); },
  };
}

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
  const coach = attachCoach(card, reassure);
  // expose esptool log
  const log = el("details", "flash-log");
  log.append(el("summary", null, "show technical log"));
  const pre = el("pre");
  logSink = pre;
  log.append(pre);
  card.append(log);
  return {
    card,
    stage(s) {
      stageEl.textContent = s;
      if (coach) coach.stage(s);
    },
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
  try { await stopVoice(); } catch {}
  try { await stopBle(); } catch {}
  try { if (state.session) await state.session.transport.disconnect(); } catch {}
  state.session = null;
  state.chip = state.mac = state.flashBytes = state.current = null;
  state.report = null;
  state.busy = false;
  // A fresh board (non-silent) starts from the top baud again; a silent
  // disconnect (retry / clean-install escalation) keeps any lowered ceiling so
  // the retry writes at the gentler speed that a write-time failure implied.
  if (!silent) { state.baudCeiling = null; /* caller decides next phase */ }
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
  if (prefersCalm()) return; // celebration stays; the motion politely doesn't
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
