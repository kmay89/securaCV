// canary-local/assets/wap.js — The WAP page driver ("First Boot").
//
// Renders wap.html entirely from devices/wap.json — the generated,
// drift-gated data (tools/gen_wap.py parses the canary-wap firmware + docs).
// The page hardcodes no version, no SSID, no topic, no boot line: a firmware
// change re-lines every surface without anyone editing this file, and CI
// fails if wap.json goes stale against the source.
//
// One tiny bus cross-wires the four surfaces (3D board reused from the Board
// Room; serial console; the phone's captive-portal setup; the dashboard +
// MQTT once it's online) so a single action — power on, finish the wizard,
// wave in the sandbox — ripples through all of them at once, like a bench.
//
// Failure posture (same as The Hub): if wap.json can't load or misses a
// section, degrade to a plain pointer at the getting-started guide — never a
// blank screen.

import { buildBoardLab } from "./board-lab.js";
import { buildSerial, buildPhone, buildDashboard, buildMqtt, buildPlugIn, buildFlash } from "./wap-ui.js";

const $ = (sel, root = document) => root.querySelector(sel);
const el = (tag, cls, text) => {
  const n = document.createElement(tag);
  if (cls) n.className = cls;
  if (text != null) n.textContent = text;
  return n;
};
const GH = "https://github.com/kmay89/securaCV/blob/main/";

// Fire-once-per-phase bus. `ap`/`online`/`mqtt` are lifecycle phases that must
// only fire a single time (so the retained MQTT snapshot / boot table can't
// double-render); `event`, `power`, `ready`, `connect` are free to repeat.
function makeBus() {
  const listeners = {};
  const reached = new Set();
  const PHASES = new Set(["ap", "online", "mqtt"]);
  return {
    has: (p) => reached.has(p),
    on(type, fn) { (listeners[type] || (listeners[type] = [])).push(fn); },
    emit(type, payload = {}) {
      if (PHASES.has(type)) {
        if (reached.has(type)) return;
        reached.add(type);
      }
      for (const fn of listeners[type] || []) {
        try { fn(payload); } catch (e) { console.error("wap bus", type, e); }
      }
    },
  };
}

async function main() {
  const mount = $("#wap");
  let data, boards;
  try {
    const res = await fetch("devices/wap.json");
    if (!res.ok) throw new Error("HTTP " + res.status);
    data = await res.json();
    for (const k of ["device", "ap", "captive", "wizard", "serial", "mqtt", "sensing", "sandbox"])
      if (!data[k]) throw new Error("missing section: " + k);
    boards = await (await fetch("devices/boards.json")).json().catch(() => null);
  } catch (e) {
    mount.append(el("p", "muted",
      "The WAP bench data failed to load (" + e.message + "). Everything it stages is also " +
      "written out in docs/getting_started_canary.md."));
    const a = el("a", null, "Open the getting-started guide →");
    a.href = GH + "docs/getting_started_canary.md";
    mount.append(a);
    return;
  }

  const bus = makeBus();

  renderVersionStrip(data);
  renderBoard(data, boards);
  renderSetup(data, bus);
  if (data.flash) renderFlash(data);
  renderNetwork(data, bus);
  renderSandbox(data, bus);
  renderKeepGoing(data);

  function section(id, kicker, title, lede) {
    const s = el("section", "hub-section");
    s.id = id;
    if (kicker) s.append(el("div", "hub-kicker", kicker));
    if (title) s.append(el("h2", null, title));
    if (lede) s.append(el("p", "hub-lede", lede));
    mount.append(s);
    return s;
  }

  // ── version strip ──
  function renderVersionStrip(d) {
    const strip = $("#wap-versions");
    if (!strip) return;
    strip.innerHTML = "";
    const chips = el("div", "hub-chips");
    for (const [label, val] of [
      ["device", d.device.name],
      ["board", d.device.board_short],
      ["firmware", "v" + d.device.fw_version],
      ["train", d.device.fw_train],
      ["senses", d.device.modality],
    ]) {
      const c = el("span", "chip");
      c.append(el("span", "hub-chip-k", label + " "), el("strong", null, val));
      chips.append(c);
    }
    strip.append(chips);
    strip.append(el("p", "fineprint hub-fresh",
      "every string here — SSID, boot log, topics, wizard labels — is parsed from the firmware by " +
      "tools/gen_wap.py and drift-checked in CI. Nothing is written twice."));
  }

  // ── §board (reuses the Board Room's real vendor CAD) ──
  function renderBoard(d, boardsData) {
    const s = section("board", "the hardware", "The board it runs on",
      "The WAP is a " + d.device.model + " — the vendor's own CAD, the same viewer the " +
      "Board Room and enclosure lab use. It feels presence through the WiFi field itself: no camera needed.");
    if (boardsData) s.append(buildBoardLab(boardsData, "canary-wap"));
    else s.append(el("p", "muted", "Board catalog unavailable — run tools/gen_boards.py."));
  }

  // ── §setup: power on (serial) + set it up (phone) ──
  function renderSetup(d, b) {
    const s = section("setup", "bring it up", "Power on, then set it up from your phone",
      "It is a W.A.P. — literally a WiFi Access Point. First boot it brings up a " +
      d.ap.ssid_prefix + "XXXX network with a device-unique password; you join it, and a " +
      "captive page walks you to canary.local. Plug it in, power it on, then watch the phone catch the network live.");
    s.append(buildPlugIn(d, b));
    const two = el("div", "wap-two");
    const left = el("div", "wap-two-col");
    left.append(el("h3", "wap-col-h", "2 · the serial console (laptop path)"), buildSerial(d, b));
    const right = el("div", "wap-two-col");
    right.append(el("h3", "wap-col-h", "3 · your phone (every path)"), buildPhone(d, b));
    two.append(left, right);
    s.append(two);

    // the durable how-it-connects facts, straight from the firmware constants
    const facts = el("div", "wap-facts");
    for (const [k, v] of [
      ["SoftAP", d.ap.ssid_example + " · WPA2 · ch " + d.ap.channel + " · " + d.ap.max_clients + " client"],
      ["Password", d.ap.password_example + "  (" + d.ap.password_scheme.split(":")[0] + "…, unique per device)"],
      ["Reach it", "canary.local · " + d.ap.mdns_example + " · " + d.ap.ip],
      ["Captive", "A→" + d.ap.ip + " (TTL " + d.captive.dns.a_ttl + "s); Apple 200 · Android 204 · Windows NCSI"],
      ["Times out", "abandoned portal reboots after " + d.ap.setup_timeout_min + " min; AP lingers " + d.ap.ap_grace_sec + "s after join"],
    ]) {
      const row = el("div", "wap-fact");
      row.append(el("span", "wap-fact-k", k), el("span", "wap-fact-v", v));
      facts.append(row);
    }
    s.append(facts);
  }

  // ── §flash: the bench skills — toolchains, BOOT/RESET, download mode ──
  function renderFlash(d) {
    const s = section("flash", "bench skills", "Flashing — and the two little buttons",
      "Kits arrive flashed, but every builder meets esptool eventually — and the most " +
      "frustrating five minutes in ESP32 land is a port that won't answer. The commands, board " +
      "settings and library pins below are parsed from the firmware's own README; the BOOT/RESET " +
      "ritual is the ESP32-S3 mask ROM's real strap logic, the same one the bench emulator stages.");
    s.append(buildFlash(d));
  }

  // ── §network: dashboard + MQTT (once online) ──
  function renderNetwork(d, b) {
    const s = section("network", "on your network", "What answers on your LAN",
      "Once it joins your WiFi (or runs standalone), the device serves its own dashboard at " +
      "canary.local and — if you pointed it at a broker — announces itself to Home Assistant over MQTT. " +
      "No YAML, no pairing codes: it publishes its own entities the instant it connects.");
    const two = el("div", "wap-two");
    const left = el("div", "wap-two-col");
    left.append(el("h3", "wap-col-h", "The dashboard (canary.local)"), buildDashboard(d, b));
    const right = el("div", "wap-two-col");
    right.append(el("h3", "wap-col-h", "MQTT — " + d.mqtt.prefix + "/…"), buildMqtt(d, b));
    two.append(left, right);
    s.append(two);
  }

  // ── §sandbox: cross-wired free play ──
  function renderSandbox(d, b) {
    const s = section("sandbox", "try it", "The sandbox",
      "Every button drives a real signal path: it lands the presence pill, writes a witness " +
      "record on the serial console, and publishes the exact MQTT the firmware would. Fire a T3 " +
      "smoke cadence, hold the panic pad, mute the mic — watch all three surfaces move together.");
    const pad = el("div", "wap-sandbox");
    const readout = el("p", "muted wap-sandbox-read", "Power the device up above, or just tap a card — the bench will bring it online for you.");
    let seq = 0;

    for (const sc of d.sandbox) {
      const card = el("button", "card wap-sand-card");
      card.append(el("strong", null, sc.label), el("span", "muted", sc.blurb));
      if (sc.ha) card.append(el("code", "fineprint wap-sand-ha", sc.ha));
      card.addEventListener("click", () => {
        // bring the whole bench online on first interaction (idempotent)
        if (!b.has("mqtt")) { b.emit("online"); b.emit("mqtt"); }
        seq += 1;
        b.emit("event", { ...sc, seqBump: seq });
        readout.textContent = "▶ " + sc.label + " — " + sc.blurb + (sc.ha ? "  →  " + sc.ha : "");
      });
      pad.append(card);
    }
    s.append(pad, readout);
  }

  // ── §keep going ──
  function renderKeepGoing(d) {
    const s = section("more", "keep going", "Where this goes next", null);
    const grid = el("div", "hub-links");
    const items = [
      ["The getting-started guide", "the whole walkthrough — connect, name devices, place them well", "docs/getting_started_canary.md"],
      ["The WAP firmware", "the sketch this page is parsed from — captive portal, MQTT, witness chain", "firmware/projects/canary-wap/README.md"],
      ["The Board Room", "pin flags + LEGO-style wiring on this exact board", "canary-local/boards.html"],
      ["The Hub", "Home Assistant on a Raspberry Pi — where these MQTT entities land", "canary-local/homeassistant.html"],
    ];
    for (const [title, body, path] of items) {
      const a = el("a", "hub-link");
      const local = path.startsWith("canary-local/");
      a.href = local ? path.replace("canary-local/", "") : GH + path;
      if (!local) { a.target = "_blank"; a.rel = "noopener noreferrer"; }
      a.append(el("strong", null, title), el("span", "muted", body), el("code", "fineprint", path));
      grid.append(a);
    }
    s.append(grid);
  }
}

main();
