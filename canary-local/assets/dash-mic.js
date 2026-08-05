// canary-local/assets/dash-mic.js — "what a listening Canary hears".
//
// Drives the REAL 4.3C mic decision core (assets/mic-sim.js, a 1:1 port of
// firmware mic_logic.h, drift-locked in tests/mic.test.js) two ways:
//
//   • Play a sound  — a scripted RMS envelope (smoke T3, CO T4, doorbell, a
//     door close, speech). No permission, always works. Shows the alarms
//     firing AND the non-alarms correctly NOT firing.
//   • Use my mic    — the visitor's live microphone, reduced to ONE loudness
//     number per 20 ms frame, fed to the same core. The audio never leaves
//     this page; only the scalar crosses, exactly as it does on hardware.
//
// Safeguards on the live path mirror smoke-bench.js by construction: mic asked
// only on an explicit gesture; every frame averaged to one number and the
// buffer dropped; browser audio "cleanup" off; full release on stop/hide; a
// standing "demonstration, not a life-safety device" disclaimer.

import {
  MicSim, SENS, SENS_ORDER, SENS_DEFAULT_INDEX, sensName, dbOverFloor,
  CADENCE, TRANSIENT, MIC_FACTS, eventWireName,
} from "./mic-sim.js";

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
const dbLabel = (pct) => {
  const d = dbOverFloor(pct);
  return (d >= 0 ? "+" : "") + d.toFixed(0) + " dB";
};

const FRAME_MS = 20;            // the firmware's audio frame
const FRAME_SAMPLES = 320;      // 20 ms @ 16 kHz
const TARGET_RATE = 16000;
const PLAY_SPEED = 2.5;         // synthetic playback wall-speed (detection
                                // timing is exact regardless — see feedTo()).
const METER_MAX = 8000;         // RMS shown full-scale on the loudness meter

// One core for the whole page. Frame clock is a monotonic counter × 20 ms, so
// cadence timing is exact whether frames arrive from a script or a live mic.
const sim = new MicSim(SENS_DEFAULT_INDEX);
const state = {
  mode: "play",       // "play" | "live"
  frame: 0,           // monotonic frame counter (drives the sim clock)
  timeline: [],       // recent {loud, band} for the rolling strip
  latest: null,       // last snapshot
  event: null,        // {kind, conf, at} sticky alarm banner
  woke: 0,            // frame index of the last wake pulse
  // playback
  playing: null,      // {frames, idx, startWall, name}
  raf: 0,
  // live
  running: false, ctx: null, stream: null, node: null, source: null, sink: null,
};

const TIMELINE_MAX = 160;

// ── one place feeds the core; everything else reads snapshots ────────────────
function feedTo(rms) {
  const now = state.frame * FRAME_MS;
  const snap = sim.feed(Math.max(0, Math.min(65535, rms | 0)), now);
  state.frame++;
  state.latest = snap;
  state.timeline.push({ loud: snap.loud, over: rms >= snap.on });
  if (state.timeline.length > TIMELINE_MAX) state.timeline.shift();
  if (snap.wake) state.woke = state.frame;
  const ev = snap.detection.event;
  if (ev !== "none") {
    state.event = { kind: ev, conf: snap.detection.confidence, at: state.frame };
  }
  return snap;
}

function resetRun() {
  sim.reset();
  state.frame = 0;
  state.timeline = [];
  state.latest = null;
  state.event = null;
  state.woke = 0;
}

// ── page scaffolding ─────────────────────────────────────────────────────────
function mountVersions() {
  const strip = $("#mic-versions");
  if (!strip) return;
  strip.innerHTML = "";
  const pill = (label, val) => {
    const p = el("span", "pill");
    p.append(el("strong", null, label + " "), document.createTextNode(val));
    return p;
  };
  strip.append(pill("board", "Waveshare 4.3C (the mic dash)"));
  strip.append(pill("runtime", "firmware core, 1:1 JS port"));
  strip.append(pill("engine", "mic_logic.h (drift-locked, offline)"));
}

function render() {
  const mount = $("#mic");
  mount.innerHTML = "";

  mount.append(disclaimer());
  mount.append(reassurance());

  const card = el("section", "mic-card");
  card.id = "mic-card";
  mount.append(card);
  card.append(modeTabs());
  card.append(controls());
  card.append(state.mode === "play" ? playPanel() : livePanel());
  card.append(viz());

  mount.append(factsTable());
  paint();
}

// The standing disclaimer — always visible, never dismissible.
function disclaimer() {
  const d = el("div", "mic-disclaimer");
  d.append(el("strong", null, "This is a demonstration, not an alarm. "));
  d.append(document.createTextNode(
    "It shows how a listening Canary Dash recognizes an alarm's sound. It is " +
    "not a smoke or CO detector and is no substitute for a UL-listed one. In " +
    "an emergency, trust your real alarms and call your local emergency number."));
  return d;
}

// The heart of the page: what it does, and what it never does.
function reassurance() {
  const wrap = el("section", "mic-reassure");
  const cols = el("div", "mic-reassure-cols");

  const does = el("div", "mic-col mic-col-does");
  does.append(el("h3", null, "What it does"));
  const ul1 = el("ul");
  MIC_FACTS.does.forEach((t) => { const li = el("li"); li.append(el("span", "mic-tick", "✓ "), document.createTextNode(t)); ul1.append(li); });
  does.append(ul1);

  const doesnt = el("div", "mic-col mic-col-doesnt");
  doesnt.append(el("h3", null, "What it never does"));
  const ul2 = el("ul");
  MIC_FACTS.doesNot.forEach((t) => { const li = el("li"); li.append(el("span", "mic-bar", "✕ "), document.createTextNode(t)); ul2.append(li); });
  doesnt.append(ul2);

  cols.append(does, doesnt);
  wrap.append(cols);

  const key = el("p", "mic-key-answer");
  key.append(el("span", "mic-key-q", "“Could someone hear me talking through it?” "));
  key.append(document.createTextNode(MIC_FACTS.couldSomeoneHearYou));
  wrap.append(key);
  return wrap;
}

function modeTabs() {
  const tabs = el("div", "mic-tabs");
  const mk = (id, label) => {
    const b = el("button", "mic-tab" + (state.mode === id ? " mic-tab-on" : ""), label);
    b.type = "button";
    b.addEventListener("click", () => {
      if (state.mode === id) return;
      stopEverything();
      state.mode = id;
      resetRun();
      render();
    });
    return b;
  };
  tabs.append(mk("play", "▶ Play a sound"));
  tabs.append(mk("live", "🎤 Use my microphone"));
  return tabs;
}

// The two firmware-real tunables: sensitivity preset + opt-in wake-on-sound.
function controls() {
  const box = el("div", "mic-controls");

  const sens = el("div", "mic-ctl");
  sens.append(el("div", "mic-ctl-label", "Sensitivity (the one environment knob)"));
  const seg = el("div", "mic-seg");
  SENS_ORDER.forEach((name, i) => {
    const b = el("button", "mic-seg-btn" + (sim.sensIndex === i ? " mic-seg-on" : ""));
    b.type = "button";
    b.append(el("strong", null, name[0].toUpperCase() + name.slice(1)));
    b.append(el("span", "mic-seg-db", " · " + dbLabel(SENS[name].on_pct) + " to trip"));
    b.addEventListener("click", () => {
      sim.setSensitivity(i);
      resetRunKeepMode();
      render();
    });
    seg.append(b);
  });
  sens.append(seg);
  box.append(sens);

  const wake = el("div", "mic-ctl");
  const wl = el("label", "mic-toggle");
  const cb = el("input");
  cb.type = "checkbox";
  cb.checked = sim.wakeOnSound;
  cb.addEventListener("change", () => {
    sim.wakeOnSound = cb.checked;
    resetRunKeepMode();
    render();
  });
  wl.append(cb);
  const wt = el("span", "mic-toggle-txt");
  wt.append(el("strong", null, "Wake the screen on a sound"));
  wt.append(document.createTextNode(
    " — opt-in. Lights a dark dash on a loud onset (" + dbLabel(TRANSIENT.RISE_PCT) +
    " over the room, one wake per " + (TRANSIENT.REFRACTORY_MS / 1000) +
    " s). Same loudness number, never speech, nothing recorded."));
  wl.append(wt);
  wake.append(wl);
  box.append(wake);

  return box;
}

function resetRunKeepMode() {
  stopPlayback();
  if (state.mode === "live" && state.running) { resetRun(); }
  else resetRun();
}

// ── PLAY panel: scripted sounds ──────────────────────────────────────────────
const SOUNDS = [
  { id: "smoke", label: "🔥 Smoke alarm (T3)", note: "three ½-second beeps, twice — should raise an Alert" },
  { id: "co",    label: "☠ CO alarm (T4)", note: "four short beeps, twice — should raise an Alert" },
  { id: "bell",  label: "🔔 Doorbell", note: "two long tones — should NOT alarm" },
  { id: "door",  label: "🚪 Door close", note: "one loud onset — wakes the screen only if you opted in; never an alarm" },
  { id: "talk",  label: "🗣 Speech", note: "irregular bursts — should NOT alarm, and is never recognized as words" },
];

function playPanel() {
  const p = el("div", "mic-play");
  const grid = el("div", "mic-sound-grid");
  SOUNDS.forEach((s) => {
    const b = el("button", "mic-sound");
    b.type = "button";
    b.append(el("span", "mic-sound-label", s.label));
    b.append(el("span", "mic-sound-note", s.note));
    b.addEventListener("click", () => playSound(s.id));
    grid.append(b);
  });
  p.append(grid);
  const hint = el("p", "mic-play-hint",
    "Pick a sound. The trace fills in beep-by-beep; the verdict says what the " +
    "core decided — and why it's the same decision the device would make.");
  p.append(hint);
  return p;
}

// Build a per-frame RMS array from segments. Each segment is {ms, rms} with a
// gentle deterministic wobble so the meter looks alive (no Math.random needed).
function framesFromSegments(segs) {
  const out = [];
  let k = 0;
  for (const s of segs) {
    const n = Math.max(1, Math.round(s.ms / FRAME_MS));
    for (let i = 0; i < n; i++) {
      const wob = s.rms > 400 ? 0 : 24 * (0.5 + 0.5 * Math.sin(k * 0.7));
      out.push(Math.round(s.rms + wob));
      k++;
    }
  }
  return out;
}

const ROOM = (ms) => ({ ms, rms: 130 });        // quiet room tone
const BEEP = (ms, rms) => ({ ms, rms });         // a loud tone

function scriptFor(id) {
  const beepR = 3600;
  switch (id) {
    case "smoke": {
      // T3: 3× ½-s beeps, ½-s gaps, then a pause. Two on-grammar cycles fire.
      const group = [];
      for (let b = 0; b < 3; b++) { group.push(BEEP(500, beepR)); if (b < 2) group.push(ROOM(500)); }
      return [ROOM(900), ...group, ROOM(1500), ...group, ROOM(1500), ...group, ROOM(1000)];
    }
    case "co": {
      // T4: 4× 100 ms beeps, 100 ms gaps, then a pause. Two cycles fire.
      const group = [];
      for (let b = 0; b < 4; b++) { group.push(BEEP(100, beepR)); if (b < 3) group.push(ROOM(100)); }
      return [ROOM(900), ...group, ROOM(1500), ...group, ROOM(1500), ...group, ROOM(1000)];
    }
    case "bell":
      // Ding-dong: two long tones — wrong count and duration for either grammar.
      return [ROOM(900), BEEP(700, 2600), ROOM(300), BEEP(700, 2600), ROOM(2000)];
    case "door":
      // A single sharp onset over a quiet room — wakes (if opted in), never alarms.
      return [ROOM(1400), BEEP(120, 7200), ROOM(2200)];
    case "talk":
      // Speech-shaped: irregular bursts, uneven durations — no grammar match.
      // Kept at conversational level (above the cadence trip line, below the
      // wake onset), so it shows the rhythm being rejected without waking.
      return [ROOM(800),
        BEEP(220, 900), ROOM(120), BEEP(180, 980), ROOM(260), BEEP(340, 820),
        ROOM(180), BEEP(150, 960), ROOM(300), BEEP(400, 880), ROOM(1500)];
    default:
      return [ROOM(1000)];
  }
}

function playSound(id) {
  stopPlayback();
  resetRun();
  const frames = framesFromSegments(scriptFor(id));
  state.playing = { frames, idx: 0, startWall: performance.now(), name: id };
  render();               // reflect the reset state immediately
  loop();
}

function stopPlayback() {
  if (state.raf) { cancelAnimationFrame(state.raf); state.raf = 0; }
  state.playing = null;
}

function loop() {
  if (state.playing) {
    const p = state.playing;
    const elapsed = performance.now() - p.startWall;
    const target = Math.min(p.frames.length, Math.floor((elapsed / FRAME_MS) * PLAY_SPEED));
    while (p.idx < target) feedTo(p.frames[p.idx++]);
    paint();
    if (p.idx >= p.frames.length) { state.playing = null; state.raf = 0; markPlayDone(); return; }
  } else if (state.running) {
    paint();
  } else { state.raf = 0; return; }
  state.raf = requestAnimationFrame(loop);
}

function markPlayDone() {
  const s = $("#mic-status");
  if (s && !state.event) s.textContent = "Done — heard the sound, no alarm cadence. That's the correct call.";
}

// ── LIVE panel: the visitor's microphone ─────────────────────────────────────
function livePanel() {
  const p = el("div", "mic-live");
  const problem = supportProblem();
  if (problem) { p.append(el("p", "mic-warn", problem)); return p; }

  if (!state.running) {
    const priv = el("p", "mic-priv");
    priv.append(el("span", "mic-priv-key", "🔒 "));
    priv.append(document.createTextNode(
      "Your microphone audio stays in this page. Each 20 ms is averaged to one " +
      "loudness number, fed to the detector, and thrown away — never recorded, " +
      "saved, or uploaded. Below, watch the ONE number that crosses. Stop " +
      "anytime and the mic is released at once."));
    p.append(priv);
    const btn = el("button", "mic-start", "🎤 Start listening");
    btn.type = "button";
    btn.addEventListener("click", () => startLive(btn));
    p.append(btn);
    const tip = el("p", "mic-play-hint",
      "Try it: hold a smoke alarm's TEST button near your laptop for two full " +
      "cycles, or just close a door (with wake-on-sound on).");
    p.append(tip);
  } else {
    const head = el("div", "mic-live-head");
    head.append(el("span", "mic-listening-dot"));
    head.append(el("span", "mic-live-txt", "Listening — the detector is running on your mic"));
    const stop = el("button", "mic-stop", "◼ Stop & release mic");
    stop.type = "button";
    stop.addEventListener("click", stopLive);
    head.append(stop);
    p.append(head);
  }
  return p;
}

function supportProblem() {
  if (!window.isSecureContext)
    return "Microphone access needs a secure page (https, or localhost). The " +
           "“Play a sound” tab works anywhere.";
  if (!navigator.mediaDevices || !navigator.mediaDevices.getUserMedia)
    return "This browser doesn't offer microphone access to web pages. Try a " +
           "current Chrome, Edge, Firefox or Safari — or use the “Play a sound” tab.";
  return null;
}

async function startLive(btn) {
  btn.disabled = true;
  btn.textContent = "Asking for the microphone…";
  try {
    const stream = await navigator.mediaDevices.getUserMedia({
      audio: { echoCancellation: false, noiseSuppression: false, autoGainControl: false, channelCount: 1 },
    });
    state.stream = stream;
    const Ctx = window.AudioContext || window.webkitAudioContext;
    let ctx;
    try { ctx = new Ctx({ sampleRate: TARGET_RATE }); } catch { ctx = new Ctx(); }
    if (ctx.state === "suspended") { try { await ctx.resume(); } catch { /* gesture-gated */ } }
    state.ctx = ctx;

    resetRun();
    const source = ctx.createMediaStreamSource(stream);
    state.source = source;
    const node = ctx.createScriptProcessor(4096, 1, 1);
    state.node = node;
    const rs = makeResampler(ctx.sampleRate, TARGET_RATE);
    const frame = new Float32Array(FRAME_SAMPLES);
    let fill = 0;

    node.onaudioprocess = (e) => {
      if (!state.running) return;
      const input = e.inputBuffer.getChannelData(0);
      const out = rs(input);
      for (let i = 0; i < out.length; i++) {
        frame[fill++] = out[i];
        if (fill === FRAME_SAMPLES) { feedTo(rmsI16(frame)); fill = 0; }
      }
    };
    source.connect(node);
    const sink = ctx.createGain();       // zero-gain sink so we never echo the room out
    sink.gain.value = 0;
    node.connect(sink);
    sink.connect(ctx.destination);
    state.sink = sink;

    state.running = true;
    render();
    loop();
  } catch (err) {
    state.running = false;
    releaseAudio();
    renderLiveError(err);
  }
}

// One int16-scale RMS from a 16 kHz frame, computed exactly like the firmware's
// read_frame_rms: subtract the frame's DC mean, THEN RMS the deviations — so a
// DC-biased mic can't inflate the scalar past what the device would compute and
// feed the (otherwise drift-locked) core a different number than the hardware.
// The frame is zeroed on the way out — the same `memset` the firmware calls
// "the privacy barrier: nothing outlives this" — so the raw samples never
// linger past the one scalar they reduce to. This IS the barrier, in-browser.
function rmsI16(frameFloats) {
  const n = frameFloats.length;
  let mean = 0;
  for (let i = 0; i < n; i++) {
    let s = frameFloats[i];
    if (s > 1) s = 1; else if (s < -1) s = -1;
    frameFloats[i] = s;
    mean += s;
  }
  mean /= n;
  let sum = 0;
  for (let i = 0; i < n; i++) {
    const d = frameFloats[i] - mean;
    sum += d * d;
  }
  frameFloats.fill(0); // privacy barrier: the raw samples don't outlive their scalar
  const r = Math.sqrt(sum / n) * 32767;
  return r > 65535 ? 65535 : Math.round(r);
}

function makeResampler(inRate, outRate) {
  const ratio = inRate / outRate;
  let tail = new Float32Array(0);
  let pos = 0;
  return (input) => {
    const buf = new Float32Array(tail.length + input.length);
    buf.set(tail, 0); buf.set(input, tail.length);
    const out = [];
    while (Math.floor(pos) + 1 < buf.length) {
      const i = Math.floor(pos), frac = pos - i;
      out.push(buf[i] * (1 - frac) + buf[i + 1] * frac);
      pos += ratio;
    }
    const keep = Math.floor(pos);
    tail = buf.slice(keep);
    pos -= keep;
    return out;
  };
}

function stopLive() {
  state.running = false;
  if (state.raf) { cancelAnimationFrame(state.raf); state.raf = 0; }
  releaseAudio();
  render();
}

function releaseAudio() {
  try { if (state.node) state.node.onaudioprocess = null; } catch { /* ignore */ }
  try { if (state.source) state.source.disconnect(); } catch { /* ignore */ }
  try { if (state.node) state.node.disconnect(); } catch { /* ignore */ }
  try { if (state.sink) state.sink.disconnect(); } catch { /* ignore */ }
  try { if (state.stream) state.stream.getTracks().forEach((t) => t.stop()); } catch { /* ignore */ }
  try { if (state.ctx) state.ctx.close(); } catch { /* ignore */ }
  state.node = state.source = state.sink = state.stream = state.ctx = null;
}

function renderLiveError(err) {
  const card = $("#mic-card");
  const name = (err && err.name) || "";
  let msg;
  if (name === "NotAllowedError" || name === "SecurityError")
    msg = "Microphone permission was declined. Nothing was captured. Allow it in " +
          "your browser's site settings and press Start — or use “Play a sound”.";
  else if (name === "NotFoundError" || name === "OverconstrainedError")
    msg = "No microphone was found. Plug one in (or enable it) and press Start.";
  else
    msg = "The microphone couldn't be opened (" + (name || "unknown error") + "). " +
          "Nothing was captured. You can try again.";
  const warn = $(".mic-live");
  if (warn) { warn.innerHTML = ""; warn.append(el("p", "mic-warn", msg)); }
}

function stopEverything() { stopPlayback(); if (state.running) stopLive(); }

// ── the visualization ────────────────────────────────────────────────────────
function viz() {
  const v = el("div", "mic-viz");

  // The one number that crosses the barrier.
  const scalar = el("div", "mic-scalar");
  scalar.id = "mic-scalar";
  const num = el("div", "mic-scalar-num", "—");
  num.id = "mic-scalar-num";
  scalar.append(num);
  scalar.append(el("div", "mic-scalar-cap",
    "the only thing crossing the privacy barrier — one loudness number / 20 ms"));
  v.append(scalar);

  // Loudness meter with the live floor + trip marks.
  const meter = el("div", "mic-meter");
  meter.append(el("div", "mic-meter-fill"));
  const floorMark = el("div", "mic-meter-floor"); floorMark.title = "the room's tracked noise floor";
  const onMark = el("div", "mic-meter-on"); onMark.title = "a beep must clear this to count as loud";
  meter.append(floorMark, onMark);
  v.append(labeled("Loudness — with the self-calibrating floor (gray) and trip line (white)", meter));

  // The rolling loud/quiet strip.
  const strip = el("div", "mic-strip"); strip.id = "mic-strip";
  v.append(labeled("What it's hearing — loud (gold) vs quiet, the last few seconds", strip));

  // Status + verdict.
  const status = el("div", "mic-status"); status.id = "mic-status";
  status.textContent = "Idle — pick a sound above, or start your mic.";
  v.append(status);
  const banner = el("div", "mic-banner"); banner.id = "mic-banner";
  v.append(banner);

  // Wake pulse.
  const wake = el("div", "mic-wake"); wake.id = "mic-wake";
  v.append(wake);
  return v;
}

function labeled(text, node) {
  const wrap = el("div", "mic-field");
  wrap.append(el("div", "mic-field-label", text), node);
  return wrap;
}

function paint() {
  const snap = state.latest;
  const numEl = $("#mic-scalar-num");
  if (numEl) numEl.textContent = snap ? String(snap.rms) : "—";

  const fill = $(".mic-meter-fill");
  if (fill && snap) {
    fill.style.width = Math.min(100, (snap.rms / METER_MAX) * 100).toFixed(1) + "%";
    fill.classList.toggle("mic-meter-hot", !!snap.loud);
    const fm = $(".mic-meter-floor");
    if (fm) fm.style.left = Math.min(100, (snap.floor / METER_MAX) * 100).toFixed(1) + "%";
    const om = $(".mic-meter-on");
    if (om) om.style.left = Math.min(100, (snap.on / METER_MAX) * 100).toFixed(1) + "%";
  }

  const strip = $("#mic-strip");
  if (strip) {
    strip.innerHTML = "";
    if (!state.timeline.length) strip.append(el("span", "mic-strip-empty", "waiting for sound…"));
    else state.timeline.forEach((t) => {
      const c = el("span", "mic-col-cell" + (t.loud ? " mic-col-loud" : ""));
      strip.append(c);
    });
  }

  const status = $("#mic-status");
  if (status && snap) {
    const beeps = sim.cad.beeps, streak = sim.cad.streak;
    let s = `floor ${snap.floor} · trips at ${snap.on} · now ${snap.rms}`;
    if (beeps > 0 || streak > 0) s += `  ·  group beeps: ${beeps} · matching cycles: ${streak}/2`;
    if (state.running && snap.rms < 40) s = "Listening — but hearing near-silence. Is the mic muted, or the sound far away?";
    status.textContent = s;
  }

  paintBanner();
  paintWake();
}

function paintBanner() {
  const banner = $("#mic-banner");
  if (!banner) return;
  const ev = state.event;
  const fresh = ev && (state.frame - ev.at) * FRAME_MS < 8000;
  if (fresh) {
    banner.className = "mic-banner mic-banner-hit" + (calm() ? "" : " mic-banner-flash");
    const label = ev.kind === "smoke_t3" ? "SMOKE-ALARM CADENCE — Alert raised"
                                          : "CO-ALARM CADENCE — Alert raised";
    banner.textContent = `🔔 ${label} · confidence ${ev.conf}% · fires as ${eventWireName(ev.kind)}`;
  } else if (state.latest) {
    banner.className = "mic-banner";
    banner.textContent = "No alarm cadence — nothing raised. Two on-grammar cycles are required.";
  } else {
    banner.className = "mic-banner";
    banner.textContent = "";
  }
}

function paintWake() {
  const wake = $("#mic-wake");
  if (!wake) return;
  const fresh = state.woke && (state.frame - state.woke) * FRAME_MS < 2600;
  if (fresh) {
    wake.className = "mic-wake mic-wake-on" + (calm() ? "" : " mic-wake-flash");
    wake.textContent = "💡 Screen woke — a loud onset, not an alarm. Nothing was recorded.";
  } else {
    wake.className = "mic-wake";
    wake.textContent = sim.wakeOnSound ? "" : "";
  }
}

// ── the real firmware settings, from the drift-locked constants ──────────────
function factsTable() {
  const facts = el("details", "mic-facts");
  facts.append(el("summary", null, "The real firmware settings (read from the core)"));
  const body = el("div", "mic-facts-body");
  const row = (k, v) => { const d = el("div", "mic-fact"); d.append(el("span", "mic-fact-k", k), el("span", "mic-fact-v", v)); body.append(d); };
  row("Smoke (T3)", `3 beeps, each ${CADENCE.T3_BEEP_MIN}–${CADENCE.T3_BEEP_MAX} ms; two cycles raise the Alert`);
  row("CO (T4)", `4 beeps, each ${CADENCE.T4_BEEP_MIN}–${CADENCE.T4_BEEP_MAX} ms; two cycles raise the Alert`);
  row("Group / reset gaps", `a gap ≥ ${CADENCE.GROUP_GAP_MS} ms closes a group; ≥ ${CADENCE.RESET_GAP_MS / 1000} s of silence clears the streak`);
  SENS_ORDER.forEach((name) => {
    const s = SENS[name];
    row("Sensitivity · " + name, `trips at ${dbLabel(s.on_pct)} over the floor, releases at ${dbLabel(s.off_pct)}; silence-clamp ${s.floor_min}`);
  });
  row("Wake-on-sound", `${dbLabel(TRANSIENT.RISE_PCT)} over the floor, one wake per ${TRANSIENT.REFRACTORY_MS / 1000} s (opt-in)`);
  row("Why rhythm alone isn't enough", "the count + per-beep duration must both fit — a doorbell has neither grammar");
  facts.append(body);
  return facts;
}

// Release the mic if the tab is hidden/closed — never hold it in the background.
window.addEventListener("pagehide", () => { if (state.running) { state.running = false; releaseAudio(); } });
document.addEventListener("visibilitychange", () => { if (document.hidden && state.running) stopLive(); });

// ── boot ─────────────────────────────────────────────────────────────────────
mountVersions();
render();
