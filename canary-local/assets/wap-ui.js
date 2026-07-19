// canary-local/assets/wap-ui.js — the WAP bench widgets.
//
// Four staged surfaces for the canary-wap device, all fed from the
// drift-gated devices/wap.json (tools/gen_wap.py parses the firmware):
//
//   · buildSerial   — the USB-CDC console: the real boot banner + the
//                     ordered setup() log stream, then the 1 Hz witness table.
//   · buildPhone    — a phone joining the SecuraCV-XXXX SoftAP, the captive
//                     sheet rendering the firmware's ACTUAL captive HTML, and
//                     the five-step setup wizard that brings the device up.
//   · buildDashboard— a faithful *sketch* of the on-device dashboard (labeled
//                     as a sketch — the real one is 5,400 lines of device-API
//                     JS we can't run offline; the pills/cards are the doc's).
//   · buildMqtt     — an MQTT-explorer view: retained topics fill on connect,
//                     events stream, the 24 HA-discovery entities announce.
//
// Everything is wired through a tiny shared bus (see wap.js) so one action —
// power on, submit the wizard, wave your arm in the sandbox — ripples across
// all four at once, the way it would on a real bench. Nothing is faked past
// what the firmware strings say; where this is a sketch, it says so to your face.

import { DeviceScene, BUILDERS, M4, roundedBox, cylinder } from "./scene3d.js";
import { upgradeRealShape } from "./real-shapes.js";
import { romBanner } from "../emulator/web/bench.js";

const el = (tag, cls, text) => {
  const n = document.createElement(tag);
  if (cls) n.className = cls;
  if (text != null) n.textContent = text;
  return n;
};
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const alive = (node) => document.body.contains(node);

// ── DOM-free cores (exported; pinned in tests/wap.test.js) ────────────────

// Substitute the illustrative device id into a topic/template ("<id>", "<device_id>").
export function withId(tmpl, id) {
  return String(tmpl).split("<device_id>").join(id).split("<id>").join(id);
}

// Flatten the serial data into one ordered list of {cls,text} console lines —
// the banner, the setup() log (tag-classed), then the ready scene. This is the
// exact order the firmware prints; the streamer just types it out.
export function bootLines(serial) {
  const out = [];
  for (const t of serial.banner || []) out.push({ cls: "wap-b", text: t });
  for (const s of serial.boot || []) {
    const tag = s.tag || "";
    const cls = tag === "[OK]" ? "ok" : tag === "[WARN]" ? "warn"
      : tag === "[--]" ? "faint" : tag === "[WIFI]" || tag === "[MQTT]" ? "net" : "prov";
    out.push({ cls: "wap-" + cls, text: `${tag} ${s.text}`.trim() });
  }
  for (const t of serial.ready || []) out.push({ cls: "wap-b", text: t });
  return out;
}

// The retained-topic store the broker keeps: a retained publish overwrites,
// a non-retained publish (events) is delivered but NOT stored, a clear removes.
// Mirrors the MQTT retention rule the firmware relies on.
export function mqttApply(store, msg) {
  if (!msg || !msg.topic) return store;
  if (msg.clear) delete store[msg.topic];
  else if (msg.retain) store[msg.topic] = msg.payload;
  return store;
}

// A sandbox/CSI event → the presence pill it lands the dashboard on (or null
// if it doesn't move presence, e.g. a mic mute).
export function pillForEvent(ev) {
  return {
    motion: "Motion", present: "Presence", subtle: "Presence",
    empty: "Quiet", quiet: "Quiet", active: "Active",
    smoke_alarm_t3: "Motion", co_alarm_t4: "Motion", silent_panic: "Presence",
  }[ev] || null;
}

// ── the cable's spine (DOM-free; pinned in tests/wap.test.js) ─────────────
// The plug-in scene's USB-C cable, in the CONNECTOR's local frame: +Z points
// into the port, the lead trails out behind (−Z) and droops off-stage. A
// cubic bezier — t 0 is just behind the strain-relief boot, t 1 the far end.
export const CABLE_BEZIER = [[0, 0, -14], [0, 0, -70], [0, -55, -105], [16, -150, -150]];
export function cablePoint(t, bz = CABLE_BEZIER) {
  const u = 1 - t, [a, b, c, d] = bz;
  const w = [u * u * u, 3 * u * u * t, 3 * u * t * t, t * t * t];
  return [0, 1, 2].map((i) => w[0] * a[i] + w[1] * b[i] + w[2] * c[i] + w[3] * d[i]);
}

// Where the cable seats and where the light answers — measured from the
// committed compact STLs, mapped through realTwoPart's placement (base at
// z −fz/2+nest/2, lid flipped at +bz/2−nest/2):
//   USB slot: base −X wall (x −16.85), the y ±5.25 notch (usb_w 10.5),
//             connector centre z ≈ 6.6 raw → scene (−16.85, 0, −1.45)
//   light pipe: lid Ø3.2 at raw (5.44, −1.68) → scene (5.44, 1.68, 8.05)
export const WAP_PORT = [-16.85, 0, -1.45];
export const WAP_LED = [5.44, 1.68, 8.05];

// ── the serial console ────────────────────────────────────────────────────
export function buildSerial(data, bus) {
  const wrap = el("div", "wap-term");
  const bar = el("div", "hub-term-bar");
  const dots = el("span", "hub-term-dots");
  dots.append(el("i"), el("i"), el("i"));
  bar.append(dots, el("span", "hub-term-title", "USB-CDC · 115200 8N1"),
    el("span", "hub-term-sim", "real firmware strings · staged boot"));
  const scroll = el("div", "wap-term-scroll");
  const controls = el("div", "hub-term-controls");
  const btnPwr = el("button", "primary small", "⏻ power on");
  const btnSkip = el("button", "ghost small", "skip to ready");
  const hint = el("span", "muted fineprint", "plug it in above — the console needs a laptop on the other end of the cable");
  btnPwr.disabled = btnSkip.disabled = true;
  controls.append(btnPwr, btnSkip, hint);
  wrap.append(bar, scroll, controls);

  const lines = bootLines(data.serial);
  let booting = false, booted = false, plugged = null;

  const put = (cls, text) => {
    const l = el("div", "wap-line " + cls);
    l.textContent = text;
    scroll.append(l);
    scroll.scrollTop = scroll.scrollHeight;
    return l;
  };

  async function boot(instant) {
    if (booting || booted) return;
    booting = true; btnPwr.disabled = true; btnSkip.disabled = true;
    scroll.innerHTML = "";
    bus.emit("power");
    let sawAp = false;
    for (const ln of lines) {
      put(ln.cls, ln.text);
      // the AP-started line is the cue for the phone to see the network
      if (!sawAp && /AP started:/.test(ln.text)) { sawAp = true; bus.emit("ap"); }
      if (!instant) {
        if (!alive(scroll)) { booting = false; return; }
        await sleep(ln.text.trim() === "" ? 8 : 26);
      }
    }
    booted = true; booting = false;
    bus.emit("ready");
  }

  // choosing the power source is the lesson: a laptop gives you this console;
  // a wall adapter powers the same boot but nobody is on the other end
  bus.on("plug", ({ source }) => {
    plugged = source;
    if (source === "laptop") {
      wrap.classList.remove("wap-term-ghost");
      if (booted || booting) {
        put("wap-net", "— laptop attached: the USB-CDC console picks up live —");
      } else {
        btnPwr.disabled = btnSkip.disabled = false;
        hint.textContent = "boots from devices/wap.json — the firmware's own boot log";
      }
    } else {
      wrap.classList.add("wap-term-ghost");
      btnPwr.disabled = btnSkip.disabled = true;
      hint.textContent = "wall power — no USB data; these lines are printed to nobody";
      if (!booted && !booting) {
        put("wap-faint", "— wall adapter: power only. The device still prints all of this; without a laptop, nobody sees it —");
        setTimeout(() => { if (alive(scroll) && !booted && !booting) boot(false); }, 700);
      }
    }
  });

  // once the device is on the network, stream the join line + start the table
  let tableStarted = false;
  function startTable() {
    if (tableStarted) return;
    tableStarted = true;
    put("wap-net", data.serial.join_line);
    put("wap-b", "");
    // the wizard's success is a real reboot — show it, ROM banner and all
    put("wap-b", "※ ── the Canary reboots itself — provisioning done ── ※");
    for (const l of romBanner("swreset").split("\n")) if (l.trim()) put("wap-faint", l);
    put("wap-faint", "(the boot banner scrolls by again; this time it joins your WiFi as a client)");
    put("wap-faint", "");
    put("wap-tbl", data.serial.runtime_header);
    for (const r of data.serial.runtime_rows) put("wap-tbl", r);
  }

  bus.on("online", () => { if (!booted) boot(true).then(startTable); else startTable(); });
  bus.on("event", (e) => {
    if (!tableStarted) startTable();
    if (e.serial) put("wap-tbl", "| " + String(1236 + (e.seqBump || 0)).padStart(4) + " | " + e.serial);
  });
  bus.on("mqtt", () => {
    put("wap-net", "[MQTT] connected");
    put("wap-net", "[MQTT] discovery: " + data.mqtt.discovery.counts.entities + " entities announced");
    put("wap-net", "[MQTT] discovery: " + data.mqtt.discovery.counts.triggers + " triggers announced");
  });

  btnPwr.addEventListener("click", () => boot(false));
  btnSkip.addEventListener("click", () => boot(true));
  return wrap;
}

// ── the phone: SoftAP join → captive sheet → setup wizard ─────────────────
export function buildPhone(data, bus) {
  const ap = data.ap, wiz = data.wizard;
  const wrap = el("div", "wap-phone-wrap");

  const phone = el("div", "wap-phone");
  const statusbar = el("div", "wap-phone-status");
  statusbar.append(el("span", null, "9:41"), el("span", "wap-phone-net", "📶  SecuraCV"), el("span", null, "100%"));
  const screen = el("div", "wap-phone-screen");
  phone.append(statusbar, screen);

  const ribbon = el("p", "ondevice wap-note");
  ribbon.append(el("strong", null, "How to read this: "), document.createTextNode(
    "the Wi-Fi sheet and wizard are staged, but the captive page is the firmware's own " +
    "CAPTIVE_PORTAL_HTML rendered verbatim, and every SSID, address, route and label is the drift-gated real one."));
  wrap.append(phone, ribbon);

  let phase = "off"; // off → ap → wifi → captive → portal → connecting → online
  let power = null;  // 'laptop' | 'wall' — changes where you read the password from

  // ---- screens -----------------------------------------------------------
  function screenLocked() {
    screen.innerHTML = "";
    const s = el("div", "wap-screen wap-screen-wait");
    const copy = power === "wall"
      ? "Plugged into the wall — the Canary boots on its own. Within ten seconds it brings up its setup network. Your password is on the box card (there's no console to read it from)."
      : power === "laptop"
        ? "Power the Canary on (in the serial console) — within ten seconds it brings up its setup network."
        : "Plug the Canary in above — laptop or wall adapter, either powers it. Then watch this phone catch its network.";
    s.append(el("div", "wap-big", "⏻"),
      el("p", "muted", copy),
      el("p", "fineprint muted", "the SoftAP SSID is derived from the device's key: " + ap.ssid_prefix + "····"));
    screen.append(s);
  }

  function screenWifi() {
    screen.innerHTML = "";
    const s = el("div", "wap-screen");
    s.append(el("div", "wap-ios-title", "Wi-Fi"));
    const list = el("div", "wap-wifi-list");
    // the device's own AP, first — the one you join
    const own = el("button", "wap-wifi-row wap-wifi-own");
    own.append(el("span", "wap-wifi-name", ap.ssid_example),
      el("span", "wap-wifi-meta", "🔒 📶"));
    own.addEventListener("click", joinAp);
    list.append(own);
    for (const n of ["Loft 2.4G", "Loft 5G", "SFR-a8b2", "eero-guest"]) {
      const r = el("button", "wap-wifi-row wap-wifi-dim");
      r.append(el("span", "wap-wifi-name", n), el("span", "wap-wifi-meta", "🔒 📶"));
      r.disabled = true;
      list.append(r);
    }
    s.append(list, el("p", "fineprint muted",
      "WPA2, one client at a time. Password " + ap.password_example + " (yours is unique — " +
      (power === "wall" ? "read it off the box card; with no laptop attached the serial line goes unseen"
                        : "the box card / the first-boot serial line above") + ")."));
    screen.append(s);
  }

  function browserChrome(addr) {
    const c = el("div", "wap-browser-chrome");
    c.append(el("span", "wap-browser-lock", "🔒"), el("span", "wap-browser-url", addr));
    return c;
  }

  function screenCaptive() {
    screen.innerHTML = "";
    const s = el("div", "wap-screen wap-screen-browser");
    const sheet = el("div", "wap-captive-head", "‹ Sign in to SecuraCV");
    // the firmware's real captive landing page, verbatim, in a sandboxed iframe
    const frame = el("iframe", "wap-captive-frame");
    frame.setAttribute("sandbox", "");
    frame.setAttribute("title", "captive portal (device HTML)");
    frame.srcdoc = data.captive.html;
    const go = el("button", "primary small wap-captive-go", "Open canary.local in your browser →");
    go.addEventListener("click", openPortal);
    s.append(sheet, frame, go);
    screen.append(s);
  }

  // the five-step wizard (a sketch of companion_pwa.h, labels drift-gated)
  function screenPortal() {
    screen.innerHTML = "";
    const s = el("div", "wap-screen wap-screen-browser");
    s.append(browserChrome("canary.local/companion"));
    const body = el("div", "wap-wiz");
    s.append(body);
    screen.append(s);
    renderStep(body, 0, {});
  }

  function renderStep(body, i, ctx) {
    body.innerHTML = "";
    const steps = wiz.steps;
    const prog = el("div", "wap-wiz-prog");
    steps.forEach((_, k) => prog.append(el("span", "wap-wiz-dot" + (k <= i ? " on" : ""))));
    body.append(prog);
    const step = steps[i];
    body.append(el("h4", "wap-wiz-h", step.title));
    if (step.sub) body.append(el("p", "muted wap-wiz-sub",
      step.sub.replace("<ssid>", ctx.ssid || "your network")));

    const next = (j, add) => renderStep(body, j, { ...ctx, ...add });

    if (i === 0) {
      const go = el("button", "primary wap-wiz-go", "Let's go");
      go.addEventListener("click", () => next(1));
      const alt = el("button", "ghost small", "Use without home WiFi");
      alt.addEventListener("click", standalone);
      body.append(go, alt);
    } else if (i === 1) {
      const list = el("div", "wap-wiz-nets");
      const nets = [["Loft 2.4G", "−48"], ["Loft 5G", "−55"], ["SFR-a8b2", "−71"], ["eero-guest", "−77"]];
      for (const [ssid, rssi] of nets) {
        const r = el("button", "wap-wiz-net");
        r.append(el("span", null, ssid), el("span", "muted fineprint", rssi + " dBm"));
        r.addEventListener("click", () => next(2, { ssid }));
        list.append(r);
      }
      body.append(el("p", "fineprint muted", "GET /api/wifi/scan"), list,
        chip("Scan again", () => next(1)));
    } else if (i === 2) {
      const inp = el("input", "wap-input");
      inp.type = "password"; inp.placeholder = "Network password";
      const row = el("div", "wap-wiz-btns");
      const back = el("button", "ghost", "Back"); back.addEventListener("click", () => next(1));
      const conn = el("button", "primary", "Connect"); conn.addEventListener("click", () => connect(ctx.ssid));
      row.append(back, conn);
      body.append(inp, row, el("p", "fineprint muted", "POST /api/wifi/connect  {ssid,password,token}"));
    }
  }

  function chip(label, fn) {
    const b = el("button", "chip wap-chip", label);
    b.addEventListener("click", fn);
    return b;
  }

  async function screenConnecting(ssid) {
    screen.innerHTML = "";
    const s = el("div", "wap-screen wap-screen-browser");
    s.append(browserChrome("canary.local/companion"));
    const body = el("div", "wap-wiz");
    body.append(el("div", "wap-spinner"), el("h4", "wap-wiz-h", "Connecting…"));
    const log = el("p", "muted wap-wiz-sub", "Sending credentials to your Canary.");
    body.append(log);
    s.append(body); screen.append(s);
    for (const t of ["Waiting for your home WiFi.", "Joining " + ssid + "…", "Verifying the link."]) {
      await sleep(700); if (!alive(screen)) return; log.textContent = t;
    }
    await sleep(600); if (!alive(screen)) return;
    screenOnline(ssid, false);
  }

  function screenOnline(ssid, standaloneMode) {
    screen.innerHTML = "";
    const step5 = wiz.steps[4] || {};
    const s = el("div", "wap-screen wap-screen-browser");
    s.append(browserChrome("canary.local/companion"));
    const body = el("div", "wap-wiz wap-wiz-done");

    // progress: all five firmware steps are walked
    const prog = el("div", "wap-wiz-prog");
    wiz.steps.forEach(() => prog.append(el("span", "wap-wiz-dot on")));
    body.append(prog);

    // step 4 — the connect outcome
    body.append(el("div", "wap-check", "✓"));
    if (standaloneMode) {
      body.append(el("h4", "wap-wiz-h", "Running standalone."),
        el("p", "muted wap-wiz-sub", "Your Canary keeps its own SecuraCV network — no home WiFi needed, nothing leaves the device."));
    } else {
      body.append(el("h4", "wap-wiz-h", "Your Canary is online."),
        el("p", "muted wap-wiz-sub", "Joined " + ssid + ". The SecuraCV setup network turns itself off in about " +
          ap.ap_grace_sec + " seconds — reconnect this phone to your home WiFi and find your Canary at canary.local."));
    }

    // step 5 — pre-flight checks (GET /api/selftest), the recovery kit, then the
    // optional Home Assistant hookup. These are the firmware's own step-5
    // surfaces (companion_pwa.h), driven from wap.json so they can't drift.
    const pf = el("div", "wap-preflight");
    pf.append(el("h5", null, step5.title || "Pre-flight checks"));
    if (step5.api) pf.append(el("p", "fineprint muted", step5.api));
    const checks = el("div", "wap-checks");
    for (const c of step5.checks || []) {
      const row = el("span", "wap-check-row");
      row.append(el("span", "wap-check-ok", "✓"), document.createTextNode(c));
      checks.append(row);
    }
    pf.append(checks);
    body.append(pf);

    // --- step 5 close-out: recovery kit, then the optional Home Assistant
    // hookup. The HA block is GATED behind saving the recovery kit — on device,
    // companion_pwa.h reveals #wiz-ha-block only after /api/provisioning-receipt
    // succeeds, because that download issues the cv_session cookie that
    // /api/mqtt/config requires (otherwise it 401s). Showing HA first would
    // demonstrate an onboarding order that cannot succeed on hardware.
    const haBlock = (step5.blocks || []).find((b) => /home assistant/i.test(b.title || ""));
    const haField = haBlock && haBlock.fields && haBlock.fields[0];
    const ha = el("div", "wap-ha-block");
    ha.style.display = "none";  // revealed only after the recovery kit is saved
    ha.append(el("h5", null, (haBlock && haBlock.title) || "Use Home Assistant? (optional)"));
    const host = el("input", "wap-input");
    host.placeholder = (haField && haField.placeholder) || "Broker address (like 192.168.1.10)";
    ha.append(host);
    const haRow = el("div", "wap-wiz-btns");
    const skip = el("button", "ghost", "Skip");
    const save = el("button", "primary", "Test & save");
    haRow.append(skip, save);
    ha.append(haRow);

    const done = (withHa, msg) => {
      finish(withHa);
      ha.style.display = "";  // ensure the outcome message is visible
      ha.innerHTML = "";
      ha.append(el("p", "ok", msg));
    };
    skip.addEventListener("click", () => done(false, "✓ Finished. Name it by its room from the dashboard's Settings."));
    save.addEventListener("click", () => done(true, "✓ Broker saved — entities announcing on Home Assistant."));

    const rk = (step5.blocks || []).find((b) => /recovery kit/i.test(b.title || ""));
    if (rk) {
      const rkEl = el("div", "wap-ha-block");
      rkEl.append(el("h5", null, rk.title));
      if (rk.detail) rkEl.append(el("p", "fineprint muted", rk.detail));
      const rkRow = el("div", "wap-wiz-btns");
      const later = el("button", "ghost", "Do this later");
      const saveKit = el("button", "primary", "Save my recovery kit");
      saveKit.addEventListener("click", () => {
        rkRow.replaceWith(el("p", "ok fineprint",
          "✓ canary-recovery-kit.json saved — this unlocks the Home Assistant step below."));
        ha.style.display = "";  // the receipt 'issued the cv_session cookie' → HA is now reachable
        host.focus();
      });
      later.addEventListener("click", () => done(false,
        "Recovery kit skipped — grab it later from Settings → Export. (Home Assistant needs the receipt's session, so it stays off for now.)"));
      rkRow.append(later, saveKit);
      rkEl.append(rkRow);
      body.append(rkEl);
    } else {
      ha.style.display = "";  // no recovery step in the data → nothing to gate on
    }
    body.append(ha);

    s.append(body); screen.append(s);
  }

  // ---- transitions -------------------------------------------------------
  function setPhase(p) { phase = p; }
  function joinAp() { if (phase === "off") return; setPhase("captive"); screenCaptive(); }
  function openPortal() { setPhase("portal"); screenPortal(); }
  function connect(ssid) { setPhase("connecting"); screenConnecting(ssid); bus.emit("connect", { ssid }); }
  function standalone() { setPhase("online"); screenOnline(null, true); bus.emit("connect", { standalone: true }); }
  function finish(withHa) {
    // emit-only: the DOM outcome message is handled by the caller's done()
    bus.emit("online", { ha: withHa });
    if (withHa) bus.emit("mqtt");
  }

  bus.on("plug", ({ source }) => { power = source; if (phase === "off") screenLocked(); });
  bus.on("ap", () => { if (phase === "off") { setPhase("ap"); screenWifi(); } });
  screenLocked();
  return wrap;
}

// ── plug it in: the device in 3D + the choice that teaches the console ────
export function buildPlugIn(data, bus) {
  const wrap = el("div", "wap-plug");
  const stage = el("div", "wap-plug-stage");
  const cv = el("canvas", "wap-plug-3d");
  cv.setAttribute("aria-label", "Interactive 3D render of the Canary WAP");
  stage.append(cv, el("div", "asmlab-hint", "drag to orbit · pinch or scroll to zoom"));
  const scene = new DeviceScene(cv, null);
  BUILDERS["canary-wap"](scene);
  const reduced = matchMedia("(prefers-reduced-motion: reduce)").matches;

  const side = el("div", "wap-plug-side");
  side.append(el("h3", "wap-col-h", "1 · plug it in"));
  side.append(el("p", "muted",
    "This is the device — the repo's own printed shells, its USB-C slot on the left wall, " +
    "its light pipe on the face. Any USB-C source powers it; what changes is what YOU get " +
    "to see. Pick where the other end of the cable goes:"));
  const choices = el("div", "wap-plug-choices");
  const verdict = el("p", "muted fineprint wap-plug-verdict");

  // ── the cable rig ──────────────────────────────────────────────────────
  // Built AFTER the real STLs land (the upgrade clears the part list). The
  // connector's local frame has +Z into the port; the port is on the −X
  // wall, so the whole rig wears R = rotY(90°) and slides along −X.
  const rig = {
    parts: [],        // [{part, local}]
    beads: [],        // power pulses riding the cable spine
    led: null,
    gap: 26,          // mm the tip floats outside the port (0 = seated)
    state: "idle",    // idle → plugging → seated (reseat wobbles gap)
    anim: null,       // {from, to, t0, dur, then}
    ledPhase: "dark", // dark → hello (double-blink) → dim → boot (burst) → breathe
    ledT0: 0,
  };
  const R_PORT = M4.rotY(Math.PI / 2);
  const easeOutBack = (t) => { const c = 1.70158; return 1 + (c + 1) * Math.pow(t - 1, 3) + c * Math.pow(t - 1, 2); };

  // a round tube swept along the cable spine, in connector-local coords
  function tubeBuilder(r = 2.3, segs = 34, ring = 10) {
    const b = { pos: [], nrm: [], uv: [], idx: [] };
    const rings = [];
    for (let i = 0; i <= segs; i++) {
      const t = i / segs;
      const p = cablePoint(t);
      const q = cablePoint(Math.min(1, t + 0.01));
      let T = [q[0] - p[0], q[1] - p[1], q[2] - p[2]];
      const tl = Math.hypot(...T) || 1; T = T.map((v) => v / tl);
      // frame: N ⊥ T, biased to world-x so the tube doesn't twist
      let N = [1, 0, 0];
      const d = N[0] * T[0] + N[1] * T[1] + N[2] * T[2];
      N = [N[0] - d * T[0], N[1] - d * T[1], N[2] - d * T[2]];
      const nl = Math.hypot(...N) || 1; N = N.map((v) => v / nl);
      const B = [T[1] * N[2] - T[2] * N[1], T[2] * N[0] - T[0] * N[2], T[0] * N[1] - T[1] * N[0]];
      const row = [];
      for (let j = 0; j < ring; j++) {
        const a = (j / ring) * Math.PI * 2;
        const n = [0, 1, 2].map((k) => N[k] * Math.cos(a) + B[k] * Math.sin(a));
        b.pos.push(p[0] + n[0] * r, p[1] + n[1] * r, p[2] + n[2] * r);
        b.nrm.push(...n); b.uv.push(0, 0);
        row.push(b.pos.length / 3 - 1);
      }
      rings.push(row);
    }
    for (let i = 0; i < segs; i++)
      for (let j = 0; j < ring; j++) {
        const a = rings[i][j], b2 = rings[i][(j + 1) % ring];
        const c2 = rings[i + 1][(j + 1) % ring], d2 = rings[i + 1][j];
        b.idx.push(a, b2, c2, a, c2, d2);
      }
    return b;
  }

  function buildRig() {
    const add = (builder, opts, local) =>
      rig.parts.push({ part: scene.addMesh(builder, opts), local });
    // USB-C metal tip (enters the slot), strain-relief boot, the lead itself
    add(roundedBox(9.0, 3.4, 6.5, 1.5), { color: [0.80, 0.81, 0.84], gloss: 0.85 }, M4.translate(0, 0, 3.1));
    add(roundedBox(11.4, 6.6, 14, 2.6), { color: [0.13, 0.13, 0.15], gloss: 0.5 }, M4.translate(0, 0, -7.2));
    add(tubeBuilder(), { color: [0.16, 0.16, 0.18], gloss: 0.45 }, M4.ident());
    // power pulses: unlit canary beads that ride the spine once power flows
    for (let i = 0; i < 5; i++) {
      const p = scene.addMesh(cylinder(1.6, 6.0, 12), { color: [1.0, 0.83, 0.31], unlit: true });
      p.model = M4.scale(0, 0, 0);
      rig.beads.push(p);
    }
    // the light pipe answers on the lid face — plus a soft halo so the
    // blink reads from across the room (the whole point of a light pipe)
    rig.led = scene.addMesh(cylinder(1.5, 1.8, 20), {
      color: [0.12, 0.11, 0.10], gloss: 0.9,
      model: M4.translate(...WAP_LED),
    });
    rig.halo = scene.addMesh(cylinder(3.6, 0.25, 24), {
      color: [0, 0, 0], unlit: true,
      model: M4.translate(WAP_LED[0], WAP_LED[1], WAP_LED[2] + 1.1),
    });
  }

  function ledColor(now) {
    const el2 = now - rig.ledT0;
    const DIM = [0.30, 0.25, 0.08], ON = [1.0, 0.83, 0.31], DARK = [0.12, 0.11, 0.10];
    switch (rig.ledPhase) {
      case "dark": return DARK;
      case "hello": { // power present: two quick blinks, then settle dim
        if (el2 > 900) { rig.ledPhase = "dim"; return DIM; }
        const k = Math.floor(el2 / 150);
        return k % 2 === 0 && k < 4 ? ON : DARK;
      }
      case "dim": return DIM;
      case "boot": { // the app is booting: a fast burst, then breathe
        if (el2 > 1100) { rig.ledPhase = "breathe"; return ON; }
        return Math.floor(el2 / 90) % 2 === 0 ? ON : DIM;
      }
      case "breathe": {
        const k = 0.35 + 0.65 * (0.5 + 0.5 * Math.sin(el2 / 350));
        return [ON[0] * k, ON[1] * k, ON[2] * k];
      }
    }
    return DARK;
  }

  scene.onTick = () => {
    if (!rig.parts.length) return;
    const now = performance.now();
    if (rig.anim) {
      const k = Math.min(1, (now - rig.anim.t0) / rig.anim.dur);
      rig.gap = rig.anim.from + (rig.anim.to - rig.anim.from) * easeOutBack(k);
      if (k >= 1) { const then = rig.anim.then; rig.anim = null; if (then) then(); }
    } else if (rig.state === "idle" && !reduced) {
      rig.gap = 26 + Math.sin(now / 900) * 2.2; // hover by the port, inviting
    }
    const W = M4.mul(M4.translate(WAP_PORT[0] - rig.gap, WAP_PORT[1], WAP_PORT[2]), R_PORT);
    for (const { part, local } of rig.parts) part.model = M4.mul(W, local);
    // power pulses flow the moment the cable seats — wall or laptop alike
    const flowing = rig.state === "seated";
    rig.beads.forEach((b, i) => {
      if (!flowing) { b.model = M4.scale(0, 0, 0); return; }
      const s = 1 - (((now / 1400) + i / rig.beads.length) % 1); // far end → port
      const p = cablePoint(s);
      b.model = M4.mul(W, M4.mul(M4.translate(p[0], p[1], p[2]), M4.rotX(Math.PI / 2)));
    });
    if (rig.led) {
      const c = ledColor(now);
      rig.led.color = c;
      // halo carries ~45% of the pipe's light; near-black when the LED is dark
      if (rig.halo) rig.halo.color = c.map((v) => Math.max(0, (v - 0.12) * 0.45));
    }
  };

  function seatCable(source, said) {
    const finish = () => {
      rig.state = "seated";
      rig.ledPhase = "hello"; rig.ledT0 = performance.now();
      verdict.textContent = said;
      bus.emit("plug", { source });
    };
    if (reduced || !rig.parts.length) { rig.gap = 0; rig.state = "seated"; // no theater, same truth
      rig.ledPhase = "dim"; verdict.textContent = said; bus.emit("plug", { source }); return; }
    if (rig.state === "seated") { // swapping the far end: quick unplug, replug
      rig.state = "plugging";
      rig.anim = { from: rig.gap, to: 14, t0: performance.now(), dur: 300,
        then: () => { rig.anim = { from: 14, to: 0, t0: performance.now(), dur: 450, then: finish }; } };
    } else {
      rig.state = "plugging";
      verdict.textContent = "▶ plugging in…";
      rig.anim = { from: rig.gap, to: 0, t0: performance.now(), dur: 750, then: finish };
    }
  }

  // the app booting (serial side) wakes the light for real
  bus.on("power", () => { if (rig.led) { rig.ledPhase = "boot"; rig.ledT0 = performance.now(); } });

  // the real STLs land first (the upgrade clears the scene), THEN the rig
  upgradeRealShape(scene, "canary-wap").then(() => {
    if (!document.body.contains(cv)) return;
    buildRig();
  });
  scene.start();

  const mk = (source, icon, title, sub, said) => {
    const b = el("button", "wap-plug-card");
    b.append(el("span", "wap-plug-icon", icon), el("strong", null, title), el("span", "muted", sub));
    b.addEventListener("click", () => {
      for (const c of choices.children) c.classList.remove("on");
      b.classList.add("on");
      seatCable(source, said);
    });
    return b;
  };
  choices.append(
    mk("laptop", "💻", "Into a laptop",
      "USB data + power — the serial console becomes your window into the boot.",
      "▶ Seated with a click. Power is flowing — the light pipe blinks hello. Press ⏻ power on in the console below and read everything it says."),
    mk("wall", "🔌", "Into a wall adapter",
      "Power only. It boots exactly the same — you just don't see it.",
      "▶ Seated with a click. Power is flowing and the light blinks — but no console anywhere. Your phone is the only window; the password comes off the box card."));
  side.append(choices, verdict);
  wrap.append(stage, side);
  return wrap;
}

// ── flashing: toolchains, the two buttons, and the download-mode ritual ───
export function buildFlash(data) {
  const f = data.flash;
  const wrap = el("div", "wap-flash");

  // the durable bench facts
  const facts = el("div", "wap-facts");
  for (const [k, v] of [["Kits", f.kit_note], ["Cable", f.cable], ["Port", f.port_hint]]) {
    const row = el("div", "wap-fact");
    row.append(el("span", "wap-fact-k", k), el("span", "wap-fact-v", v));
    facts.append(row);
  }
  wrap.append(facts);

  // BOOT + RESET, as cards
  const btns = el("div", "wap-btn-cards");
  for (const b of f.buttons) {
    const card = el("div", "wap-btn-card");
    card.append(el("span", "wap-btn-chip", b.label), el("p", "muted", b.what));
    const ul = el("ul", "wap-btn-gestures");
    for (const g of b.gestures || []) ul.append(el("li", null, g));
    card.append(ul);
    btns.append(card);
  }
  wrap.append(el("h3", "wap-col-h", "The two little buttons"), btns);

  // the ritual + the hands-on trainer
  const two = el("div", "wap-two");
  const left = el("div", "wap-two-col");
  left.append(el("h4", "wap-flash-h", f.download_mode.title));
  const ol = el("ol", "wap-ritual");
  for (const s of f.download_mode.steps) ol.append(el("li", null, s));
  left.append(ol, el("p", "fineprint muted", f.download_mode.note));
  const right = el("div", "wap-two-col");
  right.append(el("h4", "wap-flash-h", "Try the ritual — this bench is the real strap logic"));
  right.append(buildStrapTrainer());
  two.append(left, right);
  wrap.append(two);

  // toolchains
  wrap.append(el("h3", "wap-col-h", "Get the firmware on it"));
  const tabs = el("nav", "subtabs");
  const panel = el("div", "wap-tool-panel");
  const panels = {};
  for (const t of f.toolchains) {
    const b = el("button", "tab" + (t.id === "platformio" ? " on" : ""), t.name);
    b.addEventListener("click", () => {
      for (const x of tabs.children) x.classList.remove("on");
      b.classList.add("on");
      panel.innerHTML = "";
      panel.append(panels[t.id]);
    });
    tabs.append(b);
    panels[t.id] = t.id === "platformio" ? pioPanel(t) : arduinoPanel(t);
  }
  wrap.append(tabs, panel);
  panel.append(panels.platformio);

  const ribbon = el("p", "ondevice wap-note");
  ribbon.append(el("strong", null, "One firmware, two toolchains: "),
    document.createTextNode(f.one_firmware_note.replace(/^One firmware, two toolchains\.\s*/, "")));
  wrap.append(ribbon);

  // troubleshooting — the frustrating part, verbatim from the README
  const ts = el("div", "wap-trouble");
  ts.append(el("h3", "wap-col-h", "When it fights back"));
  for (const t of f.troubleshooting) {
    const d = el("details", "fix");
    d.append(el("summary", null, t.symptom));
    const ul = el("ul", "wap-btn-gestures");
    for (const fix of t.fixes) ul.append(el("li", null, fix));
    d.append(ul);
    ts.append(d);
  }
  // recovery
  const rec = el("details", "fix");
  rec.append(el("summary", null, "Lost the AP password / need a factory reset"));
  const rol = el("ol", "wap-ritual");
  for (const s of f.recovery.password) rol.append(el("li", null, s));
  rec.append(rol, el("p", "muted fineprint", f.recovery.factory));
  ts.append(rec);
  wrap.append(ts);

  wrap.append(el("p", "fineprint muted",
    "parsed from " + f.sources.build_paths + " · buttons from " + f.sources.buttons +
    " · recovery from " + f.sources.recovery + " — drift-gated in CI, nothing here can go stale."));
  return wrap;
}

function prereqLine(prereqs) {
  const p = el("p", "muted wap-prereqs");
  p.append(document.createTextNode("Prerequisites: "));
  prereqs.forEach((pr, i) => {
    const a = el("a", null, pr.name);
    a.href = pr.url; a.target = "_blank"; a.rel = "noopener noreferrer";
    p.append(a);
    if (i < prereqs.length - 1) p.append(document.createTextNode(" + "));
  });
  return p;
}

function pioPanel(t) {
  const w = el("div", "wap-tool");
  w.append(prereqLine(t.prereqs));
  const term = el("div", "wap-cmds");
  for (const c of t.commands) {
    if (c.note) term.append(el("div", "wap-cmd-note", "# " + c.note));
    term.append(el("div", "wap-cmd", "$ " + c.cmd));
  }
  w.append(term, el("p", "fineprint muted",
    "platformio.ini points src_dir at " + t.src_dir + " — the same sketch tree the Arduino IDE opens."));
  return w;
}

function arduinoPanel(t) {
  const w = el("div", "wap-tool");
  w.append(prereqLine(t.prereqs));
  const ol = el("ol", "wap-ritual");
  const li1 = el("li");
  li1.append(document.createTextNode("Boards Manager URL (File → Preferences), then install "),
    el("strong", null, t.board_pkg), document.createTextNode(":"),
    el("code", "wap-url", t.boards_url));
  ol.append(li1);
  const li2 = el("li");
  li2.append(document.createTextNode("Libraries (Tools → Manage Libraries):"));
  const ul = el("ul", "wap-btn-gestures");
  for (const lib of t.libraries) ul.append(el("li", null, lib));
  li2.append(ul);
  ol.append(li2);
  const li3 = el("li");
  li3.append(document.createTextNode("Board settings (Tools menu):"));
  const tab = el("div", "wap-facts wap-boardcfg");
  for (const [k, v] of t.board_config) {
    const row = el("div", "wap-fact");
    row.append(el("span", "wap-fact-k", k), el("span", "wap-fact-v", v));
    tab.append(row);
  }
  li3.append(tab);
  ol.append(li3);
  const li4 = el("li");
  li4.append(document.createTextNode("Open and upload: "), el("code", "wap-url", t.sketch));
  ol.append(li4);
  w.append(ol);
  return w;
}

// The strap-pin trainer: a miniature of the bench's power plane. BOOT is only
// sampled at reset — the whole lesson in one interactive truth.
function buildStrapTrainer() {
  const wrap = el("div", "wap-bench");
  const row = el("div", "wap-bench-row");
  const usb = el("button", "bench-chip", "");
  const boot = el("button", "bench-chip", "");
  const reset = el("button", "bench-chip bench-momentary", "");
  usb.append(el("strong", null, "USB-C cable"), el("span", "bench-chip-state"));
  boot.append(el("strong", null, "BOOT"), el("span", "bench-chip-state"));
  reset.append(el("strong", null, "RESET"), el("span", "bench-chip-state", "tap"));
  row.append(usb, boot, reset);
  const status = el("p", "wap-bench-status");
  const cons = el("pre", "wap-bench-console");
  wrap.append(row, status, cons);

  let plugged = false, held = false, mode = "off";
  const say = (t) => {
    cons.textContent = (cons.textContent + t + "\n").split("\n").slice(-9).join("\n");
    cons.scrollTop = cons.scrollHeight;
  };
  const sample = (how) => {
    // the mask ROM samples GPIO0 once, at reset — this line IS the lesson
    mode = held ? "download" : "run";
    for (const l of romBanner(mode === "download" ? "download" : how).split("\n")) if (l.trim()) say(l);
    say(mode === "download"
      ? "✓ download mode — the ROM waits. Click Upload in your IDE now."
      : "…app boots (the big banner above scrolls by).");
  };
  const paint = () => {
    usb.classList.toggle("on", plugged);
    usb.querySelector(".bench-chip-state").textContent = plugged ? "plugged" : "unplugged";
    boot.classList.toggle("on", held);
    boot.querySelector(".bench-chip-state").textContent = held ? "held" : "released";
    status.textContent = !plugged ? "⏚ no power — plug the cable in"
      : mode === "download" ? "ROM: waiting for download (flash now, then tap RESET)"
        : "app running — BOOT does nothing until the next reset";
  };
  usb.addEventListener("click", () => {
    plugged = !plugged;
    if (plugged) { say("— cable in —"); sample("poweron"); }
    else { mode = "off"; say("⏚ — cable out: port gone mid-anything is safe; NVS is flash — ⏚"); }
    paint();
  });
  boot.addEventListener("click", () => {
    held = !held;
    if (held && plugged && mode === "run") say("(BOOT held — nothing happens: it's only sampled at reset)");
    paint();
  });
  reset.addEventListener("click", () => {
    reset.classList.add("pressed");
    setTimeout(() => reset.classList.remove("pressed"), 160);
    if (!plugged) { say("(no power — nothing to reset)"); return; }
    say("— RESET tapped —");
    sample("reset");
    paint();
  });
  paint();
  return wrap;
}

// ── the on-device dashboard (a labeled sketch) ────────────────────────────
export function buildDashboard(data, bus) {
  const wrap = el("div", "wap-dash");
  const chrome = el("div", "hub-ha-chrome");
  chrome.append(el("span", "hub-term-dots"), el("span", "hub-ha-url", "http://canary.local/"));
  chrome.querySelector(".hub-term-dots").append(el("i"), el("i"), el("i"));

  const app = el("div", "wap-dash-app");
  const nav = el("div", "subtabs wap-dash-tabs");
  const app_body = el("div", "wap-dash-body");
  const tabs = ["Status", "Sensing", "Acoustic"];
  const panels = {};
  const tabBtns = {};
  const showTab = (name) => {
    for (const k in tabBtns) tabBtns[k].classList.toggle("on", k === name);
    for (const k in panels) panels[k].hidden = k !== name;
  };
  tabs.forEach((t, i) => {
    const b = el("button", "tab" + (i === 0 ? " on" : ""), t);
    tabBtns[t] = b;
    b.addEventListener("click", () => showTab(t));
    nav.append(b);
  });
  app.append(nav, app_body);

  // Status pill
  const pill = el("div", "wap-pill wap-pill-off", "Offline");
  const pillSub = el("p", "muted fineprint", "waiting for the radio…");
  const pStatus = el("div"); pStatus.append(pill, pillSub);
  // Sensing gauges
  const pSensing = el("div"); pSensing.hidden = true;
  const gauges = el("div", "wap-gauges");
  const gEls = {};
  for (const g of data.sensing.gauges) {
    const cell = el("div", "wap-gauge");
    const barWrap = el("div", "wap-gauge-bar");
    const fill = el("div", "wap-gauge-fill");
    barWrap.append(fill);
    cell.append(el("span", "wap-gauge-k", g), barWrap);
    gEls[g] = fill;
    gauges.append(cell);
  }
  const spec = el("div", "wap-spectrum");
  for (let i = 0; i < data.sensing.spectrum_bands; i++) spec.append(el("i"));
  pSensing.append(gauges, el("p", "fineprint muted", data.sensing.spectrum_bands + "-band CSI spectrum"), spec);
  // Acoustic card
  const pAcoustic = el("div"); pAcoustic.hidden = true;
  const acCard = el("div", "wap-ac-card");
  const acPill = el("div", "wap-ac-pill", "Listening — T3 / T4 cadences only");
  acCard.append(acPill, el("p", "fineprint muted",
    "no audio stored, ever — the mic is one loudness number per 20 ms, then the buffer is wiped. Not a UL-listed life-safety device."));
  pAcoustic.append(acCard);

  panels["Status"] = pStatus; panels["Sensing"] = pSensing; panels["Acoustic"] = pAcoustic;
  app_body.append(pStatus, pSensing, pAcoustic);

  const ribbon = el("p", "ondevice wap-note");
  ribbon.append(el("strong", null, "How to read this: "), document.createTextNode(
    "a faithful sketch of the on-device dashboard — the pills, gauges and cards are the getting-started guide's own, " +
    "but this is not the device's real 5,400-line frontend. The wasm display emulator elsewhere IS real firmware; this is a sketch."));
  wrap.append(chrome, app, ribbon);

  const PILL_CLASS = { Quiet: "quiet", Presence: "presence", Motion: "motion", Active: "active", Offline: "off" };
  function setPill(name, meaning) {
    pill.className = "wap-pill wap-pill-" + (PILL_CLASS[name] || "off");
    pill.textContent = name;
    if (meaning) pillSub.textContent = meaning;
  }
  const setGauge = (k, pct) => { if (gEls[k]) gEls[k].style.width = Math.max(4, Math.min(100, pct)) + "%"; }
  function danceSpectrum(hot) {
    [...spec.children].forEach((b) => { b.style.height = (hot ? 20 + Math.random() * 80 : 6 + Math.random() * 22) + "%"; });
  }

  bus.on("online", () => { setPill("Quiet", data.sensing.pills.find((p) => p.name === "Quiet")?.meaning); danceSpectrum(false); });
  bus.on("event", (e) => {
    const name = pillForEvent(e.id === "smoke" ? "motion" : e.id === "co" ? "motion" : e.event);
    const meaning = data.sensing.pills.find((p) => p.name === name)?.meaning;
    if (name) setPill(name, meaning);
    const hot = ["wave", "smoke", "co"].includes(e.id);
    setGauge("Motion", hot ? 78 : e.id === "sit" ? 16 : 8);
    setGauge("Breathing-band", e.id === "sit" ? 64 : 12);
    setGauge("Signal", 70);
    danceSpectrum(hot);
    if (e.id === "smoke") { acCard.classList.add("alarm"); acPill.textContent = "🔥 Smoke alarm pattern"; showTab("Acoustic"); }
    else if (e.id === "co") { acCard.classList.add("alarm"); acPill.textContent = "⚠ CO alarm pattern"; showTab("Acoustic"); }
    else if (e.id === "mute") { acCard.classList.add("muted"); acPill.textContent = "Microphone muted"; showTab("Acoustic"); }
    else if (["wave", "sit", "leave"].includes(e.id)) { acCard.classList.remove("alarm", "muted"); showTab("Sensing"); }
  });
  return wrap;
}

// ── the MQTT explorer ─────────────────────────────────────────────────────
export function buildMqtt(data, bus) {
  const m = data.mqtt, id = data.device.id_example;
  const wrap = el("div", "wap-mqtt");
  const bar = el("div", "hub-term-bar");
  bar.append(el("span", "hub-term-title", "MQTT · " + m.broker_uri.split("  ")[0].replace("<host>", "192.168.1.10")),
    el("span", "hub-term-sim", "topics + payloads from csi_mqtt.cpp"));
  wrap.append(bar);

  const cols = el("div", "wap-mqtt-cols");
  const retainedCol = el("div", "wap-mqtt-col");
  retainedCol.append(el("div", "wap-mqtt-h", "retained topics"));
  const retained = el("div", "wap-mqtt-tree");
  retainedCol.append(retained);
  const streamCol = el("div", "wap-mqtt-col");
  streamCol.append(el("div", "wap-mqtt-h", "live — " + withId(m.topic_pattern.replace("<suffix>", "events"), id)));
  const stream = el("div", "wap-mqtt-stream");
  streamCol.append(stream);
  cols.append(retainedCol, streamCol);
  wrap.append(cols);

  // HA discovery entities (collapsible)
  const disc = el("details", "wap-mqtt-disc");
  disc.append(el("summary", null, "Home Assistant discovery — " + m.discovery.counts.entities + " entities on " + m.discovery.prefix + "/…"));
  const dgrid = el("div", "wap-mqtt-ents");
  for (const e of m.discovery.entities) {
    const row = el("div", "wap-ent");
    row.append(el("span", "wap-ent-comp", e.component.replace("_", " ")),
      el("span", "wap-ent-name", e.name),
      el("code", "wap-ent-topic", withId(m.discovery.config_topic.replace("<component>", e.component).replace("<object_id>", e.object_id), id)));
    dgrid.append(row);
  }
  disc.append(dgrid);
  wrap.append(disc);

  const note = el("p", "ondevice wap-note");
  note.append(el("strong", null, "How to read this: "), document.createTextNode(
    "the topic tree and payloads are the exact strings csi_mqtt.cpp publishes (prefix " + m.prefix +
    ", only " + withId(m.topic_pattern.replace("<suffix>", "events"), id) + " is non-retained). The broker is staged; the contract is real."));
  wrap.append(note);

  const store = {};
  function renderRetained() {
    retained.innerHTML = "";
    const keys = Object.keys(store).sort();
    if (!keys.length) { retained.append(el("p", "muted fineprint", "nothing retained yet — the device isn't connected.")); return; }
    for (const k of keys) {
      const row = el("div", "wap-mqtt-row");
      row.append(el("code", "wap-mqtt-topic", k), el("code", "wap-mqtt-payload", store[k]));
      retained.append(row);
    }
  }
  function pushStream(topic, payload, cls) {
    const row = el("div", "wap-mqtt-ev " + (cls || ""));
    row.append(el("code", "wap-mqtt-topic", topic), el("code", "wap-mqtt-payload", payload));
    stream.prepend(row);
    while (stream.children.length > 7) stream.lastChild.remove();
  }
  renderRetained();

  bus.on("mqtt", () => {
    // LWT is replaced by online on connect, then the retained snapshot lands
    const seq = [];
    seq.push({ topic: withId(m.lwt.topic, id), payload: '{"online":true}', retain: true });
    for (const t of m.topics) {
      if (!t.retained) continue;
      seq.push({ topic: withId(m.topic_pattern.replace("<suffix>", t.suffix), id), payload: t.payload, retain: true });
    }
    (async () => {
      for (const msg of seq) {
        await sleep(160); if (!alive(wrap)) return;
        mqttApply(store, msg); renderRetained();
      }
      pushStream(withId(m.discovery.prefix + "/…/config", id), m.discovery.counts.entities + " entities announced (retained)", "disc");
    })();
  });
  bus.on("event", (e) => {
    for (const pub of e.mqtt || []) {
      const topic = withId(m.topic_pattern.replace("<suffix>", pub.suffix), id);
      const retain = pub.suffix !== "events";
      if (retain) { mqttApply(store, { topic, payload: pub.payload, retain: true }); renderRetained(); }
      pushStream(topic, pub.payload, retain ? "" : "live");
    }
  });
  return wrap;
}
