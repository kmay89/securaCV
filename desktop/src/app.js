// SecuraCV Lab — front-end glue.
//
// Plain JS on purpose: no framework, no build step. It talks to the Rust
// backend through Tauri's global bridge (withGlobalTauri) and never touches
// Web Serial — the OS-native flashing all happens in Rust.
//
// This file is two layers:
//   1. The *shell* — a custom, native-feeling app (left rail, splash, health
//      strip, day/night, and session memory so you land back where you left
//      off). It wears the securacv.com skin.
//   2. The *flows* — the Canary flasher, the Raspberry-Pi hub writer, the
//      serial monitor and self-update. Every backend `invoke`/`listen` call
//      and the whole state machine are unchanged from the original; the shell
//      only reorganizes where they live on screen.
//
// The connection is watched *live*, IDE-style: a background poll enumerates the
// USB ports every second and a persistent status bar reflects the state —
// scanning → found → reading chip → connected → (or) unplugged — with no
// "Connect" button to press.

const { invoke } = window.__TAURI__.core;
const { listen } = window.__TAURI__.event;

const $ = (id) => document.getElementById(id);
const openExternal = (url) =>
  invoke("plugin:opener|open_url", { url }).catch(() => {});

// Fold case/spacing/hyphens so "ESP32-S3" / "esp32s3" compare equal — the
// same guard the Rust side and the website use.
const normChip = (s) => String(s || "").toUpperCase().replace(/[\s\-_]+/g, "");

const POLL_MS = 1000;
const WE2_VID = 0x1a86;
const WE2_PID = 0x55d3;

const state = {
  catalog: null,
  manifest: null,
  hatch: null,
  appInfo: null,
  port: null,        // the port we're currently tracking
  portInfo: null,
  portKind: null,    // "esp32" | "we2"
  chip: null,        // identified chip for state.port, or null
  product: null,
  detecting: false,  // a detect_chip call is in flight
  failedPort: null,  // a port whose chip read failed — don't auto-retry it
  busy: false,       // a flash is running — pause the watcher
  monitoring: false,
  update: null,      // pending self-update, if any
  vision: {
    hostFlash: null,
    hostBoot: null,
    module: null,
  },
};

// ── session memory ───────────────────────────────────────────────────────────
// A tiny, local, non-secret memory so the app makes sense session after
// session. It never stores a Wi-Fi or MQTT *password* — only the harmless
// facts that make jumping back in seamless.
const PREFS_KEY = "securacv.lab.prefs.v2";
const prefs = loadPrefs();

function loadPrefs() {
  try {
    return JSON.parse(localStorage.getItem(PREFS_KEY)) || {};
  } catch {
    return {};
  }
}
function savePrefs() {
  try {
    localStorage.setItem(PREFS_KEY, JSON.stringify(prefs));
  } catch {
    /* private mode / quota — memory is a nicety, never a requirement */
  }
}

const PROV_FIELDS = ["device-id", "wifi-ssid", "mqtt-host", "mqtt-port", "mqtt-user"];
function persistProv() {
  prefs.prov = prefs.prov || {};
  PROV_FIELDS.forEach((id) => { prefs.prov[id] = $(id).value; });
  prefs.hubSsid = $("hub-ssid").value;
  savePrefs();
}
function restoreProv() {
  if (prefs.prov) PROV_FIELDS.forEach((id) => { if (prefs.prov[id]) $(id).value = prefs.prov[id]; });
  if (prefs.hubSsid) $("hub-ssid").value = prefs.hubSsid;
}

function rosterAdd(entry) {
  entry.ts = Date.now();
  prefs.roster = prefs.roster || [];
  prefs.roster.unshift(entry);
  prefs.roster = prefs.roster.slice(0, 12);
  savePrefs();
}

// A short, local event log — so a failure is visible and recoverable, never
// silent. Powers the health story on the About page.
function logEvent(kind, msg) {
  prefs.log = prefs.log || [];
  prefs.log.unshift({ t: Date.now(), kind, msg: String(msg).slice(0, 160) });
  prefs.log = prefs.log.slice(0, 40);
  savePrefs();
  if (!$("about-view").classList.contains("hidden")) renderAbout();
}

// ── theme (day / night), applied before first paint ──────────────────────────
(function applyStoredTheme() {
  const t = prefs.theme;
  if (t === "light" || t === "dark") document.documentElement.setAttribute("data-theme", t);
})();

function effectiveDark() {
  const t = document.documentElement.getAttribute("data-theme");
  if (t === "dark") return true;
  if (t === "light") return false;
  return window.matchMedia("(prefers-color-scheme: dark)").matches;
}
function toggleTheme() {
  const next = effectiveDark() ? "light" : "dark";
  prefs.theme = next;
  savePrefs();
  document.documentElement.setAttribute("data-theme", next);
  logEvent("info", "Switched to " + next + " mode");
}

// ── boot ─────────────────────────────────────────────────────────────────────
async function boot() {
  initShell();

  try {
    state.catalog = await invoke("load_catalog");
  } catch (e) {
    setConn("failed", "Couldn't load the catalog: " + e);
    logEvent("err", "Catalog load failed: " + e);
  }
  try {
    state.hatch = await invoke("load_hatch"); // shared Hatchery naming spec
  } catch (_) {
    state.hatch = null; // certificate degrades to name-less if unavailable
  }

  loadAppInfo();          // build number, rev, build date, firmware train
  restoreProv();          // remember the non-secret fields from last time
  restoreSession();       // land back where you left off

  document.querySelectorAll("[data-open]").forEach((a) =>
    a.addEventListener("click", (ev) => {
      ev.preventDefault();
      openExternal(a.dataset.open);
    })
  );

  $("flash-btn").addEventListener("click", onFlash);
  $("module-flash-btn").addEventListener("click", onFlashModule);
  $("monitor-start").addEventListener("click", startMonitor);
  $("monitor-stop").addEventListener("click", stopMonitor);
  $("monitor-manifest").addEventListener("click", () =>
    // The backend no longer auto-appends a newline (so "No line ending" is
    // honored), so callers that want one include it — the 'j' manifest does.
    invoke("serial_monitor_send", { command: "j\n" }).catch((e) =>
      setStatus("monitor-status", String(e), "err"))
  );
  // Arduino-style controls: pause (freeze autoscroll), clear, expand, and a
  // free-text command line with a line-ending picker.
  $("monitor-pause").addEventListener("click", () => {
    state.monitorPaused = !state.monitorPaused;
    const b = $("monitor-pause");
    b.textContent = state.monitorPaused ? "Resume" : "Pause";
    b.setAttribute("aria-pressed", String(state.monitorPaused));
    if (!state.monitorPaused) { const c = $("serial-console"); c.scrollTop = c.scrollHeight; }
  });
  $("monitor-clear").addEventListener("click", () => { $("serial-console").textContent = ""; });
  $("monitor-expand").addEventListener("click", () => {
    const on = $("serial-monitor").classList.toggle("monitor-expanded");
    const b = $("monitor-expand");
    b.textContent = on ? "Shrink" : "Expand";
    b.setAttribute("aria-pressed", String(on));
  });
  $("monitor-cmd-form").addEventListener("submit", (ev) => {
    ev.preventDefault();
    const input = $("monitor-cmd");
    const cmd = input.value;
    if (!cmd) return;
    const endMap = { "\\n": "\n", "\\r\\n": "\r\n", "\\r": "\r", "": "" };
    const ending = endMap[$("monitor-lineend").value] ?? "\n";
    invoke("serial_monitor_send", { command: cmd + ending })
      .then(() => { input.value = ""; })
      .catch((e) => setStatus("monitor-status", String(e), "err"));
  });
  $("update-btn").addEventListener("click", onInstallUpdate);
  const rerollBtn = $("cert-reroll");
  if (rerollBtn) rerollBtn.addEventListener("click", rerollCertificate);
  const printBtn = $("cert-print");
  if (printBtn) printBtn.addEventListener("click", () => {
    // The print stylesheet isolates just the certificate on the page.
    document.body.classList.add("printing-cert");
    const done = () => document.body.classList.remove("printing-cert");
    if (window.matchMedia) { const m = window.matchMedia("print"); const fn = (e) => { if (!e.matches) { done(); m.removeListener(fn); } }; m.addListener(fn); }
    window.addEventListener("afterprint", done, { once: true });
    try { window.print(); } catch (_) { done(); }
  });
  $("update-dismiss").addEventListener("click", () =>
    $("update-banner").classList.add("hidden")
  );
  // Manual re-read: clear any failure and force a fresh identify next tick.
  $("recheck").addEventListener("click", () => {
    state.failedPort = null;
    state.chip = null;
    pollPorts();
  });
  // Switching ports (multiple plugged in) restarts identification for that one.
  $("port-select").addEventListener("change", (e) => {
    stopMonitor();
    state.port = e.target.value;
    state.portInfo = null;
    state.portKind = null;
    state.chip = null;
    state.failedPort = null;
    resetSteps();
    pollPorts();
  });
  PROV_FIELDS.forEach((id) => $(id).addEventListener("input", persistProv));

  await listen("serial:log", (ev) => appendConsole("serial-console", ev.payload));
  await listen("serial:status", (ev) => {
    $("monitor-status").textContent = ev.payload;
    if (String(ev.payload).toLowerCase().includes("stopped") ||
        String(ev.payload).toLowerCase().includes("could not")) {
      setMonitorButtons(false);
    }
  });
  await listen("serial:receipt", (ev) => {
    state.vision.hostBoot = ev.payload;
    renderReceipts();
    maybeHatch();
  });
  await listen("serial:vision", (ev) => {
    const host = state.vision.hostFlash;
    if (!host || !host.product_id.includes("vision") ||
        Number(ev.payload && ev.payload.module_id) <= 0) return;
    // Compatibility with the already-published 2.2.0 image: it prints the
    // live Grove ID but predates the `j` manifest command. The release hash is
    // already in hostFlash; this boot line proves that exact image reached its
    // Vision I²C init. New images replace this with the full manifest receipt.
    state.vision.hostBoot = {
      target: "esp32-host",
      ready: true,
      manifest: { board: "canary-vision", health: null },
      vision: ev.payload,
      compatibility: "live-boot-line",
    };
    renderReceipts();
    maybeHatch();
  });
  await listen("vision:log", (ev) => appendConsole("console", ev.payload + "\n"));
  await listen("vision:progress", (ev) => {
    $("module-progress").textContent = Math.round(Number(ev.payload) * 100) + "%";
  });

  checkForUpdate();     // best-effort, in the background
  pollPorts();          // first tick now…
  setInterval(pollPorts, POLL_MS); // …then keep watching
}

// ── the shell: rail router, splash, health strip ─────────────────────────────
const VIEWS = ["canary", "hub", "atlas", "fleet", "about"];

// The Witness Wall lives in an isolated iframe (witness/witness.html). After a
// successful flash we tell it a device appeared, so the Canary you just flashed
// shows up on the wall. Wrapped so it can NEVER interfere with the flash flow.
function witnessName(product) {
  const el = $("device-id");
  const typed = el && el.value && el.value.trim();
  return String(typed || (product && (product.name || product.id)) || "New Canary").slice(0, 40);
}
function announceToWitness(product) {
  try {
    const frame = $("witness-frame");
    if (frame && frame.contentWindow) {
      frame.contentWindow.postMessage(
        { type: "witness:appear", name: witnessName(product), label: "just flashed" }, "*");
    }
    const badge = $("badge-fleet");
    if (badge) { badge.textContent = "new"; badge.classList.remove("hidden"); }
  } catch (_) { /* the wall must never break a flash */ }
}

// The real thing: the device we just flashed joins the Wi-Fi we provisioned,
// so — because this is the native app, not a sandboxed browser — we can find it
// on the LAN and populate the wall with the REAL fleet. The Rust `witness_discover`
// command does the LAN reach (no CSP; `.local` resolves via the OS). We poll it
// while the board boots + joins Wi-Fi; if nothing answers (or an older build),
// the simulated appearance from announceToWitness() stands. Never throws into
// the flash flow.
function witnessBases() {
  const bases = [];
  const host = $("mqtt-host") && $("mqtt-host").value && $("mqtt-host").value.trim();
  if (host) { bases.push("http://" + host + ":8099"); bases.push("http://" + host); }
  bases.push("http://canary.local:8099", "http://canary.local");
  return bases;
}
function discoverAndPopulate(product) {
  try {
    const frame = $("witness-frame");
    const post = (m) => { try { if (frame && frame.contentWindow) frame.contentWindow.postMessage(m, "*"); } catch (_) {} };
    const bases = witnessBases();
    const deadline = Date.now() + 30000;
    const tick = async () => {
      let fleet = null;
      try { fleet = await invoke("witness_discover", { bases }); } catch (_) { /* not up yet, or older build */ }
      if (fleet) { post({ type: "witness:fleet", fleet, highlight: witnessName(product) }); return; }
      if (Date.now() < deadline) setTimeout(tick, 2500);
    };
    tick();
  } catch (_) { /* discovery is best-effort — never affects flashing */ }
}

function initShell() {
  document.querySelectorAll(".nav-item").forEach((b) =>
    b.addEventListener("click", () => navigate(b.dataset.nav))
  );
  $("theme-toggle").addEventListener("click", toggleTheme);
  $("health-about").addEventListener("click", () => navigate("about"));
  $("health-update").addEventListener("click", () => navigate("about"));
  $("splash").addEventListener("click", dismissSplash);
  $("resume-go").addEventListener("click", () => {
    navigate(prefs.view && VIEWS.includes(prefs.view) ? prefs.view : "canary");
    $("resume").classList.add("hidden");
  });
  $("resume-dismiss").addEventListener("click", () => {
    prefs.roster = [];
    savePrefs();
    $("resume").classList.add("hidden");
  });

  window.addEventListener("online", updateNet);
  window.addEventListener("offline", updateNet);
  updateNet();

  requestAnimationFrame(() => $("app").classList.add("ready"));
  // Splash lingers just long enough to read the build line, then fades.
  setTimeout(dismissSplash, 1700);
}

function dismissSplash() {
  $("splash").classList.add("gone");
}

function navigate(view) {
  if (!VIEWS.includes(view)) view = "canary";
  VIEWS.forEach((v) => $(v + "-view").classList.toggle("hidden", v !== view));
  document.querySelectorAll(".nav-item").forEach((b) => {
    const active = b.dataset.nav === view;
    b.classList.toggle("active", active);
    b.setAttribute("aria-selected", String(active));
  });
  document.body.dataset.view = view;

  hub.active = view === "hub";
  if (view === "hub") {
    hubLoadPlan();
    hubPreflight();
    hubPollTargets();
    if (!hub.pollTimer) hub.pollTimer = setInterval(hubPollTargets, 2000);
  } else {
    if (hub.pollTimer) {
      clearInterval(hub.pollTimer);
      hub.pollTimer = null;
    }
    // Don't leave a hidden rpiboot waiting forever behind another tab —
    // stop it; the panel says how to start again.
    if (hub.piUsbWaiting) invoke("hub_pi_boot_stop").catch(() => {});
    // The first-boot watch keeps polling in the background either way, so it
    // survives a tab switch — the user may leave to check other things.
  }
  if (view === "atlas") renderAtlas();
  if (view === "about") renderAbout();
  if (view === "fleet") { const b = $("badge-fleet"); if (b) b.classList.add("hidden"); }

  prefs.view = view;
  savePrefs();
}

function restoreSession() {
  // Re-open the section you were last in.
  if (prefs.view && VIEWS.includes(prefs.view) && prefs.view !== "canary") {
    navigate(prefs.view);
  }
  updateResumeState();
}

function updateResumeState() {
  const roster = prefs.roster || [];
  if (!roster.length) { $("resume").classList.add("hidden"); return; }
  const last = roster[0];
  const when = relativeTime(last.ts);
  const what =
    last.kind === "hub"
      ? "you built a Home Assistant hub"
      : `you hatched ${article(last.name)} ${esc(last.name)}${last.chip ? " (" + esc(last.chip) + ")" : ""}`;
  $("resume-body").innerHTML =
    `Last time ${what} — ${when}. ${roster.length} device${roster.length === 1 ? "" : "s"} in this session's roster.`;
  $("resume").classList.remove("hidden");
}

// ── app info: build number, rev, build date, firmware train ──────────────────
async function loadAppInfo() {
  let info = null;
  try {
    info = await invoke("app_info");
  } catch {
    // Older backend without app_info — fall back to the Tauri app version.
    let version = "0.0.0";
    try { version = await window.__TAURI__.app.getVersion(); } catch {}
    info = { version, build_rev: "source", build_epoch: 0, fw_train: null };
  }
  if (!info.fw_train && state.catalog) info.fw_train = state.catalog.fw_train || null;
  state.appInfo = info;

  // "Last updated": the moment we first saw this exact version run.
  if (prefs.lastVersion !== info.version) {
    if (prefs.lastVersion) logEvent("ok", `Updated to v${info.version}`);
    prefs.lastVersion = info.version;
    prefs.lastVersionAt = Date.now();
    savePrefs();
  }
  renderBuildChrome();
}

function buildLabel() {
  const i = state.appInfo;
  if (!i) return "—";
  return "v" + i.version + (i.build_rev && i.build_rev !== "source" ? " · " + i.build_rev : "");
}

function renderBuildChrome() {
  const i = state.appInfo || {};
  $("rail-build").innerHTML = `<b>v${esc(i.version || "—")}</b><br>${esc(i.build_rev || "build")}`;
  $("health-build").textContent = buildLabel();
  $("health-fw").textContent = i.fw_train || "—";
  const meta = [];
  meta.push(`SecuraCV Lab · ${buildLabel()}`);
  if (i.build_epoch) meta.push(`built ${fmtDate(i.build_epoch * 1000)}`);
  $("splash-meta").innerHTML = meta.map(esc).join("<br>") + `<br><b id="splash-upd">checking for updates…</b>`;
}

function updateNet() {
  const online = navigator.onLine;
  $("health-net").classList.toggle("off", !online);
  $("health-net-text").textContent = online ? "online" : "offline";
}

// ── the live watcher ─────────────────────────────────────────────────────────
async function pollPorts() {
  if (state.busy || state.monitoring) return; // don't poke a port another task owns

  let ports;
  try {
    ports = await invoke("list_ports");
  } catch {
    return; // transient; try again next tick
  }
  const usb = ports.filter((p) => p.kind === "usb");
  syncPortSelect(usb);

  const candidate = pickCandidate(usb);

  // Nothing connected → reset to the idle "scanning" state.
  if (!candidate) {
    if (state.port) onDisconnect();
    else setConn("idle", "Scanning for a Canary — plug one in over USB-C.");
    return;
  }

  // A different port than we were tracking → start fresh on it.
  if (candidate.name !== state.port) {
    state.port = candidate.name;
    state.portInfo = candidate;
    state.portKind = null;
    state.chip = null;
    state.failedPort = null;
    resetSteps();
  }
  state.portInfo = candidate;

  if (state.chip) return;                    // already identified — steady state
  if (state.detecting) return;               // identify already in flight
  if (state.failedPort === state.port) {     // failed once; wait for re-read / replug
    return;
  }
  await identify(candidate);
}

function pickCandidate(usb) {
  if (!usb.length) return null;
  const sel = $("port-select");
  if (usb.length > 1 && sel.value) {
    const chosen = usb.find((p) => p.name === sel.value);
    if (chosen) return chosen;
  }
  return usb[0];
}

// Talk to the board once to read its chip. This is the only step that resets
// the board; the poll never does it more than once per freshly-seen port.
async function identify(portInfo) {
  const port = portInfo.name;
  const label = portInfo.product ? `${port} (${portInfo.product})` : port;
  state.detecting = true;
  if (Number(portInfo.vid) === WE2_VID && Number(portInfo.pid) === WE2_PID) {
    state.portKind = "we2";
    state.chip = "WE2";
    setConn("connected", `Connected · Grove Vision AI V2 module on ${port}`);
    $("recheck").classList.remove("hidden");
    showModuleFlow();
    state.detecting = false;
    return;
  }
  state.portKind = "esp32";
  setConn("reading", `Found ${label} — reading chip…`);
  $("download-mode").classList.add("hidden");
  try {
    const chip = await invoke("detect_chip", { port });
    if (port !== state.port) return; // unplugged/switched while we were reading
    state.chip = chip;
    setConn("connected", `Connected · ${chip} on ${port}`);
    $("recheck").classList.remove("hidden");

    invoke("fetch_manifest", { manifestUrl: state.catalog.manifest_url })
      .then((m) => {
        state.manifest = m;
        renderProducts();
      })
      .catch(() => {});
    renderProducts();
    enableCard("step-pick");
  } catch (e) {
    if (port !== state.port) return;
    state.failedPort = port;
    setConn("failed", `Found ${port} — couldn't read the chip. Put it in download mode.`);
    $("download-mode").classList.remove("hidden");
    $("recheck").classList.remove("hidden");
  } finally {
    state.detecting = false;
  }
}

function onDisconnect() {
  stopMonitor();
  state.port = null;
  state.portInfo = null;
  state.portKind = null;
  state.chip = null;
  state.product = null;
  state.failedPort = null;
  resetSteps();
  $("recheck").classList.add("hidden");
  $("port-select").classList.add("hidden");
  $("download-mode").classList.add("hidden");
  setConn("idle", "Scanning for a Canary — plug one in over USB-C.");
}

function resetSteps() {
  $("step-pick").classList.add("disabled");
  $("step-flash").classList.add("disabled");
  $("product-list").innerHTML = "";
  $("pick-sub").textContent = "We'll only show images built for your board's chip.";
  $("flash-btn").disabled = true;
  $("flash-target").textContent = "";
  $("console").classList.add("hidden");
  setStatus("flash-result", "");
  $("provisioning").classList.add("hidden");
  $("host-flash-controls").classList.remove("hidden");
  $("module-flow").classList.add("hidden");
  $("serial-monitor").classList.add("hidden");
  renderReceipts();
  maybeHatch();
}

let lastPortKey = "";
function syncPortSelect(usb) {
  const sel = $("port-select");
  const key = usb.map((p) => p.name).join("|");
  if (key !== lastPortKey) {
    lastPortKey = key;
    const prev = sel.value;
    sel.innerHTML = "";
    usb.forEach((p) => {
      const o = document.createElement("option");
      o.value = p.name;
      o.textContent = p.product ? `${p.name} — ${p.product}` : p.name;
      sel.appendChild(o);
    });
    if (prev && usb.some((p) => p.name === prev)) sel.value = prev;
  }
  sel.classList.toggle("hidden", usb.length <= 1);
}

// ── step 2: pick a firmware image (chip-guarded) ────────────────────────────
function renderProducts() {
  const list = $("product-list");
  list.innerHTML = "";
  const matches = (state.catalog.products || []).filter(
    (p) => normChip(p.chip) === normChip(state.chip)
  );

  if (!matches.length) {
    $("pick-sub").textContent = `No firmware in the catalog targets ${state.chip}.`;
    return;
  }
  $("pick-sub").textContent = `Images built for your ${state.chip}:`;

  const selectedId = state.product ? state.product.id : null;

  matches.forEach((p) => {
    const ver =
      state.manifest &&
      state.manifest.products &&
      state.manifest.products[p.id] &&
      state.manifest.products[p.id].version;

    const isSelected = p.id === selectedId;
    const row = document.createElement("label");
    row.className = "product" + (isSelected ? " selected" : "");
    row.innerHTML = `
      <input type="radio" name="product" value="${p.id}"${isSelected ? " checked" : ""}>
      <span>
        <span class="p-name">${esc(p.name)}<span class="chip-badge">${esc(p.chip)}</span></span>
        <span class="p-tag">${esc(p.tagline || "")}</span>
        <span class="p-meta">${
          ver ? "release " + esc(ver) : "no published release yet"
        }</span>
      </span>`;
    const radio = row.querySelector("input");
    radio.addEventListener("change", () => {
      document.querySelectorAll(".product").forEach((el) => el.classList.remove("selected"));
      row.classList.add("selected");
      state.product = p;
      onProductChosen(p, ver);
    });
    list.appendChild(row);
    if (isSelected) onProductChosen(p, ver);
  });
}

function onProductChosen(p, ver) {
  enableCard("step-flash");
  state.portKind = "esp32";
  $("module-flow").classList.add("hidden");
  $("host-flash-controls").classList.remove("hidden");
  $("serial-monitor").classList.remove("hidden");
  $("provisioning").classList.toggle("hidden", p.provisioning !== "usb-secrets");
  if (p.provisioning === "usb-secrets" && !$("device-id").value) {
    const family = p.id.includes("vision")
      ? "canary_vision"
      : p.id.includes("sense")
        ? "canary_sense"
        : "canary";
    const suffix = Array.from(crypto.getRandomValues(new Uint8Array(2)))
      .map((b) => b.toString(16).padStart(2, "0")).join("");
    $("device-id").value = `${family}_${suffix}`;
    $("mqtt-host").value ||= "homeassistant.local";
  }
  const btn = $("flash-btn");
  btn.disabled = !ver;
  $("flash-target").textContent = ver
    ? `${p.name} → ${state.port}`
    : "No published release for this one yet.";
}

function showModuleFlow() {
  enableCard("step-flash");
  $("step-pick").classList.add("disabled");
  $("pick-sub").textContent = "Grove Vision AI V2 module recognized — its model is flashed below.";
  $("provisioning").classList.add("hidden");
  $("host-flash-controls").classList.add("hidden");
  $("serial-monitor").classList.add("hidden");
  $("module-flow").classList.remove("hidden");
  $("console").textContent = "";
  $("console").classList.remove("hidden");
  setStatus("flash-result", "Use the module button below. The host firmware receipt is kept while you move the cable.");
  renderReceipts(true);
}

// ── step 3: flash ───────────────────────────────────────────────────────────
async function onFlash() {
  const provisioning = readProvisioning(state.product);
  if (state.product.provisioning === "usb-secrets" && !provisioning) return;
  persistProv();
  const btn = $("flash-btn");
  const con = $("console");
  con.textContent = "";
  con.classList.remove("hidden");
  btn.disabled = true;
  btn.textContent = "Flashing…";
  setStatus("flash-result", "");
  // Reflash must start from a clean slate — never show the PREVIOUS flash's
  // green result, receipts, certificate, or a stale "Broken pipe" monitor line.
  resetOutcome();
  state.busy = true; // pause the watcher so it can't grab the port
  await stopMonitor();

  const unlisten = await listen("flash:log", (ev) => {
    con.textContent += ev.payload + "\n";
    con.scrollTop = con.scrollHeight;
  });

  try {
    const receipt = await invoke("flash", {
      port: state.port,
      productId: state.product.id,
      manifestUrl: state.catalog.manifest_url,
      baud: state.catalog.flash_baud || 921600,
      detectedChip: state.chip,
      provisioning,
    });
    state.vision.hostFlash = receipt;
    state.vision.hostBoot = null;
    clearSecretFields();
    renderReceipts();
    announceToWitness(state.product);   // instant, so the wall reacts right away
    discoverAndPopulate(state.product); // then replace with the REAL fleet off the LAN
    if (requiresLiveReceipt(state.product)) {
      setStatus("flash-result", "Firmware write verified. Watching the live boot for its device receipt…", "ok");
      state.busy = false;
      await startMonitor();
    } else {
      setStatus("flash-result", "Firmware write verified. Flashing is complete. ✓", "ok");
      maybeHatch();
      // The serial monitor should just work — start it automatically so the
      // live boot log is right there, no "Start" click. It reconnects on its
      // own across the reboot (native side), so this is safe to fire now.
      startMonitor();
    }
  } catch (e) {
    setStatus("flash-result", String(e), "err");
    logEvent("err", "Flash failed: " + e);
    hideHatchCard();
  } finally {
    unlisten();
    btn.disabled = false;
    btn.textContent = "Flash my Canary";
    state.busy = false;
    // The board reboots after a flash; let the watcher re-sync from scratch.
    state.chip = null;
    state.failedPort = null;
  }
}

function readProvisioning(product) {
  if (!product || product.provisioning !== "usb-secrets") return null;
  const fields = ["device-id", "wifi-ssid", "wifi-pass", "mqtt-host", "mqtt-port", "mqtt-user", "mqtt-pass"];
  for (const id of fields) {
    const input = $(id);
    if (!input.checkValidity()) {
      input.reportValidity();
      return null;
    }
  }
  const wifiPass = $("wifi-pass").value;
  if (wifiPass && (new TextEncoder().encode(wifiPass).length < 8 ||
                   new TextEncoder().encode(wifiPass).length > 63)) {
    setStatus("flash-result", "Wi-Fi password must be 8–63 UTF-8 bytes (or empty for an open network).", "err");
    return null;
  }
  return {
    deviceId: $("device-id").value.trim(),
    wifiSsid: $("wifi-ssid").value,
    wifiPass,
    mqttHost: $("mqtt-host").value.trim(),
    mqttPort: Number($("mqtt-port").value),
    mqttUser: $("mqtt-user").value,
    mqttPass: $("mqtt-pass").value,
  };
}

function clearSecretFields() {
  $("wifi-pass").value = "";
  $("mqtt-pass").value = "";
}

async function onFlashModule() {
  const btn = $("module-flash-btn");
  btn.disabled = true;
  btn.textContent = "Flashing model…";
  state.busy = true;
  $("console").textContent = "";
  $("console").classList.remove("hidden");
  $("module-progress").textContent = "0%";
  $("module-preview").classList.add("hidden");
  setStatus("flash-result", "");
  try {
    const receipt = await invoke("flash_vision_module", {
      port: state.port,
      manifestUrl: state.catalog.we2_module.manifest_url,
    });
    state.vision.module = receipt;
    $("module-progress").textContent = "100% · inference proved";
    setStatus("flash-result", "Vision module verified, burned, answered AT, and ran one inference. ✓", "ok");
    if (receipt.preview_image) {
      $("module-preview").src = "data:image/jpeg;base64," + receipt.preview_image;
      $("module-preview").classList.remove("hidden");
    }
    renderReceipts(true);
    maybeHatch();
  } catch (e) {
    setStatus("flash-result", String(e), "err");
    logEvent("err", "Vision module flash failed: " + e);
  } finally {
    state.busy = false;
    btn.disabled = false;
    btn.textContent = "Flash & prove the Vision module";
  }
}

// ── serial monitor + earned receipts ────────────────────────────────────────
async function startMonitor() {
  if (!state.port || state.portKind !== "esp32") {
    setStatus("flash-result", "Connect the ESP32 host port to start its serial monitor.", "err");
    return;
  }
  $("serial-monitor").classList.remove("hidden");
  $("serial-console").textContent = "";
  $("monitor-status").textContent = "Waiting for the board to reappear after reboot…";
  setMonitorButtons(true);
  try {
    await invoke("start_serial_monitor", {
      port: state.port,
      vid: state.portInfo && state.portInfo.vid,
      pid: state.portInfo && state.portInfo.pid,
      baud: state.catalog.console_baud || 115200,
    });
  } catch (e) {
    setMonitorButtons(false);
    $("monitor-status").textContent = String(e);
  }
}

async function stopMonitor() {
  try { await invoke("stop_serial_monitor"); } catch (_) {}
  setMonitorButtons(false);
}

function setMonitorButtons(running) {
  state.monitoring = running;
  $("monitor-start").disabled = running;
  $("monitor-stop").disabled = !running;
  $("monitor-manifest").disabled = !running;
  $("monitor-pause").disabled = !running;
  $("monitor-cmd").disabled = !running;
  $("monitor-lineend").disabled = !running;
  $("monitor-send").disabled = !running;
}

// Wipe every "how did the last flash go" surface so a reflash never shows a
// stale green result, old receipts, a leftover certificate, or a dead monitor.
function resetOutcome() {
  setStatus("flash-result", "");
  try { hideHatchCard(); } catch (_) {}
  if (state.vision) { state.vision.hostFlash = null; state.vision.hostBoot = null; state.vision.module = null; }
  try { renderReceipts(); } catch (_) {}
  const con = $("serial-console");
  if (con) con.textContent = "";
  const ms = $("monitor-status");
  if (ms) ms.textContent = "";
  $("serial-monitor").classList.add("hidden");
}

function appendConsole(id, text) {
  const con = $(id);
  con.textContent += String(text);
  // Keep the buffer bounded — a long boot log shouldn't grow without limit.
  const CAP = 200_000;
  if (con.textContent.length > CAP) {
    con.textContent = con.textContent.slice(con.textContent.length - CAP);
  }
  // Autoscroll unless the user paused it (Arduino-style "freeze the view").
  if (!state.monitorPaused) con.scrollTop = con.scrollHeight;
}

function setReceipt(id, ok, text) {
  const row = $(id);
  row.className = "receipt " + (ok ? "ok" : "pending");
  row.querySelector("strong").textContent = text;
}

function renderReceipts(forceVision = false) {
  const host = state.vision.hostFlash;
  const boot = state.vision.hostBoot;
  const module = state.vision.module;
  const product = (state.catalog && state.catalog.products || [])
    .find((p) => host && p.id === host.product_id) || state.product;
  const receiptRequired = requiresLiveReceipt(product);
  const vision = forceVision || !!module || !!(host && host.product_id.includes("vision"));
  const any = !!host || !!boot || !!module || forceVision;
  $("receipts").classList.toggle("hidden", !any);
  if (!any) return;

  setReceipt(
    "receipt-host-image",
    !!host,
    host
      ? `✓ v${host.version} · ${host.bytes_written.toLocaleString()} B · ${host.release_verification || "SHA-256"} · ${host.installed_sha256.slice(0, 12)}…`
      : "waiting for ESP32 flash"
  );
  setReceipt(
    "receipt-host-boot",
    !!(boot && boot.ready),
    boot && boot.ready
      ? `✓ ${boot.manifest.board} · health ${boot.manifest.health ?? "unknown"}`
      : boot
        ? "manifest received; Vision I²C proof not healthy"
        : "waiting for live serial manifest"
  );
  $("receipt-host-boot").classList.toggle("hidden", !receiptRequired);
  $("receipt-module").classList.toggle("hidden", !vision);
  $("receipt-host-boot").querySelector("span").textContent = vision
    ? "Host boot + Vision I²C"
    : "Host boot manifest";
  if (vision) {
    setReceipt(
      "receipt-module",
      !!(module && module.inference_ok),
      module && module.inference_ok
        ? `✓ v${module.version} · inference · ${module.sha256.slice(0, 12)}…`
        : "waiting for WE2 model + AT inference"
    );
  }
}

function maybeHatch() {
  const host = state.vision.hostFlash;
  const boot = state.vision.hostBoot;
  const product = (state.catalog && state.catalog.products || [])
    .find((p) => host && p.id === host.product_id) || state.product;
  if (!host || (requiresLiveReceipt(product) && (!boot || !boot.ready))) {
    hideHatchCard();
    return;
  }
  const isVision = host.product_id.includes("vision");
  if (isVision && !(state.vision.module && state.vision.module.inference_ok)) {
    hideHatchCard();
    return;
  }
  showHatchCard(product);
}

// Missing/unknown catalog entries fail closed and still require a receipt.
// The generated catalog writes an explicit false only when the product's real
// firmware sources do not wire the shared self-manifest to the `j` command.
function requiresLiveReceipt(product) {
  return !product || product.serial_receipt !== false;
}

// ── self-update ─────────────────────────────────────────────────────────────
async function checkForUpdate(manual = false) {
  prefs.lastCheckedAt = Date.now();
  savePrefs();
  $("health-checked").textContent = fmtTime(prefs.lastCheckedAt);
  try {
    const up = await invoke("check_update");
    state.update = up || null;
    if (up) {
      $("update-text").textContent = `Version ${up.version} is ready (you have ${up.current_version}).`;
      $("update-banner").classList.remove("hidden");
      $("health-update").classList.remove("hidden");
      const upd = $("splash-upd"); if (upd) { upd.textContent = "update ready → v" + up.version; }
      logEvent("info", "Update available: v" + up.version);
    } else {
      $("health-update").classList.add("hidden");
      const upd = $("splash-upd"); if (upd) { upd.textContent = "up to date ✓"; }
      if (manual) logEvent("ok", "Checked — already up to date");
    }
  } catch (e) {
    // No signing key configured yet, or offline — stay quiet on the surface.
    const upd = $("splash-upd"); if (upd) { upd.textContent = navigator.onLine ? "update check unavailable" : "offline — will check later"; }
    if (manual) logEvent("err", "Update check failed: " + e);
  }
  if (!$("about-view").classList.contains("hidden")) renderAbout();
}

async function onInstallUpdate() {
  const btn = $("update-btn");
  btn.disabled = true;
  const unlisten = await listen("update:log", (ev) => {
    $("update-text").textContent = ev.payload;
  });
  try {
    await invoke("install_update"); // relaunches on success
  } catch (e) {
    $("update-text").textContent = "Update failed: " + e;
    logEvent("err", "Update install failed: " + e);
    btn.disabled = false;
  } finally {
    unlisten();
  }
}

// ── Explore / Atlas ──────────────────────────────────────────────────────────
// Two kinds of content, both organized so nothing rots: outbound links to the
// live site (stable securacv.com paths), and the field knowledge *baked into
// this build* from the embedded catalog — so it browses offline and refreshes
// itself every time the app updates.
const SITE = "https://securacv.com";
const REPO = "https://github.com/kmay89/securaCV";
const ATLAS_LINKS = [
  ["Your Canaries on the web", [
    ["Home", "The privacy-first witness, start to finish.", SITE + "/"],
    ["Meet the fleet", "Every Canary and what each one witnesses.", SITE + "/fleet"],
    ["Ecosystem", "How the Canaries, the Hub and Home Assistant fit.", SITE + "/ecosystem"],
    ["Gallery", "Real installs and the events they caught.", SITE + "/gallery"],
    ["Compare", "SecuraCV next to the subscription cameras.", SITE + "/compare"],
  ]],
  ["Learn & try", [
    ["How it works", "The witness kernel, in plain language.", SITE + "/how-it-works"],
    ["The Lab", "The browser flasher this app grew from.", SITE + "/lab"],
    ["Playground", "Drive a simulated Canary — no hardware.", SITE + "/playground"],
    ["Checkup", "Point it at a board and read its health.", SITE + "/checkup"],
  ]],
  ["Build & extend", [
    ["Download", "Get this app for every platform.", SITE + "/download"],
    ["Plugin", "Wire a Canary into your own stack.", SITE + "/plugin"],
    ["Maker", "Roll your own from parts.", SITE + "/maker"],
    ["Factory", "Batch-provision a whole run.", SITE + "/factory"],
  ]],
  ["The project", [
    ["Source on GitHub", "Every line, open. Read it, fork it.", REPO],
    ["Changelog", "What changed, release by release.", REPO + "/blob/main/CHANGELOG.md"],
    ["Security", "How issues are handled and disclosed.", REPO + "/blob/main/SECURITY.md"],
    ["Errer Labs", "The people behind the birds.", SITE + "/#about"],
  ]],
];

function extIcon() {
  return `<svg class="ac-ext" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M7 17 17 7M9 7h8v8"/></svg>`;
}
function linkIcon() {
  return `<svg class="ac-ico" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.9" stroke-linecap="round" stroke-linejoin="round"><path d="M10 13a5 5 0 0 0 7 0l2-2a5 5 0 0 0-7-7l-1 1M14 11a5 5 0 0 0-7 0l-2 2a5 5 0 0 0 7 7l1-1"/></svg>`;
}

function renderAtlas() {
  const cat = state.catalog || {};
  const parts = [];

  ATLAS_LINKS.forEach(([title, items]) => {
    parts.push(`<div class="atlas-group"><h2>${esc(title)}</h2><div class="atlas-grid">` +
      items.map(([name, blurb, url]) =>
        `<button class="atlas-card" data-url="${esc(url)}">
           <div class="ac-top">${linkIcon()}<b>${esc(name)}</b>${extIcon()}</div>
           <span>${esc(blurb)}</span>
         </button>`).join("") +
      `</div></div>`);
  });

  // Field notes — the embedded lessons, browsable offline.
  const lessons = cat.lessons || [];
  if (lessons.length) {
    parts.push(`<div class="atlas-group"><h2>Field notes · baked into this build</h2>` +
      lessons.map((l) =>
        `<div class="note-card"><div class="n-stage">${esc(l.stage || "note")}</div>
           <h3>${esc(l.title || "")}</h3><p>${esc(l.body || "")}</p></div>`).join("") +
      `</div>`);
  }

  // If something's off — recovery + the can't-brick reassurance.
  const recovery = cat.recovery || [];
  const noBrick = cat.no_brick;
  if (recovery.length || noBrick) {
    let block = `<div class="atlas-group"><h2>If something's off</h2><div class="note-card">`;
    if (noBrick) {
      block += `<h3>${esc(noBrick.headline || "")}</h3><p>${esc(noBrick.why || "")}</p>`;
      if (Array.isArray(noBrick.points))
        block += `<ul class="hint" style="margin-top:8px">${noBrick.points.map((p) => `<li>${esc(p)}</li>`).join("")}</ul>`;
    }
    block += recovery.map((r) =>
      `<div class="recovery-item"><div class="r-when">${esc(r.when || "")}</div><div class="r-do">${esc(r.do || "")}</div></div>`).join("");
    block += `</div></div>`;
    parts.push(block);
  }

  // Vision module bench guide.
  const bench = cat.we2_module && cat.we2_module.bench;
  if (bench && Array.isArray(bench.steps)) {
    let block = `<div class="atlas-group"><h2>Vision module bench</h2><div class="note-card">
      <h3>${esc(cat.we2_module.name || "Grove Vision AI V2")}</h3>
      <p>${esc(cat.we2_module.chip || "")}</p>
      <ol class="hint" style="margin-top:10px">${bench.steps.map((s) => `<li>${esc(s)}</li>`).join("")}</ol>`;
    if (Array.isArray(bench.troubleshooting))
      block += bench.troubleshooting.map((t) =>
        `<div class="recovery-item"><div class="r-when">${esc(t.when || "")}</div><div class="r-do">${esc(t.fix || "")}</div></div>`).join("");
    block += `</div></div>`;
    parts.push(block);
  }

  const body = $("atlas-body");
  body.innerHTML = parts.join("");
  body.querySelectorAll(".atlas-card").forEach((c) =>
    c.addEventListener("click", () => openExternal(c.dataset.url))
  );
}

// ── About & Health ───────────────────────────────────────────────────────────
function renderAbout() {
  const i = state.appInfo || {};
  const specs = [
    ["Version", "v" + (i.version || "—")],
    ["Build", (i.build_rev && i.build_rev !== "source") ? i.build_rev : "source"],
    ["Built", i.build_epoch ? fmtDate(i.build_epoch * 1000) : "—"],
    ["Firmware train", i.fw_train || "—"],
    ["Last updated", prefs.lastVersionAt ? relativeTime(prefs.lastVersionAt) : "just now"],
    ["Last checked", prefs.lastCheckedAt ? relativeTime(prefs.lastCheckedAt) : "—"],
  ];

  const updState = state.update
    ? `<p class="status ok">Version ${esc(state.update.version)} is ready to install.</p>
       <div class="row"><button class="btn btn-primary btn-small" id="about-update">Update &amp; relaunch</button>
       <button class="btn btn-ghost btn-small" id="about-check">Check again</button></div>`
    : `<p class="status">You're on the newest build. The app checks on its own and heals forward — updates are signed and verified before they install.</p>
       <div class="row"><button class="btn btn-ghost btn-small" id="about-check">Check now</button></div>`;

  const log = (prefs.log || []);
  const logHtml = log.length
    ? log.map((e) =>
        `<div class="log-row ${esc(e.kind || "")}"><time>${esc(fmtTime(e.t))}</time><span class="l-msg">${esc(e.msg)}</span></div>`).join("")
    : `<div class="log-row"><span class="l-msg muted">Nothing logged yet — a clean run.</span></div>`;

  $("about-body").innerHTML = `
    <div class="brand-block">
      <span class="bb-mark" aria-hidden="true">
        <svg viewBox="0 0 64 64" width="44" height="44"><circle cx="32" cy="36" r="17" fill="#FFD44F"/><circle cx="41" cy="24" r="10" fill="#FFD44F"/><circle cx="44.5" cy="22.5" r="1.8" fill="#141414"/><path d="M50 25.5 l7 2.2 -7 2.2 z" fill="#F08C2E"/><ellipse cx="26" cy="38" rx="8.5" ry="6" fill="#E3B33C"/></svg>
      </span>
      <div>
        <h2>SecuraCV&nbsp;Lab</h2>
        <p>The Lab as a native app — by <span class="bb-lab">Errer Labs</span>. No browser, no terminal, and you can't brick the board.</p>
      </div>
    </div>

    <section class="card">
      <div class="step-head"><span class="step-n">i</span><h2>This build</h2></div>
      <div class="spec-grid">
        ${specs.map(([k, v]) => `<div class="spec"><div class="s-k">${esc(k)}</div><div class="s-v small">${esc(v)}</div></div>`).join("")}
      </div>
    </section>

    <section class="card">
      <div class="step-head"><span class="step-n">↻</span><h2>Self-update</h2></div>
      ${updState}
    </section>

    <section class="card">
      <div class="step-head"><span class="step-n">≡</span><h2>Recent activity</h2></div>
      <p class="muted">A local record of what this app did — so a failure is visible and recoverable, never silent. Kept on this computer only.</p>
      <div class="log-list">${logHtml}</div>
      <div class="row"><button class="btn btn-ghost btn-small" id="about-reset">Reset the app's memory</button></div>
    </section>`;

  const check = $("about-check");
  if (check) check.addEventListener("click", () => { check.disabled = true; check.textContent = "Checking…"; checkForUpdate(true); });
  const upd = $("about-update");
  if (upd) upd.addEventListener("click", onInstallUpdate);
  const reset = $("about-reset");
  if (reset) reset.addEventListener("click", () => {
    prefs.roster = []; prefs.log = []; prefs.prov = {}; prefs.hubSsid = "";
    savePrefs();
    updateResumeState();
    logEvent("info", "Memory reset");
    renderAbout();
  });
}

// ── helpers ─────────────────────────────────────────────────────────────────
function setConn(kind, text) {
  $("conn-dot").className = "dot " + kind;
  $("conn-text").textContent = text;
  $("conn").className = "connbar " + kind;
}
function setStatus(id, msg, kind) {
  const el = $(id);
  el.textContent = msg;
  el.className = "status" + (kind ? " " + kind : "");
}
function enableCard(id) {
  $(id).classList.remove("disabled");
}

function hideHatchCard() {
  const card = $("hatch-card");
  if (card) card.classList.add("hidden");
}

function showHatchCard(product) {
  const wasHidden = $("hatch-card").classList.contains("hidden");
  const moment = hatchMoment(product);
  $("hatch-card").querySelector(".hatch-kicker").textContent = moment.kicker || "Canary hatched";
  $("hatch-title").textContent = moment.title;
  $("hatch-body").textContent = moment.body;
  const steps = $("hatch-steps");
  steps.innerHTML = "";
  moment.steps.forEach((step) => {
    const li = document.createElement("li");
    li.textContent = step;
    steps.appendChild(li);
  });
  $("hatch-card").classList.remove("hidden");
  if (wasHidden) {
    const cert = mintCertificate(product); // a real birth certificate, once
    renderCertificate(cert);
    rosterAdd({ kind: "canary", name: cert ? cert.name : (product && product.name) || "Canary", chip: state.chip || "" });
    logEvent("ok", `${cert ? cert.name : (product && product.name) || "Canary"} hatched`);
    updateResumeState();
  }
}

// ── the Hatchery: name + birth certificate (shared spec with the web Lab) ────
// The whimsical name is a display layer only — the device's functional id
// stays the stable slug written during provisioning; here it just becomes the
// certificate's Ring ID. The same name parts, mottoes and copy come from the
// embedded hatch.json the website also ships, so both surfaces hatch alike.
let lastCert = null;

// Bases used this run + across sessions (from the local fleet), so a fresh
// hatch never reuses a name until the whole pool has been spent.
const usedBases = new Set();
function pickFreshBase(first, avoid) {
  const spent = new Set(usedBases);
  for (const c of (prefs.fleet || [])) if (c.base) spent.add(c.base);
  if (avoid) spent.add(avoid);
  const open = first.filter((b) => !spent.has(b));
  const pool = open.length ? open : first; // pool exhausted → allow reuse
  return pool[Math.floor(Math.random() * pool.length)];
}

function mintCertificate(product, avoidBase) {
  const h = state.hatch;
  if (!h || !Array.isArray(h.first) || !h.first.length) return null;
  const pick = (a) => a[Math.floor(Math.random() * a.length)];
  const base = pickFreshBase(h.first, avoidBase);
  usedBases.add(base);
  const withTitle = Array.isArray(h.titles) && h.titles.length &&
    Math.random() < (typeof h.title_chance === "number" ? h.title_chance : 0.6);
  const house = (Array.isArray(h.house) && h.house.length) ? pick(h.house) : "";
  // "Nth of its name" from how many Canaries with this base name you've hatched.
  const fleet = prefs.fleet || [];
  const nth = fleet.filter((c) => c.base === base).length + 1;
  const ordinals = Array.isArray(h.ordinals) ? h.ordinals : [];
  const ordinal = ordinals[nth] || ("the " + nth + "th");
  // Ring ID = the real functional device-id if this board was provisioned,
  // else a generated ring code — either way it identifies the actual bird.
  const provisionedId = $("device-id") ? $("device-id").value.trim() : "";
  const ringId = provisionedId || genRing(h.ring_prefix || "CNRY");

  return {
    base,
    name: (withTitle ? pick(h.titles) + " " : "") + base + (house ? " " + house : ""),
    species: (product && product.name) || "Canary",
    lineage: ordinal + " of its name" + (house ? ", " + house.replace(/^the /, "") : ""),
    ringId,
    motto: (Array.isArray(h.mottoes) && h.mottoes.length) ? pick(h.mottoes) : "",
    craft: (h.certificate && h.certificate.craft) || "",
    ts: Date.now(),
  };
}

function genRing(prefix) {
  let hex;
  try {
    const b = crypto.getRandomValues(new Uint8Array(3));
    hex = Array.from(b).map((x) => x.toString(16).padStart(2, "0")).join("");
  } catch (_) {
    hex = Math.floor(Math.random() * 0xffffff).toString(16);
  }
  hex = (hex + "000000").slice(0, 6).toUpperCase();
  return prefix + "-" + hex.slice(0, 3) + "-" + hex.slice(3);
}

function renderCertificate(cert) {
  const fig = $("hatch-cert");
  if (!fig) return;
  if (!cert) { fig.hidden = true; return; }
  lastCert = cert;
  const h = state.hatch || {};
  const c = (h.certificate) || {};
  if (c.kicker) $("cert-kicker").textContent = c.kicker;
  if (c.intro) $("cert-intro").textContent = c.intro;
  if (c.foot) $("cert-foot").textContent = c.foot;
  $("cert-name").textContent = cert.name;
  $("cert-species").textContent = "Species · " + cert.species;
  $("cert-lineage").textContent = cert.lineage;
  $("cert-date").textContent = fmtDate(cert.ts);
  $("cert-id").textContent = cert.ringId;
  $("cert-craft").textContent = cert.craft || "shell pressed in the workshop · spirit woken in the hatchery";
  $("cert-motto").textContent = cert.motto ? "“" + cert.motto + "”" : "";
  fig.hidden = false;
  // Remember this bird's lineage locally so ordinals climb across sessions.
  prefs.fleet = prefs.fleet || [];
  if (!prefs.fleet.some((x) => x.ringId === cert.ringId)) {
    prefs.fleet.unshift({ name: cert.name, base: cert.base, species: cert.species, ringId: cert.ringId, ts: cert.ts });
    prefs.fleet = prefs.fleet.slice(0, 40);
    savePrefs();
  }
}

// Roll a fresh name for the same freshly-hatched bird (does not re-count it).
function rerollCertificate() {
  if (!lastCert || !state.hatch) return;
  const prev = lastCert;
  // drop the just-recorded entry so a re-roll replaces rather than stacks
  if (prefs.fleet && prefs.fleet[0] && prefs.fleet[0].ringId === prev.ringId) {
    prefs.fleet.shift();
    savePrefs();
  }
  const product = (state.catalog && state.catalog.products || [])
    .find((p) => p.name === prev.species) || { name: prev.species };
  const next = mintCertificate(product, prev.base); // never re-roll the same name
  if (next) { next.ringId = prev.ringId; next.ts = prev.ts; renderCertificate(next); }
}

function hatchMoment(product) {
  const id = product && product.id;
  // The native Vision hatch only appears after the two-board proof, so its
  // copy is stronger than the shared catalog hatch used by browser surfaces.
  if (id && id.includes("vision")) {
    return {
      kicker: "Vision Canary hatched",
      title: "Your Vision Canary is awake — both boards proved it.",
      body: "The host reported its signed identity and live Vision I²C module; the WE2 reported the pinned model and completed an inference.",
      steps: [
        "Put the board where it can see a doorway, then walk through once.",
        "Open Home Assistant and watch the presence entity flip to detected, then clear.",
        "The attended module frame above stayed on this computer; deployed events remain presence/boxes only."
      ]
    };
  }
  if (product && product.hatch && Array.isArray(product.hatch.steps)) {
    return product.hatch;
  }
  if (id && id.includes("sense")) {
    return {
      kicker: "Canary hatched",
      title: "Your Sense Canary is listening with radar.",
      body: "The first satisfying test is motion in empty air: no camera, no mic, just the mmWave witness waking up.",
      steps: [
        "Power it from the room where it will live and wait for Home Assistant discovery.",
        "Stand still for a breath, then walk past it at normal speed.",
        "Watch presence flip in Home Assistant; on Wellbeing builds, try seated breathing only after the presence card is stable."
      ]
    };
  }
  return {
    kicker: "Canary hatched",
    title: "Your Canary is on its perch.",
    body: "The magical first proof is local and physical: join its setup network, open the dashboard, then make one harmless signal it can witness.",
    steps: [
      "Join the SecuraCV-XXXX Wi-Fi network it creates and open canary.local.",
      "Tap Identify so the bird blinks and chirps — you know this is the board in your hand.",
      "Knock once near it or use the acoustic self-test card; Home Assistant automations are not fired by the self-test."
    ]
  };
}

function esc(s) {
  return String(s).replace(/[&<>"]/g, (c) =>
    ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c])
  );
}
function article(name) {
  return /^[aeiou]/i.test(String(name || "")) ? "an" : "a";
}
function fmtTime(ms) {
  try { return new Date(ms).toLocaleTimeString([], { hour: "2-digit", minute: "2-digit" }); }
  catch { return "—"; }
}
function fmtDate(ms) {
  try { return new Date(ms).toLocaleDateString([], { year: "numeric", month: "short", day: "numeric" }); }
  catch { return "—"; }
}
function relativeTime(ms) {
  const d = Date.now() - ms;
  if (d < 60e3) return "just now";
  if (d < 3600e3) { const m = Math.round(d / 60e3); return m + " min ago"; }
  if (d < 86400e3) { const h = Math.round(d / 3600e3); return h + (h === 1 ? " hour ago" : " hours ago"); }
  const days = Math.round(d / 86400e3);
  return days === 1 ? "yesterday" : days + " days ago";
}

// ── The Hub (Raspberry Pi) flow ────────────────────────────────────────────
//
// Same philosophy as the Canary flow — plugging the thing in is what advances
// the UI — but the safety posture is inverted: an ESP32 flash can't hit the
// wrong device; a raw disk write can. So every disk decision comes from the
// Rust hub-core gate and is SHOWN, never re-derived here: eligible disks are
// picker rows, refused disks appear under "hidden because…", and the write
// button only arms after a typed ERASE on a still-eligible target.

const hub = {
  active: false,
  targets: [],
  selected: null, // device path
  boards: [], // catalog boards (Pi 5 / Pi 4, with the models each covers)
  boardId: null,
  plan: null,
  pollTimer: null,
  busy: false,
  done: false,
  piUsbWaiting: false,
  platform: "", // "macos" | "linux" | …
  accountValid: false,
  accountRequested: false,
  // ETA bookkeeping, reset at each stage change
  eta: { stage: null, t0: 0, done0: 0 },
  fbTimer: null, // first-boot poll
  resumeTimer: null, // resume-across-restart poll
};

const HUB_HOST = "homeassistant.local:8123";
const HUB_REMEMBER_KEY = "securacv.hub.remember";
const HUB_LASTFLASH_KEY = "securacv.hub.lastflash";
// How long after a flash we'll still offer to resume the first-boot watch on
// relaunch — comfortably longer than HAOS's 10–20 min first boot.
const HUB_RESUME_WINDOW_MS = 45 * 60 * 1000;

const HUB_STAGE_COPY = {
  download: "Downloading Home Assistant OS…",
  decompress: "Unpacking the image…",
  write: "Writing to the card — don't remove it…",
  verify: "Reading every byte back to prove the write…",
  seed: "Adding your settings to the card…",
};

// The stages a flash walks, in order — rendered as pills that light up as
// each one passes, so there's always a visible sense of where you are.
const HUB_STAGE_ORDER = ["download", "decompress", "write", "verify", "seed"];
const HUB_STAGE_PILL = {
  download: "Download",
  decompress: "Unpack",
  write: "Write",
  verify: "Verify",
  seed: "Settings",
};

function hubRenderPills(activeStage) {
  const box = $("hub-pills");
  if (!box) return;
  const activeIdx = HUB_STAGE_ORDER.indexOf(activeStage);
  box.innerHTML = HUB_STAGE_ORDER.map((s, i) => {
    const cls = i < activeIdx ? "done" : i === activeIdx ? "active" : "";
    return `<span class="stage-pill ${cls}">${HUB_STAGE_PILL[s]}</span>`;
  }).join("");
}

function hubInit() {
  $("hub-flash-btn").addEventListener("click", hubFlash);
  $("hub-confirm").addEventListener("input", hubArm);
  $("hub-ethernet").addEventListener("change", () => {
    const wired = $("hub-ethernet").checked;
    $("hub-ssid").disabled = wired;
    $("hub-pass").disabled = wired;
    $("hub-hidden-net").disabled = wired;
    hubArm();
  });
  ["hub-ssid", "hub-pass"].forEach((id) => $(id).addEventListener("input", hubArm));
  $("hub-ssid").addEventListener("input", persistProv);
  // Account fields: live-validate on every keystroke.
  ["hub-acct-name", "hub-acct-user", "hub-acct-pass", "hub-acct-pass2"].forEach((id) =>
    $(id).addEventListener("input", () => {
      hubValidateAccount();
      hubArm();
    })
  );
  // First-boot companion controls.
  $("hub-fb-open").addEventListener("click", () => openExternal("http://" + HUB_HOST));
  $("hub-fb-stop").addEventListener("click", hubStopFirstBoot);
  // Restore remembered non-secret fields.
  hubRestoreSettings();
  // If we were reopened soon after a flash, quietly resume watching for it.
  hubMaybeResume();
  $("hub-resume-dot").className = "dot reading";

  $("hub-pi-usb-btn").addEventListener("click", hubPiUsbToggle);
  listen("hub:pi-usb", (e) => {
    // rpiboot's own narration, one line at a time — the last line is the state.
    $("hub-pi-usb-status").textContent = e.payload;
  });
  listen("hub:pi-usb-done", (e) => {
    hub.piUsbWaiting = false;
    $("hub-pi-usb-btn").textContent = "Wait for my Pi";
    $("hub-pi-usb-status").textContent =
      e.payload === 0
        ? "Done — your Pi is becoming a disk; it will appear above in a few seconds."
        : "Stopped. Click again to retry (hold the power button while connecting).";
  });

  listen("hub:log", (e) => {
    const el = $("hub-console");
    el.classList.remove("hidden");
    el.textContent += e.payload + "\n";
    el.scrollTop = el.scrollHeight;
  });
  listen("hub:progress", (e) => {
    const { stage, done, total } = e.payload;
    hub.stage = stage;
    $("hub-progress-wrap").classList.remove("hidden");
    $("hub-stage").textContent = HUB_STAGE_COPY[stage] || stage;
    hubRenderPills(stage);

    const fill = $("hub-bar-fill");
    if (total) {
      fill.classList.remove("indet");
      fill.style.width = Math.min(100, Math.round((done / total) * 100)) + "%";
      $("hub-eta").textContent = hubEta(stage, done, total);
    } else {
      // Unknown total: an alive, sweeping bar — never a full one that lies.
      fill.classList.add("indet");
      fill.style.width = "";
      $("hub-eta").textContent = "";
    }
    // Stopping is safe at every stage, but the copy should be honest about
    // what stopping mid-write means for the card.
    $("hub-stop-btn").textContent =
      stage === "write" || stage === "verify" ? "Stop (card will need a fresh flash)" : "Stop";
  });
  // Fired by the backend a beat BEFORE macOS's authopen prompt appears (see
  // hub.rs). Put a prominent, by-name cue in the big status line so the Touch ID
  // / password dialog reads as expected, not sketchy. The next progress event —
  // the write actually moving, i.e. they approved — overwrites this via
  // HUB_STAGE_COPY, so it clears itself.
  listen("hub:mac-auth", () => {
    $("hub-progress-wrap").classList.remove("hidden");
    $("hub-stage").textContent =
      "👆 Approve the macOS prompt — Touch ID or your password. It's “authopen”, Apple's built-in disk-writing helper (the same one Raspberry Pi Imager uses).";
    $("hub-console").classList.remove("hidden");
  });
  $("hub-stop-btn").addEventListener("click", async () => {
    $("hub-stop-btn").disabled = true;
    $("hub-stage").textContent = "Stopping — finishing the current chunk…";
    try {
      await invoke("hub_flash_cancel");
    } catch (e) {
      setStatus("hub-result", String(e), "err");
    }
  });
}

// ── resume across a restart ──────────────────────────────────────────────────
// A flash writes a tiny "last flash" record; on the next launch, if it's
// recent and the hub isn't up yet, we quietly re-offer to watch for it — so a
// crash, quit, or reboot mid-first-boot never loses the thread.
function hubRecordFlash() {
  try {
    localStorage.setItem(HUB_LASTFLASH_KEY, JSON.stringify({ host: HUB_HOST, at: Date.now() }));
  } catch (_) {}
}
function hubClearFlashRecord() {
  try { localStorage.removeItem(HUB_LASTFLASH_KEY); } catch (_) {}
}
async function hubMaybeResume() {
  let rec;
  try { rec = JSON.parse(localStorage.getItem(HUB_LASTFLASH_KEY) || "null"); } catch (_) { rec = null; }
  if (!rec || !rec.at || Date.now() - rec.at > HUB_RESUME_WINDOW_MS) { hubClearFlashRecord(); return; }
  // If it's already up, there's nothing to resume — just tidy the record.
  let up = false;
  try { up = await invoke("hub_probe_hub", { host: rec.host || HUB_HOST }); } catch (_) {}
  if (up) { hubClearFlashRecord(); return; }
  const banner = $("hub-resume");
  banner.classList.remove("hidden");
  $("hub-resume-open").addEventListener("click", () => openExternal("http://" + (rec.host || HUB_HOST)));
  $("hub-resume-dismiss").addEventListener("click", () => {
    hubClearFlashRecord();
    banner.classList.add("hidden");
    if (hub.resumeTimer) { clearInterval(hub.resumeTimer); hub.resumeTimer = null; }
  });
  const tick = async () => {
    let alive = false;
    try { alive = await invoke("hub_probe_hub", { host: rec.host || HUB_HOST }); } catch (_) {}
    if (alive) {
      if (hub.resumeTimer) { clearInterval(hub.resumeTimer); hub.resumeTimer = null; }
      $("hub-resume-dot").className = "dot connected";
      $("hub-resume-text").textContent = "Your hub from earlier is up. 🐤";
      $("hub-resume-open").classList.remove("hidden");
      hubNotify("Your hub is ready", "Open " + (rec.host || HUB_HOST) + " to log in.");
      hubChime();
      hubClearFlashRecord();
    }
  };
  hub.resumeTimer = setInterval(tick, 6000);
  tick();
}

// Turn a backend error into calm, useful words: what happened, why the
// hardware is fine, and the one thing to do next. Cancels are not errors.
function hubPresentError(raw) {
  const msg = String(raw);
  if (msg.startsWith("cancelled:")) {
    setStatus(
      "hub-result",
      "Stopped, no harm done. The card just needs a fresh flash whenever you're ready — type ERASE again to go.",
      ""
    );
    return;
  }
  let friendly;
  if (msg.includes("read-back verification FAILED")) {
    friendly =
      "The card and we are telling different stories — it didn't hold what we wrote, which " +
      "usually means a worn-out or counterfeit card. Best move: try a different card. " +
      "Nothing else on your computer was touched.";
  } else if (
    msg.includes("no longer connected") ||
    msg.includes("disappeared") ||
    /write failed after|read-back failed|Input\/output|no such device|not connected/i.test(msg)
  ) {
    friendly =
      "The card wandered off mid-job (loose reader? bumped cable?). No harm done — plug it back " +
      "in and type ERASE again; we start clean from the top, with no half-finished leftovers.";
  } else if (/no space|not enough|ENOSPC/i.test(msg)) {
    friendly =
      "Your computer ran low on room to stage the image (we need about 6 GB free). Clear a little " +
      "space and type ERASE to try again — your card wasn't touched.";
  } else if (msg.includes("download") || /network|timed out|connection/i.test(msg)) {
    friendly =
      "The internet let us down mid-download — we even tried twice. Your card is untouched, and " +
      "any part we'd fetched is kept, so a retry picks up quickly. Type ERASE to try again.";
  } else if (msg.includes("no permission") || msg.includes("authorize")) {
    friendly =
      "Your computer wouldn't let us open the disk — that's it being protective, not broken. " +
      "The message below says exactly which permission to grant, then try again.";
  } else {
    friendly =
      "Well, that didn't go to plan — our fault, not yours, and your card is fine. " +
      "The details are below; a retry sorts out most of these.";
  }
  setStatus("hub-result", friendly + "\n\n" + msg, "err");
}

// ── ETA ─────────────────────────────────────────────────────────────────────
// A rolling estimate for the CURRENT stage: honest ("about 3m left"), never a
// fake total across stages. Reset the clock whenever the stage changes.
function hubEta(stage, done, total) {
  if (hub.eta.stage !== stage) {
    hub.eta = { stage, t0: Date.now(), done0: done };
    return "";
  }
  const dt = (Date.now() - hub.eta.t0) / 1000;
  const bytes = done - hub.eta.done0;
  if (dt < 1.5 || bytes <= 0) return ""; // let it settle before guessing
  const rate = bytes / dt; // bytes/sec
  const secs = Math.max(0, Math.round((total - done) / rate));
  if (secs < 3) return "almost there";
  if (secs < 60) return `about ${secs}s left`;
  const m = Math.floor(secs / 60);
  const s = secs % 60;
  return `about ${m}m ${s.toString().padStart(2, "0")}s left`;
}

// ── account fields ──────────────────────────────────────────────────────────
function hubAccountValue() {
  if (!hub.accountRequested) return null;
  return {
    name: $("hub-acct-name").value.trim(),
    username: $("hub-acct-user").value.trim(),
    password: $("hub-acct-pass").value,
  };
}

function hubValidateAccount() {
  const name = $("hub-acct-name").value.trim();
  const user = $("hub-acct-user").value.trim();
  const pass = $("hub-acct-pass").value;
  const pass2 = $("hub-acct-pass2").value;
  const hint = $("hub-acct-hint");
  // Intent to pre-make an account is signalled by a PASSWORD — a name or
  // username alone can't make a login and must never block a plain flash
  // (they may be remembered from last time). And because the password field
  // lives inside the collapsed panel, "requested" can only ever become true
  // while the panel is open — so any blocking hint below is always visible.
  hub.accountRequested = pass.length > 0 || pass2.length > 0;
  if (!hub.accountRequested) {
    hub.accountValid = false;
    // A gentle, non-blocking nudge if they've started a name/username.
    hint.textContent =
      name || user
        ? "Add a password to pre-make your login and skip Home Assistant's setup wizard — or leave it blank to set up on first boot."
        : "";
    return;
  }
  let msg = "";
  let ok = true;
  if (!name) { ok = false; msg = "Add your name."; }
  else if (!user) { ok = false; msg = "Pick a username."; }
  else if (/\s/.test(user)) { ok = false; msg = "The username can't contain spaces."; }
  else if (pass.length < 8) { ok = false; msg = `Password needs 8+ characters (${pass.length} so far).`; }
  else if (pass !== pass2) { ok = false; msg = "The two passwords don't match yet."; }
  else {
    // Gentle strength read — never blocking, just encouraging.
    const variety =
      (/[a-z]/.test(pass) ? 1 : 0) + (/[A-Z]/.test(pass) ? 1 : 0) +
      (/[0-9]/.test(pass) ? 1 : 0) + (/[^a-zA-Z0-9]/.test(pass) ? 1 : 0);
    const strength = pass.length >= 16 || (pass.length >= 12 && variety >= 3)
      ? "strong" : pass.length >= 10 ? "decent" : "okay";
    msg = `Looks good — password strength: ${strength}. First boot will be a login page.`;
  }
  hub.accountValid = ok;
  hint.textContent = msg;
}

// ── remember non-secret settings (never passwords) ──────────────────────────
function hubSaveSettings() {
  try {
    if (!$("hub-remember").checked) {
      localStorage.removeItem(HUB_REMEMBER_KEY);
      return;
    }
    localStorage.setItem(
      HUB_REMEMBER_KEY,
      JSON.stringify({
        ssid: $("hub-ssid").value.trim(),
        hidden: $("hub-hidden-net").checked,
        boardId: hub.boardId,
        acctName: $("hub-acct-name").value.trim(),
        acctUser: $("hub-acct-user").value.trim(),
      })
    );
  } catch (_) {}
}

function hubRestoreSettings() {
  try {
    const raw = localStorage.getItem(HUB_REMEMBER_KEY);
    if (!raw) return;
    const s = JSON.parse(raw);
    if (s.ssid) $("hub-ssid").value = s.ssid;
    if (s.hidden) $("hub-hidden-net").checked = true;
    if (s.acctName) $("hub-acct-name").value = s.acctName;
    if (s.acctUser) $("hub-acct-user").value = s.acctUser;
    if (s.boardId) hub.boardId = s.boardId; // applied when the board list renders
    // Restored account fields must re-validate, or the panel reads as
    // "untouched" and the account is silently skipped despite showing values.
    hubValidateAccount();
  } catch (_) {}
}

// UTF-8 byte length (SSID limit is 32 *bytes*, not characters).
function hubByteLen(s) {
  return new TextEncoder().encode(s).length;
}

// ── preflight (free space + platform hint) ──────────────────────────────────
async function hubPreflight() {
  try {
    const pf = await invoke("hub_preflight");
    hub.platform = pf.platform || "";
    const line = $("hub-preflight");
    if (!pf.staging_ok) {
      const freeGb = (pf.staging_free_bytes / 1024 ** 3).toFixed(1);
      line.textContent =
        `⚠ Only ${freeGb} GB free for staging — the image needs ~6 GB of room. ` +
        "Free some space first, or the flash may stop partway.";
    } else {
      line.textContent =
        hub.platform === "macos"
          ? "Heads up: when the write starts, macOS asks for Touch ID or your password — a prompt from “authopen”, Apple's built-in disk-writing helper. That's expected; approve it and the flash continues."
          : "";
    }
  } catch (_) {
    /* preflight is advisory; never block on it */
  }
}

// ── first-boot companion ────────────────────────────────────────────────────
// Close the loop across the silent 10–20 min: poll the hub until it answers,
// then invite the user in. Nothing here can hurt the card — it's read-only.
function hubStartFirstBoot() {
  // Leave a breadcrumb so a restart/quit mid-first-boot can resume the watch.
  hubRecordFlash();
  const panel = $("hub-firstboot");
  panel.classList.remove("hidden");
  $("hub-fb-dot").className = "dot reading";
  $("hub-fb-text").textContent =
    "Put the card in your Pi and power it on — watching for it to come online. First boot takes 10–20 minutes while it sets itself up; the blinking light is normal. You can walk away, we'll ping you.";
  $("hub-fb-open").classList.add("hidden");
  hubRenderQr();
  hubStopFirstBoot(true); // clear any prior timer without hiding the panel
  const tick = async () => {
    let up = false;
    try {
      up = await invoke("hub_probe_hub", { host: HUB_HOST });
    } catch (_) {}
    if (up) {
      hubStopFirstBoot(true);
      $("hub-fb-dot").className = "dot connected";
      $("hub-fb-text").textContent = "It's alive! Your hub is up. 🐤";
      const openBtn = $("hub-fb-open");
      openBtn.classList.remove("hidden");
      openBtn.classList.remove("alive-pop");
      void openBtn.offsetWidth;
      openBtn.classList.add("alive-pop");
      hubNotify("Your hub is ready", "Open " + HUB_HOST + " to log in.");
      hubChime();
      hubClearFlashRecord(); // it's up — nothing left to resume
    }
  };
  hub.fbTimer = setInterval(tick, 5000);
  tick();
}

function hubStopFirstBoot(keepPanel) {
  if (hub.fbTimer) {
    clearInterval(hub.fbTimer);
    hub.fbTimer = null;
  }
  if (!keepPanel) $("hub-firstboot").classList.add("hidden");
}

async function hubRenderQr() {
  try {
    const { default: qrcode } = await import("./vendor/qrcode/qrcode.mjs");
    const qr = qrcode(0, "M");
    qr.addData("http://" + HUB_HOST);
    qr.make();
    const box = $("hub-fb-qr");
    box.innerHTML = qr.createSvgTag({ cellSize: 4, margin: 2 });
    box.classList.remove("hidden");
  } catch (_) {
    /* QR is a bonus; the Open button is the real path */
  }
}

// ── notification + chime ────────────────────────────────────────────────────
async function hubNotify(title, body) {
  try {
    const n = window.__TAURI__ && window.__TAURI__.notification;
    if (!n) return;
    let granted = await n.isPermissionGranted();
    if (!granted) granted = (await n.requestPermission()) === "granted";
    if (granted) n.sendNotification({ title, body });
  } catch (_) {}
}

function hubChime() {
  try {
    const Ctx = window.AudioContext || window.webkitAudioContext;
    if (!Ctx) return;
    const ctx = new Ctx();
    const now = ctx.currentTime;
    [880, 1320].forEach((freq, i) => {
      const osc = ctx.createOscillator();
      const gain = ctx.createGain();
      osc.frequency.value = freq;
      osc.type = "sine";
      const t = now + i * 0.14;
      gain.gain.setValueAtTime(0.0001, t);
      gain.gain.exponentialRampToValueAtTime(0.15, t + 0.02);
      gain.gain.exponentialRampToValueAtTime(0.0001, t + 0.22);
      osc.connect(gain).connect(ctx.destination);
      osc.start(t);
      osc.stop(t + 0.24);
    });
  } catch (_) {}
}

async function hubLoadPlan() {
  try {
    // The board list renders once from the embedded catalog; the plan
    // re-resolves whenever the operator picks a different Pi.
    if (!hub.boards.length) {
      const catalog = await invoke("load_hub_catalog");
      hub.boards = catalog.base_os.boards || [];
      const recommended = hub.boards.find((b) => b.recommended);
      hub.boardId = hub.boardId || (recommended ? recommended.id : null);
      $("hub-board-list").innerHTML = hub.boards
        .map(
          (b) => `<label class="product${b.id === hub.boardId ? " selected" : ""}">
            <input type="radio" name="hub-board" value="${esc(b.id)}" ${
              b.id === hub.boardId ? "checked" : ""
            }>
            <div><div class="p-name">${esc(b.name)}${
              b.recommended ? '<span class="chip-badge">recommended</span>' : ""
            }</div>
            <div class="p-tag">also fits: ${esc((b.covers || []).join(" · "))}</div>
            <div class="p-meta">${esc(b.durable_default || "")}</div></div>
          </label>`
        )
        .join("");
      $("hub-board-list")
        .querySelectorAll("input[name=hub-board]")
        .forEach((r) =>
          r.addEventListener("change", (ev) => {
            hub.boardId = ev.target.value;
            hub.plan = null;
            hubLoadPlan();
          })
        );
      const excluded = catalog.base_os.excluded_boards || [];
      $("hub-excluded-list").innerHTML = excluded
        .map(
          (x) =>
            `<div class="hidden-disk"><strong>${esc(x.model)}</strong> — ${esc(x.why)}</div>`
        )
        .join("");
    }
    if (hub.plan) return;
    hub.plan = await invoke("hub_plan", { boardId: hub.boardId });
    // Reflect the picked board in the list highlight + explainer line.
    $("hub-board-list")
      .querySelectorAll(".product")
      .forEach((el) =>
        el.classList.toggle("selected", el.querySelector("input").value === hub.boardId)
      );
    $("hub-os-label").textContent = hub.plan.os_label + " · " + hub.plan.board_name;
    $("hub-pin-state").textContent = hub.plan.pinned
      ? "against this release's pinned checksum"
      : "against Home Assistant's published checksum";
    const gb = Math.round(hub.plan.min_card_bytes / 1024 ** 3);
    $("hub-card-req").textContent =
      "You'll need a " + (gb + 4) + " GB or larger card — 64 GB recommended.";
    // The USB-C on-ramp hint is per-family (CMs use the IO-board jumper;
    // the Pi 400 needs a reader) — say the right thing for the picked board.
    const board = hub.boards.find((b) => b.id === hub.boardId);
    if (board && board.usb_device_boot) {
      $("hub-pi-usb-hint").textContent = "For this board: " + board.usb_device_boot + ".";
    }
    hubArm();
  } catch (e) {
    setStatus("hub-result", "Couldn't load the hub catalog: " + e, "err");
  }
}

async function hubPollTargets() {
  if (!hub.active || hub.busy) return;
  let targets = [];
  try {
    targets = await invoke("list_hub_targets");
  } catch (e) {
    $("hub-target-sub").textContent = "Couldn't read this computer's disks: " + e;
    return;
  }
  hub.targets = targets;
  const eligible = targets.filter((t) => t.eligible);
  const refused = targets.filter((t) => !t.eligible);

  // Keep (or auto-pick) the selection: a single eligible disk selects itself —
  // plugging in the card IS the choice.
  if (hub.selected && !eligible.some((t) => t.path === hub.selected)) hub.selected = null;
  if (!hub.selected && eligible.length === 1) hub.selected = eligible[0].path;

  const list = $("hub-target-list");
  list.innerHTML = eligible.length
    ? eligible
        .map((t) => {
          const sel = t.path === hub.selected;
          const isPi = /rpi[-_ ]?msd/i.test(t.model);
          const warns = t.warnings.map((w) => `<div class="p-warn">⚠ ${esc(w)}</div>`).join("");
          return `<label class="product${sel ? " selected" : ""}">
            <input type="radio" name="hub-target" value="${esc(t.path)}" ${sel ? "checked" : ""}>
            <div><div class="p-name">${esc(t.model)}${
              isPi ? '<span class="chip-badge">your Pi, over USB-C</span>' : ""
            }</div>
            <div class="p-tag">${esc(t.path)} · ${hubFmtBytes(t.size_bytes)}</div>${warns}</div>
          </label>`;
        })
        .join("")
    : `<p class="muted">Waiting for a card or external drive…</p>`;
  list.querySelectorAll("input[name=hub-target]").forEach((r) =>
    r.addEventListener("change", (ev) => {
      hub.selected = ev.target.value;
      hubPollTargets();
    })
  );

  const hiddenBox = $("hub-hidden");
  if (refused.length) {
    hiddenBox.classList.remove("hidden");
    $("hub-hidden-summary").textContent =
      refused.length + (refused.length === 1 ? " disk is" : " disks are") + " hidden — here's why";
    $("hub-hidden-list").innerHTML = refused
      .map(
        (t) =>
          `<div class="hidden-disk"><strong>${esc(t.model)}</strong>
           <span class="mono">${esc(t.path)}</span> — ${esc(t.refused_because || "")}</div>`
      )
      .join("");
  } else {
    hiddenBox.classList.add("hidden");
  }

  $("hub-step-wifi").classList.toggle("disabled", !hub.selected);
  $("hub-step-write").classList.toggle("disabled", !hub.selected);
  hubArm();
}

async function hubPiUsbToggle() {
  if (hub.piUsbWaiting) {
    try {
      await invoke("hub_pi_boot_stop");
    } catch (e) {
      $("hub-pi-usb-status").textContent = String(e);
    }
    return; // hub:pi-usb-done resets the button
  }
  try {
    await invoke("hub_pi_boot_start");
    hub.piUsbWaiting = true;
    $("hub-pi-usb-btn").textContent = "Stop waiting";
    $("hub-pi-usb-status").textContent =
      "Waiting… hold the Pi's power button, connect USB-C, release.";
  } catch (e) {
    $("hub-pi-usb-status").textContent = String(e);
  }
}

function hubWifiValue() {
  if ($("hub-ethernet").checked) return null;
  return {
    ssid: $("hub-ssid").value.trim(),
    passphrase: $("hub-pass").value,
    hidden: $("hub-hidden-net").checked,
  };
}

function hubArm() {
  const target = hub.targets.find((t) => t.path === hub.selected);
  const wifi = hubWifiValue();
  // Mirror the backend's WPA bounds so the button never arms on input the
  // Rust side will reject: SSID 1–32 bytes, passphrase 8–63 chars (or a
  // 64-hex PMK, which the backend also accepts).
  const wifiOk =
    wifi === null ||
    (wifi.ssid.length > 0 &&
      hubByteLen(wifi.ssid) <= 32 &&
      (wifi.passphrase.length === 64
        ? /^[0-9a-fA-F]{64}$/.test(wifi.passphrase)
        : wifi.passphrase.length >= 8 && wifi.passphrase.length <= 63));
  // If the account panel has been started, it must be valid to arm — but an
  // untouched panel never blocks the flash.
  const accountOk = !hub.accountRequested || hub.accountValid;
  const armed =
    !!target &&
    wifiOk &&
    accountOk &&
    $("hub-confirm").value.trim().toUpperCase() === "ERASE" &&
    !hub.busy;
  $("hub-flash-btn").disabled = !armed;
  $("hub-write-summary").textContent = target
    ? `${hub.plan ? hub.plan.os_label : "Home Assistant OS"} → ${target.model} ` +
      `(${target.path}, ${hubFmtBytes(target.size_bytes)})` +
      (wifi === null ? " · wired ethernet" : "") +
      (hub.accountRequested && hub.accountValid ? " · account pre-made" : "")
    : "Pick a disk above first.";
}

async function hubFlash() {
  const target = hub.targets.find((t) => t.path === hub.selected);
  if (!target || hub.busy) return;
  hubStopFirstBoot(); // a fresh flash supersedes any prior first-boot watch
  hub.busy = true;
  state.busy = true; // pause the Canary port watcher during the heavy write
  $("hub-flash-btn").disabled = true;
  $("hub-stop-btn").classList.remove("hidden");
  $("hub-stop-btn").disabled = false;
  $("hub-hatch").classList.add("hidden");
  $("hub-console").textContent = "";
  $("hub-console").classList.remove("hidden");
  setStatus("hub-result", "", "");
  hubSaveSettings();
  try {
    const receipt = await invoke("hub_flash", {
      boardId: hub.boardId || (hub.plan ? hub.plan.board_id : "rpi5-64"),
      diskPath: target.path,
      confirmed: true,
      wifi: hubWifiValue(),
      account: hubAccountValue(),
    });
    hub.done = true;
    setStatus("hub-result", "Done — written, read back, and verified.", "ok");
    logEvent("ok", "Home Assistant hub written to " + target.model);
    hubShowHatch(receipt);
    hubNotify("Your card is ready", "Now boot it in your Pi — the app will tell you when it's online.");
    hubChime();
    hubStartFirstBoot();
  } catch (e) {
    hubPresentError(e);
    logEvent("err", "Hub write failed: " + e);
  } finally {
    hub.busy = false;
    state.busy = false;
    // Leave no stale theater: the bar and stop button belong to a running
    // flash only. The console keeps the full story for the curious.
    $("hub-stop-btn").classList.add("hidden");
    $("hub-progress-wrap").classList.add("hidden");
    $("hub-bar-fill").classList.remove("indet");
    $("hub-bar-fill").style.width = "0%";
    $("hub-eta").textContent = "";
    $("hub-confirm").value = "";
    hubArm();
  }
}

function hubShowHatch(receipt) {
  const wasHidden = $("hub-hatch").classList.contains("hidden");
  // All stages complete — light every pill green, then the gentle reveal.
  const box = $("hub-pills");
  if (box)
    box.innerHTML = HUB_STAGE_ORDER.map(
      (s) => `<span class="stage-pill done">${HUB_STAGE_PILL[s]}</span>`
    ).join("");
  const hatch = $("hub-hatch");
  hatch.classList.remove("hidden");
  hatch.classList.remove("alive-pop");
  void hatch.offsetWidth; // restart the animation each success
  hatch.classList.add("alive-pop");
  const wifiLine = receipt.wifi_seeded
    ? " Your Wi-Fi rides along on the card."
    : receipt.wifi_note
      ? " " + receipt.wifi_note
      : " Plug in ethernet before you power it on.";
  const acctLine = receipt.account_note ? " " + receipt.account_note : "";
  const cacheLine = receipt.used_cache ? " (reused your verified local copy — no re-download.)" : "";
  // The eject note is shown ALWAYS when present — independent of whether the
  // Wi-Fi/account seed succeeded — because a still-mounted card must be
  // ejected before it's pulled, or a CONFIG write can be left half-flushed.
  const ejectLine = receipt.eject_note ? " ⚠ " + receipt.eject_note : "";
  $("hub-hatch-body").textContent =
    `${receipt.os_label} is on ${receipt.target_path} — every byte read back and matched ` +
    `(SHA-256 ${receipt.sha256.slice(0, 16)}…).${cacheLine}${wifiLine}${acctLine}${ejectLine}`;
  // If the account was pre-made, the third step is "log in", not "create".
  const accountMade = receipt.account_seeded;
  const steps = [
    "Put the card in your Raspberry Pi (or connect the SSD) and power it on.",
    "First boot takes 10–20 minutes while Home Assistant sets itself up — the LED activity is normal.",
    accountMade
      ? `Open http://${HUB_HOST} and log in with the account you just made.`
      : `Open http://${HUB_HOST} on any device in your home and create your account.`,
    "Then follow “The Hub” guide to bring in your Canaries — securacv.com/lab → Home Assistant.",
  ];
  $("hub-hatch-steps").innerHTML = steps.map((s) => `<li>${esc(s)}</li>`).join("");
  if (wasHidden) {
    rosterAdd({ kind: "hub", name: "Home Assistant hub", chip: "" });
    updateResumeState();
  }
}

function hubFmtBytes(n) {
  if (!n) return "unknown size";
  const units = ["B", "KB", "MB", "GB", "TB"];
  let u = 0;
  let v = n;
  while (v >= 1024 && u < units.length - 1) {
    v /= 1024;
    u++;
  }
  return (u ? v.toFixed(1) : v) + " " + units[u];
}

window.addEventListener("DOMContentLoaded", () => {
  boot();
  hubInit();
});
