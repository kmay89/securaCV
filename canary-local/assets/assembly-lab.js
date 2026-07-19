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
import { validateDevice, itemize, fmtDims, fmtLen, UNIT_MODES } from "./assembly-rules.js";

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

  // the drafting gate: the same ordering rules CI enforces, shown not hidden —
  // internals before shells, seals before lids, fasteners last & outermost
  const check = validateDevice(dev);
  const gate = el("p", "asm-gate" + (check.ok ? " ok" : " bad"));
  gate.append(
    el("strong", null, check.ok ? "✓ drafting check" : "⚠ drafting check"),
    document.createTextNode(check.ok
      ? " — internals precede shells, seals precede lids, fasteners drive last and sit outermost."
      : ` — ${check.violations.length} ordering rule violation(s); details in the console.`));
  if (!check.ok) console.warn(`assembly rules — ${deviceId}:`, check.violations);
  wrap.append(gate);

  // 3D stage (+ the balloon layer: item numbers over the exploded drawing)
  const stage = el("div", "asmlab-stage");
  const cv = el("canvas", "asmlab-3d");
  const hint = el("div", "asmlab-hint", "drag to orbit · scroll to zoom");
  const balloonLayer = el("div", "asm-balloons");
  stage.append(cv, balloonLayer, hint);
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

    // raw-frame bbox of a resolved part (STL/board: the parse; proc: its
    // builders' own vertices through their local transforms) — feeds the
    // caliper readout and the assembled-envelope line
    const bboxOfMeshes = (meshes) => {
      const lo = [1e9, 1e9, 1e9], hi = [-1e9, -1e9, -1e9];
      for (const mm of meshes) {
        const P = mm.builder.pos, L = mm.local;
        for (let i = 0; i < P.length; i += 3) {
          let x = P[i], y = P[i + 1], z = P[i + 2];
          if (L) {
            const nx = L[0] * x + L[4] * y + L[8] * z + L[12];
            const ny = L[1] * x + L[5] * y + L[9] * z + L[13];
            const nz = L[2] * x + L[6] * y + L[10] * z + L[14];
            x = nx; y = ny; z = nz;
          }
          lo[0] = Math.min(lo[0], x); lo[1] = Math.min(lo[1], y); lo[2] = Math.min(lo[2], z);
          hi[0] = Math.max(hi[0], x); hi[1] = Math.max(hi[1], y); hi[2] = Math.max(hi[2], z);
        }
      }
      return { lo, hi, size: [hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]] };
    };

    // resolve each part spec → engine parts (expanding fastener instances)
    for (const p of dev.parts) {
      let meshes, center = [0, 0, 0];
      if (p.source === "stl") {
        // base "preview": render-verified meshes from enclosures/preview/
        // (the displays' v0.1-dev parts); default: the print-validated catalog
        const base = p.base === "preview" ? "enclosures/preview/" : ENC_BASE;
        const { mesh, bbox } = parseSTL(await fetchBuf(base + p.file));
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
      const rawBbox = bboxOfMeshes(meshes);

      if (p.instances) { // fasteners: one engine part per placement, shared metadata
        // instances are [x, y] on a shared iz plane, or full [x, y, z]; irot
        // tilts every instance the same way (screws driving down a 25° axis)
        p.instances.forEach(([x, y, z], i) => asm.add({
          id: p.id + i, meshes: meshes.map((m) => ({ ...m })),
          seated: { pos: [x, y, z ?? p.iz ?? 0], rot: p.irot },
          explode: p.explode, insert: p.insert, step: p.step, name: p.name, qty: p.qty, ref: p.ref, center,
          rawBbox,
        }));
      } else {
        asm.add({ id: p.id, meshes, seated: p.seated, explode: p.explode, insert: p.insert,
          step: p.step, name: p.name, qty: p.qty, ref: p.ref, center, rawBbox });
      }
    }

    // auto-frame from the exploded extent — measured in the CAMERA's axes
    // (rotate the bounds through the home pose first; framing device-frame
    // bounds let a long explode chain run off-screen down the diagonal)
    let lo = [1e9, 1e9, 1e9], hi = [-1e9, -1e9, -1e9];
    for (const part of asm.parts) {
      const w = M.mul(M.t(part.explode[0], part.explode[1], part.explode[2]), part.seated);
      const c = [w[12] + part.center[0], w[13] + part.center[1], w[14] + part.center[2]];
      for (let i = 0; i < 3; i++) { lo[i] = Math.min(lo[i], c[i] - 16); hi[i] = Math.max(hi[i], c[i] + 16); }
    }
    const f = dev.frame || {};
    const R = M.mul(M.rx(f.rx ?? -0.46), M.ry(f.ry ?? 0.66));
    const rlo = [1e9, 1e9, 1e9], rhi = [-1e9, -1e9, -1e9];
    for (const cx of [lo[0], hi[0]]) for (const cy of [lo[1], hi[1]]) for (const cz of [lo[2], hi[2]]) {
      const x = R[0] * cx + R[4] * cy + R[8] * cz;
      const y = R[1] * cx + R[5] * cy + R[9] * cz;
      const z = R[2] * cx + R[6] * cy + R[10] * cz;
      rlo[0] = Math.min(rlo[0], x); rhi[0] = Math.max(rhi[0], x);
      rlo[1] = Math.min(rlo[1], y); rhi[1] = Math.max(rhi[1], y);
      rlo[2] = Math.min(rlo[2], z); rhi[2] = Math.max(rhi[2], z);
    }
    const span = Math.max(rhi[0] - rlo[0], rhi[1] - rlo[1]);
    scene.dist = span * (f.pad || 2.4) * 0.85;
    scene.rot = { x: f.rx ?? -0.46, y: f.ry ?? 0.66 };
    scene.home = { ...scene.rot };
    scene.viewY = f.vy ?? ((rhi[1] + rlo[1]) / 2);

    asm.mount();
    buildUI();
  })().catch((e) => { controls.innerHTML = ""; controls.append(el("p", "muted", "assembly failed to load: " + e.message)); });

  // ── the player UI ──
  function buildUI() {
    controls.innerHTML = "";
    const steps = dev.steps || [];

    // ── item numbers: balloons on the drawing ↔ rows in the parts list ──
    // (one number per distinct part, in build order — instanced fasteners
    // collapse to a single item carrying their qty, drafting style)
    const items = itemize(dev);
    const anchorOf = new Map(); // item n → engine part (first instance)
    for (const it of items) {
      const ep = asm.parts.find((q) => q.name === it.name);
      if (ep) anchorOf.set(it.n, ep);
    }
    const bomWrap = el("div", "asm-bom");
    const flashRow = (n) => {
      const row = bomWrap.querySelector(`[data-item="${n}"]`);
      if (!row) return;
      row.scrollIntoView({ block: "nearest", behavior: "smooth" });
      row.classList.add("flash");
      setTimeout(() => row.classList.remove("flash"), 1200);
    };
    const balloons = new Map();
    for (const it of items) {
      const b = el("button", "asm-balloon", String(it.n));
      b.title = `${it.n} — ${it.name}${it.qty > 1 ? ` ×${it.qty}` : ""}`;
      b.addEventListener("click", () => flashRow(it.n));
      balloonLayer.append(b);
      balloons.set(it.n, b);
    }
    asm.onFrame = () => {
      const on = asm.mode === "explode" && asm.explodeT > 0.12 && !balloonLayer.hidden;
      for (const [n, b] of balloons) {
        const ep = anchorOf.get(n);
        if (!on || !ep) { b.style.display = "none"; continue; }
        const [x, y, z] = asm.partAnchor(ep);
        const pr = scene.project(x, y, z);
        if (!pr.visible || pr.x < 10 || pr.y < 14 ||
            pr.x > cv.clientWidth - 24 || pr.y > cv.clientHeight - 14) {
          b.style.display = "none"; continue;
        }
        b.style.display = "flex";
        b.style.left = `${pr.x}px`;
        b.style.top = `${pr.y}px`;
      }
    };

    // ── the caliper: every dimension in mm · decimal inch · fractional inch ──
    const envelopeDims = () => {
      const lo = [1e9, 1e9, 1e9], hi = [-1e9, -1e9, -1e9];
      for (const part of asm.parts) {
        const b = part.rawBbox, s = part.seated;
        if (!b) continue;
        for (const cx of [b.lo[0], b.hi[0]])
          for (const cy of [b.lo[1], b.hi[1]])
            for (const cz of [b.lo[2], b.hi[2]]) {
              const x = s[0] * cx + s[4] * cy + s[8] * cz + s[12];
              const y = s[1] * cx + s[5] * cy + s[9] * cz + s[13];
              const z = s[2] * cx + s[6] * cy + s[10] * cz + s[14];
              lo[0] = Math.min(lo[0], x); hi[0] = Math.max(hi[0], x);
              lo[1] = Math.min(lo[1], y); hi[1] = Math.max(hi[1], y);
              lo[2] = Math.min(lo[2], z); hi[2] = Math.max(hi[2], z);
            }
      }
      return [hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]];
    };
    let unitMode = localStorage.getItem("scv-units") || "all";
    const bomHead = el("div", "asm-bomhead");
    const bomTitle = el("strong", null, "Parts list");
    const unitsBtn = el("button", "ghost small asm-units");
    const envLine = el("p", "asm-envelope");
    bomHead.append(bomTitle, unitsBtn);
    const renderBom = () => {
      unitsBtn.textContent =
        { all: "units: mm · in · frac", mm: "units: mm", in: "units: in · frac" }[unitMode];
      envLine.textContent = `assembled envelope (on its stand):  ${fmtDims(envelopeDims(), unitMode)}`;
      const t = el("table", "asm-bomtab");
      const hr = el("tr");
      for (const h of ["item", "part", "ref", "qty", "size (bounding)"]) hr.append(el("th", null, h));
      t.append(hr);
      for (const it of items) {
        const ep = anchorOf.get(it.n);
        const tr = el("tr");
        tr.dataset.item = it.n;
        tr.append(
          el("td", "asm-bom-n", String(it.n)),
          (() => {
            const td = el("td");
            td.append(document.createTextNode(it.name + " "), el("span", "asm-cls", it.cls));
            return td;
          })(),
          el("td", null, it.ref || "—"),
          el("td", null, String(it.qty)),
          el("td", "asm-bom-size", ep?.rawBbox ? fmtDims(ep.rawBbox.size, unitMode) : "—"));
        tr.addEventListener("click", () => flashRow(it.n));
        t.append(tr);
      }
      bomWrap.innerHTML = "";
      bomWrap.append(t);
    };
    unitsBtn.addEventListener("click", () => {
      unitMode = UNIT_MODES[(UNIT_MODES.indexOf(unitMode) + 1) % UNIT_MODES.length];
      localStorage.setItem("scv-units", unitMode);
      renderBom();
    });
    renderBom();

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

    // info card + the ballooned parts list under it
    const card = el("div", "asm-card");
    info.append(card, bomHead, envLine, bomWrap);

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
      if (!document.body.contains(cv)) { stop(); return; } // tab closed mid-play
      if (cur === steps.length - 1) { stop(); return; }
      go(cur + 1);
      if (timer !== null) clearTimeout(timer);
      timer = setTimeout(advance, 1900);
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
      balloonLayer.hidden = step; // balloons belong to the exploded drawing
      if (step) go(cur); else { stop(); asm.setExplode(Number(slider.value) / 100); }
    };
    bExpl.addEventListener("click", () => setMode(false));
    bStep.addEventListener("click", () => setMode(true));

    // keyboard nav when the stage has focus
    cv.tabIndex = 0;
    cv.addEventListener("keydown", (e) => {
      if (stepWrap.hidden) return;
      if (e.key === "ArrowRight") { e.preventDefault(); stop(); go(cur + 1); }
      else if (e.key === "ArrowLeft") { e.preventDefault(); stop(); go(cur - 1); }
    });

    asm.setExplode(1); // land on the full exploded view — the "wow"
  }

  return wrap;
}
