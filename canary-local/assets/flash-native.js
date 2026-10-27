// canary-local/assets/flash-native.js — the Lab app's native USB flash path.
//
// Inside the SecuraCV Lab desktop app the page runs in the OS webview, which
// has no Web Serial — so the browser flasher (flash.js) could only say "get
// the Flasher app", in the app that exists to make that detour unnecessary.
// When the native shell reports `native_capabilities().serial`, flash.js
// mounts this bench instead: the ports come from the OS (`list_ports`), the
// board is read and written by the Lab's bundled espflash through the SAME
// commands the SecuraCV Flasher uses (`detect_chip`, `fetch_manifest`,
// `flash`, `start_serial_monitor`, `serial_monitor_send` — desktop/flash-engine,
// shared by both apps), and the same events stream back (`flash:log`, `flash:progress`,
// `serial:status`, `serial:log`, `serial:receipt`; `flash:changemap` is drawn
// from a safety copy, which this bench doesn't take, so it never comes).
//
// Two flashers, two frontends (CLAUDE.md): every diagnostic the Flasher's
// frontend (desktop/src/app.js) gives on this path is given here too — the
// Linux port hints the backend names, the chip-guard and unbundled-manifest
// refusals, espflash's tail classified into a named failure with its fix,
// the gentler-speed retry for transport faults, and the "pinned to a release
// that isn't cut" answer. canary-local/tests/desktop_parity.test.js holds the
// classifier below to app.js's by text and runs both over espflash's words.
//
// What this bench does NOT carry from the Flasher: the automatic full-chip
// safety copy (and so the change map drawn from it), room presets (detection
// dials / radar reflexes — it sends the engine empty dials, which writes no
// dial keys), the minted API token and fleet book (the Lab has no secret
// drawer, so the firmware derives its own token as always), the rescue
// bench, the local-file install and the Vision module burn. Those stay in the
// Flasher; the card says so, and says where.

import * as core from "./flash-core.js";

const tauri = () =>
  typeof window !== "undefined" && window.__TAURI__ && window.__TAURI__.core ? window.__TAURI__ : null;
const invoke = (cmd, args) => tauri().core.invoke(cmd, args);
const listen = (evt, cb) =>
  tauri().event ? tauri().event.listen(evt, cb) : Promise.resolve(() => {});

const el = (tag, cls, text) => {
  const n = document.createElement(tag);
  if (cls) n.className = cls;
  if (text != null) n.textContent = text;
  return n;
};

/** The native shell's capabilities, or null when this page is not running in
    the Lab app (the website, a plain browser). `serial` is the one this bench
    needs: true only on a desktop build that bundles espflash. */
export async function probeNative() {
  if (!tauri()) return null;
  try {
    return (await invoke("native_capabilities")) || {};
  } catch {
    return {}; // an app without the capability seam flashes nothing natively
  }
}

/** In the Lab app, but no native flashing here (the iPhone/iPad shell, or a
    desktop build without the bundled flash engine). MOBILE.md: the Flash bench
    is hidden there and points to the desktop app instead — never the
    website's "get Chrome" card, which no app user can act on. */
export function renderNativeUnavailable(caps) {
  const box = el("section", "flash-card flash-unsupported");
  box.append(el("h2", null, "Flashing over USB isn’t available on this device"));
  box.append(el("p", "muted", caps && caps.serial_list
    // A desktop build that lists ports but ships no flash engine (the Lab
    // bundles espflash for macOS and Linux only).
    ? "This build of the Lab doesn’t carry the flash engine — it ships with the macOS " +
      "and Linux apps. Everything else in the Lab works as usual."
    : "iPhone and iPad don’t let apps talk to USB serial boards, so the Lab can’t reach " +
      "a Canary’s port from here. Everything else in the Lab works as usual."));
  box.append(el("p", "fineprint",
    "To flash a Canary, use the SecuraCV Lab or the SecuraCV Flasher on a Mac or " +
    "Linux computer (securacv.com/download) — plug the board in over USB-C and it " +
    "takes it from there."));
  return box;
}

// ── flash-failure classification — the Flasher's own, verbatim ─────────────
// desktop/src/app.js classifyFlashError: the browser's kinds and fixes with
// the match patterns widened for espflash and serialport-rs phrasings. The
// native path hands us espflash's words (the backend keeps its last lines in
// the error), so this — not flash-core.js's Web Serial classifier — is the
// one that names them. desktop_parity.test.js holds the two copies equal.
function classifyFlashError(err) {
  const msg = String((err && (err.message || err.name)) || err || "").toLowerCase();
  const has = (...subs) => subs.some((s) => msg.includes(s));

  // The bundled espflash never started: the app's own engine failed, not the
  // board (flash-engine sidecar.rs spawn_error, "could not start espflash: …";
  // each app's host says "bundled espflash missing: …" when the sidecar can't
  // even be resolved). Checked FIRST: the OS words such a failure in
  // port-shaped terms ("Permission denied" for a file without its execute
  // bit, "No such file or directory" for a missing loader), and the checks
  // below would read those as the port's and coach dialout or download mode
  // at a board that was never the problem.
  if (has("could not start espflash", "bundled espflash missing"))
    return { kind: "engine", title: "The app's flash engine couldn't start",
      hint: "That's this app, not your board — the espflash engine it bundles wouldn't " +
        "run on this computer, so download mode won't help. Install the newest release " +
        "for this computer from securacv.com/download (the Mac download is universal, " +
        "Apple Silicon and Intel; the Linux one is for x86-64 PCs). If the newest one " +
        "fails the same way, please report it, with the reason the system gave." };

  // An image refused before the write because it has no bootloader for this
  // chip where its ROM loads one (flash-engine image.rs
  // check_bootloader_offset; the browser's bootloaderPlacement says the same
  // words). No retry, cable or download mode changes what an image keeps
  // there.
  if (has("would not start on the"))
    return { kind: "layout", title: "That image wouldn't start on this board",
      hint: "Nothing was written. It has no bootloader for this chip where the chip " +
        "loads one from, so the board couldn't start from it (the details say where). " +
        "Use an image laid out for this chip: SecuraCV factory images from fw-v2.4.18 on are." };

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
  if (has("checksum", "sha-256", "sha256", "md5", "hash mismatch", "failed its", "signature", "signed by"))
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

// The system's own reason a bundled espflash didn't start, out of the
// backend's one-line spawn error (flash-engine sidecar.rs spawn_error keeps
// the raw OS error last, after any hint): its "(os error N)" clause when it
// has one, else everything after the first colon.
function spawnReason(line) {
  const os = /[^():]*\(os error \d+\)/.exec(line);
  return (os ? os[0] : line.slice(line.indexOf(":") + 1)).trim();
}

// The one sentence of that classifier's download advice this bench can't
// honor: the local-file install lives in the Flasher, not here.
const withoutLocalFile = (hint) =>
  hint.replace(" You can also install a local .bin under Advanced.",
    " A local .bin can be installed with the SecuraCV Flasher.");

// Who checked what, said apart. The APP checked the release before a byte was
// written (flash-engine release.rs: its SHA-256, and its Ed25519 signature
// against the key pinned in the catalog); espflash and the chip only confirmed
// the write landed. "Verified" is kept for the signature check alone — the
// engine reports `checksum-only` when no release key is pinned, and that is
// said as a checksum, never as verified.
const releaseCheck = (how) => how === "ed25519+sha256"
  ? "release signature verified against the pinned key (ed25519+sha256)"
  : "release SHA-256 matched — no release key is pinned, so no signature was checked";

// Only these failures are worth a slower retry — the Flasher's rule
// (app.js BAUD_RETRY_KINDS). A bad image, a busy port or a refused permission
// fails identically at every speed; `unknown` is excluded because retrying it
// would walk a busy port down the whole ladder, repeating the download and
// the full-chip erase each time.
const BAUD_RETRY_KINDS = new Set(["not-in-download", "read-stall", "device-lost"]);

// The backend's own first line names an OS-level cause on Linux (port_hint.rs
// PERMISSION_HINT / BUSY_HINT): say it, and don't coach BOOT/RESET — the
// Flasher's identify() makes the same call.
const OS_LEVEL_RE = /Linux blocked opening|holding the board's serial port/i;

// The Flasher's rule for which firmwares prove themselves with a live boot
// receipt (app.js requiresLiveReceipt): the generated catalog says so from
// the product's real serial commands, and a missing field fails closed.
const requiresLiveReceipt = (product) => !product || product.serial_receipt !== false;

/** Mount the native bench. `renderWifiFields(box, product)` is flash.js's own
    Wi-Fi / broker form — the browser's form, reused, so a Canary set up here
    gets exactly the fields (and the saved network) it gets in the browser.
    The post-flash monitor's command chips are the flashed product's own, from
    the `catalog` handed in (products[].monitor through flash-core.js
    monitorVocabulary — sweep A86): the record the browser flasher's monitor
    and the Flasher read too, held to each firmware's command handler by
    gen_flash.py. */
export async function mountNativeBench(mount, { catalog, renderWifiFields }) {
  const st = {
    ports: [], port: null, portInfo: null, chip: null, flashBytes: null, mac: null,
    macCheck: null, detecting: false, failedPort: null, busy: false,
    devChannel: false, manifest: null, manifestError: null, product: null,
    baudCeiling: null, monitoring: false, pollTimer: null,
  };
  const manifestUrl = () => (st.devChannel ? core.DEV_FLASH_MANIFEST_URL : catalog.manifest_url);
  // The monitor's command row (its chips, the typed command and Send) is
  // usable exactly while the monitor runs, the Flasher's setMonitorButtons:
  // every place the bench takes the port back or the monitor ends goes
  // through here, so a stopped monitor never leaves a live-looking Send.
  let monitorControls = [];
  const setMonitoring = (on) => {
    st.monitoring = on;
    for (const c of monitorControls) c.disabled = !on;
  };
  // The receipt card's serial:status / serial:log / serial:receipt listeners
  // belong to that card alone (sweep A85). A new flash, a new card and a
  // re-read each let go of the last card's three, or every further flash in
  // one session leaves another set writing into a card that is no longer on
  // the page (and a failed reflash, which draws no card, leaves them
  // listening to a monitor that is not running).
  let receiptCard = null; // { unlisteners: [] } while a card is drawn
  const releaseReceipt = (card = receiptCard) => {
    if (!card) return;
    for (const u of card.unlisteners.splice(0)) u();
    if (receiptCard === card) receiptCard = null;
  };

  // ── the cards ────────────────────────────────────────────────────────────
  const head = el("section", "flash-card");
  head.append(el("h2", null, "Flash over USB — right here in the Lab"));
  head.append(el("p", "muted",
    "The Lab app flashes with its own bundled espflash engine — no browser, no Web " +
    "Serial. It is the SecuraCV Flasher’s engine: the board is only ever offered " +
    "firmware built for its chip, every image is checked against the signed " +
    "release before a byte is written, and you can’t brick it — the ESP32’s " +
    "bootloader is in ROM."));
  const conn = el("p", "flash-stage", "Scanning for a Canary — plug one in over USB-C.");
  const portRow = el("div", "flash-row flash-hidden");
  const portSel = el("select");
  portSel.setAttribute("aria-label", "Serial port");
  portRow.append(portSel);
  const recheck = el("button", "ghost flash-hidden", "Read the board again");
  const note = el("p", "flash-note flash-note-soft flash-hidden");
  head.append(conn, portRow, recheck, note);
  head.append(el("p", "fineprint",
    "The automatic safety copy, room presets, the fleet book, the rescue bench, " +
    "local-file installs and the Vision camera module live in the SecuraCV Flasher " +
    "(securacv.com/download)."));

  const pick = el("section", "flash-card flash-hidden");
  const flashCard = el("section", "flash-card flash-hidden");
  const result = el("section", "flash-card flash-hidden");
  mount.append(head, pick, flashCard, result);

  const setConn = (text) => { conn.textContent = text; };
  const showNote = (text) => {
    note.textContent = text || "";
    note.classList.toggle("flash-hidden", !text);
  };

  // ── ports: the OS's list, polled like the Flasher's watcher ──────────────
  const syncPortSelect = (usb) => {
    const names = usb.map((p) => p.name).join("\n");
    if (portSel.dataset.names !== names) {
      portSel.dataset.names = names;
      portSel.innerHTML = "";
      for (const p of usb) {
        const o = el("option", null, p.product ? `${p.name} — ${p.product}` : p.name);
        o.value = p.name;
        portSel.append(o);
      }
      if (st.port && usb.some((p) => p.name === st.port)) portSel.value = st.port;
    }
    portRow.classList.toggle("flash-hidden", usb.length < 2);
  };

  const resetBoard = () => {
    st.chip = null; st.flashBytes = null; st.mac = null; st.macCheck = null;
    st.product = null; st.failedPort = null;
    pick.classList.add("flash-hidden");
    flashCard.classList.add("flash-hidden");
    recheck.classList.add("flash-hidden");
    showNote("");
  };

  const pollPorts = async () => {
    if (st.busy || st.monitoring) return; // a flash or the monitor owns the port
    let ports;
    try { ports = await invoke("list_ports"); } catch { return; } // transient
    const usb = (ports || []).filter((p) => p.kind === "usb");
    st.ports = usb;
    syncPortSelect(usb);
    if (!usb.length) {
      if (st.port) { st.port = null; st.portInfo = null; resetBoard(); }
      setConn("Scanning for a Canary — plug one in over USB-C.");
      return;
    }
    const chosen = (usb.length > 1 && portSel.value && usb.find((p) => p.name === portSel.value)) || usb[0];
    if (chosen.name !== st.port) {
      st.port = chosen.name;
      resetBoard();
    }
    st.portInfo = chosen;
    if (st.chip || st.detecting || st.failedPort === st.port) return;
    await identify(chosen);
  };

  // The Grove Vision AI V2 camera module enumerates as its own USB port (the
  // catalog's we2_module USB id). It is not an ESP32 — reading it with
  // board-info would fail and coach download mode at a board that has none.
  // The Flasher recognizes it by the same id before it reads anything.
  const we2 = catalog.we2_module || {};
  const hexId = (v) => parseInt(String(v || "").replace(/^0x/i, ""), 16);
  const isVisionModule = (p) =>
    p.vid != null && p.pid != null && Number(p.vid) === hexId(we2.usb_vid) && Number(p.pid) === hexId(we2.usb_pid);

  // ── read the board: detect_chip, exactly as the Flasher asks ─────────────
  const identify = async (portInfo) => {
    const port = portInfo.name;
    const label = portInfo.product ? `${port} (${portInfo.product})` : port;
    if (isVisionModule(portInfo)) {
      st.failedPort = port; // nothing to read here; wait for a replug
      setConn(`Found ${port} — that’s the Grove Vision AI V2 camera module. Its model ` +
        "is loaded with the SecuraCV Flasher (securacv.com/download); plug the Canary " +
        "itself in here to flash its firmware.");
      return;
    }
    st.detecting = true;
    setConn(`Found ${label} — reading chip…`);
    showNote("");
    try {
      const info = await invoke("detect_chip", { port });
      if (port !== st.port) return; // unplugged or switched while reading
      st.chip = info.chip;
      st.flashBytes = info.flash_bytes;
      st.mac = info.mac;
      st.macCheck = info.mac_check || null;
      const size = st.flashBytes ? ` · ${Math.round(st.flashBytes / (1024 * 1024))} MB flash` : "";
      setConn(`Connected · ${info.chip}${size} on ${port}`);
      if (st.macCheck && st.macCheck.level && st.macCheck.level !== "clear") {
        showNote(`${st.macCheck.label}${st.macCheck.detail ? " — " + st.macCheck.detail : ""}`);
      }
      recheck.classList.remove("flash-hidden");
      await refreshManifest();
      renderPick();
    } catch (e) {
      if (port !== st.port) return;
      st.failedPort = port;
      // The Flasher's call, word for word in effect: an OS-level cause the
      // backend named (Linux permission / ModemManager) is said as-is; a
      // named failure gets its title and fix; only a nameless one coaches
      // download mode — for that one it IS the fix.
      const firstLine = String(e).split("\n")[0].trim();
      const osLevel = OS_LEVEL_RE.test(firstLine);
      const c = classifyFlashError(e);
      const named = !osLevel && c.kind !== "unknown" && c.kind !== "not-in-download";
      // The Flasher's rule for a bundled espflash that never started: the
      // app's failure, said with the system's reason and no driver note.
      const engine = c.kind === "engine";
      const reason = engine ? ` (The system said: ${spawnReason(firstLine)})` : "";
      const bridge = core.usbBridgeInfo(portInfo.vid, portInfo.pid);
      const bridgeNote = !osLevel && !engine && bridge ? " " + bridge.note : "";
      setConn(osLevel ? `Found ${port} — ${firstLine}`
        : named ? `Found ${port} — ${c.title}. ${withoutLocalFile(c.hint)}${reason}${bridgeNote}`
          : `Found ${port} — couldn't read the chip. Put it in download mode ` +
            `(hold BOOT, tap RESET, release BOOT), then read it again.${bridgeNote}`);
      recheck.classList.remove("flash-hidden");
    } finally {
      st.detecting = false;
    }
  };

  recheck.addEventListener("click", async () => {
    if (st.busy) return;
    // The monitor owns the port after a flash; reading the board means
    // taking it back first.
    if (st.monitoring) {
      try { await invoke("stop_serial_monitor"); } catch { /* already stopped */ }
      setMonitoring(false);
    }
    releaseReceipt(); // the card's listeners go with the port it watched
    st.failedPort = null;
    st.chip = null;
    pollPorts();
  });
  portSel.addEventListener("change", () => { if (!st.busy) pollPorts(); });

  // ── the release: what is published for each product ─────────────────────
  const refreshManifest = async () => {
    st.manifest = null;
    st.manifestError = null;
    try {
      st.manifest = await invoke("fetch_manifest", { manifestUrl: manifestUrl() });
    } catch (e) {
      // The `HTTP <code>` token is the backend's contract (net.rs): a real
      // "that release has no manifest" answer, told apart from a transport
      // failure that proves nothing about whether the release exists.
      const text = String(e);
      const http = /HTTP (\d{3})/.exec(text);
      const tag = core.releaseTagFromManifestUrl(manifestUrl());
      st.manifestError = http
        ? (tag
          ? `This bench is pinned to firmware release ${tag} — no release manifest (HTTP ${http[1]}). ` +
            "Nothing can be flashed from it until that release is published."
          : `No published release on this channel yet (HTTP ${http[1]}).`)
        : `Couldn’t reach the release manifest — check your connection and read the board again. (${text})`;
    }
  };

  // ── pick: only products built for this silicon ───────────────────────────
  const renderPick = () => {
    pick.innerHTML = "";
    pick.classList.remove("flash-hidden");
    pick.append(el("h2", null, `Pick the firmware for this ${st.chip}`));
    const dev = el("label", "flash-row");
    const devChk = el("input");
    devChk.type = "checkbox";
    devChk.checked = st.devChannel;
    dev.append(devChk, document.createTextNode(
      " Dev channel — the rolling fw-dev-latest prerelease: usually the newest dev build, or " +
      "the stable one when none is newer (the note above says which version), checked exactly " +
      "the same way (chip guard, SHA-256, the release signature)"));
    devChk.addEventListener("change", async () => {
      if (st.busy) { devChk.checked = st.devChannel; return; }
      st.devChannel = devChk.checked;
      st.product = null;
      flashCard.classList.add("flash-hidden");
      await refreshManifest();
      renderPick();
    });
    // Loud on purpose, like the Flasher's banner: nobody should flash a
    // prerelease without a line saying so.
    if (st.devChannel) {
      // Where it stands is the two catalogs' fw_train, said by
      // core.devChannelStanding ("" until the dev manifest has loaded).
      const standing = core.devChannelStanding(st.manifest && st.manifest.fw_train, catalog.fw_train);
      pick.append(el("p", "flash-note",
        ["DEV CHANNEL — fw-dev-latest, not the pinned stable release.", standing,
          "Untick it below to go back."].filter(Boolean).join(" ")));
    }
    if (st.manifestError) pick.append(el("p", "flash-note", st.manifestError));
    const products = core.productsForChip(catalog, st.chip);
    if (!products.length) {
      pick.append(el("p", "muted",
        `No Canary firmware is built for the ${st.chip}. The catalog only offers images ` +
        "for the silicon they were compiled for — that guard is the point."));
    }
    const list = el("div", "flash-products");
    for (const p of products) {
      const entry = st.manifest && st.manifest.products && st.manifest.products[p.id];
      const fit = core.flashFitVerdict(p, st.flashBytes);
      const row = el("label", "flash-row");
      const radio = el("input");
      radio.type = "radio";
      radio.name = "native-product";
      radio.disabled = !entry || !fit.fits;
      radio.checked = st.product && st.product.id === p.id;
      const words = el("span", null, p.name);
      const sub = !fit.fits ? ` — ${fit.why}`
        : entry ? ` — v${entry.version}` : " — not in this release";
      words.append(el("span", "muted", sub));
      row.append(radio, words);
      radio.addEventListener("change", () => { st.product = p; renderFlash(); });
      list.append(row);
    }
    pick.append(list, dev);
  };

  // ── flash: the Flasher's invoke, the browser's form ──────────────────────
  const renderFlash = () => {
    const product = st.product;
    flashCard.innerHTML = "";
    result.classList.add("flash-hidden");
    if (!product) { flashCard.classList.add("flash-hidden"); return; }
    flashCard.classList.remove("flash-hidden");
    flashCard.append(el("h2", null, `Install ${product.name}`));
    // The browser's form, in a box of its own so it can be drawn afresh after
    // every attempt: onFlash clears the Wi-Fi password the moment it reads it
    // (never left in the DOM), and a retry from that cleared field would
    // provision — and remember — an empty password. A fresh draw pre-fills it
    // from the form's Wi-Fi memory, as the browser's own retry does.
    const formBox = el("div");
    flashCard.append(formBox);
    const form = { current: renderWifiFields(formBox, product) };
    const redrawForm = () => {
      formBox.innerHTML = "";
      form.current = renderWifiFields(formBox, product);
    };
    const eraseRow = el("label", "flash-row");
    const erase = el("input");
    erase.type = "checkbox";
    // The Flasher's default for a board of unknown provenance: wipe the
    // whole chip first, so nothing a previous owner left in an untouched
    // partition rides through. espflash can't report what's resident, so
    // it is the user's answer — defaulting to the safe one.
    erase.checked = true;
    eraseRow.append(erase, document.createTextNode(
      " First time I’m flashing this board — erase the whole chip first"));
    flashCard.append(eraseRow);
    const go = el("button", "primary", "Flash my Canary");
    flashCard.append(go);
    const stage = el("p", "flash-stage muted", "");
    const bar = el("div", "flash-bar flash-hidden");
    const fill = el("div", "flash-bar-fill");
    bar.append(fill);
    const log = el("details", "flash-log");
    log.append(el("summary", null, "show technical log"));
    const pre = el("pre");
    log.append(pre);
    flashCard.append(stage, bar, log);
    const line = (t) => { pre.textContent += t + "\n"; pre.scrollTop = pre.scrollHeight; };
    go.addEventListener("click", () => onFlash({ product, form, redrawForm, erase, go, stage, bar, fill, line }));
  };

  const baudLadder = () => {
    const top = catalog.flash_baud || 921600;
    const ceiling = st.baudCeiling || Infinity;
    const rungs = [top, ...core.FLASH_BAUDS.filter((b) => b !== top)]
      .filter((b) => b <= ceiling)
      .sort((a, b) => b - a);
    return rungs.length ? rungs : [core.FLASH_BAUDS[core.FLASH_BAUDS.length - 1]];
  };

  const withBaudLadder = async (fn, line) => {
    const rungs = baudLadder();
    let lastErr;
    for (let i = 0; i < rungs.length; i++) {
      if (i > 0) line(`→ trying a gentler speed (${rungs[i]})…`);
      try {
        const out = await fn(rungs[i]);
        st.baudCeiling = null; // it worked — stop handicapping later attempts
        return out;
      } catch (e) {
        lastErr = e;
        if (!BAUD_RETRY_KINDS.has(classifyFlashError(e).kind) || i === rungs.length - 1) throw e;
        st.baudCeiling = rungs[i + 1];
      }
    }
    throw lastErr;
  };

  const onFlash = async ({ product, form, redrawForm, erase, go, stage, bar, fill, line }) => {
    if (st.busy) return;
    const fit = core.flashFitVerdict(product, st.flashBytes);
    if (!fit.fits) { stage.textContent = `✗ ${fit.why}`; return; }
    const creds = form.current.credentials();
    if (!creds.ok) return; // the form already says what to fix
    // Never leave the password sitting in the DOM — cleared the moment it is
    // read, as flash.js does, so a failed flash (download mode, a busy port)
    // doesn't strand it there. The form is redrawn (pre-filled from its Wi-Fi
    // memory) once the attempt ends, success or not.
    form.current.clear();
    const mqtt = creds.mqtt || {};
    // The Flasher's provisioning object (app.js readProvisioning), from the
    // browser's form: identity + broker when this firmware reads them, the
    // network when one was typed, the OTA answer always.
    const provisioning = {
      deviceId: String(mqtt.deviceId || "").trim(),
      wifiSsid: creds.wifi ? creds.wifi.ssid : "",
      wifiPass: creds.wifi ? creds.wifi.pass : "",
      mqttHost: String(mqtt.mqttHost || "").trim(),
      mqttPort: Number(mqtt.mqttPort) || 1883,
      mqttUser: mqtt.mqttUser || "",
      mqttPass: mqtt.mqttPass || "",
      mqttTls: Number(mqtt.mqttTls) || 0,
      mqttCa: String(mqtt.mqttCa || "").trim(),
      mqttFp: mqtt.mqttFp || "",
      wifiNvs: product.wifi_nvs || "string",
      apiToken: "",
      dials: { u8: {}, u32: {} },
      autoUpdate: !!creds.autoUpdate,
    };
    st.busy = true;
    go.disabled = true;
    go.textContent = "Flashing…";
    result.classList.add("flash-hidden");
    stage.textContent = "Getting ready…";
    bar.classList.remove("flash-hidden");
    fill.style.width = "0%";
    const unlisten = await listen("flash:log", (ev) => {
      const t = String(ev.payload);
      line(t);
      if (/^[→✓✗⚠]/.test(t)) stage.textContent = t;
    });
    const unlistenProgress = await listen("flash:progress", (ev) => {
      const p = ev.payload || {};
      if (p.stage !== "download") return;
      if (p.total > 0) {
        stage.textContent = `Downloading the signed image — ${Math.round(p.done / 1024)} of ${Math.round(p.total / 1024)} KB…`;
        fill.style.width = `${Math.min(100, (100 * p.done) / p.total).toFixed(1)}%`;
      }
    });
    try { await invoke("stop_serial_monitor"); } catch { /* not running is fine */ }
    setMonitoring(false);
    releaseReceipt(); // the last card's monitor is over; its listeners go too
    try {
      const receipt = await withBaudLadder((baud) => invoke("flash", {
        port: st.port,
        productId: product.id,
        manifestUrl: manifestUrl(),
        baud,
        detectedChip: st.chip,
        provisioning,
        eraseFirst: !!erase.checked,
        // No safety copy on this bench, so no change map: absent, honestly.
        backupPath: "",
      }), line);
      fill.style.width = "100%";
      await renderReceipt(product, receipt);
    } catch (e) {
      const c = classifyFlashError(e);
      stage.textContent = (c.title ? c.title + ". " : "The flash didn’t finish. ") +
        withoutLocalFile(c.hint);
      line(`✗ ${e}`);
      result.innerHTML = "";
      result.classList.remove("flash-hidden");
      result.append(el("h2", null, c.title || "The flash didn’t finish"));
      result.append(el("p", "muted", withoutLocalFile(c.hint)));
      const details = el("details", "flash-log");
      details.append(el("summary", null, "Details"), el("pre", null, String(e)));
      result.append(details);
      // A layout refusal wrote nothing, and no retry or download mode changes
      // where an image keeps its bootloader: say that instead.
      result.append(el("p", "fineprint", c.kind === "layout"
        ? "Nothing was written — pick an image laid out for this chip."
        : "Nothing is bricked — the ESP32’s first-stage bootloader is in ROM. Put the board " +
          "back in download mode (hold BOOT, tap RESET, release BOOT) and try again."));
    } finally {
      unlisten();
      unlistenProgress();
      redrawForm();
      go.disabled = false;
      go.textContent = "Flash my Canary";
      st.busy = false;
      st.chip = null; // the board reboots after a write; read it fresh next time
    }
  };

  // ── after the write: the board's own boot receipt ────────────────────────
  const renderReceipt = async (product, receipt) => {
    // Claim the port for the monitor BEFORE anything awaits: the poll must
    // not read the rebooting board (a board-info resets it) while the
    // monitor is still starting.
    monitorControls = []; // the last receipt's row went with its card
    releaseReceipt(); // …and so do its listeners, before this card adds its own
    const card = { unlisteners: [] };
    receiptCard = card;
    setMonitoring(true);
    result.innerHTML = "";
    result.classList.remove("flash-hidden");
    result.append(el("h2", null, `${product.name} v${receipt.version} is on the board`));
    result.append(el("p", "muted",
      `${receipt.chip_write_verified ? "Written, and espflash confirmed the write on the chip" : "Written"} · ` +
      `${releaseCheck(receipt.release_verification)} · ${receipt.channel} channel · ` +
      `installed SHA-256 ${String(receipt.installed_sha256).slice(0, 16)}…` +
      (receipt.provisioned ? ". Your settings were sealed into the image; their values are never logged." : "")));
    const status = el("p", "flash-stage", "");
    const boot = el("pre", "flash-console-tall");
    result.append(status, boot);
    if (!requiresLiveReceipt(product)) {
      status.textContent = "Flashing is complete. ✓ This firmware doesn’t report a live boot receipt.";
    } else {
      status.textContent = "Watching the live boot for its device receipt…";
    }
    // The serial monitor, started the way the Flasher starts it after a flash:
    // one deliberate reboot of a native-USB board so its boot streams from
    // the first line, then a pure observer that rides out re-enumeration.
    const hold = async (pending) => { card.unlisteners.push(await pending); };
    await hold(listen("serial:status", (ev) => {
      status.textContent = String(ev.payload);
      // The monitor thread only ends when stopped; hand the port back to the
      // poll then (the Flasher reads the same word).
      if (String(ev.payload).toLowerCase().includes("stopped")) setMonitoring(false);
    }));
    await hold(listen("serial:log", (ev) => {
      boot.textContent = (boot.textContent + String(ev.payload)).slice(-16 * 1024);
      boot.scrollTop = boot.scrollHeight;
    }));
    await hold(listen("serial:receipt", (ev) => {
      const r = ev.payload || {};
      const m = r.manifest || {};
      // The self-manifest's `firmware` is the version string the board
      // reports (securacv.canary.manifest/v1) — the Flasher reads it as such.
      const fw = typeof m.firmware === "string" && m.firmware ? ` · firmware ${m.firmware}` : "";
      status.textContent = r.ready
        ? `✓ ${m.board || product.name} booted and answered with its receipt${fw}.`
        : `The board answered${fw} — still waiting for everything it reports to come up.`;
    }));
    // Talk back to the firmware's console: the Flasher's command row (its
    // monitor chips and command box, desktop/src/app.js renderMonitorCmds and
    // #monitor-cmd-form), in its words. The chips are the flashed product's
    // own commands (sweep A86) and the box's placeholder says what that
    // firmware reads — single keys, the tuning console's words, or nothing.
    // Each chip sends its command and the box sends what was typed, each with
    // the newline the firmware's command parser reads (the engine sends
    // exactly what it is handed and appends nothing). A refused send is said
    // in the status line, as the Flasher says it, never dropped. No
    // line-ending picker: the Flasher's is for raw keystrokes, and this box is
    // the browser flasher's, newline only.
    const sendCommand = async (command) => {
      if (!st.monitoring) return false;
      try {
        await invoke("serial_monitor_send", { command });
        return true;
      } catch (e) {
        status.textContent = String(e);
        return false;
      }
    };
    const vocab = core.monitorVocabulary(catalog, product);
    const chips = el("div", "flash-mon-chips");
    for (const [cmd, label] of vocab.cmds) {
      const b = el("button", "ghost small", `${label} (${cmd})`);
      b.type = "button";
      b.dataset.cmd = cmd;
      b.addEventListener("click", () => { sendCommand(cmd + "\n"); });
      chips.append(b);
      monitorControls.push(b);
    }
    const inRow = el("div", "flash-mon-input");
    const input = el("input");
    input.type = "text";
    input.placeholder = vocab.placeholder;
    input.setAttribute("aria-label", "Serial command");
    input.maxLength = 128;
    const sendBtn = el("button", "primary small", "Send");
    sendBtn.type = "button";
    const sendTyped = async () => {
      const text = input.value;
      if (!text) return;
      if (await sendCommand(text + "\n")) input.value = "";
    };
    input.addEventListener("keydown", (ev) => {
      if (ev.key !== "Enter") return;
      if (ev.preventDefault) ev.preventDefault();
      sendTyped();
    });
    sendBtn.addEventListener("click", () => { sendTyped(); });
    inRow.append(input, sendBtn);
    monitorControls.push(input, sendBtn);
    setMonitoring(st.monitoring); // the row starts as usable as the monitor
    result.append(chips, inRow);

    const stop = el("button", "ghost", "Stop watching");
    stop.addEventListener("click", async () => {
      try { await invoke("stop_serial_monitor"); } catch { /* already stopped */ }
      setMonitoring(false);
      releaseReceipt(card);
      stop.remove();
    });
    result.append(stop);
    try {
      await invoke("start_serial_monitor", {
        port: st.port,
        vid: st.portInfo ? st.portInfo.vid : null,
        pid: st.portInfo ? st.portInfo.pid : null,
        baud: catalog.console_baud || 115200,
        postFlash: true,
      });
    } catch (e) {
      setMonitoring(false);
      status.textContent = `The write landed${receipt.chip_write_verified ? " (espflash confirmed it on the chip)" : ""}, ` +
        `but the serial monitor didn’t start (${e}).`;
    }
  };

  // ── go ───────────────────────────────────────────────────────────────────
  await pollPorts();
  st.pollTimer = setInterval(pollPorts, 1000);
}
