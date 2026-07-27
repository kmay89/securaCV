// canary-local/assets/eyes-bench.js — "through Canary eyes".
//
// Runs the REAL Canary Vision decision firmware (detect_config.cpp,
// presence_fsm.cpp, voxel_tracker.cpp — compiled to WebAssembly) on boxes
// derived from the visitor's webcam. The browser stages detection boxes —
// the same sensor boundary the Grove Vision AI V2 module occupies on
// hardware, where the NPU hands the ESP32 boxes and the ESP32 never sees a
// pixel. Every verdict after that boundary (is someone present, are they
// dwelling, which voxel, which event fires) is the firmware's, in wasm.
//
// The stand-in sensor is deliberately humble: it is a motion-blob detector,
// not a person detector. It marks moving things as the person class because
// motion is all it can honestly claim. The real module is smarter — it
// recognizes a person who holds still. The point of this bench is not the
// sensor; it is everything after the boundary, which is the same code the
// device runs.
//
// Safeguards, by construction (mirrors smoke-bench.js):
//   • The camera is requested only from an explicit button press (user
//     gesture), after the page has said in plain words what it will do.
//   • Frames are examined one at a time and immediately discarded — nothing
//     is recorded, buffered to disk, stored, or sent anywhere. There is no
//     network path in this file at all (bar reading the local build stamp).
//   • Stop fully releases the camera (every track stopped); so do pagehide
//     and a hidden tab.
//   • Graceful, specific fallbacks for: no getUserMedia, insecure context,
//     permission denied, no device.
//   • Decorative motion respects prefers-reduced-motion.
//   • A standing, unmissable disclaimer: this is a demonstration of how the
//     presence engine behaves, NOT a security device.

import { createVisionFirmwareCore } from "../emulator/web/vision-core.js";

const $ = (s, r = document) => r.querySelector(s);
function el(tag, cls, text) {
  const n = document.createElement(tag);
  if (cls) n.className = cls;
  if (text != null) n.textContent = text;
  return n;
}
function calm() {
  try { return matchMedia("(prefers-reduced-motion: reduce)").matches; }
  catch { return false; }
}

// ── the stand-in sensor (pure, exported for tests) ─────────────────────────
//
// Frame-difference motion → per-cell changed-pixel counts → connected blobs
// → SSCMA-shaped boxes {x,y,w,h,score,target} in TOP-LEFT frame coordinates,
// the convention vision_emu_push_box expects. (vision-ui.js's iou/nms speak
// the SSCMA center convention for the staged scene; do not mix the two.)

export const SENSOR = {
  cells: 24,        // mask resolution: cells per frame side
  pxDelta: 26,      // a pixel "moved" if its gray value changed this much
  cellOn: 12,       // moved pixels that light a cell (of cellPx² total)
  minCells: 3,      // blobs smaller than this are sensor noise
  holdMs: 900,      // sensor-side memory: how long a lost blob is held
  iouMerge: 0.35,   // overlapping blobs above this merge into one report
};

// Per-cell moved-pixel counts between two grayscale frames (w×h, row-major).
export function motionCells(prevGray, gray, w, h, opts = SENSOR) {
  const cells = opts.cells;
  const counts = new Uint16Array(cells * cells);
  if (!prevGray) return counts;
  for (let y = 0; y < h; y++) {
    const cy = Math.min(cells - 1, ((y * cells) / h) | 0);
    for (let x = 0; x < w; x++) {
      const k = y * w + x;
      const d = gray[k] - prevGray[k];
      if (d > opts.pxDelta || d < -opts.pxDelta) {
        counts[cy * cells + Math.min(cells - 1, ((x * cells) / w) | 0)]++;
      }
    }
  }
  return counts;
}

// Connected components (4-neighbor) over the lit cells → boxes in frame px.
export function blobBoxes(counts, w, h, opts = SENSOR) {
  const cells = opts.cells;
  const lit = new Uint8Array(cells * cells);
  for (let i = 0; i < lit.length; i++) lit[i] = counts[i] >= opts.cellOn ? 1 : 0;
  const seen = new Uint8Array(cells * cells);
  const boxes = [];
  const cw = w / cells, ch = h / cells;
  const cellArea = Math.max(1, (cw | 0) * (ch | 0));
  for (let start = 0; start < lit.length; start++) {
    if (!lit[start] || seen[start]) continue;
    let minR = cells, maxR = -1, minC = cells, maxC = -1, moved = 0, n = 0;
    const stack = [start];
    seen[start] = 1;
    while (stack.length) {
      const i = stack.pop();
      const r = (i / cells) | 0, c = i % cells;
      if (r < minR) minR = r; if (r > maxR) maxR = r;
      if (c < minC) minC = c; if (c > maxC) maxC = c;
      moved += counts[i]; n++;
      if (c > 0 && lit[i - 1] && !seen[i - 1]) { seen[i - 1] = 1; stack.push(i - 1); }
      if (c < cells - 1 && lit[i + 1] && !seen[i + 1]) { seen[i + 1] = 1; stack.push(i + 1); }
      if (r > 0 && lit[i - cells] && !seen[i - cells]) { seen[i - cells] = 1; stack.push(i - cells); }
      if (r < cells - 1 && lit[i + cells] && !seen[i + cells]) { seen[i + cells] = 1; stack.push(i + cells); }
    }
    if (n < opts.minCells) continue;
    // Score: blob size + how solidly its cells moved, mapped into 50–99.
    // It is a motion confidence, not a person confidence — and it is meant
    // to leave small flickers under the firmware's score threshold, so the
    // device's own filter is what discards them, visibly.
    const density = Math.min(1, moved / (n * cellArea));
    boxes.push({
      x: Math.round(minC * cw),
      y: Math.round(minR * ch),
      w: Math.max(1, Math.round((maxC - minC + 1) * cw)),
      h: Math.max(1, Math.round((maxR - minR + 1) * ch)),
      score: Math.min(99, 50 + n * 2 + Math.round(density * 30)),
      cells: n,
    });
  }
  return boxes;
}

// IoU / merge on TOP-LEFT boxes (matches what gets pushed into the wasm).
export function iouTopLeft(a, b) {
  const iw = Math.max(0, Math.min(a.x + a.w, b.x + b.w) - Math.max(a.x, b.x));
  const ih = Math.max(0, Math.min(a.y + a.h, b.y + b.h) - Math.max(a.y, b.y));
  const inter = iw * ih;
  const union = a.w * a.h + b.w * b.h - inter;
  return union <= 0 ? 0 : inter / union;
}

export function mergeBoxes(boxes, iouThr) {
  const sorted = [...(boxes || [])].sort((a, b) => b.score - a.score);
  const kept = [];
  for (const b of sorted) {
    if (kept.some((k) => iouTopLeft(k, b) > iouThr)) continue;
    kept.push(b);
  }
  return kept;
}

// The full stand-in sensor step: grays in, ≤32 merged boxes out.
export function detectBoxes(prevGray, gray, w, h, opts = SENSOR) {
  const counts = motionCells(prevGray, gray, w, h, opts);
  return mergeBoxes(blobBoxes(counts, w, h, opts), opts.iouMerge).slice(0, 32);
}

// Sensor-side short memory: a blob that just stopped moving is held briefly
// with a decaying score, the way real camera silicon keeps reporting someone
// who pauses mid-room. Purely sensor-side; the firmware's own lost_timeout
// still makes the presence call.
export function holdBoxes(memory, boxes, nowMs, opts = SENSOR) {
  if (boxes.length) return { boxes, memory: { boxes, atMs: nowMs } };
  if (memory && memory.boxes.length && nowMs - memory.atMs < opts.holdMs) {
    const held = memory.boxes.map((b) => ({
      ...b,
      score: Math.max(1, b.score - Math.round(((nowMs - memory.atMs) / opts.holdMs) * 40)),
    }));
    return { boxes: held, memory };
  }
  return { boxes: [], memory: null };
}

// Friendly one-liners for the firmware's event vocabulary.
export const EVENT_TEXT = {
  presence_started: "presence started — the room is no longer empty",
  dwell_started: "dwell started — they are staying, not passing through",
  dwell_ended: "dwell ended",
  presence_ended: "presence ended — the room reads empty again",
  interaction_likely: "interaction likely — someone lingered, then left",
};

// ── wasm core boot ─────────────────────────────────────────────────────────

async function readStamp() {
  try {
    const r = await fetch("emulator/dist/canary-vision-core.meta.json", { cache: "no-store" });
    if (r.ok) return await r.json();
  } catch { /* file:// or offline — the chip just omits the version */ }
  return null;
}

function mountVersions(stamp) {
  const strip = $("#eyes-versions");
  if (!strip) return;
  strip.innerHTML = "";
  const pill = (label, val) => {
    const p = el("span", "pill");
    p.append(el("strong", null, label + " "), document.createTextNode(val));
    return p;
  };
  if (stamp && stamp.fw_version) strip.append(pill("firmware", stamp.fw_version));
  strip.append(pill("runtime", "real firmware wasm"));
  strip.append(pill("engine", "canary-vision-core (vendored, offline)"));
}

// ── page state ─────────────────────────────────────────────────────────────

const state = {
  core: null,
  contract: null,
  running: false,
  stream: null,
  video: null,
  raf: 0,
  lastTickMs: -1,
  t0: 0,
  prevGray: null,
  memory: null,
  lastBoxes: [],
  lastTick: null,
  events: [],
};

// Lazy — this module's pure sensor helpers also load under Node for the
// honesty-gate test, where there is no document.
let work = null, workCtx = null;
function workCanvas() {
  if (!work) {
    work = document.createElement("canvas");
    workCtx = work.getContext("2d", { willReadFrequently: true });
  }
  return work;
}

function supportProblem() {
  if (!window.isSecureContext) {
    return "Camera access needs a secure page (https, or localhost). " +
           "Open this page over https and the button will work.";
  }
  if (!navigator.mediaDevices || !navigator.mediaDevices.getUserMedia) {
    return "This browser doesn't offer camera access to web pages. " +
           "Try a current Chrome, Edge, Firefox or Safari.";
  }
  return null;
}

// ── rendering ──────────────────────────────────────────────────────────────

function render() {
  const mount = $("#eyes");
  mount.innerHTML = "";

  // The standing disclaimer — always visible, never dismissible.
  const disc = el("div", "eyes-disclaimer");
  disc.append(el("strong", null, "This is a demonstration, not a security device. "));
  disc.append(document.createTextNode(
    "It shows how a Canary Vision decides presence. The stand-in sensor here " +
    "is a motion detector, not the real camera module — it loses people who " +
    "hold perfectly still, and the real module does not. Everything after " +
    "the sensor is the device's own firmware."));
  mount.append(disc);

  const card = el("section", "eyes-card");
  card.id = "eyes-card";
  mount.append(card);

  if (!state.running) renderIdle(card);
  else renderLive(card);

  renderFacts(mount);
}

function liNode(...parts) {
  const li = el("li");
  li.append(...parts.map((x) => (typeof x === "string" ? document.createTextNode(x) : x)));
  return li;
}

function renderIdle(card) {
  card.append(el("h2", null, "See what crosses the boundary"));
  const how = el("ol", "eyes-how");
  how.append(liNode("Press ", el("strong", null, "Start watching"),
    " below and allow the camera when your browser asks."));
  how.append(liNode("Walk into frame. Watch yourself become a box, the box " +
    "become a voxel, and the firmware call ", el("strong", null, "presence"), "."));
  how.append(liNode("Stay a while — ", el("strong", null, "dwell"),
    " fires. Leave — the room reads empty again. Every one of those calls " +
    "is made in the device's compiled firmware, not in this page."));
  card.append(how);

  const problem = supportProblem();
  if (problem) {
    card.append(el("p", "eyes-warn", problem));
    return;
  }

  const priv = el("p", "eyes-priv");
  priv.append(el("span", "eyes-priv-key", "🔒 "));
  priv.append(document.createTextNode(
    "Your camera stays in this page. Frames are examined one at a time and " +
    "thrown away — never recorded, saved, or uploaded. Only boxes cross into " +
    "the firmware, exactly like on the real device. Stop anytime and the " +
    "camera is released at once."));
  card.append(priv);

  const btn = el("button", "eyes-start", "📷 Start watching");
  btn.type = "button";
  btn.addEventListener("click", () => start(btn));
  card.append(btn);
}

function renderLive(card) {
  card.classList.add("eyes-live");

  const head = el("div", "eyes-live-head");
  head.append(el("span", "eyes-watching-dot"),
    el("span", null, "Watching — the firmware presence engine is running on your camera"));
  const stop = el("button", "eyes-stop", "◼ Stop & release camera");
  stop.type = "button";
  stop.addEventListener("click", stopWatching);
  head.append(stop);
  card.append(head);

  const panes = el("div", "eyes-panes");
  card.append(panes);

  // 1 — sensor side: the room, seen only by this page.
  const sensor = el("div", "eyes-pane");
  sensor.append(el("h3", null, "Sensor side — your room"));
  sensor.append(el("p", "eyes-pane-sub",
    "Seen only by this page, mirrored. The gold boxes are the stand-in " +
    "sensor's motion blobs — the last thing that exists as pixels."));
  const stage = el("div", "eyes-stage");
  state.video = document.createElement("video");
  state.video.playsInline = true;
  state.video.muted = true;
  stage.append(state.video);
  const overlay = document.createElement("canvas");
  overlay.className = "eyes-overlay";
  stage.append(overlay);
  sensor.append(stage);
  panes.append(sensor);

  // 2 — the boundary: the exact payload, and the exact nothing-else.
  const boundary = el("div", "eyes-pane");
  boundary.append(el("h3", null, "Across the boundary"));
  boundary.append(el("p", "eyes-pane-sub",
    "The integers the sensor hands the firmware — six per box, nothing else. " +
    "No pixels exist on this side."));
  const aim = document.createElement("canvas");
  aim.className = "eyes-aim";
  boundary.append(aim);
  const payload = el("code", "eyes-payload", "—");
  payload.id = "eyes-payload";
  boundary.append(payload);
  panes.append(boundary);

  // 3 — the firmware's verdict.
  const verdict = el("div", "eyes-pane");
  verdict.append(el("h3", null, "The firmware's verdict"));
  verdict.append(el("p", "eyes-pane-sub",
    "Read straight from the wasm each tick — presence, dwell, confidence, " +
    "and the events the device would publish."));
  const chips = el("div", "eyes-chips");
  chips.id = "eyes-chips";
  verdict.append(chips);
  const log = el("ol", "eyes-events");
  log.id = "eyes-events";
  log.setAttribute("aria-live", "polite");
  verdict.append(log);
  panes.append(verdict);

  state.overlay = overlay;
  state.aim = aim;
}

// The real firmware settings — read from the running core, never hardcoded.
function renderFacts(mount) {
  if (!state.contract) return;
  const c = state.contract;
  const details = el("details", "eyes-facts");
  details.append(el("summary", null, "the real firmware settings (read from the wasm)"));
  const table = el("table");
  const row = (k, v) => {
    const tr = el("tr");
    tr.append(el("td", null, k), el("td", null, String(v)));
    return tr;
  };
  table.append(
    row("firmware", c.firmware),
    row("frame", `${c.frame_w} × ${c.frame_h}`),
    row("voxel grid", `${c.voxel_rows} × ${c.voxel_cols}`),
    row("invoke period", `${c.invoke_period_ms} ms`),
    row("person class", c.person_target),
    row("score threshold", `≥ ${c.score_min}`),
    row("lost timeout", `${c.lost_timeout_ms} ms`),
    row("dwell start", `${c.dwell_start_ms} ms`),
  );
  details.append(table);
  mount.append(details);
}

// ── the live loop ──────────────────────────────────────────────────────────

async function start(btn) {
  btn.disabled = true;
  try {
    state.stream = await navigator.mediaDevices.getUserMedia({
      video: { width: { ideal: 640 }, height: { ideal: 480 }, facingMode: "user" },
      audio: false,
    });
  } catch (err) {
    btn.disabled = false;
    const why = err && err.name === "NotAllowedError"
      ? "Camera permission was declined — that's fine. Nothing runs without it."
      : err && err.name === "NotFoundError"
        ? "No camera was found on this device."
        : "The camera could not be started (" + (err && err.name || "unknown") + ").";
    const old = $(".eyes-warn");
    if (old) old.remove();
    $("#eyes-card").append(el("p", "eyes-warn", why));
    return;
  }

  state.running = true;
  state.prevGray = null;
  state.memory = null;
  state.events = [];
  state.lastTickMs = -1;
  state.t0 = performance.now();
  state.core.reset();
  render();

  state.video.srcObject = state.stream;
  await state.video.play().catch(() => {});
  state.raf = requestAnimationFrame(loop);
}

function stopWatching() {
  state.running = false;
  if (state.raf) cancelAnimationFrame(state.raf);
  state.raf = 0;
  if (state.stream) {
    for (const t of state.stream.getTracks()) t.stop();
    state.stream = null;
  }
  render();
}

function loop() {
  if (!state.running) return;
  state.raf = requestAnimationFrame(loop);
  const c = state.contract;
  const nowMs = Math.round(performance.now() - state.t0);
  if (state.lastTickMs >= 0 && nowMs - state.lastTickMs < c.invoke_period_ms) return;
  state.lastTickMs = nowMs;

  const video = state.video;
  if (!video || !video.videoWidth) return;

  // Stage the frame at the firmware's own retina, mirrored, cover-cropped.
  const W = c.frame_w, H = c.frame_h;
  workCanvas();
  if (work.width !== W) { work.width = W; work.height = H; }
  const vw = video.videoWidth, vh = video.videoHeight;
  const scale = Math.max(W / vw, H / vh);
  const cw = W / scale, ch = H / scale;
  workCtx.save();
  workCtx.scale(-1, 1);
  workCtx.drawImage(video, (vw - cw) / 2, (vh - ch) / 2, cw, ch, -W, 0, W, H);
  workCtx.restore();

  const frame = workCtx.getImageData(0, 0, W, H).data;
  const gray = new Uint8ClampedArray(W * H);
  for (let i = 0, j = 0; j < gray.length; i += 4, j++) {
    gray[j] = (0.299 * frame[i] + 0.587 * frame[i + 1] + 0.114 * frame[i + 2]) | 0;
  }

  // The stand-in sensor: motion blobs, briefly held, marked person-class
  // because motion is all it can honestly claim.
  const fresh = detectBoxes(state.prevGray, gray, W, H);
  state.prevGray = gray;
  const held = holdBoxes(state.memory, fresh, nowMs);
  state.memory = held.memory;
  const boxes = held.boxes.map((b) => ({
    x: b.x, y: b.y, w: b.w, h: b.h, score: b.score, target: c.person_target,
  }));
  state.lastBoxes = boxes;

  // Across the boundary: the firmware decides.
  state.lastTick = state.core.tick(nowMs, boxes);
  if (state.lastTick.event) {
    state.events.push({
      atMs: nowMs,
      event: state.lastTick.event,
      reason: state.lastTick.reason,
    });
  }
  paint();
}

function paint() {
  const c = state.contract;
  const tick = state.lastTick;

  // Sensor overlay: boxes over the mirrored video.
  const ov = state.overlay;
  if (ov) {
    const rect = ov.parentElement.getBoundingClientRect();
    if (ov.width !== (rect.width | 0)) { ov.width = rect.width; ov.height = rect.height; }
    const g = ov.getContext("2d");
    g.clearRect(0, 0, ov.width, ov.height);
    const sx = ov.width / c.frame_w, sy = ov.height / c.frame_h;
    g.strokeStyle = "#ffd44f";
    g.lineWidth = 2;
    for (const b of state.lastBoxes) g.strokeRect(b.x * sx, b.y * sy, b.w * sx, b.h * sy);
  }

  // Boundary pane: boxes + voxel grid on black — coordinates, nothing else.
  const aim = state.aim;
  if (aim) {
    if (aim.width !== c.frame_w) { aim.width = c.frame_w; aim.height = c.frame_h; }
    const g = aim.getContext("2d");
    g.fillStyle = "#08090b";
    g.fillRect(0, 0, aim.width, aim.height);
    g.strokeStyle = "rgba(255,255,255,0.12)";
    g.lineWidth = 1;
    for (let r = 1; r < c.voxel_rows; r++) {
      g.beginPath();
      g.moveTo(0, (r * c.frame_h) / c.voxel_rows);
      g.lineTo(c.frame_w, (r * c.frame_h) / c.voxel_rows);
      g.stroke();
    }
    for (let col = 1; col < c.voxel_cols; col++) {
      g.beginPath();
      g.moveTo((col * c.frame_w) / c.voxel_cols, 0);
      g.lineTo((col * c.frame_w) / c.voxel_cols, c.frame_h);
      g.stroke();
    }
    if (tick && tick.sample && tick.sample.person_now && tick.sample.voxel) {
      const v = tick.sample.voxel;
      g.fillStyle = "rgba(255,212,79,0.14)";
      g.fillRect((v.c * c.frame_w) / c.voxel_cols, (v.r * c.frame_h) / c.voxel_rows,
        c.frame_w / c.voxel_cols, c.frame_h / c.voxel_rows);
    }
    g.strokeStyle = "#ffd44f";
    for (const b of state.lastBoxes) g.strokeRect(b.x, b.y, b.w, b.h);
  }

  const payload = $("#eyes-payload");
  if (payload) {
    payload.textContent = state.lastBoxes.length
      ? state.lastBoxes.map((b) =>
          `{x:${b.x} y:${b.y} w:${b.w} h:${b.h} score:${b.score} target:${b.target}}`).join(" ")
      : "(no boxes this tick — an empty frame is also a report)";
  }

  const chips = $("#eyes-chips");
  if (chips && tick) {
    chips.innerHTML = "";
    const chip = (label, on, extra) => {
      const s = el("span", "eyes-chip" + (on ? " on" : ""), label + (extra ? " " + extra : ""));
      chips.append(s);
    };
    chip("presence", !!tick.fsm.presence, tick.fsm.presence ? `${(tick.fsm.presence_ms / 1000).toFixed(1)}s` : "");
    chip("dwelling", !!tick.fsm.dwelling, tick.fsm.dwelling ? `${(tick.fsm.dwell_ms / 1000).toFixed(1)}s` : "");
    chip("confidence", tick.fsm.confidence > 0, String(tick.fsm.confidence));
    if (tick.sample) {
      chip("count", tick.sample.person_count > 0, String(tick.sample.person_count));
      if (tick.sample.posture) chip("posture", true, tick.sample.posture);
      if (tick.sample.proximity) chip("proximity", true, tick.sample.proximity);
    }
  }

  const log = $("#eyes-events");
  if (log && log.childElementCount !== state.events.length) {
    log.innerHTML = "";
    for (const e of [...state.events].reverse().slice(0, 30)) {
      const li = el("li");
      li.append(el("strong", null, e.event));
      li.append(document.createTextNode(
        " · " + (EVENT_TEXT[e.event] || "") +
        (e.reason ? ` (${e.reason})` : "") +
        ` · t+${(e.atMs / 1000).toFixed(1)}s`));
      log.append(li);
    }
  }
}

// ── boot ───────────────────────────────────────────────────────────────────

async function boot() {
  const mount = $("#eyes");
  try {
    state.core = await createVisionFirmwareCore(globalThis.createCanaryVisionCore);
    state.contract = state.core.contract;
  } catch (err) {
    mount.innerHTML = "";
    const p = el("p", "eyes-warn",
      "The committed Canary Vision firmware core could not start (" +
      String(err && err.message || err) + "). The same pipeline is described in " +
      "the Vision bench and docs/hardware/canary_vision_getting_started.md.");
    mount.append(p);
    return;
  }
  mountVersions(await readStamp());
  render();
  if (calm()) document.documentElement.classList.add("eyes-calm");
}

if (typeof document !== "undefined" && $("#eyes")) {
  addEventListener("pagehide", stopWatching);
  addEventListener("visibilitychange", () => {
    if (document.visibilityState === "hidden" && state.running) stopWatching();
  });
  boot();
}
