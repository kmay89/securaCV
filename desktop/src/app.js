// SecuraCV Flasher — front-end glue.
//
// Plain JS on purpose: no framework, no build step. It talks to the Rust
// backend through Tauri's global bridge (withGlobalTauri) and never touches
// Web Serial — the OS-native flashing all happens in Rust.
//
// The connection is watched *live*, IDE-style: a background poll enumerates the
// USB ports every second and a persistent status bar reflects the state —
// scanning → found → reading chip → connected → (or) unplugged — with no
// "Connect" button to press. Chip identification (which momentarily talks to the
// board) runs once per freshly-seen port, not on every poll.

const { invoke } = window.__TAURI__.core;
const { listen } = window.__TAURI__.event;

const $ = (id) => document.getElementById(id);
const openExternal = (url) =>
  invoke("plugin:opener|open_url", { url }).catch(() => {});

// Fold case/spacing/hyphens so "ESP32-S3" / "esp32s3" compare equal — the
// same guard the Rust side and the website use.
const normChip = (s) => String(s || "").toUpperCase().replace(/[\s\-_]+/g, "");

const POLL_MS = 1000;

const state = {
  catalog: null,
  manifest: null,
  port: null,        // the port we're currently tracking
  chip: null,        // identified chip for state.port, or null
  product: null,
  detecting: false,  // a detect_chip call is in flight
  failedPort: null,  // a port whose chip read failed — don't auto-retry it
  busy: false,       // a flash is running — pause the watcher
};

// ── boot ───────────────────────────────────────────────────────────────────
async function boot() {
  try {
    state.catalog = await invoke("load_catalog");
    const tag = $("version-tag");
    if (state.catalog.fw_train)
      tag.textContent = "· firmware train " + state.catalog.fw_train;
  } catch (e) {
    setConn("failed", "Couldn't load the catalog: " + e);
  }

  document.querySelectorAll("[data-open]").forEach((a) =>
    a.addEventListener("click", (ev) => {
      ev.preventDefault();
      openExternal(a.dataset.open);
    })
  );

  $("flash-btn").addEventListener("click", onFlash);
  $("update-btn").addEventListener("click", onInstallUpdate);
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
    state.port = e.target.value;
    state.chip = null;
    state.failedPort = null;
    resetSteps();
    pollPorts();
  });

  checkForUpdate();     // best-effort, in the background
  pollPorts();          // first tick now…
  setInterval(pollPorts, POLL_MS); // …then keep watching
}

// ── the live watcher ─────────────────────────────────────────────────────────
async function pollPorts() {
  if (state.busy) return; // don't poke the port mid-flash

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
    state.chip = null;
    state.failedPort = null;
    resetSteps();
  }

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
  state.port = null;
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
  hideHatchCard();
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
  const btn = $("flash-btn");
  btn.disabled = !ver;
  $("flash-target").textContent = ver
    ? `${p.name} → ${state.port}`
    : "No published release for this one yet.";
}

// ── step 3: flash ───────────────────────────────────────────────────────────
async function onFlash() {
  const btn = $("flash-btn");
  const con = $("console");
  con.textContent = "";
  con.classList.remove("hidden");
  btn.disabled = true;
  btn.textContent = "Flashing…";
  setStatus("flash-result", "");
  state.busy = true; // pause the watcher so it can't grab the port

  const unlisten = await listen("flash:log", (ev) => {
    con.textContent += ev.payload + "\n";
    con.scrollTop = con.scrollHeight;
  });

  try {
    await invoke("flash", {
      port: state.port,
      productId: state.product.id,
      manifestUrl: state.catalog.manifest_url,
      baud: state.catalog.flash_baud || 921600,
    });
    setStatus("flash-result", "Done — your Canary is booting its new firmware. ✓", "ok");
    showHatchCard(state.product);
  } catch (e) {
    setStatus("flash-result", String(e), "err");
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

// ── self-update ─────────────────────────────────────────────────────────────
async function checkForUpdate() {
  try {
    const up = await invoke("check_update");
    if (up) {
      $("update-text").textContent = `Version ${up.version} is ready (you have ${up.current_version}).`;
      $("update-banner").classList.remove("hidden");
    }
  } catch (_) {
    // No signing key configured yet, or offline — stay quiet.
  }
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
    btn.disabled = false;
  } finally {
    unlisten();
  }
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
}

function hatchMoment(product) {
  if (product && product.hatch && Array.isArray(product.hatch.steps)) {
    return product.hatch;
  }

  const id = product && product.id;
  if (id && id.includes("vision")) {
    return {
      kicker: "Canary hatched",
      title: "Your Vision Canary is waking up.",
      body: "Give it one visible, privacy-safe thing to notice immediately: presence only, no faces and no saved frames.",
      steps: [
        "If you have not flashed the Grove Vision AI V2 module yet, move the USB-C cable to the module port and flash the pinned model.",
        "Put the board where it can see a doorway, then walk through once.",
        "Open Home Assistant and watch the presence entity flip to detected, then clear."
      ]
    };
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

window.addEventListener("DOMContentLoaded", boot);
