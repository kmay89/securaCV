/* harness.js — lifted out of emulator/web/harness.html so the page can carry a strict Content-Security-Policy
   (script-src 'self', no inline, no hashes to re-pin on every edit; the policy table is
   canary-local/tools/gen_csp.py). Same code, same load order — only the file moved. */
import { CanaryEmulator, demoFleet } from "./emu-shell.js";

// Which glass boots is a query parameter, not a hard-coded script tag:
// ?flavor=watch (the default) | dash | nightstand | touch169 | amoled241 |
// nightlight,
// each a committed dist/canary-display-<flavor>.js exporting
// createCanaryEmu<Flavor>. The boot probe walks every flavor this way, so
// a display CI never booted cannot ship broken.
// Allowlist, not sanitization: the query value only ever SELECTS one of
// these entries; no user-controlled string reaches a script URL or a
// global name. Add a flavor here when a new dist bundle is committed.
const FLAVORS = {
  watch:     { src: "../dist/canary-display-watch.js",     factory: "createCanaryEmuWatch" },
  dash:      { src: "../dist/canary-display-dash.js",      factory: "createCanaryEmuDash" },
  nightstand:{ src: "../dist/canary-display-nightstand.js",factory: "createCanaryEmuNightstand" },
  touch169:  { src: "../dist/canary-display-touch169.js",  factory: "createCanaryEmuTouch169" },
  amoled241: { src: "../dist/canary-display-amoled241.js", factory: "createCanaryEmuAmoled241" },
  nightlight:{ src: "../dist/canary-display-nightlight.js",factory: "createCanaryEmuNightlight" },
};
const q = new URLSearchParams(location.search);
const wanted = (q.get("flavor") || "watch").toLowerCase();
const entry = Object.hasOwn(FLAVORS, wanted) ? FLAVORS[wanted] : null;
if (!entry) throw new Error("unknown flavor; known: " + Object.keys(FLAVORS).join(", "));
const flavor = wanted;
await new Promise((resolve, reject) => {
  const tag = document.createElement("script");
  tag.src = entry.src;
  tag.onload = resolve;
  tag.onerror = () => reject(new Error("dist bundle failed to load for " + flavor));
  document.head.appendChild(tag);
});
const factory = window[entry.factory];
if (typeof factory !== "function") throw new Error("dist bundle for " + flavor + " exports no factory");

const serial = document.getElementById("serial");
// shapes: every shape the firmware announced for its glass (onDisplayReady,
// F184), with how many frames it had drawn by then — so a probe can tell the
// glass the FIRST frame landed on (a saved rotation is worn before the
// splash, main.cpp) from one the firmware turned to later.
const state = { booted: false, flushes: 0, serialText: "", mqtt: [], flavor, shapes: [] };
window.__state = state;

const emu = new CanaryEmulator(factory, {
  canvas: document.getElementById("glass"),
  onSerial: (t) => {
    state.serialText += t;
    serial.textContent = state.serialText.slice(-20000);
    serial.scrollTop = serial.scrollHeight;
  },
  onDisplayReady: (w, h, round) => {
    state.shapes.push({ w, h, frames: state.flushes });
    if (round) document.getElementById("glass").classList.add("round");
  },
  onFrame: () => { state.flushes++; },
  onMqtt: (m) => { state.mqtt.push(m); },
  onBacklight: (glow) => {
    document.getElementById("glass").style.filter = `brightness(${Math.max(0.05, glow)})`;
  },
  onNetEvent: (k, d) => { state.mqtt.push({ dir: "net", topic: k, payload: d }); },
  onReboot: () => location.reload(),
});
window.__emu = emu;

// The bench buttons (see harness.html #controls): data-emu names the call,
// data-arg its numeric argument. Delegated here instead of onclick= attributes
// so the harness carries the same no-inline-script CSP as the product pages.
const CONTROLS = {
  wifi: (n) => emu.setWifi(n !== 0),
  broker: (n) => emu.setBroker(n !== 0),
  time: (n) => emu.setTimeScale(n),
};
document.getElementById("controls").addEventListener("click", (e) => {
  const b = e.target.closest("button[data-emu]");
  if (!b || !Object.hasOwn(CONTROLS, b.dataset.emu)) return;
  CONTROLS[b.dataset.emu](Number(b.dataset.arg));
});

// ?rotation=0..3: a saved rotation staged before power-on, so the glass boots
// turned the way main.cpp brings up a unit that saved it: the dash's
// canary::glass::Rotation (F184: 1 is the 480x800 portrait) or the
// nightlight's quarter turns (F204: 1 is the 320x180 landscape). Only those
// four values.
const ROTATIONS = { 0: 0, 1: 1, 2: 2, 3: 3 };
const rotationParam = q.get("rotation");
if (rotationParam !== null && !Object.hasOwn(ROTATIONS, rotationParam)) {
  throw new Error("rotation must be 0, 1, 2 or 3");
}
// ?timescale=<x>: the emulated clock's speed from power-on (F206), so a probe
// can read the splash on a slowed clock from its first frame (0.5 = half
// speed). A number above 0 and at most 64; anything else is refused.
const timeScaleParam = q.get("timescale");
const timeScale = timeScaleParam === null ? null : Number(timeScaleParam);
if (timeScale !== null && !(Number.isFinite(timeScale) && timeScale > 0 && timeScale <= 64)) {
  throw new Error("timescale must be a number above 0 and at most 64");
}
// A boot the shell refuses (a dist without a binding the query asks for) is
// named on window.__harnessError, so a probe fails on the reason at once
// instead of waiting out __ready.
try {
  await emu.start({
    provisioned: q.get("provisioned") !== "0",
    firstMeeting: q.get("meet") === "1",
    seed: 1234,
    rotation: rotationParam === null ? null : ROTATIONS[rotationParam],
    timeScale,
  });
} catch (e) {
  window.__harnessError = String((e && e.message) || e);
  throw e;
}
if (q.get("hour") !== null) emu.setLocalHour(Number(q.get("hour")));
state.booted = true;

const fleet = demoFleet();
window.__fleet = fleet;
for (const w of fleet) await emu.addWitness(w);
emu.startFleetHeartbeat(15000);

document.getElementById("status").textContent = "booted; fleet staged";
window.__ready = true;
