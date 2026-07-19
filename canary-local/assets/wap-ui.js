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
  const hint = el("span", "muted fineprint", "boots from devices/wap.json — the firmware's own boot log");
  controls.append(btnPwr, btnSkip, hint);
  wrap.append(bar, scroll, controls);

  const lines = bootLines(data.serial);
  let booting = false, booted = false;

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

  // once the device is on the network, stream the join line + start the table
  let tableStarted = false;
  function startTable() {
    if (tableStarted) return;
    tableStarted = true;
    put("wap-net", data.serial.join_line);
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

  // ---- screens -----------------------------------------------------------
  function screenLocked() {
    screen.innerHTML = "";
    const s = el("div", "wap-screen wap-screen-wait");
    s.append(el("div", "wap-big", "⏻"),
      el("p", "muted", "Power the Canary on (in the serial console) — within ten seconds it brings up its setup network."),
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
      "WPA2, one client at a time. Password " + ap.password_example + " (yours is unique — the box card / first-boot serial line)."));
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
    const s = el("div", "wap-screen wap-screen-browser");
    s.append(browserChrome("canary.local/companion"));
    const body = el("div", "wap-wiz wap-wiz-done");
    body.append(el("div", "wap-check", "✓"));
    if (standaloneMode) {
      body.append(el("h4", "wap-wiz-h", "Running standalone."),
        el("p", "muted wap-wiz-sub", "Your Canary keeps its own SecuraCV network — no home WiFi needed, nothing leaves the device."));
    } else {
      body.append(el("h4", "wap-wiz-h", "Your Canary is online."),
        el("p", "muted wap-wiz-sub", "Joined " + ssid + ". The SecuraCV setup network turns itself off in about " +
          ap.ap_grace_sec + " seconds — reconnect this phone to your home WiFi and find your Canary at canary.local."));
    }
    // optional Home Assistant hookup (step 5 HA block)
    const ha = el("div", "wap-ha-block");
    ha.append(el("h5", null, "Use Home Assistant? (optional)"));
    const host = el("input", "wap-input"); host.placeholder = "Broker address (like 192.168.1.10)";
    ha.append(host);
    const row = el("div", "wap-wiz-btns");
    const skip = el("button", "ghost", "Skip"); skip.addEventListener("click", () => finish(false));
    const save = el("button", "primary", "Test & save"); save.addEventListener("click", () => finish(true));
    row.append(skip, save);
    ha.append(row);
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
    bus.emit("online", { ha: withHa });
    if (withHa) bus.emit("mqtt");
    const done = screen.querySelector(".wap-ha-block");
    if (done) { done.innerHTML = ""; done.append(el("p", "ok", withHa ? "✓ Broker saved — entities announcing on Home Assistant." : "✓ Finished. Name it by its room from the dashboard.")); }
  }

  bus.on("ap", () => { if (phase === "off") { setPhase("ap"); screenWifi(); } });
  screenLocked();
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
