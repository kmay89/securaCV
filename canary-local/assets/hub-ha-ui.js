// canary-local/assets/hub-ha-ui.js — a faithful sketch of Home Assistant.
//
// Honesty first: unlike the display emulator (which is the shipping
// firmware in wasm), this is NOT Home Assistant's frontend — it is a
// staged sketch of it. What keeps it honest is that everything it shows
// is drift-gated: the entity names come from docs/homeassistant_setup.md
// §Step 4 (parsed by gen_homeassistant.py — the demo cannot show an
// entity the doc stopped promising), the topics are the doc's own MQTT
// contract, and the behaviors (mic toggles signed into the chain, smoke
// cadence → critical push) restate what the integration actually does.
//
// The set pieces:
//   · MQTT discovery, live: entities pop into the device card one by one,
//     each stamped with the retained config topic that announced it
//   · the Verified Timeline card with real-looking ✓ rows
//   · a working Microphone Mute switch (and its signed chain event)
//   · the smoke-alarm drill: sensor latches ON, the blueprint automation
//     fires, a phone notification slides in — the whole "why", in 8 s

const el = (tag, cls, text) => {
  const n = document.createElement(tag);
  if (cls) n.className = cls;
  if (text != null) n.textContent = text;
  return n;
};

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const slug = (name) => name.toLowerCase().replace(/[^a-z0-9]+/g, "_").replace(/^_|_$/g, "");

export function buildHaDemo(demo, vars) {
  const wrap = el("div", "hub-ha");

  // browser-ish chrome so it reads as "this is a web app on your LAN"
  const chrome = el("div", "hub-ha-chrome");
  const url = el("span", "hub-ha-url", "http://homeassistant.local:8123/lovelace/witnesses");
  chrome.append(el("span", "hub-term-dots hub-ha-dots"), url);
  chrome.querySelector(".hub-term-dots").append(el("i"), el("i"), el("i"));

  const app = el("div", "hub-ha-app");
  const side = el("nav", "hub-ha-side");
  for (const [glyph, label, on] of [
    ["▦", "Overview", true], ["⚡", "Energy", false], ["🗺", "Map", false],
    ["◔", "History", false], ["🧩", "HACS", false], ["⚙", "Settings", false],
  ]) {
    const item = el("div", "hub-ha-nav" + (on ? " on" : ""));
    item.append(el("span", "hub-ha-glyph", glyph), el("span", null, label));
    side.append(item);
  }

  const main = el("div", "hub-ha-main");
  const top = el("div", "hub-ha-top");
  top.append(el("span", "hub-ha-title", "Witnesses"),
             el("span", "hub-ha-ver", "Home Assistant " + vars.ha));
  const grid = el("div", "hub-ha-grid");
  main.append(top, grid);
  app.append(side, main);

  // ── device card (fills via "discovery") ──
  const devCard = el("div", "hub-ha-card");
  devCard.append(el("h4", null, demo.device_name));
  const discovery = el("p", "hub-ha-wait", "waiting for MQTT discovery…");
  devCard.append(discovery);
  const rows = el("div", "hub-ha-rows");
  devCard.append(rows);

  // ── timeline card ──
  const tlCard = el("div", "hub-ha-card");
  tlCard.append(el("h4", null, "SecuraCV Verified Timeline"));
  const tlStatus = el("div", "hub-ha-chain", "chain — · verifying…");
  const tl = el("div", "hub-ha-tl");
  tlCard.append(tlStatus, tl);

  // ── automation card ──
  const autoCard = el("div", "hub-ha-card");
  autoCard.append(el("h4", null, demo.drill.automation));
  const autoState = el("div", "hub-ha-auto idle", "idle — armed");
  const autoTrace = el("div", "hub-ha-trace");
  autoCard.append(autoState, autoTrace);
  const drillBtn = el("button", "primary hub-ha-drill", demo.drill.label);
  const drillNote = el("p", "muted fineprint", demo.drill.time_note);
  autoCard.append(drillBtn, drillNote);

  grid.append(devCard, tlCard, autoCard);

  // phone notification toast
  const toast = el("div", "hub-ha-toast");
  toast.append(
    el("div", "hub-ha-toast-app", "◱ Home Assistant · now"),
    el("div", "hub-ha-toast-title", demo.drill.notification.title),
    el("div", "hub-ha-toast-body", demo.drill.notification.body));

  wrap.append(chrome, app, toast);
  const ribbon = el("p", "ondevice hub-ha-note");
  ribbon.append(el("strong", null, "How to read this: "), document.createTextNode(demo.note));
  wrap.append(ribbon);

  // ── state ──
  const entState = new Map(); // name → {row, valueEl, value}
  let seq = 1284;
  let discovered = false;
  let drilling = false;

  function timelineAdd(text, cls = "") {
    const row = el("div", "hub-ha-tlrow " + cls);
    const t = new Date();
    const hh = String(t.getHours()).padStart(2, "0") + ":" + String(t.getMinutes()).padStart(2, "0");
    seq += 1;
    row.append(
      el("span", "hub-ha-tltime", hh),
      el("span", "hub-ha-tltext", text),
      el("span", "hub-ha-tlsig", "✓ #" + seq));
    tl.prepend(row);
    while (tl.children.length > 6) tl.lastChild.remove();
    tlStatus.textContent = `chain ${seq} · verified ✓ (Ed25519, pinned key)`;
  }

  function setValue(name, value, alert = false) {
    const s = entState.get(name);
    if (!s) return;
    s.value = value;
    s.valueEl.textContent = value;
    s.row.classList.toggle("alert", alert);
  }

  function entityRow(ent) {
    const row = el("div", "hub-ha-row");
    const icon = el("span", "hub-ha-icon", ent.icon || "•");
    const nameWrap = el("div", "hub-ha-name");
    nameWrap.append(el("span", null, ent.name));
    if (ent.note) nameWrap.append(el("span", "hub-ha-desc", ent.note));
    const value = el("span", "hub-ha-value");

    if (ent.kind === "switch") {
      const sw = el("button", "hub-ha-switch", "");
      sw.setAttribute("role", "switch");
      sw.setAttribute("aria-checked", "false");
      sw.setAttribute("aria-label", ent.name);
      sw.addEventListener("click", () => {
        const onNow = sw.classList.toggle("on");
        sw.setAttribute("aria-checked", String(onNow));
        timelineAdd(`mic ${onNow ? "muted" : "live"} (source: ha) — signed into the witness chain`,
                    onNow ? "warn" : "");
      });
      value.append(sw);
    } else {
      value.textContent = ent.initial + (ent.unit ? " " + ent.unit : "");
    }
    row.append(icon, nameWrap, value);

    if (ent.attributes) {
      row.classList.add("has-attrs");
      const attrs = el("div", "hub-ha-attrs");
      for (const [k, v] of Object.entries(ent.attributes))
        attrs.append(el("code", null, `${k}: ${v}`));
      row.append(attrs);
      row.addEventListener("click", () => row.classList.toggle("open"));
    }
    entState.set(ent.name, { row, valueEl: value, value: ent.initial });
    return row;
  }

  async function playDiscovery() {
    if (discovered) return;
    discovered = true;
    discovery.textContent = "retained config topics arriving…";
    for (const ent of demo.entities) {
      await sleep(260);
      if (!document.body.contains(wrap)) return;
      const row = entityRow(ent);
      row.classList.add("landing");
      rows.append(row);
      discovery.textContent =
        `homeassistant/${ent.kind}/securacv_${slug(ent.name)}/config  (retained)`;
      requestAnimationFrame(() => row.classList.remove("landing"));
    }
    await sleep(500);
    discovery.textContent = "discovered in " + (demo.entities.length * 0.26).toFixed(1) +
      " s — on a real network: under 30 s from first MQTT publish";
    timelineAdd("device discovered — key pinned on first contact (TOFU)");
    // gentle liveness: uptime/witness count tick so the card feels inhabited
    liveTick();
  }

  let tickTimer = null;
  function liveTick() {
    if (tickTimer) return;
    let count = 1284;
    tickTimer = setInterval(() => {
      if (!document.body.contains(wrap)) { clearInterval(tickTimer); return; }
      if (Math.random() < 0.4 && !drilling) {
        count += 1;
        setValue("Witness Count", count.toLocaleString() + " records");
      }
    }, 4000);
  }

  async function drill() {
    if (drilling || !discovered) return;
    drilling = true;
    drillBtn.disabled = true;

    setValue(demo.drill.trigger_entity, "on", true);
    timelineAdd("acoustic_event: smoke_t3 — NFPA 72 T3 cadence matched", "alert");
    await sleep(500);

    autoState.className = "hub-ha-auto firing";
    autoState.textContent = "triggered — " + new Date().toLocaleTimeString();
    autoTrace.innerHTML = "";
    for (const step of [
      "trigger: binary_sensor → 'Smoke Alarm Heard' to 'on'",
      "condition: none (critical path)",
      "action: notify.mobile_app — critical, bypasses silent mode",
    ]) {
      await sleep(420);
      if (!document.body.contains(wrap)) return;
      autoTrace.append(el("div", "hub-ha-tracestep", "✓ " + step));
    }
    toast.classList.add("show");
    await sleep(demo.drill.clear_after_s * 1000);
    if (!document.body.contains(wrap)) return;

    toast.classList.remove("show");
    setValue(demo.drill.trigger_entity, "off", false);
    timelineAdd("acoustic_event cleared — alarm stopped");
    autoState.className = "hub-ha-auto idle";
    autoState.textContent = "idle — armed";
    drilling = false;
    drillBtn.disabled = false;
  }

  drillBtn.addEventListener("click", drill);

  // discovery plays itself when the demo scrolls into view
  const obs = new IntersectionObserver((entries) => {
    if (entries.some((e) => e.isIntersecting)) { playDiscovery(); obs.disconnect(); }
  }, { threshold: 0.35 });
  obs.observe(app);

  return wrap;
}
