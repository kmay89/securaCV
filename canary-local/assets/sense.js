// canary-local/assets/sense.js — The Sense page driver ("Radar School").
//
// Renders sense.html entirely from devices/sense.json — the generated,
// drift-gated data (tools/gen_sense.py parses the canary-sense firmware +
// design doc). The page hardcodes no threshold, no topic, no boot line: a
// firmware change re-lines every surface without anyone editing this file,
// and CI fails if sense.json goes stale against the source.
//
// One tiny bus cross-wires the surfaces (3D radome; USB provisioning + the
// serial console; the placement lab running the real FSM thresholds; MQTT +
// the HA entity set; the sandbox) so a single action — flash it, drag a
// person through the cone, pull the radar cable — ripples through all of
// them at once, like a bench.
//
// Failure posture (same as The WAP): if sense.json can't load or misses a
// section, degrade to a plain pointer at the design doc — never a blank page.

import {
  buildDevice, buildProvision, buildSerial, buildRadarLab,
  buildMqtt, buildEntities, buildPlacement, buildTuning,
  buildUseCases, buildSandbox,
} from "./sense-ui.js";

const $ = (sel, root = document) => root.querySelector(sel);
const el = (tag, cls, text) => {
  const n = document.createElement(tag);
  if (cls) n.className = cls;
  if (text != null) n.textContent = text;
  return n;
};
const GH = "https://github.com/kmay89/securaCV/blob/main/";

// Fire-once-per-phase bus. `power`/`wifi`/`mqtt`/`ready` are lifecycle phases
// that must only fire once (the retained MQTT snapshot can't double-render);
// `state`, `serial`, `labevent`, `labcmd`, … repeat freely.
function makeBus() {
  const listeners = {};
  const reached = new Set();
  const PHASES = new Set(["power", "wifi", "mqtt", "ready"]);
  return {
    has: (p) => reached.has(p),
    on(type, fn) { (listeners[type] || (listeners[type] = [])).push(fn); },
    emit(type, payload = {}) {
      if (PHASES.has(type)) {
        if (reached.has(type)) return;
        reached.add(type);
      }
      for (const fn of listeners[type] || []) {
        try { fn(payload); } catch (e) { console.error("sense bus", type, e); }
      }
    },
  };
}

async function main() {
  const mount = $("#sense");
  let data;
  try {
    const res = await fetch("devices/sense.json");
    if (!res.ok) throw new Error("HTTP " + res.status);
    data = await res.json();
    for (const k of ["device", "radar", "fsm", "provisioning", "serial", "mqtt",
                     "placement", "tuning", "sandbox"])
      if (!data[k]) throw new Error("missing section: " + k);
  } catch (e) {
    mount.append(el("p", "muted",
      "The Sense bench data failed to load (" + e.message + "). Everything it stages is " +
      "also written out in the design doc."));
    const a = el("a", null, "Open the canary-sense design doc →");
    a.href = GH + "docs/canary_sense_mr60bha2_design.md";
    mount.append(a);
    return;
  }

  const bus = makeBus();

  renderVersionStrip(data);

  const sDevice = section("device", "the hardware", "A radar with no eyes",
    "The Sense is a " + data.device.board + " kit in the printable RADOME shell. " +
    data.radar.band + ", " + data.radar.fov + " — and by construction it cannot know who you are.");
  sDevice.append(buildDevice(data, bus));
  sDevice.append(protocolDetails(data));

  const sLab = section("lab", "placement is the tuning", "The placement lab",
    "The real cone, the firmware's real thresholds — mirrored in JS and drift-gated in CI. " +
    "Drag the people through it. Invite the cat. Turn on the fan. Watch the actual FSM decide, " +
    "debounce, bucket, and refuse.");
  sLab.append(buildRadarLab(data, bus));

  const sSetup = section("setup", "bring it up", "Provision it once, over USB",
    data.provisioning.intro);
  sSetup.append(buildProvision(data, bus));
  const sTwo = el("div", "wap-two");
  const left = el("div", "wap-two-col");
  left.append(el("h3", "wap-col-h", "4 · the serial console"), buildSerial(data, bus));
  const right = el("div", "wap-two-col");
  right.append(el("h3", "wap-col-h", "what answers on your network"), netFacts(data));
  sTwo.append(left, right);
  sSetup.append(sTwo);

  const sNet = section("network", "on your network", "MQTT + Home Assistant",
    "Once online it announces its own entities over MQTT discovery — no YAML, no pairing codes. " +
    "Every event is Ed25519-signed and hash-chained; Home Assistant TOFU-pins the pubkey from the " +
    "health topic and renders the green device-verified badge.");
  const nTwo = el("div", "wap-two");
  const nLeft = el("div", "wap-two-col");
  nLeft.append(el("h3", "wap-col-h", "the broker's view"), buildMqtt(data, bus));
  const nRight = el("div", "wap-two-col");
  nRight.append(el("h3", "wap-col-h", "the Home Assistant entity set"), buildEntities(data));
  nTwo.append(nLeft, nRight);
  sNet.append(nTwo);

  const sPlace = section("placement", "where it lives", "Mounting — the three geometries",
    "Presence is a wall product; vitals are a bed product. Seeed's own install specs, " +
    "the radome physics, and the official keep-out list.");
  sPlace.append(buildPlacement(data));

  const sTune = section("tuning", "max capability, least error", "The tuning playbook",
    null);
  sTune.append(buildTuning(data));
  sTune.append(realityCheck(data));

  const sUses = section("uses", "what it's for", "Use it like a witness",
    "Five deployments from the design doc — each one a thing a camera can't do, " +
    "shouldn't do, or shouldn't be trusted alone to do.");
  sUses.append(buildUseCases(data));

  const sSand = section("sandbox", "try it", "The sandbox",
    "Every card drives a real signal path: the lab moves, the console prints the firmware's " +
    "line, and the exact MQTT the device would publish streams on the broker.");
  sSand.append(buildSandbox(data, bus));

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
    const strip = $("#sense-versions");
    if (!strip) return;
    strip.innerHTML = "";
    const chips = el("div", "hub-chips");
    for (const [label, val] of [
      ["device", d.device.name],
      ["board", d.device.board_short],
      ["radar", d.radar.module],
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
      "every threshold, topic, boot line and entity here is parsed from the firmware by " +
      "tools/gen_sense.py and drift-checked in CI. Nothing is written twice."));
  }

  // ── the wire protocol, for the curious ──
  function protocolDetails(d) {
    const pr = d.radar.protocol;
    const det = el("details", "fix sense-proto");
    det.append(el("summary", null, "The wire protocol — what actually crosses that UART"));
    const facts = el("div", "wap-facts");
    for (const [k, v] of [["SOF", pr.sof], ["Header", pr.header], ["Payload", pr.payload],
                          ["Checksum", pr.checksum], ["Aggregation", pr.aggregation], ["Resync", pr.resync]]) {
      const row = el("div", "wap-fact");
      row.append(el("span", "wap-fact-k", k), el("span", "wap-fact-v", v));
      facts.append(row);
    }
    det.append(facts);
    const grid = el("div", "wap-mqtt-ents");
    for (const f of pr.frames) {
      const row = el("div", "wap-ent");
      row.append(el("span", "wap-ent-comp", f.id), el("span", "wap-ent-name", f.name),
        el("code", "wap-ent-topic", f.decodes));
      grid.append(row);
    }
    det.append(grid, el("p", "fineprint muted",
      d.radar.blackbox_note + " Decoder: firmware/common/sensors/mmwave_mr60/mr60_uart.h — host-tested in firmware/tests_host/."));
    return det;
  }

  // ── network facts (no HTTP server — that's the point) ──
  function netFacts(d) {
    const wrap = el("div");
    const facts = el("div", "wap-facts");
    for (const [k, v] of [
      ["No web server", "no dashboard on the device, no HTTP at all — the attack surface a witness doesn't need"],
      ["mDNS", d.device.host_example + ".local — a _securacv._tcp fleet advert (device_id, fw, model, dt, role=witness) so the companion app and sibling Canaries find it"],
      ["MQTT", d.mqtt.broker_uri + " — " + d.mqtt.topic_pattern],
      ["Offline", d.mqtt.offline_note],
    ]) {
      const row = el("div", "wap-fact");
      row.append(el("span", "wap-fact-k", k), el("span", "wap-fact-v", v));
      facts.append(row);
    }
    wrap.append(facts);
    return wrap;
  }

  // ── the reality-check strip ──
  function realityCheck(d) {
    const r = d.tuning.reality;
    const wrap = el("div", "sense-reality");
    wrap.append(el("h3", "wap-col-h", r.title));
    const grid = el("div", "sense-captable");
    const head = el("div", "sense-caprow sense-caphead");
    for (const h of ["the claim", "the reality", "per"]) head.append(el("span", null, h));
    grid.append(head);
    for (const f of r.flags) {
      const row = el("div", "sense-caprow");
      row.append(el("span", null, f.claim), el("span", "muted", f.reality),
        el("span", "sense-src sense-src-" + f.src, f.src));
      grid.append(row);
    }
    wrap.append(grid);
    const links = el("p", "fineprint muted");
    links.append(document.createTextNode("Primary sources: "));
    d.tuning.links.forEach((l, i) => {
      const a = el("a", null, l.name);
      a.href = l.url; a.target = "_blank"; a.rel = "noopener noreferrer";
      links.append(a);
      if (i < d.tuning.links.length - 1) links.append(document.createTextNode(" · "));
    });
    wrap.append(links);
    return wrap;
  }

  // ── keep going ──
  function renderKeepGoing(d) {
    const s = section("more", "keep going", "Where this goes next", null);
    const grid = el("div", "hub-links");
    const items = [
      ["The design doc", "the whole plan — privacy classes, scenarios, the MR60FDA2 fall sibling", d.docs.design],
      ["The Sense firmware", "the sources this page is parsed from — FSMs, witness chain, discovery", "firmware/projects/canary-sense/README.md"],
      ["The WAP — first boot", "the WiFi-CSI sibling: captive portal, browser flashing, the other physics", "canary-local/wap.html"],
      ["The Hub", "Home Assistant on a Raspberry Pi — where these entities land", "canary-local/homeassistant.html"],
      ["The RADOME enclosure", "the printable shell — membrane thickness computed and asserted", d.docs.enclosure],
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
