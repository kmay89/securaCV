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
         formatDetections, detectionSummary, WE2_CLASSES } from "./we2-core.js";
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
    async write(bytes) {
      const w = port.writable.getWriter();
      try { await w.write(bytes instanceof Uint8Array ? bytes : Uint8Array.from(bytes)); }
      finally { w.releaseLock(); }
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
  // works even if the running firmware answers nothing
  (async () => {
    const at = makeAtClient(s.t);
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
  src.append(pinned, local, file);
  box.append(el("h3", "wap-col-h", "What goes on it"), src);

  const note = el("p", "fineprint", "");
  box.append(note);

  pinned.addEventListener("click", async () => {
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
    // give the app firmware a beat to come up, then handshake
    await sleep(1200);
    const ver = await at.cmd("VER?", { timeoutMs: 4000 });
    if (ver && ver.code === 0) {
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
      proof.textContent = "No AT answer after reboot — power-cycle the module (unplug/replug) " +
        "and it should come up with the new model. The burn itself completed and verified.";
    }
  })();

  // ── the bench: live preview + the two on-module thresholds ──
  const bench = el("div", "we2-bench");
  bench.append(el("h3", "wap-col-h", "Bench check — see what it sees (optional)"));
  bench.append(el("p", "fineprint",
    "This streams camera frames from the module to this page over your USB cable — the " +
    "attended, one-time bench check the guide describes. Frames render below and go nowhere " +
    "else. Mounted-and-deployed aiming stays boxes-only over MQTT (the Aim card)."));
  const startBtn = el("button", "primary", "Start live preview");
  const stopBtn = el("button", "ghost", "Stop");
  stopBtn.disabled = true;
  const rowB = el("div", "we2-btnrow");
  rowB.append(startBtn, stopBtn);
  const stage = el("div", "we2-preview");
  const img = document.createElement("canvas");
  img.className = "we2-preview-cv";
  stage.append(img);
  const meta = el("p", "fineprint", "");
  const seenBanner = el("div", "we2-seen flash-hidden");
  const sliders = el("div", "vis-preview-controls");
  const mkSlider = (label, cmd, init) => {
    const boxS = el("label", "vis-slider");
    const out = el("output", null, String(init));
    const input = document.createElement("input");
    input.type = "range"; input.min = "0"; input.max = "100"; input.value = String(init);
    input.addEventListener("change", async () => {
      out.value = input.value;
      await at.cmd(cmd + "=" + input.value, { timeoutMs: 2000 });
    });
    boxS.append(el("span", null, label), input, out);
    return boxS;
  };
  sliders.append(mkSlider("Confidence (TSCORE)", "TSCORE", 70), mkSlider("IoU (TIOU)", "TIOU", 45));
  bench.append(rowB, seenBanner, stage, sliders, meta);
  box.append(bench);

  // Only the pinned model is known to be the person detector. A model the user
  // brought can detect anything, so we don't claim its class is "person" (empty
  // map → generic "object" labels) and the celebration stays honest below.
  const previewClasses = job.pinned ? WE2_CLASSES : [];

  let previewing = false, seen = false;
  at.onEvent((f) => {
    if (!previewing || !f.data) return;
    const ctx2 = img.getContext("2d");
    const draw = (boxes) => {
      const dets = formatDetections(boxes, previewClasses);
      for (const d of dets) {
        // Confidence-tinted: a confident hit glows green, a marginal one stays amber.
        const hot = d.score >= 60;
        ctx2.lineWidth = 3;
        ctx2.strokeStyle = hot ? "#7CFF9B" : "#FFD44F";
        ctx2.strokeRect(d.x - d.w / 2, d.y - d.h / 2, d.w, d.h);
        ctx2.font = "700 13px ui-monospace, Menlo, monospace";
        const tw = ctx2.measureText(d.text).width + 10;
        ctx2.fillStyle = hot ? "rgba(10,38,20,0.9)" : "rgba(20,20,20,0.85)";
        ctx2.fillRect(d.x - d.w / 2, d.y - d.h / 2 - 20, tw, 19);
        ctx2.fillStyle = hot ? "#7CFF9B" : "#FFD44F";
        ctx2.fillText(d.text, d.x - d.w / 2 + 5, d.y - d.h / 2 - 6);
      }
      // A clean readout — "1 person · 92% confident" for the pinned model, or
      // "1 object · 92% confident" for a custom one — never a raw JSON dump.
      meta.textContent = detectionSummary(boxes, previewClasses);
      // The wow: the first time it detects anything, celebrate once. Only the
      // pinned person model earns "it sees you"; a custom model "sees something".
      if (dets.length && !seen) {
        seen = true;
        seenBanner.textContent = job.pinned
          ? "👁 It sees you — the model is live and working ✓"
          : "👁 It sees something — the model is live and working ✓";
        seenBanner.classList.remove("flash-hidden");
      }
    };
    if (f.data.image) {
      const image = new Image();
      image.onload = () => {
        img.width = image.width; img.height = image.height;
        ctx2.drawImage(image, 0, 0);
        draw(f.data.boxes);
      };
      image.src = "data:image/jpeg;base64," + f.data.image;
    } else if (f.data.boxes) {
      draw(f.data.boxes);
    }
  });
  startBtn.addEventListener("click", async () => {
    previewing = true;
    seen = false; seenBanner.classList.add("flash-hidden");
    meta.textContent = "watching…";
    startBtn.disabled = true; stopBtn.disabled = false;
    await at.cmd("INVOKE=-1,0,0", { timeoutMs: 4000 }); // continuous, with frames
  });
  stopBtn.addEventListener("click", async () => {
    previewing = false;
    startBtn.disabled = false; stopBtn.disabled = true;
    await at.cmd("BREAK", { timeoutMs: 2000 });
  });

  const done = el("button", "ghost", "Disconnect — I’m done");
  done.addEventListener("click", async () => {
    previewing = false;
    try { await at.cmd("BREAK", { timeoutMs: 800 }); } catch { /* leaving anyway */ }
    at.stop();
    await s.t.close();
    ctx.back();
  });
  box.append(done);
  return box;
}
