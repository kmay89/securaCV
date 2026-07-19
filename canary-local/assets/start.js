// canary-local/assets/start.js — the Get Started guide driver.
//
// Renders start.html entirely from devices/start.json (gen_start.py):
// mission picker → OS picker (macOS / Windows / Linux, remembered) →
// chapters of steps with one-tap-copy commands in the bench terminal's
// visual voice. Gamified honestly: steps check off, a progress bar
// fills, the canary celebrates — and all of it lives in localStorage
// on this device only. No accounts, no telemetry, nothing phones.
//
// Split the Hub's way: a DOM-free core (variant resolution + progress
// model, tested in tests/start.test.js) and a renderer below it.

import { expandVars, copyText, daysOld } from "./hub-term.js";

// ── core (DOM-free) ─────────────────────────────────────────────────────

// Which variant a step shows for a given OS: exact match, else "all".
// A step with neither is a build bug the tests refuse.
export function variantFor(step, osId) {
  return step.variants[osId] || step.variants.all || null;
}

// What to render when the OS may not be chosen yet. Before a choice we
// show only OS-agnostic content — never a guessed platform's commands
// (a Mac visitor must not see, or copy, a Linux dd invocation).
export function variantToRender(step, osId) {
  return osId ? variantFor(step, osId) : (step.variants.all || null);
}

// Flat list of [chapterIdx, stepIdx] pairs — the progress denominator.
export function stepKeys(mission) {
  const keys = [];
  mission.chapters.forEach((ch, ci) =>
    ch.steps.forEach((_, si) => keys.push(ci + ":" + si)));
  return keys;
}

// Progress model over a plain storage object ({get,set} — localStorage
// in the page, a Map-backed shim in tests).
export function createProgress(missionId, store) {
  const KEY = "securacv.start." + missionId;
  let done;
  try { done = new Set(JSON.parse(store.get(KEY) || "[]")); }
  catch { done = new Set(); }
  const save = () => store.set(KEY, JSON.stringify([...done]));
  return {
    has: (k) => done.has(k),
    toggle(k) { done.has(k) ? done.delete(k) : done.add(k); save(); return done.has(k); },
    count: () => done.size,
    reset() { done.clear(); save(); },
  };
}

export const OS_KEY = "securacv.start.os";

// ── renderer ────────────────────────────────────────────────────────────
const $ = (sel, root = document) => root.querySelector(sel);
const el = (tag, cls, text) => {
  const n = document.createElement(tag);
  if (cls) n.className = cls;
  if (text != null) n.textContent = text;
  return n;
};
const GH = "https://github.com/kmay89/securaCV/blob/main/";

const storage = {
  get: (k) => { try { return localStorage.getItem(k); } catch { return null; } },
  set: (k, v) => { try { localStorage.setItem(k, v); } catch { /* private mode */ } },
};

async function main() {
  const mount = $("#start");
  let data;
  try {
    const res = await fetch("devices/start.json");
    if (!res.ok) throw new Error("HTTP " + res.status);
    data = await res.json();
    for (const k of ["oses", "missions", "upstream", "integration"])
      if (!data[k]) throw new Error("missing section: " + k);
  } catch (e) {
    mount.append(el("p", "muted",
      "The guide's data failed to load (" + e.message + "). The same paths " +
      "are written out in the documentation index."));
    const a = el("a", null, "Open the written docs →");
    a.href = GH + "docs/README.md";
    mount.append(a);
    return;
  }

  const vars = {
    haos: data.upstream.haos_version,
    ha: data.upstream.ha_version,
    integration: data.integration.version,
    min_ha: data.integration.min_ha,
  };

  renderVersionStrip(data);

  let os = storage.get(OS_KEY);
  if (!data.oses.some((o) => o.id === os)) os = null;

  const picker = el("section", "hub-section start-picker");
  picker.append(el("div", "hub-kicker", "step zero"), el("h2", null, "Where are you starting from?"));
  const missionGrid = el("div", "start-missions");
  picker.append(missionGrid);

  const osRow = el("div", "start-osrow");
  osRow.append(el("span", "muted", "My computer: "));
  const osBtns = new Map();
  for (const o of data.oses) {
    const b = el("button", "tab", o.glyph + " " + o.label);
    b.addEventListener("click", () => setOs(o.id));
    osBtns.set(o.id, b);
    osRow.append(b);
  }
  picker.append(osRow);
  mount.append(picker);

  const missionMount = el("div");
  mount.append(missionMount);

  let mission = null;

  function setOs(id) {
    os = id;
    storage.set(OS_KEY, id);
    for (const [k, b] of osBtns) b.classList.toggle("on", k === id);
    if (mission) openMission(mission); // re-render steps under the new OS
  }

  function missionCard(m) {
    const c = el("button", "start-mission");
    c.append(el("span", "start-glyph", m.glyph));
    const body = el("span", "start-mission-body");
    body.append(el("strong", null, m.title), el("span", "muted", m.line));
    const meta = el("span", "start-meta");
    meta.append(el("span", "chip", m.time), el("span", "chip chip-dim", m.difficulty));
    body.append(meta);
    c.append(body);
    c.addEventListener("click", () => {
      mission = m;
      [...missionGrid.children].forEach((x, i) =>
        x.classList.toggle("on", data.missions[i] === m));
      openMission(m);
      missionMount.scrollIntoView({ behavior: "smooth", block: "start" });
    });
    return c;
  }
  for (const m of data.missions) missionGrid.append(missionCard(m));

  function openMission(m) {
    missionMount.innerHTML = "";
    const progress = createProgress(m.id, storage);
    const keys = stepKeys(m);

    const head = el("section", "hub-section start-head");
    head.append(el("div", "hub-kicker", "your path"), el("h2", null, m.glyph + " " + m.title));

    // the honest scoreboard: N of M, a filling bar, a canary that reacts
    const barWrap = el("div", "start-bar");
    const bar = el("div", "start-bar-fill");
    barWrap.append(bar);
    const score = el("p", "muted start-score");
    const bird = el("span", "start-bird", "🐤");
    head.append(barWrap, score, bird);
    missionMount.append(head);

    const refresh = () => {
      const n = keys.filter((k) => progress.has(k)).length;
      const t = keys.length;
      bar.style.width = (t ? Math.round((n / t) * 100) : 0) + "%";
      score.textContent = n + " of " + t + " steps done" + (n === t && t ? " — every step ✓" : "");
      bird.textContent = n === 0 ? "🥚" : n < t ? "🐤" : "🎉";
      bird.title = n === 0 ? "an egg — let's hatch this"
        : n < t ? "hatched and working on it" : "fully fledged!";
    };

    m.chapters.forEach((ch, ci) => {
      const s = el("section", "hub-section start-chapter");
      s.append(el("h3", null, ch.title));
      if (ch.intro) s.append(el("p", "hub-lede", expandVars(ch.intro, vars)));
      ch.steps.forEach((step, si) => s.append(stepCard(step, ci + ":" + si)));
      missionMount.append(s);
    });

    const reset = el("button", "ghost small", "start this path over");
    reset.addEventListener("click", () => { progress.reset(); openMission(m); });
    missionMount.append(reset);

    function stepCard(step, key) {
      const card = el("div", "start-step");
      const row = el("div", "start-step-row");
      const check = el("button", "start-check");
      check.setAttribute("aria-label", "Mark step done: " + step.title);
      const setCheck = () => {
        const on = progress.has(key);
        check.textContent = on ? "✓" : "";
        check.classList.toggle("on", on);
        card.classList.toggle("done", on);
      };
      check.addEventListener("click", () => { progress.toggle(key); setCheck(); refresh(); });
      row.append(check, el("h4", null, step.title));
      if (step.doc) {
        const a = el("a", "fineprint start-doc", "the written version →");
        a.href = GH + step.doc; a.target = "_blank"; a.rel = "noopener noreferrer";
        row.append(a);
      }
      card.append(row);

      const v = variantToRender(step, os);
      if (!os && !step.variants.all) {
        card.append(el("p", "muted fineprint start-nudge",
          "⬆ Pick your computer above — the exact steps differ per system."));
      }
      if (v) {
        for (const b of v.bullets || []) card.append(el("p", "body start-bullet", "· " + expandVars(b, vars)));
        if (v.danger) {
          const d = el("p", "start-danger", "⚠ " + v.danger);
          card.append(d);
        }
        for (const c of v.cmds || []) card.append(cmdBlock(c));
        for (const c of [...(v.copies || []), ...(step.copies_all || [])]) card.append(copyRow(c));
        for (const l of v.links || []) {
          const a = el("a", "start-link", l.label + " →");
          a.href = l.href;
          if (/^https?:/.test(l.href)) { a.target = "_blank"; a.rel = "noopener noreferrer"; }
          card.append(a);
        }
      }
      if (step.note) card.append(el("p", "ondevice start-note", expandVars(step.note, vars)));
      if (step.next_mission) {
        const nm = data.missions.find((x) => x.id === step.next_mission);
        if (nm) {
          const b = el("button", "primary small", "continue: " + nm.title + " →");
          b.addEventListener("click", () => {
            mission = nm;
            [...missionGrid.children].forEach((x, i) =>
              x.classList.toggle("on", data.missions[i] === nm));
            openMission(nm);
            missionMount.scrollIntoView({ behavior: "smooth", block: "start" });
          });
          card.append(b);
        }
      }
      setCheck();
      return card;
    }

    // a real command in the bench's clothes: prompt, one-tap copy,
    // expected output behind a disclosure so phones aren't walled in text
    function cmdBlock(c) {
      const cmd = expandVars(c.cmd, vars);
      const win = el("div", "start-cmd");
      const line = el("div", "hub-line");
      line.append(el("span", "hub-prompt", "$ "), el("span", "hub-cmd", cmd));
      const copy = el("button", "hub-copy", "⧉");
      copy.title = "Copy this command";
      copy.setAttribute("aria-label", "Copy command: " + cmd);
      copy.addEventListener("click", () => {
        copyText(cmd).then((ok) => {
          copy.textContent = ok ? "✓" : "⧉";
          copy.classList.toggle("copied", ok);
          setTimeout(() => { copy.textContent = "⧉"; copy.classList.remove("copied"); }, 1200);
        });
      });
      line.append(copy);
      win.append(line);
      const out = (c.out || []).map((l) => expandVars(l, vars)).filter(Boolean);
      if (out.length) {
        const det = document.createElement("details");
        det.className = "start-expect";
        const sum = document.createElement("summary");
        sum.textContent = "what you should see";
        det.append(sum);
        for (const l of out) det.append(el("div", "hub-line hub-out", l));
        win.append(det);
      }
      if (c.note) win.append(el("p", "fineprint muted start-cmd-note", expandVars(c.note, vars)));
      return win;
    }

    // a copyable non-command string (URLs to paste into UIs)
    function copyRow(c) {
      const row = el("div", "start-cmd start-copyrow");
      const line = el("div", "hub-line");
      line.append(el("span", "hub-out", c.label + ": "), el("span", "hub-cmd", c.text));
      const copy = el("button", "hub-copy", "⧉");
      copy.title = "Copy: " + c.label;
      copy.setAttribute("aria-label", "Copy " + c.label + ": " + c.text);
      copy.addEventListener("click", () => {
        copyText(c.text).then((ok) => {
          copy.textContent = ok ? "✓" : "⧉";
          copy.classList.toggle("copied", ok);
          setTimeout(() => { copy.textContent = "⧉"; copy.classList.remove("copied"); }, 1200);
        });
      });
      line.append(copy);
      row.append(line);
      return row;
    }

    refresh();
  }

  if (os) setOs(os);
}

function renderVersionStrip(d) {
  const strip = $("#start-versions");
  if (!strip) return;
  const chips = el("div", "hub-chips");
  for (const [label, val] of [
    ["Home Assistant", d.upstream.ha_version],
    ["HA OS image", d.upstream.haos_version],
    ["integration", "v" + d.integration.version],
    ["firmware train", d.fw_train],
  ]) {
    const c = el("span", "chip");
    c.append(el("span", "hub-chip-k", label + " "), el("strong", null, val));
    chips.append(c);
  }
  strip.append(chips);
  const age = daysOld(d.upstream.fetched_at);
  if (age != null && age > 120) {
    const line = el("p", "fineprint stale",
      "version snapshot is " + age + " days old — check the repo's Actions tab");
    strip.append(line);
  }
}

// Run only inside a page — tests import this module's DOM-free core in node.
if (typeof document !== "undefined") main();
