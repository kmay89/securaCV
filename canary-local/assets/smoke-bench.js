// canary-local/assets/smoke-bench.js — "listen for a smoke alarm".
//
// Runs the REAL Canary WAP acoustic detector (securacv_audio.cpp, compiled to
// WebAssembly) on the visitor's microphone. The browser only stages PCM — the
// same sensor boundary the PDM mic occupies on hardware; every verdict (is
// this beep alarm-band, does this rhythm fit the NFPA-72 T3 / UL-2034 T4
// template, does an event fire) is the firmware's, in wasm.
//
// Safeguards, by construction:
//   • The mic is requested only from an explicit button press (user gesture),
//     after the page has said in plain words what it will do.
//   • Audio is processed frame-by-frame and immediately discarded — nothing is
//     recorded, buffered to disk, stored, or sent anywhere. There is no network
//     path in this file at all (bar reading the local build stamp).
//   • Browser audio "cleanup" (echo cancellation, noise suppression, auto-gain)
//     is turned OFF — it would gut the 3.4 kHz alarm tone — and that's stated.
//   • Stop fully releases the stream (every track stopped, context closed).
//   • Graceful, specific fallbacks for: no getUserMedia, insecure context,
//     permission denied, no device, and a mic that hears nothing.
//   • Decorative motion respects prefers-reduced-motion.
//   • A standing, unmissable disclaimer: this is a demonstration of how the
//     detector works, NOT a life-safety device.

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

const FRAME_SAMPLES = 320;      // 20 ms at 16 kHz — the detector's frame
const TARGET_RATE = 16000;

// ── wasm core boot ──────────────────────────────────────────────────────────
let core = null;   // cwrapped ABI
async function bootCore() {
  const factory = globalThis.createCanaryAudioCore;
  if (typeof factory !== "function") {
    throw new Error("the committed Canary WAP acoustic core is missing");
  }
  const m = await factory();
  const c = {
    m,
    contract: JSON.parse(m.cwrap("audio_emu_contract_json", "string", [])()),
    reset: m.cwrap("audio_emu_reset", null, []),
    frameSamples: m.cwrap("audio_emu_frame_samples", "number", []),
    framePtr: m.cwrap("audio_emu_frame_ptr", "number", []),
    processFrame: m.cwrap("audio_emu_process_frame", "string", []),
    setThresholds: m.cwrap("audio_emu_set_thresholds", "number", ["number", "number"]),
  };
  if (c.contract.schema !== "securacv.canary-wap.audio-core/v1") {
    throw new Error("acoustic core schema mismatch: " + c.contract.schema);
  }
  return c;
}

// The build stamp (which firmware these bytes are) — best-effort, local only.
async function readStamp() {
  try {
    const r = await fetch("emulator/dist/canary-wap-audio.meta.json", { cache: "no-store" });
    if (r.ok) return await r.json();
  } catch { /* file:// or offline — the chip just omits the version */ }
  return null;
}

// ── page scaffolding ────────────────────────────────────────────────────────
const state = {
  running: false,
  ctx: null,
  stream: null,
  node: null,
  source: null,
  latest: null,      // most recent per-frame JSON snapshot
  eventAt: 0,        // stream-time of the last alarm event, for the banner
  eventKind: 0,
  eventConf: 0,
  raf: 0,
  frames: 0,
};

function mountVersions(stamp) {
  const strip = $("#smoke-versions");
  if (!strip) return;
  strip.innerHTML = "";
  const pill = (label, val) => {
    const p = el("span", "pill");
    p.append(el("strong", null, label + " "), document.createTextNode(val));
    return p;
  };
  if (stamp && stamp.fw_version) strip.append(pill("firmware", stamp.fw_version));
  strip.append(pill("runtime", "real firmware wasm"));
  strip.append(pill("engine", "securacv_audio (vendored, offline)"));
}

function render() {
  const mount = $("#smoke");
  mount.innerHTML = "";

  // The standing disclaimer — always visible, never dismissible.
  const disc = el("div", "smoke-disclaimer");
  disc.append(el("strong", null, "This is a demonstration, not an alarm. "));
  disc.append(document.createTextNode(
    "It shows how a Canary WAP recognizes an alarm's sound. It is not a smoke " +
    "or CO detector and is no substitute for a UL-listed one. In an emergency, " +
    "trust your real alarms and call your local emergency number."));
  mount.append(disc);

  const card = el("section", "smoke-card");
  card.id = "smoke-card";
  mount.append(card);

  if (!state.running) renderIdle(card);
  else renderLive(card);

  renderFacts(mount);
}

function supportProblem() {
  if (!window.isSecureContext) {
    return "Microphone access needs a secure page (https, or localhost). " +
           "Open this page over https and the button will work.";
  }
  if (!navigator.mediaDevices || !navigator.mediaDevices.getUserMedia) {
    return "This browser doesn't offer microphone access to web pages. " +
           "Try a current Chrome, Edge, Firefox or Safari.";
  }
  return null;
}

function renderIdle(card) {
  card.append(el("h2", null, "Hear what the WAP hears"));
  const how = el("ol", "smoke-how");
  how.append(liNode("Have a smoke or CO alarm handy — or a phone playing a " +
    "smoke-alarm test tone."));
  how.append(liNode("Press ", strongNode("Start listening"), " below and allow " +
    "the microphone when your browser asks."));
  how.append(liNode("Hold the alarm near your microphone and press its ",
    strongNode("TEST"), " button (or play the tone). Watch the trace fill in."));
  card.append(how);

  const problem = supportProblem();
  if (problem) {
    const warn = el("p", "smoke-warn", problem);
    card.append(warn);
    return;
  }

  const priv = el("p", "smoke-priv");
  priv.append(el("span", "smoke-priv-key", "🔒 "));
  priv.append(document.createTextNode(
    "Your microphone audio stays in this page. It's examined 20 milliseconds " +
    "at a time and thrown away — never recorded, saved, or uploaded. Stop " +
    "anytime and the mic is released at once."));
  card.append(priv);

  const btn = el("button", "smoke-start", "🎤 Start listening");
  btn.type = "button";
  btn.addEventListener("click", () => start(btn));
  card.append(btn);
}

function liNode(...parts) { const li = el("li"); li.append(...parts.map(toNode)); return li; }
function strongNode(t) { return el("strong", null, t); }
function toNode(x) { return typeof x === "string" ? document.createTextNode(x) : x; }

function renderLive(card) {
  card.classList.add("smoke-live");
  const head = el("div", "smoke-live-head");
  const dot = el("span", "smoke-listening-dot");
  head.append(dot, el("span", null, "Listening — the detector is running on your mic"));
  const stop = el("button", "smoke-stop", "◼ Stop & release mic");
  stop.type = "button";
  stop.addEventListener("click", stopListening);
  head.append(stop);
  card.append(head);

  // Live level meter (the same RMS the envelope hysteresis reads).
  const meter = el("div", "smoke-meter");
  meter.append(el("div", "smoke-meter-fill"));
  const onMark = el("div", "smoke-meter-on");
  onMark.title = "The loudness a beep must clear to register";
  meter.append(onMark);
  card.append(labeled("Loudness (live RMS)", meter));

  // The cadence trace — recent beeps, colored by whether they were
  // alarm-band (the tone gate) — this is the "show me why" view.
  const trace = el("div", "smoke-trace");
  trace.id = "smoke-trace";
  card.append(labeled("Cadence — recent beeps (gold = alarm-band tone)", trace));

  // Event banner.
  const banner = el("div", "smoke-banner");
  banner.id = "smoke-banner";
  card.append(banner);

  // Sensitivity — the real runtime threshold path (Device-tab equivalent).
  const sens = el("div", "smoke-sens");
  const slider = el("input");
  slider.type = "range";
  slider.min = "300"; slider.max = "1500"; slider.step = "50";
  slider.value = String(core.contract.rms_on);
  slider.addEventListener("input", () => {
    const on = Number(slider.value);
    const off = Math.max(100, Math.round(on / 2));
    core.setThresholds(on, off);
    sensVal.textContent = `on ${on} / off ${off}`;
  });
  const sensVal = el("span", "smoke-sens-val", `on ${core.contract.rms_on} / off ${core.contract.rms_off}`);
  sens.append(el("span", "smoke-sens-label", "Sensitivity"), slider, sensVal);
  sens.title = "A quieter alarm (small laptop speaker, across the room) may need a lower threshold.";
  card.append(sens);
}

function labeled(text, node) {
  const wrap = el("div", "smoke-field");
  wrap.append(el("div", "smoke-field-label", text), node);
  return wrap;
}

function renderFacts(mount) {
  const c = core.contract;
  const facts = el("details", "smoke-facts");
  facts.append(el("summary", null, "What counts as an alarm (the real firmware settings)"));
  const body = el("div", "smoke-facts-body");
  const row = (k, v) => { const d = el("div", "smoke-fact"); d.append(el("span", "smoke-fact-k", k), el("span", "smoke-fact-v", v)); body.append(d); };
  row("Alarm-band tone", `${c.tone_fc_hz} Hz center — a beep must be ≥ ${c.tone_min_x100}% concentrated here`);
  row("T3 (smoke)", `${c.t3.beeps}× ${c.t3.beep_ms} ms beeps, ${c.t3.gap_ms} ms gaps, then ~${c.t3.pause_ms} ms silence`);
  row("T4 (CO)", `${c.t4.beeps}× ${c.t4.beep_ms} ms beeps, ${c.t4.gap_ms} ms gaps, then ~${c.t4.pause_ms} ms silence`);
  row("Why a rhythm isn't enough", "three door-slams have the beat but not the 3.4 kHz tone — the gate rejects them");
  facts.append(body);
  mount.append(facts);
}

// ── mic → wasm plumbing ─────────────────────────────────────────────────────
async function start(btn) {
  btn.disabled = true;
  btn.textContent = "Asking for the microphone…";
  try {
    // Processing OFF: echo-cancel / noise-suppress / AGC would attack the
    // narrow alarm tone. We want the raw room.
    const stream = await navigator.mediaDevices.getUserMedia({
      audio: {
        echoCancellation: false,
        noiseSuppression: false,
        autoGainControl: false,
        channelCount: 1,
      },
    });
    state.stream = stream;

    const Ctx = window.AudioContext || window.webkitAudioContext;
    let ctx;
    try { ctx = new Ctx({ sampleRate: TARGET_RATE }); }
    catch { ctx = new Ctx(); }            // some engines reject a forced rate
    if (ctx.state === "suspended") { try { await ctx.resume(); } catch { /* gesture-gated */ } }
    state.ctx = ctx;

    core.reset();
    core.setThresholds(core.contract.rms_on, core.contract.rms_off);

    const source = ctx.createMediaStreamSource(stream);
    state.source = source;
    const node = ctx.createScriptProcessor(4096, 1, 1);
    state.node = node;
    const rs = makeResampler(ctx.sampleRate, TARGET_RATE);
    let frame = new Float32Array(FRAME_SAMPLES);
    let fill = 0;

    node.onaudioprocess = (e) => {
      if (!state.running) return;
      const input = e.inputBuffer.getChannelData(0);
      const out = rs(input);                 // Float32 at 16 kHz
      for (let i = 0; i < out.length; i++) {
        frame[fill++] = out[i];
        if (fill === FRAME_SAMPLES) {
          feedFrame(frame);
          fill = 0;
        }
      }
    };
    source.connect(node);
    // ScriptProcessor needs a sink to pump; a zero-gain node keeps it silent
    // so we never echo the room back out of the speakers.
    const sink = ctx.createGain();
    sink.gain.value = 0;
    node.connect(sink);
    sink.connect(ctx.destination);
    state.sink = sink;

    state.running = true;
    state.frames = 0;
    render();
    loop();
  } catch (err) {
    state.running = false;
    releaseAudio();
    renderError(err);
  }
}

function feedFrame(frameFloats) {
  const m = core.m;
  const ptr = core.framePtr();
  // Re-view HEAP16 every frame: ALLOW_MEMORY_GROWTH can detach the buffer.
  const view = new Int16Array(m.HEAP16.buffer, ptr, FRAME_SAMPLES);
  for (let i = 0; i < FRAME_SAMPLES; i++) {
    let s = frameFloats[i];
    if (s > 1) s = 1; else if (s < -1) s = -1;
    view[i] = (s * 32767) | 0;
  }
  const snap = JSON.parse(core.processFrame());
  state.latest = snap;
  state.frames++;
  if (snap.event === 1 || snap.event === 2) {
    state.eventKind = snap.event;
    state.eventConf = snap.confidence;
    state.eventAt = state.frames * 20;    // stream-time ms
  }
}

// Linear-interpolation resampler from inRate → outRate, carrying its
// fractional read position across callbacks so beeps aren't chopped.
function makeResampler(inRate, outRate) {
  const ratio = inRate / outRate;
  let tail = new Float32Array(0);
  let pos = 0;
  return (input) => {
    const buf = new Float32Array(tail.length + input.length);
    buf.set(tail, 0); buf.set(input, tail.length);
    const out = [];
    while (Math.floor(pos) + 1 < buf.length) {
      const i = Math.floor(pos);
      const frac = pos - i;
      out.push(buf[i] * (1 - frac) + buf[i + 1] * frac);
      pos += ratio;
    }
    const keep = Math.floor(pos);
    tail = buf.slice(keep);
    pos -= keep;
    return out;
  };
}

// ── render loop (decoupled from the 50 Hz audio callback) ────────────────────
function loop() {
  if (!state.running) return;
  paint();
  state.raf = requestAnimationFrame(loop);
}

function paint() {
  const snap = state.latest;
  if (!snap) return;
  const c = core.contract;

  const fill = $(".smoke-meter-fill");
  if (fill) {
    // Map RMS (0..~6000 typical) to a width; the "on" mark sits at rms_on.
    const pct = Math.min(100, (snap.level / 6000) * 100);
    fill.style.width = pct.toFixed(1) + "%";
    fill.classList.toggle("smoke-meter-hot", snap.level >= c.rms_on);
    const onMark = $(".smoke-meter-on");
    if (onMark) onMark.style.left = Math.min(100, (c.rms_on / 6000) * 100).toFixed(1) + "%";
  }

  const trace = $("#smoke-trace");
  if (trace) paintTrace(trace, snap, c);

  const banner = $("#smoke-banner");
  if (banner) paintBanner(banner, snap);

  // A mic that hears nothing for a while: gently coach.
  const head = $(".smoke-live-head span:nth-child(2)");
  if (head) {
    if (snap.zero_streak > 100) head.textContent = "Listening — but hearing silence. Is the mic muted, or the alarm far away?";
    else head.textContent = "Listening — the detector is running on your mic";
  }
}

function paintTrace(trace, snap, c) {
  // Newest transitions first; show ON states (beeps) as bars, gold when the
  // beep was alarm-band (tone_x100 ≥ the gate), gray when it wasn't.
  const beeps = (snap.transitions || []).filter((t) => !t.is_on); // OFF-entry carries the prev ON (beep) stats
  trace.innerHTML = "";
  if (!beeps.length) {
    trace.append(el("span", "smoke-trace-empty", "waiting for sound…"));
    return;
  }
  // oldest → newest for reading left-to-right
  beeps.slice().reverse().forEach((t) => {
    const bar = el("div", "smoke-bar");
    const h = Math.max(8, Math.min(60, (t.dur_ms / 10)));  // 500 ms → 50px
    bar.style.height = h + "px";
    const band = t.tone_x100 >= c.tone_min_x100;
    bar.classList.toggle("smoke-bar-band", band);
    bar.title = `${t.dur_ms} ms beep · ${band ? "alarm-band" : "off-band"} (${t.tone_x100}% @ ${c.tone_fc_hz} Hz)`;
    trace.append(bar);
  });
}

function paintBanner(banner, snap) {
  const age = state.frames * 20 - state.eventAt;
  const fresh = state.eventKind && age >= 0 && age < 6000;   // hold ~6 s
  if (fresh) {
    banner.className = "smoke-banner smoke-banner-hit" + (calm() ? "" : " smoke-banner-flash");
    const label = state.eventKind === 1 ? "SMOKE-ALARM CADENCE DETECTED"
                                        : "CO-ALARM CADENCE DETECTED";
    banner.textContent = `🔔 ${label} — confidence ${state.eventConf}%`;
  } else {
    banner.className = "smoke-banner";
    banner.textContent = snap && snap.t3_total + snap.t4_total > 0
      ? "No alarm cadence right now — the trace shows what it's hearing."
      : "No alarm cadence yet. Press your alarm's TEST button near the mic.";
  }
}

// ── teardown ─────────────────────────────────────────────────────────────────
function stopListening() {
  state.running = false;
  if (state.raf) cancelAnimationFrame(state.raf);
  releaseAudio();
  state.latest = null;
  state.eventKind = 0;
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

function renderError(err) {
  const card = $("#smoke-card") || $("#smoke");
  const name = (err && err.name) || "";
  let msg;
  if (name === "NotAllowedError" || name === "SecurityError") {
    msg = "Microphone permission was declined. No problem — nothing was captured. " +
          "To try again, allow the mic in your browser's site settings and press Start.";
  } else if (name === "NotFoundError" || name === "OverconstrainedError") {
    msg = "No microphone was found. Plug one in (or enable it) and press Start.";
  } else {
    msg = "The microphone couldn't be opened (" + (name || "unknown error") + "). " +
          "Nothing was captured. You can try again.";
  }
  card.innerHTML = "";
  card.append(el("h2", null, "Couldn't start listening"));
  card.append(el("p", "smoke-warn", msg));
  const again = el("button", "smoke-start", "🎤 Try again");
  again.type = "button";
  again.addEventListener("click", () => { render(); });
  card.append(again);
}

// Release the mic if the tab is hidden/closed — never hold it in the background.
window.addEventListener("pagehide", () => { if (state.running) { state.running = false; releaseAudio(); } });
document.addEventListener("visibilitychange", () => {
  if (document.hidden && state.running) stopListening();
});

// ── boot ─────────────────────────────────────────────────────────────────────
(async () => {
  const mount = $("#smoke");
  try {
    core = await bootCore();
  } catch (err) {
    mount.innerHTML = "";
    mount.append(el("p", "smoke-warn",
      "The acoustic detector core didn't load. This bench needs the committed " +
      "WebAssembly build (emulator/dist/canary-wap-audio.js). " + (err && err.message ? "(" + err.message + ")" : "")));
    return;
  }
  const stamp = await readStamp();
  mountVersions(stamp);
  render();
})();
