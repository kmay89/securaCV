// canary-local/assets/vision-ui.js — the Vision bench widgets.
//
// Staged surfaces for the canary-vision device (Grove Vision AI V2 + ESP32
// host), all fed from the drift-gated devices/vision.json (tools/
// gen_vision.py parses the firmware + docs):
//
//   · buildPorts     — the two-USB-C-port gotcha, as a hands-on picker: pick
//                      a task, plug the right port, learn why the wrong one
//                      can't work.
//   · buildModelLoad — the one-time SenseCraft model load, staged click by
//                      click, ending in a live preview pane whose bounding
//                      boxes run the same detection semantics the firmware
//                      does (best-box, class filter, thresholds, NMS).
//   · buildSerial    — the host's USB-CDC console: the real boot banner +
//                      setup() log stream, values straight from config.h.
//   · buildMqtt      — an MQTT-explorer view: retained topics fill on
//                      connect, events stream, the HA discovery set announces.
//   · buildAim       — the Aim card emulation: the boxes-only live channel,
//                      drawn the way the Lovelace card draws it — a wireframe
//                      box and a voxel cell on black. No scene. That's the point.
//   · buildTunePlay  — the sandbox: scenario buttons drive a staged scene,
//                      four live tuning numbers change what the witness
//                      claims, the FSM pills + event log show the result.
//   · buildPlacement — placement rules + use-case presets that apply real
//                      threshold/timeout numbers to the sandbox.
//   · buildFlashHost — flashing the ESP32 host (and the module-flasher roadmap).
//
// The preview canvas draws a STAGED SCENE — cartoon actors, not a camera
// feed — and says so on its face. The detection math on top of it is not
// staged: bestBox / bboxToVoxel / the presence FSM below mirror
// vision_mgr.cpp and presence_fsm.cpp line for line, and are pinned by
// tests/vision.test.js against the firmware sources.

import { buildBoardLab } from "./board-lab.js";

const el = (tag, cls, text) => {
  const n = document.createElement(tag);
  if (cls) n.className = cls;
  if (text != null) n.textContent = text;
  return n;
};
const alive = (node) => document.body.contains(node);

// ── DOM-free cores (exported; pinned in tests/vision.test.js) ─────────────

// Substitute the illustrative device id into a topic/template.
export function withId(tmpl, id) {
  return String(tmpl).split("<device_id>").join(id).split("<id>").join(id);
}

// Mirrors vision_mgr.cpp pick_best_person_box(): person class only, score at
// or above the threshold, highest score wins. One box, however many people.
export function bestBox(boxes, cfg) {
  let found = null, best = -1;
  for (const b of boxes || []) {
    if (b.target !== cfg.person_target) continue;
    if (b.score < cfg.score_min) continue;
    if (b.score > best) { best = b.score; found = b; }
  }
  return found;
}

// Mirrors vision_mgr.cpp bbox_to_voxel(): the box CENTER (SSCMA boxes carry
// center coords; w/h spread around it) lands in one integer grid cell.
// Matches the firmware's integer math exactly, clamps included.
export function bboxToVoxel(bb, g) {
  const cols = g.cols || 1, rows = g.rows || 1;
  const cx = bb.x + Math.trunc(bb.w / 2);
  const cy = bb.y + Math.trunc(bb.h / 2);
  // |0 truncates toward zero like C integer division (and folds away -0)
  let c = ((cx * cols) / g.w) | 0;
  let r = ((cy * rows) / g.h) | 0;
  if (c < 0) c = 0; if (c > cols - 1) c = cols - 1;
  if (r < 0) r = 0; if (r > rows - 1) r = rows - 1;
  return { r, c, rows, cols };
}

// Intersection-over-union on center-coordinate boxes (the SSCMA convention).
export function iou(a, b) {
  const ax0 = a.x - a.w / 2, ay0 = a.y - a.h / 2, ax1 = a.x + a.w / 2, ay1 = a.y + a.h / 2;
  const bx0 = b.x - b.w / 2, by0 = b.y - b.h / 2, bx1 = b.x + b.w / 2, by1 = b.y + b.h / 2;
  const iw = Math.max(0, Math.min(ax1, bx1) - Math.max(ax0, bx0));
  const ih = Math.max(0, Math.min(ay1, by1) - Math.max(ay0, by0));
  const inter = iw * ih;
  const union = a.w * a.h + b.w * b.h - inter;
  return union <= 0 ? 0 : inter / union;
}

// Non-max suppression — what SenseCraft's IoU slider stages: boxes that
// overlap a higher-scoring box more than the threshold are dropped.
export function nms(boxes, iouThr) {
  const sorted = [...(boxes || [])].sort((a, b) => b.score - a.score);
  const kept = [];
  for (const b of sorted) {
    if (kept.some((k) => k.target === b.target && iou(k, b) > iouThr)) continue;
    kept.push(b);
  }
  return kept;
}

// The firmware's aim payload, key for key (main.cpp aim_publish()).
export function aimPayload(sample, g) {
  const p = sample && sample.person_now;
  const bb = p ? sample.bbox : { x: 0, y: 0, w: 0, h: 0, score: 0 };
  const v = p ? sample.voxel : { r: -1, c: -1 };
  return {
    present: !!p,
    x: bb.x, y: bb.y, w: bb.w, h: bb.h,
    score: bb.score || 0,
    vr: v.r, vc: v.c, rows: g.rows, cols: g.cols,
    fw: g.w, fh: g.h,
  };
}

// The presence FSM, mirroring presence_fsm.cpp for the full vocabulary:
// presence_started → dwell_started → dwell_ended → presence_ended →
// interaction_likely. One event per tick, in the firmware's order:
// dwell_ended fires the tick before presence_ended, and a visit that
// qualified — it dwelled, or held one voxel cell past the zone window —
// signs interaction_likely within the post-leave window. `lastReason`
// carries the firmware's reason string for the event payload.
export function makeFsm() {
  const s = {
    presence: false, dwelling: false, presenceStart: 0, lastSeen: 0,
    dwellLatch: false, zoneLatch: false, zoneCandidate: false,
    stableCell: null, stableEnter: 0,
    lastLeave: 0, lastLeaveSeen: 0, interactionEmitted: false,
  };
  const fsm = {
    lastReason: null,
    get state() { return { presence: s.presence, dwelling: s.dwelling }; },
    reset() {
      s.presence = s.dwelling = false; s.presenceStart = s.lastSeen = 0;
      s.dwellLatch = s.zoneLatch = s.zoneCandidate = false;
      s.stableCell = null; s.stableEnter = 0;
      s.lastLeave = s.lastLeaveSeen = 0; s.interactionEmitted = false;
      fsm.lastReason = null;
    },
    // `cell` is the occupied voxel cell key ("r,c") — the voxel_tracker
    // mirror: entering a different cell restarts the stable-zone clock.
    tick(personNow, nowMs, cfg, cell = null) {
      fsm.lastReason = null;
      if (personNow) {
        s.lastSeen = nowMs;
        if (cell != null && cell !== s.stableCell) { s.stableCell = cell; s.stableEnter = nowMs; }
        if (!s.presence) {
          s.presence = true; s.dwelling = false; s.presenceStart = nowMs;
          s.dwellLatch = s.zoneLatch = s.zoneCandidate = false;
          s.interactionEmitted = false;
          s.stableCell = cell; s.stableEnter = nowMs;
          return "presence_started";
        }
        if (!s.dwelling && nowMs - s.presenceStart >= cfg.dwell_start_ms) {
          s.dwelling = true;
          return "dwell_started";
        }
        if (!s.zoneCandidate && cell != null &&
            nowMs - s.stableEnter >= (cfg.zone_interaction_ms ?? 2500)) {
          s.zoneCandidate = true;
        }
        if (s.dwelling) s.dwellLatch = true;
        if (s.zoneCandidate) s.zoneLatch = true;
        return null;
      }
      if (s.presence && nowMs - s.lastSeen > cfg.lost_timeout_ms) {
        if (s.dwelling) { s.dwelling = false; return "dwell_ended"; }
        s.presence = false;
        s.lastLeave = nowMs;
        return "presence_ended";
      }
      if (!s.presence && s.lastLeave !== 0 && s.lastLeave !== s.lastLeaveSeen) {
        s.lastLeaveSeen = s.lastLeave;
        s.interactionEmitted = false;
      }
      if (!s.presence && s.lastLeave !== 0 && !s.interactionEmitted) {
        const win = cfg.interaction_window_ms ?? 3000;
        const qualified = s.dwellLatch || s.zoneLatch;
        if (qualified && nowMs - s.lastLeave <= win) {
          s.interactionEmitted = true;
          fsm.lastReason = s.dwellLatch ? "dwell_then_left" : "zone_interaction_then_left";
          s.dwellLatch = s.zoneLatch = false;
          return "interaction_likely";
        }
        if (nowMs - s.lastLeave > win) {
          s.interactionEmitted = true;
          s.dwellLatch = s.zoneLatch = false;
        }
      }
      return null;
    },
  };
  return fsm;
}

// Flatten the serial data into ordered {cls,text} console lines.
export function bootLines(serial) {
  const out = [];
  for (const t of serial.banner || []) out.push({ cls: "wap-b", text: t });
  for (const s of serial.boot || []) {
    const tag = s.tag || "";
    const cls = tag === "[OK]" ? "ok" : tag === "[WARN]" ? "warn"
      : tag === "[--]" ? "faint"
      : tag === "[WIFI]" || tag === "[MQTT]" || tag === "[I2C]" || tag === "[DISC]" ? "net" : "b";
    out.push({ cls: "wap-" + cls, text: `${tag} ${s.text}`.trim() });
  }
  for (const t of serial.ready || []) out.push({ cls: "wap-b", text: t });
  return out;
}

// The retained-topic store the broker keeps.
export function mqttApply(store, msg) {
  if (!msg || !msg.topic) return store;
  if (msg.clear) delete store[msg.topic];
  else if (msg.retain) store[msg.topic] = msg.payload;
  return store;
}

// ── the staged scene simulation ───────────────────────────────────────────
// A deterministic-enough cartoon room in SSCMA frame coordinates. Actors
// produce raw candidate boxes (class + noisy score); everything downstream —
// preview NMS, firmware best-box, FSM — runs the real math above.

export class VisionSim {
  constructor(data) {
    this.g = { cols: data.detect.voxel.cols, rows: data.detect.voxel.rows,
               w: data.detect.frame.w, h: data.detect.frame.h };
    this.cfg = {
      person_target: data.detect.person_target,
      score_min: data.detect.score_min,
      lost_timeout_ms: data.detect.lost_timeout_ms,
      dwell_start_ms: data.detect.dwell_start_ms,
      interaction_window_ms: data.detect.interaction_window_ms,
      zone_interaction_ms: data.detect.zone_interaction_ms,
    };
    // SSCMA-Micro's own YOLO defaults (TSCORE 50 / TIOU 45); overwritten
    // from data.model_load.wire in vision.js when present
    this.preview = { confidence: 50, iou: 45 };
    this.fsm = makeFsm();
    this.actors = [];        // {kind, t0, dur, ...}
    this.raw = [];           // last frame's raw boxes
    this.sample = { person_now: false, bbox: null, voxel: null };
    this.lastEvent = null;
    this.listeners = { frame: [], event: [] };
    this.t = 0;              // sim clock, ms
    this._acc = 0;
  }
  on(type, fn) { this.listeners[type].push(fn); }
  emitEvent(name) {
    this.lastEvent = name;
    for (const fn of this.listeners.event) fn(name, this.snapshot());
  }
  snapshot() {
    return { t: this.t, raw: this.raw, sample: this.sample,
             fsm: this.fsm.state, reason: this.fsm.lastReason,
             cfg: { ...this.cfg }, g: this.g };
  }

  // scenario launchers — each stages actors on the sim clock
  run(id) {
    const t = this.t;
    const W = this.g.w;
    if (id === "walk") {
      this.actors.push({ kind: "person", t0: t, dur: 6000, from: -30, to: W + 30, y: 118, hgt: 128 });
    } else if (id === "linger") {
      // walk in, then stand near center past the dwell timer
      const dwell = this.cfg.dwell_start_ms;
      this.actors.push({ kind: "person", t0: t, dur: 2600 + dwell + 3000, from: -30, to: W / 2,
                         y: 116, hgt: 130, holdAt: 2600, holdFor: dwell + 2500 });
    } else if (id === "leave") {
      // whoever is on stage walks off; if nobody is, stage a quick exit
      let any = false;
      for (const a of this.actors) {
        if (a.kind === "person" || a.kind === "person2") { a.exitAt = t; any = true; }
      }
      if (!any) this.actors.push({ kind: "person", t0: t, dur: 2200, from: W / 2, to: W + 30, y: 118, hgt: 128 });
    } else if (id === "cat") {
      this.actors.push({ kind: "cat", t0: t, dur: 5000, from: W + 20, to: -20, y: 196, hgt: 30 });
    } else if (id === "two") {
      this.actors.push({ kind: "person", t0: t, dur: 7000, from: -30, to: W + 30, y: 118, hgt: 132 });
      this.actors.push({ kind: "person2", t0: t + 500, dur: 7000, from: W + 30, to: -30, y: 132, hgt: 104 });
    } else if (id === "tv") {
      this.actors.push({ kind: "tv", t0: t, dur: 14000 });
    }
  }

  actorBox(a, now) {
    const k = Math.min(1, Math.max(0, (now - a.t0) / a.dur));
    if (a.kind === "tv") {
      // a face on screen: person-class, low and flickery score
      const on = Math.floor((now - a.t0) / 400) % 5 !== 4; // drops out periodically
      if (!on) return null;
      return { x: 186, y: 74, w: 34, h: 40, target: this.cfg.person_target,
               score: 52 + Math.round(Math.sin(now / 230) * 9), actor: "tv" };
    }
    let px;
    if (a.exitAt != null) {
      const kk = Math.min(1, (now - a.exitAt) / 1600);
      px = a._lastX != null ? a._lastX + (this.g.w + 40 - a._lastX) * kk : this.g.w + 40;
      if (kk >= 1) a.done = true;
    } else if (a.holdAt != null) {
      const walkMs = a.holdAt;
      const tt = now - a.t0;
      if (tt < walkMs) px = a.from + (a.to - a.from) * (tt / walkMs);
      else if (tt < walkMs + a.holdFor) px = a.to + Math.sin(now / 500) * 2; // breathing sway
      else { const kk = (tt - walkMs - a.holdFor) / 2400; px = a.to + (this.g.w + 40 - a.to) * Math.min(1, kk); }
    } else {
      px = a.from + (a.to - a.from) * k;
    }
    a._lastX = px;
    if (k >= 1 && a.holdAt == null && a.exitAt == null) a.done = true;
    if (px < -28 || px > this.g.w + 28) return null;
    const jitter = () => Math.round((Math.random() - 0.5) * 3);
    if (a.kind === "cat") {
      return { x: Math.round(px) + jitter(), y: a.y + jitter(), w: 44, h: a.hgt,
               target: 8, score: 62 + Math.round(Math.random() * 18), actor: "cat" };
    }
    const w = a.kind === "person2" ? 52 : 62;
    // scores sag near the frame edges (partial body) — the aiming lesson
    const edge = Math.min(px, this.g.w - px);
    const edgePenalty = edge < 40 ? Math.round((40 - Math.max(0, edge)) * 0.45) : 0;
    const base = a.kind === "person2" ? 84 : 90;
    return { x: Math.round(px) + jitter(), y: a.y + jitter(), w, h: a.hgt,
             target: this.cfg.person_target,
             score: Math.max(25, base - edgePenalty + Math.round((Math.random() - 0.5) * 6)),
             actor: a.kind };
  }

  // advance the sim clock; INVOKE_PERIOD_MS cadence for the detector
  tick(dtMs, invokePeriodMs) {
    this.t += dtMs;
    this._acc += dtMs;
    if (this._acc < invokePeriodMs) return null;
    this._acc = 0;

    this.actors = this.actors.filter((a) => !a.done && this.t - a.t0 < a.dur + 8000);
    const raw = [];
    for (const a of this.actors) {
      const b = this.actorBox(a, this.t);
      if (b) {
        raw.push(b);
        // the model sometimes emits a duplicate, offset lower-scoring box —
        // this is what the IoU/NMS slider exists to clean up
        if ((a.kind === "person" || a.kind === "person2") && Math.random() < 0.35) {
          raw.push({ ...b, x: b.x + 7, y: b.y - 5, w: b.w + 8,
                     score: Math.max(20, b.score - 14 - Math.round(Math.random() * 10)),
                     dup: true });
        }
      }
    }
    this.raw = raw;

    const bb = bestBox(raw, this.cfg);
    this.sample = {
      person_now: !!bb,
      bbox: bb,
      voxel: bb ? bboxToVoxel(bb, this.g) : { r: -1, c: -1, rows: this.g.rows, cols: this.g.cols },
    };
    const cell = bb ? this.sample.voxel.r + "," + this.sample.voxel.c : null;
    const ev = this.fsm.tick(!!bb, this.t, this.cfg, cell);
    for (const fn of this.listeners.frame) fn(this.snapshot());
    if (ev) this.emitEvent(ev);
    return ev;
  }
}

// ── canvas painters ───────────────────────────────────────────────────────

function scaleCanvas(cv, cssW, cssH) {
  const dpr = Math.min(2, window.devicePixelRatio || 1);
  cv.width = cssW * dpr; cv.height = cssH * dpr;
  cv.style.width = cssW + "px"; cv.style.height = cssH + "px";
  const ctx = cv.getContext("2d");
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  return ctx;
}

// The staged room, drawn in frame coords scaled to the canvas.
function drawScene(ctx, W, H, sim) {
  const sx = W / sim.g.w, sy = H / sim.g.h;
  ctx.clearRect(0, 0, W, H);
  // wall + floor
  ctx.fillStyle = "#191b20"; ctx.fillRect(0, 0, W, H);
  ctx.fillStyle = "#101216"; ctx.fillRect(0, H * 0.72, W, H * 0.28);
  // window (left) — the backlight trap
  ctx.fillStyle = "#232a36"; ctx.fillRect(14 * sx, 28 * sy, 52 * sx, 66 * sy);
  ctx.strokeStyle = "#2e3947"; ctx.lineWidth = 2;
  ctx.strokeRect(14 * sx, 28 * sy, 52 * sx, 66 * sy);
  ctx.beginPath();
  ctx.moveTo(40 * sx, 28 * sy); ctx.lineTo(40 * sx, 94 * sy);
  ctx.moveTo(14 * sx, 61 * sy); ctx.lineTo(66 * sx, 61 * sy);
  ctx.stroke();
  // door frame (right)
  ctx.strokeStyle = "#33302a"; ctx.lineWidth = 3;
  ctx.strokeRect(W - 46 * sx, 34 * sy, 34 * sx, H * 0.72 - 34 * sy);
  // TV on the back wall
  const tvOn = sim.actors.some((a) => a.kind === "tv");
  ctx.fillStyle = tvOn ? "#3b4f66" : "#0c0d10";
  ctx.fillRect(168 * sx, 52 * sy, 52 * sx, 34 * sy);
  ctx.strokeStyle = "#2a2c31"; ctx.lineWidth = 2;
  ctx.strokeRect(168 * sx, 52 * sy, 52 * sx, 34 * sy);
  if (tvOn) { // the face on screen
    ctx.fillStyle = "#c8b6a0";
    ctx.beginPath(); ctx.arc(194 * sx, 66 * sy, 7 * sx, 0, Math.PI * 2); ctx.fill();
    ctx.fillRect(188 * sx, 74 * sy, 12 * sx, 10 * sy);
  }
  // plant (front left)
  ctx.strokeStyle = "#2e4632"; ctx.lineWidth = 3;
  ctx.beginPath();
  ctx.moveTo(30 * sx, 200 * sy); ctx.quadraticCurveTo(24 * sx, 172 * sy, 34 * sx, 156 * sy);
  ctx.moveTo(30 * sx, 200 * sy); ctx.quadraticCurveTo(40 * sx, 176 * sy, 30 * sx, 158 * sy);
  ctx.stroke();
  ctx.fillStyle = "#3a2e24"; ctx.fillRect(22 * sx, 198 * sy, 18 * sx, 12 * sy);
  // actors
  for (const b of sim.raw) {
    if (b.dup) continue;
    const x = b.x * sx, y = b.y * sy, w = b.w * sx, h = b.h * sy;
    if (b.actor === "cat") {
      ctx.fillStyle = "#8a7a68";
      ctx.beginPath();
      ctx.ellipse(x + w / 2, y + h * 0.62, w * 0.42, h * 0.34, 0, 0, Math.PI * 2);
      ctx.fill();
      ctx.beginPath(); ctx.arc(x + w * 0.86, y + h * 0.38, h * 0.24, 0, Math.PI * 2); ctx.fill();
      ctx.beginPath(); // ears
      ctx.moveTo(x + w * 0.78, y + h * 0.24); ctx.lineTo(x + w * 0.82, y + h * 0.05); ctx.lineTo(x + w * 0.88, y + h * 0.22);
      ctx.moveTo(x + w * 0.88, y + h * 0.22); ctx.lineTo(x + w * 0.94, y + h * 0.06); ctx.lineTo(x + w * 0.95, y + h * 0.26);
      ctx.fill();
    } else if (b.actor === "tv") {
      // drawn as part of the TV above
    } else {
      const tone = b.actor === "person2" ? "#7d8ba0" : "#9aa7bd";
      ctx.fillStyle = tone;
      // body
      const bw = w * 0.44;
      ctx.fillRect(x + w / 2 - bw / 2, y + h * 0.26, bw, h * 0.5);
      // head
      ctx.beginPath(); ctx.arc(x + w / 2, y + h * 0.14, h * 0.11, 0, Math.PI * 2); ctx.fill();
      // legs
      const t = performance.now() / 160 + (b.actor === "person2" ? 2 : 0);
      const swing = Math.sin(t) * w * 0.12;
      ctx.fillRect(x + w / 2 - bw / 2 + swing / 2, y + h * 0.74, bw * 0.34, h * 0.26);
      ctx.fillRect(x + w / 2 + bw / 2 - bw * 0.34 - swing / 2, y + h * 0.74, bw * 0.34, h * 0.26);
    }
  }
}

function drawBoxes(ctx, W, H, sim, boxes, { labelDup = false } = {}) {
  const sx = W / sim.g.w, sy = H / sim.g.h;
  for (const b of boxes) {
    const isPerson = b.target === sim.cfg.person_target;
    const px = b.x * sx, py = b.y * sy, pw = b.w * sx, ph = b.h * sy;
    ctx.strokeStyle = b.dup ? "rgba(255,212,79,0.35)" : isPerson ? "#ffd44f" : "#7cdcff";
    ctx.lineWidth = b.dup ? 1 : 2;
    ctx.setLineDash(b.dup ? [4, 4] : []);
    ctx.strokeRect(px, py, pw, ph);
    ctx.setLineDash([]);
    const label = (isPerson ? "person " : "class " + b.target + " ") + b.score;
    ctx.font = "600 11px ui-monospace, Menlo, monospace";
    const tw = ctx.measureText(label).width + 8;
    ctx.fillStyle = b.dup ? "rgba(20,20,20,0.55)" : "rgba(20,20,20,0.8)";
    ctx.fillRect(px, py - 16, tw, 15);
    ctx.fillStyle = b.dup ? "rgba(255,212,79,0.5)" : isPerson ? "#ffd44f" : "#7cdcff";
    ctx.fillText(label + (labelDup && b.dup ? " dup" : ""), px + 4, py - 4);
  }
}

// ── §the two ports ────────────────────────────────────────────────────────
export function buildPorts(data) {
  const wrap = el("div", "vis-ports");
  const diagram = el("div", "vis-ports-diagram");

  // the stacked assembly, as an honest schematic
  const mod = el("div", "vis-mod");
  mod.append(el("div", "vis-mod-label", "Grove Vision AI V2 · Himax HX6538"));
  const cam = el("div", "vis-mod-cam", "CSI → OV5647 camera");
  const grove = el("div", "vis-mod-grove", "Grove (I2C · 0x62)");
  const xiao = el("div", "vis-xiao");
  xiao.append(el("div", "vis-xiao-label", "XIAO ESP32 · runs canary-vision"));
  const portM = el("button", "vis-port vis-port-module");
  portM.append(el("span", "vis-port-name", "USB-C"), el("span", "vis-port-sub", "module"));
  const portX = el("button", "vis-port vis-port-xiao");
  portX.append(el("span", "vis-port-name", "USB-C"), el("span", "vis-port-sub", "XIAO"));
  mod.append(cam, grove, xiao, portM);
  xiao.append(portX);
  diagram.append(mod);

  const side = el("div", "vis-ports-side");
  side.append(el("p", "muted",
    "A fully assembled Canary Vision has two USB-C ports going to two different " +
    "computers — this trips everyone once. Pick a job, then plug the right port:"));
  const tasks = el("div", "vis-ports-tasks");
  const verdict = el("p", "muted fineprint vis-ports-verdict",
    "…or click a port first to see what lives behind it.");
  let picked = null;

  const flash = (elm) => { elm.classList.remove("pulse"); void elm.offsetWidth; elm.classList.add("pulse"); };

  for (const row of data.ports.table) {
    const isModule = /module/i.test(row.port);
    const b = el("button", "vis-task");
    b.append(el("strong", null, row.task), el("span", "muted", "→ which port?"));
    b.dataset.want = isModule ? "module" : "xiao";
    b.addEventListener("click", () => {
      for (const c of tasks.children) c.classList.remove("on");
      b.classList.add("on");
      picked = b;
      portM.classList.remove("good", "bad");
      portX.classList.remove("good", "bad");
      verdict.textContent = "Now click the port you'd plug into.";
    });
    tasks.append(b);
  }

  const answer = (which, portBtn) => {
    if (!picked) {
      verdict.textContent = which === "module"
        ? "The module's own port — it talks to the Himax HX6538: model loads, SenseCraft preview, module firmware. It cannot see the ESP32."
        : "The XIAO's port — it talks to the ESP32: flashing canary-vision, serial logs. It cannot reach the Himax flash.";
      flash(portBtn);
      return;
    }
    const want = picked.dataset.want;
    const right = want === which;
    portBtn.classList.add(right ? "good" : "bad");
    verdict.textContent = right
      ? "✓ Right port. " + (which === "module"
          ? "This one goes to the Himax chip — the model lives in the module's own 16 MB flash."
          : "This one goes to the ESP32 — firmware and serial logs live here.")
      : "✗ That port physically can't do this job — " + (which === "module"
          ? "the module port cannot see the ESP32."
          : "the XIAO port cannot reach the Himax flash.") + " Try the other one.";
  };
  portM.addEventListener("click", () => answer("module", portM));
  portX.addEventListener("click", () => answer("xiao", portX));

  side.append(tasks, verdict);
  const rules = el("div", "wap-facts");
  for (const [k, v] of [
    ["Rule", data.ports.rule],
    ["Power", data.ports.power],
    ["Stacking", "both USB-C ports must face the same direction — backwards seating feeds power into GPIO and can kill either board"],
  ]) {
    const row = el("div", "wap-fact");
    row.append(el("span", "wap-fact-k", k), el("span", "wap-fact-v", v));
    rules.append(row);
  }
  wrap.append(diagram, side);
  const outer = el("div");
  outer.append(wrap, rules);
  return outer;
}

// ── §the model load (SenseCraft, staged) ──────────────────────────────────
export function buildModelLoad(data, bus, sim) {
  const d = data.model_load;
  const outer0 = el("div");
  if (d.securacv_flasher) {
    const ribbon = el("p", "ondevice wap-note");
    const a = el("a", null, "open the module flasher →");
    a.href = "flash.html";
    ribbon.append(el("strong", null, "Skip the vendor site: "),
      document.createTextNode(d.securacv_flasher + " "), a);
    outer0.append(ribbon);
  }
  const wrap = el("div", "vis-load");

  // left: the staged workspace
  const ws = el("div", "vis-ws");
  const bar = el("div", "hub-term-bar");
  const dots = el("span", "hub-term-dots");
  dots.append(el("i"), el("i"), el("i"));
  bar.append(dots, el("span", "hub-term-title", "SenseCraft AI · Vision Workspace"),
    el("span", "hub-term-sim", "staged — the real one is Seeed's site"));
  const body = el("div", "vis-ws-body");
  ws.append(bar, body);

  // right: the steps rail
  const rail = el("div", "vis-load-rail");
  rail.append(el("h3", "wap-col-h", "One-time · on the MODULE's port"));
  const steps = [
    ["Open " + d.url.replace("https://", ""), d.browser],
    ["Plug the module's USB-C", d.port],
    ["Connect → pick the serial port", "shows as “USB Single Serial” (CH343). " + d.driver.note],
    ["Select Model → " + d.model, d.duration],
    ["Sanity-check the live preview", "stand back, wave — a box with a score should follow you"],
    ["Check the class list", d.class_check],
    ["Disconnect", d.persistence],
  ];
  const ol = el("ol", "vis-steps");
  const items = steps.map(([t, sub]) => {
    const li = el("li");
    li.append(el("strong", null, t), el("span", "muted", sub));
    ol.append(li);
    return li;
  });
  rail.append(ol);

  // the staged workspace state machine
  let stage = 0;
  const setStage = (n) => {
    stage = n;
    items.forEach((li, i) => li.classList.toggle("on", i <= n - 1));
    render();
  };

  function render() {
    body.innerHTML = "";
    if (stage === 0) {
      const c = el("div", "vis-ws-center");
      const btn = el("button", "primary", "Connect");
      c.append(el("p", "muted", "No device connected."), btn);
      btn.addEventListener("click", () => setStage(1));
      body.append(c);
    } else if (stage === 1) {
      const c = el("div", "vis-ws-center");
      c.append(el("p", null, "Select a serial port"));
      const port = el("button", "vis-ws-port", "USB Single Serial (CH343)");
      const none = el("p", "muted fineprint", "nothing listed? wrong port (XIAO's), a charge-only cable, or the CH343 driver is missing");
      port.addEventListener("click", () => setStage(2));
      c.append(port, none);
      body.append(c);
    } else if (stage === 2) {
      const c = el("div", "vis-ws-center");
      c.append(el("p", null, "Select Model"));
      const grid = el("div", "vis-ws-models");
      const person = el("button", "vis-ws-model on");
      person.append(el("strong", null, "Person Detection"),
        el("span", "muted", "the one canary-vision expects — person as class " + data.detect.person_target));
      person.addEventListener("click", () => setStage(3));
      grid.append(person);
      for (const other of ["Face detection", "Gesture detection", "Pet detection"]) {
        const b = el("button", "vis-ws-model dim");
        b.append(el("strong", null, other), el("span", "muted", "exists — not this build's brain"));
        b.addEventListener("click", () => {
          b.classList.add("shake");
          setTimeout(() => b.classList.remove("shake"), 400);
        });
        grid.append(b);
      }
      c.append(grid, el("p", "muted fineprint",
        "no choice anxiety: the walkthrough names exactly one model, and the firmware's " +
        "class-index setting absorbs any future swap without a rebuild"));
      body.append(c);
    } else if (stage === 3) {
      const c = el("div", "vis-ws-center");
      const p = el("p", null, "Uploading Person Detection…");
      const barOuter = el("div", "vis-progress");
      const fill = el("div", "vis-progress-fill");
      barOuter.append(fill);
      c.append(p, barOuter, el("p", "muted fineprint",
        "1–2 minutes on the real thing (staged here) — keep the tab foregrounded; backgrounding can abort the transfer"));
      body.append(c);
      let k = 0;
      const t = setInterval(() => {
        if (!alive(fill)) { clearInterval(t); return; }
        k = Math.min(100, k + 2 + Math.random() * 3);
        fill.style.width = k + "%";
        if (k >= 100) { clearInterval(t); setStage(4); }
      }, 90);
    } else {
      // the live preview pane — the staged scene + real detection math
      const head = el("div", "vis-ws-preview-head");
      head.append(el("strong", null, "Preview"),
        el("span", "hub-term-sim", "a staged scene — not a camera feed; the real pane shows your room"));
      const stageEl = el("div", "vis-preview-stage");
      const cv = el("canvas", "vis-preview-cv");
      stageEl.append(cv);
      const controls = el("div", "vis-preview-controls");
      const mk = (label, key, val) => {
        const box = el("label", "vis-slider");
        const name = el("span", null, label);
        const out = el("output", null, String(val));
        const input = el("input");
        input.type = "range"; input.min = "0"; input.max = "100"; input.value = String(val);
        input.addEventListener("input", () => {
          sim.preview[key] = +input.value;
          out.value = input.value;
        });
        box.append(name, input, out);
        return box;
      };
      controls.append(
        mk("Confidence", "confidence", sim.preview.confidence),
        mk("IoU", "iou", sim.preview.iou),
      );
      const cast = el("div", "vis-preview-cast");
      for (const [id, label] of [["walk", "🚶 someone walks by"], ["two", "🧑‍🤝‍🧑 two people"], ["cat", "🐈 the cat"], ["tv", "📺 TV on"]]) {
        const b = el("button", "ghost small", label);
        b.addEventListener("click", () => sim.run(id));
        cast.append(b);
      }
      const note = el("p", "muted fineprint",
        "Both sliders are the workspace's — on the wire they're AT+TSCORE / AT+TIOU. " +
        "Confidence hides low-scoring boxes, IoU merges duplicates (watch the dashed twin " +
        "vanish as you raise it). The firmware applies its own threshold later — score ≥ " +
        data.detect.score_min + " by default, tunable from HA.");
      const done = el("button", "primary small", "Looks right — disconnect");
      done.addEventListener("click", () => {
        bus.emit("model", {});
        setStage(0); stage = 5;
        body.innerHTML = "";
        const c = el("div", "vis-ws-center");
        c.append(el("p", null, "✓ Model loaded — it now runs autonomously on every boot."),
          el("p", "muted fineprint", d.persistence),
          el("p", "muted fineprint", d.pauses_i2c));
        body.append(c);
        items.forEach((li) => li.classList.add("on"));
      });
      body.append(head, stageEl, controls, cast, note, done);

      // paint loop for the preview pane
      const paint = () => {
        if (!alive(cv)) return;
        const W = cv.clientWidth || 460, H = Math.round(W * (sim.g.h / sim.g.w) * 0.75);
        const ctx = scaleCanvas(cv, W, H);
        drawScene(ctx, W, H, sim);
        // stage coords → drawBoxes expects top-left boxes; sim raw boxes are
        // top-left already (actor rects), so pass through with preview filters
        const conf = sim.preview.confidence;
        const shown = nms(sim.raw.filter((b) => b.score >= conf), sim.preview.iou / 100);
        drawBoxes(ctx, W, H, sim, shown, { labelDup: true });
        requestAnimationFrame(paint);
      };
      requestAnimationFrame(paint);
      // make sure something is on stage the first time
      if (!sim.actors.length) sim.run("walk");
    }
  }
  render();

  wrap.append(ws, rail);

  const facts = el("div", "wap-facts");
  for (const [k, v] of [
    ["Where", d.url + " · Chrome/Edge (WebSerial)"],
    ["Sliders", d.sliders.map((s) => s.name + " — " + s.what).join("  ·  ")],
    ["On the wire", d.wire ? d.wire.how + " " + d.wire.sliders : ""],
    ["The model", d.model_facts
      ? d.model_facts.arch + " · " + d.model_facts.accuracy + " · " + d.model_facts.speed +
        " · " + d.model_facts.license
      : ""],
    ["Privacy", d.preview_note],
    ["Heads-up", d.pauses_i2c],
  ].filter(([, v]) => v)) {
    const row = el("div", "wap-fact");
    row.append(el("span", "wap-fact-k", k), el("span", "wap-fact-v", v));
    facts.append(row);
  }
  outer0.append(wrap, facts);
  return outer0;
}

// ── §the serial console (host boot) ───────────────────────────────────────
export function buildSerial(data, bus) {
  const wrap = el("div", "wap-term");
  const bar = el("div", "hub-term-bar");
  const dots = el("span", "hub-term-dots");
  dots.append(el("i"), el("i"), el("i"));
  bar.append(dots, el("span", "hub-term-title", data.serial.port_hint),
    el("span", "hub-term-sim", "real firmware strings · staged boot"));
  const scroll = el("div", "wap-term-scroll");
  const controls = el("div", "hub-term-controls");
  const btnPwr = el("button", "primary small", "⏻ power on");
  const btnSkip = el("button", "ghost small", "skip to ready");
  const hint = el("span", "muted fineprint",
    "the XIAO's port on a laptop — `pio device monitor` is this window");
  controls.append(btnPwr, btnSkip, hint);
  wrap.append(bar, scroll, controls);

  const lines = bootLines(data.serial);
  let state = "idle"; // idle → booting → booted

  const put = (cls, text) => {
    const l = el("div", "wap-line " + cls);
    l.textContent = text;
    scroll.append(l);
    scroll.scrollTop = scroll.scrollHeight;
  };
  put("wap-faint", "— console idle · press power —");

  const finish = () => {
    state = "booted";
    btnSkip.disabled = true;
    put("wap-ok", "— setup() done; loop() watching —");
    bus.emit("online", {});
    bus.emit("mqtt", {});
  };
  async function boot(instant) {
    if (state === "booted" || (state === "booting" && !instant)) return;
    const first = state === "idle";
    btnPwr.disabled = true;
    if (first) bus.emit("power", {});
    if (instant) {
      state = "booted"; // stops a streaming boot mid-flight
      scroll.innerHTML = "";
      for (const ln of lines) put(ln.cls, ln.text);
      finish();
      return;
    }
    state = "booting";
    scroll.innerHTML = "";
    for (const ln of lines) {
      if (!alive(scroll) || state !== "booting") return; // skip took over
      put(ln.cls, ln.text);
      await new Promise((r) => setTimeout(r, ln.text.trim() ? 90 + Math.random() * 120 : 30));
    }
    if (state === "booting") finish();
  }
  btnPwr.addEventListener("click", () => boot(false));
  btnSkip.addEventListener("click", () => boot(true));

  bus.on("sim-event", ({ name, snap }) => {
    if (state !== "booted") return;
    const c = snap.sample.bbox ? snap.sample.bbox.score : 0;
    const why = snap.reason ? " reason=" + snap.reason : "";
    put("wap-prov", `[EVT] ${name}${why}  conf=${c} voxel=${snap.sample.voxel ? snap.sample.voxel.r + "," + snap.sample.voxel.c : "-"}`);
  });
  return wrap;
}

// ── §the MQTT explorer ────────────────────────────────────────────────────
export function buildMqtt(data, bus) {
  const id = data.device.id_example;
  const wrap = el("div", "wap-mqtt");
  const bar = el("div", "hub-term-bar");
  const dots = el("span", "hub-term-dots");
  dots.append(el("i"), el("i"), el("i"));
  bar.append(dots, el("span", "hub-term-title", "your broker · securacv/" + id + "/#"),
    el("span", "hub-term-sim", "topics from topics.h · payloads from main.cpp"));
  const scroll = el("div", "wap-term-scroll vis-mqtt-scroll");
  wrap.append(bar, scroll);

  const store = {};
  const rows = new Map();

  const row = (topic, payload, retain) => {
    let r = rows.get(topic);
    if (!r) {
      r = el("div", "vis-mqtt-row");
      const t = el("div", "vis-mqtt-topic");
      t.append(el("span", null, topic), retain ? el("span", "vis-ret", "retained") : el("span", "vis-ret vis-ret-no", "event"));
      const p = el("code", "vis-mqtt-payload");
      r.append(t, p);
      rows.set(topic, r);
      scroll.append(r);
    }
    r.querySelector(".vis-mqtt-payload").textContent = payload;
    r.classList.remove("tick"); void r.offsetWidth; r.classList.add("tick");
    scroll.scrollTop = 0;
  };

  const idle = el("p", "muted fineprint vis-mqtt-idle",
    "quiet — boot the device in the console (or just poke the sandbox) and the retained surfaces fill in");
  scroll.append(idle);

  bus.on("online", () => {
    idle.remove();
    const base = "securacv/" + id;
    row(base + "/status", '"online"', true);
    row(base + "/cfg/state", JSON.stringify(data.mqtt.cfg_state_example), true);
    row(base + "/state", '{"presence":"empty","occupants":"none","confidence":0}', true);
    row(base + "/health", '{"fw":"' + data.device.fw_version + '","public_key":"ed25519:…"}', true);
    row(base + "/chain", '{"length":1,"head":"…"}', true);
    row(base + "/aim/state", '"OFF"', true);
  });
  bus.on("mqtt", () => {
    const n = data.mqtt.discovery.entities.length;
    row(data.mqtt.ha_prefix + "/…/" + id + "/…/config", n + " retained discovery payloads → the device simply appears in HA", true);
  });
  bus.on("sim-event", ({ name, snap }) => {
    const base = "securacv/" + id;
    const bb = snap.sample.bbox || { x: 0, y: 0, w: 0, h: 0, score: 0 };
    const v = snap.sample.voxel || { r: -1, c: -1 };
    row(base + "/events", JSON.stringify({
      event: name, ...(snap.reason ? { reason: snap.reason } : {}),
      signed: true, confidence: bb.score,
      voxel: { r: v.r, c: v.c }, bbox: { x: bb.x, y: bb.y, w: bb.w, h: bb.h },
    }), false);
    row(base + "/state", JSON.stringify({
      presence: snap.fsm.presence ? (snap.fsm.dwelling ? "dwelling" : "present") : "empty",
      confidence: bb.score,
    }), true);
  });
  bus.on("cfg", ({ cfg }) => {
    row("securacv/" + id + "/cfg/state", JSON.stringify({
      target: cfg.person_target, score: cfg.score_min,
      lost_ms: cfg.lost_timeout_ms, dwell_ms: cfg.dwell_start_ms,
    }), true);
  });
  bus.on("aim-frame", ({ payload }) => {
    row("securacv/" + id + "/aim", JSON.stringify(payload), false);
  });
  bus.on("aim-state", ({ on }) => {
    row("securacv/" + id + "/aim/state", on ? '"ON"' : '"OFF"', true);
  });

  // the topic reference table
  const table = el("div", "vis-topics");
  for (const t of data.mqtt.topics) {
    const r = el("div", "vis-topic");
    r.append(el("code", null, "securacv/<id>/" + t.suffix),
      el("span", "vis-ret" + (t.retain ? "" : " vis-ret-no"), t.retain ? "retained" : "event"),
      el("span", "muted", t.desc));
    table.append(r);
  }
  const det = el("details", "fix vis-topics-det");
  det.append(el("summary", null, "every topic it speaks (" + data.mqtt.topics.length + ", from topics.h)"), table);

  // the discovery entity set
  const ents = el("div", "vis-entities");
  for (const e of data.mqtt.discovery.entities) {
    const c = el("div", "vis-entity");
    c.append(el("span", "vis-entity-kind", e.component), el("strong", null, e.name),
      el("span", "muted", e.desc));
    ents.append(c);
  }
  const det2 = el("details", "fix vis-topics-det");
  det2.append(el("summary", null,
    data.mqtt.discovery.entities.length + " Home Assistant entities, announced by the device itself"), ents);

  const outer = el("div");
  outer.append(wrap, det, det2,
    el("p", "muted fineprint", data.mqtt.discovery.note));
  return outer;
}

// ── §the aim card (boxes, never pixels) ───────────────────────────────────
export function buildAim(data, bus, sim) {
  const a = data.aim;
  const wrap = el("div", "vis-aim");

  const card = el("div", "vis-aim-card");
  const head = el("div", "vis-aim-head");
  const sw = el("button", "vis-aim-switch", "Start aiming");
  const state = el("span", "muted fineprint", "aim assist off · off by default on every boot");
  head.append(sw, state);
  const cv = el("canvas", "vis-aim-cv");
  const ticker = el("code", "vis-aim-ticker", "— no stream —");
  card.append(head, cv, ticker);

  const side = el("div", "vis-aim-side");
  side.append(el("h3", "wap-col-h", "Aim it after mounting — from the couch"));
  const ol = el("ol", "vis-steps");
  for (const s of a.steps) {
    const li = el("li"); li.append(el("span", "muted", s)); ol.append(li);
  }
  side.append(ol, el("p", "ondevice wap-note", a.why_not_sensecraft));

  let on = false, autoOffAt = 0, lastPub = 0;
  const AUTOOFF_SIM = 60 * 1000; // staged ×10: 1 min here = the firmware's 10

  const setOn = (v, why) => {
    on = v;
    sw.textContent = on ? "Stop aiming" : "Start aiming";
    sw.classList.toggle("on", on);
    state.textContent = on
      ? "streaming boxes at ~" + Math.round(1000 / a.publish_ms) + " Hz · auto-off armed (staged ×10 — " +
        (a.auto_off_ms / 60000) + " min on the device)"
      : why === "auto"
        ? "auto-off: a forgotten switch can't stream box telemetry forever"
        : "aim assist off · off by default on every boot";
    if (on) autoOffAt = performance.now() + AUTOOFF_SIM;
    bus.emit("aim-state", { on });
    if (!on) ticker.textContent = "— no stream —";
  };
  sw.addEventListener("click", () => setOn(!on, "user"));

  sim.on("frame", (snap) => {
    if (!alive(cv)) return;
    // paint the card whether or not the stream is on — off shows the empty grid
    const W = cv.clientWidth || 420, H = Math.round(W * (sim.g.h / sim.g.w) * 0.75);
    const ctx = scaleCanvas(cv, W, H);
    ctx.fillStyle = "#08090b"; ctx.fillRect(0, 0, W, H);
    // voxel grid
    const { cols, rows } = sim.g;
    ctx.strokeStyle = "#1d2027"; ctx.lineWidth = 1;
    for (let i = 1; i < cols; i++) { ctx.beginPath(); ctx.moveTo((W * i) / cols, 0); ctx.lineTo((W * i) / cols, H); ctx.stroke(); }
    for (let i = 1; i < rows; i++) { ctx.beginPath(); ctx.moveTo(0, (H * i) / rows); ctx.lineTo(W, (H * i) / rows); ctx.stroke(); }
    if (!on) {
      ctx.fillStyle = "#3a3d45";
      ctx.font = "600 12px ui-monospace, Menlo, monospace";
      ctx.fillText("aim assist off — flip the switch", 14, H / 2);
      return;
    }
    const now = performance.now();
    if (now > autoOffAt) { setOn(false, "auto"); return; }
    const period = snap.sample.person_now ? a.publish_ms : a.idle_publish_ms;
    if (now - lastPub >= period) {
      lastPub = now;
      const payload = aimPayload(snap.sample, sim.g);
      ticker.textContent = "securacv/<id>/aim  " + JSON.stringify(payload);
      bus.emit("aim-frame", { payload });
    }
    if (snap.sample.person_now) {
      const bb = snap.sample.bbox, v = snap.sample.voxel;
      const sx = W / sim.g.w, sy = H / sim.g.h;
      // the claimed voxel cell
      ctx.fillStyle = "rgba(255,212,79,0.10)";
      ctx.fillRect((W * v.c) / cols, (H * v.r) / rows, W / cols, H / rows);
      ctx.strokeStyle = "rgba(255,212,79,0.5)";
      ctx.strokeRect((W * v.c) / cols, (H * v.r) / rows, W / cols, H / rows);
      // the wireframe box — coordinates only, exactly what crossed the wire
      ctx.strokeStyle = "#ffd44f"; ctx.lineWidth = 2;
      ctx.strokeRect(bb.x * sx, bb.y * sy, bb.w * sx, bb.h * sy);
      ctx.font = "600 12px ui-monospace, Menlo, monospace";
      ctx.fillStyle = "#ffd44f";
      ctx.fillText("score " + bb.score + " · voxel " + v.r + "," + v.c, bb.x * sx + 4, bb.y * sy - 6);
    } else {
      ctx.fillStyle = "#3a3d45";
      ctx.font = "600 12px ui-monospace, Menlo, monospace";
      ctx.fillText("present:false — the card clears its box", 14, H / 2);
    }
  });

  wrap.append(card, side);
  return wrap;
}

// ── §the sandbox: scenarios + live tuning + the witness verdict ───────────
export function buildTunePlay(data, bus, sim) {
  const wrap = el("div", "vis-play");

  // scenario cast
  const cast = el("div", "wap-sandbox vis-cast");
  for (const sc of data.sandbox) {
    const b = el("button", "card wap-sand-card");
    b.append(el("strong", null, sc.icon + " " + sc.label), el("span", "muted", sc.blurb));
    b.addEventListener("click", () => {
      bus.emit("wake", {});
      sim.run(sc.id);
    });
    cast.append(b);
  }

  // the witness verdict strip
  const verdict = el("div", "vis-verdict");
  const pillPresence = el("span", "vis-pill", "Quiet");
  const pillConf = el("span", "vis-pill vis-pill-dim", "conf —");
  const pillVoxel = el("span", "vis-pill vis-pill-dim", "voxel —");
  const pillEvent = el("span", "vis-pill vis-pill-dim", "last event: —");
  verdict.append(pillPresence, pillConf, pillVoxel, pillEvent);

  // live tuning — the four NVS-backed numbers
  const tune = el("div", "vis-tune");
  const mkNum = (label, key, val, min, max, step, unit, fmt = (v) => v) => {
    const box = el("label", "vis-slider vis-tune-row");
    const name = el("span", null, label);
    const out = el("output", null, fmt(val) + (unit || ""));
    const input = el("input");
    input.type = "range"; input.min = String(min); input.max = String(max);
    input.step = String(step); input.value = String(val);
    input.addEventListener("input", () => {
      sim.cfg[key] = +input.value;
      out.value = fmt(+input.value) + (unit || "");
      bus.emit("cfg", { cfg: sim.cfg, key });
    });
    box.append(name, input, out);
    box._set = (v) => { input.value = String(v); out.value = fmt(v) + (unit || ""); };
    return box;
  };
  const b = data.detect.bounds;
  const rows = {
    score_min: mkNum("Score threshold", "score_min", sim.cfg.score_min, b.score[0], b.score[1], 1, " %"),
    lost_timeout_ms: mkNum("Lost timeout", "lost_timeout_ms", sim.cfg.lost_timeout_ms, b.lost_ms[0], b.lost_ms[1], 250, " ms"),
    dwell_start_ms: mkNum("Dwell start", "dwell_start_ms", sim.cfg.dwell_start_ms, b.dwell_ms[0], b.dwell_ms[1], 500, " s", (v) => (v / 1000).toFixed(1)),
  };
  tune.append(el("h3", "wap-col-h", "The four live numbers (NVS-backed — no rebuild, ever)"),
    rows.score_min, rows.lost_timeout_ms, rows.dwell_start_ms);
  const tgt = el("p", "muted fineprint",
    "the fourth — person class index (0–255) — only moves when you load a non-default model; " +
    "the sandbox keeps it at " + data.detect.person_target);
  tune.append(tgt);
  const why = el("div", "vis-tune-why");
  for (const t of data.tuning) {
    const r = el("div", "vis-tune-whyrow");
    r.append(el("strong", null, t.setting), el("code", "fineprint", t.key + " · " + t.range),
      el("span", "muted", t.why));
    why.append(r);
  }
  tune.append(why);

  bus.on("preset", ({ preset }) => {
    sim.cfg.score_min = preset.score;
    sim.cfg.lost_timeout_ms = preset.lost_ms;
    sim.cfg.dwell_start_ms = preset.dwell_ms;
    rows.score_min._set(preset.score);
    rows.lost_timeout_ms._set(preset.lost_ms);
    rows.dwell_start_ms._set(preset.dwell_ms);
    bus.emit("cfg", { cfg: sim.cfg, key: "preset" });
  });

  // event log
  const log = el("div", "vis-evlog");
  const logHead = el("div", "vis-evlog-head", "witness events — what actually publishes");
  const logBody = el("div", "vis-evlog-body");
  logBody.append(el("p", "muted fineprint", "nothing yet — send someone through"));
  log.append(logHead, logBody);

  sim.on("frame", (snap) => {
    const p = snap.fsm.presence, d = snap.fsm.dwelling;
    pillPresence.textContent = d ? "Dwelling" : p ? "Presence" : "Quiet";
    pillPresence.className = "vis-pill" + (d ? " vis-pill-dwell" : p ? " vis-pill-on" : "");
    const bb2 = snap.sample.bbox;
    pillConf.textContent = "conf " + (bb2 ? bb2.score : "—");
    pillVoxel.textContent = "voxel " + (bb2 ? snap.sample.voxel.r + "," + snap.sample.voxel.c : "—");
  });
  sim.on("event", (name, snap) => {
    bus.emit("sim-event", { name, snap });
    pillEvent.textContent = "last event: " + name;
    pillEvent.className = "vis-pill vis-pill-ev";
    if (logBody.firstChild && logBody.firstChild.tagName === "P") logBody.innerHTML = "";
    const r = el("div", "vis-evlog-row");
    const bb3 = snap.sample.bbox;
    r.append(el("code", null, name + (snap.reason ? " (" + snap.reason + ")" : "")),
      el("span", "muted", bb3 ? "conf " + bb3.score + " · voxel " + snap.sample.voxel.r + "," + snap.sample.voxel.c : "frame empty"),
      el("span", "fineprint", "signed · seq +1"));
    logBody.prepend(r);
    while (logBody.children.length > 8) logBody.lastChild.remove();
  });

  const left = el("div", "vis-play-left");
  left.append(cast, verdict, log);
  wrap.append(left, tune);
  return wrap;
}

// ── §placement + use cases ────────────────────────────────────────────────
export function buildPlacement(data, bus) {
  const p = data.placement;
  const wrap = el("div");

  const rules = el("div", "wap-facts");
  for (const r of p.rules) {
    const row = el("div", "wap-fact");
    row.append(el("span", "wap-fact-k", r.k), el("span", "wap-fact-v", r.v));
    rules.append(row);
  }
  wrap.append(rules, el("p", "muted fineprint", p.fov_note));

  const grid = el("div", "vis-usecases");
  const verdict = el("p", "muted fineprint vis-uc-verdict",
    "pick where it'll live — the preset lands on the sandbox sliders above, exactly like HA would write them");
  for (const uc of p.use_cases) {
    const c = el("button", "card vis-uc");
    c.append(el("strong", null, uc.icon + " " + uc.title), el("span", "muted", uc.blurb),
      el("code", "fineprint", "score ≥ " + uc.preset.score + " · lost " + uc.preset.lost_ms +
        " ms · dwell " + uc.preset.dwell_ms / 1000 + " s"));
    c.addEventListener("click", () => {
      for (const x of grid.children) x.classList.remove("on");
      c.classList.add("on");
      bus.emit("wake", {});
      bus.emit("preset", { preset: uc.preset, label: uc.title });
      verdict.textContent = "▶ " + uc.title + " — applied to the sandbox. On a real device these three " +
        "MQTT writes land on securacv/<id>/cfg/…/set and persist in NVS.";
    });
    grid.append(c);
  }
  wrap.append(grid, verdict, el("p", "ondevice wap-note", p.no_outdoor));
  return wrap;
}

// ── §flash the host ───────────────────────────────────────────────────────
export function buildFlashHost(data) {
  const f = data.flash;
  const wrap = el("div");

  const facts = el("div", "wap-facts");
  for (const [k, v] of [
    ["Port", f.port_rule],
    ["Secrets", f.secrets],
    ["Monitor", f.monitor],
  ]) {
    const row = el("div", "wap-fact");
    row.append(el("span", "wap-fact-k", k), el("span", "wap-fact-v", v));
    facts.append(row);
  }
  wrap.append(facts);

  const term = el("div", "wap-term vis-flash-term");
  const bar = el("div", "hub-term-bar");
  const dots = el("span", "hub-term-dots");
  dots.append(el("i"), el("i"), el("i"));
  bar.append(dots, el("span", "hub-term-title", "firmware/projects/canary-vision"),
    el("span", "hub-term-sim", "commands from the README — copy them for real"));
  const body = el("div", "wap-term-scroll vis-flash-cmds");
  for (const cmd of f.quickstart) {
    const r = el("div", "vis-cmd");
    const code = el("code", null, "$ " + cmd);
    const copy = el("button", "hub-copy", "copy");
    copy.addEventListener("click", async () => {
      try { await navigator.clipboard.writeText(cmd); copy.textContent = "✓"; copy.classList.add("copied"); }
      catch { copy.textContent = "ctrl-c"; }
      setTimeout(() => { copy.textContent = "copy"; copy.classList.remove("copied"); }, 1400);
    });
    r.append(code, copy);
    body.append(r);
  }
  term.append(bar, body);
  wrap.append(term);

  // host table
  const hosts = el("div", "vis-topics");
  for (const h of data.device.hosts) {
    const r = el("div", "vis-topic");
    r.append(el("code", null, h.env), el("span", "muted", h.board + " — " + h.hookup + " · I2C " + h.i2c));
    hosts.append(r);
  }
  const det = el("details", "fix vis-topics-det");
  det.append(el("summary", null, "the three host builds (pick your env)"), hosts);
  wrap.append(det);

  // the browser flasher + the roadmap
  const two = el("div", "wap-two");
  const l = el("div", "wap-two-col");
  l.append(el("h3", "wap-col-h", "No terminal? The Lab flashes it"));
  const a = el("a", "hub-link");
  a.href = "flash.html";
  a.append(el("strong", null, "Flash a Canary — in your browser"),
    el("span", "muted", f.browser.note),
    el("code", "fineprint", f.browser.products.map((p2) => p2.name).join(" · ")));
  l.append(a);
  const r2 = el("div", "wap-two-col");
  r2.append(el("h3", "wap-col-h", "And the module's model?"));
  const road = el("div", "vis-roadmap");
  road.append(el("p", null, data.roadmap.today),
    el("p", "muted", data.roadmap.next),
    el("span", "vis-ret", "status: " + data.roadmap.status));
  r2.append(road);
  two.append(l, r2);
  wrap.append(two);
  return wrap;
}

// ── §troubleshooting ──────────────────────────────────────────────────────
export function buildTrouble(data) {
  const wrap = el("div", "wap-trouble");
  for (const t of data.troubleshooting) {
    const d = el("details", "fix");
    d.append(el("summary", null, t.symptom));
    d.append(el("p", "fix-step", t.fix));
    wrap.append(d);
  }
  const rec = el("details", "fix");
  rec.append(el("summary", null, "Deeper recovery — bricked module, factory firmware"));
  const ul = el("ul", "wap-btn-gestures");
  for (const s of data.recovery.boot_reset) ul.append(el("li", null, s));
  ul.append(el("li", null, data.recovery.i2c_rescue));
  ul.append(el("li", null, data.recovery.factory));
  rec.append(ul);
  wrap.append(rec);
  return wrap;
}

export { buildBoardLab };
