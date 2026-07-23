// canary-local/assets/vision.js — The Vision page driver ("First Watch").
//
// Renders vision.html entirely from devices/vision.json — the generated,
// drift-gated data (tools/gen_vision.py parses the canary-vision firmware +
// the Grove Vision AI V2 docs). The page hardcodes no threshold, no topic,
// no boot line, no port name: a firmware change re-lines every surface
// without anyone editing this file, and CI fails if vision.json goes stale
// against the source.
//
// One tiny bus cross-wires the surfaces (3D board; the two-port picker; the
// staged SenseCraft model load with its live-preview sim; the host boot
// console; MQTT/HA; the boxes-only Aim card; the tuning sandbox) so a single
// action — load the model, power on, send the cat through — ripples through
// all of them at once, like a bench. One VisionSim instance is the shared
// ground truth: the SAME compiled production detection pipeline, NVS tuning,
// voxel tracker and FSM feed the preview pane, aim card, event log and MQTT
// stream. JavaScript owns the staged sensor scene, never Canary decisions.
//
// Failure posture (same as The Hub/WAP): if vision.json can't load or
// misses a section, degrade to a plain pointer at the getting-started
// guide — never a blank screen.

import {
  VisionSim, buildBoardLab, buildPorts, buildModelLoad, buildSerial,
  buildMqtt, buildAim, buildTunePlay, buildPlacement, buildFlashHost,
  buildTrouble,
} from "./vision-ui.js";
import { createVisionFirmwareCore } from "../emulator/web/vision-core.js";

const $ = (sel, root = document) => root.querySelector(sel);
const el = (tag, cls, text) => {
  const n = document.createElement(tag);
  if (cls) n.className = cls;
  if (text != null) n.textContent = text;
  return n;
};
const GH = "https://github.com/kmay89/securaCV/blob/main/";

// Fire-once-per-phase bus (same contract as wap.js): `model`/`online`/`mqtt`
// are lifecycle phases that fire a single time; the rest repeat freely.
function makeBus() {
  const listeners = {};
  const reached = new Set();
  const PHASES = new Set(["model", "online", "mqtt"]);
  return {
    has: (p) => reached.has(p),
    on(type, fn) { (listeners[type] || (listeners[type] = [])).push(fn); },
    emit(type, payload = {}) {
      if (PHASES.has(type)) {
        if (reached.has(type)) return;
        reached.add(type);
      }
      for (const fn of listeners[type] || []) {
        try { fn(payload); } catch (e) { console.error("vision bus", type, e); }
      }
    },
  };
}

async function main() {
  const mount = $("#vision");
  let data, boards, firmwareCore;
  try {
    const res = await fetch("devices/vision.json");
    if (!res.ok) throw new Error("HTTP " + res.status);
    data = await res.json();
    for (const k of ["device", "module", "ports", "model_load", "detect", "serial", "mqtt", "aim", "placement", "sandbox"])
      if (!data[k]) throw new Error("missing section: " + k);
    boards = await (await fetch("devices/boards.json")).json().catch(() => null);
    firmwareCore = await createVisionFirmwareCore(globalThis.createCanaryVisionCore);
    firmwareCore.assertGeneratedData(data);
  } catch (e) {
    mount.append(el("p", "muted",
      "The Vision firmware bench failed its startup check (" + e.message + "). Everything it stages is " +
      "also written out in docs/hardware/canary_vision_getting_started.md."));
    const a = el("a", null, "Open the getting-started guide →");
    a.href = GH + "docs/hardware/canary_vision_getting_started.md";
    mount.append(a);
    return;
  }

  const bus = makeBus();
  const sim = new VisionSim(data, firmwareCore);
  if (data.model_load.wire) {
    sim.preview.confidence = data.model_load.wire.tscore_default;
    sim.preview.iou = data.model_load.wire.tiou_default;
  }

  // the one sim loop — everything watches the same ground truth
  let last = performance.now();
  (function loop(now) {
    const dt = Math.min(200, now - last);
    last = now;
    sim.tick(dt, data.detect.invoke_period_ms);
    requestAnimationFrame(loop);
  })(last);

  // poking any sandbox surface brings the bench online (idempotent)
  bus.on("wake", () => {
    if (!bus.has("mqtt")) { bus.emit("online"); bus.emit("mqtt"); }
  });

  renderVersionStrip(data);
  renderBoard(data, boards);
  renderPorts(data);
  renderModel(data);
  renderFlash(data);
  renderSerial(data);
  renderNetwork(data);
  renderAim(data);
  renderPlace(data);
  renderPlay(data);
  renderTrouble(data);
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
    const strip = $("#vision-versions");
    if (!strip) return;
    strip.innerHTML = "";
    const chips = el("div", "hub-chips");
    for (const [label, val] of [
      ["device", d.device.name],
      ["module", d.module.name],
      ["firmware", "v" + d.device.fw_version],
      ["runtime", "real firmware wasm"],
      ["train", d.device.fw_train],
      ["senses", d.device.modality],
    ]) {
      const c = el("span", "chip");
      c.append(el("span", "hub-chip-k", label + " "), el("strong", null, val));
      chips.append(c);
    }
    strip.append(chips);
    strip.append(el("p", "fineprint hub-fresh",
      "the staged sensor boxes flow through canary-vision's compiled detection pipeline, " +
      "NVS-backed tuning and presence FSM. Generated copy is checked against that wasm " +
      "contract at startup, and CI rebuilds the artifact from this tree."));
  }

  // ── §board ──
  function renderBoard(d, boardsData) {
    const s = section("board", "the hardware", "A camera that is also the computer",
      "The Grove Vision AI V2 is a self-contained vision inference machine: a Himax HX6538 " +
      "with an Ethos-U55 NPU runs the detection model on-module, and the ESP32 host only " +
      "ever hears semantic results over I2C. The vendor's own CAD below — the same viewer " +
      "the Board Room uses.");
    if (boardsData) s.append(buildBoardLab(boardsData, "canary-vision"));
    else s.append(el("p", "muted", "Board catalog unavailable — run tools/gen_boards.py."));
    const facts = el("div", "wap-facts");
    for (const sp of d.module.specs) {
      const row = el("div", "wap-fact");
      row.append(el("span", "wap-fact-k", sp.k), el("span", "wap-fact-v", sp.v));
      facts.append(row);
    }
    s.append(facts);
    s.append(el("p", "ondevice wap-note", d.module.privacy + " " + d.module.unused));
  }

  // ── §ports ──
  function renderPorts(d) {
    const s = section("ports", "the gotcha", "Two USB-C ports, two different computers",
      "Assembled, a Canary Vision is two computers stacked: the module runs the AI, the " +
      "XIAO runs canary-vision. Each has its own USB-C, and each port only reaches its own " +
      "chip. Learn it here instead of at the bench.");
    s.append(buildPorts(d));
  }

  // ── §model ──
  function renderModel(d) {
    const s = section("model", "one-time setup", "Load the brain — once",
      "The detection model lives in the module's own 16 MB flash and survives power cycles " +
      "and every future firmware update. You load it once with Seeed's SenseCraft workspace " +
      "— staged here click for click, including the live preview with its bounding boxes, " +
      "so the real thing holds zero surprises.");
    s.append(buildModelLoad(d, bus, sim));
  }

  // ── §flash ──
  function renderFlash(d) {
    const s = section("flash", "the other port", "Flash canary-vision onto the host",
      "Now the XIAO's USB-C — the small board. Three commands, or the Lab's in-browser " +
      "flasher if you'd rather never see a terminal. Your WiFi and broker live in " +
      "secrets.h; the first flash seeds the device, signed OTA updates inherit it.");
    s.append(buildFlashHost(d));
  }

  // ── §serial ──
  function renderSerial(d) {
    const s = section("serial", "first boot", "Power on and read what it says",
      "The host boots, checks its hardware, introduces the module over I2C, joins WiFi and " +
      "announces itself to Home Assistant. Every string below is the firmware's own — the " +
      "one line to care about is Grove Vision AI ID: non-zero means the link is alive.");
    s.append(buildSerial(d, bus));
  }

  // ── §network ──
  function renderNetwork(d) {
    const s = section("network", "on your network", "What answers on your LAN",
      "No YAML, no pairing codes: the firmware publishes retained MQTT Discovery configs " +
      "and the device simply appears in Home Assistant — presence, dwelling, confidence, " +
      "voxel, diagnostics, a signed-OTA update entity, and four live tuning numbers.");
    s.append(buildMqtt(d, bus));
  }

  // ── §aim ──
  function renderAim(d) {
    const s = section("aim", "boxes, never pixels", "Aim it from the couch",
      "SenseCraft's preview needs a laptop on the module's port and streams real frames. " +
      "After mounting you never do that again: the Aim card draws the live box and voxel " +
      "cell from coordinates alone, over your local MQTT. This is the exact payload — and " +
      "the exact nothing-else — that leaves the device.");
    s.append(buildAim(d, bus, sim));
  }

  // ── §placement ──
  function renderPlace(d) {
    const s = section("place", "where it lives", "Placement — and what to tell it",
      "A witness never pans; the mounting decision is the detection decision. House rules " +
      "below, then a preset for the room it's watching — each one lands real numbers on " +
      "the same sliders Home Assistant exposes.");
    s.append(buildPlacement(d, bus));
  }

  // ── §sandbox ──
  function renderPlay(d) {
    const s = section("play", "try it", "The sandbox",
      "Send actors through the staged scene and watch the whole pipeline answer: the " +
      "best-box rule picks one person, the voxel grid coarsens the where, the FSM times " +
      "presence into dwelling, and only signed claims reach MQTT. Then drag the sliders " +
      "and watch the same scene produce different truth.");
    s.append(buildTunePlay(d, bus, sim));
  }

  // ── §troubleshooting ──
  function renderTrouble(d) {
    const s = section("fix", "when it fights back", "The symptom table",
      "Every row parsed from the device guide and the walkthrough — the same fixes, in " +
      "the same words.");
    s.append(buildTrouble(d));
  }

  // ── §keep going ──
  function renderKeepGoing(d) {
    const s = section("more", "keep going", "Where this goes next", null);
    const grid = el("div", "hub-links");
    const items = [
      ["The getting-started walkthrough", "unboxing → aimed and publishing, ~45 minutes", d.docs.getting_started],
      ["The device guide", "ports, wiring, protocol, recovery — the deep reference", d.docs.device_guide],
      ["The Vision firmware", "the code this page is parsed from", d.docs.firmware_readme],
      ["The Board Room", "pin flags + wiring on this exact board", "canary-local/boards.html"],
      ["Flash a Canary", "the in-browser flasher for the ESP32 host", "canary-local/flash.html"],
      ["The Hub", "Home Assistant on a Raspberry Pi — where these entities land", "canary-local/homeassistant.html"],
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
