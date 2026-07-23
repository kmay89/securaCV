// canary-local/assets/chirp.js — the Nursery's voice, tiny and synthesized.
//
// A Canary tool should be able to chirp. These are short WebAudio birdsong
// sketches — sine sweeps with soft envelopes, no samples, no network, a few
// hundred bytes. OFF by default: sound is invited, never sprung. The toggle
// persists per-browser (localStorage); every call is safe to make whether
// audio is enabled, blocked, or unsupported — it just quietly does nothing.
//
// Vocabulary (keep it small; each moment owns ONE sound):
//   hello  — board connected: two rising notes, "who's this?"
//   hatch  — install verified: a three-note fanfare, the newborn's first song
//   seen   — a bench first-detection: one bright tick
//   found  — presence flip on a bench: soft double-tick
//   oops   — a recoverable error: one low, round, non-alarming note
//
// Vanilla ES module, zero dependencies (repo convention).

const KEY = "nursery.chirps";
let ctx = null;

export function chirpsEnabled() {
  try { return localStorage.getItem(KEY) === "on"; } catch { return false; }
}

export function setChirpsEnabled(on) {
  try { localStorage.setItem(KEY, on ? "on" : "off"); } catch { /* private mode */ }
  if (on) chirp("hello"); // immediate feedback, from the enabling gesture
}

function audio() {
  if (!ctx) {
    try { ctx = new (window.AudioContext || window.webkitAudioContext)(); } catch { return null; }
  }
  if (ctx && ctx.state === "suspended") { try { ctx.resume(); } catch { /* gesture-gated */ } }
  return ctx;
}

// One note: a sine that sweeps f0→f1 over `dur`, with a fast attack and a
// gentle exponential release — the shape of a small bird, not a beeper.
function note(ac, t, f0, f1, dur, peak = 0.14) {
  const o = ac.createOscillator();
  const g = ac.createGain();
  o.type = "sine";
  o.frequency.setValueAtTime(f0, t);
  o.frequency.exponentialRampToValueAtTime(Math.max(f1, 40), t + dur);
  g.gain.setValueAtTime(0.0001, t);
  g.gain.exponentialRampToValueAtTime(peak, t + 0.012);
  g.gain.exponentialRampToValueAtTime(0.0001, t + dur);
  o.connect(g).connect(ac.destination);
  o.start(t);
  o.stop(t + dur + 0.02);
}

const SONGS = {
  hello: (ac, t) => { note(ac, t, 1200, 1800, 0.09); note(ac, t + 0.11, 1500, 2300, 0.12); },
  hatch: (ac, t) => { note(ac, t, 1100, 1700, 0.09); note(ac, t + 0.10, 1400, 2100, 0.09);
                      note(ac, t + 0.22, 1800, 2600, 0.16, 0.16); },
  seen:  (ac, t) => { note(ac, t, 2000, 2600, 0.07, 0.12); },
  found: (ac, t) => { note(ac, t, 1600, 2000, 0.06, 0.1); note(ac, t + 0.08, 1900, 2300, 0.06, 0.1); },
  oops:  (ac, t) => { note(ac, t, 500, 380, 0.22, 0.1); },
};

export function chirp(name) {
  if (!chirpsEnabled()) return;
  const ac = audio();
  if (!ac) return;
  const song = SONGS[name];
  if (!song) return;
  try { song(ac, ac.currentTime + 0.01); } catch { /* audio hiccup — never break the flow */ }
}

// The toggle chip the pages mount (flash.html's journey row). Reflects and
// persists state; enabling chirps chirps, so the choice is heard at once.
export function chirpToggle(doc = document) {
  const btn = doc.createElement("button");
  btn.type = "button";
  btn.className = "nursery-chirp-toggle";
  const sync = () => {
    const on = chirpsEnabled();
    btn.textContent = on ? "🔊 chirps on" : "🔇 chirps off";
    btn.setAttribute("aria-pressed", String(on));
    btn.title = on ? "The Nursery sings at the big moments — click to hush it."
                   : "Let the Nursery sing at the big moments (hello, hatch, first sight).";
  };
  btn.addEventListener("click", () => { setChirpsEnabled(!chirpsEnabled()); sync(); });
  sync();
  return btn;
}
