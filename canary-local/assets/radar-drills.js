// canary-local/assets/radar-drills.js — DOM-free cores for the proving ground.
//
// Three jobs, all pure ((input) → output, time injected) so Node tests drive
// them and the page just renders:
//
//   DrillEngine   the post-flash check suite: each drill arms, watches the
//                 parsed console events (flash-core.js parseSenseLine et al.
//                 — the SAME events whether the cable is real or the twin),
//                 and settles pass/fail. No drill ever writes a knob the
//                 user didn't ask for: the one command drill re-sets a value
//                 to itself, which the firmware answers "(unchanged)".
//   calibPlan     zone calibration: raw_dist samples collected while the
//                 person stands at each zone edge → clamped near/mid
//                 recommendations (the same bounds the console will enforce).
//   placementAdvice  the interactive placement scorer: sense-sim.js's
//                 radarView/vitalsQuality physics → a grade and the fixes,
//                 so "where should it live?" gets an answer, not a shrug.

import { radarView, vitalsQuality } from "./sense-sim.js";
import { PRESENCE_KNOBS } from "./radar-emu.js";

// ============================================================================
// The drill suite
// ============================================================================
//
// A drill definition:
//   { id, title, ask, prove, kind: "auto" | "guided", timeoutMs,
//     wellbeingOnly?, start(ctx)?, on(ev, st, ctx) -> "pass"|"fail:…"|null }
// ctx: { send(cmd), now, wellbeing } — send speaks the tuning console.
// st: per-run scratch state (reset on every arm).
//
// Event vocabulary is flash-core.js's: {kind:"cfg"|"tune"|"radar"|"sense"|
// "presence"|"vitals"|"bpm"|"health", …}.

const isPresent = (ev) =>
  (ev.kind === "radar" || ev.kind === "sense" || ev.kind === "presence") && ev.presence === "present";
const isClear = (ev) =>
  (ev.kind === "radar" || ev.kind === "sense" || ev.kind === "presence") && ev.presence === "clear";

export const DRILLS = [
  {
    id: "console", kind: "auto", timeoutMs: 6000,
    title: "The console answers",
    ask: "asks the board for its knob snapshot",
    prove: "a [cfg] line arrives — the tuning console is alive on this cable",
    start(ctx) { ctx.send("cfg"); },
    on(ev) { return ev.kind === "cfg" ? "pass" : null; },
  },
  {
    id: "stream", kind: "auto", timeoutMs: 8000,
    title: "The radar speaks on schedule",
    ask: "listens for the periodic [radar] heartbeat",
    prove: "three stream lines arrive — the radar link and the stream timer both run",
    on(ev, st) {
      if (ev.kind !== "radar") return null;
      st.seen = (st.seen || 0) + 1;
      return st.seen >= 3 ? "pass" : null;
    },
  },
  {
    id: "roundtrip", kind: "auto", timeoutMs: 6000,
    title: "A knob round-trips",
    ask: "re-sets debounce to its current value (a deliberate no-op)",
    prove: "the [tune] ok verdict and the refreshed [cfg] echo agree — set → clamp → NVS → echo all work",
    on(ev, st, ctx) {
      if (ev.kind === "cfg" && st.sent == null) {
        st.want = ev.values.debounce;
        if (!Number.isFinite(st.want)) return "fail:no debounce knob in the [cfg] line";
        st.sent = true;
        ctx.send(`set debounce ${st.want}`);
        return null;
      }
      if (st.sent && ev.kind === "tune") {
        if (!ev.ok) return "fail:the console refused: " + ev.text;
        st.verdict = true;
        return null;
      }
      if (st.verdict && ev.kind === "cfg") {
        return ev.values.debounce === st.want ? "pass"
          : `fail:echo says debounce=${ev.values.debounce}, expected ${st.want}`;
      }
      return null;
    },
    start(ctx) { ctx.send("cfg"); },
  },
  {
    id: "clean", kind: "auto", timeoutMs: 12000,
    title: "Frames arrive clean",
    ask: "watches the checksum-error counter across five stream lines",
    prove: "errs stays flat — the radar UART is wired right and framing is healthy",
    on(ev, st) {
      if (ev.kind !== "radar" || !Number.isFinite(ev.frame_errs)) return null;
      if (st.first == null) { st.first = ev.frame_errs; st.seen = 1; return null; }
      st.seen += 1;
      if (st.seen < 5) return null;
      return ev.frame_errs === st.first ? "pass"
        : `fail:errs climbed ${st.first} → ${ev.frame_errs} — check the radar UART wiring`;
    },
  },
  {
    id: "walk_in", kind: "guided", timeoutMs: 30000,
    title: "It sees you arrive",
    ask: "walk into the radar's view and keep moving",
    prove: "presence flips to “present” within the debounce — the whole chain senses",
    on(ev) { return isPresent(ev) ? "pass" : null; },
  },
  {
    id: "bands", kind: "guided", timeoutMs: 60000,
    title: "The three bands track you",
    ask: "walk slowly from right up close to the far side of the room",
    prove: "near, mid and far are each reported — the range gates fit the room",
    on(ev, st) {
      if ((ev.kind !== "radar" && ev.kind !== "sense") || ev.presence !== "present") return null;
      st.bands = st.bands || new Set();
      if (["near", "mid", "far"].includes(ev.range)) st.bands.add(ev.range);
      return st.bands.size === 3 ? "pass" : null;
    },
  },
  {
    id: "two_up", kind: "guided", timeoutMs: 45000,
    title: "It counts to two-plus",
    ask: "bring a second person (or wave a friend in)",
    prove: "the occupant bucket climbs to 2+ — and vitals politely refuse while it does",
    on(ev) {
      if ((ev.kind === "radar" || ev.kind === "sense") && ev.count === "2+") return "pass";
      return null;
    },
  },
  {
    id: "still_hold", kind: "guided", timeoutMs: 40000, holdMs: 10000,
    title: "Stillness doesn't lose you",
    ask: "sit statue-still in view for ten seconds",
    prove: "presence holds — the mmWave advantage a PIR can't offer",
    on(ev, st, ctx) {
      if (isClear(ev)) {
        return st.since != null ? "fail:presence dropped while you held still — raise the clear timeout" : null;
      }
      if (!isPresent(ev)) return null;
      if (st.since == null) st.since = ctx.now;
      return ctx.now - st.since >= (this.holdMs || 10000) ? "pass" : null;
    },
  },
  {
    id: "walk_out", kind: "guided", timeoutMs: 60000,
    title: "It lets you leave",
    ask: "leave the radar's view entirely",
    prove: "presence breathes out to “clear” after the clear timeout",
    on(ev, st) {
      if (isPresent(ev)) { st.wasPresent = true; return null; }
      return st.wasPresent && isClear(ev) ? "pass" : null;
    },
  },
  {
    id: "vitals_lock", kind: "guided", timeoutMs: 90000, wellbeingOnly: true,
    title: "Breathing locks on",
    ask: "sit alone, still, a couple of meters away — and settle",
    prove: "the vitals lock confirms and live breath/heart numbers appear (one person only)",
    on(ev) {
      if (ev.kind === "bpm" && ev.breath > 0) return "pass";
      if (ev.kind === "radar" && ev.lock === "locked" && Number.isFinite(ev.breath) && ev.breath > 0) return "pass";
      return null;
    },
  },
];

export function drillsFor(wellbeing) {
  return DRILLS.filter((d) => !d.wellbeingOnly || wellbeing);
}

// One armed drill at a time (the guided flow is a hand-held sequence, and
// auto drills each finish in seconds). Results persist across arms.
export class DrillEngine {
  // send: (cmd) => void — the console's writer, real or twin.
  constructor(drills, send, wellbeing) {
    this.drills = drills;
    this.send = send;
    this.wellbeing = !!wellbeing;
    this.results = {}; // id -> {status: "pass"|"fail", reason?, ms}
    this.active = null;
    this.onSettle = null; // (id, result) => void
  }

  byId(id) { return this.drills.find((d) => d.id === id); }

  arm(id, now) {
    const d = this.byId(id);
    if (!d) return false;
    this.active = { d, st: {}, armedAt: now };
    if (d.start) d.start({ send: this.send, now, wellbeing: this.wellbeing });
    return true;
  }

  disarm() { this.active = null; }

  _settle(status, reason, now) {
    const { d, armedAt } = this.active;
    this.active = null;
    const result = { status, reason, ms: now - armedAt };
    this.results[d.id] = result;
    if (this.onSettle) this.onSettle(d.id, result);
  }

  // Feed one parsed console event (flash-core vocabulary).
  feed(ev, now) {
    if (!this.active || !ev) return;
    const { d, st, armedAt } = this.active;
    const verdict = d.on ? d.on(ev, st, { send: this.send, now, wellbeing: this.wellbeing }) : null;
    if (verdict === "pass") { this._settle("pass", null, now); return; }
    if (typeof verdict === "string" && verdict.startsWith("fail:")) {
      this._settle("fail", verdict.slice(5), now);
      return;
    }
    if (now - armedAt >= d.timeoutMs) {
      this._settle("fail", "timed out — see the drill's hint and try again", now);
    }
  }

  // Time-only tick so a silent console still times out (and hold-style
  // drills can pass on pure elapsed time).
  tick(now) {
    this.feed({ kind: "health" }, now); // a neutral event: drives timeouts/holds only
  }

  summary() {
    const total = this.drills.length;
    let pass = 0, fail = 0;
    for (const d of this.drills) {
      const r = this.results[d.id];
      if (!r) continue;
      if (r.status === "pass") pass++;
      else fail++;
    }
    return { total, pass, fail, done: pass + fail };
  }
}

// ============================================================================
// Zone calibration — raw_dist marks → near/mid recommendations
// ============================================================================

export function median(xs) {
  if (!xs || !xs.length) return null;
  const s = [...xs].sort((a, b) => a - b);
  const m = s.length >> 1;
  return s.length % 2 ? s[m] : Math.round((s[m - 1] + s[m]) / 2);
}

// nearSamples / midSamples: raw_dist cm readings captured while the person
// held still at each zone edge. Returns { near_cm, mid_cm, notes[] } clamped
// to the console's own bounds, mid held a stride past near, both rounded to
// 10 cm so the numbers read like numbers a person chose.
export function calibPlan(nearSamples, midSamples) {
  const bounds = Object.fromEntries(PRESENCE_KNOBS.map((k) => [k.name, [k.lo, k.hi]]));
  const notes = [];
  const round10 = (v) => Math.round(v / 10) * 10;
  const clamp = (v, [lo, hi], name) => {
    const c = Math.min(Math.max(v, lo), hi);
    if (c !== v) notes.push(`${name} clamped to ${c} cm (console range ${lo}–${hi})`);
    return c;
  };

  const nearMed = median(nearSamples);
  const midMed = median(midSamples);
  if (nearMed == null || midMed == null) return { near_cm: null, mid_cm: null, notes: ["not enough samples"] };

  let near = clamp(round10(nearMed), bounds.near, "near");
  let mid = clamp(round10(midMed), bounds.mid, "mid");
  if (mid <= near) {
    // The bands must nest: hold mid at least half a meter past near.
    mid = clamp(near + 50, bounds.mid, "mid");
    if (mid <= near) {
      near = Math.max(bounds.near[0], mid - 50);
      notes.push("your two marks were too close together — near was pulled in to keep the bands nested");
    } else {
      notes.push("your two marks were too close together — mid was pushed out to keep the bands nested");
    }
  }
  return { near_cm: near, mid_cm: mid, notes };
}

// ============================================================================
// Placement advice — the physics, graded, with the fixes
// ============================================================================
//
// mode "presence": grades whether the spot will detect reliably.
// mode "vitals": grades the bedside geometry via the vitals quality model.
// scene/hw are sense-sim.js shapes (hw = devices/senselab.json "hardware").

export function placementAdvice(scene, hw, mode) {
  const view = radarView(scene, hw);
  const fixes = [];
  let grade, headline;

  if (mode === "vitals") {
    const vq = vitalsQuality(scene, hw);
    const q = vq.quality;
    grade = q >= 0.75 ? "A" : q >= 0.5 ? "B" : q > 0.15 ? "C" : "D";
    headline =
      grade === "A" ? "Textbook — this is the bedside install Seeed specs."
      : grade === "B" ? "Workable — the lock will hold, with some jitter."
      : grade === "C" ? "Marginal — expect dropouts and slow locks."
      : "This spot cannot hold a vitals lock.";
    for (const r of vq.reasons) fixes.push(r);
    if (view.r > hw.vitals_max_m) fixes.push(`move the mount so radar-to-chest is under ${hw.vitals_max_m} m (sweet spot ≈ ${hw.vitals_ref_m} m)`);
    if (scene.person.orientation === "side" && !(scene.mount === "ceiling" && scene.person.posture === "lying")) {
      fixes.push("aim the boresight at the chest, not across it — a side-on chest barely moves toward the radar");
    }
    return { grade, headline, fixes, view, quality: q };
  }

  // presence
  if (!view.inFov) {
    grade = "D";
    headline = "Outside the antenna sector — the radar cannot see this spot.";
    fixes.push(`the sector is ${hw.fov_deg}° wide — re-aim the face, or move the mount toward the zone`);
  } else if (!view.inRange) {
    grade = "D";
    headline = view.r < hw.presence_min_m
      ? "Too close — inside the radar's minimum range."
      : "Beyond the detection envelope.";
    if (view.r >= hw.presence_max_m) fixes.push(`the spec says ${hw.presence_max_m} m but plan on ~4 m effective — bring the zone inside 4`);
  } else if (view.r > 4.0) {
    grade = "C";
    headline = "Detectable on paper — but past the ~4 m effective range Seeed's own troubleshooting doc admits.";
    fixes.push("plan your zone inside 4 m; the 6 m figure is the spec sheet, not the field");
  } else if (view.offBoresightDeg > (hw.fov_deg / 2) * 0.75) {
    grade = "B";
    headline = "Covered, but near the edge of the sector where the antenna rolls off.";
    fixes.push("square the radar face toward the middle of the zone");
  } else {
    grade = "A";
    headline = "Solid — inside the sector, inside effective range.";
  }
  return { grade, headline, fixes, view, quality: null };
}
