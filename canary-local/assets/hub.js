// canary-local/assets/hub.js — The Hub page driver.
//
// Renders homeassistant.html entirely from devices/homeassistant.json —
// the generated, drift-gated data (tools/gen_homeassistant.py). The page
// hardcodes no version, no entity name, no command: a Home Assistant
// release bump or an integration change re-lines every section without
// anyone editing this file.
//
// Self-report: the upstream snapshot carries its own fetched_at date and
// the page computes, out loud, how old it is. Fresh is silent; stale
// (>120 days — the freshness workflow runs weekly, so this means it has
// failed ~17 times in a row) turns the line amber and says which
// workflow to go look at. Failure posture: if the JSON can't load or
// misses required sections, the page degrades to a plain pointer at
// docs/homeassistant_setup.md — never a blank screen, never a stale
// version presented as current.

import { DeviceScene } from "./scene3d.js";
import { Assembly, M } from "./assembly.js";
import { PARTS } from "./hub-parts.js";
import { buildTerminal, daysOld } from "./hub-term.js";
import { buildHaDemo } from "./hub-ha-ui.js";
import { buildHubWizard } from "./hub-setup-wizard.js";

const $ = (sel, root = document) => root.querySelector(sel);
const el = (tag, cls, text) => {
  const n = document.createElement(tag);
  if (cls) n.className = cls;
  if (text != null) n.textContent = text;
  return n;
};
const GH = "https://github.com/kmay89/securaCV/blob/main/";

async function main() {
  const mount = $("#hub");
  let data;
  try {
    const res = await fetch("devices/homeassistant.json");
    if (!res.ok) throw new Error("HTTP " + res.status);
    data = await res.json();
    for (const k of ["integration", "upstream", "why", "hardware", "terminal", "ha_demo"])
      if (!data[k]) throw new Error("missing section: " + k);
  } catch (e) {
    mount.append(el("p", "muted",
      "The Hub's data failed to load (" + e.message + "). Everything on this " +
      "page is also readable, un-animated, in docs/homeassistant_setup.md."));
    const a = el("a", null, "Open the written guide →");
    a.href = GH + "docs/homeassistant_setup.md";
    mount.append(a);
    return;
  }

  const vars = {
    haos: data.upstream.haos_version,
    ha: data.upstream.ha_version,
    integration: data.integration.version,
    min_ha: data.integration.min_ha,
  };

  renderVersionStrip(data, vars);
  renderWhy(data.why);
  renderWizard(data);
  renderHardware(data.hardware);
  renderTerminal(data.terminal, vars);
  renderDemo(data.ha_demo, vars);
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

  // ── version strip + staleness self-report ──
  function renderVersionStrip(d, v) {
    const strip = $("#hub-versions");
    if (!strip) return;
    strip.innerHTML = "";
    const chips = el("div", "hub-chips");
    for (const [label, val] of [
      ["Home Assistant", v.ha],
      ["HA OS image", v.haos],
      ["integration", "v" + v.integration + " via HACS"],
      ["needs HA ≥", v.min_ha],
      ["firmware train", d.fw_train],
    ]) {
      const c = el("span", "chip");
      c.append(el("span", "hub-chip-k", label + " "), el("strong", null, val));
      chips.append(c);
    }
    strip.append(chips);

    const age = daysOld(d.upstream.fetched_at);
    const line = el("p", "fineprint hub-fresh");
    if (age == null) {
      line.textContent = "upstream snapshot date unreadable — treat versions with suspicion";
      line.classList.add("stale");
    } else {
      line.textContent =
        `upstream versions snapshotted ${d.upstream.fetched_at} (${age} day${age === 1 ? "" : "s"} ago) ` +
        `from ${new URL(d.upstream.source).host} · refreshed weekly by CI`;
      if (age > 120) {
        line.classList.add("stale");
        line.textContent += " — that's long enough that the homeassistant-freshness " +
          "workflow has likely been failing; check the repo's Actions tab";
      }
    }
    strip.append(line);
  }

  // ── §why ──
  function renderWhy(why) {
    const s = section("why", "why a hub at all", "Witnesses converge somewhere",
      "Every Canary is complete alone. Home Assistant is what turns N lone " +
      "witnesses into a household.");
    const grid = el("div", "hub-why");
    for (const w of why) {
      const c = el("div", "hub-why-card");
      c.append(el("h3", null, w.title), el("p", "body", w.body));
      grid.append(c);
    }
    s.append(grid);
  }

  // ── §setup wizard: hand-hold HA + MQTT so nobody gives up ──
  function renderWizard(d) {
    const s = section("setup", "set it up, step by step",
      "Let’s get your hub running — together",
      "Home Assistant and MQTT, explained plainly and walked one small step at a " +
      "time. Every step tells you exactly what to type and has a “Stuck?” way " +
      "forward, so you can’t get lost. It saves your place as you go.");
    buildHubWizard(s, d);
  }

  // ── §hardware: needs list + the assembly stage ──
  function renderHardware(hw) {
    const s = section("build", "the hardware", hw.title, hw.intro);

    const needs = el("div", "hub-needs");
    for (const n of hw.needs) {
      const row = el("div", "hub-need");
      row.append(el("strong", null, n.item), el("span", "muted", n.note));
      if (n.from_doc) row.append(el("span", "chip chip-dim hub-need-src", "from the setup guide"));
      needs.append(row);
    }
    s.append(needs);

    const ribbon = el("p", "ondevice");
    ribbon.append(el("strong", null, "How to read this: "), document.createTextNode(
      "real board dimensions (Pi 4B, 85 × 56 mm), a deliberately generic case, " +
      "staged choreography."));
    s.append(ribbon);

    const stage = el("div", "asmlab-stage hub-stage");
    const cv = el("canvas", "asmlab-3d");
    stage.append(cv, el("div", "asmlab-hint", "drag to orbit · scroll to zoom"));
    const controls = el("div", "asm-controls");
    const info = el("div", "asm-info");
    s.append(stage, controls, info);

    const scene = new DeviceScene(cv, null);
    const asm = new Assembly(scene);
    cv.__scene = scene; cv.__asm = asm;
    const obs = new IntersectionObserver(() => {
      if (!document.body.contains(cv)) { asm.unmount(); scene.stop(); obs.disconnect(); }
    });
    obs.observe(cv);

    for (const p of hw.parts) {
      asm.add({
        id: p.id, meshes: PARTS[p.part](p.params || {}).map((m) => ({ ...m })),
        seated: p.seated, explode: p.explode, insert: p.insert,
        step: p.step, name: p.name, qty: p.qty, ref: p.ref,
      });
    }

    // auto-frame from the exploded extent (assembly-lab's recipe)
    let lo = [1e9, 1e9, 1e9], hi = [-1e9, -1e9, -1e9];
    for (const part of asm.parts) {
      const w = M.mul(M.t(part.explode[0], part.explode[1], part.explode[2]), part.seated);
      for (let i = 0; i < 3; i++) {
        lo[i] = Math.min(lo[i], w[12 + i] - 20); hi[i] = Math.max(hi[i], w[12 + i] + 20);
      }
    }
    const span = Math.max(hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]);
    const f = hw.frame || {};
    scene.dist = span * (f.pad || 2.3);
    scene.rot = { x: f.rx ?? -0.5, y: f.ry ?? 0.7 };
    scene.home = { ...scene.rot };
    scene.viewY = (hi[2] + lo[2]) / 2;
    asm.mount();

    buildPlayer(hw, asm, scene, cv, controls, info, f);
  }

  // the exploded/step player — assembly-lab.js's UI, hub-sized
  function buildPlayer(hw, asm, scene, cv, controls, info, frame) {
    const steps = hw.steps || [];
    const modeRow = el("div", "subtabs asm-mode");
    const bExpl = el("button", "tab on", "Exploded");
    const bStep = el("button", "tab", "Step by step");
    modeRow.append(bExpl, bStep);
    controls.append(modeRow);

    const explWrap = el("div", "asm-explode");
    const slider = document.createElement("input");
    slider.type = "range"; slider.min = "0"; slider.max = "100"; slider.value = "100"; slider.className = "asm-slider";
    const sLabel = el("span", "muted", "exploded");
    explWrap.append(el("span", "muted", "together"), slider, sLabel);
    controls.append(explWrap);

    const stepWrap = el("div", "asm-step");
    stepWrap.hidden = true;
    const rail = el("div", "asm-rail");
    const dots = steps.map((_, i) => {
      const d = el("button", "asm-dot"); d.title = steps[i].title;
      d.addEventListener("click", () => go(i));
      rail.append(d); return d;
    });
    const nav = el("div", "asm-nav");
    const prev = el("button", "ghost", "‹ back");
    const play = el("button", "primary", "▶ play");
    const next = el("button", "primary", "next ›");
    const counter = el("span", "muted asm-counter");
    nav.append(prev, play, counter, next);
    stepWrap.append(rail, nav);
    controls.append(stepWrap);

    const card = el("div", "asm-card");
    info.append(card);

    let cur = 0, playing = false, timer = null;

    const renderCard = (i) => {
      card.innerHTML = "";
      const s = steps[i];
      card.append(el("div", "asm-step-n", `Step ${i + 1} of ${steps.length}`));
      card.append(el("h4", "asm-title", s.title));
      card.append(el("p", "body", s.note || ""));
      const badges = el("div", "asm-badges");
      const seen = new Set();
      for (const p of asm.parts) {
        if (p.step !== i || seen.has(p.name)) continue;
        seen.add(p.name);
        const b = el("span", "asm-badge");
        b.append(document.createTextNode(p.name));
        if (p.qty > 1) b.append(el("span", "asm-qty", "×" + p.qty));
        if (p.ref) b.append(el("code", "asm-ref", p.ref));
        badges.append(b);
      }
      if (badges.children.length) card.append(badges);
    };

    const go = (i) => {
      cur = Math.max(0, Math.min(steps.length - 1, i));
      asm.showStep(cur, { animate: true });
      dots.forEach((d, k) => d.classList.toggle("on", k <= cur));
      dots.forEach((d, k) => d.classList.toggle("active", k === cur));
      counter.textContent = `${cur + 1} / ${steps.length}`;
      prev.disabled = cur === 0;
      next.textContent = cur === steps.length - 1 ? "finish ✓" : "next ›";
      renderCard(cur);
      const t = steps.length > 1 ? cur / (steps.length - 1) : 0;
      asm.tweenCamera({ x: (frame.rx ?? -0.5) - t * 0.1, y: (frame.ry ?? 0.7) + t * 0.16, d: scene.dist });
    };

    const stop = () => { playing = false; play.textContent = "▶ play"; if (timer) clearTimeout(timer); timer = null; };
    const advance = () => {
      if (!playing || !document.body.contains(cv) || cur === steps.length - 1) { stop(); return; }
      go(cur + 1);
      timer = setTimeout(advance, 2100);
    };
    play.addEventListener("click", () => {
      if (playing) { stop(); return; }
      playing = true; play.textContent = "⏸ pause";
      if (cur === steps.length - 1) go(0);
      timer = setTimeout(advance, 900);
    });
    prev.addEventListener("click", () => { stop(); go(cur - 1); });
    next.addEventListener("click", () => { stop(); cur === steps.length - 1 ? go(0) : go(cur + 1); });
    slider.addEventListener("input", () => {
      const t = Number(slider.value) / 100;
      asm.setExplode(t);
      sLabel.textContent = t < 0.02 ? "together" : t > 0.98 ? "exploded" : `${Math.round(t * 100)}%`;
    });
    const setMode = (step) => {
      bStep.classList.toggle("on", step); bExpl.classList.toggle("on", !step);
      stepWrap.hidden = !step; explWrap.hidden = step; card.hidden = !step;
      if (step) go(cur); else { stop(); asm.setExplode(Number(slider.value) / 100); }
    };
    bExpl.addEventListener("click", () => setMode(false));
    bStep.addEventListener("click", () => setMode(true));
    cv.tabIndex = 0;
    cv.addEventListener("keydown", (e) => {
      if (stepWrap.hidden) return;
      if (e.key === "ArrowRight") { e.preventDefault(); stop(); go(cur + 1); }
      else if (e.key === "ArrowLeft") { e.preventDefault(); stop(); go(cur - 1); }
    });

    card.hidden = true;
    asm.setExplode(1);
  }

  // ── §terminal ──
  function renderTerminal(term, v) {
    const s = section("terminal", "the software", "The bench terminal",
      "You can't dd a card from a web page, so the bench replays the real " +
      "commands with recorded output — versions live from the snapshot above.");
    s.append(buildTerminal(term, v));
  }

  // ── §demo ──
  function renderDemo(demo, v) {
    const s = section("payoff", "the payoff", "Thirty seconds after the broker",
      "A Canary joins the network and announces itself — no YAML, no pairing " +
      "codes. Flip the mic mute; run the smoke-alarm drill.");
    s.append(buildHaDemo(demo, v));
  }

  // ── §keep going ──
  function renderKeepGoing(d) {
    const s = section("more", "keep going", "Where this goes next", null);
    const grid = el("div", "hub-links");
    const items = [
      ["The full written guide", "every step, plus kernel mode and troubleshooting", d.docs.setup],
      ["Alert blueprints", "one-click alerts — smoke/CO, tamper, chain failure, offline", d.docs.blueprints],
      ["The Verified Timeline card", "the ✓-timeline card from the demo, for your real dashboard", d.docs.timeline_card],
      ["Device trust & PKI", "what pinned keys mean; pinning stricter than TOFU", d.docs.device_trust],
      ["Firmware updates from HA", "signed pull-OTA with automatic rollback", d.docs.firmware_ota],
      ["Add cameras with Frigate", "sealed evidence from RTSP cameras on the same Pi", d.docs.frigate],
    ];
    for (const [title, body, path] of items) {
      const a = el("a", "hub-link");
      a.href = GH + path;
      a.target = "_blank"; a.rel = "noopener noreferrer";
      a.append(el("strong", null, title), el("span", "muted", body), el("code", "fineprint", path));
      grid.append(a);
    }
    s.append(grid);
  }
}

main();
