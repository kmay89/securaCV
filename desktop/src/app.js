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

// The firmware release tag a manifest URL is pinned to ("fw-v2.3.0"), or null if
// it isn't a pinned release asset. Deliberately the same behavior as
// canary-local/assets/flash-core.js releaseTagFromManifestUrl() — the two
// flashers do NOT share a frontend, so a diagnostic added to one has to be
// added to the other or half our users keep getting the vague version.
const releaseTagFromManifestUrl = (url) => {
  const m = /\/releases\/download\/([^/]+)\//.exec(String(url || ""));
  if (!m) return null;
  const tag = decodeURIComponent(m[1]);
  return /^fw-v/.test(tag) ? tag : null;
};

// The size gate — parity with canary-local/assets/flash-core.js
// flashFitVerdict(), same words on purpose. The chip guard says "right
// silicon"; this says "enough of it": a product built for a BIGGER flash
// than the chip in hand writes fine and then boot-loops before the app
// prints a single line (esp_core_dump_flash "config is corrupted" + an
// instant RTC_SW_CPU_RST reboot — the XIAO-Sense-with-a-Freenove-image
// mistake). Smaller-on-bigger is fine (headroom); an unmeasured chip is
// never judged.
const flashFitVerdict = (product, flashBytes) => {
  const need = product && Number(product.flash_mb);
  const mb = flashBytes ? Math.round(flashBytes / (1024 * 1024)) : null;
  if (!need || !mb || need <= mb) return { fits: true, needMb: need || null, haveMb: mb };
  return {
    fits: false, needMb: need, haveMb: mb,
    why: `${product.name} is built for a ${need} MB board ` +
      `(${product.board_label}), but the connected chip has ${mb} MB of ` +
      `flash. The write would finish and the board would then boot-loop ` +
      `before printing anything — wrong-board image, not a broken board.`,
  };
};

// The dev channel's one stable address: the rolling fw-dev-latest prerelease.
// A fixed first-party constant, deliberately NOT a URL override — the toggle
// can only ever mean this URL, and the Rust side accepts it as the ONE
// alternative to the catalog's pinned manifest. Same constant as
// canary-local/assets/flash-core.js DEV_FLASH_MANIFEST_URL (the two flashers
// share no code; the desktop-parity test keeps them agreeing).
const DEV_FLASH_MANIFEST_URL =
  "https://github.com/kmay89/securaCV/releases/download/fw-dev-latest/manifest-flash.json";

const POLL_MS = 1000;
// Self-update cadence. Checked once at launch, then routinely while the app
// stays open — a bench machine that never relaunches must still hear about
// updates — and again when the window regains focus after sitting stale.
const UPDATE_RECHECK_MS = 6 * 60 * 60 * 1000; // every 6 hours while open
const UPDATE_STALE_MS = 4 * 60 * 60 * 1000;   // focus re-check if older than this
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
  flashBytes: null,  // detected flash size (bytes) for state.chip — the rescue backup needs it
  mac: null,         // detected MAC (for the health report + backup name), or null
  product: null,
  detecting: false,  // a detect_chip call is in flight
  failedPort: null,  // a port whose chip read failed — don't auto-retry it
  busy: false,       // a flash is running — pause the watcher
  monitoring: false,
  devChannel: false, // fetch fw-dev-latest instead of the pinned stable release
  localFile: null,   // { path, name, size, sha256, esp_magic } picked under Advanced
  update: null,      // pending self-update, if any
  announcedUpdate: null, // last update version logged, so routine re-checks stay quiet
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

// ── the secret drawer ────────────────────────────────────────────────────────
// Where the setup profile's passwords and each Canary's API token live: the
// OS credential store when this platform has one (macOS Keychain, Windows
// Credential Manager), else this app's own prefs file — and the consent copy
// names which, instead of pretending. Keys are namespaced and URI-encoded
// (an SSID can hold spaces/emoji); prefs.secretKeys tracks every key ever
// written so "Reset the app's memory" can sweep the OS store too.
const secretStore = {
  backend: "none",
  _ready: null,
  // Idempotent; every accessor awaits it, so a get() fired from early init
  // (hubRestoreSettings runs at DOMContentLoaded) can't race the backend
  // answer and wrongly fall back to the prefs file.
  init() {
    this._ready = this._ready || (async () => {
      try { this.backend = await invoke("secret_backend"); } catch (_) { this.backend = "none"; }
    })();
    return this._ready;
  },
  where() {
    return this.backend === "keychain" ? "your Mac's Keychain"
      : this.backend === "credential-manager" ? "Windows Credential Manager"
      : "this app's local settings";
  },
  key(...parts) { return parts.map((p) => encodeURIComponent(String(p))).join(":"); },
  track(key, on) {
    prefs.secretKeys = prefs.secretKeys || [];
    const i = prefs.secretKeys.indexOf(key);
    if (on && i < 0) prefs.secretKeys.push(key);
    if (!on && i >= 0) prefs.secretKeys.splice(i, 1);
    savePrefs();
  },
  async set(key, value) {
    await this.init();
    if (!value) return this.delete(key);
    if (this.backend !== "none") {
      try {
        await invoke("secret_set", { key, value });
        if (prefs.secrets && key in prefs.secrets) { delete prefs.secrets[key]; }
        this.track(key, true);
        return true;
      } catch (e) {
        // The OS store exists but refused (locked, permission declined).
        // Never quietly downgrade to the prefs file — the consent note
        // beside the checkbox promised the OS store, so a silent weaker
        // fallback would make that copy a lie. Store nowhere, say so.
        logEvent("err", "Couldn't save to " + this.where() + ": " + e);
        return false;
      }
    }
    // No OS store on this platform — the prefs file IS the advertised
    // destination here (the consent note names it).
    prefs.secrets = prefs.secrets || {};
    prefs.secrets[key] = value;
    this.track(key, true);
    return true;
  },
  async get(key) {
    await this.init();
    if (this.backend !== "none") {
      try {
        const v = await invoke("secret_get", { key });
        if (v != null) return v;
      } catch (_) {}
    }
    return (prefs.secrets && prefs.secrets[key]) || null;
  },
  async delete(key) {
    await this.init();
    let ok = true;
    if (this.backend !== "none") {
      try { await invoke("secret_delete", { key }); }
      catch (e) {
        // Deletion refused (store locked). Keep the key TRACKED — forgetting
        // it here would make the next "Reset the app's memory" report success
        // while the credential still sits in the OS store.
        ok = false;
        logEvent("err", "Couldn't remove from " + this.where() + ": " + e);
      }
    }
    if (prefs.secrets && key in prefs.secrets) delete prefs.secrets[key];
    if (ok) this.track(key, false);
    else savePrefs();
    return ok;
  },
  // Returns how many entries could NOT be removed, so the caller can report
  // an incomplete sweep instead of a false clean bill.
  async wipeAll() {
    let failed = 0;
    for (const k of [...(prefs.secretKeys || [])]) {
      if (!(await this.delete(k))) failed++;
    }
    prefs.secrets = {};
    if (!failed) prefs.secretKeys = [];
    savePrefs();
    return failed;
  },
};

// The device's local-API bearer credential, minted HERE so the fleet book
// knows it without a pairing dance. Same alphabet + unbiased rejection
// sampling as the firmware's own format_api_token_string and the browser
// flasher's mintApiToken (flash-core.js): reject bytes >= 248 (248 = 62*4),
// modulo below that is exactly uniform.
const TOKEN_ALPHABET = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
function mintApiToken() {
  for (;;) {
    const bytes = crypto.getRandomValues(new Uint8Array(96));
    let out = "";
    for (const b of bytes) {
      if (b >= 248) continue; // unbiased base62
      out += TOKEN_ALPHABET[b % 62];
      if (out.length === 32) return "cv_" + out;
    }
    // 96 bytes practically never yields fewer than 32 keepers — loop for form.
  }
}

// ── flash-failure classification (parity with the browser flasher) ──────────
// The browser's classifyFlashError (flash-core.js) turns a raw error into a
// named kind + a specific fix; the desktop used to print String(e) and hope.
// Same kinds, same fixes, with the match patterns widened for espflash and
// serialport-rs phrasings alongside esptool-js ones.
function classifyFlashError(err) {
  const msg = String((err && (err.message || err.name)) || err || "").toLowerCase();
  const has = (...subs) => subs.some((s) => msg.includes(s));

  if (has("failed to open serial port", "port is already open", "already open",
          "resource temporarily unavailable", "resource busy", "device or resource busy") ||
      (has("open") && has("access", "busy", "in use")))
    return { kind: "port-busy", title: "That port is busy",
      hint: "Another program is holding the board. Close the Arduino IDE / PlatformIO " +
        "serial monitor and any browser-flasher tab, then unplug, replug, and try " +
        "again. On Linux the holder is often ModemManager probing a just-plugged " +
        "board — wait ~30 s, or add the udev rule from INSTALL.md (Linux section) " +
        "so it ignores Canaries for good." };

  if (has("device has been lost", "device lost", "no device", "disconnected",
          "device not configured", "input/output error", "broken pipe"))
    return { kind: "device-lost", title: "The board disappeared",
      hint: "The connection dropped — usually the cable, the port, or the board " +
        "resetting. Reseat the USB-C cable (a data cable, not charge-only) and " +
        "reconnect. Nothing was harmed — you can't brick it from here." };

  if (has("access denied", "permission denied", "permission", "not allowed"))
    return { kind: "permission", title: "The system wouldn't grant access to the port",
      hint: "On Linux, add yourself to the dialout group (sudo usermod -aG dialout " +
        "$USER, then log out and back in); on Windows, install the board's USB-serial " +
        "driver. Then reconnect." };

  // Integrity BEFORE the generic download case, so a checksum message never
  // reads as a network problem.
  if (has("checksum", "sha-256", "sha256", "md5", "hash mismatch", "failed its", "signature"))
    return { kind: "integrity", title: "The image failed its integrity check",
      hint: "Nothing was written. The download may have been corrupted, or the signed " +
        "release is mid-update — wait a moment and try again." };

  if (has("download failed", "http ", "failed to fetch", "couldn't reach", "manifest", "dns"))
    return { kind: "download", title: "Couldn't download the firmware image",
      hint: "That's a network or release issue, not your board. Check your connection " +
        "and try again; if it persists, the signed release may be mid-update. You can " +
        "also install a local .bin under Advanced." };

  if (has("timed out", "timeout", "no serial data", "failed to sync", "failed to connect",
          "invalid head of packet", "wrong boot mode"))
    return { kind: "not-in-download", title: "The board isn't answering the bootloader",
      hint: "It's almost certainly not in download mode. Hold BOOT, tap RESET, release " +
        "BOOT, then reconnect." };

  if (has("short read", "stalled", "read timeout"))
    return { kind: "read-stall", title: "A read stalled partway",
      hint: "Reseat the cable and try again — a shorter, known-good USB-C data cable is " +
        "the usual fix." };

  return { kind: "unknown", title: null,
    hint: "If this keeps happening: unplug the board, plug it back in, put it in download " +
      "mode (hold BOOT, tap RESET, release BOOT), and retry." };
}

// USB-serial bridge chips that need an OS driver (parity: flash-core.js
// usbBridgeInfo). The XIAO boards use Espressif's native USB (0x303A) — no
// driver, no note.
const USB_BRIDGES = {
  0x10c4: { name: "Silicon Labs CP210x", driverUrl: "https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers" },
  0x1a86: { name: "WCH CH340/CH343", driverUrl: "https://www.wch-ic.com/downloads/CH341SER_EXE.html" },
  0x0403: { name: "FTDI FT232", driverUrl: "https://ftdichip.com/drivers/vcp-drivers/" },
};
const NATIVE_USB_VENDORS = new Set([0x303a]);
function usbBridgeInfo(vid, pid) {
  if (vid == null) return null;
  if (NATIVE_USB_VENDORS.has(vid)) return null;
  const b = USB_BRIDGES[vid];
  if (!b) return null;
  return {
    name: b.name, driverUrl: b.driverUrl, vid, pid: pid == null ? null : pid,
    note: `This board talks over a ${b.name} USB-serial chip. If it won't connect, ` +
      `install that chip's driver (Windows and older macOS need it), then reconnect.`,
  };
}

// One-click copyable diagnostic (parity: flash-core.js buildDiagnosticReport)
// — everything a bug report needs, gathered instead of asked for.
function buildDiagnosticReport(info = {}) {
  const lines = ["SecuraCV Flasher diagnostic", "==========================="];
  const add = (label, val) => {
    if (val === undefined || val === null || val === "") return;
    lines.push(`${label}: ${val}`);
  };
  add("when", info.when);
  add("app", info.app);
  add("platform", info.platform);
  add("firmware train", info.catalogVersion);
  add("chip", info.chip);
  add("MAC", info.mac);
  add("flash size", info.flashBytes ? info.flashBytes + " bytes" : undefined);
  add("USB device", info.usb);
  add("chosen product", info.product);
  add("stage", info.stage);
  add("error", info.error);
  if (info.logTail) {
    lines.push("--- last console output ---");
    lines.push(String(info.logTail).split("\n").slice(-12).join("\n"));
  }
  return lines.join("\n") + "\n";
}

// The standard Wi-Fi QR payload (parity: flash-core.js wifiQrString) — WPA
// assumed unless the password is empty; backslash-escape \ ; , " :
function wifiQrString(ssid, pass) {
  const esc2 = (s) => String(s).replace(/([\\;,":])/g, "\\$1");
  return pass
    ? `WIFI:T:WPA;S:${esc2(ssid)};P:${esc2(pass)};;`
    : `WIFI:T:nopass;S:${esc2(ssid)};;`;
}

// Per-setting help topics from the embedded catalog's settings_help registry
// (parity: flash-core.js helpTopic + flash.js helpDot). The registry is
// generated from the firmware's own values, so the help can't drift from
// what the code does.
// The settings_help registry was written for the browser page, and two of its
// entries name browser mechanics — the address bar, the Downloads folder —
// that are simply false in this app. Same topics, this frontend's words;
// layered over the registry entry so its other fields still show through.
const HELP_DESKTOP_OVERRIDES = {
  dev_channel: {
    what: "The checkbox below switches this app to the rolling prerelease — signed with the same key, newer, less soaked.",
    when: "You're testing a fix the maintainer just cut. Untick it to return to stable.",
  },
  skip_backup: {
    what: "Normally every byte on the board is saved into this app's backups folder before anything is written — the console names the file, and it is your undo button.",
  },
  wifi_bake: {
    what: "Typed here, your network is written into the chip's settings region in the same pass as the firmware — the Canary joins it on first boot. It never leaves this app except over the USB cable.",
  },
};
function helpTopic(id) {
  const reg = state.catalog && state.catalog.settings_help;
  const t = reg && typeof reg === "object" ? reg[id] : null;
  if (!(t && t.label && t.what)) return null;
  return { ...t, ...HELP_DESKTOP_OVERRIDES[id] };
}
function helpDot(id) {
  const t = helpTopic(id);
  if (!t) return null;
  const btn = document.createElement("button");
  btn.type = "button";
  btn.className = "help-dot";
  btn.textContent = "?";
  btn.setAttribute("aria-label", "About " + t.label);
  btn.addEventListener("click", () => {
    let pop = btn.nextElementSibling;
    if (pop && pop.classList.contains("help-pop")) { pop.remove(); return; }
    pop = document.createElement("span");
    pop.className = "help-pop";
    pop.textContent = t.what + (t.why ? " " + t.why : "");
    btn.after(pop);
  });
  return btn;
}

// ── the fleet book ───────────────────────────────────────────────────────────
// A durable record of every Canary this app flashed: who they are, what they
// run, when they were last seen on the network. Keyed by MAC when the chip
// reported one (stable across renames), else the device id. Live status and
// OTA ride on top of this in the Fleet tab (fleetBook controller below).
function bookAll() { return prefs.book || (prefs.book = {}); }
function bookKey(mac, deviceId) { return (mac && String(mac).toLowerCase()) || (deviceId ? "id:" + deviceId : null); }
function bookUpsert(patch) {
  const key = bookKey(patch.mac, patch.deviceId);
  if (!key) return null;
  const book = bookAll();
  const entry = book[key] || { key, addedAt: Date.now() };
  for (const [k, v] of Object.entries(patch)) {
    if (v !== undefined && v !== null && v !== "") entry[k] = v;
  }
  entry.key = key;
  book[key] = entry;
  savePrefs();
  fleetBook.renderSoon();
  return entry;
}
function bookRemove(key) {
  const book = bookAll();
  const entry = book[key];
  delete book[key];
  savePrefs();
  if (entry) secretStore.delete(secretStore.key("canary", entry.mac || entry.deviceId, "token"));
  fleetBook.renderSoon();
}
// The most recent flash's book entry, so late arrivals (serial receipt,
// certificate name) know which row to enrich without re-deriving identity.
let lastBookKey = null;

// ── the setup profile: type it once, every later flash fills itself in ──────
// The non-secret fields already persist live (PROV_FIELDS above). These three
// close the loop on the secrets: the consent note names where they go, a
// successful flash saves them, and opening the form gets them back.
function profileNoteRefresh() {
  const note = $("prov-remember-note");
  if (!note) return;
  note.textContent = $("prov-remember").checked
    ? `Wi-Fi and broker passwords go to ${secretStore.where()}; names, hosts and ports stay in this app. A reflash fills every field back in.`
    : "Nothing is saved — you'll retype the passwords next time.";
  const hubNote = $("hub-remember-note");
  if (hubNote) {
    hubNote.textContent = $("hub-remember").checked
      ? `Passwords go to ${secretStore.where()} — a relaunch can pick up right where you left off.`
      : "";
  }
}

// Fill the password fields from the profile — never over something typed,
// and say so out loud when it happens (a silent prefill of a secret reads
// as a leak). Best-effort: any failure just means typing it again.
async function profileAutofill() {
  try {
    if (!$("prov-remember").checked) return;
    const filled = [];
    const ssid = $("wifi-ssid").value.trim();
    if (ssid && !$("wifi-pass").value) {
      const pw = await secretStore.get(secretStore.key("wifi", ssid));
      if (pw && !$("wifi-pass").value && $("wifi-ssid").value.trim() === ssid) {
        $("wifi-pass").value = pw;
        filled.push(`Wi-Fi password for “${ssid}”`);
      }
    }
    const host = $("mqtt-host").value.trim();
    if (host && !$("mqtt-pass").value) {
      const user = $("mqtt-user").value.trim();
      const pw = await secretStore.get(secretStore.key("mqtt", host, user));
      if (pw && !$("mqtt-pass").value && $("mqtt-host").value.trim() === host) {
        $("mqtt-pass").value = pw;
        filled.push("broker password");
      }
    }
    const note = $("prov-autofill-note");
    if (note && filled.length) {
      note.textContent = `✓ Filled from your setup profile (${secretStore.where()}): ${filled.join(" and ")}.`;
      note.classList.remove("hidden");
    }
  } catch (_) { /* autofill is a nicety, never a blocker */ }
}

// A flash that succeeded IS the consent moment the checkbox announced:
// remember exactly what was written, so the next board is a plug-in and a
// click. Passwords by SSID / broker, so two homes don't overwrite each other.
// A refused save (Keychain locked) is surfaced right under the form — the
// flash itself is unaffected, but "remembered" must never be assumed.
function profileSaveFromFlash(p) {
  try {
    if (!$("prov-remember").checked || !p) return;
    const saves = [];
    if (p.wifiSsid) saves.push(secretStore.set(secretStore.key("wifi", p.wifiSsid), p.wifiPass || ""));
    if (p.mqttHost && p.mqttPass) {
      saves.push(secretStore.set(secretStore.key("mqtt", p.mqttHost, p.mqttUser || ""), p.mqttPass));
    }
    Promise.all(saves).then((results) => {
      if (results.every(Boolean)) return;
      const note = $("prov-autofill-note");
      if (note) {
        note.textContent =
          `⚠ Couldn't save the password${results.length > 1 ? "s" : ""} to ${secretStore.where()}` +
          " (locked, or permission declined) — this flash is unaffected, but you'll retype next time.";
        note.classList.remove("hidden");
      }
    }).catch(() => {});
  } catch (_) {}
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

  // The shell painted, so this launch actually arrived — tell the launch
  // guard before doing anything that could fail.
  //
  // What it is watching for is the launch where none of this runs at all: a
  // force quit can leave the webview's saved session mid-write, and the
  // synchronous `localStorage` read at the top of this file then wedges before
  // first paint. From outside, that is a Dock icon bouncing forever with no
  // window and nothing to click. The native side can only recognize it by its
  // absence, so a good launch has to say so out loud. Deliberately early and
  // deliberately not awaited: a catalog that won't load is a visible error in
  // a working window, not a failed launch.
  // See src-tauri/src/launch_guard.rs.
  invoke("ui_ready").catch(() => { /* older backend — nothing to report to */ });

  try {
    state.catalog = await invoke("load_catalog");
    renderAccessNotes();
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
  // The nursery strip counts hatchlings from THIS app session.
  state.sessionStart = Date.now();
  // Which secret store this platform offers decides the consent wording —
  // ask first, then word the profile notes from the real answer.
  secretStore.init().then(profileNoteRefresh);
  // Wi-Fi as a QR (parity with the browser flasher) — rendered on demand,
  // never automatically: a password on screen is a choice, not a default.
  $("wifi-qr-btn").addEventListener("click", async () => {
    const box = $("wifi-qr-box");
    if (!box.classList.contains("hidden")) { box.classList.add("hidden"); box.innerHTML = ""; return; }
    const ssid = $("wifi-ssid").value.trim();
    if (!ssid) { setStatus("flash-result", "Type the Wi-Fi name first — the QR encodes it.", "err"); return; }
    try {
      const { default: qrcode } = await import("./vendor/qrcode/qrcode.mjs");
      const qr = qrcode(0, "M");
      qr.addData(wifiQrString(ssid, $("wifi-pass").value));
      qr.make();
      box.innerHTML = qr.createSvgTag({ cellSize: 4, margin: 2 });
      box.classList.remove("hidden");
    } catch (_) { /* the QR is a bonus; typing still works */ }
  });
  // The QR is a snapshot of the fields at click time — an edit makes it a
  // lie, and a camera can still scan the OLD password off the screen. Any
  // change to what it encoded clears it.
  const wifiQrInvalidate = () => {
    const box = $("wifi-qr-box");
    box.classList.add("hidden");
    box.innerHTML = "";
  };
  $("wifi-ssid").addEventListener("input", wifiQrInvalidate);
  $("wifi-pass").addEventListener("input", wifiQrInvalidate);
  // One-click diagnostic report (parity: browser's diagnosticReportButton).
  $("diag-report-btn").addEventListener("click", async () => {
    const note = $("diag-report-note");
    const info = state.appInfo || {};
    const bridge = state.portInfo && usbBridgeInfo(state.portInfo.vid, state.portInfo.pid);
    const usb = state.portInfo
      ? [state.portInfo.product, state.portInfo.vid != null
          ? `vid 0x${Number(state.portInfo.vid).toString(16)} pid 0x${Number(state.portInfo.pid || 0).toString(16)}`
          : null, bridge ? `bridge: ${bridge.name}` : null].filter(Boolean).join(" · ")
      : undefined;
    const report = buildDiagnosticReport({
      when: new Date().toISOString(),
      app: "SecuraCV Flasher v" + (info.version || "?") + " (" + (info.build_rev || "source") + ")",
      platform: navigator.platform + " · " + navigator.userAgent,
      catalogVersion: (state.catalog && state.catalog.fw_train) || undefined,
      chip: state.chip || undefined,
      mac: state.mac || undefined,
      flashBytes: state.flashBytes || undefined,
      usb,
      product: state.product ? state.product.id : undefined,
      stage: state.busy ? "mid-flash" : "idle",
      error: state.lastError || undefined,
      logTail: ($("console") && $("console").textContent) ||
        ($("serial-console") && $("serial-console").textContent) || undefined,
    });
    const out = $("diag-report-out");
    try {
      await navigator.clipboard.writeText(report);
      note.textContent = "Copied ✓ — paste it into a GitHub issue or discussion.";
      out.classList.add("hidden");
      out.value = "";
    } catch (_) {
      // No clipboard (denied, or a locked-down webview). The app log is no
      // home for it — logEvent truncates every entry — so show the FULL
      // report to select by hand, the browser flasher's fallback.
      out.value = report;
      out.classList.remove("hidden");
      out.focus();
      out.select();
      note.textContent = "Couldn't reach the clipboard — copy the report from the box below.";
    }
    setTimeout(() => { note.textContent = ""; }, 6000);
  });
  // Per-setting help dots from the embedded catalog's settings_help registry
  // (parity: the browser's teaching layer). Attached where a desktop control
  // matches a registry topic; a catalog without the registry degrades to none.
  for (const [sel, topic] of [
    ["#adv-dev summary", "dev_channel"],
    ["#adv-backup summary", "skip_backup"],
    ['label[for="wifi-ssid"], #provisioning label:has(#wifi-ssid)', "wifi_bake"],
    ["#serial-monitor summary", "serial_monitor"],
    ["#adv-local summary", "local_file"],
  ]) {
    try {
      const host = document.querySelector(sel);
      const dot = host && helpDot(topic);
      if (dot) host.appendChild(dot);
    } catch (_) { /* :has() unavailable or control absent — help stays optional */ }
  }
  $("prov-remember").addEventListener("change", profileNoteRefresh);
  $("hub-remember").addEventListener("change", profileNoteRefresh);
  // A changed network name means a possibly-known password — offer it.
  $("wifi-ssid").addEventListener("change", profileAutofill);
  $("mqtt-host").addEventListener("change", profileAutofill);
  $("mqtt-user").addEventListener("change", profileAutofill);

  document.querySelectorAll("[data-open]").forEach((a) =>
    a.addEventListener("click", (ev) => {
      ev.preventDefault();
      openExternal(a.dataset.open);
    })
  );

  $("flash-btn").addEventListener("click", onFlash);
  $("module-flash-btn").addEventListener("click", onFlashModule);
  $("bench-start").addEventListener("click", onBenchStart);
  $("bench-stop").addEventListener("click", onBenchStop);
  $("bench-tscore").addEventListener("change", () => onBenchSlider("tscore"));
  $("bench-tiou").addEventListener("change", () => onBenchSlider("tiou"));
  $("dev-channel").addEventListener("change", onDevChannelToggle);
  $("local-pick").addEventListener("click", onPickLocalFile);
  $("local-flash-btn").addEventListener("click", onFlashLocalFile);
  $("rescue-backup-btn").addEventListener("click", onRescueBackup);
  $("rescue-restore-btn").addEventListener("click", onRescueRestore);
  $("rescue-erase-btn").addEventListener("click", onRescueErase);
  $("health-check-btn").addEventListener("click", onHealthCheck);
  updateLocalFlashUi();
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
  $("monitor-clear").addEventListener("click", () => {
    $("serial-console").textContent = "";
    clearBootDiagnosis(); // clearing the log clears its verdict too
  });
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
  // Radar tuning suite controls (Sense boards; the sliders wire themselves
  // per-knob in renderSenseTune).
  $("sense-reset").addEventListener("click", () => sendTune("reset"));
  $("sense-stream").addEventListener("change", (e) =>
    sendTune(e.target.value === "0" ? "stream off" : "stream " + e.target.value));
  $("sense-raw").addEventListener("change", (e) =>
    sendTune(e.target.checked ? "raw on" : "raw off"));
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
  $("update-dismiss").addEventListener("click", () => {
    $("update-banner").classList.add("hidden");
    // Remember which version was waved off, so the routine re-checks don't
    // nag — the banner returns only for a NEWER version (or a manual check).
    if (state.update) { prefs.updateDismissed = state.update.version; savePrefs(); }
  });
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

  await listen("serial:log", (ev) => {
    feedSenseTune(ev.payload); // the tuning panel reads [cfg]/[tune] replies
    appendConsole("serial-console", ev.payload);
    // Watch the boot log for the fatal signatures that have a specific fix
    // (parity: the browser's maybeDiagnose on the same stream). Fed the whole
    // console, not the one line, because a backtrace's giveaway can arrive
    // several lines after the line that doomed it.
    const con = $("serial-console");
    if (con) maybeDiagnose(con.textContent);
  });
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
    // The boot receipt carries the device's REAL identity (device_id,
    // pubkey fingerprint, firmware) — enrich the fleet book's row with it.
    try {
      const m = ev.payload && ev.payload.manifest;
      const entry = m && lastBookKey && bookAll()[lastBookKey];
      if (entry) {
        if (m.device_id) entry.deviceId = m.device_id;
        if (m.pubkey_fp) entry.pubkeyFp = m.pubkey_fp;
        if (m.firmware) entry.fw = m.firmware;
        savePrefs();
        fleetBook.renderSoon();
      }
    } catch (_) { /* the book must never break a flash */ }
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
  // …and keep checking: on a routine while the window stays open, plus a
  // catch-up when the user comes back to a window that sat idle. Both are
  // quiet — they only surface anything when an update is actually ready.
  setInterval(() => checkForUpdate(), UPDATE_RECHECK_MS);
  window.addEventListener("focus", () => {
    if (Date.now() - (prefs.lastCheckedAt || 0) > UPDATE_STALE_MS) checkForUpdate();
  });
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

// The real thing: because this is the native app, not a sandboxed browser, we
// can reach the LAN and populate the wall with the REAL fleet. The Rust
// `witness_discover` command does the LAN reach (no CSP; `.local` resolves via
// the OS — Bonjour on macOS, avahi on Linux). ONE controller owns all of it:
//   - opening the Fleet tab starts a continuous scan (and stops on tab leave),
//     so the tab shows your actual Canaries without ever flashing anything;
//   - a successful flash triggers a fast 30 s burst with the new device
//     highlighted, while the board boots and joins Wi-Fi.
// If nothing answers (or an older firmware build), the wall keeps its demo /
// simulated state. Every path is wrapped: discovery can NEVER affect flashing.
function witnessBases() {
  const bases = [];
  const host = $("mqtt-host") && $("mqtt-host").value && $("mqtt-host").value.trim();
  if (host) { bases.push("http://" + host + ":8099"); bases.push("http://" + host); }
  bases.push("http://canary.local:8099", "http://canary.local");
  return bases;
}
const witnessDiscovery = {
  timer: null,        // next scheduled tick
  scanning: false,    // continuous mode (fleet tab open)
  burstUntil: 0,      // fast-poll deadline after a flash
  highlight: null,    // device name to highlight on next find
  inFlight: false,    // a witness_discover call is running (they can take ~8 s)
  found: false,
  post(m) { try { const f = $("witness-frame"); if (f && f.contentWindow) f.contentWindow.postMessage(m, "*"); } catch (_) {} },
  status(msg, live) {
    try {
      const el = $("fleet-scan-status");
      if (el) { el.textContent = msg; el.classList.toggle("live", !!live); }
    } catch (_) {}
  },
  async tick() {
    if (this.inFlight) return this.schedule();
    this.inFlight = true;
    let fleet = null;
    try { fleet = await invoke("witness_discover", { bases: witnessBases() }); }
    catch (_) { /* nothing answering yet, or an older build without the command */ }
    this.inFlight = false;
    if (fleet) {
      const n = (fleet.devices || fleet.canaries || (Array.isArray(fleet) ? fleet : [])).length;
      this.post({ type: "witness:fleet", fleet, highlight: this.highlight });
      this.highlight = null;
      this.found = true;
      this.status("● Live — " + (n || "your") + " Canar" + (n === 1 ? "y" : "ies") + " on your network", true);
    } else if (!this.found) {
      this.status("Scanning your network for Canaries… nothing answering yet — flash one, or make sure a Canary is on this Wi-Fi.");
    }
    this.schedule();
  },
  schedule() {
    if (this.timer) { clearTimeout(this.timer); this.timer = null; }
    const bursting = Date.now() < this.burstUntil;
    if (!this.scanning && !bursting) return;
    this.timer = setTimeout(() => this.tick().catch(() => {}), bursting ? 2500 : 10000);
  },
  start() {          // fleet tab opened — continuous scan
    try {
      this.scanning = true;
      if (!this.found) this.status("Scanning your network for Canaries…");
      this.tick().catch(() => {});
    } catch (_) {}
  },
  stop() {           // fleet tab left — stop unless a post-flash burst is live
    try {
      this.scanning = false;
      if (Date.now() >= this.burstUntil && this.timer) { clearTimeout(this.timer); this.timer = null; }
    } catch (_) {}
  },
  burst(product) {   // just flashed — poll fast while the board boots + joins
    try {
      this.highlight = witnessName(product);
      this.burstUntil = Date.now() + 30000;
      this.tick().catch(() => {});
    } catch (_) { /* discovery is best-effort — never affects flashing */ }
  },
};
function discoverAndPopulate(product) { witnessDiscovery.burst(product); fleetBook.burst(); }

// ── the fleet book controller ────────────────────────────────────────────────
// The management half of the Fleet tab (the Witness Wall below it stays the
// ambient half): every Canary this app flashed, merged live with a real mDNS
// browse of `_securacv._tcp` (fleet_scan, Rust), each row with status and —
// where the flasher seeded the board's API token — one-click identify and
// over-the-air update. The update is the DEVICE's own signed pull pipeline
// (POST /api/ota/install): it downloads, verifies Ed25519 + SHA-256 against
// its pinned key, and A/B-swaps with rollback. This app only rings the bell.
// Every path is wrapped: the book can NEVER affect flashing.
const fleetBook = {
  timer: null,
  scanning: false,
  burstUntil: 0,
  inFlight: false,
  // deviceId → last sighting (+ seenAt). STICKY across scans: ESP32 mDNS
  // responders are lossy on busy Wi-Fi, and one missed 2.5 s browse window
  // must not flicker a live device "offline". A device is online while its
  // last answer is fresh (STALE_MS) and drops off the map only after
  // PRUNE_MS of silence.
  sightings: {},
  STALE_MS: 45 * 1000,
  PRUNE_MS: 3 * 60 * 1000,
  // deviceId → discovered web port. Displays on older firmware announce
  // the port-1 "formality" advert even though their glass page +
  // /api/fleet listen on :80 — one unauthenticated probe upgrades them.
  webPorts: {},
  probedAt: {},      // deviceId → last probe attempt (retry sparsely)
  // book key → the device's answer to a fresh challenge, SHOWN and never
  // gating. See whoami.rs: the proof is relay-able, so it says which key
  // answered, not who holds the socket. Gating a token on it would trade
  // an honest "unverified" for false confidence.
  proof: {},
  proofAt: {},       // book key → last challenge (re-ask sparsely)
  lastScanAt: 0,
  hubUp: null,       // last hub_probe_hub answer (null = never asked)
  opBusy: {},        // book key → an identify/update is running
  opNote: {},        // book key → live status line under the row
  renderQueued: false,

  renderSoon() {
    if (this.renderQueued) return;
    this.renderQueued = true;
    requestAnimationFrame(() => { this.renderQueued = false; try { this.render(); } catch (_) {} });
  },

  async tick() {
    if (this.inFlight) return this.schedule();
    this.inFlight = true;
    try {
      const found = await invoke("fleet_scan", { timeoutMs: 2500 });
      const now = Date.now();
      // MERGE, never replace: a missed browse window keeps the previous
      // sighting; only prolonged silence prunes it.
      for (const s of found || []) {
        if (s && s.deviceId) this.sightings[s.deviceId] = { ...s, seenAt: now };
      }
      for (const [id, s] of Object.entries(this.sightings)) {
        if (now - s.seenAt > this.PRUNE_MS) delete this.sightings[id];
      }
      this.lastScanAt = now;
      // Fold what the network says back into the book: a row's firmware and
      // address follow the DEVICE (it may have updated itself), and last-seen
      // becomes a real timestamp instead of a guess.
      const book = bookAll();
      for (const entry of Object.values(book)) {
        const s = entry.deviceId && this.sightings[entry.deviceId];
        if (!s) continue;
        entry.lastSeenAt = s.seenAt;
        if (s.fw) entry.fw = s.fw;
        if (s.ip) entry.ip = s.ip;
        if (s.host) entry.host = s.host;
        if (s.port) entry.port = s.port;
        if (s.deviceType) entry.deviceType = s.deviceType;
        if (s.role) entry.role = s.role;
      }
      savePrefs();
      // Fielded displays on older firmware announce port 1 (the "formality"
      // advert) while their glass page listens on :80 — probe once per device
      // (sparsely thereafter) so their rows gain a real address + Open button.
      await this.probeWebPorts();
      await this.probeIdentity();
      // The hub row, if the book has one, gets its own reachability probe.
      if (book["id:hub"]) {
        try { this.hubUp = await invoke("hub_probe_hub", { host: HUB_HOST }); }
        catch (_) { this.hubUp = false; }
        if (this.hubUp) { book["id:hub"].lastSeenAt = Date.now(); savePrefs(); }
      }
    } catch (_) { /* no mDNS on this network, or an older backend — the book still renders */ }
    this.inFlight = false;
    this.renderSoon();
    this.schedule();
  },

  // One cheap unauthenticated GET /api/fleet on :80 per port-1 advertiser —
  // the endpoint is public by design, and answering it proves a web surface
  // the advert didn't admit to. Re-tried only every 10 minutes so a truly
  // serverless device (vision/sense) costs one connection attempt, rarely.
  async probeWebPorts() {
    const now = Date.now();
    for (const [id, s] of Object.entries(this.sightings)) {
      if (s.port > 1 || this.webPorts[id]) continue;
      const host = s.ip || s.host;
      if (!host) continue;
      if (this.probedAt[id] && now - this.probedAt[id] < 10 * 60 * 1000) continue;
      this.probedAt[id] = now;
      try {
        const base = "http://" + (host.includes(":") && !host.startsWith("[") ? "[" + host + "]" : host);
        const r = await invoke("fleet_device_call", { base, token: "", action: "fleet" });
        if (r && r.httpStatus >= 200 && r.httpStatus < 300) this.webPorts[id] = 80;
      } catch (_) { /* nothing listening — exactly what the advert said */ }
    }
  },
  // Ask each reachable, known device to sign a fresh nonce. Best-effort and
  // sparse: firmware without the endpoint 404s (reported as "unavailable",
  // never as a failed check), and nothing here blocks or gates anything.
  async probeIdentity() {
    const now = Date.now();
    for (const e of Object.values(bookAll())) {
      if (!e || !e.deviceId) continue;
      const base = this.baseFor(e);
      if (!base) continue;
      if (this.proofAt[e.key] && now - this.proofAt[e.key] < 10 * 60 * 1000) continue;
      this.proofAt[e.key] = now;
      try {
        const r = await invoke("device_whoami", {
          base,
          deviceId: e.deviceId,
          expectedFp: e.pubkeyFp || "",
        });
        if (!r || !r.proof) continue;
        this.proof[e.key] = r;
        // Trust on first sight: record the fingerprint the first time a
        // device proves itself, so a LATER change shows up as wrong-key.
        if (r.proof === "answered" && !e.pubkeyFp && r.seenFp) {
          bookUpsert({ mac: e.mac, deviceId: e.deviceId, pubkeyFp: r.seenFp });
        }
        if (r.proof === "wrong-key") {
          logEvent("warn", `${e.name || e.deviceId}: ${r.detail}`);
        }
      } catch (_) { /* unreachable or refused — absence of proof, not disproof */ }
    }
    this.renderSoon();
  },
  schedule() {
    if (this.timer) { clearTimeout(this.timer); this.timer = null; }
    const bursting = Date.now() < this.burstUntil;
    if (!this.scanning && !bursting) return;
    this.timer = setTimeout(() => this.tick().catch(() => {}), bursting ? 4000 : 10000);
  },
  start() {
    try { this.scanning = true; this.renderSoon(); this.tick().catch(() => {}); } catch (_) {}
  },
  stop() {
    try {
      this.scanning = false;
      if (Date.now() >= this.burstUntil && this.timer) { clearTimeout(this.timer); this.timer = null; }
    } catch (_) {}
  },
  burst() {
    try { this.burstUntil = Date.now() + 45000; this.tick().catch(() => {}); } catch (_) {}
  },

  fresh(entry) {
    const s = entry.deviceId && this.sightings[entry.deviceId];
    return s && Date.now() - s.seenAt <= this.STALE_MS ? s : null;
  },
  online(entry) {
    return !!this.fresh(entry) ||
      (entry.key === "id:hub" && this.hubUp === true);
  },
  // The port a device actually answers HTTP on: its own advert when honest
  // (canary/wap :80, displays on newer firmware), else the probed :80
  // upgrade (fielded displays that still announce the port-1 formality).
  // Vision and sense really have no server — their rows stay
  // presence-and-version only (their OTA rides MQTT).
  webPortFor(entry) {
    const s = entry.deviceId && this.sightings[entry.deviceId];
    if (!s || !(s.ip || s.host)) return 0;
    if (s.port > 1) return s.port;
    return this.webPorts[entry.deviceId] || 0;
  },
  httpCapable(entry) {
    return this.webPortFor(entry) > 0;
  },
  baseFor(entry) {
    const s = entry.deviceId && this.sightings[entry.deviceId];
    const port = this.webPortFor(entry);
    if (!s || !port) return null;
    let host = s.ip || s.host;
    // An IPv6 literal (the IPv6-only-LAN case) must ride in brackets or the
    // URL is malformed — with or without a port. The Rust base guard already
    // unwraps brackets before its private-host check.
    if (host.includes(":") && !host.startsWith("[")) host = "[" + host + "]";
    return "http://" + host + (port !== 80 ? ":" + port : "");
  },
  tokenKey(entry) {
    return secretStore.key("canary", entry.mac || entry.deviceId, "token");
  },

  async identify(entry) {
    const key = entry.key;
    if (this.opBusy[key]) return;
    this.opBusy[key] = true;
    this.note(key, "Asking it to speak up…");
    try {
      const token = await secretStore.get(this.tokenKey(entry));
      const base = this.baseFor(entry);
      if (!token || !base) { this.note(key, token ? "It isn't answering right now." : this.noTokenNote()); return; }
      const r = await invoke("fleet_device_call", { base, token, action: "identify" });
      if (r.httpStatus === 401) this.note(key, this.staleTokenNote());
      else if (r.httpStatus === 404) this.note(key, "This firmware doesn't have an identify signal yet.");
      else this.note(key, "✓ Watch for the blink.");
    } catch (e) {
      this.note(key, "Couldn't reach it: " + e);
    } finally {
      this.opBusy[key] = false;
      this.renderSoon();
    }
  },

  // The one-click OTA: start the board's own check-and-install pipeline,
  // then narrate its status until it reboots into the new build (or says
  // what failed). NOTE the firmware contract: POST /api/ota/install runs
  // securacv_ota_check_and_install() — check INCLUDED — as one async task,
  // and a separate /api/ota/check first would make the install 409
  // (ota_busy), so there is deliberately no pre-check call here. A 409 on
  // install itself means some other run (the daily auto-check) holds the
  // engine — retry briefly instead of giving up.
  async otaUpdate(entry) {
    const key = entry.key;
    if (this.opBusy[key]) return;
    this.opBusy[key] = true;
    try {
      const token = await secretStore.get(this.tokenKey(entry));
      const base = this.baseFor(entry);
      if (!token || !base) { this.note(key, token ? "It isn't answering right now." : this.noTokenNote()); return; }
      this.note(key, "Starting the update — the board downloads and verifies it itself (signed, with automatic rollback)…");
      let started = false;
      let lostSocket = false;
      for (let attempt = 0; attempt < 4 && !started; attempt++) {
        if (attempt) await new Promise((r) => setTimeout(r, 5000));
        let inst = null;
        try { inst = await invoke("fleet_device_call", { base, token, action: "ota-install" }); }
        catch (_) {
          // The install call can outlive its own socket (the board can get
          // to rebooting) — treat transport loss as "watch the status".
          lostSocket = true;
          started = true;
          break;
        }
        if (inst.httpStatus === 401) { this.note(key, this.staleTokenNote()); return; }
        if (inst.httpStatus === 404) { this.note(key, "This build doesn't carry the update engine — reflash over USB instead."); return; }
        if (inst.httpStatus === 409) { this.note(key, "The board's update engine is busy — waiting for it to free up…"); continue; }
        if (inst.httpStatus >= 400) { this.note(key, "The board refused to start the update (HTTP " + inst.httpStatus + "). Try again in a minute."); return; }
        started = true;
      }
      if (!started) { this.note(key, "The board's update engine stayed busy — it may be mid-check on its own. Try again in a minute."); return; }
      const startFw = entry.fw || "";
      const t0 = Date.now();
      let lastText = "";
      let sawActivity = lostSocket;
      while (Date.now() - t0 < 5 * 60 * 1000) {
        await new Promise((r) => setTimeout(r, 3000));
        let st = null;
        try { st = await invoke("fleet_device_call", { base, token, action: "ota-status" }); }
        catch (_) { sawActivity = true; this.note(key, "Rebooting into the new build…"); continue; }
        const b = (st && st.body) || {};
        const stateName = String(b.state || "").toUpperCase();
        if (b.error && b.error !== "NONE" && b.error_text) { this.note(key, "The board reports: " + b.error_text); return; }
        if (stateName && !stateName.includes("IDLE")) {
          sawActivity = true;
          lastText = stateName.includes("DOWNLOAD")
            ? `Downloading — ${b.progress || 0}%`
            : (b.state_text || "Working…");
          this.note(key, lastText);
          continue;
        }
        // IDLE — the task finished (or never had anything to do). Judge by
        // what actually changed, not by which poll we happened to land on.
        if (b.installed_version && b.installed_version !== startFw) {
          entry.fw = b.installed_version;
          savePrefs();
          this.note(key, `✓ Updated — now on v${b.installed_version}.`);
          return;
        }
        if (b.update_available === false) {
          if (b.installed_version) { entry.fw = b.installed_version; savePrefs(); }
          this.note(key, `✓ Already up to date — v${b.installed_version || startFw || "?"}.`);
          return;
        }
        if (sawActivity) {
          this.note(key, "The run finished but the board still reports the old version — its own settings page has the details. Safe to try again.");
          return;
        }
        this.note(key, lastText || "Waiting for the board to start…");
      }
      this.note(key, "Still working — the board finishes on its own; check back in a minute.");
    } catch (e) {
      this.note(key, "Couldn't start the update: " + e);
    } finally {
      this.opBusy[key] = false;
      this.renderSoon();
    }
  },

  noTokenNote() {
    return "No stored key for this board — reflash it once with this app (any version) and the key is minted and kept automatically.";
  },
  staleTokenNote() {
    return "The board refused the stored key — it was probably reflashed elsewhere. One reflash here mints a fresh one.";
  },
  note(key, text) {
    this.opNote[key] = text;
    const el = document.querySelector(`[data-book-note="${CSS.escape(key)}"]`);
    if (el) { el.textContent = text; el.classList.remove("hidden"); }
    else this.renderSoon();
  },

  render() {
    const host = $("fleet-book");
    if (!host) return;
    const book = Object.values(bookAll())
      .sort((a, b) => (b.flashedAt || b.addedAt || 0) - (a.flashedAt || a.addedAt || 0));
    const train = state.catalog && state.catalog.fw_train;
    const known = new Set(book.map((e) => e.deviceId).filter(Boolean));
    const strangers = Object.values(this.sightings).filter(
      (s) => !known.has(s.deviceId) && Date.now() - s.seenAt <= this.STALE_MS);

    const rows = book.map((e) => {
      const online = this.online(e);
      const behind = train && e.fw && fwBehind(e.fw, train);
      const isHub = e.key === "id:hub";
      const name = e.ringName || e.deviceId || e.productLabel || "Canary";
      const sub = [
        e.productLabel || e.product || "",
        e.deviceId && e.ringName ? e.deviceId : "",
        e.fw ? "fw " + e.fw : "",
        e.ip || "",
      ].filter(Boolean).join(" · ");
      const seen = online
        ? `<span class="fb-dot on"></span> on your network`
        : e.lastSeenAt
          ? `<span class="fb-dot off"></span> last seen ${relativeTime(e.lastSeenAt)}`
          : `<span class="fb-dot off"></span> not seen on the network yet`;
      const canTalk = online && this.httpCapable(e) && e.hasToken;
      // Any web-capable device gets an Open button to its own page — the
      // display's glass mirror, the wap's dashboard. No token needed to
      // open a browser at it; the device's own auth takes it from there.
      const canOpen = online && !isHub && this.httpCapable(e);
      const busy = !!this.opBusy[e.key];
      const buttons = isHub
        ? `<button class="btn btn-ghost btn-small" data-book-open="${esc(e.key)}">Open</button>`
        : [
            canOpen ? `<button class="btn btn-ghost btn-small" data-book-visit="${esc(e.key)}">Open</button>` : "",
            canTalk ? `<button class="btn btn-ghost btn-small" data-book-identify="${esc(e.key)}" ${busy ? "disabled" : ""}>Identify</button>` : "",
            canTalk && behind ? `<button class="btn btn-primary btn-small" data-book-update="${esc(e.key)}" ${busy ? "disabled" : ""}>Update over the air</button>` : "",
            `<button class="link-btn" data-book-remove="${esc(e.key)}">Remove</button>`,
          ].filter(Boolean).join("");
      const badge = isHub
        ? (this.hubUp ? `<span class="fb-badge ok">hub · up</span>` : "")
        : behind
          ? `<span class="fb-badge warn">update available → ${esc(train)}</span>`
          : e.fw && train && !fwBehind(e.fw, train)
            ? `<span class="fb-badge ok">up to date</span>`
            : "";
      // The identity proof, SHOWN and never gating. A relay can forward a
      // genuine device's answer, so this says who answered, not who is
      // holding the socket — see whoami.rs and the fleet-book doc. The one
      // loud case is wrong-key: a different identity answering for this
      // device id is worth interrupting for.
      const pr = this.proof[e.key];
      const proofBadge = !pr ? ""
        : pr.proof === "answered"
          ? `<span class="fb-badge proof" title="Signed a fresh challenge with the identity key recorded at flash time. This does not prove the address is genuine — a relay can forward a real device's answer.">identity answered</span>`
          : pr.proof === "wrong-key"
            ? `<span class="fb-badge bad" title="${esc(pr.detail || "")}">different key answered</span>`
            : pr.proof === "bad-signature"
              ? `<span class="fb-badge bad" title="${esc(pr.detail || "")}">bad signature</span>`
              : "";
      const note = this.opNote[e.key]
        ? `<p class="fb-note" data-book-note="${esc(e.key)}">${esc(this.opNote[e.key])}</p>`
        : `<p class="fb-note hidden" data-book-note="${esc(e.key)}"></p>`;
      return `<div class="fb-row">
        <div class="fb-main">
          <div class="fb-name">${esc(name)} ${badge} ${proofBadge}</div>
          <div class="fb-sub">${esc(sub)}</div>
          <div class="fb-seen">${seen}</div>
        </div>
        <div class="fb-actions">${buttons}</div>
        ${note}
      </div>`;
    });

    const strangerRows = strangers.map((s) => `
      <div class="fb-row fb-stranger">
        <div class="fb-main">
          <div class="fb-name">${esc(s.name || s.deviceId)}</div>
          <div class="fb-sub">${esc([s.model || s.deviceType || "Canary", s.fw ? "fw " + s.fw : "", s.ip || ""].filter(Boolean).join(" · "))}</div>
          <div class="fb-seen"><span class="fb-dot on"></span> on your network — not in your book</div>
        </div>
        <div class="fb-actions"><button class="btn btn-ghost btn-small" data-book-adopt="${esc(s.deviceId)}">Add to book</button></div>
      </div>`);

    const empty = !rows.length && !strangerRows.length;
    host.innerHTML = `
      <div class="fb-head">
        <h2>Your fleet book</h2>
        <span class="muted">${book.length ? `${book.length} device${book.length === 1 ? "" : "s"} · ` : ""}${
          this.lastScanAt ? "network checked " + relativeTime(this.lastScanAt) : "checking your network…"}</span>
      </div>
      ${empty ? `<p class="muted fb-empty">Every Canary you flash lands here automatically — with its name,
        firmware, live network status, and (for boards this app flashed) a one-click over-the-air update.
        Flash your first one under “Flash a Canary”.</p>` : rows.join("") + strangerRows.join("")}`;

    host.querySelectorAll("[data-book-identify]").forEach((b) =>
      b.addEventListener("click", () => { const e = bookAll()[b.dataset.bookIdentify]; if (e) this.identify(e); }));
    host.querySelectorAll("[data-book-update]").forEach((b) =>
      b.addEventListener("click", () => { const e = bookAll()[b.dataset.bookUpdate]; if (e) this.otaUpdate(e); }));
    host.querySelectorAll("[data-book-remove]").forEach((b) =>
      b.addEventListener("click", () => bookRemove(b.dataset.bookRemove)));
    host.querySelectorAll("[data-book-open]").forEach((b) =>
      b.addEventListener("click", () => openExternal("http://" + HUB_HOST)));
    host.querySelectorAll("[data-book-visit]").forEach((b) =>
      b.addEventListener("click", () => {
        const e = bookAll()[b.dataset.bookVisit];
        const base = e && this.baseFor(e);
        if (base) openExternal(base);
      }));
    host.querySelectorAll("[data-book-adopt]").forEach((b) =>
      b.addEventListener("click", () => {
        const s = this.sightings[b.dataset.bookAdopt];
        if (!s) return;
        bookUpsert({
          deviceId: s.deviceId, productLabel: s.model || "", fw: s.fw || "",
          ip: s.ip || "", host: s.host || "", port: s.port || 0,
          deviceType: s.deviceType || "", role: s.role || "",
          lastSeenAt: Date.now(),
        });
      }));
  },
};

// Dotted-version compare for "is this board behind the app's firmware train".
// Anything non-numeric compares as NOT behind — never nag on a dev build.
// ── the board passport (parity: flash-core.js) ───────────────────────────────
// What the board IS and what it has lived through, read at connect time by the
// `board_passport` command. These three helpers are pure so they can be
// asserted without a board; the browser's originals are the reference and the
// parity gate keeps them in step.

// Map a firmware image's compiled project_name back to a catalog product, so a
// read-back says "this is a Canary WAP" and not just a version string. Longest
// asset_stem match wins (canary-vision-xiao-s3 must beat canary).
function matchProjectToProduct(catalog, projectName) {
  const pn = String(projectName || "").toLowerCase();
  if (!pn) return null;
  let best = null;
  for (const p of (catalog && catalog.products) || []) {
    const stem = String(p.asset_stem || "").toLowerCase();
    if (!stem) continue;
    if (pn.includes(stem) || pn.includes(stem.replace(/-/g, "_"))) {
      if (!best || stem.length > String(best.asset_stem).length) best = p;
    }
  }
  return best;
}

// Semver-ish compare returning -1/0/1, or null when either side can't be
// parsed. A prerelease sorts BELOW its release (2.4.7-rc1 < 2.4.7).
function compareVersions(a, b) {
  const parse = (v) => {
    const m = /^v?(\d+)(?:\.(\d+))?(?:\.(\d+))?(?:[-+](.+))?$/.exec(String(v || "").trim());
    if (!m) return null;
    return { parts: [+m[1], +(m[2] || 0), +(m[3] || 0)], pre: m[4] || null };
  };
  const pa = parse(a), pb = parse(b);
  if (!pa || !pb) return null;
  for (let i = 0; i < 3; i++) {
    if (pa.parts[i] !== pb.parts[i]) return pa.parts[i] < pb.parts[i] ? -1 : 1;
  }
  if (!!pa.pre !== !!pb.pre) return pa.pre ? -1 : 1;
  if (pa.pre && pb.pre && pa.pre !== pb.pre) return pa.pre < pb.pre ? -1 : 1;
  return 0;
}

// The one-line verdict for a candidate install, given what the board runs now.
// A downgrade is allowed — that's what the safety copy is for — but never
// silent, which is the whole reason this exists.
function installVerdict({ current, currentProduct, product, version }) {
  if (!current || current.unknown) {
    return { kind: "fresh", icon: "✦", label: "First install",
      detail: "Nothing SecuraCV is on this board yet — this is the one-time first setup." };
  }
  if (currentProduct && product && currentProduct.id !== product.id) {
    return { kind: "switch", icon: "⇄", label: `Switch from ${currentProduct.name}`,
      detail: `The board runs ${currentProduct.name} ${current.version || ""}`.replace(/\s+$/, "") +
        ` today. Installing ${product.name} changes what this Canary is — settings from the old role don't carry over.` };
  }
  const cmp = compareVersions(current.version, version);
  if (cmp === null) {
    return { kind: "unknown", icon: "•", label: "Version unknown",
      detail: "The board's current version couldn't be compared — the install itself is unaffected." };
  }
  if (cmp < 0) {
    return { kind: "update", icon: "↑", label: `Update · ${current.version} → ${version}`,
      detail: "A newer build of the same firmware. Your settings region is left alone." };
  }
  if (cmp > 0) {
    return { kind: "downgrade", icon: "↓", label: `Downgrade · ${current.version} → ${version}`,
      detail: "This is OLDER than what the board runs now. That's allowed — useful when a release " +
        "misbehaves — but newer settings may not be understood by older firmware. The automatic " +
        "safety copy is your way back." };
  }
  return { kind: "same", icon: "=", label: `Reinstall · already on ${version}`,
    detail: "The board already runs this exact version. Installing again rewrites it byte-for-byte — " +
      "harmless, and occasionally exactly what a misbehaving board needs." };
}

// The passport's lived-history rows, from the read-only probes.
function passportRows({ ota, witness, coredump } = {}) {
  const rows = [];
  if (ota && typeof ota.updatesSeen === "number" && ota.updatesSeen > 0)
    rows.push({ id: "updates", label: "Updates seen", value: String(ota.updatesSeen), tone: "ok" });
  if (witness && typeof witness.boots === "number")
    rows.push({ id: "boots", label: "Lifetime boots", value: String(witness.boots), tone: "ok" });
  if (witness && typeof witness.seq === "number")
    rows.push({ id: "seq", label: "Witness records", value: String(witness.seq), tone: "ok" });
  if (witness && witness.tamper)
    rows.push({ id: "tamper", label: "Tamper flag", value: "raised — expected if you've opened it", tone: "warn" });
  if (coredump)
    rows.push(coredump.present
      ? { id: "crash", label: "Crash record", value: "one saved crash dump on board", tone: "warn" }
      // Only what the probe actually establishes: the dump region is empty
      // NOW. A wipe, a cleared dump, or a fault that never got to persist one
      // all leave it empty, so "it has never crashed" would be a lifetime
      // claim this read cannot support.
      : { id: "crash", label: "Crash record", value: "no saved crash dump", tone: "ok" });
  return rows;
}

// ── self-healing: turn a boot log into a diagnosis + fix ─────────────────────
// After a flash the board boots and prints to serial. A handful of fatal
// signatures have specific, actionable fixes — surface those instead of raw
// scroll. Ported verbatim from flash-core.js (the two frontends share no code,
// and a diagnostic in only one of them is a diagnostic half the users never
// get). `action` is a machine hint: "clean-install" (reflash with a full
// erase) or "power" — which is NOT a reflash, and the distinction matters:
// telling someone to reinstall firmware when their USB port can't power the
// board sends them around the loop forever. Ordered most-specific first.
const BOOT_SIGNATURES = [
  {
    signature: "brownout",
    test: /brownout detector was triggered|\bBROWNOUT_RST\b|rst:0x[0-9a-f]+ \(BROWNOUT/i,
    means: "The board browned out — it isn't getting enough power to boot.",
    fix: "This isn't the firmware. Use a different USB port (straight into the " +
      "computer, not a hub), or a powered hub, and a shorter good-quality data " +
      "cable. Then reconnect.",
    action: "power",
  },
  {
    signature: "panic",
    test: /guru meditation|backtrace:|panic'?ed|abort\(\) was called|assert failed/i,
    means: "The new firmware crashed as it started up.",
    fix: "A clean install usually clears this — it wipes stale settings a previous " +
      "firmware may have left behind. Reconnect and tick first contact.",
    action: "clean-install",
  },
  {
    signature: "no-app",
    test: /invalid header: 0x|no bootable app partitions|ota_data partition|not found any bootable/i,
    means: "The bootloader couldn't find a complete, valid app to run.",
    fix: "Reconnect and flash again with a full erase (tick first contact) so a " +
      "whole image is laid down from scratch.",
    action: "clean-install",
  },
  {
    signature: "flash-error",
    test: /flash read err|checksum failed|corrupt|e \(\d+\) esp_image/i,
    means: "The board had trouble reading its own flash.",
    fix: "Reseat the cable and do a clean install. If it keeps happening across " +
      "several boards, that one's flash chip may be failing.",
    action: "clean-install",
  },
];

function diagnoseBootLog(text) {
  const t = String(text || "");
  if (t.length < 8) return null;
  for (const s of BOOT_SIGNATURES) {
    if (s.test.test(t)) {
      return { signature: s.signature, means: s.means, fix: s.fix, action: s.action };
    }
  }
  return null;
}

// ── flash-time dials: catalog → clamped typed NVS ints ───────────────────────
// Vision's detection dials and Sense's radar reflexes are NVS-backed — the
// same live numbers Home Assistant retunes later — so seeding them at install
// time means a Canary is already right for its room on first boot. Ports of
// flash-core.js's detectDials/detectValuesToNvs/reflexDials/reflexValuesToNvs;
// pure, so the parity gate can run them against the browser's originals.
//
// The catalog owns every key, bound and default. Clamping here is not
// belt-and-braces: a value outside the firmware's range is a setting the
// device will reject or misread, and silently writing one is worse than
// writing nothing.
function detectDials(catalog, product) {
  if (!product || !catalog) return null;
  const p = (catalog.products || []).find((x) => x.id === product.id);
  return (p && p.detect) || null;
}

function detectValuesToNvs(values, dials) {
  const b = (dials && dials.bounds) || {};
  const clamp = (v, [lo, hi] = [0, 0xffffffff]) => Math.min(Math.max(Math.round(v), lo), hi);
  const out = { u8: {}, u32: {} };
  if (values == null) return out;
  if (Number.isFinite(values.target)) out.u8.det_target = clamp(values.target, b.target || [0, 255]);
  if (Number.isFinite(values.score)) out.u8.det_score = clamp(values.score, b.score || [0, 100]);
  if (Number.isFinite(values.lost_ms)) out.u32.det_lost = clamp(values.lost_ms, b.lost_ms);
  if (Number.isFinite(values.dwell_ms)) out.u32.det_dwell = clamp(values.dwell_ms, b.dwell_ms);
  return out;
}

function reflexDials(catalog, product) {
  if (!product || !catalog) return null;
  const p = (catalog.products || []).find((x) => x.id === product.id);
  const r = p && p.reflexes;
  return (r && r.applies === "runtime" && r.nvs) ? r : null;
}

function reflexValuesToNvs(values, reflexes) {
  const out = { u32: {} };
  if (!values || !reflexes || !Array.isArray(reflexes.knobs)) return out;
  for (const k of reflexes.knobs) {
    const v = values[k.id];
    if (!Number.isFinite(v) || !k.nvs) continue;
    const [lo, hi] = Array.isArray(k.bounds) && k.bounds.length === 2 ? k.bounds : [0, 0xffffffff];
    out.u32[k.nvs] = Math.min(Math.max(Math.round(v), lo), hi);
  }
  return out;
}

// Render the room presets for whichever dial family this product has. The
// browser offers fine-tune sliders too; this ships the presets, which is where
// the value is — the numbers behind them stay retunable from Home Assistant.
//
// "As it ships" is the default and writes NOTHING: the firmware's own defaults
// are already the answer, and seeding them explicitly would turn a value the
// maintainer can change in a future release into one frozen at flash time.
function renderDials(product) {
  const box = $("dials");
  if (!box) return;
  // Keep a choice that still belongs to THIS product. renderProducts() re-runs
  // whenever the manifest or the board passport lands, and each re-run calls
  // onProductChosen() for the selected row — so an unconditional reset here
  // silently discarded a preset picked during those seconds, and the flash
  // then wrote no dials at all. The passport read is exactly when a user has
  // time to pick one, because the Install button is disabled until it lands.
  if (!state.dialChoice || state.dialChoice.productId !== product.id) {
    state.dialChoice = null;
  }
  box.innerHTML = "";
  const detect = detectDials(state.catalog, product);
  const reflexes = reflexDials(state.catalog, product);
  const spec = detect || reflexes;
  if (!spec || !Array.isArray(spec.presets) || !spec.presets.length) {
    box.classList.add("hidden");
    return;
  }
  box.classList.remove("hidden");
  box.append(Object.assign(document.createElement("h3"), {
    textContent: detect ? "Tune it for the room (optional)" : "Radar reflexes (optional)",
  }));
  box.append(Object.assign(document.createElement("p"), {
    className: "fineprint",
    textContent: detect
      ? "Bake a starting point into the chip so it behaves right where it's going. " +
        "These are the same numbers Home Assistant retunes live — nothing is locked in."
      : "A starting point for the radar's reflexes, written with the firmware. " +
        "Home Assistant retunes all of them live afterward.",
  }));
  const grid = document.createElement("div");
  grid.className = "dial-grid";
  for (const preset of spec.presets) {
    const b = document.createElement("button");
    b.type = "button";
    b.className = "dial-card";
    b.dataset.preset = preset.id;
    b.append(
      Object.assign(document.createElement("span"), { className: "dial-icon", textContent: preset.icon || "•" }),
      Object.assign(document.createElement("span"), { className: "dial-title", textContent: preset.title }),
      Object.assign(document.createElement("span"), { className: "dial-blurb", textContent: preset.blurb || "" }),
    );
    b.addEventListener("click", () => {
      grid.querySelectorAll(".dial-card").forEach((c) => c.classList.remove("selected"));
      b.classList.add("selected");
      // The shipped preset means "write nothing" — see above. (Both that and
      // "nothing picked yet" are null, which is fine: they mean the same
      // thing to the flash and both show the shipped card selected.)
      state.dialChoice = preset.id === "ships"
        ? null
        : {
            productId: product.id,
            presetId: preset.id,
            kind: detect ? "detect" : "reflex",
            values: preset.values,
            title: preset.title,
          };
    });
    grid.appendChild(b);
  }
  box.appendChild(grid);
  // Restore the surviving choice, else the shipped default.
  const keep = state.dialChoice && grid.querySelector(`[data-preset="${state.dialChoice.presetId}"]`);
  const show = keep || grid.querySelector('[data-preset="ships"]') || grid.firstElementChild;
  if (show) show.classList.add("selected");
}

// The chosen preset as typed NVS ints for the backend, or null when the user
// left it as it ships.
function dialsForFlash(product) {
  const c = state.dialChoice;
  if (!c) return null;
  const out = c.kind === "detect"
    ? detectValuesToNvs(c.values, detectDials(state.catalog, product))
    : reflexValuesToNvs(c.values, reflexDials(state.catalog, product));
  const u8 = out.u8 || {};
  const u32 = out.u32 || {};
  if (!Object.keys(u8).length && !Object.keys(u32).length) return null;
  return { u8, u32 };
}

// ── self-healing: the flash baud ladder ──────────────────────────────────────
// Transfer speeds to try, fastest first. Flaky cables, unpowered hubs and long
// USB runs sync the ROM fine at 115200 and then choke on the high-speed
// transfer — a rung down just works. The browser walks this during connect
// (esptool-js connects and writes separately); espflash does both in one shot,
// so here the ladder wraps the whole invocation. Same constant, same policy.
const FLASH_BAUDS = [921600, 460800, 230400, 115200];

// The rungs to try this attempt: the catalog's speed first, then every slower
// rung, never anything above the ceiling a previous failure set. Never empty —
// the slowest rung always survives, because "no speed left to try" would turn
// a recoverable cable problem into a dead end.
function baudLadder() {
  const top = (state.catalog && state.catalog.flash_baud) || 921600;
  const ceiling = state.baudCeiling || Infinity;
  const rungs = [top, ...FLASH_BAUDS.filter((b) => b !== top)]
    .filter((b) => b <= ceiling)
    .sort((a, b) => b - a);
  return rungs.length ? rungs : [FLASH_BAUDS[FLASH_BAUDS.length - 1]];
}

// Only these failures are worth a slower retry. A bad image, a busy port or a
// refused permission will fail identically at every speed, and retrying those
// three more times just makes the user wait longer for the same answer.
//
// `unknown` is deliberately NOT here. Every espflash exit used to arrive as
// "exited with code 1" with its real output dropped, so everything classified
// as `unknown` — and including it would have retried a busy port or a denied
// permission down the whole ladder, repeating the download, the verification
// and (on first contact) the full-chip erase each time. The backend now keeps
// espflash's last lines in the error, so genuine transport faults classify as
// device-lost / not-in-download / read-stall on their own merits and this set
// can stay honest.
const BAUD_RETRY_KINDS = new Set(["not-in-download", "read-stall", "device-lost"]);

// Run `fn(baud)` down the ladder until it succeeds or the rungs run out.
async function withBaudLadder(fn, con) {
  const rungs = baudLadder();
  let lastErr;
  for (let i = 0; i < rungs.length; i++) {
    const baud = rungs[i];
    if (i > 0 && con) con.textContent += `→ trying a gentler speed (${baud})…\n`;
    try {
      const out = await fn(baud);
      state.usedBaud = baud;
      state.baudCeiling = null; // it worked — stop handicapping later attempts
      return out;
    } catch (e) {
      lastErr = e;
      const kind = classifyFlashError(e).kind;
      if (!BAUD_RETRY_KINDS.has(kind) || i === rungs.length - 1) throw e;
      // Remember the ceiling even if the user gives up here, so the NEXT
      // attempt doesn't start by repeating the speed that just failed.
      state.baudCeiling = rungs[i + 1];
      logEvent("warn", `Flash failed at ${baud} (${kind}) — dropping to ${rungs[i + 1]}`);
    }
  }
  throw lastErr;
}

// ── the change map ───────────────────────────────────────────────────────────
// What this install actually touches, computed by the backend from the safety
// copy and the verified image (parity: the browser's diffInstall render). The
// headline is the question people ask — do my settings survive? — and it goes
// first, because it is the only line most people need.
function renderChangeMap(map) {
  const box = $("change-map");
  if (!box) return;
  box.innerHTML = "";
  if (!map || !Array.isArray(map.rows) || !map.rows.length) {
    box.classList.add("hidden");
    return;
  }
  box.classList.remove("hidden");
  box.append(Object.assign(document.createElement("h3"), {
    textContent: "What this install changes",
  }));
  if (map.settings) {
    const s = document.createElement("p");
    s.className = "cm-verdict " + (map.settings.kept ? "is-kept" : "is-reset");
    s.textContent = map.settings.text;
    box.appendChild(s);
  }
  if (map.layoutChanged) {
    box.append(Object.assign(document.createElement("p"), {
      className: "cm-layout",
      textContent: "The partition layout itself changes with this image — the board's " +
        "regions are being rearranged, not just rewritten.",
    }));
  }
  const WORDS = {
    untouched: "left alone — the image doesn't reach it",
    identical: "already byte-for-byte identical",
    wiped: "reset to factory-fresh",
    changed: "rewritten",
  };
  const ul = document.createElement("ul");
  ul.className = "cm-rows";
  for (const r of map.rows) {
    const li = document.createElement("li");
    li.className = "cm-row is-" + r.verdict;
    const what = r.verdict === "changed" && r.changedPct
      ? `${WORDS.changed} (${r.changedPct}% of its bytes differ)`
      : WORDS[r.verdict] || r.verdict;
    const detail = r.before && r.after && r.before !== r.after ? ` · ${r.before} → ${r.after}` : "";
    li.append(
      Object.assign(document.createElement("span"), { className: "cm-k", textContent: r.label }),
      Object.assign(document.createElement("span"), { className: "cm-v", textContent: what + detail }),
    );
    ul.appendChild(li);
  }
  box.appendChild(ul);
}

// Say why there is no map, rather than showing nothing. The browser does the
// same ("No change map this time — it needs the safety copy to compare
// against"); an empty space where a panel used to be reads as a bug.
function renderChangeMapUnavailable(reason) {
  const box = $("change-map");
  if (!box || !reason) return;
  box.innerHTML = "";
  box.classList.remove("hidden");
  box.append(Object.assign(document.createElement("p"), {
    className: "cm-none",
    textContent: reason,
  }));
}

function clearBootDiagnosis() {
  state.diagnosedSig = null;
  const box = $("boot-diagnosis");
  if (box) { box.classList.add("hidden"); box.innerHTML = ""; }
}

// Show the diagnosis once per signature — a boot loop reprints the same panic
// forever, and a banner that re-renders on every line is noise, not help.
function maybeDiagnose(text) {
  const d = diagnoseBootLog(text);
  const box = $("boot-diagnosis");
  if (!box || !d || state.diagnosedSig === d.signature) return;
  state.diagnosedSig = d.signature;
  box.innerHTML = "";
  box.className = "boot-diagnosis is-" + d.action;
  box.append(
    Object.assign(document.createElement("div"), {
      className: "bd-head",
      textContent: (d.action === "power" ? "🔌 " : "🩺 ") + d.means,
    }),
    Object.assign(document.createElement("div"), { className: "bd-fix", textContent: d.fix }),
  );
  box.classList.remove("hidden");
  logEvent("warn", "Boot diagnosis: " + d.signature + " — " + d.means);
}

function fwBehind(fw, train) {
  const pa = String(fw).split(".").map((n) => parseInt(n, 10));
  const pb = String(train).split(".").map((n) => parseInt(n, 10));
  if (!pa.length || !pb.length || pa.some(Number.isNaN) || pb.some(Number.isNaN)) return false;
  for (let i = 0; i < 3; i++) {
    const a = pa[i] || 0, b = pb[i] || 0;
    if (a !== b) return a < b;
  }
  return false;
}
// The wall announces witness:ready when its iframe boots; re-sync anything the
// host already knows (a host post fired before boot would have been dropped).
window.addEventListener("message", (e) => {
  const d = e && e.data;
  if (d && d.type === "witness:ready" && (witnessDiscovery.scanning || Date.now() < witnessDiscovery.burstUntil)) {
    witnessDiscovery.tick().catch(() => {});
  }
});

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
  if (view === "fleet") {
    const b = $("badge-fleet");
    if (b) b.classList.add("hidden");
    witnessDiscovery.start();   // scan the LAN while the tab is open
    fleetBook.start();          // …and browse mDNS for the book's live status
  } else {
    witnessDiscovery.stop();    // (a live post-flash burst keeps running)
    fleetBook.stop();
  }

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
// How many consecutive ticks the tracked port may be missing before we call it
// unplugged. A USB-CDC board drops off the bus for a moment when it reboots
// (right after a flash, or on EN/RESET), and the serial monitor rides that out
// — so the watcher must too, or every reboot would read as an unplug.
// Re-enumeration is usually 1–3 s; eight gives a slow OS margin while a real
// unplug still resolves in seconds instead of never.
const UNPLUG_GRACE_POLLS = 8;
let missingPolls = 0;

async function pollPorts() {
  if (state.busy) return; // a flash/rescue owns the port — hands off entirely

  let ports;
  try {
    ports = await invoke("list_ports");
  } catch {
    return; // transient; try again next tick
  }
  const usb = ports.filter((p) => p.kind === "usb");
  syncPortSelect(usb);

  // While the serial monitor owns the port we must not OPEN it (no identify,
  // no port switching) — but presence is still ours to report. Enumerating
  // ports is a read-only OS query that touches nothing, and skipping it here
  // is exactly how the green dot once outlived the board: flash → monitor
  // auto-starts → unplug the Canary, and the app said "Connected" forever.
  if (state.monitoring) {
    trackPresenceWhileMonitoring(usb);
    return;
  }
  missingPolls = 0;

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

// The monitor's native side follows the board by port name first, then by
// VID/PID when the name changes across a reboot (matching_port in
// serial_monitor.rs). Mirror that here so the status bar and the monitor
// always agree on whether the board is still with us.
function trackPresenceWhileMonitoring(usb) {
  const byName = state.port ? usb.find((p) => p.name === state.port) : null;
  const info = state.portInfo;
  const byUsbId = !byName && info
    ? usb.find((p) => Number(p.vid) === Number(info.vid) && Number(p.pid) === Number(info.pid))
    : null;
  const found = byName || byUsbId;
  if (found) {
    if (byUsbId) {
      // Same board, new name after re-enumeration — follow it, like the monitor does.
      state.port = found.name;
    }
    state.portInfo = found;
    missingPolls = 0;
    setConn("connected", `Connected · ${state.chip || "Canary"} on ${state.port}`);
    return;
  }
  missingPolls += 1;
  if (missingPolls < UNPLUG_GRACE_POLLS) {
    // A rebooting board re-enumerates within a second or two; don't call a
    // blink an unplug. The monitor is waiting it out on its own thread.
    setConn("reading", "Board went away — waiting for it to come back…");
    return;
  }
  missingPolls = 0;
  onDisconnect(); // really gone: stop the monitor, back to the scanning state
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
    const info = await invoke("detect_chip", { port });
    if (port !== state.port) return; // unplugged/switched while we were reading
    state.chip = info.chip;
    state.flashBytes = info.flash_bytes; // may be null if board-info didn't report it
    state.mac = info.mac; // may be null
    // The read-only MAC intake check, straight off the connect read.
    state.macCheck = info.mac_check || null;
    setConn("connected", `Connected · ${info.chip} on ${port}`);
    $("recheck").classList.remove("hidden");

    refreshManifest();
    renderProducts();
    enableCard("step-pick");
    updateLocalFlashUi();
    // Read the board's passport in the background. Deliberately NOT awaited:
    // the product list is already usable, and a board whose passport won't
    // read must still be flashable — that's often exactly the board that
    // needs it. renderProducts() runs again when the answer lands.
    readPassport(port);
  } catch (e) {
    if (port !== state.port) return;
    state.failedPort = port;
    // detect_chip's error names the real cause when it knows one — on Linux,
    // "permission denied" and "port held by ModemManager" have a one-line OS
    // fix, and coaching the BOOT/RESET ritual for those sends the user to the
    // wrong fix forever. Show the backend's first line for an OS-level
    // failure (and hide the download-mode coaching); blame download mode only
    // when nothing else was named. (The browser flasher makes the same call
    // in classifyFlashError — two frontends, same diagnostic.)
    const firstLine = String(e).split("\n")[0].trim();
    const osLevel = /Linux blocked opening|holding the board's serial port/i.test(firstLine);
    // A known USB-serial bridge that won't talk is usually a missing OS
    // driver — name the chip and the driver instead of coaching BOOT/RESET
    // at a port the OS can't even open properly (parity: usbBridgeInfo).
    const bridge = state.portInfo && usbBridgeInfo(state.portInfo.vid, state.portInfo.pid);
    const bridgeNote = !osLevel && bridge ? " " + bridge.note : "";
    setConn("failed", osLevel
      ? `Found ${port} — ${firstLine}`
      : `Found ${port} — couldn't read the chip. Put it in download mode.${bridgeNote}`);
    $("download-mode").classList.toggle("hidden", osLevel);
    $("recheck").classList.remove("hidden");
  } finally {
    state.detecting = false;
  }
}

// ── read the passport, then say who this board is ────────────────────────────
// Every field is best-effort. The one thing this must never do is report an
// unread board as a blank one: "we couldn't look" and "there is nothing there"
// are different answers, and only the backend can tell them apart (it returns
// blank:true solely when the read SUCCEEDED and found no partition table).
async function readPassport(port) {
  state.passport = null;
  state.resident = null;
  // The picker is usable while this runs, so the flash button waits on it:
  // starting an install mid-read would show NO verdict at all, and a silent
  // downgrade is exactly what this feature exists to prevent. The wait is
  // bounded — see the settle timer below — because "can't read the board"
  // must never become "can't flash the board".
  state.passportPending = true;
  const settle = setTimeout(() => {
    if (state.passportPending && port === state.port) {
      state.passportPending = false;
      state.resident = state.resident || { unknown: true };
      renderPassport();
      renderProducts();
    }
  }, 25000);
  try {
    const p = await invoke("board_passport", {
      port,
      baud: (state.catalog && state.catalog.flash_baud) || 921600,
    });
    if (port !== state.port) return; // unplugged/switched while reading
    state.passport = p;
    if (p.blank) {
      state.resident = { unknown: false, blank: true };
    } else if (p.resident) {
      state.resident = {
        version: p.resident.version,
        projectName: p.resident.projectName,
        built: p.resident.built,
        slot: p.resident.slot,
        product: matchProjectToProduct(state.catalog, p.resident.projectName),
      };
    } else {
      state.resident = { unknown: true };
    }
  } catch (_) {
    if (port !== state.port) return;
    state.resident = { unknown: true }; // read failed — NOT a blank board
  } finally {
    clearTimeout(settle);
    // Settles on every path — but only for the board this call belongs to.
    // Swap boards mid-read and a NEWER readPassport is already in flight with
    // the flag raised; clearing it here unconditionally would open the gate
    // while that read is still running, which is the exact hole this closes.
    if (port === state.port) state.passportPending = false;
  }
  renderPassport();
  renderProducts(); // verdict rows depend on what's resident
}

function renderPassport() {
  const box = $("passport");
  if (!box) return;
  const p = state.passport;
  const r = state.resident;
  box.innerHTML = "";
  if (!r) { box.classList.add("hidden"); return; }
  box.classList.remove("hidden");

  const head = document.createElement("div");
  head.className = "pp-head";
  if (r.blank) {
    head.textContent = "A blank board — nothing installed yet.";
  } else if (r.unknown) {
    head.textContent = "Couldn't read what's installed (the install itself is unaffected).";
  } else {
    const who = r.product ? r.product.name : r.projectName || "unknown firmware";
    head.textContent = `Running ${who}${r.version ? " " + r.version : ""}` +
      (r.slot ? ` from ${r.slot}` : "");
  }
  box.appendChild(head);

  // The same board, met twice: the fleet book already knows this MAC. Says so
  // before the flash, so a reflash is a choice rather than a surprise.
  const known = state.mac && bookAll()[String(state.mac).toLowerCase()];
  if (known) {
    const seen = document.createElement("div");
    seen.className = "pp-known";
    const when = known.flashedAt ? new Date(known.flashedAt).toLocaleDateString() : null;
    seen.textContent = `You've flashed this exact board before` +
      (known.name ? ` — it's "${known.name}" in your fleet` : "") +
      (when ? `, last written ${when}.` : ".");
    box.appendChild(seen);
  }

  if (r.built) {
    const b = document.createElement("div");
    b.className = "pp-built muted";
    b.textContent = "Built " + r.built;
    box.appendChild(b);
  }
  // The MAC intake finding, but only when it is worth saying: a normal
  // factory address is the expected case and needs no line of its own.
  const mc = state.macCheck;
  if (mc && mc.level && mc.level !== "clear" && mc.level !== "unknown") {
    const w = document.createElement("div");
    w.className = "pp-macwarn is-" + mc.level;
    w.textContent = (mc.level === "stop" ? "✗ " : "⚠ ") + mc.label +
      (mc.detail ? " " + mc.detail : "");
    box.appendChild(w);
  }

  const rows = passportRows(p || {});
  if (rows.length) {
    const ul = document.createElement("ul");
    ul.className = "pp-rows";
    for (const row of rows) {
      const li = document.createElement("li");
      li.className = "pp-row pp-" + row.tone;
      li.append(Object.assign(document.createElement("span"), { className: "pp-k", textContent: row.label }),
                Object.assign(document.createElement("span"), { className: "pp-v", textContent: row.value }));
      ul.appendChild(li);
    }
    box.appendChild(ul);
  }
}

function onDisconnect() {
  stopMonitor();
  stopBenchQuiet(); // the module (and its port) just went away
  state.port = null;
  state.portInfo = null;
  state.portKind = null;
  state.passport = null;
  state.resident = null;
  state.passportPending = false;
  // The lowered speed belonged to THAT board and THAT cable. Carrying it to
  // the next board would quietly hold a healthy one at 115200 for the rest of
  // the session — the ladder is a per-board remedy, not a session-wide verdict.
  state.baudCeiling = null;
  state.usedBaud = null;
  // A preset was chosen for the board that just left. The next one starts
  // from its firmware's own defaults, not the last board's room.
  state.dialChoice = null;
  state.chip = null;
  state.flashBytes = null;
  state.mac = null;
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
  // Re-arm the first-contact erase for every board that attaches. The checkbox
  // starts ticked in the markup, but that only happens once per app launch —
  // so unticking it to reflash a known Canary would silently carry over to the
  // next board plugged in, which is precisely the marketplace board that needs
  // the wipe. The safe default has to be restored per board, not per session.
  if ($("first-contact")) $("first-contact").checked = true;
  $("module-flow").classList.add("hidden");
  $("serial-monitor").classList.add("hidden");
  renderReceipts();
  maybeHatch();
  updateLocalFlashUi();
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

// The manifest this session flashes from: the catalog's pinned stable
// release, or — only while the Advanced toggle is on — the fixed dev constant.
function activeManifestUrl() {
  return state.devChannel ? DEV_FLASH_MANIFEST_URL : state.catalog.manifest_url;
}

// Stale-fetch guard: a stable and a dev fetch can be in flight at once (the
// toggle mid-fetch), and the network decides which lands last — only the
// NEWEST refresh may write state, or the products shown belong to the other
// channel.
let manifestGeneration = 0;
function refreshManifest() {
  const generation = ++manifestGeneration;
  state.manifest = null;
  state.manifestError = null;
  invoke("fetch_manifest", { manifestUrl: activeManifestUrl() })
    .then((m) => {
      if (generation !== manifestGeneration) return; // superseded by a newer refresh
      state.manifest = m;
      state.manifestError = null;
      renderProducts();
    })
    // Keep WHY. Swallowing this is how every product came to read "no
    // published release yet" — indistinguishable from "nothing has ever
    // shipped" — when the real answer was "the release this build is pinned
    // to was never cut". renderProducts() turns it into that sentence.
    .catch((e) => {
      if (generation !== manifestGeneration) return; // superseded by a newer refresh
      state.manifestError = String((e && e.message) || e || "unreachable");
      renderProducts();
    });
}

// ── step 2: pick a firmware image (chip-guarded) ────────────────────────────
/* The picker slot — the same rule the in-browser flasher follows, because the
 * two frontends share no UI code and drift is the standing hazard here
 * (AGENTS.md rule 7). The board's own isometric figure when the embedded
 * catalog carries one, a neutral placeholder when it doesn't; the SAME size
 * either way, so the list never reflows as figures land for more of the fleet.
 *
 * The placeholder deliberately resembles no product. Drawing a plausible
 * generic board would be worse than drawing nothing — somebody matching the
 * hardware in their hand against a picture of different hardware is the one
 * failure this system exists to prevent. See docs/design/FLEET_FIGURES.md.
 *
 * The SVG is inlined in flash.json, which build.rs bakes into the binary, so
 * this draws with no file on disk and no network.
 */
function figureSlot(p) {
  const f = p.figure;
  if (f && f.svg) {
    const t = f.shared
      ? `${f.title} — this board is also built as another product`
      : f.title;
    return `<span class="p-fig" title="${esc(t)}">${f.svg}</span>`;
  }
  return (
    '<span class="p-fig placeholder" title="No drawing for this board yet">' +
    '<svg viewBox="0 0 64 64" aria-hidden="true">' +
    '<rect x="12" y="18" width="40" height="28" rx="3"/>' +
    '<rect class="c" x="20" y="26" width="24" height="12" rx="1.5"/></svg>' +
    `<i>${esc((p.chip || "").replace("ESP32-", ""))}</i></span>`
  );
}

// The install verdict as a row badge. Silent until both halves are known —
// what the board runs (passport) and what the release offers (manifest) —
// because a guess here is worse than a blank: "Update" on a board we never
// read would be an invented promise.
function verdictMarkup(product, version) {
  if (!state.resident || !version) return "";
  const r = state.resident;
  // A board we COULDN'T read is not a board with nothing on it. installVerdict
  // maps both `null` and `{unknown:true}` to "First install", which is right
  // for the browser — its probe cannot tell a blank chip from an unreadable
  // one, so both arrive as `{unknown:true}`. This backend can (it returns
  // blank:true only when the read SUCCEEDED and found no table), so saying
  // "nothing SecuraCV is on this board yet" here would be an invented claim
  // that contradicts the passport card sitting directly above it — and would
  // hide a downgrade. Keep installVerdict byte-identical to the browser's and
  // make the distinction at the call site, where the better information is.
  if (r.unknown) {
    return `<span class="p-verdict is-unknown">` +
      `<span class="p-verdict-label">• Can't tell yet</span>` +
      `<span class="p-verdict-detail">This board's current firmware couldn't be read, so ` +
      `there's no telling whether this is an update, a reinstall, or a step backward. ` +
      `The install itself is unaffected, and the automatic safety copy still runs.</span></span>`;
  }
  const v = installVerdict({
    current: r.blank ? null : r,
    currentProduct: r.product || null,
    product,
    version,
  });
  return `<span class="p-verdict is-${v.kind}">` +
    `<span class="p-verdict-label">${esc(v.icon)} ${esc(v.label)}</span>` +
    `<span class="p-verdict-detail">${esc(v.detail)}</span></span>`;
}

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
  // The catalog pins an EXACT firmware release (canary-local/tools/gen_flash.py
  // explains why /latest/ is unsafe in a shared release namespace). If that
  // release hasn't been cut, the fetch 404s and every row below reads "no
  // published release yet" — which reads like the project has never shipped.
  // Name the tag instead, so the answer is "that release is missing", not "your
  // board, your network, or this app is broken".
  if (state.manifestError && !state.manifest) {
    const pinned = releaseTagFromManifestUrl(activeManifestUrl());
    // A pinned URL that didn't load does NOT prove the release is missing: the
    // machine may be offline, or DNS/TLS may have failed, in which case telling
    // someone to "wait for the release to be cut" sends them to look at the
    // wrong thing. The Rust command distinguishes the two (see fetch_manifest:
    // an HTTP status means the server answered), so only claim the release is
    // uncut when we actually got a status back.
    const status = /\bHTTP (\d{3})\b/.exec(state.manifestError);
    if (state.devChannel && status) {
      // The dev pointer isn't a pinned fw-v* tag, so name it explicitly the
      // same way the stable path names its pinned release — "that release
      // doesn't exist yet", never "something is broken".
      $("pick-sub").textContent =
        `Images for your ${state.chip} — the dev channel points at the rolling ` +
        `fw-dev-latest prerelease, and no dev release has been cut yet ` +
        `(HTTP ${status[1]}). Turn the dev channel off under Advanced for the ` +
        `stable release, or install a local file.`;
    } else if (status && pinned) {
      $("pick-sub").textContent =
        `Images for your ${state.chip} — but this build is pinned to firmware ` +
        `release ${pinned}, and that release has no published images ` +
        `(HTTP ${status[1]}). Install a local file under Advanced until it is cut.`;
    } else if (status) {
      $("pick-sub").textContent =
        `Images for your ${state.chip} — the firmware manifest returned HTTP ` +
        `${status[1]}. Install a local file under Advanced.`;
    } else {
      // "didn't load" rather than "couldn't reach": with no HTTP status this is
      // either a transport failure OR a manifest that arrived malformed, and the
      // second one did reach us.
      $("pick-sub").textContent =
        `Images for your ${state.chip} — the firmware manifest didn't load ` +
        `(${state.manifestError}). ` +
        (pinned ? `This build is pinned to firmware release ${pinned}; if that ` +
                  `release exists, the images appear once you're back online. ` : "") +
        `Install a local file under Advanced.`;
    }
  } else {
    $("pick-sub").textContent = `Images built for your ${state.chip}:`;
  }

  const selectedId = state.product ? state.product.id : null;

  matches.forEach((p) => {
    const ver =
      state.manifest &&
      state.manifest.products &&
      state.manifest.products[p.id] &&
      state.manifest.products[p.id].version;

    // The size gate: right chip, not enough of it. The row stays visible so
    // the product line is browsable, but it can't be picked — same refusal,
    // same words, as the browser flasher's refreshManifestState.
    const fit = flashFitVerdict(p, state.flashBytes);
    const isSelected = fit.fits && p.id === selectedId;
    const row = document.createElement("label");
    row.className = "product" + (isSelected ? " selected" : "") + (fit.fits ? "" : " unfit");
    row.innerHTML = `
      <input type="radio" name="product" value="${p.id}"${isSelected ? " checked" : ""}${fit.fits ? "" : " disabled"}>
      ${figureSlot(p)}
      <span>
        <span class="p-name">${esc(p.name)}<span class="chip-badge">${esc(p.chip)}</span></span>
        <span class="p-tag">${esc(p.tagline || "")}</span>
        <span class="p-meta">${
          fit.fits
            ? ver ? "release " + esc(ver) : "no published release yet"
            : `needs a ${fit.needMb} MB board — this chip has ${fit.haveMb} MB`
        }</span>${
          p.figure && p.figure.shared
            ? '<span class="p-note">This board is also sold as another product — check the name, not just the picture.</span>'
            : ""
        }${
          // Support tier from flash.json (derived from the board registry).
          // Same words as the browser flasher on purpose — half the users are
          // here, and a diagnostic that lives in only one of the two frontends
          // is a diagnostic half the users never get.
          p.tier
            ? `<span class="p-tier${p.tier.first ? " is-first" : ""}">` +
              `<span class="p-tier-label">${esc(p.tier.label)}</span>` +
              `<span>${esc(p.tier.line)}</span></span>`
            : ""
        }${
          // What installing THIS image would do to THIS board — update,
          // downgrade, reinstall, or a change of role. Only once the passport
          // has been read and the release version is known; an unread board
          // says nothing rather than guessing.
          verdictMarkup(p, ver)
        }
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

// Boards whose flashing port isn't the one you can see. Catalog-driven
// (products[].access, from gen_flash.py's BOARD_ACCESS — the same block the
// in-browser flasher renders, so the two frontends can't drift), and shown in
// the CONNECT step because it's the one instruction that has to arrive before
// the cable does: on the 60 GHz radar kit the port that flashes is the XIAO's,
// inside the case, while the reachable one is power. Plug into the reachable
// port and the port list stays empty — which reads as "my board is dead"
// instead of "wrong port". Families without an access block get no card.
function renderAccessNotes() {
  const host = $("access-notes");
  if (!host) return;
  host.textContent = "";
  const seen = new Set();
  for (const p of (state.catalog && state.catalog.products) || []) {
    const a = p.access;
    // Dedup on the access entry, not the family — same reason as the browser
    // flasher's accessCards(): the ESP32-CAM and the WROOM DevKit share the
    // `canary` family and need opposite instructions.
    const key = a && (a.key || p.family);
    if (!a || seen.has(key)) continue;
    seen.add(key);
    const d = document.createElement("details");
    d.className = "hint";
    d.dataset.family = p.family;
    d.innerHTML = `
      <summary>${esc(p.name.replace(/ · .*$/, ""))} — ${esc(a.headline)}</summary>
      <p>Flash into <strong>${esc(a.flash_port)}</strong>.</p>
      <ol>${a.steps.map((s) => `<li>${esc(s)}</li>`).join("")}</ol>
      <p class="muted"><strong>That other port — ${esc(a.other_port)}:</strong>
        ${esc(a.other_effect)}</p>
      ${a.enclosure_note ? `<p class="muted">${esc(a.enclosure_note)}</p>` : ""}
      ${a.reassembly ? `<p class="fineprint">${esc(a.reassembly)}</p>` : ""}`;
    host.append(d);
  }
}

function onProductChosen(p, ver) {
  enableCard("step-flash");
  state.portKind = "esp32";
  $("module-flow").classList.add("hidden");
  $("host-flash-controls").classList.remove("hidden");
  $("serial-monitor").classList.remove("hidden");
  // Wi-Fi preload is standard on EVERY board (each firmware reads the same
  // NVS namespace, catalog wifi_nvs says in which encoding) — so the network
  // fields always show. Identity + broker stay usb-secrets-only: the other
  // firmwares configure that part themselves (AP portal / on-glass).
  const usbSecrets = p.provisioning === "usb-secrets";
  // See readProvisioning(): reading a broker out of NVS is a firmware
  // capability, separate from how identity is provisioned. Displays are
  // on-glass AND read a broker, and used to fall through the gap.
  const broker = p.broker_nvs === true;
  $("provisioning").classList.remove("hidden");
  document.querySelectorAll("#provisioning .usb-only").forEach((n) => {
    // The name row also shows for broker-capable boards (displays): their
    // firmware derives its MQTT topics from dev_id, and without a seeded one
    // the flavor's SHARED compiled id (canary_dash_001) goes sticky on first
    // boot — two of the same display would collide on the same topics. The
    // glass setup only ever asks for Wi-Fi, so this is the one place a
    // display can be named. Optional there; clearing it writes nothing.
    const isIdentity = !!n.querySelector("#device-id");
    n.classList.toggle("hidden", !(usbSecrets || (broker && isIdentity)));
    const input = n.querySelector("input");
    if (input) input.required = usbSecrets && input.id === "device-id";
  });
  document.querySelectorAll("#provisioning .broker-only").forEach((n) => {
    n.classList.toggle("hidden", !broker);
    const input = n.querySelector("input");
    // Required only where the board has no other way to be told: a USB-secrets
    // board configures its broker here or nowhere. A display can also be given
    // one on the glass, so leaving these blank stays a legitimate choice.
    if (input) input.required = usbSecrets && ["mqtt-host", "mqtt-port"].includes(input.id);
  });
  $("wifi-ssid").required = usbSecrets;
  $("provision-note").textContent = usbSecrets
    ? "The verified release image is generic. These values are written directly into this board's settings partition and never logged."
    : broker
    ? "Optional, and worth doing: bake in your network AND your hub's broker, and this board comes up already talking to Home Assistant. Skip either and its on-screen setup still works. Written straight into this board's settings partition, never logged."
    : "Optional: bake your network in and the Canary joins it on first boot — skip it and the board's own setup path (phone portal or on-screen) still works.";
  profileNoteRefresh();
  // Offer the network this computer is on — the common case — so joining is
  // one Tab and a password away (which Keychain/password managers can fill).
  if (!$("wifi-ssid").value) {
    invoke("current_ssid").then((ssid) => {
      if (ssid && !$("wifi-ssid").value) $("wifi-ssid").value = ssid;
    }).catch(() => {});
  }
  if ((usbSecrets || broker) && !$("device-id").value) {
    const family = p.id.includes("display")
      ? "canary_display"
      : p.id.includes("vision")
        ? "canary_vision"
        : p.id.includes("sense")
          ? "canary_sense"
          : "canary";
    const suffix = Array.from(crypto.getRandomValues(new Uint8Array(2)))
      .map((b) => b.toString(16).padStart(2, "0")).join("");
    $("device-id").value = `${family}_${suffix}`;
  }
  // The broker default is the broker CAPABILITY's default, not usb-secrets':
  // a display gets homeassistant.local suggested too (the browser flasher
  // suggests the same), and clearing it means "skip — nothing is written".
  // No hub yet? Leave it: a hub built later answers at exactly this name, so
  // Canaries flashed today find it by themselves the day it exists.
  if (usbSecrets || broker) $("mqtt-host").value ||= "homeassistant.local";
  profileAutofill();
  renderDials(p); // room presets, for the products whose firmware reads them
  const btn = $("flash-btn");
  // Wait for the passport before arming the button. Without this the picker
  // is live during the connect-time read, so a quick hand could start an
  // install while state.resident is still null — no verdict rendered, and a
  // downgrade or a role switch goes through in silence. Bounded by
  // readPassport's settle timer, so an unreadable board still flashes.
  btn.disabled = !ver || !!state.passportPending;
  $("flash-target").textContent = !ver
    ? "No published release for this one yet."
    : state.passportPending
      ? `Reading the board first — ${p.name} → ${state.port}`
      : `${p.name} → ${state.port}`;
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
  setStatus("flash-result", "Use the module button below. The host firmware receipt is kept while you move the cable." +
    (state.vision.hostFlash ? "" :
      " (The demo takes two boards — the XIAO host gets the Canary Vision firmware through its own USB-C port, before or after this one.)"));
  benchReset();
  renderReceipts(true);
}

// ── step 3: flash ───────────────────────────────────────────────────────────
async function onFlash() {
  // Snapshot the chosen product and channel for this write: a dev-channel
  // toggle clears state.product, and the receipt logic below must keep
  // judging the product that was actually written, not whatever the UI
  // holds by the time the flash finishes.
  const product = state.product;
  // The size gate, enforced at the button as well as on the cards: the pick
  // may predate chip detection, so an unfit product can still be sitting in
  // state.product even though its row is now disabled.
  {
    const fit = flashFitVerdict(product, state.flashBytes);
    if (!fit.fits) {
      setStatus("flash-result", `✗ ${fit.why}`, "err");
      return;
    }
  }
  const manifestUrl = activeManifestUrl();
  // false = the user TYPED provisioning values that don't validate — abort so
  // the install can't succeed while silently dropping the Wi-Fi they asked
  // for. null = intentionally skipped (wifi-only board, empty SSID): flash on.
  const provisioning = readProvisioning(product);
  if (provisioning === false) return;
  if (product.provisioning === "usb-secrets" && !provisioning) return;
  persistProv();
  // Blob-scheme firmwares (canary/wap) load their local-API bearer token from
  // NVS before deriving one — so mint it HERE, seed it with the Wi-Fi, and the
  // fleet book holds this board's credential from birth (no pairing dance).
  const apiToken =
    provisioning && provisioning.wifiSsid && product.wifi_nvs === "blob" ? mintApiToken() : "";
  if (provisioning) provisioning.apiToken = apiToken;
  const btn = $("flash-btn");
  const con = $("console");
  con.textContent = "";
  con.classList.remove("hidden");
  btn.disabled = true;
  btn.textContent = "Flashing…";
  $("dev-channel").disabled = true;
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
  const unlistenRescue = await listen("rescue:log", (ev) => {
    con.textContent += ev.payload + "\n";
    con.scrollTop = con.scrollHeight;
  });
  // The change map arrives once, after the image is verified and before a
  // byte is written — the moment both sides of the comparison exist.
  const unlistenMap = await listen("flash:changemap", (ev) => renderChangeMap(ev.payload));

  // Safety copy FIRST — the undo the desktop one-shot flow never had (the
  // browser has done this unasked for ages; parity at last). Once per board
  // per session, skippable under Advanced, and a failed copy is a warning
  // rather than a blocker: the browser makes the same call, because a board
  // that can't be read is often exactly the one that needs rescuing.
  let backupPath = null;
  state.backupsTaken = state.backupsTaken || {};
  // "Once per board per session" dedupes strictly by MAC — the browser's rule
  // (haveBackupForThisBoard). A board that didn't report one gets a fresh
  // copy every time: keying on the USB port instead would let the second
  // board through a port silently inherit the first board's "already copied".
  const backupKey = state.mac ? String(state.mac).toLowerCase() : null;
  const skipBackup = !!($("skip-backup") && $("skip-backup").checked);
  // Why there may be no change map. The map needs the board's CURRENT bytes,
  // and the only copy we take is the pristine one from before the first write
  // — reusing it on a reflash would draw the map against a "before" that
  // stopped being true one flash ago, which is worse than drawing nothing.
  // Whatever the reason, it gets said out loud: a panel that silently
  // vanishes reads as a broken feature.
  let noMapReason = null;
  if (skipBackup) {
    noMapReason = "No change map this time — the safety copy is switched off under Advanced, " +
      "and the map is drawn from it.";
  } else if (!state.flashBytes) {
    noMapReason = "No change map this time — the board didn't report its flash size, so there " +
      "was nothing to read a copy from.";
  } else if (backupKey && state.backupsTaken[backupKey]) {
    noMapReason = "No change map this time — the safety copy from earlier in this session is " +
      "kept as your undo, and it describes the board as it was before that first install, " +
      "not as it is now.";
  }
  if (!skipBackup && state.flashBytes && !(backupKey && state.backupsTaken[backupKey])) {
    try {
      backupPath = await invoke("auto_backup_path", { mac: state.mac || "" });
      con.textContent += "→ safety copy first — reading the whole chip before anything is written…\n";
      await invoke("backup_flash", {
        port: state.port,
        outPath: backupPath,
        flashSize: state.flashBytes,
        baud: state.catalog.flash_baud || 921600,
      });
      if (backupKey) state.backupsTaken[backupKey] = backupPath;
      logEvent("ok", "Safety copy saved: " + backupPath);
    } catch (e) {
      backupPath = null;
      noMapReason = "No change map this time — the safety copy it's drawn from couldn't be " +
        "read off this board.";
      con.textContent += "⚠ Couldn't take the safety copy (" + e + ") — continuing. " +
        "Advanced → Rescue can still try a manual backup.\n";
    }
  }
  if (!backupPath && noMapReason) renderChangeMapUnavailable(noMapReason);

  try {
    const receipt = await withBaudLadder((baud) => invoke("flash", {
      port: state.port,
      productId: product.id,
      manifestUrl,
      baud,
      detectedChip: state.chip,
      provisioning,
      // First contact with a board we've never written: wipe the whole chip
      // rather than only the regions we're about to write, so nothing a
      // previous owner left in an untouched partition rides through. The
      // browser flasher decides this by reading the board; espflash can't
      // report what's resident, so here it's the user's answer on step 1.
      eraseFirst: !!($("first-contact") && $("first-contact").checked),
      // Absent when the copy was skipped or failed: no map, honestly.
      backupPath: backupPath || "",
    }), con);
    state.vision.hostFlash = receipt;
    state.vision.hostBoot = null;
    // The flash succeeded — that's the consent moment the "Remember" box
    // announced: save the profile, record the board in the fleet book, and
    // put its API credential in the secret drawer.
    profileSaveFromFlash(provisioning);
    try {
      const entry = bookUpsert({
        mac: (state.mac || "").toLowerCase(),
        deviceId: provisioning ? provisioning.deviceId : "",
        product: product.id,
        productLabel: product.name,
        chip: state.chip || "",
        fw: receipt.version,
        flashedAt: Date.now(),
        hasToken: !!apiToken,
        wifiBaked: !!(provisioning && provisioning.wifiSsid),
        backupPath: backupPath || "",
      });
      lastBookKey = entry ? entry.key : null;
      state.lastProvisionedId = provisioning ? provisioning.deviceId : "";
      if (apiToken && entry) {
        // If the drawer refuses (Keychain locked), the book must not claim a
        // key it doesn't hold — the row falls back to the no-token note.
        secretStore
          .set(secretStore.key("canary", entry.mac || entry.deviceId, "token"), apiToken)
          .then((ok) => {
            if (!ok) { entry.hasToken = false; savePrefs(); fleetBook.renderSoon(); }
          })
          .catch(() => {});
      }
    } catch (_) { /* the book must never break a flash */ }
    clearSecretFields();
    renderReceipts();
    renderTokenOnce(apiToken);    // the credential, shown once (parity: browser done card)
    renderNurseryStrip();         // this session's hatchlings, where the next board plugs in
    announceToWitness(product);   // instant, so the wall reacts right away
    discoverAndPopulate(product); // then replace with the REAL fleet off the LAN
    // A device id names ONE board: clear the field so the next board gets a
    // fresh suggestion instead of a twin (same id = same MQTT topics), which
    // is exactly the collision the fleet book exists to prevent. The id just
    // written lives on in the book row and on the certificate.
    $("device-id").value = "";
    persistProv();
    // The Vision is a TWO-board Canary: the ESP32 host just flashed here, and
    // the Grove Vision AI V2 camera module loads its model through its OWN
    // USB-C port. Say the next move out loud, or the demo dies half-done with
    // a module that never got a brain.
    const moduleNext = product.id && product.id.includes("vision") && !state.vision.module
      ? " Board 1 of 2 done — now move the USB cable to the CAMERA MODULE's own USB-C port " +
        "(the wide port on the carrier board, next to the Grove socket — not the XIAO's). " +
        "I'll recognize the module and offer its model below."
      : "";
    if (requiresLiveReceipt(product)) {
      setStatus("flash-result", "Firmware write verified. Watching the live boot for its device receipt…" + moduleNext, "ok");
      state.busy = false;
      await startMonitor({ postFlash: true });
    } else {
      setStatus("flash-result", "Firmware write verified. Flashing is complete. ✓" + moduleNext, "ok");
      maybeHatch();
      // The serial monitor should just work — start it automatically so the
      // live boot log is right there, no "Start" click. It reconnects on its
      // own across the reboot (native side), so this is safe to fire now.
      startMonitor({ postFlash: true });
    }
  } catch (e) {
    // Name the failure and its fix, never just the raw error (parity with the
    // browser's classifyFlashError — same kinds, same remedies).
    const c = classifyFlashError(e);
    state.lastError = String(e);
    setStatus(
      "flash-result",
      (c.title ? c.title + ". " : "The flash didn't finish. ") + c.hint +
        " (Details: " + e + ")",
      "err"
    );
    logEvent("err", "Flash failed [" + c.kind + "]: " + e);
    hideHatchCard();
  } finally {
    unlisten();
    unlistenRescue();
    unlistenMap(); // or every reflash stacks another map listener
    btn.disabled = false;
    btn.textContent = "Flash my Canary";
    $("dev-channel").disabled = false;
    state.busy = false;
    // The board reboots after a flash; let the watcher re-sync from scratch.
    state.chip = null;
    state.failedPort = null;
  }
}

// ── the token, shown once (parity: the browser's done-card token panel) ─────
// The desktop mints the credential and banks it in the OS keychain — but the
// owner deserves to SEE it too: the keychain can refuse (locked), and a copy
// in a password manager is the honest backstop. Shown after each flash that
// minted one; cleared by resetOutcome like every other per-flash artifact.
function renderTokenOnce(apiToken) {
  const box = $("token-once");
  if (!box) return;
  if (!apiToken) { box.classList.add("hidden"); box.innerHTML = ""; return; }
  box.innerHTML =
    `<b>This board's local API key — shown once.</b> ` +
    `<span class="muted">Written into the board with the Wi-Fi, and kept in ${esc(secretStore.where())} ` +
    `for the fleet book. Copy it if you want it somewhere else too — a reflash mints a new one.</span>` +
    `<span class="token-row"><code>${esc(apiToken)}</code>` +
    `<button type="button" class="btn btn-ghost btn-small" id="token-copy">Copy</button></span>`;
  box.classList.remove("hidden");
  const btn = $("token-copy");
  if (btn) btn.addEventListener("click", async () => {
    try { await navigator.clipboard.writeText(apiToken); btn.textContent = "Copied ✓"; }
    catch (_) { btn.textContent = "Select + copy by hand"; }
    setTimeout(() => { btn.textContent = "Copy"; }, 2500);
  });
}

// ── this session's nursery (parity: the browser's roster strip) ─────────────
// A batch flasher's working memory: every board hatched since the app opened,
// numbered, with the facts that matter mid-batch — right where the next board
// gets plugged in. Distinct from the durable fleet book (the fleet view):
// this is "how far through the pile am I", not "what exists".
function renderNurseryStrip() {
  const box = $("nursery-strip");
  if (!box) return;
  const since = state.sessionStart || 0;
  const batch = Object.values(bookAll())
    .filter((e) => (e.flashedAt || 0) >= since)
    .sort((a, b) => (a.flashedAt || 0) - (b.flashedAt || 0));
  if (!batch.length) { box.classList.add("hidden"); box.innerHTML = ""; return; }
  const rows = batch.map((e, i) => {
    const ago = Math.max(0, Math.round((Date.now() - e.flashedAt) / 60000));
    const when = ago < 1 ? "just now" : ago === 1 ? "1 min ago" : `${ago} min ago`;
    const macTail = e.mac ? e.mac.split(":").slice(-2).join(":") : "";
    const extras = [e.wifiBaked ? "Wi-Fi baked ✓" : null, e.hasToken ? "API key kept ✓" : null]
      .filter(Boolean).join(" · ");
    return `<div class="nursery-row"><span class="nursery-n">${i + 1}</span>` +
      `<b>${esc(e.ringName || e.deviceId || e.productLabel || "Canary")}</b>` +
      `<span class="muted">${esc([e.productLabel, e.fw ? "v" + e.fw : "", macTail ? "…" + macTail : ""].filter(Boolean).join(" · "))}</span>` +
      (extras ? `<span class="nursery-extras">${esc(extras)}</span>` : "") +
      `<span class="muted nursery-ago">${esc(when)}</span></div>`;
  }).join("");
  box.innerHTML = `<div class="nursery-head">🐣 This session's nursery — ` +
    `${batch.length} hatchling${batch.length === 1 ? "" : "s"} so far</div>` + rows;
  box.classList.remove("hidden");
}

function readProvisioning(product) {
  if (!product) return null;
  const usbSecrets = product.provisioning === "usb-secrets";
  // Whether THIS firmware reads broker credentials out of NVS — a property of
  // the firmware, not of how its identity is provisioned. The displays are the
  // case that made the distinction necessary: they are `on-glass`, so the old
  // `usbSecrets` gate blanked their broker fields, yet canary-display's
  // runtime_config.h carries mqtt_host/port/user/pass and mqtt_mgr.cpp reads
  // them. A display therefore had NO way to be told a broker — not here, and
  // not on the glass, whose portal only ever asked for Wi-Fi.
  const broker = product.broker_nvs === true;
  // Wi-Fi-only boards: an empty SSID just means "skip the preload" — the
  // board's own setup path still works, so nothing to validate or write.
  // A chosen room preset is a thing to write, exactly like a network or a
  // broker. Today every dial-carrying product is usb-secrets so this arm can't
  // be reached by one — but relying on that coincidence would mean a future
  // dial product silently drops the preset the user picked.
  const dials = dialsForFlash(product);
  if (!usbSecrets && !$("wifi-ssid").value && !$("mqtt-host").value && !dials) return null;
  const fields = ["wifi-ssid", "wifi-pass"]
    .concat(usbSecrets || broker ? ["device-id"] : [])
    .concat(broker ? ["mqtt-host", "mqtt-port", "mqtt-user", "mqtt-pass"] : []);
  for (const id of fields) {
    const input = $(id);
    if (!input.checkValidity()) {
      input.reportValidity();
      return false; // typed but invalid — the caller must NOT flash without it
    }
  }
  const wifiPass = $("wifi-pass").value;
  if (wifiPass && (new TextEncoder().encode(wifiPass).length < 8 ||
                   new TextEncoder().encode(wifiPass).length > 63)) {
    setStatus("flash-result", "Wi-Fi password must be 8–63 UTF-8 bytes (or empty for an open network).", "err");
    return false; // same: an answer the user gave, not an answer to drop
  }
  return {
    // Identity rides along for broker-capable boards too (displays): their
    // MQTT topics derive from dev_id, and the alternative is the flavor's
    // shared compiled id going sticky. Cleared field → empty → not written.
    deviceId: usbSecrets || broker ? $("device-id").value.trim() : "",
    wifiSsid: $("wifi-ssid").value,
    wifiPass,
    mqttHost: broker ? $("mqtt-host").value.trim() : "",
    mqttPort: broker ? Number($("mqtt-port").value) || 1883 : 1883,
    mqttUser: broker ? $("mqtt-user").value : "",
    mqttPass: broker ? $("mqtt-pass").value : "",
    // Which NVS encoding this firmware reads (catalog, from the source):
    // "blob" for canary/wap, "string" for sense/vision/display.
    wifiNvs: product.wifi_nvs || "string",
    // Room presets as clamped typed ints, or empty when the user left the
    // dials as they ship (the backend writes no dial keys at all then).
    dials: dials || { u8: {}, u32: {} },
  };
}

function clearSecretFields() {
  $("wifi-pass").value = "";
  $("mqtt-pass").value = "";
  // Wiping the field while its password stays scannable in a rendered QR
  // would be theater — the QR goes with it.
  const qr = $("wifi-qr-box");
  if (qr) { qr.classList.add("hidden"); qr.innerHTML = ""; }
}

// ── module inference preview: the frame WITH its detections drawn ───────────
// The receipt's `boxes` are SSCMA detections — [x, y, w, h, score, target],
// x/y the box CENTER in frame pixels, score already a 0-100 percent. Geometry,
// identity colors, and confidence bands mirror the browser bench (the source
// of truth is canary-local/assets/we2-core.js stylizeDetections; the two
// flashers share no code, so per the two-flashers rule the paint lives here
// too). Garbage boxes are dropped, never thrown on.
const MODULE_CLASSES = ["person"];
const PREVIEW_HUES = [140, 45, 200, 320, 20, 260, 80, 175];

function moduleDetections(boxes, classes = MODULE_CLASSES) {
  const out = [];
  for (const b of boxes || []) {
    if (!Array.isArray(b) || b.length < 6) continue;
    const [x, y, w, h, score, target] = b;
    if (![x, y, w, h].every((n) => typeof n === "number" && Number.isFinite(n))) continue;
    const pct = Math.max(0, Math.min(100, Math.round(Number(score) || 0)));
    out.push({ x, y, w, h, score: pct, label: classes[target] || "object" });
  }
  return out;
}

function moduleSummary(boxes, classes = MODULE_CLASSES) {
  const dets = moduleDetections(boxes, classes);
  if (!dets.length) return "nothing in frame";
  const top = dets.reduce((m, d) => Math.max(m, d.score), 0);
  const word = dets[0].label + (dets.length > 1 ? "s" : "");
  return `${dets.length} ${word} · top ${top}%`;
}

// Draw one frame's detections onto a 2d context — shared by the one-shot
// receipt preview and the live bench, so the two renders can't drift.
function paintBoxes(ctx, boxes, classes) {
  moduleDetections(boxes, classes).forEach((d, i) => {
    const hue = PREVIEW_HUES[i % PREVIEW_HUES.length];
    const band = d.score >= 60 ? "ok" : d.score >= 35 ? "soft" : "faint";
    const color = `hsl(${hue} 90% ${band === "ok" ? 66 : 58}%)`;
    const x = d.x - d.w / 2, y = d.y - d.h / 2;
    ctx.lineWidth = band === "ok" ? 3 : 2;
    ctx.strokeStyle = color;
    ctx.strokeRect(x, y, d.w, d.h);
    // corner ticks make thin boxes readable on busy frames
    ctx.lineWidth = band === "ok" ? 5 : 3;
    const t = Math.min(14, d.w / 4, d.h / 4);
    ctx.beginPath();
    for (const [cx, cy, dx, dy] of [[x, y, 1, 1], [x + d.w, y, -1, 1], [x, y + d.h, 1, -1], [x + d.w, y + d.h, -1, -1]]) {
      ctx.moveTo(cx + dx * t, cy); ctx.lineTo(cx, cy); ctx.lineTo(cx, cy + dy * t);
    }
    ctx.stroke();
    const text = `#${i + 1} ${d.label} · ${d.score}%`;
    ctx.font = "700 13px ui-monospace, Menlo, monospace";
    const tw = ctx.measureText(text).width + 10;
    const ly = y - 20 >= 0 ? y - 20 : y + d.h + 1;
    ctx.fillStyle = `hsl(${hue} 65% 14% / ${band === "ok" ? 0.92 : 0.85})`;
    ctx.fillRect(x, ly, tw, 19);
    ctx.fillStyle = color;
    ctx.fillText(text, x + 5, ly + 14);
  });
}

function renderModulePreview(receipt) {
  const cv = $("module-preview");
  if (!receipt || !receipt.preview_image || !cv.getContext) return;
  const img = new Image();
  img.onload = () => {
    cv.width = img.width; cv.height = img.height;
    const ctx = cv.getContext("2d");
    ctx.drawImage(img, 0, 0);
    paintBoxes(ctx, receipt.boxes, MODULE_CLASSES);
    cv.classList.remove("hidden");
  };
  img.src = "data:image/jpeg;base64," + receipt.preview_image;
}

async function onFlashModule() {
  await stopBenchQuiet(); // the bench holds the module's port — release it first
  const btn = $("module-flash-btn");
  btn.disabled = true;
  btn.textContent = "Flashing model…";
  $("dev-channel").disabled = true;
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
    $("module-progress").textContent = "100% · inference proved · " + moduleSummary(receipt.boxes);
    // Mirror of the host-side nudge: whichever board flashed first, the
    // other one is named — with its port — before this counts as done.
    const hostNext = state.vision.hostFlash ? "" :
      " Board 1 of 2 done — the XIAO host still needs the Canary Vision firmware: " +
      "move the cable to the XIAO's own USB-C port and pick Canary Vision above.";
    setStatus("flash-result", "Vision module verified, burned, answered AT, and ran one inference. ✓" + hostNext, "ok");
    renderModulePreview(receipt);
    renderReceipts(true);
    maybeHatch();
  } catch (e) {
    setStatus("flash-result", String(e), "err");
    logEvent("err", "Vision module flash failed: " + e);
  } finally {
    state.busy = false;
    btn.disabled = false;
    btn.textContent = "Flash & prove the Vision module";
    $("dev-channel").disabled = false;
  }
}

// ── the live bench: see what it sees, tune TSCORE/TIOU — no reflash ─────────
// Parity with the browser Lab's bench (canary-local/assets/we2-flash.js
// mountBench): the same protocol — continuous INVOKE=-1,0,0 for frames with
// boxes, BREAK to stop, TSCORE/TIOU set-then-read-back so the sliders never
// lie — and the same honesty: labels read "person" only when the module's own
// model card pins that class. Frames arrive over the we2:bench Tauri event
// and go nowhere but this window.
const liveBench = {
  running: false, pinned: false, seen: false,
  thr: { tscore: 50, tiou: 45 },
  lastFrame: null, frames: 0, fpsT0: 0,
  unlisten: null,
  session: null, // the backend worker's id — a dead worker's tail is ignored
};

function benchClasses() { return liveBench.pinned ? ["person"] : []; }

function syncSlider(key) {
  $("bench-" + key).value = String(liveBench.thr[key]);
  $("bench-" + key + "-out").textContent = String(liveBench.thr[key]);
  if (key === "tscore") $("bench-meter-tick").style.left = liveBench.thr.tscore + "%";
}

function benchGuide() {
  // Catalog-driven steps + troubleshooting (we2_module.bench) — the same spec
  // the browser bench renders. Defaults seed the sliders until the module's
  // own values are read back.
  const guide = (state.catalog && state.catalog.we2_module && state.catalog.we2_module.bench) || null;
  const body = $("bench-guide-body");
  body.textContent = "";
  if (!guide) { $("bench-guide").classList.add("hidden"); return; }
  if (guide.defaults) {
    if (typeof guide.defaults.tscore === "number") liveBench.thr.tscore = guide.defaults.tscore;
    if (typeof guide.defaults.tiou === "number") liveBench.thr.tiou = guide.defaults.tiou;
  }
  syncSlider("tscore"); syncSlider("tiou");
  const ol = document.createElement("ol");
  for (const s of guide.steps || []) {
    const li = document.createElement("li");
    li.textContent = s;
    ol.append(li);
  }
  body.append(ol);
  for (const t of guide.troubleshooting || []) {
    const p = document.createElement("p");
    p.className = "muted";
    p.textContent = `${t.when} — ${t.fix}`;
    body.append(p);
  }
  $("bench-guide").classList.toggle("hidden", !(guide.steps || []).length);
}

function benchReset() {
  liveBench.running = false; liveBench.pinned = false; liveBench.seen = false;
  liveBench.lastFrame = null; liveBench.frames = 0; liveBench.fpsT0 = 0;
  $("bench-start").disabled = false;
  $("bench-stop").disabled = true;
  $("bench-tscore").disabled = true;
  $("bench-tiou").disabled = true;
  $("bench-fps").textContent = "";
  $("bench-id").textContent = "";
  $("bench-seen").classList.add("hidden");
  $("bench-canvas").classList.add("hidden");
  $("bench-meter-fill").style.width = "0%";
  $("bench-meter-label").textContent = "watching… nothing yet";
  benchGuide();
}

function benchRender(boxes) {
  const cv = $("bench-canvas");
  if (!cv.getContext) return;
  const ctx = cv.getContext("2d");
  if (liveBench.lastFrame) {
    cv.width = liveBench.lastFrame.width;
    cv.height = liveBench.lastFrame.height;
    ctx.drawImage(liveBench.lastFrame, 0, 0);
  } else {
    cv.width = cv.width || 240;
    cv.height = cv.height || 240;
    ctx.fillStyle = "#08090b";
    ctx.fillRect(0, 0, cv.width, cv.height);
  }
  cv.classList.remove("hidden");
  paintBoxes(ctx, boxes, benchClasses());
  // The meter: best confidence vs the module's own reporting floor (TSCORE),
  // so the threshold slider visibly MEANS something.
  const dets = moduleDetections(boxes, benchClasses());
  const top = dets.reduce((m, d) => Math.max(m, d.score), 0);
  $("bench-meter-fill").style.width = (dets.length ? top : 0) + "%";
  $("bench-meter-label").textContent = dets.length
    ? `${moduleSummary(boxes, benchClasses())} — ${top > liveBench.thr.tscore
        ? "clears the floor by " + (top - liveBench.thr.tscore) : "at the floor"}`
    : "watching… nothing above the floor";
  if (dets.length && !liveBench.seen) {
    liveBench.seen = true;
    const seen = $("bench-seen");
    seen.textContent = liveBench.pinned
      ? "👁 It sees you — the model is live and working ✓"
      : "👁 It sees something — the model is live and working ✓";
    seen.className = "status ok";
    seen.classList.remove("hidden");
  }
}

function onBenchEvent(ev) {
  const p = (ev && ev.payload) || {};
  // A retired worker's tail (its `stopped`, a late frame) must never touch a
  // replacement bench: every backend event names its session, and only the
  // current one is heard.
  if (p.session != null && liveBench.session != null && p.session !== liveBench.session) return;
  if (p.kind === "log") {
    appendConsole("console", p.line + "\n");
  } else if (p.kind === "id") {
    liveBench.pinned = !!p.pinned;
    $("bench-id").textContent =
      "SSCMA firmware " + (p.software || "?") +
      (p.model ? " · model: " + p.model : " · no model card — labels will read “object”");
    $("bench-tscore").disabled = false;
    $("bench-tiou").disabled = false;
    benchReadThresholds();
  } else if (p.kind === "event" && p.data) {
    if (p.data.image) {
      const img = new Image();
      img.onload = () => {
        liveBench.lastFrame = img;
        liveBench.frames++;
        const now = performance.now();
        if (now - liveBench.fpsT0 >= 1000) {
          $("bench-fps").textContent =
            Math.round((liveBench.frames * 1000) / (now - liveBench.fpsT0)) + " fps";
          liveBench.frames = 0;
          liveBench.fpsT0 = now;
        }
        benchRender(p.data.boxes || []);
      };
      img.src = "data:image/jpeg;base64," + p.data.image;
    } else if (p.data.boxes) {
      benchRender(p.data.boxes); // boxes-only event: repaint over the CURRENT frame
    }
  } else if (p.kind === "error") {
    setStatus("flash-result", String(p.message || "the bench stopped"), "err");
    logEvent("err", "Live bench: " + (p.message || "stopped"));
    benchTeardown();
  } else if (p.kind === "stopped") {
    benchTeardown();
  }
}

async function benchReadThresholds() {
  // Ask the module for its CURRENT values so the sliders start honest.
  for (const key of ["tscore", "tiou"]) {
    try {
      const q = await invoke("we2_bench_cmd", { body: key.toUpperCase() + "?" });
      const v = q && q.data;
      if (typeof v === "number" && v >= 0 && v <= 100) liveBench.thr[key] = v;
    } catch (_) { /* keep the catalog default */ }
    syncSlider(key);
  }
}

async function onBenchSlider(key) {
  const input = $("bench-" + key);
  const cmd = key.toUpperCase();
  try {
    await invoke("we2_bench_cmd", { body: cmd + "=" + input.value });
    // Read back what the module actually holds — the slider never lies.
    const q = await invoke("we2_bench_cmd", { body: cmd + "?" });
    const v = q && typeof q.data === "number" ? q.data : Number(input.value);
    liveBench.thr[key] = v;
  } catch (e) {
    setStatus("flash-result", String(e), "err");
  }
  syncSlider(key);
}

async function onBenchStart() {
  if (liveBench.running || state.portKind !== "we2" || !state.port) return;
  liveBench.running = true;
  liveBench.seen = false;
  liveBench.lastFrame = null;
  liveBench.frames = 0;
  liveBench.fpsT0 = performance.now();
  $("bench-start").disabled = true;
  $("bench-stop").disabled = false;
  $("module-flash-btn").disabled = true; // the bench holds the module's port
  $("bench-seen").classList.add("hidden");
  $("bench-meter-label").textContent = "watching…";
  if (!liveBench.unlisten) liveBench.unlisten = await listen("we2:bench", onBenchEvent);
  try {
    liveBench.session = await invoke("we2_bench_start", { port: state.port });
  } catch (e) {
    setStatus("flash-result", String(e), "err");
    benchTeardown();
  }
}

async function onBenchStop() {
  try { await invoke("we2_bench_stop"); } catch (_) { /* stopping anyway */ }
  benchTeardown();
}

function benchTeardown() {
  liveBench.running = false;
  liveBench.session = null; // everything after this is a dead worker's tail
  $("bench-start").disabled = false;
  $("bench-stop").disabled = true;
  $("bench-fps").textContent = "";
  if (!state.busy) $("module-flash-btn").disabled = false;
}

// Quietly release the port before anything else needs it (a flash, unplug).
async function stopBenchQuiet() {
  if (!liveBench.running) return;
  try { await invoke("we2_bench_stop"); } catch (_) { /* going away anyway */ }
  benchTeardown();
}

// ── Advanced: dev channel + local file ──────────────────────────────────────
function onDevChannelToggle() {
  // Never mid-flash: the in-flight write already resolved its manifest, and
  // clearing state under it would leave the receipt logic waiting on a
  // product that no longer exists. The checkbox is disabled while busy too —
  // this guard covers any path that flips it anyway.
  if (state.busy) {
    $("dev-channel").checked = state.devChannel;
    return;
  }
  state.devChannel = $("dev-channel").checked;
  $("dev-banner").classList.toggle("hidden", !state.devChannel);
  logEvent("info", state.devChannel
    ? "Dev channel on — flashing from fw-dev-latest"
    : "Dev channel off — back to the pinned stable release");
  // The chosen product's version belongs to the OTHER channel now — drop it
  // and re-resolve availability from the newly-active manifest. Without a
  // chip there's no product list to re-render yet; the toggle still sticks.
  state.product = null;
  if (state.chip && state.portKind === "esp32") {
    $("step-flash").classList.add("disabled");
    $("flash-btn").disabled = true;
    $("flash-target").textContent = "";
    refreshManifest();
    renderProducts();
  }
}

// A local file skips the catalog's chip *guard* (there's no product to
// compare against), but never the chip-detect step — the note names exactly
// which chip is connected so the mismatch risk stays visible, not hidden.
function updateLocalFlashUi() {
  const connected = !!state.chip && state.portKind === "esp32";
  $("local-chip-note").textContent = connected
    ? `Connected: ${state.chip} on ${state.port}. A local file skips the ` +
      `catalog's chip guard — there's no product to compare against — so ` +
      `make sure this build targets that chip.`
    : "Plug in a Canary and let the chip read finish first — the file is " +
      "written to whichever chip is connected.";
  $("local-flash-btn").disabled = !(state.localFile && connected && !state.busy);
  // The rescue bench sits in the same Advanced card and gates on the same
  // connection, so refresh it on every pass through here.
  updateRescueUi();
}

// ── the rescue bench (Advanced): back up / restore / erase ──────────────────
// Parity with the browser Lab's rescue tools, over the native espflash sidecar
// (backup_flash / write_local_image / erase_chip stream `rescue:log`). Nothing
// here can brick the board — the ESP32's first-stage bootloader is mask ROM.
function rescueButtons(enabled) {
  for (const id of ["rescue-backup-btn", "rescue-restore-btn", "rescue-erase-btn"]) {
    const b = $(id);
    if (b) b.disabled = !enabled;
  }
}

function updateRescueUi() {
  const connected = !!state.chip && state.portKind === "esp32";
  const note = $("rescue-chip-note");
  if (note) {
    note.textContent = connected
      ? `Connected: ${state.chip} on ${state.port}` +
        (state.flashBytes ? ` · ${Math.round(state.flashBytes / (1 << 20))} MB flash` : "")
      : "Plug in a Canary and let the chip read finish first.";
  }
  const on = connected && !state.busy;
  rescueButtons(on);
  // A backup is a full-chip read, so it needs the flash size; without it,
  // restore and erase still work but backup can't.
  const backupBtn = $("rescue-backup-btn");
  if (backupBtn && !state.flashBytes) backupBtn.disabled = true;
  // The health check reads its own sizes from the partition table, so it only
  // needs a connection.
  const healthBtn = $("health-check-btn");
  if (healthBtn) healthBtn.disabled = !on;
  const healthNote = $("health-chip-note");
  if (healthNote) {
    healthNote.textContent = connected
      ? `Connected: ${state.chip} on ${state.port}${state.mac ? ` · ${state.mac}` : ""}`
      : "Plug in a Canary and let the chip read finish first.";
  }
}

const flashBaud = () => (state.catalog && state.catalog.flash_baud) || 921600;

// Release the busy-lock a rescue handler took before opening its dialog, when
// the user cancels out. The watcher resumes and the buttons re-enable.
function cancelRescue() {
  state.busy = false;
  updateRescueUi();
}

// The shared plumbing for one CONFIRMED rescue operation: a fresh console, the
// rescue:log stream, and releasing the busy-lock. The caller has already
// snapshotted the target and set state.busy (so the port watcher can't swap the
// board out while a dialog is open), so `op` runs the invoke against that
// snapshot and returns the success line; a throw becomes the error line. A
// write/erase reboots or wipes the board, so the detected chip is dropped
// afterward to force a clean re-read; keepChip:true is for the read-only backup.
async function runRescue(label, op, { keepChip = false } = {}) {
  const con = $("rescue-console");
  con.textContent = "";
  con.classList.remove("hidden");
  setStatus("rescue-result", "");
  resetOutcome();
  await stopMonitor();
  const unlisten = await listen("rescue:log", (ev) => {
    con.textContent += ev.payload + "\n";
    con.scrollTop = con.scrollHeight;
  });
  try {
    const okMsg = await op();
    setStatus("rescue-result", okMsg, "ok");
    logEvent("ok", `${label} ✓`);
  } catch (e) {
    setStatus("rescue-result", String(e), "err");
    logEvent("err", `${label} failed: ` + e);
  } finally {
    unlisten();
    state.busy = false;
    if (!keepChip) {
      state.chip = null;
      state.flashBytes = null;
      state.failedPort = null;
    }
    updateRescueUi();
  }
}

async function onRescueBackup() {
  if (state.busy || !state.chip || state.portKind !== "esp32") return;
  if (!state.flashBytes) {
    setStatus("rescue-result",
      "Couldn't read this chip's flash size — reconnect in download mode and try again.", "err");
    return;
  }
  // Snapshot the target and take the busy-lock BEFORE the save sheet opens: the
  // 1 s port watcher is otherwise free to swap in a different board while the
  // dialog sits open, and the write must land on the board the user is looking
  // at — never whatever got plugged in mid-dialog.
  const port = state.port, chip = state.chip, flashBytes = state.flashBytes;
  state.busy = true;
  rescueButtons(false);
  let out = null;
  try {
    out = await window.__TAURI__.dialog.save({
      defaultPath: `securacv-${chip}-backup.bin`.replace(/[^\w.-]+/g, "-"),
      filters: [{ name: "Flash backup", extensions: ["bin"] }],
    });
  } catch (e) {
    setStatus("rescue-result", String(e), "err");
    cancelRescue();
    return;
  }
  if (!out) { cancelRescue(); return; }
  // Read-only: the board isn't rebooted, so keep the detected chip.
  await runRescue("Backup", async () => {
    await invoke("backup_flash", { port, outPath: out, flashSize: flashBytes, baud: flashBaud() });
    return "Backup saved. Store the file like a house key — it holds the board's " +
      "identity key and saved Wi-Fi.";
  }, { keepChip: true });
}

async function onRescueRestore() {
  if (state.busy || !state.chip || state.portKind !== "esp32") return;
  // Snapshot + lock before the picker and the confirm, so the board named in
  // the confirmation is the exact board written — not one swapped in meanwhile.
  const port = state.port, chip = state.chip, flashBytes = state.flashBytes;
  state.busy = true;
  rescueButtons(false);
  let path = null;
  try {
    path = await window.__TAURI__.dialog.open({
      multiple: false,
      directory: false,
      filters: [{ name: "Firmware image or backup", extensions: ["bin"] }],
    });
  } catch (e) {
    setStatus("rescue-result", String(e), "err");
    cancelRescue();
    return;
  }
  if (!path) { cancelRescue(); return; }
  const name = String(path).split(/[\\/]/).pop();
  let ok = false;
  try {
    ok = await window.__TAURI__.dialog.confirm(
      `Write ${name} to the ${chip} on ${port}?\n\nThis overwrites the whole chip ` +
      `from offset 0. A backup or a merged factory .bin is right; an app-only ` +
      `build is refused before anything is written.`,
      { title: "Restore / write a .bin", kind: "warning" });
  } catch (_) {
    ok = window.confirm(`Write ${name} to the ${chip} on ${port}?`);
  }
  if (!ok) { cancelRescue(); return; }
  await runRescue("Restore", async () => {
    await invoke("write_local_image", {
      port,
      path,
      flashSize: flashBytes || null, // optional fit-check; the shape guard runs regardless
      baud: flashBaud(),
    });
    return `${name} written — the board is rebooting into it. Run a health check next.`;
  });
}

async function onRescueErase() {
  if (state.busy || !state.chip || state.portKind !== "esp32") return;
  // Erase is the most dangerous of the three — it destroys the identity key —
  // so snapshotting the target under the busy-lock before the confirm matters
  // most here: approving must wipe the named board, never a mid-dialog swap-in.
  const port = state.port, chip = state.chip;
  state.busy = true;
  rescueButtons(false);
  let ok = false;
  try {
    ok = await window.__TAURI__.dialog.confirm(
      `Erase the entire ${chip} on ${port}?\n\nThis wipes everything — including ` +
      `the board's Ed25519 identity key and saved Wi-Fi. Do this before selling ` +
      `or giving a board away. Back it up first if you might want any of it back.`,
      { title: "Erase the whole chip", kind: "warning" });
  } catch (_) {
    ok = window.confirm(`Erase the entire ${chip}? This destroys the identity key.`);
  }
  if (!ok) { cancelRescue(); return; }
  await runRescue("Erase", async () => {
    await invoke("erase_chip", { port });
    return "Chip erased — factory-fresh. Flash any image next.";
  });
}

// ── the health check: read the board's story, change nothing ────────────────
// Parity with the browser Lab's health report. Read-only, so no confirm — but
// it still snapshots the target + takes the busy-lock before touching the wire,
// like the rescue ops, so the watcher can't swap boards mid-read.
async function onHealthCheck() {
  if (state.busy || !state.chip || state.portKind !== "esp32") return;
  const port = state.port, chip = state.chip, mac = state.mac, flashBytes = state.flashBytes;
  state.busy = true;
  rescueButtons(false);
  const btn = $("health-check-btn");
  if (btn) { btn.disabled = true; btn.textContent = "Reading…"; }
  const con = $("health-console");
  con.textContent = "";
  con.classList.remove("hidden");
  $("health-report").classList.add("hidden");
  setStatus("health-result", "");
  resetOutcome();
  await stopMonitor();
  const unlisten = await listen("health:log", (ev) => {
    con.textContent += ev.payload + "\n";
    con.scrollTop = con.scrollHeight;
  });
  try {
    const report = await invoke("health_check", { port, chip, mac, flashBytes, baud: flashBaud() });
    report.generatedAt = new Date().toISOString(); // the browser stamps this too
    renderHealthReport(report);
    const v = report.verdict || {};
    setStatus("health-result", `Verdict: ${v.headline || "read complete"}.`,
      v.level === "attn" ? "err" : "ok");
    logEvent("ok", "Health check read");
  } catch (e) {
    setStatus("health-result", String(e), "err");
    logEvent("err", "Health check failed: " + e);
  } finally {
    unlisten();
    state.busy = false; // read-only — the board wasn't touched, keep the detected chip
    if (btn) btn.textContent = "Run a health check";
    updateRescueUi();
  }
}

// Build the report DOM from the object the backend returns. Everything read off
// flash (project names, versions, labels) goes in via textContent, never HTML.
function renderHealthReport(r) {
  const box = $("health-report");
  box.textContent = "";
  box.classList.remove("hidden");

  const h = (tag, cls, text) => {
    const e = document.createElement(tag);
    if (cls) e.className = cls;
    if (text != null) e.textContent = text;
    return e;
  };
  const row = (label, value, tone) => {
    const d = h("div", "health-row" + (tone ? " health-" + tone : ""));
    d.append(h("span", "health-row-k", label), h("span", "health-row-v", value));
    box.append(d);
  };
  const section = (title) => box.append(h("h3", "health-h", title));

  // Verdict + self-heal findings first — the point of the whole thing.
  const v = r.verdict || {};
  const vBox = h("div", "health-verdict health-" + (v.level || "ok"));
  vBox.append(h("strong", "health-verdict-h", `${v.level === "ok" ? "✓" : "⚠"} ${v.headline || ""}`));
  for (const f of (v.findings || [])) {
    const fEl = h("div", "health-finding");
    fEl.append(h("p", "health-finding-t", f.title));
    if (f.fix) fEl.append(h("p", "health-finding-fix muted", f.fix));
    vBox.append(fEl);
  }
  box.append(vBox);

  // Facts.
  if (r.chip) row("Chip", r.chip);
  if (r.mac) row("ID (MAC)", r.mac);
  if (r.flashBytes) row("Flash", `${Math.round(r.flashBytes / (1 << 20))} MB`);

  if (r.blank) {
    box.append(h("p", "muted", "No partition table at 0x8000 — this chip looks blank."));
    appendSaveReport(box, r);
    return;
  }

  if (r.slots && r.slots.length) {
    section("Firmware on the board");
    for (const s of r.slots) {
      const val = s.empty
        ? "empty"
        : `${s.project || "?"} ${s.version || ""}`.trim() + (s.built ? ` · built ${s.built}` : "");
      row(s.label + (s.active ? " — running now" : ""), val, s.active ? "ok" : null);
    }
  }

  if (r.ota) {
    section("Update history");
    if (r.ota.fresh && !r.ota.updatesSeen) {
      row("Over-the-air updates", "none yet — factory image");
    } else {
      row("Updates recorded", String(r.ota.updatesSeen));
      row("Boot state", r.ota.stateText, r.ota.pendingVerify ? "warn" : null);
    }
  }

  section("Health");
  if (r.coredump) {
    r.coredump.present
      ? row("Crash dump", `found (${r.coredump.size} bytes) — the board hard-crashed`, "warn")
      : row("Crash dump", "none stored", "ok");
  }
  if (r.witness && r.witness.boots != null) row("Lifetime boots", String(r.witness.boots));
  if (r.witness && r.witness.tamper) row("Tamper flag", `set (${r.witness.tamper})`, "warn");

  if (r.witness) {
    section("Witness chain");
    if (r.witness.seq != null) row("Records chained", String(r.witness.seq), "ok");
    if (r.witness.chainHeadFp) row("Chain head", r.witness.chainHeadFp + "…");
    row("Device identity", r.witness.provisioned ? "provisioned" : "not provisioned",
      r.witness.provisioned ? "ok" : null);
    row("Wi-Fi settings", r.witness.wifiConfigured ? "stored" : "none");
    box.append(h("p", "muted",
      "Presence only — the identity key and Wi-Fi password are never shown or saved; " +
      "the report notes only whether they exist."));
  }

  if (r.partitions && r.partitions.length) {
    section("Flash map");
    for (const p of r.partitions) {
      row(p.label || p.kind,
        `${p.kind} · ${Math.round(p.size / 1024)} KB @ 0x${(p.offset >>> 0).toString(16)}`);
    }
  }

  appendSaveReport(box, r);
}

function appendSaveReport(box, r) {
  const rowDiv = document.createElement("div");
  rowDiv.className = "row";
  const save = document.createElement("button");
  save.className = "btn btn-ghost btn-small";
  save.textContent = "Save report (.json)…";
  save.addEventListener("click", () => onSaveHealthReport(r));
  rowDiv.append(save);
  box.append(rowDiv);
}

async function onSaveHealthReport(r) {
  const stamp = (r.mac || "").replace(/[^0-9a-fA-F]/g, "").slice(-6).toLowerCase() || "canary";
  let out = null;
  try {
    out = await window.__TAURI__.dialog.save({
      defaultPath: `canary-${stamp}-report.json`,
      filters: [{ name: "Health report", extensions: ["json"] }],
    });
  } catch (e) {
    setStatus("health-result", String(e), "err");
    return;
  }
  if (!out) return;
  try {
    await invoke("save_text_file", { path: out, contents: JSON.stringify(r, null, 2) });
    setStatus("health-result", "Report saved.", "ok");
  } catch (e) {
    setStatus("health-result", "Couldn't save the report: " + e, "err");
  }
}

async function onPickLocalFile() {
  let path = null;
  try {
    path = await window.__TAURI__.dialog.open({
      multiple: false,
      directory: false,
      filters: [{ name: "Factory firmware image", extensions: ["bin"] }],
    });
  } catch (e) {
    setStatus("local-result", String(e), "err");
    return;
  }
  if (!path) return;
  setStatus("local-result", "");
  $("local-name").textContent = "reading…";
  try {
    // Size + SHA-256 are computed and SHOWN before anything is written, so
    // the confirm is over a named, fingerprinted file — never a blind path.
    const info = await invoke("inspect_local_file", { path });
    state.localFile = { path, name: String(path).split(/[\\/]/).pop(), ...info };
    $("local-name").textContent = state.localFile.name;
    $("local-size").textContent = state.localFile.size.toLocaleString() + " B";
    $("local-sha").textContent = state.localFile.sha256;
    const magic = $("local-magic");
    magic.classList.toggle("hidden", state.localFile.esp_magic);
    if (!state.localFile.esp_magic) {
      // Advisory only: the backend already required the factory shape (the
      // partition table at 0x8000), and on the catalog's chips the
      // bootloader sits at 0x0 — so a missing 0xE9 is worth a sentence,
      // never a refusal.
      magic.textContent =
        "⚠ This file doesn't start with 0xE9 — the ESP32's own “program " +
        "starts here” marker, which a factory image for these chips opens " +
        "with. It can still be written; the board can't be bricked.";
    }
    $("local-info").classList.remove("hidden");
  } catch (e) {
    state.localFile = null;
    $("local-name").textContent = "";
    $("local-info").classList.add("hidden");
    setStatus("local-result", String(e), "err");
  }
  updateLocalFlashUi();
}

async function onFlashLocalFile() {
  const file = state.localFile;
  if (!file || !state.chip || state.portKind !== "esp32") return;
  // The explicit confirm every other write in this app requires: a named
  // payload, a named target, and a yes that means yes.
  let ok = false;
  try {
    ok = await window.__TAURI__.dialog.confirm(
      `Write ${file.name} (${file.size.toLocaleString()} bytes) to the ` +
      `${state.chip} on ${state.port}?\n\nWe can't vouch for a personal ` +
      `file's origin — that part is on you.`,
      { title: "Install a local file", kind: "warning" }
    );
  } catch (_) {
    ok = window.confirm(`Write ${file.name} to the ${state.chip} on ${state.port}?`);
  }
  if (!ok) return;

  const btn = $("local-flash-btn");
  const con = $("local-console");
  con.textContent = "";
  con.classList.remove("hidden");
  btn.disabled = true;
  btn.textContent = "Writing…";
  $("dev-channel").disabled = true;
  setStatus("local-result", "");
  resetOutcome();
  state.busy = true; // pause the watcher so it can't grab the port
  await stopMonitor();

  const unlisten = await listen("flash:log", (ev) => {
    con.textContent += ev.payload + "\n";
    con.scrollTop = con.scrollHeight;
  });

  try {
    // The inspected size + fingerprint ride along so the backend can prove
    // the confirmed bytes are the ones still on disk — a changed file is
    // refused, never silently written.
    const receipt = await invoke("flash_local_file", {
      port: state.port,
      baud: state.catalog.flash_baud || 921600,
      path: file.path,
      expectedSize: file.size,
      expectedSha256: file.sha256,
    });
    state.vision.hostFlash = receipt;
    state.vision.hostBoot = null;
    renderReceipts();
    setStatus(
      "local-result",
      `Write verified. ${file.name} is on the board — local-file ` +
      `(fingerprint only), SHA-256 ${receipt.installed_sha256.slice(0, 16)}….`,
      "ok"
    );
    logEvent("ok", `Local file ${file.name} flashed`);
    state.busy = false;
    // Same as the release path: the boot log should just be there.
    startMonitor({ postFlash: true });
  } catch (e) {
    setStatus("local-result", String(e), "err");
    logEvent("err", "Local-file flash failed: " + e);
  } finally {
    unlisten();
    btn.textContent = "Write this file to the board";
    $("dev-channel").disabled = false;
    state.busy = false;
    // The board reboots after a flash; let the watcher re-sync from scratch.
    state.chip = null;
    state.failedPort = null;
    updateLocalFlashUi();
  }
}

// ── radar tuning suite (Sense): live knobs over the monitor port ────────────
// Mirror of the web flasher's radar-bench tuning suite (two frontends share
// no UI code — parity is the promise, see CLAUDE.md). The firmware side is
// common/console/tuning_console.h: `set <knob> <value>` clamps, applies to
// the live FSMs, and persists to NVS; every command answers with the full
// `[cfg]` snapshot line, which is the only state this panel trusts.

const senseTune = {
  tail: "",       // partial console line across serial:log chunks
  synced: false,  // saw a [cfg] snapshot this monitor session
  active: false,  // panel visible (a Sense product is on the bench)
  timer: 0,       // honesty timeout for "no tuning console"
};

function senseProductForTuning() {
  // The product whose firmware is (or just landed) on the board: the last
  // host flash wins, else the chosen product card.
  const flashedId = state.vision && state.vision.hostFlash && state.vision.hostFlash.product_id;
  const id = flashedId || (state.product && state.product.id);
  if (!id) return null;
  const p = ((state.catalog && state.catalog.products) || []).find((x) => x.id === id);
  return p && p.reflexes && Array.isArray(p.reflexes.knobs) ? p : null;
}

function sendTune(cmd) {
  invoke("serial_monitor_send", { command: cmd + "\n" }).catch((e) =>
    setStatus("monitor-status", String(e), "err"));
}

function renderSenseTune() {
  const p = senseProductForTuning();
  const wrap = $("sense-tune");
  senseTune.active = !!p;
  senseTune.synced = false;
  senseTune.tail = "";
  clearTimeout(senseTune.timer);
  wrap.classList.toggle("hidden", !p);
  if (!p) return;
  const badge = $("sense-tune-badge");
  badge.textContent = "syncing with the board…";
  const grid = $("sense-knobs");
  grid.textContent = "";
  for (const k of p.reflexes.knobs) {
    if (!k.console) continue;
    const row = document.createElement("label");
    row.className = "sense-knob";
    const name = document.createElement("span");
    name.className = "sense-knob-name";
    name.textContent = k.console;
    const input = document.createElement("input");
    input.type = "range";
    input.min = String(k.bounds[0]);
    input.max = String(k.bounds[1]);
    input.step = k.unit === "cm" ? "10" : k.unit === "bpm" ? "1" : "50";
    input.value = String(k.value);
    input.disabled = true; // enabled on the first [cfg] sync
    input.dataset.knob = k.console;
    input.dataset.unit = k.unit;
    const val = document.createElement("span");
    val.className = "sense-knob-val";
    val.textContent = `${k.value} ${k.unit}`;
    input.addEventListener("input", () => { val.textContent = `${input.value} ${k.unit}`; });
    input.addEventListener("change", () => sendTune(`set ${k.console} ${input.value}`));
    row.title = `${k.id} — default ${k.value} ${k.unit}, range ${k.bounds[0]}–${k.bounds[1]}`;
    row.append(name, input, val);
    grid.append(row);
  }
  $("sense-reset").disabled = true;
  $("sense-stream").disabled = true;
  $("sense-raw").disabled = true;
}

function senseTuneHandshake() {
  if (!senseTune.active) return;
  // Ask for the knob snapshot (twice — the board may still be rebooting),
  // then be honest if this firmware predates the tuning console.
  setTimeout(() => { if (state.monitoring && !senseTune.synced) sendTune("cfg"); }, 800);
  setTimeout(() => { if (state.monitoring && !senseTune.synced) sendTune("cfg"); }, 2500);
  senseTune.timer = setTimeout(() => {
    if (state.monitoring && senseTune.active && !senseTune.synced) {
      $("sense-tune-badge").textContent =
        "no tuning console on this firmware — install the latest release to tune live";
    }
  }, 5000);
}

// `[cfg] debounce=300 … stream=1000 raw=0` — the whole knob state on one
// line, sent on demand and after every set/reset. stream/raw are console
// session state, split out from the knob map.
function parseSenseCfgLine(line) {
  const t = String(line || "").trim();
  if (!/^\[cfg\]\s/.test(t)) return null;
  const values = {};
  let stream = null, raw = null;
  const re = /([a-z_]+)=(\d+)/g;
  let m;
  while ((m = re.exec(t))) {
    const v = Number(m[2]);
    if (m[1] === "stream") stream = v;
    else if (m[1] === "raw") raw = v === 1;
    else values[m[1]] = v;
  }
  return Object.keys(values).length ? { values, stream, raw } : null;
}

function onSenseTuneLine(line) {
  const badge = $("sense-tune-badge");
  const cfg = parseSenseCfgLine(line);
  if (cfg) {
    senseTune.synced = true;
    for (const input of $("sense-knobs").querySelectorAll("input[type=range]")) {
      const v = cfg.values[input.dataset.knob];
      if (!Number.isFinite(v)) continue;
      input.value = String(v);
      input.disabled = false;
      const val = input.parentElement.querySelector(".sense-knob-val");
      if (val) val.textContent = `${v} ${input.dataset.unit}`;
    }
    $("sense-reset").disabled = false;
    $("sense-stream").disabled = false;
    $("sense-raw").disabled = false;
    if (Number.isFinite(cfg.stream)) {
      const sel = $("sense-stream");
      const v = String(cfg.stream);
      if ([...sel.options].some((o) => o.value === v)) sel.value = v;
    }
    if (typeof cfg.raw === "boolean") $("sense-raw").checked = cfg.raw;
    badge.textContent = "LIVE — knobs synced with the board";
    return;
  }
  const verdict = /^\[tune\]\s+(ok|err)\s+(.*)$/.exec(String(line).trim());
  if (verdict) badge.textContent = (verdict[1] === "ok" ? "✓ " : "⚠ ") + verdict[2];
}

function feedSenseTune(chunk) {
  if (!senseTune.active) return;
  senseTune.tail += String(chunk);
  const lines = senseTune.tail.split("\n");
  senseTune.tail = lines.pop() || "";
  if (senseTune.tail.length > 512) senseTune.tail = ""; // never hoard a runaway line
  for (const line of lines) onSenseTuneLine(line);
}

// ── serial monitor + earned receipts ────────────────────────────────────────
async function startMonitor(opts) {
  if (!state.port || state.portKind !== "esp32") {
    setStatus("flash-result", "Connect the ESP32 host port to start its serial monitor.", "err");
    return;
  }
  $("serial-monitor").classList.remove("hidden");
  $("serial-console").textContent = "";
  clearBootDiagnosis(); // a fresh boot gets a fresh verdict
  $("monitor-status").textContent = "Waiting for the board to reappear after reboot…";
  renderSenseTune(); // the radar tuning suite rides the monitor on Sense boards
  setMonitorButtons(true);
  try {
    await invoke("start_serial_monitor", {
      port: state.port,
      vid: state.portInfo && state.portInfo.vid,
      pid: state.portInfo && state.portInfo.pid,
      baud: state.catalog.console_baud || 115200,
      // Only the flash flows set this. It lets the monitor reboot a
      // native-USB board ONCE so the boot streams from its first line — and
      // rescues the board espflash left sitting in its bootloader, which is
      // what used to demand the unplug/replug ritual. The Start button and
      // every other caller attach as pure observers: watching a running
      // board must never restart it.
      postFlash: !!(opts && opts.postFlash),
    });
    senseTuneHandshake();
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
  setStatus("local-result", "");
  try { renderTokenOnce(null); } catch (_) {}
  // The previous install's map describes bytes that are about to stop being
  // true — showing it beside a new flash would be a lie by staleness.
  try { renderChangeMap(null); } catch (_) {}
  try { hideHatchCard(); } catch (_) {}
  // Host receipts belong to the image being overwritten — clear them. The
  // MODULE receipt survives: the model lives in the camera module's own
  // 16 MB flash and persists across every host reflash, so a module-first
  // session must still count it after the host is flashed (wiping it here
  // sent the user back to reflash a module that was already done, and the
  // two-board hatch could never fire).
  if (state.vision) { state.vision.hostFlash = null; state.vision.hostBoot = null; }
  try { renderReceipts(); } catch (_) {}
  const con = $("serial-console");
  if (con) con.textContent = "";
  const ms = $("monitor-status");
  if (ms) ms.textContent = "";
  $("serial-monitor").classList.add("hidden");
  $("sense-tune").classList.add("hidden");
  senseTune.active = false;
  clearTimeout(senseTune.timer);
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

  // Name what was written honestly: a release names its version and channel
  // (dev flashes must never read as stable); a local file has no version —
  // only its fingerprint speaks for it.
  const hostLabel = host
    ? `✓ ${host.channel === "local" ? host.product_id : "v" + host.version}` +
      `${host.channel === "dev" ? " (dev)" : ""} · ${host.bytes_written.toLocaleString()} B · ` +
      `${host.release_verification || "SHA-256"} · ${host.installed_sha256.slice(0, 12)}…`
    : vision
      ? "waiting for ESP32 flash — plug the XIAO's own USB-C port"
      : "waiting for ESP32 flash";
  setReceipt("receipt-host-image", !!host, hostLabel);
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
        : "waiting for WE2 model — plug the module's own USB-C port (wide port by the Grove socket)"
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
// The update's own release notes, rendered safely: escape everything first,
// then re-allow exactly two bits of markdown the notes use — `- ` bullets and
// `**bold**` — so the banner can say WHAT is changing, not just a number.
function notesHtml(notes) {
  const lines = String(notes || "").split("\n").map((l) => l.trim()).filter(Boolean);
  if (!lines.length) return "";
  const li = [];
  const p = [];
  const bold = (s) => esc(s).replace(/\*\*([^*]+)\*\*/g, "<b>$1</b>");
  lines.forEach((l) => {
    if (l.startsWith("- ")) li.push(`<li>${bold(l.slice(2))}</li>`);
    else p.push(bold(l));
  });
  return (p.length ? `<p>${p.join(" ")}</p>` : "") + (li.length ? `<ul>${li.join("")}</ul>` : "");
}

async function checkForUpdate(manual = false) {
  prefs.lastCheckedAt = Date.now();
  savePrefs();
  $("health-checked").textContent = fmtTime(prefs.lastCheckedAt);
  try {
    const up = await invoke("check_update");
    state.update = up || null;
    if (up) {
      $("update-text").textContent = `Version ${up.version} is ready (you have ${up.current_version}).`;
      $("update-notes").innerHTML = notesHtml(up.notes);
      $("update-notes").classList.toggle("hidden", !up.notes);
      // Routine re-checks stay polite: a version the user already clicked
      // "Later" on doesn't re-raise the banner (the About page and the health
      // chip still show it; a manual check always does).
      if (manual || prefs.updateDismissed !== up.version) {
        $("update-banner").classList.remove("hidden");
      }
      $("health-update").classList.remove("hidden");
      const upd = $("splash-upd"); if (upd) { upd.textContent = "update ready → v" + up.version; }
      if (state.announcedUpdate !== up.version) {
        state.announcedUpdate = up.version; // routine re-checks don't re-log it
        logEvent("info", "Update available: v" + up.version);
      }
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
  if (state.update) logEvent("info", "Installing update v" + state.update.version + "…");
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
       ${state.update.notes ? `<div class="update-notes"><b>What's changing</b>${notesHtml(state.update.notes)}</div>` : ""}
       <div class="row"><button class="btn btn-primary btn-small" id="about-update">Update &amp; relaunch</button>
       <button class="btn btn-ghost btn-small" id="about-check">Check again</button></div>`
    : `<p class="status">You're on the newest build. The app checks on its own — at launch and every few hours — and heals forward; updates are signed and verified before they install.</p>
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
      <p class="fineprint muted">Reset also forgets the setup profile's saved passwords (from ${esc(secretStore.where())}), every stored device API key, and the fleet book.</p>
    </section>`;

  const check = $("about-check");
  if (check) check.addEventListener("click", () => { check.disabled = true; check.textContent = "Checking…"; checkForUpdate(true); });
  const upd = $("about-update");
  if (upd) upd.addEventListener("click", onInstallUpdate);
  const reset = $("about-reset");
  if (reset) reset.addEventListener("click", async () => {
    prefs.roster = []; prefs.log = []; prefs.prov = {}; prefs.hubSsid = "";
    prefs.book = {};
    savePrefs();
    // Sweep the secret drawer too — every key this app ever wrote, gone from
    // the OS store (and the prefs fallback), not just from the tracked list.
    // An incomplete sweep is reported as exactly that, never as a clean
    // reset: the leftover keys stay tracked so running reset again retries.
    let leftBehind = 0;
    try { leftBehind = await secretStore.wipeAll(); } catch (_) {}
    fleetBook.renderSoon();
    updateResumeState();
    logEvent(
      leftBehind ? "err" : "info",
      leftBehind
        ? `Memory reset — but ${leftBehind} entr${leftBehind === 1 ? "y" : "ies"} in ` +
          `${secretStore.where()} couldn't be removed (store locked?). Reset again to retry.`
        : "Memory reset"
    );
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
    // The boot receipt carries the board's REAL identity, and `boot` is
    // already in hand here — so the certificate is derived from the key
    // rather than rolled, and matches the one the iPhone will show for this
    // same Canary. Without this the derived path was unreachable and both
    // apps quietly went back to naming the bird twice.
    const bootFp = (boot && boot.manifest && boot.manifest.pubkey_fp) || "";
    const cert = mintCertificate(product, undefined, bootFp); // a real birth certificate, once
    renderCertificate(cert);
    rosterAdd({ kind: "canary", name: cert ? cert.name : (product && product.name) || "Canary", chip: state.chip || "" });
    logEvent("ok", `${cert ? cert.name : (product && product.name) || "Canary"} hatched`);
    updateResumeState();
    // The certificate's name is how the owner KNOWS this bird — carry it
    // onto the fleet book row so the Fleet tab speaks the same name.
    try {
      const entry = cert && lastBookKey && bookAll()[lastBookKey];
      if (entry) { entry.ringName = cert.name; savePrefs(); fleetBook.renderSoon(); }
    } catch (_) {}
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

// A device with a KEY names itself. Derived from its fingerprint with the
// SAME algorithm the browser Lab and the iPhone app run
// (canary-local/tools/hatchery/derive.mjs, ios/Shared/BirthCertificate.swift),
// so one bird has one name on every surface with nothing stored or synced.
// Inlined rather than imported because this bundle can't reach canary-local;
// tests/desktop_parity.test.js fails the build if it drifts from the module.
function hatchFnv1a(str) {
  let h = 0x811c9dc5;
  for (let i = 0; i < str.length; i++) {
    h ^= str.charCodeAt(i) & 0xff;
    h = (h + ((h << 1) + (h << 4) + (h << 7) + (h << 8) + (h << 24))) >>> 0;
  }
  return h >>> 0;
}
function hatchSeededRng(seedText) {
  let s = hatchFnv1a(String(seedText)) || 0x9e3779b9;
  return () => {
    s ^= (s << 13) >>> 0; s >>>= 0;
    s ^= s >>> 17;
    s ^= (s << 5) >>> 0; s >>>= 0;
    return (s >>> 0) / 0x100000000;
  };
}
function deriveCertificate(h, fingerprint, product, deviceId) {
  const fp = String(fingerprint || "").trim().toLowerCase();
  if (!h || !Array.isArray(h.first) || !h.first.length || !fp) return null;
  const rng = hatchSeededRng(fp);
  const pick = (a) => a[Math.floor(rng() * a.length)];
  const base = pick(h.first);
  const withTitle = Array.isArray(h.titles) && h.titles.length &&
    rng() < (typeof h.title_chance === "number" ? h.title_chance : 0.6);
  const house = (Array.isArray(h.house) && h.house.length) ? pick(h.house) : "";
  const ordinals = Array.isArray(h.ordinals) ? h.ordinals : [];
  const nth = ordinals.length > 1 ? 1 + Math.floor(rng() * (ordinals.length - 1)) : 1;
  const ordinal = ordinals[nth] || ("the " + nth + "th");
  const title = withTitle ? pick(h.titles) + " " : "";
  const motto = (Array.isArray(h.mottoes) && h.mottoes.length) ? pick(h.mottoes) : "";
  return {
    base,
    name: title + base + (house ? " " + house : ""),
    species: (product && product.name) || "Canary",
    lineage: ordinal + " of its name" + (house ? ", " + house.replace(/^the /, "") : ""),
    ringId: String(deviceId || "").trim() || fp.toUpperCase(),
    motto,
    craft: (h.certificate && h.certificate.craft) || "",
    derived: true,
  };
}

function mintCertificate(product, avoidBase, fingerprint) {
  const h = state.hatch;
  if (!h || !Array.isArray(h.first) || !h.first.length) return null;
  // The derived path wins whenever we know the device's identity; the roll
  // below is only for a blank chip that has no key yet.
  const derived = deriveCertificate(h, fingerprint || state.deviceFingerprint,
                                    product, state.deviceId);
  if (derived) { derived.ts = Date.now(); return derived; }
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
  // Read the id captured AT flash time: the field itself is cleared after a
  // success so the next board can't inherit a twin id.
  const provisionedId = state.lastProvisionedId
    || ($("device-id") ? $("device-id").value.trim() : "");
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
  // The dice button retires for a derived name — it renders the board's key,
  // so there is nothing to re-roll and every other app derives the same one.
  const rerollBtn = $("cert-reroll");
  if (rerollBtn) rerollBtn.hidden = !!cert.derived;
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
  // A derived name cannot be re-rolled: it is a rendering of the board's key,
  // and every other surface derives the same one. Re-rolling would make this
  // app the only one calling the bird by that name.
  if (prev.derived) return;
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
      "On your phone, join the SecuraCV-XXXX Wi-Fi network it creates — the Canary's setup wizard pops up on its own a moment later. (If it doesn't, open canary.local — or 192.168.4.1 — in your browser.)",
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
  // True when the flashed target was the Pi itself presented over USB-C
  // (rpiboot mass-storage) — the "what now" steps differ from a card reader.
  flashedViaPi: false,
  accountValid: false,
  accountRequested: false,
  // ETA bookkeeping, reset at each stage change
  eta: { stage: null, t0: 0, done0: 0 },
  fbTimer: null, // first-boot poll
  fbCountStop: null, // stops the escalation countdown paint (hubCountdownStart)
  resumeTimer: null, // resume-across-restart poll
  // True while the self-setup run (over the hub's service console) is going —
  // one at a time, and the backend enforces the same.
  headlessBusy: false,
  // The last flash's receipt, so the first-boot watch knows whether this card
  // carries the self-setup bundle it should run when the hub answers.
  lastReceipt: null,
  // The account typed at flash time, held in MEMORY ONLY so the first-boot
  // watch can finish Home Assistant's own onboarding the moment the hub
  // answers. Never persisted — a relaunch loses it on purpose, and the resume
  // copy says so honestly instead of pretending.
  pendingAccount: null,
  // A leftover human-facing note from the onboarding run (e.g. "one setup
  // page still wants a click"), so the self-setup finish line can carry it
  // instead of overwriting it with a clean "all done".
  onboardNote: null,
};

const HUB_HOST = "homeassistant.local:8123";
const HUB_REMEMBER_KEY = "securacv.hub.remember";
const HUB_LASTFLASH_KEY = "securacv.hub.lastflash";
// How long after a flash we'll still offer to resume the first-boot watch on
// relaunch — comfortably longer than HAOS's 10–20 min first boot.
const HUB_RESUME_WINDOW_MS = 45 * 60 * 1000;
// When the first-boot watch escalates from "be patient" to "here's how to go
// find it": past the honest 10–20 min first-boot window with margin. The watch
// keeps polling — this only changes what the panel says.
const HUB_FB_ESCALATE_MS = 25 * 60 * 1000;

const HUB_STAGE_COPY = {
  download: "Downloading Home Assistant OS…",
  decompress: "Unpacking the image…",
  write: "Writing to the card — don't remove it…",
  verify: "Reading every byte back to prove the write…",
  seed: "Adding your settings to the card…",
};

// The stages a flash walks, in order — rendered as pills that light up as
// each one passes, so there's always a visible sense of where you are.
// Settings now go into the IMAGE, before the write — so "Settings" sits between
// Unpack and Write. The pill row marks everything before the active stage as
// done, so an order that doesn't match the pipeline makes finished stages
// un-tick themselves halfway through a flash.
const HUB_STAGE_ORDER = ["download", "decompress", "seed", "write", "verify"];
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
    // Ticking "wired" is an explicit choice, so make the fields agree with it
    // BEFORE disabling them. A returning user has their remembered SSID
    // restored into a field with no password; leaving that text in place while
    // the inputs go disabled would read as "Wi-Fi typed" to the contradiction
    // guard and disable the flash button with its own remedy — "clear the
    // fields" — out of reach behind a disabled input.
    // Setting .value programmatically fires no `input` event, so the
    // remembered SSID stays in prefs and comes back when ethernet is unticked.
    if (wired) {
      $("hub-ssid").value = "";
      $("hub-pass").value = "";
      $("hub-hidden-net").checked = false;
    } else if (prefs.hubSsid) {
      $("hub-ssid").value = prefs.hubSsid;
    }
    $("hub-ssid").disabled = wired;
    $("hub-pass").disabled = wired;
    $("hub-hidden-net").disabled = wired;
    hubArm();
  });
  ["hub-ssid", "hub-pass"].forEach((id) => $(id).addEventListener("input", hubArm));
  $("hub-ssid").addEventListener("input", persistProv);
  // Same courtesy the Canary form gets: offer the network this computer is on,
  // but never over a remembered SSID or an explicit "wired" choice.
  if (!$("hub-ssid").value && !$("hub-ethernet").checked) {
    invoke("current_ssid").then((ssid) => {
      if (ssid && !$("hub-ssid").value && !$("hub-ethernet").checked) {
        $("hub-ssid").value = ssid;
      }
    }).catch(() => {});
  }
  // Account fields: live-validate on every keystroke.
  ["hub-acct-name", "hub-acct-user", "hub-acct-pass", "hub-acct-pass2"].forEach((id) =>
    $(id).addEventListener("input", () => {
      hubValidateAccount();
      hubArm();
    })
  );
  // Self-setup opt-in: arm/summary refresh, and remembered like the other
  // non-secret fields.
  $("hub-provision").addEventListener("change", () => {
    $("hub-provision-hint").textContent = $("hub-provision").checked
      ? "After the flash, keep this app open: it watches for the hub and finishes setup the moment it answers."
      : "";
    hubArm();
  });
  // Self-setup narration rides the same console as the flash itself.
  listen("hub:headless-log", (e) => {
    const el = $("hub-console");
    el.classList.remove("hidden");
    el.textContent += e.payload + "\n";
    el.scrollTop = el.scrollHeight;
  });
  // First-boot companion controls.
  $("hub-fb-open").addEventListener("click", () => openExternal("http://" + HUB_HOST));
  $("hub-fb-setup").addEventListener("click", () => {
    $("hub-fb-setup").classList.add("hidden");
    hubRunHeadlessSetup($("hub-fb-text"), $("hub-fb-setup"), $("hub-fb-host").value).then(
      (report) => {
        // A reachability failure is the moment the address field earns its
        // place: mDNS-blocked networks need the IP from the router.
        if (!(report && report.ok)) $("hub-fb-host").classList.remove("hidden");
      }
    );
  });
  $("hub-fb-stop").addEventListener("click", hubStopFirstBoot);
  // Standing self-setup: shown whenever this computer holds a maintenance key,
  // so a hub first-booted long after its flash (outside the resume window) can
  // still be finished with one click.
  invoke("hub_headless_available")
    .then((ok) => {
      if (ok) $("hub-standing").classList.remove("hidden");
    })
    .catch(() => {});
  $("hub-standing-run").addEventListener("click", async () => {
    const btn = $("hub-standing-run");
    btn.disabled = true;
    btn.textContent = "Setting up…";
    await hubRunHeadlessSetup($("hub-standing-text"), null, $("hub-standing-host").value);
    btn.disabled = false;
    btn.textContent = "Finish hub setup";
  });
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
  listen("hub:pi-usb-hint", (e) => {
    // A recognized failure (on Linux, almost always the missing udev rule) — show
    // the actionable fix in the persistent hint line so "done" can't bury it.
    $("hub-pi-usb-hint").textContent = e.payload;
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
function hubRecordFlash(provisionPending, piholeChoice, accountPending) {
  try {
    localStorage.setItem(
      HUB_LASTFLASH_KEY,
      JSON.stringify({
        host: HUB_HOST,
        at: Date.now(),
        // True when the card carries the self-setup bundle and this app still
        // owes the hub a setup run — survives a quit/relaunch mid-first-boot.
        provision: !!provisionPending,
        // True when an account was typed at flash time. The password itself
        // is never in THIS record — with "Remember these" on it lives in the
        // OS secret drawer, and hubMaybeResume restores it from there; with
        // it off, the resume copy says honestly that the wizard awaits.
        account: !!accountPending,
        // The Pi-hole decision travels WITH the pending run, not with the
        // remembered-settings blob. "Remember these" may be off, and the
        // checkbox's HTML default is checked — so relying on the live checkbox
        // after a relaunch would silently install Pi-hole for someone who
        // explicitly unticked it. An opt-out has to survive the restart.
        pihole: !!piholeChoice,
      })
    );
  } catch (_) {}
}
function hubClearFlashRecord() {
  try { localStorage.removeItem(HUB_LASTFLASH_KEY); } catch (_) {}
}
async function hubMaybeResume() {
  let rec;
  try { rec = JSON.parse(localStorage.getItem(HUB_LASTFLASH_KEY) || "null"); } catch (_) { rec = null; }
  if (!rec || !rec.at || Date.now() - rec.at > HUB_RESUME_WINDOW_MS) { hubClearFlashRecord(); return; }
  // If it's already up, there's nothing to resume — unless this app still owes
  // that hub its self-setup run (quit mid-first-boot with the bundle seeded).
  let up = false;
  try { up = await invoke("hub_probe_hub", { host: rec.host || HUB_HOST }); } catch (_) {}
  // With "Remember these" on, the login typed at flash time survived in the
  // OS secret drawer — restore it, and this relaunched app can still finish
  // Home Assistant onboarding itself, keeping the original promise across a
  // quit. The honest "you'll meet the wizard" note below now covers only the
  // cases where the password truly is gone (remember off, drawer refused).
  if (rec.account && !(hub.pendingAccount && hub.pendingAccount.password)) {
    try {
      const rawAcct = await secretStore.get(secretStore.key("hub", "account"));
      const acct = rawAcct ? JSON.parse(rawAcct) : null;
      if (acct && acct.password) hub.pendingAccount = acct;
    } catch (_) {}
  }
  const accountNote = rec.account && !(hub.pendingAccount && hub.pendingAccount.password)
    ? "This app was closed before it could finish your Home Assistant account, so if the " +
      "hub asks you to create one, that's why — its wizard takes a minute and wants the " +
      "same details you typed when flashing."
    : "";
  if (accountNote) hub.onboardNote = accountNote;
  if (up && !rec.provision) {
    if (hub.pendingAccount && hub.pendingAccount.password) {
      // The hub is up and the login survived the relaunch — finish the
      // account now, exactly as the live first-boot watch would have.
      const banner = $("hub-resume");
      banner.classList.remove("hidden");
      $("hub-resume-dot").className = "dot connected";
      $("hub-resume-text").textContent =
        "Your hub from earlier is up — creating your Home Assistant account…";
      $("hub-resume-open").classList.remove("hidden");
      $("hub-resume-open").addEventListener("click", () => openExternal("http://" + (rec.host || HUB_HOST)));
      $("hub-resume-dismiss").addEventListener("click", () => banner.classList.add("hidden"));
      await hubRunOnboarding($("hub-resume-text"), rec.host);
      hubClearFlashRecord();
      return;
    }
    // Nothing to resume — but an account-only flash still deserves the note
    // above rather than a silent vanish into a surprise wizard.
    if (accountNote) {
      const banner = $("hub-resume");
      banner.classList.remove("hidden");
      $("hub-resume-dot").className = "dot connected";
      $("hub-resume-text").textContent = "Your hub from earlier is up. 🐤 " + accountNote;
      $("hub-resume-open").classList.remove("hidden");
      $("hub-resume-open").addEventListener("click", () => openExternal("http://" + (rec.host || HUB_HOST)));
      $("hub-resume-dismiss").addEventListener("click", () => banner.classList.add("hidden"));
    }
    hubClearFlashRecord();
    return;
  }
  if (up && rec.provision) {
    const banner = $("hub-resume");
    banner.classList.remove("hidden");
    $("hub-resume-dot").className = "dot connected";
    $("hub-resume-text").textContent =
      "The hub you flashed earlier is up — finishing its setup from this computer…";
    $("hub-resume-open").classList.remove("hidden");
    $("hub-resume-open").addEventListener("click", () => openExternal("http://" + (rec.host || HUB_HOST)));
    $("hub-resume-dismiss").addEventListener("click", () => {
      hubClearFlashRecord();
      banner.classList.add("hidden");
    });
    $("hub-resume-setup").addEventListener("click", () => {
      $("hub-resume-setup").classList.add("hidden");
      hubRunHeadlessSetup($("hub-resume-text"), $("hub-resume-setup"), undefined, rec.pihole);
    });
    // Account first, same order as the live first-boot watch: being left at
    // a sign-in page is the failure this flow exists to prevent.
    if (hub.pendingAccount && hub.pendingAccount.password) {
      await hubRunOnboarding($("hub-resume-text"), rec.host);
    }
    const report = await hubRunHeadlessSetup(
      $("hub-resume-text"), $("hub-resume-setup"), undefined, rec.pihole
    );
    if (report && report.ok) hubClearFlashRecord();
    return;
  }
  const banner = $("hub-resume");
  banner.classList.remove("hidden");
  // Same countdown as the live watch, from the persisted flash time — the
  // relaunched app owes the user the same "when do I start worrying" answer.
  const stopCount = hubCountdownStart($("hub-resume-count"), rec.at);
  $("hub-resume-open").addEventListener("click", () => openExternal("http://" + (rec.host || HUB_HOST)));
  $("hub-resume-dismiss").addEventListener("click", () => {
    hubClearFlashRecord();
    banner.classList.add("hidden");
    stopCount();
    if (hub.resumeTimer) { clearInterval(hub.resumeTimer); hub.resumeTimer = null; }
  });
  let escalated = false;
  const tick = async () => {
    let alive = false;
    try { alive = await invoke("hub_probe_hub", { host: rec.host || HUB_HOST }); } catch (_) {}
    // The resumed watch escalates exactly like the live one — measured from
    // the persisted flash time (rec.at), so a relaunch mid-first-boot never
    // loses the 25-minute troubleshooting transition along with the timer.
    if (!alive && !escalated && Date.now() - rec.at > HUB_FB_ESCALATE_MS) {
      escalated = true;
      $("hub-resume-text").textContent = hubFindItChecklist();
    }
    if (alive) {
      if (hub.resumeTimer) { clearInterval(hub.resumeTimer); hub.resumeTimer = null; }
      stopCount();
      $("hub-resume-dot").className = "dot connected";
      $("hub-resume-open").classList.remove("hidden");
      // Account first on either path — the login that survived the relaunch
      // in the secret drawer finishes now, before any add-on installs.
      const ranOnboarding = !!(hub.pendingAccount && hub.pendingAccount.password);
      if (ranOnboarding) {
        $("hub-resume-text").textContent =
          "Your hub from earlier is up — creating your Home Assistant account…";
        await hubRunOnboarding($("hub-resume-text"), rec.host);
      }
      if (rec.provision) {
        // The relaunched app still owes this hub its self-setup run.
        $("hub-resume-text").textContent =
          "Your hub from earlier is up — finishing its setup from this computer…";
        $("hub-resume-setup").addEventListener("click", () => {
          $("hub-resume-setup").classList.add("hidden");
          hubRunHeadlessSetup($("hub-resume-text"), $("hub-resume-setup"), undefined, rec.pihole);
        });
        const report = await hubRunHeadlessSetup(
          $("hub-resume-text"), $("hub-resume-setup"), undefined, rec.pihole
        );
        if (report && report.ok) hubClearFlashRecord();
      } else {
        // accountNote (from the top of hubMaybeResume) is the same honesty the
        // early already-up path shows: without the secret drawer, a relaunch
        // still loses the password — say so instead of a surprise wizard. When
        // onboarding just ran, its own outcome line stays on screen.
        if (!ranOnboarding) {
          $("hub-resume-text").textContent =
            "Your hub from earlier is up. 🐤" + (accountNote ? " " + accountNote : "");
        }
        hubNotify("Your hub is ready", "Open " + (rec.host || HUB_HOST) + " to log in.");
        hubChime();
        hubClearFlashRecord();
      }
    }
  };
  hub.resumeTimer = setInterval(tick, 6000);
  tick();
}

// Visible countdown to the go-find-it escalation, so the troubleshooting
// tips are something the user can see coming ("in 18:32") rather than a
// surprise at the 25-minute mark — and so "nothing yet" reads as expected,
// not broken. Painted every second from the SAME deadline the escalation
// check uses (startedAt + HUB_FB_ESCALATE_MS); self-stops and hides its
// element when the deadline passes. Returns a stop() for the hub-answered
// and watch-dismissed paths.
function hubCountdownStart(el, startedAt) {
  let timer = null;
  const stop = () => {
    if (timer) { clearInterval(timer); timer = null; }
    el.classList.add("hidden");
  };
  const paint = () => {
    const left = startedAt + HUB_FB_ESCALATE_MS - Date.now();
    if (left <= 0) { stop(); return; }
    const m = Math.floor(left / 60000);
    const s = String(Math.floor((left % 60000) / 1000)).padStart(2, "0");
    el.textContent =
      "If it hasn't answered in " + m + ":" + s + ", we'll walk you through finding it.";
    el.classList.remove("hidden");
  };
  timer = setInterval(paint, 1000);
  paint();
  return stop;
}

// The go-find-it checklist for a hub that's past the honest first-boot
// window. Shared by the live first-boot watch AND the resumed-after-relaunch
// watch — the hub is often fine and it's the *finding* that failed (this
// computer can't resolve .local, a router blocking mDNS, a typo'd Wi-Fi
// password we can't see from here); a headless user must never be left
// staring at a spinner with no next move on either path.
function hubFindItChecklist() {
  return (
    "Longer than a normal first boot now. The Pi may well be fine and just hard to find; " +
    "try these, in order: " +
    "1) Open http://" + HUB_HOST + " from a phone or another computer — some computers " +
    "can't resolve .local names even when the hub is up (this watcher has the same limit). " +
    "2) Look in your router's device list for “homeassistant” and open its IP address " +
    "with :8123 on the end. " +
    "3) If you set Wi-Fi at flash time, a mistyped network name or password is invisible " +
    "from outside — plug an ethernet cable into the Pi (it needs no setup at all), or " +
    "re-flash the card with the Wi-Fi typed fresh. " +
    "Still watching in the background — if it answers, we'll say so."
  );
}

// Turn a backend error into calm, useful words: what happened, why the
// hardware is fine, and the one thing to do next. Cancels are not errors.
function hubPresentError(raw) {
  const msg = String(raw);
  if (msg.startsWith("canceled:")) {
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
    msg = `Looks good — password strength: ${strength}. Keep this app open after the flash and ` +
      "it finishes your account the moment the hub comes online.";
  }
  hub.accountValid = ok;
  hint.textContent = msg;
}

// ── remember settings (passwords in the OS secret drawer, the rest here) ────
// Non-secrets stay in localStorage as always. With "Remember" ticked, the
// two passwords now ALSO survive — in the OS credential store via
// secretStore (the note beside the checkbox names it): the Wi-Fi key under
// the same `wifi:<ssid>` entry the Canary form shares (one home, one
// profile), and the Home Assistant login under `hub:account`, which is what
// lets a relaunched app still finish onboarding (hubMaybeResume).
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
        provision: $("hub-provision").checked,
        // Stored even when unchecked: an explicit "no Pi-hole" must survive
        // a restart just like a yes.
        pihole: $("hub-provision-pihole").checked,
      })
    );
    const ssid = $("hub-ssid").value.trim();
    if (ssid && $("hub-pass").value) {
      secretStore.set(secretStore.key("wifi", ssid), $("hub-pass").value);
    }
    const acct = hub.accountRequested && hub.accountValid ? hubAccountValue() : null;
    if (acct) {
      secretStore.set(secretStore.key("hub", "account"), JSON.stringify(acct));
    }
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
    if (s.provision) $("hub-provision").checked = true;
    if ("pihole" in s) $("hub-provision-pihole").checked = !!s.pihole;
    if (s.boardId) hub.boardId = s.boardId; // applied when the board list renders
    // Restored account fields must re-validate, or the panel reads as
    // "untouched" and the account is silently skipped despite showing values.
    hubValidateAccount();
    // The passwords come back from the secret drawer — never over anything
    // typed, and both passes re-validate so arming stays truthful.
    if (s.ssid && !$("hub-pass").value) {
      secretStore.get(secretStore.key("wifi", s.ssid)).then((pw) => {
        if (pw && !$("hub-pass").value && $("hub-ssid").value.trim() === s.ssid) {
          $("hub-pass").value = pw;
          hubArm();
        }
      }).catch(() => {});
    }
    if (s.acctUser && !$("hub-acct-pass").value) {
      secretStore.get(secretStore.key("hub", "account")).then((rawAcct) => {
        if (!rawAcct || $("hub-acct-pass").value) return;
        const acct = JSON.parse(rawAcct);
        if (acct && acct.username === s.acctUser && acct.password) {
          $("hub-acct-pass").value = acct.password;
          $("hub-acct-pass2").value = acct.password;
          hubValidateAccount();
          hubArm();
        }
      }).catch(() => {});
    }
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
  // Leave a breadcrumb so a restart/quit mid-first-boot can resume the watch —
  // including whether this app still owes the hub its self-setup run.
  hubRecordFlash(
    hub.lastReceipt && hub.lastReceipt.provision_seeded,
    $("hub-provision-pihole").checked,
    hub.pendingAccount
  );
  const panel = $("hub-firstboot");
  panel.classList.remove("hidden");
  $("hub-fb-dot").className = "dot reading";
  $("hub-fb-text").textContent =
    (hub.flashedViaPi
      ? "Unplug the USB-C cable, then plug your Pi into its own power supply — it stays a plain USB disk until that power-cycle. Watching for it to come online."
      : "Put the card in your Pi and power it on — watching for it to come online.") +
    " No monitor or keyboard needed — the hub announces itself on your network by itself." +
    " First boot takes 10–20 minutes while it sets itself up; the blinking light is normal. Leave it powered if you can (a power cut usually just delays it — worst case is re-flashing). You can walk away, we'll ping you.";
  $("hub-fb-open").classList.add("hidden");
  hubRenderQr();
  hubStopFirstBoot(true); // clear any prior timer without hiding the panel
  const t0 = Date.now();
  hub.fbCountStop = hubCountdownStart($("hub-fb-count"), t0);
  let escalated = false;
  const tick = async () => {
    let up = false;
    try {
      up = await invoke("hub_probe_hub", { host: HUB_HOST });
    } catch (_) {}
    // Past the honest first-boot window with nothing heard: swap "be patient"
    // for the go-find-it checklist (see hubFindItChecklist).
    if (!up && !escalated && Date.now() - t0 > HUB_FB_ESCALATE_MS) {
      escalated = true;
      $("hub-fb-text").textContent = hubFindItChecklist();
    }
    if (up) {
      hubStopFirstBoot(true);
      $("hub-fb-dot").className = "dot connected";
      const openBtn = $("hub-fb-open");
      openBtn.classList.remove("hidden");
      openBtn.classList.remove("alive-pop");
      void openBtn.offsetWidth;
      openBtn.classList.add("alive-pop");
      const wantAccount = !!(hub.pendingAccount && hub.pendingAccount.password);
      const wantSetup = hub.lastReceipt && hub.lastReceipt.provision_seeded;
      // The account comes FIRST: being left at Home Assistant's sign-in or
      // wizard page is the failure this whole flow exists to prevent, and the
      // add-on installs below don't depend on it — but the user does.
      if (wantAccount) {
        $("hub-fb-text").textContent =
          "It's alive! Creating your Home Assistant account on the hub…";
        hubNotify("Your hub is ready", "Finishing your account automatically — no setup wizard for you.");
        await hubRunOnboarding($("hub-fb-text"));
      }
      if (wantSetup) {
        // The hub is up and the card carries the self-setup bundle — this is
        // the moment the whole option exists for. Run it now; the record is
        // cleared only once setup actually finished, so a quit mid-run still
        // resumes into "run self-setup" rather than losing the thread.
        $("hub-fb-text").textContent =
          "Finishing setup from this computer — broker, MQTT, Frigate, securaCV…";
        if (!wantAccount) {
          hubNotify("Your hub is ready", "Finishing setup automatically — no monitor needed.");
        }
        const report = await hubRunHeadlessSetup($("hub-fb-text"), $("hub-fb-setup"));
        if (report && report.ok) hubClearFlashRecord();
        else $("hub-fb-host").classList.remove("hidden");
      } else if (wantAccount) {
        // hubRunOnboarding already wrote the outcome into the status line.
        hubChime();
        hubClearFlashRecord();
      } else {
        $("hub-fb-text").textContent = "It's alive! Your hub is up. 🐤";
        hubNotify("Your hub is ready", "Open " + HUB_HOST + " to log in.");
        hubChime();
        hubClearFlashRecord(); // it's up — nothing left to resume
      }
    }
  };
  hub.fbTimer = setInterval(tick, 5000);
  tick();
}

// ── finishing Home Assistant's own first-run setup ──────────────────────────
// The card carries a hashed `.storage` head start, but the promise is kept
// HERE: once the hub answers, this app drives Home Assistant's own onboarding
// API to done — create the owner account, finish the wizard's remaining pages,
// and VERIFY the login opens the hub. It reads the hub's real state first and
// only does what's missing, so it converges no matter what actually happened
// on first boot: the seed applied (verify and stop), it didn't (do it all), a
// previous run died partway, or someone clicked ahead in a browser. Nobody is
// ever left at a sign-in page holding credentials the hub has never heard of.
async function hubRunOnboarding(statusEl, hostOverride) {
  const acct = hub.pendingAccount;
  if (!acct || !acct.password) return null;
  // Unlike the ssh path, onboarding wants the web port — the backend adds
  // :8123 itself when a cleaned host doesn't carry one.
  const host = hubCleanHost(hostOverride) || HUB_HOST;
  const attempts = 5;
  for (let attempt = 1; attempt <= attempts; attempt++) {
    try {
      const report = await invoke("hub_onboard", {
        host,
        name: acct.name,
        username: acct.username,
        password: acct.password,
      });
      // Any report is a definitive answer from the hub — the password's job
      // is done either way, so drop it from memory now.
      hub.pendingAccount = null;
      if (report.ok) {
        hub.onboardNote = null;
        statusEl.textContent =
          "Your Home Assistant account is ready — the login is checked and works. " +
          "Open http://" + HUB_HOST + " and sign in any time. 🐤";
      } else {
        hub.onboardNote = report.note ||
          "Home Assistant still has a setup step waiting in the browser.";
        statusEl.textContent = hub.onboardNote;
      }
      return report;
    } catch (e) {
      // Unreachable / not ready — HA can answer its front page moments before
      // its API is up. Patience beats an error here.
      if (attempt === attempts) {
        hub.onboardNote =
          "Couldn't finish your Home Assistant account automatically (" + e + "). " +
          "Nothing is lost: open the hub and its wizard walks you through the same steps.";
        statusEl.textContent = hub.onboardNote;
        return null;
      }
      await new Promise((r) => setTimeout(r, 6000));
    }
  }
  return null;
}

// ── self-setup over the hub's service console ───────────────────────────────
// The card carried the bundle; the hub is answering; now this computer is the
// "other screen" that finishes the job: it connects to the hub's service
// console (port 22222, unlocked by the maintenance key made at flash time) and
// runs the bundle's own host_provision.sh — Mosquitto, the MQTT integration,
// Frigate, securaCV, each step narrated in the console below. Idempotent on
// the hub side, so retrying after any stumble is always safe.
// A pasted address arrives however the user found it — "http://10.0.0.5:8123",
// "homeassistant.local/", a bare IP. Reduce it to the bare host the console
// connection needs; the backend validates it again.
function hubCleanHost(raw) {
  return (raw || "").trim().replace(/^https?:\/\//i, "").replace(/[/:].*$/, "");
}

async function hubRunHeadlessSetup(statusEl, retryBtn, hostOverride, piholeOverride) {
  if (hub.headlessBusy) return null;
  hub.headlessBusy = true;
  const el = $("hub-console");
  el.classList.remove("hidden");
  el.textContent += "\n— finishing hub setup from this computer —\n";
  el.scrollTop = el.scrollHeight;
  try {
    // Console port is fixed; strip :8123 from the probe host. An override (an
    // IP found in the router when mDNS is blocked) wins when given.
    const host = hubCleanHost(hostOverride) || HUB_HOST.replace(/:\d+$/, "");
    // A resumed run passes the choice recorded at flash time; only a live,
    // in-session run reads the checkbox.
    const withPihole =
      piholeOverride === undefined ? !!$("hub-provision-pihole").checked : !!piholeOverride;
    const report = await invoke("hub_headless_setup", { host, dryRun: false, withPihole });
    if (report.ok) {
      statusEl.textContent =
        "Setup finished — Mosquitto, MQTT, Frigate, and securaCV are installed" +
        (withPihole
          ? ", plus Pi-hole. To switch Pi-hole on, point your router's DNS at the hub's IP — until then it sits idle."
          : ".") +
        " Open your hub and the SecuraCV panel is waiting. 🐤" +
        // A clean install must not bury an account that still needs a human —
        // carry the onboarding run's note over the finish line.
        (hub.onboardNote ? " One thing about your account: " + hub.onboardNote : "");
      hubNotify(
        "Hub setup finished",
        "Broker, MQTT, Frigate, and securaCV are installed — open " + HUB_HOST + "."
      );
      hubChime();
      logEvent("ok", "Hub self-setup finished over the service console");
    } else {
      statusEl.textContent =
        (report.note || "The setup run didn't finish.") + " Details are in the console below.";
      if (retryBtn) retryBtn.classList.remove("hidden");
      logEvent("err", "Hub self-setup didn't finish (safe to retry)");
    }
    return report;
  } catch (e) {
    statusEl.textContent = "Couldn't run self-setup: " + e + " — it's safe to try again.";
    if (retryBtn) retryBtn.classList.remove("hidden");
    return null;
  } finally {
    hub.headlessBusy = false;
  }
}

function hubStopFirstBoot(keepPanel) {
  if (hub.fbTimer) {
    clearInterval(hub.fbTimer);
    hub.fbTimer = null;
  }
  if (hub.fbCountStop) {
    hub.fbCountStop();
    hub.fbCountStop = null;
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
  // Typed Wi-Fi + "wired ethernet" selected is a contradiction, and the old
  // behavior resolved it by silently throwing the Wi-Fi away: hubWifiValue()
  // returns null, the backend logs "no Wi-Fi to seed (wired ethernet
  // assumed)", and the hub boots with no network. Someone who typed a
  // password meant it — say which one is winning instead of picking one.
  const typedWifi = !!($("hub-ssid").value.trim() || $("hub-pass").value);
  const contradiction = $("hub-ethernet").checked && typedWifi;
  if (contradiction) {
    setStatus(
      "hub-result",
      "You've typed Wi-Fi but chosen wired ethernet — the Wi-Fi would be dropped. " +
        "Untick ethernet to use the Wi-Fi, or clear the fields to go wired.",
      "err",
    );
  }
  $("hub-flash-btn").disabled = !armed || contradiction;
  $("hub-write-summary").textContent = target
    ? `${hub.plan ? hub.plan.os_label : "Home Assistant OS"} → ${target.model} ` +
      `(${target.path}, ${hubFmtBytes(target.size_bytes)})` +
      (wifi === null ? " · wired ethernet" : "") +
      (hub.accountRequested && hub.accountValid ? " · account pre-made" : "") +
      ($("hub-provision").checked ? " · self-setup" : "")
    : "Pick a disk above first.";
}

async function hubFlash() {
  const target = hub.targets.find((t) => t.path === hub.selected);
  if (!target || hub.busy) return;
  hubStopFirstBoot(); // a fresh flash supersedes any prior first-boot watch
  // Remember HOW this flash reaches the Pi — the "what now" steps differ
  // between a card in a reader and the Pi itself acting as a USB disk.
  hub.flashedViaPi = /rpi[-_ ]?msd/i.test(target.model);
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
    // Keep the typed account in memory for the first-boot watch: the card gets
    // a hashed head start, but the PROMISE is kept post-boot, when this app
    // finishes Home Assistant's own onboarding with these exact credentials.
    hub.pendingAccount = hubAccountValue();
    hub.onboardNote = null;
    const receipt = await invoke("hub_flash", {
      boardId: hub.boardId || (hub.plan ? hub.plan.board_id : "rpi5-64"),
      diskPath: target.path,
      confirmed: true,
      wifi: hubWifiValue(),
      account: hub.pendingAccount,
      provision: $("hub-provision").checked,
    });
    hub.done = true;
    hub.lastReceipt = receipt;
    setStatus("hub-result", "Done — written, read back, and verified.", "ok");
    logEvent("ok", "Home Assistant hub written to " + target.model);
    // The hub belongs in the fleet book too — it's the meeting point every
    // Canary's broker field defaults to, and its row answers "is it up?".
    try {
      bookUpsert({
        deviceId: "hub",
        productLabel: "Home Assistant hub",
        role: "hub",
        flashedAt: Date.now(),
      });
    } catch (_) {}
    hubShowHatch(receipt);
    hubNotify(
      "Your card is ready",
      hub.flashedViaPi
        ? "Unplug the USB-C cable, then power your Pi from its own supply — the app will tell you when it's online."
        : "Now boot it in your Pi — the app will tell you when it's online."
    );
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
  // Wi-Fi that was asked for and couldn't be placed now fails the flash before
  // the card is touched, so reaching this screen without it means none was
  // asked for. There is no "seeded but maybe not" state left to explain.
  const wifiLine = receipt.wifi_seeded
    ? " Your Wi-Fi went into the image before the write, so it's covered by the same verification."
    : " Plug in ethernet before you power it on.";
  const acctLine = receipt.account_note ? " " + receipt.account_note : "";
  const provLine = receipt.provision_note ? " " + receipt.provision_note : "";
  const cacheLine = receipt.used_cache ? " (reused your verified local copy — no re-download.)" : "";
  // Shown whenever present. Advice rather than a warning now: nothing is
  // written to the card after the verified write, so a card that wouldn't
  // auto-eject has nothing pending to lose.
  const ejectLine = receipt.eject_note ? " ⚠ " + receipt.eject_note : "";
  $("hub-hatch-body").textContent =
    `${receipt.os_label} is on ${receipt.target_path} — every byte read back and matched ` +
    `(SHA-256 ${receipt.sha256.slice(0, 16)}…).${cacheLine}${wifiLine}${acctLine}${provLine}${ejectLine}`;
  // With an account typed at flash time, the app itself finishes Home
  // Assistant's onboarding when the hub answers — the step below promises
  // exactly that, not the card seed (which is only a head start).
  const accountTyped = !!hub.pendingAccount;
  // The first step depends on HOW we reached the Pi's storage: a card in a
  // reader moves to the Pi; the Pi-over-USB-C path has nothing to move — but
  // it stays a plain USB disk until it's power-cycled onto its own supply.
  const bootStep = hub.flashedViaPi
    ? "Nothing to move — the storage we just wrote is already inside your Pi. It's still in " +
      "“act as a disk” mode though, so it won't start on its own: unplug the USB-C cable " +
      "from this computer, then plug the Pi into its normal power supply. That power-cycle " +
      "is what starts the first boot."
    : "Take the card out of the reader and put it in your Raspberry Pi (or connect the SSD), " +
      "then power it on.";
  // With the self-setup seed on the card, the broker/MQTT installs below are
  // not the user's job — this app does them the moment the hub answers.
  const selfSetup = receipt.provision_seeded;
  const steps = [
    bootStep,
    "First boot usually takes under 10 minutes while Home Assistant unpacks and downloads " +
      "itself, though a slow card can stretch it to 20 — LED activity is it working, not a " +
      "problem. Best to leave it plugged in for those minutes; if power does get cut, it " +
      "nearly always recovers on the next boot, and the true worst case is simply " +
      "re-flashing this card.",
    ...(selfSetup
      ? [
          "Keep this app open. The moment the hub answers, this app connects to its service " +
            "console and installs everything itself — Mosquitto broker, the MQTT connection, " +
            "Frigate, and securaCV" +
            ($("hub-provision-pihole").checked ? ", plus Pi-hole" : "") +
            " — narrated in the console below. Nothing to click on the hub.",
        ]
      : []),
    ...(selfSetup && $("hub-provision-pihole").checked
      ? [
          "When setup finishes, switch Pi-hole on by pointing your router's DNS at the hub's " +
            "IP (in the router's DHCP settings). From then on, Pi-hole's page shows every " +
            "domain every device on your network asks for — the way to see that nothing, " +
            "Canaries included, is quietly talking out — and known ad/tracker domains are " +
            "refused for the whole house. Until the router change it sits idle.",
        ]
      : []),
    accountTyped
      ? "Keep this app open through first boot: the moment the hub answers, this app creates " +
        "your Home Assistant account on it and checks the login actually works — no setup " +
        `wizard for you. Then open http://${HUB_HOST} and sign in with the details you typed.`
      : `Open http://${HUB_HOST} on any device in your home and create your account.`,
    ...(selfSetup
      ? []
      : [
          "Once you're in, give your Canaries their meeting point: in Home Assistant go to " +
            "Settings → Apps → Install app, choose “Mosquitto broker”, and press Start. When " +
            "Home Assistant then offers to set up the newly discovered “MQTT” integration, accept " +
            "with the defaults — they're exactly right for Canaries (the broker lives on the hub " +
            "itself, port 1883).",
        ]),
    // Which login the Canaries use depends on who made it. When this app
    // provisioned the hub, the plan already minted a broker-local “canary”
    // account — telling someone to ALSO create a Home Assistant user would hand
    // them a second, different credential and no way to know which one the
    // devices want. When they set the hub up by hand, a Home Assistant user is
    // the practical answer: editing the broker's own `logins` means hand-editing
    // the add-on's configuration, which the rest of this guide tells them to
    // leave alone.
    selfSetup
      ? "Your Canaries' login was made for you during setup: the broker refuses anonymous " +
        "connections, so the hub minted a “canary” account with a password of its own. Read " +
        "that password in Home Assistant under Settings → Apps → Mosquitto broker → " +
        "Configuration → Logins, and type the pair into the MQTT fields when you flash each " +
        "Canary. It can only speak MQTT — it is not a Home Assistant account and cannot act " +
        "as one."
      : "Now make the login your Canaries will use: the broker refuses anonymous connections, " +
        "and Home Assistant's own reserved accounts don't apply to them. Turn on Advanced Mode " +
        "in your profile, then Settings → People → Users → Add user — “securacv” and a " +
        "password you'll reuse on each device. It needs no administrator rights. Type that " +
        "same pair into the MQTT fields when you flash each Canary.",
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

// Wi-Fi / broker secrets are masked TEXT inputs (see index.html): the Show
// toggle flips the masking class, NEVER the input type — flipping to
// type="password" re-summons the OS's "invent a strong password" sheet for a
// key the router already owns (firmware/LESSONS_LEARNED.md). "Use saved"
// fills the field from the OS's own Wi-Fi store (Keychain / NetworkManager),
// behind the system's consent prompt, on an explicit click only.
function pwInit() {
  document.querySelectorAll(".pw-toggle").forEach((btn) => {
    btn.addEventListener("click", () => {
      const input = $(btn.dataset.pw);
      if (!input) return;
      const masked = input.classList.toggle("pw-masked");
      btn.textContent = masked ? "Show" : "Hide";
      btn.setAttribute("aria-label", masked ? "Show the password" : "Hide the password");
    });
  });
  document.querySelectorAll(".pw-fetch").forEach((btn) => {
    const idle = btn.textContent;
    let restore = null;
    const note = (text) => {
      btn.textContent = text;
      clearTimeout(restore);
      restore = setTimeout(() => { btn.textContent = idle; }, 4000);
    };
    btn.addEventListener("click", async () => {
      const pass = $(btn.dataset.pw);
      const ssidField = $(btn.dataset.ssid);
      const ssid = ssidField ? ssidField.value.trim() : "";
      if (!pass) return;
      if (!ssid) {
        note("type the Wi-Fi name first");
        ssidField?.focus();
        return;
      }
      btn.disabled = true;
      btn.textContent = "asking the system…";
      try {
        const pw = await invoke("saved_wifi_password", { ssid });
        pass.value = pw;
        // Fire the same event typing would, so arming/validation listeners see it.
        pass.dispatchEvent(new Event("input", { bubbles: true }));
        clearTimeout(restore);
        btn.textContent = idle;
      } catch (e) {
        note(String(e));
      } finally {
        btn.disabled = false;
      }
    });
  });
}

window.addEventListener("DOMContentLoaded", () => {
  boot();
  hubInit();
  pwInit();
});
