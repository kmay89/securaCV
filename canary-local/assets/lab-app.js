/* lab-app — what the Lab is when it runs as an app (desktop-lab/).

   The website Lab is a guided build line. The desktop app is that AND a
   machine with native reach: it can list the USB ports (`list_ports`),
   hear every Canary announcing itself on the LAN (`fleet_scan`, mDNS) and
   ask a kernel for its fleet report (`witness_discover`). Before this module
   those powers only surfaced inside two benches (Flash over USB, the Witness
   Wall) and in the menu bar tray — the app opened on a website overview and
   read like a table of contents. This module gives the app a Home that
   answers the three questions a person opens it with: what is plugged in,
   what is on my network, and where was I — plus a fleet page, a status strip
   under every view, keyboard shortcuts and a first-run welcome.

   Gated on the Tauri shell AND on `native_capabilities()`: a capability that
   is false never lights a path that can only fail (the iPad shell lists no
   ports and browses no mDNS). On the website none of this renders.

   What it may say about a device is deliberately small and comes only from
   the fields the DISCOVERY.md contract names: name, online, chain, hub, and
   the mDNS TXT facts (fw, model, dt, role). Presence is what the network
   SAID: a board heard in this scan is online; a kernel row that says
   `online: false` is offline; silence is "not heard" — never a claim. The
   word "verified" is never used: it belongs to a chain walked against a key
   pinned at pairing, which the Lab does not hold yet.

   Every invoke() here names a command desktop-lab/src-tauri/src/lib.rs
   registers (canary-local/tests/lab_bundle.test.js holds both directions). */

import { SETTINGS_AVAILABLE, updateState, watchUpdates } from "./lab-settings.js";

export const IN_APP = !!(window.__TAURI__ && window.__TAURI__.core);
const invoke = (cmd, args) => window.__TAURI__.core.invoke(cmd, args);

/** The native shell's answer, filled by probeApp() before the first render. */
export const CAPS = { probed: false, serial: false, serial_list: false, mdns: false,
  discovery: false, notifications: false, self_update: false };

/** The build that is running (`app_info`, the same DTO the Flasher's About
    reads), filled beside the capabilities; empty off-app. */
export const APP_INFO = {};

export async function probeApp() {
  if (!IN_APP) return CAPS;
  try { Object.assign(CAPS, (await invoke("native_capabilities")) || {}); } catch { /* no seam: nothing lights */ }
  try { Object.assign(APP_INFO, (await invoke("app_info")) || {}); } catch { /* shown as — */ }
  CAPS.probed = true;
  // The Home and the strip show the updater's last word; follow it from here
  // (the routine check runs with no Settings panel open) and redraw on change.
  watchUpdates().catch(() => {});
  updateState.listeners.add(() => fleet.emit());
  return CAPS;
}

/** True where the app has something native to show on a Home: ports to list
    or a network to hear. The two app views are routed only when this is. */
export const appViews = () => IN_APP && (CAPS.serial_list || CAPS.mdns || CAPS.discovery);

/* ---- tiny hyperscript (the shell's idiom; CSSOM for style, never an attribute) ---- */
function h(tag, attrs, ...kids) {
  const n = document.createElement(tag);
  if (attrs) for (const [k, v] of Object.entries(attrs)) {
    if (v == null || v === false) continue;
    if (k === "class") n.className = v;
    else if (k === "html") n.innerHTML = v;
    else if (k === "style") n.style.cssText = v;
    else if (k.startsWith("on")) n.addEventListener(k.slice(2).toLowerCase(), v);
    else n.setAttribute(k, v === true ? "" : v);
  }
  for (const c of kids.flat()) {
    if (c == null || c === false) continue;
    n.append(c.nodeType ? c : document.createTextNode(String(c)));
  }
  return n;
}
const ico = (paths) => h("span", { class: "ic", "aria-hidden": "true",
  html: `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round">${paths}</svg>` });
const I = {
  usb: '<path d="M12 3v14"/><path d="M9 6l3-3 3 3"/><circle cx="12" cy="19" r="2"/><path d="M8 11h-2a2 2 0 00-2 2v1M16 9h2a2 2 0 012 2v2"/>',
  net: '<circle cx="12" cy="12" r="2.5"/><path d="M6.3 6.3a8 8 0 000 11.4M17.7 6.3a8 8 0 010 11.4M3.5 3.5a12 12 0 000 17M20.5 3.5a12 12 0 010 17"/>',
  app: '<rect x="3" y="4" width="18" height="14" rx="2"/><path d="M8 21h8M12 18v3"/>',
  flash: '<path d="M13 2L4.5 13H11l-1 9 8.5-11H12z" fill="currentColor" stroke="none"/>',
  home: '<path d="M3 11l9-7 9 7"/><path d="M5 10v9h14v-9"/>',
  fleet: '<rect x="3" y="5" width="18" height="12" rx="2"/><path d="M8 21h8M12 17v4"/>',
  wall: '<rect x="2" y="4" width="20" height="13" rx="2"/><path d="M7 9h4M7 12h7"/>',
  build: '<path d="M14.7 6.3a4 4 0 00-5.4 5.4l-6 6 2 2 6-6a4 4 0 005.4-5.4l-2.3 2.3-2-2z"/>',
  prove: '<path d="M12 3l7 3v5c0 4.5-3 7.5-7 9-4-1.5-7-4.5-7-9V6z"/><path d="M9 12l2 2 4-4"/>',
  map: '<path d="M3 6l6-2 6 2 6-2v14l-6 2-6-2-6 2z"/><path d="M9 4v14M15 6v14"/>',
  go: '<path d="M5 12h14M13 6l6 6-6 6"/>',
  reload: '<path d="M20 12a8 8 0 11-2.3-5.7"/><path d="M20 4v5h-5"/>',
  start: '<path d="M5 3l14 9-14 9z"/>',
};

/* ---- storage (on-device only; nothing here is ever uploaded) ---- */
const KEYS = {
  seen: "scv-fleet-seen",      // deviceId -> the last mDNS facts + when heard
  last: "scv-lab-last",        // the bench you were on
  welcomed: "scv-lab-welcomed",
  kernel: "scv-kernel",        // the typed kernel address (witness-host.js reads the same key)
  progress: "scv-lab-progress",
  benches: "scv-lab-benches",  // every bench ever opened, by slug
  setupHidden: "scv-lab-setup-hidden",
};
// Written by the Flash bench (assets/flash.js INSTALL_TIMES_KEY) when an
// install completed and its bytes matched — read here, never written, as
// the one honest sign that a board was flashed from this computer.
const FLASH_MEMORY_KEY = "canary.flash.installTimes.v1";
const load = (k, fallback) => { try { return JSON.parse(localStorage.getItem(k)) ?? fallback; } catch { return fallback; } };
const save = (k, v) => { try { localStorage.setItem(k, JSON.stringify(v)); } catch { /* private mode: not remembered */ } };

export function rememberBench(entry) {
  if (!entry) return;
  save(KEYS.last, { slug: entry.bench.slug, noun: entry.bench.noun, stage: entry.stage.name, n: entry.stage.n, at: Date.now() });
  const opened = benchesOpened();
  if (!opened.has(entry.bench.slug)) { opened.add(entry.bench.slug); save(KEYS.benches, [...opened]); }
}
export const lastBench = () => load(KEYS.last, null);
export const benchesOpened = () => new Set(load(KEYS.benches, []));
export const kernelAddress = () => { try { return localStorage.getItem(KEYS.kernel) || ""; } catch { return ""; } };
export const welcomed = () => !!load(KEYS.welcomed, false);
export const flashedHere = () => { try { return Object.keys(JSON.parse(localStorage.getItem(FLASH_MEMORY_KEY) || "{}")).length > 0; } catch { return false; } };
export function forgetEverything() {
  for (const k of [KEYS.seen, KEYS.last, KEYS.welcomed, KEYS.progress, KEYS.benches, KEYS.setupHidden]) { try { localStorage.removeItem(k); } catch {} }
}

/* ---- words ---- */
const relTime = (ms) => {
  const d = Date.now() - ms;
  if (d < 90e3) return "just now";
  if (d < 3600e3) return Math.round(d / 60e3) + " min ago";
  if (d < 86400e3) { const n = Math.round(d / 3600e3); return n + (n === 1 ? " hour ago" : " hours ago"); }
  const days = Math.round(d / 86400e3);
  return days === 1 ? "yesterday" : days + " days ago";
};
const plural = (n, one, many) => n + " " + (n === 1 ? one : many);

/** Dotted-version compare for "is this board behind the app's firmware
    train". Anything non-numeric (a dev build) is never called behind. */
export function fwBehind(fw, train) {
  const a = String(fw || "").split("-")[0].split(".").map((n) => parseInt(n, 10));
  const b = String(train || "").split("-")[0].split(".").map((n) => parseInt(n, 10));
  if (a.length < 2 || b.length < 2 || a.some(Number.isNaN) || b.some(Number.isNaN)) return false;
  for (let i = 0; i < 3; i++) { const x = a[i] || 0, y = b[i] || 0; if (x !== y) return x < y; }
  return false;
}

/** The kernel bases to try, in the Apple TV's order: the typed address
    first, then the well-known three (witness-host.js keeps the same list). */
export function kernelBases() {
  const b = [];
  const k = kernelAddress();
  if (/^https?:\/\//i.test(k)) b.push(k);
  b.push("http://canary.local:8099", "http://canary.local:8799", "http://canary.local");
  return [...new Set(b)];
}

/** Where a browsed board serves its own page, when it serves one. A board
    that announces a real port is reachable there; the WAP, the all-rounder
    and the displays serve :80 even when their advert still says port 1 (the
    Flasher's fleet book probes them the same way). A Sense or Vision runs no
    server at all, so it gets no address. */
export function deviceBase(row) {
  const addr = row.ip && !String(row.ip).includes(":") ? row.ip : String(row.host || "").replace(/\.$/, "");
  if (!addr) return null;
  if (row.port && row.port !== 1) return `http://${addr}:${row.port}`;
  if (/wap|display|dash|watch|glance|^canary$/.test(String(row.deviceType || row.role || ""))) return `http://${addr}`;
  return null;
}

/* ---- the fleet poller ------------------------------------------------------
   One controller owns every native read. Two cadences: ACTIVE while a view
   that shows the answers is open (Home, Your fleet), IDLE under any other
   view, where only the status strip reads it. Every read is best-effort and
   wrapped: discovery must never take the app down with it. */
export const fleet = {
  ports: [],          // list_ports, usb kind only
  sightings: [],      // this scan's mDNS answers
  kernel: null,       // the last /api/fleet document, or null
  kernelAt: 0,
  scanAt: 0,          // when the last full scan finished
  scanning: false,
  started: false,
  active: false,
  train: null,        // devices/registry.json fw_train
  timer: null,
  subs: new Set(),
  ACTIVE_MS: 15000,
  IDLE_MS: 60000,

  subscribe(fn) { this.subs.add(fn); return () => this.subs.delete(fn); },
  emit() { for (const fn of this.subs) { try { fn(this); } catch {} } },

  start() {
    if (this.started || !IN_APP) return;
    this.started = true;
    fetch("devices/registry.json", { cache: "no-cache" })
      .then((r) => r.json()).then((reg) => { this.train = reg && reg.fw_train; this.emit(); })
      .catch(() => {});
    document.addEventListener("visibilitychange", () => { if (!document.hidden) this.tick(); });
    this.tick();
  },
  setActive(on) {
    if (this.active === on) return;
    this.active = on;
    if (on && this.started && Date.now() - this.scanAt > 5000) this.tick();
    else this.schedule();
  },
  schedule() {
    if (this.timer) clearTimeout(this.timer);
    if (document.hidden) { this.timer = null; return; }
    this.timer = setTimeout(() => this.tick(), this.active ? this.ACTIVE_MS : this.IDLE_MS);
  },
  async tick() {
    if (this.scanning) return;
    this.scanning = true;
    this.emit();
    try {
      if (CAPS.serial_list) {
        try { this.ports = ((await invoke("list_ports")) || []).filter((p) => p && p.kind === "usb"); }
        catch { /* transient; keep the last list */ }
      }
      if (CAPS.mdns) {
        try {
          this.sightings = (await invoke("fleet_scan", { timeoutMs: 2500 })) || [];
          const seen = load(KEYS.seen, {});
          const now = Date.now();
          for (const s of this.sightings) {
            if (!s || !s.deviceId) continue;
            seen[s.deviceId] = { name: s.name, model: s.model, deviceType: s.deviceType, role: s.role,
              fw: s.fw, ip: s.ip, host: s.host, port: s.port, at: now };
          }
          // Forget a board unheard for a month: a note that old is a guess.
          for (const [id, r] of Object.entries(seen)) if (now - (r.at || 0) > 30 * 86400e3) delete seen[id];
          save(KEYS.seen, seen);
        } catch { this.sightings = []; }
      }
      if (CAPS.discovery) {
        try { this.kernel = await invoke("witness_discover", { bases: kernelBases() }); this.kernelAt = Date.now(); }
        catch { this.kernel = null; }
      }
    } finally {
      this.scanning = false;
      this.scanAt = Date.now();
      this.emit();
      this.schedule();
    }
  },

  /** Every device this app knows, merged: this scan's boards, the kernel's
      rows, and boards heard before (from on-device memory), one row each. */
  devices() {
    const rows = new Map();   // key -> row
    const now = Date.now();
    const seen = load(KEYS.seen, {});
    const live = new Set(this.sightings.map((s) => s && s.deviceId).filter(Boolean));
    for (const [id, r] of Object.entries(seen)) {
      rows.set("m:" + id, { key: "m:" + id, id, name: r.name || id, model: r.model || "", deviceType: r.deviceType || "",
        role: r.role || "", fw: r.fw || "", ip: r.ip || "", host: r.host || "", port: r.port || 0,
        online: live.has(id) ? true : null, heardAt: r.at || 0, chain: null, hub: null, source: "mdns" });
    }
    for (const s of this.sightings) {
      if (!s || !s.deviceId) continue;
      const key = "m:" + s.deviceId;
      const r = rows.get(key) || { key, id: s.deviceId, source: "mdns", chain: null, hub: null };
      Object.assign(r, { name: s.name || s.deviceId, model: s.model || "", deviceType: s.deviceType || "", role: s.role || "",
        fw: s.fw || "", ip: s.ip || "", host: s.host || "", port: s.port || 0, online: true, heardAt: now });
      rows.set(key, r);
    }
    const doc = this.kernel;
    const list = doc && (doc.devices || doc.canaries || (Array.isArray(doc) ? doc : [])) || [];
    for (const d of list) {
      if (!d || typeof d.name !== "string" || !d.name.trim()) continue;
      const name = d.name.trim().slice(0, 40);
      const online = typeof d.online === "boolean" ? d.online : null;
      const word = (v) => (typeof v === "string" && /^[a-z0-9_-]{1,16}$/.test(v) ? v : null);
      const match = [...rows.values()].find((r) => r.name.toLowerCase() === name.toLowerCase());
      if (match) {
        // The kernel's word outranks silence; a board heard this scan stays online.
        if (match.online !== true && online !== null) match.online = online;
        match.chain = word(d.chain); match.hub = word(d.hub); match.source = "both";
        if (!match.fw && typeof d.fw === "string") match.fw = d.fw.slice(0, 24);
      } else {
        rows.set("k:" + name, { key: "k:" + name, id: "", name, model: "", deviceType: "", role: "",
          fw: typeof d.fw === "string" ? d.fw.slice(0, 24) : "", ip: "", host: "", port: 0,
          online, heardAt: 0, chain: word(d.chain), hub: word(d.hub), source: "kernel" });
      }
    }
    const order = (r) => (r.online === true ? 0 : r.online === false ? 1 : 2);
    return [...rows.values()].sort((a, b) => order(a) - order(b) || a.name.localeCompare(b.name));
  },
  onlineCount() { return this.devices().filter((r) => r.online === true).length; },
  kernelName() {
    const k = this.kernel;
    const n = k && !Array.isArray(k) && (k.kernel || k.name || k.hub);
    return typeof n === "string" ? n.trim().slice(0, 40) : "";
  },
};

/** Presence, in the only words the network's report supports. */
export function presenceWord(row) {
  if (row.online === true) return "online";
  if (row.online === false) return "offline";
  if (row.heardAt) return "last heard " + relTime(row.heardAt);
  return "hasn't said";
}

/* ---- pieces ---- */
function deviceRow(row, ctx, { compact = false } = {}) {
  const train = fleet.train;
  const behind = row.fw && train && fwBehind(row.fw, train);
  const base = deviceBase(row);
  const dot = h("span", { class: "dv-dot " + (row.online === true ? "on" : row.online === false ? "off" : "quiet"), "aria-hidden": "true" });
  const facts = [row.model || row.deviceType || (row.source === "kernel" ? "reported by the kernel" : "Canary"),
    row.fw ? "fw " + row.fw : null, compact ? null : row.ip || (row.host ? row.host.replace(/\.$/, "") : null)].filter(Boolean);
  const health = [];
  if (row.hub === "down") health.push(h("span", { class: "dv-tag warn" }, "hub down"));
  if (row.chain && row.chain !== "ok") health.push(h("span", { class: "dv-tag warn" }, "chain " + row.chain));
  if (behind) health.push(h("span", { class: "dv-tag up", title: "This app carries firmware " + train + "; the board reports " + row.fw }, "update to " + train));
  const actions = h("div", { class: "dv-actions" });
  if (base && row.online === true) actions.append(h("a", { class: "btn ghost small", href: base, target: "_blank", rel: "noopener", title: "Open the device's own page in your browser" }, "Open"));
  if (behind && CAPS.serial) actions.append(h("button", { class: "btn ghost small", onclick: () => ctx.goBench("flash"), title: "Flash the newest firmware over USB" }, "Flash"));
  return h("div", { class: "dv-row" },
    dot,
    h("div", { class: "dv-main" },
      h("div", { class: "dv-name" }, row.name, ...health),
      h("div", { class: "dv-sub" }, facts.join(" · "))),
    h("div", { class: "dv-presence " + (row.online === true ? "on" : "") }, presenceWord(row)),
    actions);
}

function emptyFleet(ctx) {
  return h("div", { class: "dv-empty" },
    h("p", {}, CAPS.mdns
      ? "No Canaries heard on this network yet. A Canary announces itself once it joins your Wi-Fi."
      : "This build of the Lab doesn't browse the network for Canaries."),
    h("div", { class: "actions" },
      CAPS.serial ? h("button", { class: "btn primary", onclick: () => ctx.goBench("flash") }, ico(I.flash), "Flash your first Canary") : null,
      h("button", { class: "btn ghost", onclick: () => ctx.goBench("first-boot") }, "First boot, step by step")));
}

function usbLine() {
  const p = fleet.ports[0];
  if (!CAPS.serial_list) return { head: "Not on this device", sub: "This build lists no USB ports.", action: null };
  if (!p) return { head: "Nothing plugged in", sub: "Connect a Canary over a USB-C data cable and it shows up here.", action: null };
  const who = p.product || p.manufacturer || "USB serial board";
  const more = fleet.ports.length > 1 ? ` (+${fleet.ports.length - 1} more)` : "";
  return { head: who + more, sub: p.name + (p.vid != null ? " · USB " + Number(p.vid).toString(16).padStart(4, "0") + ":" + Number(p.pid || 0).toString(16).padStart(4, "0") : ""),
    action: CAPS.serial ? "flash" : null };
}

function kernelLine() {
  const name = fleet.kernelName();
  const rows = fleet.kernel && (fleet.kernel.devices || fleet.kernel.canaries || (Array.isArray(fleet.kernel) ? fleet.kernel : [])) || [];
  if (fleet.kernel) return "Hub " + (name ? "“" + name + "” " : "") + "answering · " + plural(rows.length, "device", "devices") + " reported";
  if (!CAPS.discovery) return "";
  return kernelAddress() ? "No hub answering at " + kernelAddress().replace(/^https?:\/\//, "") : "No hub answering yet";
}

/* ---- the setup path, as the app can see it ----
   Five steps from a bare board to a fleet on the wall, each one answered by
   something this app observes rather than something you tick: a port
   listed, an install the Flash bench remembered, a Canary announcing itself
   (so it joined Wi-Fi), a hub answering, the Wall opened. The first step not
   yet done is the one with the button. A step a build cannot observe is not
   listed — nothing here claims what it cannot see. */
export function setupSteps(ctx) {
  const heard = fleet.devices().some((r) => r.source !== "kernel");
  const usb = usbLine();
  return [
    { id: "plug", title: "Plug a board in over USB", done: fleet.ports.length > 0,
      sub: fleet.ports.length ? usb.head + " on " + fleet.ports[0].name : "Use a USB-C data cable; a charge-only cable shows nothing.",
      go: CAPS.serial ? ["flash", "Flash a board"] : null, show: CAPS.serial_list },
    { id: "flash", title: "Put signed firmware on it", done: flashedHere(),
      sub: "The Flash bench backs the board up, writes the image and checks every byte.",
      go: ["flash", "Flash a board"], show: CAPS.serial },
    { id: "boot", title: "First boot: name it, join your Wi-Fi", done: heard,
      sub: heard ? "A Canary has announced itself on your network." : "Power it up and it hands you a setup network. First boot walks you through it.",
      go: ["first-boot", "First boot"], show: CAPS.mdns },
    { id: "hub", title: "Give it a hub", done: !!fleet.kernel,
      sub: fleet.kernel ? kernelLine() : "A hub keeps the whole fleet's record. Not at canary.local? Type its address on Your fleet.",
      go: ["hub", "Set up the hub"], show: CAPS.discovery },
    { id: "wall", title: "Watch the wall", done: benchesOpened().has("witness-wall"),
      sub: "Your fleet's record on one screen, with the Canaries the app found.",
      go: ["witness-wall", "Watch the wall"], show: true },
  ].filter((s) => s.show);
}
const setupHidden = () => !!load(KEYS.setupHidden, false);

function setupCard(ctx, onChange) {
  const steps = setupSteps(ctx);
  const done = steps.filter((s) => s.done).length;
  const hide = () => { save(KEYS.setupHidden, true); onChange(); };
  const head = h("div", { class: "dash-sec-head" },
    h("h2", {}, "Set up a Canary"),
    h("span", { class: "muted" }, done + " of " + steps.length + " done"),
    h("button", { class: "link", onclick: hide, title: "Hide this list; Updates & about can bring it back" }, "Hide"));
  if (done === steps.length) {
    return h("section", { class: "dash-sec setup" }, head,
      h("p", { class: "set-lede" }, "Every step done. The fleet is yours to watch; the build line below goes deeper."));
  }
  let current = true;
  const rows = steps.map((s, i) => {
    const state = s.done ? "done" : current ? "now" : "later";
    if (!s.done) current = false;
    return h("li", { class: "step " + state },
      h("span", { class: "step-n", "aria-hidden": "true" }, s.done ? h("span", { html: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="3" stroke-linecap="round" stroke-linejoin="round"><path d="M5 12l5 5L20 7"/></svg>' }) : String(i + 1)),
      h("div", { class: "step-main" },
        h("div", { class: "step-t" }, s.title, s.done ? h("span", { class: "step-ok" }, "done") : null),
        h("div", { class: "step-s" }, s.sub)),
      s.go && !s.done ? h("button", { class: "btn small " + (state === "now" ? "primary" : "ghost"), onclick: () => ctx.goBench(s.go[0]) }, s.go[1]) : null);
  });
  return h("section", { class: "dash-sec setup" }, head, h("ol", { class: "steps" }, ...rows));
}

/* ---- Home ---- */
export function dashboardView(ctx) {
  const root = h("div", { class: "dash" });
  const render = () => {
    const devices = fleet.devices();
    const online = devices.filter((r) => r.online === true).length;
    const usb = usbLine();
    const last = lastBench();
    const info = ctx.appInfo() || {};
    const upd = updateState;
    const netCard = h("button", { class: "dcard", onclick: () => ctx.navigate("devices") },
      h("div", { class: "dc-top" }, ico(I.net), h("span", { class: "dc-k" }, "On your network")),
      h("div", { class: "dc-big" }, CAPS.mdns || CAPS.discovery
        ? (devices.length ? plural(online, "Canary online", "Canaries online") + (devices.length > online ? " of " + devices.length + " known" : "") : "No Canaries heard yet")
        : "Not on this device"),
      h("div", { class: "dc-sub" }, kernelLine() || (fleet.scanning ? "Listening…" : "Listens every " + Math.round(fleet.ACTIVE_MS / 1000) + " s while this page is open")),
      h("span", { class: "dc-go", html: `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round">${I.go}</svg>` }));
    const usbCard = h("button", { class: "dcard", onclick: () => ctx.goBench("flash") },
      h("div", { class: "dc-top" }, ico(I.usb), h("span", { class: "dc-k" }, "Over USB")),
      h("div", { class: "dc-big" }, usb.head),
      h("div", { class: "dc-sub" }, usb.sub),
      h("span", { class: "dc-go", html: `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round">${I.go}</svg>` }));
    const appCard = h("button", { class: "dcard", onclick: () => ctx.navigate(SETTINGS_AVAILABLE ? "settings" : "family") },
      h("div", { class: "dc-top" }, ico(I.app), h("span", { class: "dc-k" }, "This app")),
      h("div", { class: "dc-big" }, upd.update ? "Update ready · v" + upd.update.version : info.version ? "SecuraCV Lab v" + info.version : "SecuraCV Lab"),
      h("div", { class: "dc-sub" }, [fleet.train ? "firmware " + fleet.train : null,
        upd.update ? "signed and ready to install" : upd.failed ? "release channel unreachable" : upd.checked ? "up to date" : "checks for updates on its own"].filter(Boolean).join(" · ")),
      h("span", { class: "dc-go", html: `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round">${I.go}</svg>` }));

    const quick = [
      ["flash", "Flash a board", "Put signed firmware on a Canary over USB", I.flash, CAPS.serial],
      ["first-boot", "First boot", "Name it, join Wi-Fi, prove it is alive", I.start, true],
      ["workshop", "Build a case", "Spec a body, print its shell", I.build, true],
      ["hub", "Set up the hub", "Home Assistant, the kernel, the broker", I.home, true],
      ["witness-wall", "Watch the wall", "Your fleet's record, live", I.wall, true],
      ["vault", "Prove it", "Evidence that holds, break-glass", I.prove, true],
    ].filter((q) => q[4]);

    const list = devices.slice(0, 6).map((r) => deviceRow(r, ctx, { compact: true }));
    root.replaceChildren(...[
      h("div", { class: "crumbs" }, h("b", {}, "Home")),
      h("div", { class: "c-h" }, h("h1", {}, "Your Lab")),
      h("p", { class: "verb" }, "What's plugged in, what's on your network, and where you left off."),
      welcomed() ? null : welcomeCard(ctx, () => render()),
      h("div", { class: "dash-grid" }, netCard, usbCard, appCard),
      setupHidden() ? null : setupCard(ctx, () => render()),
      last ? h("section", { class: "dash-sec" },
        h("h2", {}, "Pick up where you left off"),
        h("button", { class: "resume-card", onclick: () => ctx.navigate(last.slug) },
          h("span", { class: "rc-stage" }, last.n + " · " + last.stage),
          h("span", { class: "rc-noun" }, last.noun),
          h("span", { class: "rc-when" }, relTime(last.at)),
          h("span", { class: "dc-go", html: `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round">${I.go}</svg>` }))) : null,
      h("section", { class: "dash-sec" },
        h("div", { class: "dash-sec-head" },
          h("h2", {}, "Your fleet"),
          devices.length > 6 ? h("button", { class: "link", onclick: () => ctx.navigate("devices") }, "All " + devices.length + " →") : null),
        list.length ? h("div", { class: "dv-list" }, ...list) : emptyFleet(ctx)),
      h("section", { class: "dash-sec" },
        h("h2", {}, "Do something"),
        h("div", { class: "quick-grid" }, ...quick.map(([slug, title, sub, icon]) =>
          h("button", { class: "qcard", onclick: () => ctx.goBench(slug) },
            ico(icon), h("b", {}, title), h("span", {}, sub))))),
      h("section", { class: "dash-sec" },
        h("div", { class: "dash-sec-head" },
          h("h2", {}, "The build line"),
          h("button", { class: "link", onclick: () => ctx.navigate("overview") }, "Workshop map →")),
        h("div", { class: "line-chips" }, ...ctx.stages().map((s) =>
          h("button", { class: "lchip" + (ctx.visited().has(s.id) ? " done" : ""), style: `--ac:${s.accent}`, onclick: () => ctx.goBench(ctx.firstOf(s).slug) },
            h("span", { class: "n" }, String(s.n)), h("span", {}, s.name), h("small", {}, s.verb))))),
      h("p", { class: "dash-foot" }, "Presence is what the network reported. Nothing here is verified: the Lab holds no pairing key yet."),
    ].filter(Boolean));
  };
  render();
  const off = fleet.subscribe(render);
  root.addEventListener("scv:leave", off, { once: true });
  return root;
}

function welcomeCard(ctx, onDone) {
  const close = () => { save(KEYS.welcomed, true); onDone(); };
  return h("section", { class: "welcome" },
    h("div", { class: "w-head" }, h("b", {}, "Welcome to the Lab"), h("button", { class: "link", onclick: close }, "Dismiss")),
    h("p", {}, "Everything here runs on this computer. Pick where to start:"),
    h("div", { class: "w-paths" },
      h("button", { class: "wpath", onclick: () => { close(); ctx.goBench(CAPS.serial ? "flash" : "first-boot"); } },
        ico(I.flash), h("b", {}, "I have a board"), h("span", {}, "Plug it in over USB and put signed firmware on it.")),
      h("button", { class: "wpath", onclick: () => { close(); ctx.navigate("devices"); } },
        ico(I.fleet), h("b", {}, "I have Canaries running"), h("span", {}, "See who is on your network and open each one.")),
      h("button", { class: "wpath", onclick: () => { close(); ctx.navigate("overview"); } },
        ico(I.map), h("b", {}, "Just exploring"), h("span", {}, "Walk the workshop: choose, build, flash, sense, home, prove."))));
}

/* ---- Your fleet ---- */
export function devicesView(ctx) {
  const root = h("div", { class: "devices" });
  const addr = h("input", { type: "url", class: "kaddr", id: "kaddr", placeholder: "http://192.168.1.40:8099", value: kernelAddress(),
    spellcheck: "false", autocomplete: "off" });
  const saveAddr = async () => {
    const v = addr.value.trim();
    if (v && !/^https?:\/\/[^\s/]+/i.test(v)) { note.textContent = "Type the address as http://host or http://host:port."; return; }
    try { if (v) localStorage.setItem(KEYS.kernel, v); else localStorage.removeItem(KEYS.kernel); } catch {}
    note.textContent = v ? "Saved. The tray and the Witness Wall use it too." : "Cleared — the well-known addresses are tried.";
    if (CAPS.notifications) { try { await invoke("companion_set_bases", { bases: kernelBases() }); } catch { /* the tray keeps what it had */ } }
    fleet.tick();
  };
  const note = h("span", { class: "muted" });
  const render = () => {
    const devices = fleet.devices();
    const online = devices.filter((r) => r.online === true).length;
    root.replaceChildren(
      h("div", { class: "crumbs" }, h("b", {}, "Your fleet")),
      h("div", { class: "c-h" }, h("h1", {}, "Your fleet"),
        h("span", { class: "scan-pill" + (fleet.scanning ? " live" : "") }, fleet.scanning ? "listening…" : fleet.scanAt ? "checked " + relTime(fleet.scanAt) : "")),
      h("p", { class: "verb" }, devices.length
        ? plural(online, "Canary online", "Canaries online") + " · " + plural(devices.length, "device known", "devices known") + " to this app"
        : "Every Canary that announces itself on your network lands here, with its firmware and address."),
      devices.length ? h("div", { class: "dv-list" }, ...devices.map((r) => deviceRow(r, ctx))) : emptyFleet(ctx),
      h("section", { class: "dash-sec" },
        h("h2", {}, "Your hub"),
        h("p", { class: "set-lede" }, kernelLine() || "A hub keeps the record for the whole fleet and answers for every Canary on it. The app looks for one at canary.local; type its address if it lives elsewhere."),
        h("label", { class: "kaddr-label", for: "kaddr" }, "Hub address"),
        h("div", { class: "kaddr-row" }, addr, h("button", { class: "btn ghost", onclick: saveAddr }, "Save"), note),
        h("div", { class: "actions" },
          h("button", { class: "btn ghost", onclick: () => ctx.goBench("witness-wall") }, ico(I.wall), "Watch the wall"),
          h("button", { class: "btn ghost", onclick: () => ctx.goBench("hub") }, ico(I.home), "Set up a hub"),
          h("button", { class: "btn ghost", onclick: () => fleet.tick() }, ico(I.reload), "Listen again"))),
      h("p", { class: "dash-foot" },
        "Presence is what the network reported: a board heard in the last scan is online, one the hub reports offline is offline, and one that went quiet reads last heard. " +
        "Nothing here is verified — the Lab holds no pairing key yet — and nothing leaves this computer."),
    );
  };
  render();
  const off = fleet.subscribe(() => { if (document.activeElement !== addr) render(); });
  root.addEventListener("scv:leave", off, { once: true });
  return root;
}

/* ---- the status strip under every view ---- */
export function statusStrip(ctx) {
  const bar = h("footer", { class: "foot app-foot", role: "status", "aria-live": "polite" });
  const chip = (icon, text, to, cls) => h("button", { class: "fchip " + (cls || ""), onclick: to }, ico(icon), h("span", {}, text));
  const render = () => {
    const devices = fleet.devices();
    const online = devices.filter((r) => r.online === true).length;
    const usb = fleet.ports[0];
    const info = ctx.appInfo() || {};
    bar.replaceChildren(...[
      (CAPS.mdns || CAPS.discovery) ? chip(I.net, devices.length ? online + " of " + devices.length + " online" : fleet.scanning ? "listening…" : "no Canaries heard", () => ctx.navigate("devices"), online ? "on" : "") : null,
      CAPS.serial_list ? chip(I.usb, usb ? (usb.product || "board") + " on " + usb.name : "no board over USB", () => ctx.goBench("flash"), usb ? "on" : "") : null,
      CAPS.discovery ? chip(I.home, fleet.kernel ? "hub " + (fleet.kernelName() || "answering") : "no hub", () => ctx.navigate("devices"), fleet.kernel ? "on" : "") : null,
      h("span", { class: "grow" }),
      chip(I.app, "v" + (info.version || "—") + (fleet.train ? " · fw " + fleet.train : "") + (updateState.update ? " · update ready" : ""),
        () => ctx.navigate(SETTINGS_AVAILABLE ? "settings" : "family"), updateState.update ? "up" : ""),
    ].filter(Boolean));
  };
  render();
  fleet.subscribe(render);
  return bar;
}

/* ---- keyboard shortcuts (the shell's own document; a framed bench keeps its own keys) ---- */
export const SHORTCUTS = [
  ["⌘ K", "Search benches"],
  ["⌘ ⇧ H", "Home"],
  ["⌘ 1 – 6", "Jump to a stage of the build line"],
  ["⌘ [ / ⌘ ]", "Previous / next bench"],
  ["⌘ R", "Reload the bench you are on"],
  ["⌘ ,", "Settings"],
];
/** One chord, one answer — the same for a key pressed in the shell's own
    document and for one a framed bench forwarded (lab-nav.js). `typing`
    is whether a text field had focus: a digit or a bracket there is
    their own text. Returns whether the chord was taken. */
export function handleShortcut(ctx, { key, shiftKey = false, typing = false }) {
  const k = String(key || "").toLowerCase();
  if (k === "k") { ctx.focusSearch(); return true; }
  if (k === "h" && shiftKey) { ctx.navigate("dashboard"); return true; }
  if (k === ",") { if (SETTINGS_AVAILABLE) ctx.navigate("settings"); return true; }
  if (k === "r") { ctx.reloadBench(); return true; }
  if (typing) return false;
  if (/^[1-6]$/.test(k) && !shiftKey) { const s = ctx.stages()[Number(k) - 1]; if (s) ctx.goBench(ctx.firstOf(s).slug); return true; }
  if (k === "[" || k === "]") { ctx.step(k === "]" ? 1 : -1); return true; }
  return false;
}
export function installShortcuts(ctx) {
  window.addEventListener("keydown", (e) => {
    const mod = e.metaKey || e.ctrlKey;
    if (!mod || e.altKey) return;
    const t = e.target;
    const typing = !!t && (/^(input|textarea|select)$/i.test(t.tagName || "") || t.isContentEditable === true);
    if (handleShortcut(ctx, { key: e.key, shiftKey: e.shiftKey, typing })) e.preventDefault();
  });
  // The same chords, pressed inside a framed bench: focus is in the frame
  // after any click there, and its keydowns never reach this document, so
  // lab-nav.js forwards them. Only from the bench actually on screen, same
  // origin, and only the key itself — one character, nothing to echo.
  window.addEventListener("message", (e) => {
    if (e.origin !== location.origin) return;
    const d = e.data;
    if (!d || d.source !== "scv-lab" || d.type !== "key") return;
    const frame = document.querySelector(".bench-frame");
    if (!frame || e.source !== frame.contentWindow) return;
    handleShortcut(ctx, { key: String(d.key || "").slice(0, 1), shiftKey: d.shiftKey === true, typing: d.typing === true });
  });
}
