// canary-local/assets/assembly-lab.js — the Assemble tab.
//
// Two ways to understand the build, both from the real parts: an exploded view
// you can scrub open, and a step-by-step player where each part flies into place
// with the catalog's own assembly sentence as the caption. The parts are the
// printed enclosure STLs + the vendor board GLB + procedurally-built
// fasteners/battery (assembly.js); the step text is the drift-gated README
// §Assembly carried in build.json. Choreography lives in devices/assembly.json.

import { DeviceScene } from "./scene3d.js";
import { parseSTL } from "./stl.js";
import { parseGLB } from "./glb.js";
import { Assembly, PARTS, M } from "./assembly.js";

const el = (tag, cls, text) => {
  const n = document.createElement(tag);
  if (cls) n.className = cls;
  if (text != null) n.textContent = text;
  return n;
};

const ENC_BASE = "../docs/hardware/enclosure/";
const BOARD_BASE = "boards/";

async function fetchBuf(url) { return (await fetch(url)).arrayBuffer(); }

export function buildAssemblyLab(asmData, buildData, deviceId) {
  const wrap = el("div", "asmlab");
  const dev = asmData?.devices?.[deviceId];
  if (!dev) {
    wrap.append(el("p", "muted",
      "No guided assembly for this device yet — it lands when its parts and steps are choreographed."));
    return wrap;
  }
  const readmeSteps = buildData?.devices?.[deviceId]?.assembly?.steps || [];

  // honesty ribbon
  const ribbon = el("p", "ondevice asm-prov");
  ribbon.append(el("strong", null, "How to read this: "), document.createTextNode(
    "every part is the real thing — the printed STLs, the vendor board, and fasteners built to the BOM's sizes. " +
    "The step captions are the enclosure catalog's own assembly steps. The choreography (how they move) is staged."));
  wrap.append(ribbon);

  // 3D stage
  const stage = el("div", "asmlab-stage");
  const cv = el("canvas", "asmlab-3d");
  const hint = el("div", "asmlab-hint", "drag to orbit · scroll to zoom");
  stage.append(cv, hint);
  wrap.append(stage);

  // controls + info mount points (filled after parts load)
  const controls = el("div", "asm-controls");
  const info = el("div", "asm-info");
  wrap.append(controls, info);
  controls.append(el("p", "muted", "loading parts…"));

  const scene = new DeviceScene(cv, null);
  const asm = new Assembly(scene);
  cv.__scene = scene; cv.__asm = asm; // test/debug handles
  const obs = new IntersectionObserver(() => {
    if (!document.body.contains(cv)) { asm.unmount(); scene.stop(); obs.disconnect(); }
  });
  obs.observe(cv);

  (async () => {
    const pal = asmData.palette || {};
    const colorOf = (c) => (Array.isArray(c) ? c : pal[c]) || [0.5, 0.5, 0.5];

    // resolve each part spec → engine parts (expanding fastener instances)
    for (const p of dev.parts) {
      let meshes, center = [0, 0, 0];
      if (p.source === "stl") {
        const { mesh, bbox } = parseSTL(await fetchBuf(ENC_BASE + p.file));
        meshes = [{ builder: mesh, color: colorOf(p.color), gloss: p.gloss ?? 0.23 }];
        center = bbox.center;
      } else if (p.source === "board") {
        const { parts, bbox } = parseGLB(await fetchBuf(BOARD_BASE + p.board + ".glb"));
        const local = M.t(-bbox.center[0], -bbox.center[1], -bbox.center[2]);
        meshes = parts.map((q) => ({ builder: { pos: q.pos, nrm: q.nrm, uv: q.uv, idx: q.idx }, color: q.color, gloss: 0.3, local }));
      } else { // proc
        meshes = PARTS[p.part](p.params || {});
      }
      if (!document.body.contains(cv)) return; // tab closed mid-load

      if (p.instances) { // fasteners: one engine part per placement, shared metadata
        p.instances.forEach(([x, y], i) => asm.add({
          id: p.id + i, meshes: meshes.map((m) => ({ ...m })), seated: { pos: [x, y, p.iz || 0] },
          explode: p.explode, insert: p.insert, step: p.step, name: p.name, qty: p.qty, ref: p.ref, center,
        }));
      } else {
        asm.add({ id: p.id, meshes, seated: p.seated, explode: p.explode, insert: p.insert,
          step: p.step, name: p.name, qty: p.qty, ref: p.ref, center });
      }
    }

    // auto-frame from the exploded extent
    let lo = [1e9, 1e9, 1e9], hi = [-1e9, -1e9, -1e9];
    for (const part of asm.parts) {
      const w = M.mul(M.t(part.explode[0], part.explode[1], part.explode[2]), part.seated);
      const c = [w[12] + part.center[0], w[13] + part.center[1], w[14] + part.center[2]];
      for (let i = 0; i < 3; i++) { lo[i] = Math.min(lo[i], c[i] - 16); hi[i] = Math.max(hi[i], c[i] + 16); }
    }
    const span = Math.max(hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]);
    const f = dev.frame || {};
    scene.dist = span * (f.pad || 2.4);
    scene.rot = { x: f.rx ?? -0.46, y: f.ry ?? 0.66 };
    scene.home = { ...scene.rot };
    scene.viewY = f.vy ?? ((hi[2] + lo[2]) / 2);

    asm.mount();
    buildUI();
  })().catch((e) => { controls.innerHTML = ""; controls.append(el("p", "muted", "assembly failed to load: " + e.message)); });

  // ── the player UI ──
  function buildUI() {
    controls.innerHTML = "";
    const steps = dev.steps || [];
    const modeRow = el("div", "subtabs asm-mode");
    const bExpl = el("button", "tab on", "Exploded");
    const bStep = el("button", "tab", "Step by step");
    modeRow.append(bExpl, bStep);
    controls.append(modeRow);

    // exploded controls
    const explWrap = el("div", "asm-explode");
    const slider = document.createElement("input");
    slider.type = "range"; slider.min = "0"; slider.max = "100"; slider.value = "100"; slider.className = "asm-slider";
    const sLabel = el("span", "muted", "exploded");
    explWrap.append(el("span", "muted", "together"), slider, sLabel);
    controls.append(explWrap);

    // step controls
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

    // info card
    const card = el("div", "asm-card");
    info.append(card);

    let cur = 0, playing = false, timer = null;

    const renderCard = (i) => {
      card.innerHTML = "";
      const s = steps[i];
      card.append(el("div", "asm-step-n", `Step ${i + 1} of ${steps.length}`));
      card.append(el("h4", "asm-title", s.title));
      const text = s.readmeStep != null ? (readmeSteps[s.readmeStep] || s.note || "") : (s.note || "");
      card.append(el("p", "body", text));
      // part badges for parts revealed at this step
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
      if (s.readmeStep != null) card.append(el("p", "muted fineprint", "— from the enclosure catalog's §Assembly"));
    };

    const frameStep = (i) => {
      // gentle per-step camera nudge so the eye follows the action up the stack
      const t = steps.length > 1 ? i / (steps.length - 1) : 0;
      asm.tweenCamera({ x: (dev.frame?.rx ?? -0.46) - t * 0.12, y: (dev.frame?.ry ?? 0.66) + t * 0.18, d: scene.dist });
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
      frameStep(cur);
    };

    const stop = () => { playing = false; play.textContent = "▶ play"; if (timer) clearTimeout(timer); timer = null; };
    const advance = () => {
      if (!playing) return;
      if (cur === steps.length - 1) { stop(); return; }
      go(cur + 1); timer = setTimeout(advance, 1900);
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

    // keyboard nav when the stage has focus
    cv.tabIndex = 0;
    cv.addEventListener("keydown", (e) => {
      if (stepWrap.hidden) return;
      if (e.key === "ArrowRight") { stop(); go(cur + 1); }
      else if (e.key === "ArrowLeft") { stop(); go(cur - 1); }
    });

    asm.setExplode(1); // land on the full exploded view — the "wow"
  }

  return wrap;
}
