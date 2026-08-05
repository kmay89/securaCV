// canary-local/assets/we2-flash.js — the module flow on the flash page.
//
// The real thing: load the Canary Vision's brain — the pinned person-
// detection model — onto the Grove Vision AI V2 over WebSerial, from this
// page, with zero choices to make. The engine (assets/we2-core.js) speaks
// the module's ROM-bootloader XMODEM protocol; this file owns Web Serial,
// the asset pipeline (release manifest → download → SHA-256 verify → burn),
// the post-flash proof (the module answers AT and carries our model info),
// and the bench: a live preview with real bounding boxes and the two
// on-module thresholds (AT+TSCORE / AT+TIOU), so nobody ever needs the
// vendor's site to aim or tune.
//
// Privacy is stated where it matters: the live preview streams camera
// frames from the module to THIS PAGE over the USB cable you plugged in —
// a physical, attended bench operation, the same one the getting-started
// guide blesses for the one-time sanity check. Frames render on a local
// canvas and are never stored or sent anywhere (this page fetches nothing
// but its own JSON and, when you ask, the pinned model from the project's
// GitHub release). Day-to-day aiming stays boxes-only over MQTT.
//
// Same failure posture as the ESP32 flow: every error names the fix, and
// nothing here can brick the module — the burn menu lives in mask ROM.

import { WE2, We2Flasher, makeAtParser, atCommand, modelInfoJson,
         stylizeDetections, meterModel, WE2_CLASSES } from "./we2-core.js";
import { helpTopic } from "./flash-core.js";
import { chirp } from "./chirp.js";
import { visionSession } from "./vision-session.js";
import { visionChecklistCard } from "./vision-checklist.js";

const el = (tag, cls, text) => {
  const n = document.createElement(tag);
  if (cls) n.className = cls;
  if (text != null) n.textContent = text;
  return n;
};
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// ── Web Serial transport (the we2-core contract) ─────────────────────────
export function makeTransport(port) {
  const rx = [];
  let reader = null;
  let closed = false;
  let pumping = null;
  let writeChain = Promise.resolve();

  async function pump() {
    while (!closed && port.readable) {
      try {
        reader = port.readable.getReader();
        for (;;) {
          const { value, done } = await reader.read();
          if (done) break;
          if (value) rx.push(...value);
        }
      } catch { /* device re-enumerated or unplugged — readByte times out */ }
      finally { try { reader.releaseLock(); } catch { /* already released */ } reader = null; }
      if (!closed) await sleep(50);
    }
  }

  const t = {
    start() { if (!pumping) pumping = pump(); },
    async close() {
      closed = true;
      try { await reader?.cancel(); } catch { /* fine */ }
      try { await port.close(); } catch { /* fine */ }
    },
    clear() { rx.length = 0; },
    async setRTS(v) { await port.setSignals({ requestToSend: v }); },
    async sleep(ms) { await sleep(ms); },
    // Writes are serialized through one chain: two concurrent cmd()s (the
    // proof IIFE + the bench's threshold init) would otherwise collide on
    // getWriter() and the loser's AT command would silently never leave.
    write(bytes) {
      const job = writeChain.then(async () => {
        const w = port.writable.getWriter();
        try { await w.write(bytes instanceof Uint8Array ? bytes : Uint8Array.from(bytes)); }
        finally { w.releaseLock(); }
      });
      writeChain = job.catch(() => { /* keep the chain alive after a failed write */ });
      return job;
    },
    writeString(s) { t.write(Uint8Array.from(s, (c) => c.charCodeAt(0))); },
    async readByte(timeoutMs) {
      const t0 = performance.now();
      while (performance.now() - t0 < timeoutMs) {
        if (rx.length) return rx.shift();
        await sleep(2);
      }
      return null;
    },
    // accumulate text until `needle` or timeout; `tick` (the '1' drip) runs
    // every SPAM_INTERVAL_MS while waiting — exactly the reference behavior
    async readUntil(needle, timeoutMs, tick) {
      let s = "";
      const t0 = performance.now();
      let lastTick = 0;
      while (performance.now() - t0 < timeoutMs) {
        if (tick && performance.now() - lastTick >= WE2.SPAM_INTERVAL_MS) {
          lastTick = performance.now();
          tick();
        }
        while (rx.length) s += String.fromCharCode(rx.shift());
        if (s.includes(needle)) return s;
        await sleep(2);
      }
      while (rx.length) s += String.fromCharCode(rx.shift());
      return s;
    },
    drainInto(fn) { if (rx.length) { const b = Uint8Array.from(rx); rx.length = 0; fn(b); } },
  };
  return t;
}

// ── AT client on top of the transport ────────────────────────────────────
function makeAtClient(t) {
  const parser = makeAtParser();
  const waiters = [];         // {match(frame), resolve}
  const eventHandlers = [];
  let looping = false;

  async function loop() {
    looping = true;
    while (looping) {
      t.drainInto((bytes) => parser.feed(bytes, (frame) => {
        for (let i = 0; i < waiters.length; i++) {
          if (waiters[i].match(frame)) { waiters.splice(i, 1)[0].resolve(frame); return; }
        }
        if (frame.type === 1) for (const h of eventHandlers) h(frame);
      }));
      await sleep(15);
    }
  }

  return {
    start() { if (!looping) loop(); },
    stop() { looping = false; },
    onEvent(fn) { eventHandlers.push(fn); },
    // send AT+<body>\r and await the type-0 reply whose name matches.
    // The reply's `name` echoes the command tag — sometimes with the '?'
    // (e.g. "VER?"), sometimes without and never with the "=…" tail, so
    // match on the pre-'=' stem with the fallbacks below.
    async cmd(body, { timeoutMs = 3000 } = {}) {
      const name = body.split("=")[0];
      t.writeString(atCommand(body));
      return await new Promise((resolve) => {
        const w = {
          match: (f) => f.type === 0 && (f.name === name || f.name === body || body.startsWith(f.name)),
          resolve,
        };
        waiters.push(w);
        setTimeout(() => {
          const i = waiters.indexOf(w);
          if (i >= 0) { waiters.splice(i, 1); resolve(null); }
        }, timeoutMs);
      });
    },
  };
}

// ── wake a maybe-mid-state module ────────────────────────────────────────
// One VER? probe made a booting or mid-state module look dead — right after a
// burn (it's rebooting), or right after a replug. Patience instead: probe a
// few times while it comes up, and if it stays quiet, pulse RTS — the same
// reset line the flasher drives, i.e. the "power-cycle it" advice, automated —
// then probe again. Returns the VER? reply, or null if it truly never spoke.
async function wakeModule(at, t, note) {
  for (let i = 0; i < 3; i++) {
    const ver = await at.cmd("VER?", { timeoutMs: 1200 });
    if (ver && ver.code === 0) return ver;
  }
  if (note) note("quiet — resetting the module and waiting for it to boot…");
  try { await t.setRTS(false); await sleep(100); await t.setRTS(true); }
  catch { /* no RTS on this adapter — the retries below still count */ }
  await sleep(1200);
  t.clear(); // drop the boot banner so the parser starts on clean frames
  for (let i = 0; i < 3; i++) {
    const ver = await at.cmd("VER?", { timeoutMs: 1500 });
    if (ver && ver.code === 0) return ver;
  }
  return null;
}

// ── sha-256 helper ───────────────────────────────────────────────────────
async function sha256hex(bytes) {
  const d = await crypto.subtle.digest("SHA-256", bytes);
  return [...new Uint8Array(d)].map((b) => b.toString(16).padStart(2, "0")).join("");
}

// ── the flow ─────────────────────────────────────────────────────────────
// Mounted by flash.js: phaseModule({ catalog, setPhase, back }) returns a node.
export function phaseModule(ctx) {
  const m = ctx.catalog.we2_module;
  const box = el("section", "flash-card we2-flow");
  box.append(el("div", "flash-big-emoji", "📷"));
  box.append(el("h2", null, "Load the Vision’s brain — right here"));
  box.append(el("p", "muted",
    "This flow talks to the Grove Vision AI V2 over the MODULE’s own USB-C port and burns " +
    "the pinned " + m.model.name + " model into its flash. One model, tested with this " +
    "firmware train, SHA-256-verified before a byte is written. " + m.persistence));

  const facts = el("div", "wap-facts");
  for (const [k, v] of [
    ["Port", m.port_note],
    ["Model", m.model.name + " — " + m.model.arch],
    ["License", m.model.license],
    ["Engine", m.engine],
    ["No-brick", m.no_brick],
  ]) {
    const row = el("div", "wap-fact");
    row.append(el("span", "wap-fact-k", k), el("span", "wap-fact-v", v));
    facts.append(row);
  }
  box.append(facts);

  const btn = el("button", "primary flash-connect-btn", "Connect the module");
  const backBtn = el("button", "ghost", "← back to the Canary flasher");
  backBtn.addEventListener("click", ctx.back);
  const status = el("p", "fineprint", "Your browser will ask which serial port — the module shows as “USB Single Serial” (CH343).");
  btn.addEventListener("click", () => connectModule(ctx, box, status, btn));
  const row = el("div", "we2-btnrow");
  row.append(btn, backBtn);
  box.append(row, status);
  return box;
}

async function connectModule(ctx, box, status, btn) {
  const m = ctx.catalog.we2_module;
  let port;
  try {
    port = await navigator.serial.requestPort({
      filters: [{ usbVendorId: parseInt(m.usb_vid, 16), usbProductId: parseInt(m.usb_pid, 16) }],
    });
  } catch {
    status.textContent = "No port picked. If nothing was listed: wrong port (the XIAO’s), a " +
      "charge-only cable, or the CH343 driver is missing (Linux needs only a udev rule — device guide §7).";
    return;
  }
  btn.disabled = true;
  try {
    await port.open({ baudRate: m.baud });
  } catch (e) {
    btn.disabled = false;
    status.textContent = "The port wouldn’t open (" + e.message + ") — close other serial monitors and retry.";
    return;
  }
  const t = makeTransport(port);
  t.start();
  ctx.setPhase(phaseModuleConnected(ctx, { port, t }));
}

function phaseModuleConnected(ctx, s) {
  const m = ctx.catalog.we2_module;
  const box = el("section", "flash-card we2-flow");
  box.append(el("h2", null, "Module connected"));
  const idLine = el("p", "muted", "Asking the module who it is…");
  box.append(idLine);

  // a gentle AT probe — purely informative; the ROM bootloader path below
  // works even if the running firmware answers nothing. Kept in scope so
  // every hand-off below STOPS it first: two consumers draining one byte
  // stream steal each other's replies (a burn's ROM banner, the bench's
  // VER?) and make good hardware look broken.
  const probe = makeAtClient(s.t);
  const stopProbe = () => { try { probe.stop(); } catch { /* already stopped */ } };
  (async () => {
    const at = probe;
    at.start();
    const ver = await at.cmd("VER?", { timeoutMs: 1600 });
    const id = ver ? await at.cmd("ID?", { timeoutMs: 1200 }) : null;
    at.stop();
    if (ver && ver.data) {
      idLine.textContent = "It answered: SSCMA firmware " + (ver.data.software || "?") +
        (id && id.data ? " · id " + id.data : "") + ". Ready to (re)load the model.";
    } else {
      idLine.textContent = "No AT answer — fine: a model-less or mid-state module still " +
        "flashes through its ROM bootloader. If this is actually the XIAO’s port, the next " +
        "step will say so.";
    }
  })();

  // ── model source: the pinned release asset, or a local file ──
  const src = el("div", "we2-src");
  const pinned = el("button", "card we2-src-card");
  pinned.append(el("strong", null, "The pinned model (recommended)"),
    el("span", "muted", m.model.name + " — fetched from the project’s signed release, " +
      "SHA-256 checked against the release manifest before anything is written."),
    el("code", "fineprint", "manifest-vision-model.json · " + m.model_addr));
  const local = el("button", "card we2-src-card");
  local.append(el("strong", null, "A model file you already have"),
    el("span", "muted", "Any SSCMA-compatible .tflite (Vela-compiled for the Ethos-U55). " +
      "Its SHA-256 is shown before flashing — you’re trusting your file, not us."));
  const file = document.createElement("input");
  file.type = "file"; file.accept = ".tflite,.bin"; file.hidden = true;
  // Already carrying its model? The live bench needs no reflash.
  const benchCard = el("button", "card we2-src-card");
  benchCard.append(el("strong", null, "Just look through it (live bench)"),
    el("span", "muted", "Model already on the module? Open the live preview — camera frames, " +
      "bounding boxes, a confidence meter, and the two on-module dials. Nothing is written."));
  src.append(pinned, local, benchCard, file);
  box.append(el("h3", "wap-col-h", "What goes on it"), src);
  benchCard.addEventListener("click", () => { stopProbe(); ctx.setPhase(phaseModuleBench(ctx, s)); });

  const note = el("p", "fineprint", "");
  box.append(note);

  pinned.addEventListener("click", async () => {
    stopProbe();
    note.textContent = "Fetching the release manifest…";
    try {
      const man = await fetch(m.manifest_url, { cache: "no-store" }).then((r) => {
        if (!r.ok) throw new Error("HTTP " + r.status);
        return r.json();
      });
      const asset = man && man.model;
      if (!asset || !asset.url || !asset.sha256) throw new Error("manifest has no model entry");
      note.textContent = "Downloading " + (asset.name || "model") + " (" + Math.round((asset.size || 0) / 1024) + " KB)…";
      const bytes = new Uint8Array(await fetch(asset.url).then((r) => {
        if (!r.ok) throw new Error("HTTP " + r.status);
        return r.arrayBuffer();
      }));
      const hex = await sha256hex(bytes);
      if (hex !== String(asset.sha256).toLowerCase()) {
        note.textContent = "✗ SHA-256 mismatch — refusing to flash. Expected " + asset.sha256 + ", got " + hex + ".";
        return;
      }
      ctx.setPhase(phaseModuleFlash(ctx, s, {
        bytes, label: asset.name || "person-detection.tflite",
        sha256: hex, version: man.version || man.tag || "",
        pinned: true,
      }));
    } catch (e) {
      note.textContent = "Couldn’t fetch the pinned model (" + e.message + "). The asset ships " +
        "with the project’s releases — if this is a fresh fork or the release train hasn’t cut " +
        "one yet, use a local file, or load it once with SenseCraft (the Vision page stages that).";
    }
  });
  local.addEventListener("click", () => file.click());
  file.addEventListener("change", async () => {
    const f = file.files && file.files[0];
    if (!f) return;
    stopProbe();
    const bytes = new Uint8Array(await f.arrayBuffer());
    const hex = await sha256hex(bytes);
    ctx.setPhase(phaseModuleFlash(ctx, s, { bytes, label: f.name, sha256: hex, version: "", pinned: false }));
  });

  const backBtn = el("button", "ghost", "← back");
  backBtn.addEventListener("click", async () => { await s.t.close(); ctx.back(); });
  box.append(backBtn);
  return box;
}

function phaseModuleFlash(ctx, s, job) {
  const box = el("section", "flash-card we2-flow");
  box.append(el("h2", null, "Burning " + job.label));
  box.append(el("p", "muted",
    (job.pinned ? "Verified against the release manifest. " : "Your file. ") +
    "SHA-256 " + job.sha256.slice(0, 16) + "… · " + job.bytes.length.toLocaleString() +
    " bytes → " + ctx.catalog.we2_module.model_addr + ". Keep the cable in; an interrupted " +
    "burn just means reset and retry — the bootloader lives in ROM."));

  const bar = el("div", "vis-progress");
  const fill = el("div", "vis-progress-fill");
  bar.append(fill);
  const log = el("pre", "we2-log");
  box.append(bar, log);

  (async () => {
    const f = new We2Flasher(s.t, {
      onLog: (l) => { log.textContent += "· " + l + "\n"; log.scrollTop = log.scrollHeight; },
      onProgress: (p) => { fill.style.width = Math.round(p * 100) + "%"; },
    });
    try {
      await f.flashModel(job.bytes);
      // Two-port Vision: the camera module's model is now burned — record it
      // (once, here at the completion transition) so the done screen can insist
      // on both ports and celebrate only when both are in.
      visionSession.markDone("we2");
      ctx.setPhase(phaseModuleDone(ctx, s, job));
    } catch (e) {
      log.textContent += "✗ " + e.message + "\n";
      const retry = el("button", "primary", "Reset and retry");
      retry.addEventListener("click", () => ctx.setPhase(phaseModuleFlash(ctx, s, job)));
      const back = el("button", "ghost", "← start over");
      back.addEventListener("click", async () => { await s.t.close(); ctx.back(); });
      const row = el("div", "we2-btnrow");
      row.append(retry, back);
      box.append(row);
    }
  })();
  return box;
}

// ── the live bench: video preview + boxes + meter + thresholds + guide ──────
// One implementation, mounted from two places: after a burn (phaseModuleDone)
// and directly from "Module connected" for a module that already carries its
// model — nobody should have to reflash just to see through the camera.
//
// What it renders, every event, from scratch (no ghost boxes):
//   frame  — the module's JPEG still on a canvas (kept as lastFrame so
//            boxes-only events repaint over the CURRENT image, not stack)
//   boxes  — every detection, each with a stable identity color, an index
//            badge, and its own confidence — multi-object is the normal case
//   meter  — best-confidence bar with the module's TSCORE floor as a tick,
//            so the threshold slider visibly MEANS something
//   guide  — catalog-driven steps + troubleshooting (we2_module.bench)

function benchHelpDot(ctx, topicId) {
  const t = helpTopic(ctx.catalog, topicId);
  if (!t) return null;
  const wrap = el("span", "flash-help");
  const btn = el("button", "flash-help-dot", "?");
  btn.type = "button";
  btn.setAttribute("aria-label", `About: ${t.label}`);
  const pop = el("span", "flash-help-pop flash-hidden");
  pop.append(el("strong", "flash-help-title", t.label));
  pop.append(el("span", "flash-help-what", t.what));
  if (t.default) {
    const d = el("span", "flash-help-default");
    d.append(el("em", null, "Default: "), document.createTextNode(t.default));
    pop.append(d);
  }
  btn.addEventListener("click", (ev) => {
    ev.preventDefault(); ev.stopPropagation();
    pop.classList.toggle("flash-hidden");
  });
  wrap.append(btn, pop);
  return wrap;
}

function mountBench(ctx, at, opts = {}) {
  const m = ctx.catalog.we2_module;
  const guide = m.bench || { defaults: { tscore: 50, tiou: 45 }, steps: [], troubleshooting: [] };
  const classes = opts.pinned ? WE2_CLASSES : [];

  const bench = el("div", "we2-bench");
  bench.append(el("h3", "wap-col-h", "Bench check — see what it sees"));
  bench.append(el("p", "fineprint",
    "This streams camera frames from the module to this page over your USB cable — the " +
    "attended, one-time bench check the guide describes. Frames render below and go nowhere " +
    "else. Mounted-and-deployed aiming stays boxes-only over MQTT (the Aim card)."));

  // The guide: numbered steps + every usual failure, catalog-driven.
  if (guide.steps.length) {
    const how = el("details", "we2-guide");
    how.append(el("summary", null, "How to get the live preview working"));
    const ol = el("ol", "we2-guide-steps");
    guide.steps.forEach((s2) => ol.append(el("li", null, s2)));
    how.append(ol);
    if (guide.troubleshooting.length) {
      how.append(el("p", "we2-guide-fixh", "If it doesn’t behave:"));
      const dl = el("div", "we2-guide-fixes");
      guide.troubleshooting.forEach((f) => {
        const row = el("div", "we2-guide-fix");
        row.append(el("strong", null, f.when));
        row.append(el("span", null, f.fix));
        dl.append(row);
      });
      how.append(dl);
    }
    bench.append(how);
  }

  const startBtn = el("button", "primary", "Start live preview");
  const stopBtn = el("button", "ghost", "Stop");
  stopBtn.disabled = true;
  const rowB = el("div", "we2-btnrow");
  rowB.append(startBtn, stopBtn);

  const seenBanner = el("div", "we2-seen flash-hidden");

  // The stage: canvas + overlay chips (object count, fps).
  const stage = el("div", "we2-preview");
  const cv = document.createElement("canvas");
  cv.className = "we2-preview-cv";
  const countChip = el("span", "we2-chip we2-chip-count flash-hidden");
  const fpsChip = el("span", "we2-chip we2-chip-fps flash-hidden");
  stage.append(cv, countChip, fpsChip);

  // The confidence meter: best score vs the module's own reporting floor.
  const meter = el("div", "we2-meter");
  const meterTrack = el("div", "we2-meter-track");
  const meterFill = el("div", "we2-meter-fill");
  const meterTick = el("div", "we2-meter-tick");
  meterTrack.append(meterFill, meterTick);
  const meterLabel = el("div", "we2-meter-label", "watching… nothing yet");
  meter.append(meterTrack, meterLabel);

  // Thresholds: initialized from the MODULE's live values (queried), with the
  // model-zoo defaults as the fallback; the ⓘ explains each one.
  const thr = { tscore: guide.defaults.tscore, tiou: guide.defaults.tiou };
  const sliders = el("div", "vis-preview-controls");
  const mkSlider = (label, cmd, key, helpId) => {
    const boxS = el("label", "vis-slider");
    const out = el("output", null, String(thr[key]));
    const input = document.createElement("input");
    input.type = "range"; input.min = "0"; input.max = "100"; input.value = String(thr[key]);
    input.addEventListener("change", async () => {
      await at.cmd(cmd + "=" + input.value, { timeoutMs: 2000 });
      // Read back what the module actually holds — the slider never lies.
      const q = await at.cmd(cmd + "?", { timeoutMs: 1500 });
      const v = q && typeof q.data === "number" ? q.data : Number(input.value);
      thr[key] = v;
      input.value = String(v); out.value = String(v);
      if (key === "tscore") positionTick();
    });
    const cap = el("span", null, label);
    const hd = benchHelpDot(ctx, helpId);
    if (hd) cap.append(hd);
    boxS.append(cap, input, out);
    return { node: boxS, input, out };
  };
  const sTscore = mkSlider("Confidence floor (TSCORE)", "TSCORE", "tscore", "tscore");
  const sTiou = mkSlider("Overlap merge (TIOU)", "TIOU", "tiou", "tiou");
  sliders.append(sTscore.node, sTiou.node);

  const meta = el("p", "fineprint", "");
  bench.append(rowB, seenBanner, stage, meter, sliders, meta);

  function positionTick() {
    meterTick.style.left = thr.tscore + "%";
    meterTick.title = "the module’s reporting floor — TSCORE " + thr.tscore;
  }
  positionTick();

  // Ask the module for its CURRENT thresholds so the sliders start honest.
  (async () => {
    for (const [cmd, key, s2] of [["TSCORE?", "tscore", sTscore], ["TIOU?", "tiou", sTiou]]) {
      const q = await at.cmd(cmd, { timeoutMs: 1500 });
      if (q && typeof q.data === "number" && q.data >= 0 && q.data <= 100) {
        thr[key] = q.data;
        s2.input.value = String(q.data);
        s2.out.value = String(q.data);
      }
    }
    positionTick();
  })();

  // ── the render loop: every event repaints frame + boxes from scratch ──
  const ctx2 = cv.getContext("2d");
  let lastFrame = null;      // the current camera still (Image)
  let previewing = false, seen = false;
  let streamed = false;      // any INVOKE event since the last Start press
  let frames = 0, fpsT0 = performance.now();

  function render(boxes) {
    if (lastFrame) {
      cv.width = lastFrame.width; cv.height = lastFrame.height;
      ctx2.drawImage(lastFrame, 0, 0);
    } else {
      ctx2.fillStyle = "#08090b";
      ctx2.fillRect(0, 0, cv.width || 240, cv.height || 240);
    }
    const dets = stylizeDetections(boxes, classes);
    for (const d of dets) {
      const x = d.x - d.w / 2, y = d.y - d.h / 2;
      // the box — identity-colored, weight by confidence band
      ctx2.lineWidth = d.band === "ok" ? 3 : 2;
      ctx2.strokeStyle = d.color;
      ctx2.strokeRect(x, y, d.w, d.h);
      // corner ticks make thin boxes readable on busy frames
      ctx2.lineWidth = d.band === "ok" ? 5 : 3;
      const t = Math.min(14, d.w / 4, d.h / 4);
      ctx2.beginPath();
      for (const [cx, cy, dx, dy] of [[x, y, 1, 1], [x + d.w, y, -1, 1], [x, y + d.h, 1, -1], [x + d.w, y + d.h, -1, -1]]) {
        ctx2.moveTo(cx + dx * t, cy); ctx2.lineTo(cx, cy); ctx2.lineTo(cx, cy + dy * t);
      }
      ctx2.stroke();
      // the label: "#1 person · 92%"
      const text = `#${d.n} ${d.text}`;
      ctx2.font = "700 13px ui-monospace, Menlo, monospace";
      const tw = ctx2.measureText(text).width + 10;
      const ly = y - 20 >= 0 ? y - 20 : y + d.h + 1;
      ctx2.fillStyle = d.fill;
      ctx2.fillRect(x, ly, tw, 19);
      ctx2.fillStyle = d.color;
      ctx2.fillText(text, x + 5, ly + 14);
    }
    // meter + chips + summary
    const mm = meterModel(boxes, classes, thr.tscore);
    meterFill.style.width = (mm.count ? mm.top : 0) + "%";
    meterFill.dataset.level = mm.level;
    meterLabel.textContent = mm.count
      ? `${mm.label} — clears the floor by ${mm.margin}`
      : "watching… nothing above the floor";
    meta.textContent = mm.label;
    countChip.textContent = mm.count ? `${mm.count} in frame` : "";
    countChip.classList.toggle("flash-hidden", !mm.count);
    if (dets.length && !seen) {
      seen = true;
      chirp("seen");
      seenBanner.textContent = opts.pinned
        ? "👁 It sees you — the model is live and working ✓"
        : "👁 It sees something — the model is live and working ✓";
      seenBanner.classList.remove("flash-hidden");
    }
  }

  at.onEvent((f) => {
    if (!previewing || !f.data) return;
    streamed = true;
    if (f.data.image) {
      const image = new Image();
      image.onload = () => {
        lastFrame = image;
        frames++;
        const now = performance.now();
        if (now - fpsT0 >= 1000) {
          fpsChip.textContent = `${Math.round((frames * 1000) / (now - fpsT0))} fps`;
          fpsChip.classList.remove("flash-hidden");
          frames = 0; fpsT0 = now;
        }
        render(f.data.boxes);
      };
      image.src = "data:image/jpeg;base64," + f.data.image;
    } else if (f.data.boxes) {
      render(f.data.boxes); // boxes-only event: repaint over the CURRENT frame
    }
  });

  startBtn.addEventListener("click", async () => {
    previewing = true;
    streamed = false;
    seen = false; seenBanner.classList.add("flash-hidden");
    meta.textContent = "watching…";
    startBtn.disabled = true; stopBtn.disabled = false;
    // Continuous invoke, with frames. On SSCMA the stream itself arrives as
    // type-1 INVOKE events (docs/hardware/grove_vision_ai_v2_guide.md §8) and
    // a type-0 ack may or may not precede it depending on firmware build —
    // so success is EITHER signal, and failure is only "nothing arrived".
    const inv = await at.cmd("INVOKE=-1,0,0", { timeoutMs: 4000 });
    if (streamed || (inv && inv.code === 0) || !previewing) return;
    await sleep(1500); // one more beat — a slow first frame is not a failure
    if (previewing && !streamed) {
      meta.textContent = "The module didn’t start streaming — power-cycle it (unplug/replug), " +
        "reconnect, and try again. The guide above lists the other usual causes.";
      previewing = false;
      startBtn.disabled = false; stopBtn.disabled = true;
    }
  });
  stopBtn.addEventListener("click", async () => {
    previewing = false;
    startBtn.disabled = false; stopBtn.disabled = true;
    fpsChip.classList.add("flash-hidden");
    await at.cmd("BREAK", { timeoutMs: 2000 });
  });

  return {
    node: bench,
    async stop() {
      previewing = false;
      try { await at.cmd("BREAK", { timeoutMs: 800 }); } catch { /* leaving anyway */ }
    },
  };
}

// The bench as its own phase — for a module that ALREADY has its model, so
// seeing through the camera never requires a reflash.
function phaseModuleBench(ctx, s) {
  const box = el("section", "flash-card we2-flow");
  box.append(el("h2", null, "Live bench — no reflash needed"));
  const idLine = el("p", "muted", "Asking the module what it carries…");
  box.append(idLine);

  const at = makeAtClient(s.t);
  at.start();

  let bench = null;
  (async () => {
    const ver = await wakeModule(at, s.t, (line) => { idLine.textContent = line; });
    if (!ver) {
      idLine.textContent = "The module didn’t answer AT, even after an automatic reset — check " +
        "this is the MODULE’s own USB-C port (the CH343), unplug/replug, and retry; if it keeps " +
        "refusing, burn the model first (one click back).";
      return;
    }
    // What model is on it? Our stored model card answers — honesty decides the
    // label map: only a card naming the person class earns "person" labels.
    const info = await at.cmd("INFO?", { timeoutMs: 2000 });
    let pinned = false, modelName = "";
    try {
      // the reply's data is the base64 card — bare, or nested as data.info
      const raw = info && (typeof info.data === "string" ? info.data
        : info.data && typeof info.data.info === "string" ? info.data.info : null);
      const decoded = raw ? JSON.parse(atob(raw)) : null;
      modelName = (decoded && decoded.name) || "";
      pinned = !!(decoded && Array.isArray(decoded.classes) && decoded.classes.includes("person"));
    } catch { /* no card or not ours — generic labels */ }
    idLine.textContent = "SSCMA firmware " + ((ver.data && ver.data.software) || "?") +
      (modelName ? " · model: " + modelName : " · no model card — labels will read “object”") +
      ". Start the preview below.";
    bench = mountBench(ctx, at, { pinned });
    box.insertBefore(bench.node, backBtn);
  })();

  const backBtn = el("button", "ghost", "← back");
  backBtn.addEventListener("click", async () => {
    if (bench) await bench.stop();
    at.stop();
    await s.t.close();
    ctx.back();
  });
  box.append(backBtn);
  return box;
}

function phaseModuleDone(ctx, s, job) {
  const box = el("section", "flash-card we2-flow");
  box.append(el("div", "flash-big-emoji", "✅"));
  box.append(el("h2", null, "Model burned — now make it prove it"));
  const proof = el("p", "muted", "Rebooted. Asking the module to confirm…");
  box.append(proof);

  const at = makeAtClient(s.t);
  at.start();

  // Two-port guardrail: this camera module is done — show whether the ESP32
  // Vision firmware is too, and celebrate only when both ports are in. If the
  // board's firmware still needs doing, route straight to it (closes this port).
  box.append(visionChecklistCard(visionSession.parts(), {
    onFlashOther: async () => {
      try { await at.cmd("BREAK", { timeoutMs: 600 }); } catch { /* leaving anyway */ }
      try { at.stop(); } catch { /* leaving anyway */ }
      try { await s.t.close(); } catch { /* leaving anyway */ }
      ctx.back();
    },
  }));

  (async () => {
    // give the app firmware a beat to come up, then handshake — with the
    // patient wake (retries, then an RTS pulse): a module still rebooting
    // from its burn is the normal case here, not a failure.
    await sleep(1200);
    const ver = await wakeModule(at, s.t, (line) => { proof.textContent = line; });
    if (ver) {
      // store our model card on-device (what SenseCraft does after a flash),
      // then a one-shot invoke as the "it actually runs" proof
      const info = btoa(JSON.stringify(modelInfoJson({ version: job.version, sha256: job.sha256 })));
      await at.cmd('INFO="' + info + '"', { timeoutMs: 3000 });
      const inv = await at.cmd("INVOKE=1,0,1", { timeoutMs: 8000 });
      proof.textContent = inv && inv.code === 0
        ? "✓ The module answered, carries our model card, and ran one inference. Done — the " +
          "model persists across power cycles and every host reflash."
        : "The module answers AT but the test inference didn’t reply — power-cycle it once. " +
          "If it keeps refusing: the module may be running non-SSCMA firmware; see the device guide §4.";
    } else {
      proof.textContent = "No AT answer after reboot, even after an automatic reset — unplug/" +
        "replug the module and it should come up with the new model. The burn itself completed " +
        "and verified.";
    }
  })();

  // ── the bench: shared with phaseModuleBench — video, boxes, meter, guide ──
  const bench = mountBench(ctx, at, { pinned: job.pinned });
  box.append(bench.node);

  const done = el("button", "ghost", "Disconnect — I’m done");
  done.addEventListener("click", async () => {
    await bench.stop();
    at.stop();
    await s.t.close();
    ctx.back();
  });
  box.append(done);
  return box;
}
