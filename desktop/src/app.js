// SecuraCV Flasher — front-end glue.
//
// Plain JS on purpose: no framework, no build step. It talks to the Rust
// backend through Tauri's global bridge (withGlobalTauri) and never touches
// Web Serial — the OS-native flashing all happens in Rust.

const { invoke } = window.__TAURI__.core;
const { listen } = window.__TAURI__.event;

const $ = (id) => document.getElementById(id);
const openExternal = (url) =>
  invoke("plugin:opener|open_url", { url }).catch(() => {});

// Fold case/spacing/hyphens so "ESP32-S3" / "esp32s3" compare equal — the
// same guard the Rust side and the website use.
const normChip = (s) =>
  String(s || "").toUpperCase().replace(/[\s\-_]+/g, "");

const state = {
  catalog: null,
  manifest: null,
  port: null,
  chip: null,
  product: null,
};

// ── boot ───────────────────────────────────────────────────────────────────
async function boot() {
  try {
    state.catalog = await invoke("load_catalog");
    const tag = $("version-tag");
    if (state.catalog.fw_train)
      tag.textContent = "· firmware train " + state.catalog.fw_train;
  } catch (e) {
    setStatus("detect-status", "Couldn't load the catalog: " + e, "err");
  }

  // External links open in the real browser, not inside the app.
  document.querySelectorAll("[data-open]").forEach((a) =>
    a.addEventListener("click", (ev) => {
      ev.preventDefault();
      openExternal(a.dataset.open);
    })
  );

  $("detect-btn").addEventListener("click", onDetect);
  $("port-select").addEventListener("change", (e) => {
    state.port = e.target.value;
  });
  $("flash-btn").addEventListener("click", onFlash);
  $("update-btn").addEventListener("click", onInstallUpdate);
  $("update-dismiss").addEventListener("click", () =>
    $("update-banner").classList.add("hidden")
  );

  checkForUpdate(); // best-effort, in the background
}

// ── step 1: find the board + detect its chip ────────────────────────────────
async function onDetect() {
  const btn = $("detect-btn");
  btn.disabled = true;
  setStatus("detect-status", "Looking for your Canary…");
  $("download-mode").classList.add("hidden");

  let ports;
  try {
    ports = await invoke("list_ports");
  } catch (e) {
    setStatus("detect-status", "Serial error: " + e, "err");
    btn.disabled = false;
    return;
  }

  const usb = ports.filter((p) => p.kind === "usb");
  const pick = usb.length ? usb : ports;
  if (!pick.length) {
    setStatus(
      "detect-status",
      "No serial device found. Use a data USB-C cable and replug.",
      "err"
    );
    $("download-mode").classList.remove("hidden");
    btn.disabled = false;
    return;
  }

  // Show a picker when there's more than one candidate; otherwise auto-pick.
  const sel = $("port-select");
  sel.innerHTML = "";
  pick.forEach((p) => {
    const o = document.createElement("option");
    o.value = p.name;
    o.textContent = p.product ? `${p.name} — ${p.product}` : p.name;
    sel.appendChild(o);
  });
  if (pick.length > 1) sel.classList.remove("hidden");
  state.port = sel.value || pick[0].name;

  setStatus("detect-status", `Reading the chip on ${state.port}…`);
  try {
    state.chip = await invoke("detect_chip", { port: state.port });
  } catch (e) {
    setStatus("detect-status", String(e), "err");
    $("download-mode").classList.remove("hidden");
    btn.disabled = false;
    return;
  }

  setStatus(
    "detect-status",
    `Found an ${state.chip} on ${state.port}. ✓`,
    "ok"
  );
  btn.disabled = false;
  btn.textContent = "Find again";

  // Pull the live release manifest so we can show versions (best-effort).
  invoke("fetch_manifest", { manifestUrl: state.catalog.manifest_url })
    .then((m) => {
      state.manifest = m;
      renderProducts();
    })
    .catch(() => {
      state.manifest = null;
    });

  renderProducts();
  enableCard("step-pick");
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

  matches.forEach((p) => {
    const ver =
      state.manifest &&
      state.manifest.products &&
      state.manifest.products[p.id] &&
      state.manifest.products[p.id].version;

    const row = document.createElement("label");
    row.className = "product";
    row.innerHTML = `
      <input type="radio" name="product" value="${p.id}">
      <span>
        <span class="p-name">${esc(p.name)}<span class="chip-badge">${esc(p.chip)}</span></span>
        <span class="p-tag">${esc(p.tagline || "")}</span>
        <span class="p-meta">${
          ver ? "release " + esc(ver) : "no published release yet"
        }</span>
      </span>`;
    const radio = row.querySelector("input");
    radio.addEventListener("change", () => {
      document
        .querySelectorAll(".product")
        .forEach((el) => el.classList.remove("selected"));
      row.classList.add("selected");
      state.product = p;
      onProductChosen(p, ver);
    });
    list.appendChild(row);
  });
}

function onProductChosen(p, ver) {
  enableCard("step-flash");
  const btn = $("flash-btn");
  const hasImage = !!ver;
  btn.disabled = !hasImage;
  $("flash-target").textContent = hasImage
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
    setStatus(
      "flash-result",
      "Done — your Canary is booting its new firmware. ✓",
      "ok"
    );
  } catch (e) {
    setStatus("flash-result", String(e), "err");
  } finally {
    unlisten();
    btn.disabled = false;
    btn.textContent = "Flash my Canary";
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
function setStatus(id, msg, kind) {
  const el = $(id);
  el.textContent = msg;
  el.className = "status" + (kind ? " " + kind : "");
}
function enableCard(id) {
  $(id).classList.remove("disabled");
}
function esc(s) {
  return String(s).replace(/[&<>"]/g, (c) =>
    ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c])
  );
}

window.addEventListener("DOMContentLoaded", boot);
