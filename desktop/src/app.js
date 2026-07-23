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
    invoke("serial_monitor_send", { command: "j" }).catch((e) =>
      setStatus("monitor-status", String(e), "err"))
  );
  $("update-btn").addEventListener("click", onInstallUpdate);
  const rerollBtn = $("cert-reroll");
  if (rerollBtn) rerollBtn.addEventListener("click", rerollCertificate);
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
const VIEWS = ["canary", "hub", "atlas", "about"];

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
    hubPollTargets();
    if (!hub.pollTimer) hub.pollTimer = setInterval(hubPollTargets, 2000);
  } else if (hub.pollTimer) {
    clearInterval(hub.pollTimer);
    hub.pollTimer = null;
  }
  if (view === "atlas") renderAtlas();
  if (view === "about") renderAbout();

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
    if (requiresLiveReceipt(state.product)) {
      setStatus("flash-result", "Firmware write verified. Watching the live boot for its device receipt…", "ok");
      state.busy = false;
      await startMonitor();
    } else {
      setStatus("flash-result", "Firmware write verified. Flashing is complete. ✓", "ok");
      maybeHatch();
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
}

function appendConsole(id, text) {
  const con = $(id);
  con.textContent += String(text);
  con.scrollTop = con.scrollHeight;
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

function mintCertificate(product) {
  const h = state.hatch;
  if (!h || !Array.isArray(h.first) || !h.first.length) return null;
  const pick = (a) => a[Math.floor(Math.random() * a.length)];
  const base = pick(h.first);
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
  const next = mintCertificate(product);
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
  plan: null,
  pollTimer: null,
  busy: false,
  done: false,
  piUsbWaiting: false,
};

const HUB_STAGE_COPY = {
  download: "Downloading Home Assistant OS…",
  decompress: "Unpacking the image…",
  write: "Writing to the card — don't remove it…",
  verify: "Reading every byte back to prove the write…",
  seed: "Seeding your Wi-Fi onto the card…",
};

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
    $("hub-progress-wrap").classList.remove("hidden");
    $("hub-stage").textContent = HUB_STAGE_COPY[stage] || stage;
    const fill = $("hub-bar-fill");
    if (total) {
      fill.style.width = Math.min(100, Math.round((done / total) * 100)) + "%";
    } else {
      fill.style.width = "100%";
    }
  });
}

async function hubLoadPlan() {
  if (hub.plan) return;
  try {
    hub.plan = await invoke("hub_plan", { boardId: null });
    $("hub-os-label").textContent = hub.plan.os_label;
    $("hub-pin-state").textContent = hub.plan.pinned
      ? "against this release's pinned checksum"
      : "against Home Assistant's published checksum";
    const gb = Math.round(hub.plan.min_card_bytes / 1024 ** 3);
    $("hub-card-req").textContent =
      "You'll need a " + (gb + 4) + " GB or larger card — 64 GB recommended.";
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
  const wifiOk = wifi === null || (wifi.ssid.length > 0 && wifi.passphrase.length >= 8);
  const armed =
    !!target && wifiOk && $("hub-confirm").value.trim().toUpperCase() === "ERASE" && !hub.busy;
  $("hub-flash-btn").disabled = !armed;
  $("hub-write-summary").textContent = target
    ? `${hub.plan ? hub.plan.os_label : "Home Assistant OS"} → ${target.model} ` +
      `(${target.path}, ${hubFmtBytes(target.size_bytes)})` +
      (wifi === null ? " · wired ethernet" : "")
    : "Pick a disk above first.";
}

async function hubFlash() {
  const target = hub.targets.find((t) => t.path === hub.selected);
  if (!target || hub.busy) return;
  hub.busy = true;
  state.busy = true; // pause the Canary port watcher during the heavy write
  $("hub-flash-btn").disabled = true;
  $("hub-console").textContent = "";
  $("hub-console").classList.remove("hidden");
  setStatus("hub-result", "", "");
  try {
    const receipt = await invoke("hub_flash", {
      boardId: hub.plan ? hub.plan.board_id : "rpi5-64",
      diskPath: target.path,
      confirmed: true,
      wifi: hubWifiValue(),
    });
    hub.done = true;
    setStatus("hub-result", "Done — written, read back, and verified.", "ok");
    logEvent("ok", "Home Assistant hub written to " + target.model);
    hubShowHatch(receipt);
  } catch (e) {
    setStatus("hub-result", String(e), "err");
    logEvent("err", "Hub write failed: " + e);
  } finally {
    hub.busy = false;
    state.busy = false;
    $("hub-confirm").value = "";
    hubArm();
  }
}

function hubShowHatch(receipt) {
  const wasHidden = $("hub-hatch").classList.contains("hidden");
  $("hub-hatch").classList.remove("hidden");
  $("hub-hatch-body").textContent =
    `${receipt.os_label} is on ${receipt.target_path} — every byte read back and matched ` +
    `(SHA-256 ${receipt.sha256.slice(0, 16)}…).` +
    (receipt.wifi_seeded
      ? " Your Wi-Fi rides along on the card."
      : " Plug in ethernet before you power it on.");
  const steps = [
    "Put the card in your Raspberry Pi (or connect the SSD) and power it on.",
    "First boot takes 10–20 minutes while Home Assistant sets itself up — the LED activity is normal.",
    "Open http://homeassistant.local:8123 on any device in your home and create your account.",
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
