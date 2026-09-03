// canary-local/assets/app.js — the canary.local page.
//
// A pairing-card gallery of the Canary family. Each card is a live 3D
// object; opening a display's card boots the REAL firmware (wasm) behind
// its glass and offers three ways in: a guided Tour, symptom-first
// Fix-it flows, and free play with the LAN switches. Witness devices
// (no glass) get their decoder cards — LED grammar, chirp meanings,
// setup path. Everything works offline; nothing phones anywhere.

import { DeviceScene, BUILDERS } from "./scene3d.js";
import { buildFinishPicker, startFinishShowcase, hasUserChoice } from "./finishes.js";
import { fmtLen, UNIT_MODES } from "./assembly-rules.js";
import { upgradeRealShape } from "./real-shapes.js";
import { buildEnclosureLab } from "./enclosure-lab.js";
import { buildBuildIt } from "./build-it.js";
import { buildBoardLab } from "./board-lab.js";
import { buildAssemblyLab } from "./assembly-lab.js";
import { CanaryEmulator, demoFleet } from "../emulator/web/emu-shell.js";
import { BenchPower, romBanner } from "../emulator/web/bench.js";
import { DEMO, beatsBetween } from "./mode-sim.js";
import {
  DISPLAY_TOUR,
  DISPLAY_FIXES,
  BENCH_FIXES,
  LED_GRAMMAR,
  CHIRP_GRAMMAR,
  ledSequence,
} from "./guides.js";

const $ = (sel, el = document) => el.querySelector(sel);
const el = (tag, cls, text) => {
  const n = document.createElement(tag);
  if (cls) n.className = cls;
  if (text != null) n.textContent = text;
  return n;
};

const state = {
  registry: null,
  enclosures: null,
  cards: new Map(), // id → {scene}
  sheet: null,      // open device context
};

// ── boot ────────────────────────────────────────────────────────────────
async function main() {
  const res = await fetch("devices/registry.json");
  state.registry = await res.json();
  state.enclosures = await fetch("devices/enclosures.json")
    .then((r) => r.json())
    .catch(() => null);
  state.build = await fetch("devices/build.json")
    .then((r) => r.json())
    .catch(() => null);
  state.catalog = await fetch("devices/catalog.json")
    .then((r) => r.json())
    .catch(() => null);
  state.boards = await fetch("devices/boards.json")
    .then((r) => r.json())
    .catch(() => null);
  state.assembly = await fetch("devices/assembly.json")
    .then((r) => r.json())
    .catch(() => null);
  $("#fw-train").textContent = `firmware train ${state.registry.fw_train}`;
  mountFinishPicker();
  renderCards();
  // The models cross-fade to the active finish live (role-tagged shell parts
  // read it per-frame — no rebuild). On a first visit, run the ambient
  // showcase: a slow, calm cycle through the palette that demos customisation
  // until the visitor picks a swatch. Honor a saved choice and reduced motion.
  if (!hasUserChoice() && !matchMedia("(prefers-reduced-motion: reduce)").matches) {
    startFinishShowcase();
  }
  // Deep link from the chooser: fleet.html#<device-id> opens its sheet.
  const target = decodeURIComponent(location.hash.slice(1));
  const dev = state.registry.devices.find((d) => d.id === target);
  if (dev) openSheet(dev);
}

// the finish picker rides in the hero, above the fineprint line
function mountFinishPicker() {
  const hero = $("#hero");
  if (!hero || hero.querySelector(".finish-bar")) return;
  const bar = el("div", "finish-bar");
  bar.append(el("span", "finish-bar-cap", "Finish"), buildFinishPicker());
  const fine = hero.querySelector(".fineprint");
  hero.insertBefore(bar, fine || null);
}

function renderCards() {
  const grid = $("#cards");
  grid.innerHTML = "";
  for (const dev of state.registry.devices) {
    const card = el("button", "card");
    card.setAttribute("aria-label", `Open ${dev.name}`);
    const cv = el("canvas", "card-3d");
    const name = el("div", "card-name", dev.name);
    const tag = el("div", "card-tag", dev.tagline);
    const persona = dev.persona ? el("div", "card-persona", `${dev.persona.name} · ${dev.persona.specialty}`) : null;
    const chips = el("div", "card-chips");
    chips.append(
      el("span", "chip", dev.kind === "display" ? "shows"
        : dev.kind === "concept" ? "would sense" : "senses"),
      el("span", "chip chip-dim", dev.board.split("+")[0].trim())
    );
    if (dev.emulator) chips.append(el("span", "chip chip-live", "live firmware"));
    if (dev.kind === "concept") chips.append(el("span", "chip chip-soon", "coming soon"));
    card.append(cv, name, tag);
    if (persona) card.append(persona);
    card.append(chips);
    grid.append(card);

    const scene = new DeviceScene(cv, null);
    (BUILDERS[dev.id] || BUILDERS["canary-wap"])(scene);
    upgradeRealShape(scene, dev.id);
    scene.start();
    state.cards.set(dev.id, { scene, dev });

    card.addEventListener("click", () => openSheet(dev));
  }
}

// ── device sheet ────────────────────────────────────────────────────────
async function openSheet(dev) {
  closeSheet();
  const root = $("#sheet-root");
  root.innerHTML = "";
  root.classList.add("open");
  document.body.classList.add("locked");

  const sheet = el("div", "sheet");
  const grab = el("div", "grab");
  const head = el("header", "sheet-head");
  head.append(
    el("h2", null, dev.name),
    el("p", "muted", dev.tagline),
    (() => {
      const b = el("button", "close", "✕");
      b.addEventListener("click", closeSheet);
      return b;
    })()
  );

  const stage = el("div", "stage");
  const three = el("canvas", "stage-3d");
  stage.append(three);

  const side = el("div", "side");
  sheet.append(grab, head, stage, side);
  root.append(sheet);
  requestAnimationFrame(() => sheet.classList.add("up"));

  const ctx = {
    dev,
    scene: new DeviceScene(three, null),
    emu: null,
    fleet: null,
    nvsImage: new Map(),
    dispose: [],
  };
  state.sheet = ctx;
  (BUILDERS[dev.id] || BUILDERS["canary-wap"])(ctx.scene);
  upgradeRealShape(ctx.scene, dev.id);
  ctx.scene.start();

  if (dev.kind === "concept") buildConceptSheet(ctx, side);
  else if (dev.emulator) await buildDisplaySheet(ctx, side, stage);
  else buildWitnessSheet(ctx, side);
}

// ── concept sheet: a coming-soon teaser gets research, not theater ─────
// No LED grammar, no joining flow, no enclosure lab — those would imply
// shipped behavior. A concept shows its idea, the researched radio facts
// (each with a source), the staged firmware plan, and the request door.
function buildConceptSheet(ctx, side) {
  const dev = ctx.dev;
  const c = dev.concept || {};
  const tabs = el("nav", "tabs");
  const panel = el("div", "panel");
  side.append(tabs, panel);

  const requestDoor = () => {
    const a = el("a", "primary small door", "→ request it (opens a GitHub issue)");
    a.href = "https://github.com/kmay89/securaCV/issues/new?title=" +
      encodeURIComponent(c.request_title || `Concept request: ${dev.name}`) +
      "&body=" + encodeURIComponent(c.request_body || "");
    a.target = "_blank";
    a.rel = "noopener";
    return a;
  };

  // Tab labels and the plan panel's intro/link default to the original
  // Fence Guard copy (unchanged for that entry) but are overridable per
  // concept — "The radio" and a firmware stub link are mesh-specific and
  // don't fit e.g. a camera concept with no firmware stub at all.
  const radioTabLabel = c.radio_tab_label || "The radio";
  const planTabLabel = c.plan_tab_label || "Firmware plan";
  const planIntro = c.plan_intro ||
    "Staged as a pending stub in the firmware tree — requirements first, code later, honestly labeled.";
  const planDoc = c.plan_doc || {
    label: "read the stub → firmware/projects/canary-fence-guard",
    url: "https://github.com/kmay89/securaCV/blob/main/firmware/projects/canary-fence-guard/README.md",
  };

  const views = {
    Persona: () => personaView(dev),
    "The idea": () => {
      const w = el("div");
      w.append(el("p", "body", c.idea || dev.tagline));
      for (const p of c.points || []) {
        const line = el("p", "muted");
        line.append(el("strong", null, p.k + " — "), document.createTextNode(p.v));
        w.append(line);
      }
      w.append(el("p", "muted",
        "This is a concept card: nothing on it ships today, and it never pretends otherwise."));
      w.append(requestDoor());
      return w;
    },
    [radioTabLabel]: () => {
      const w = el("div", "specs");
      const dl = el("dl");
      for (const r of c.radio || []) dl.append(el("dt", null, r.k), el("dd", null, r.v));
      w.append(dl);
      if (c.sources?.length) {
        const links = el("p", "muted");
        links.append("Researched from: ");
        c.sources.forEach((s, i) => {
          const a = el("a", null, s.name);
          a.href = s.url;
          a.target = "_blank";
          a.rel = "noopener";
          links.append(i ? " · " : "", a);
        });
        w.append(links);
      }
      return w;
    },
    [planTabLabel]: () => {
      const w = el("div");
      w.append(el("p", "muted", planIntro));
      const ol = el("ol", "concept-plan");
      for (const step of c.plan || []) ol.append(el("li", null, step));
      w.append(ol);
      const a = el("a", null, planDoc.label);
      a.href = planDoc.url;
      a.target = "_blank";
      a.rel = "noopener";
      w.append(el("p", "muted"), a);
      return w;
    },
    Specs: () => specsView(dev),
  };
  for (const name of Object.keys(views)) {
    const b = el("button", "tab", name);
    b.addEventListener("click", () => {
      for (const t of tabs.children) t.classList.remove("on");
      b.classList.add("on");
      panel.innerHTML = "";
      panel.append(views[name]());
    });
    tabs.append(b);
  }
  tabs.children[0].click();
}

function closeSheet() {
  const c = state.sheet;
  if (!c) return;
  for (const d of c.dispose) try { d(); } catch {}
  c.scene?.stop();
  c.emu?.stopFleetHeartbeat();
  state.sheet = null;
  $("#sheet-root").classList.remove("open");
  $("#sheet-root").innerHTML = "";
  document.body.classList.remove("locked");
}

// ── display sheet: live firmware ────────────────────────────────────────
async function buildDisplaySheet(ctx, side, stage) {
  const dev = ctx.dev;

  // The glass: the emulator's native canvas — touchable, and the very
  // same pixels the 3D screen textures from. The shade above it is the
  // bench's honesty layer: rail down or ROM parked → the panel says so.
  const glassWrap = el("div", "glass-wrap");
  const glassHolder = el("div", "glass-holder");
  const glass = el("canvas", "glass");
  glass.style.width = dev.glass.round ? "232px" : "464px";
  if (dev.glass.round) glass.classList.add("round");
  const shade = el("div", "glass-shade");
  if (dev.glass.round) shade.classList.add("round");
  glassHolder.append(glass, shade);
  glassWrap.append(glassHolder, el("div", "glass-hint", "this is the panel — touch it"));
  stage.append(glassWrap);
  const setShade = (text) => {
    shade.textContent = text || "";
    shade.classList.toggle("on", !!text);
  };

  const tabs = el("nav", "tabs");
  const panel = el("div", "panel");
  side.append(tabs, panel);

  const serialLog = el("pre", "log");
  const wireLog = el("div", "wirelog");
  const noteLine = el("div", "note");

  // Web Audio chime — a persistent square "piezo" voice whose frequency AND
  // gain follow the firmware's real per-control-tick writes (onTone), so the
  // in-browser display reproduces the actual Canary Voice envelope, glissando,
  // warble, and volume model — not a flat beep. Created lazily on first gesture
  // (autoplay policy); muteable and remembered across visits.
  let audio = null, voiceOsc = null, voiceGain = null;
  let soundMuted = false;
  try { soundMuted = localStorage.getItem("canary-sound") === "off"; } catch {}
  const audioResume = () => { try { if (audio && audio.state === "suspended") audio.resume(); } catch {} };
  const ensureVoice = () => {
    if (!audio) audio = new AudioContext();
    if (!voiceOsc) {
      voiceOsc = audio.createOscillator();
      voiceOsc.type = "square";
      voiceGain = audio.createGain();
      voiceGain.gain.value = 0;
      const lp = audio.createBiquadFilter();
      lp.type = "lowpass"; lp.frequency.value = 5600; lp.Q.value = 0.4;
      voiceOsc.connect(voiceGain).connect(lp).connect(audio.destination);
      voiceOsc.start();
    }
  };
  // onTone(freqHz, gain0to1): frequency + the note's duty-derived level. freq 0
  // is a rest/end (silence). MASTER keeps the square wave civil next to real
  // audio; a short gain ramp de-zips the per-tick updates.
  const MASTER = 0.22;
  const tone = (f, gain) => {
    if (soundMuted) return;
    // A hidden page holds the bird's beak shut: release any held note and take
    // no new ones. The firmware keeps ticking; a background tab or window must
    // never sing to an empty room.
    if (document.hidden) { if (voiceGain) { try { voiceGain.gain.setTargetAtTime(0, audio.currentTime, 0.004); } catch {} } return; }
    try {
      ensureVoice(); audioResume();
      const t = audio.currentTime;
      if (f > 0) {
        voiceOsc.frequency.setValueAtTime(f, t);
        voiceGain.gain.setTargetAtTime(Math.max(0, Math.min(1, gain || 0)) * MASTER, t, 0.003);
      } else {
        voiceGain.gain.setTargetAtTime(0, t, 0.004);
      }
    } catch {}
  };
  const setSound = (on) => {
    soundMuted = !on;
    try { localStorage.setItem("canary-sound", on ? "on" : "off"); } catch {}
    if (!on && voiceGain) { try { voiceGain.gain.setTargetAtTime(0, audio.currentTime, 0.01); } catch {} }
  };
  // The per-tick gate above only cuts a note at the NEXT firmware write; if
  // ticks pause while hidden, a held note would ring on. This is the
  // deterministic cut the moment the page leaves the screen. Named and
  // removed on dispose — each sheet builds its own audio closure, so a
  // listener left behind would stack up (and pin that closure) on every
  // open/close cycle.
  const onHidden = () => {
    if (document.hidden && voiceGain) { try { voiceGain.gain.setTargetAtTime(0, audio.currentTime, 0.01); } catch {} }
  };
  document.addEventListener("visibilitychange", onHidden);
  ctx.dispose.push(() => {
    document.removeEventListener("visibilitychange", onHidden);
    // and full quiet on close: this sheet's context is unreachable after
    // teardown, so a note held mid-close would otherwise ring forever
    try { if (voiceGain) voiceGain.gain.value = 0; } catch {}
    try { if (voiceOsc) voiceOsc.stop(); } catch {}
    try { if (audio) audio.close(); } catch {}
  });
  // A small, unobtrusive corner toggle — the chime is meaningful and infrequent
  // (boot, alerts, the touch you make), but the room is yours.
  (() => {
    if (document.getElementById("canary-sound-toggle")) return;
    // Its rules live in canary-local.css (#canary-sound-toggle). They used to
    // be a <style> element created here — which is inline style, and the
    // page's CSP has no 'unsafe-inline' (canary-local/tools/gen_csp.py).
    const btn = el("button", null);
    btn.id = "canary-sound-toggle";
    btn.type = "button";
    btn.title = "Display sound";
    btn.setAttribute("aria-label", "Toggle display sound");
    const paint = () => { btn.textContent = soundMuted ? "🔇" : "🔊"; btn.setAttribute("aria-pressed", String(!soundMuted)); };
    btn.addEventListener("click", () => { setSound(soundMuted); paint(); if (!soundMuted) { ensureVoice(); audioResume(); } });
    paint();
    document.body.append(btn);
  })();

  // The page ships only the watch/dash twins as <script> tags; every other
  // twin (nightstand, touch169, whatever comes next) loads on demand from
  // the registry's module path — the card, not the page, knows which
  // firmware it needs. A factory already on window (script tag) no-ops.
  if (!window[dev.emulator.factory]) {
    await new Promise((resolve) => {
      const s = document.createElement("script");
      s.src = dev.emulator.module;
      s.onload = resolve;
      s.onerror = resolve;  // fall through: the boot path reports a missing factory honestly
      document.head.append(s);
    });
  }
  const factory = window[dev.emulator.factory];
  const serialAppend = (t) => {
    serialLog.textContent = (serialLog.textContent + t).slice(-24000);
    serialLog.scrollTop = serialLog.scrollHeight;
  };
  const wait = (ms) => new Promise((r) => setTimeout(r, ms));
  // One boot at a time: a second click mid-boot would race two async
  // boots against the shared ctx.emu (the earlier one resuming against a
  // half-wired replacement — TypeError / duplicate fleet, review catch).
  let bootBusy = false;
  const boot = async (opts = {}) => {
    if (bootBusy) return;
    // The bench owns the rail: no power (or a ROM parked in download
    // mode) means no boot, no matter who asks.
    if (ctx.bench && !ctx.bench.canBoot()) return;
    bootBusy = true;
    try {
      await bootInner(opts);
    } finally {
      bootBusy = false;
    }
  };
  // Power-plane boots queue behind an in-flight one instead of being
  // dropped (rapid cable toggling must not strand a powered board dark);
  // only the newest request survives the wait.
  let bootReq = 0;
  const bootWhenFree = async () => {
    const my = ++bootReq;
    while (bootBusy) await wait(150);
    if (my !== bootReq) return;
    await boot({ preserve: true });
  };
  const bootInner = async (opts = {}) => {
    setShade(null);
    // Boot is a user gesture; prime the audio voice now so the firmware's
    // power-on chirp isn't swallowed by a still-suspended AudioContext.
    if (!soundMuted) { try { ensureVoice(); audioResume(); } catch {} }
    ctx.emu = new CanaryEmulator(factory, {
      canvas: glass,
      onSerial: serialAppend,
      onDisplayReady: () => {},
      onFrame: () => {},
      onBacklight: (glow) => {
        ctx.scene.setGlow(glow);
        glass.style.filter = `brightness(${Math.max(0.06, glow)})`;
      },
      onTone: tone,
      onMqtt: (m) => {
        const row = el("div", `wire wire-${m.dir}`);
        row.append(
          el("span", "wire-dir", { in: "→dev", out: "dev→", sub: "sub", connect: "link", lwt: "LWT", net: "net" }[m.dir] || m.dir),
          el("span", "wire-topic", m.topic),
          el("span", "wire-pay", String(m.payload).slice(0, 110))
        );
        wireLog.prepend(row);
        while (wireLog.children.length > 250) wireLog.lastChild.remove();
      },
      onNetEvent: (k, d) => {
        const row = el("div", "wire wire-net");
        row.append(el("span", "wire-dir", "net"), el("span", "wire-topic", k), el("span", "wire-pay", d));
        wireLog.prepend(row);
      },
      onNvsWrite: (ns, key, hexVal) => ctx.nvsImage.set(`${ns}/${key}`, hexVal),
      onReboot: async () => {
        ctx.emu.retire();
        if (ctx.bench?.bootHeld) {
          // Even a software reset re-samples the straps — BOOT held low
          // parks the ROM in download mode instead of rebooting the app.
          ctx.bench.firmwareRestart();
          return;
        }
        note("device rebooted — booting again with its memory intact");
        serialAppend("\n\n※ ── power cycle ── ※\n\n" + romBanner("swreset"));
        await boot({ preserve: true });
      },
    });
    ctx.scene.src = glass; // 3D screen textures from the live panel
    // A preserved image (reboot / bench power event) is preseeded before
    // power-on, so setup() always reads the surviving flash — never a
    // race against the firmware's resume. A "meet again" reboot must not
    // restore the remembered hello — that memory is exactly what the
    // button un-remembers.
    const img = !opts.preserve
      ? null
      : opts.firstMeeting
        ? new Map([...ctx.nvsImage].filter(([k]) => !k.startsWith("scv-hello/")))
        : ctx.nvsImage;
    await ctx.emu.start({
      provisioned: true,
      firstMeeting: !!opts.firstMeeting,
      seed: 20260719,
      nvsImage: img,
    });
    ctx.emu.setLocalHour(10);
    // The fleet outlives display reboots: real witnesses keep their keys
    // when a display power-cycles, so the same SimWitness objects (same
    // WebCrypto keypairs, same chain state) re-publish after a reboot —
    // otherwise the preserved TOFU pins would flag every sibling as
    // FAILED, which is exactly the honesty bug the page teaches against.
    ctx.fleet ||= demoFleet();
    for (const w of ctx.fleet) await ctx.emu.addWitness(w);
    ctx.emu.startFleetHeartbeat(15000);
    ctx.bootAt = performance.now();
  };

  const note = (t) => { noteLine.textContent = t || ""; };

  // ── The power plane (emulator/web/bench.js) ───────────────────────────
  // Cable, battery, switch, buttons, hardwired lights — the physical
  // layer the firmware can't see, driving the same boot/kill machinery
  // the rail drives on silicon. NVS is flash: it rides through every
  // power event untouched, which is why every bench boot preserves it.
  const killPower = (cause) => {
    ctx.emu?.retire();
    ctx.scene.setGlow(0);
    const g2d = glass.getContext("2d");
    g2d.fillStyle = "#000";
    g2d.fillRect(0, 0, glass.width, glass.height);
    glass.style.filter = "";
    setShade("no power");
    serialAppend(`\n⏚ ── rail down (${cause}) — serial port gone ── ⏚\n`);
  };
  const enterDownload = () => {
    ctx.scene.setGlow(0);
    const g2d = glass.getContext("2d");
    g2d.fillStyle = "#000";
    g2d.fillRect(0, 0, glass.width, glass.height);
    setShade("waiting for download");
    serialAppend("\n※ ── reset, BOOT held low ── ※\n\n" + romBanner("download"));
  };
  ctx.bench = dev.bench
    ? new BenchPower(dev.bench, {
        onLog: (line) => {
          serialAppend(`\n⏚ bench │ ${line}\n`);
          note(line);
        },
        onPower: async (up, cause) => {
          if (!up) {
            killPower(cause);
            return;
          }
          if (ctx.bench.mode === "download") {
            setShade(null);
            enterDownload();
            return;
          }
          serialAppend("\n※ ── power restored ── ※\n\n" + romBanner("poweron"));
          await bootWhenFree();
        },
        onReset: async (kind) => {
          ctx.emu?.retire();
          if (kind === "download") {
            enterDownload();
            return;
          }
          serialAppend("\n※ ── reset ── ※\n\n" + romBanner("reset"));
          await bootWhenFree();
        },
      })
    : null;
  if (ctx.bench) {
    const benchTick = setInterval(() => ctx.bench.tick(400), 400);
    ctx.dispose.push(() => clearInterval(benchTick));
  }

  serialAppend(romBanner("poweron"));
  await boot();

  const guideCtx = {
    emu: ctx.emu,
    fleet: ctx.fleet,
    setHour: (h) => ctx.emu.setLocalHour(h),
    note,
    meetAgain: async () => {
      if (ctx.bench && !ctx.bench.canBoot()) {
        note("no power on the bench — restore power first (Bench tab)");
        return;
      }
      note("rebooting for a first meeting — the bird will introduce itself");
      serialAppend("\n\n※ ── power cycle (first meeting) ── ※\n\n" + romBanner("poweron"));
      ctx.emu.retire();
      await boot({ preserve: true, firstMeeting: true });
    },
  };
  // keep guideCtx live across reboots
  const guideProxy = new Proxy(guideCtx, {
    get: (t, k) =>
      k === "emu" ? ctx.emu : k === "fleet" ? ctx.fleet : k === "bench" ? ctx.bench : t[k],
  });

  const views = {
    Persona: () => personaView(dev),
    Tour: () => tourView(guideProxy, DISPLAY_TOUR, noteLine),
    "Fix it": () => fixView(guideProxy, DISPLAY_FIXES, noteLine),
    "Try it": () => tryView(guideProxy, noteLine),
    ...(ctx.bench ? { Bench: () => benchView(ctx, guideProxy, noteLine) } : {}),
    Wire: () => wireView(serialLog, wireLog),
    Enclosure: () => buildEnclosureLab(state.enclosures, dev.id, state.build, state.catalog),
    ...(state.boards?.device_board?.[dev.id]
      ? { Board: () => buildBoardLab(state.boards, dev.id) } : {}),
    ...(state.assembly?.devices?.[dev.id]
      ? { Assemble: () => buildAssemblyLab(state.assembly, state.build, dev.id) } : {}),
    "Build it": () => buildBuildIt(state.build, dev),
    Specs: () => specsView(dev),
  };
  let active = null;
  for (const name of Object.keys(views)) {
    const b = el("button", "tab", name);
    b.addEventListener("click", () => {
      for (const t of tabs.children) t.classList.remove("on");
      b.classList.add("on");
      panel.innerHTML = "";
      panel.append(views[name]());
      active = name;
    });
    tabs.append(b);
  }
  tabs.children[0].click();
}

function tourView(ctx, steps, noteLine) {
  const wrap = el("div", "tour");
  let i = 0;
  const title = el("h3");
  const body = el("p", "body");
  const nav = el("div", "tour-nav");
  const prev = el("button", "ghost", "‹ back");
  const count = el("span", "muted");
  const next = el("button", "primary", "next ›");
  nav.append(prev, count, next);
  wrap.append(title, body, noteLine, nav);

  const show = async (n) => {
    i = Math.max(0, Math.min(steps.length - 1, n));
    const s = steps[i];
    title.textContent = s.title;
    body.textContent = s.body;
    count.textContent = `${i + 1} / ${steps.length}`;
    prev.disabled = i === 0;
    next.textContent = i === steps.length - 1 ? "finish ✓" : "next ›";
    noteLine.textContent = "";
    if (s.stage) await s.stage(ctx);
  };
  prev.addEventListener("click", () => show(i - 1));
  next.addEventListener("click", () => (i === steps.length - 1 ? show(0) : show(i + 1)));
  show(0);
  return wrap;
}

function fixView(ctx, fixes, noteLine) {
  const wrap = el("div", "fixes");
  wrap.append(el("p", "muted", "Pick what you're seeing. Each step stages this emulator into that exact state — compare it with your real glass."));
  for (const f of fixes) {
    const d = el("details", "fix");
    const s = el("summary", null, f.symptom);
    d.append(s);
    f.steps.forEach((st, idx) => {
      const step = el("div", "fix-step");
      const h = el("h4", null, `${idx + 1}. ${st.title}`);
      const b = el("p", "body", st.body);
      step.append(h, b);
      if (st.onDevice) {
        const od = el("p", "ondevice");
        od.append(el("strong", null, "On the real device: "), document.createTextNode(st.onDevice));
        step.append(od);
      }
      if (st.stage) {
        const btn = el("button", "primary small", "show me on the glass");
        btn.addEventListener("click", async () => {
          noteLine.textContent = "";
          await st.stage(ctx);
        });
        step.append(btn);
      }
      d.append(step);
    });
    wrap.append(d);
  }
  wrap.append(noteLine);
  return wrap;
}

function tryView(ctx, noteLine) {
  const wrap = el("div", "try");
  const mk = (label, fn, cls = "ghost") => {
    const b = el("button", cls, label);
    b.addEventListener("click", () => fn());
    return b;
  };

  // ── The style rail: the firmware's own Character ring, in ring order ──
  // Names and captions read back from the wasm (never hardcoded), so the
  // page can only ever demo what the firmware can actually do.
  const styleWrap = el("div", "style-rail");
  styleWrap.append(el("p", "muted",
    "Its Character — seven curated ages of technology. The look, the type, " +
    "the bird's temperament, the words it uses when all is calm. Alarms " +
    "never restyle; night outranks every look. The choice persists through " +
    "reboots, exactly like the real glass."));
  const rail = el("div", "style-row");
  const paint = async () => {
    const active = await ctx.emu.activeCharacter?.();
    for (const b of rail.children) {
      const on = Number(b.dataset.id) === active;
      b.classList.toggle("on", on);
      // The selection ring glows in the chip's own accent.
      b.style.boxShadow = on ? `0 0 0 1.5px ${b.dataset.accent} inset` : "";
      b.style.borderColor = on ? b.dataset.accent : "";
      b.style.borderLeftColor = b.dataset.accent;
    }
  };
  (async () => {
    for (const c of (await ctx.emu.characterRing?.()) ?? []) {
      const b = el("button", "style-chip");
      b.dataset.id = c.id;
      // Each chip wears its Character — ground, ink, accent, straight
      // from the firmware table. The rail IS the ring, not a menu of it.
      b.style.background = c.bg;
      b.style.borderLeft = `4px solid ${c.accent}`;
      const nm = el("strong", null, c.name);
      nm.style.color = c.text;
      const cap = el("span", null, c.caption);
      cap.style.color = c.text;
      cap.style.opacity = "0.62";
      b.append(nm, cap);
      b.dataset.accent = c.accent;
      b.addEventListener("click", () => {
        ctx.emu.applyCharacter(c.id);
        noteLine.textContent = `${c.name} — ${c.caption}`;
        paint();
      });
      rail.append(b);
    }
    paint();
  })();
  styleWrap.append(rail);
  styleWrap.append(mk("meet the bird again (first boot)",
    () => ctx.meetAgain?.(), "primary"));

  // ── The storyline (display_modes.md §demo): the mode system's scripted
  // household, played through THIS page's staged witnesses into the real
  // firmware — the same beats, seconds and severities the on-device demo
  // gear feeds its own faces (the table is drift-locked to the firmware in
  // tests/mode.test.js). Shown at 6× wall speed; here the events even ride
  // signed chains where the browser has Ed25519, because the staged
  // witnesses really sign. Cast mapping is presentational: the beats'
  // front door / garage land on their namesakes, the indoor stirs on the
  // nursery.
  // The player state rides ctx (not this closure): the view can be rebuilt
  // mid-story, and a rebuilt button must find — and be able to stop — the
  // running lap instead of stacking a second one.
  ctx.story ||= { timer: null, clock: 0 };
  const STORY_CAST = [0, 1, 2, 1]; // beat witness -> staged fleet index
  const stopStory = () => {
    if (ctx.story.timer) clearInterval(ctx.story.timer);
    ctx.story.timer = null;
    ctx.story.clock = 0;
    ctx.emu.witnessTamper(ctx.fleet[2], false); // never leave a staged tamper
    storyBtn.textContent = "▶ play the demo storyline";
  };
  const storyBtn = mk("▶ play the demo storyline", () => {
    if (ctx.story.timer) {
      stopStory();
      noteLine.textContent = "storyline stopped — the household is yours again.";
      return;
    }
    storyBtn.textContent = "⏸ storyline playing (6×) — tap to stop";
    ctx.story.timer = setInterval(() => {
      const prev = ctx.story.clock;
      ctx.story.clock = (ctx.story.clock + 1) % DEMO.LOOP_S;
      for (const i of beatsBetween(prev, ctx.story.clock)) {
        const b = DEMO.BEATS[i];
        const w = ctx.fleet[STORY_CAST[b.witness]];
        if (b.intent === "tamper") {
          ctx.emu.witnessTamper(w, true, "tamper_contact");
        } else if (b.event === "cleared") {
          ctx.emu.witnessTamper(w, false);
          ctx.emu.witnessEvent(w, "cleared");
        } else {
          ctx.emu.witnessEvent(w, b.event);
        }
        noteLine.textContent =
          `storyline ${b.atS}s — ${w.name}: ${b.event.replaceAll("_", " ")}` +
          ` [${b.intent}]` +
          (b.intent === "alert" || b.intent === "tamper"
            ? " — hold the glass to acknowledge"
            : "");
      }
    }, 1000 / 6);
  }, "primary");
  if (ctx.story.timer) {
    storyBtn.textContent = "⏸ storyline playing (6×) — tap to stop";
  }

  const grid = el("div", "try-grid");
  grid.append(
    storyBtn,
    mk("Wi-Fi down", () => ctx.emu.setWifi(false)),
    mk("Wi-Fi up", () => ctx.emu.setWifi(true)),
    mk("broker down", () => ctx.emu.setBroker(false)),
    mk("broker up", () => ctx.emu.setBroker(true)),
    mk("stage 10:00", () => ctx.setHour(10)),
    mk("stage 23:00 (night)", () => ctx.setHour(23)),
    mk("time ×60", () => ctx.emu.setTimeScale(60)),
    mk("time ×1", () => ctx.emu.setTimeScale(1)),
    mk("motion at the door", () => ctx.emu.witnessEvent(ctx.fleet[0], "presence_detected")),
    mk("tamper the garage", () => ctx.emu.witnessTamper(ctx.fleet[2], true, "enclosure_tamper"), "danger"),
    mk("clear tamper", () => ctx.emu.witnessTamper(ctx.fleet[2], false)),
    mk("silence the nursery", () => ctx.emu.witnessSilence(ctx.fleet[1])),
    mk("revive the nursery", () => ctx.emu.witnessRevive(ctx.fleet[1])),
    mk("sibling display acks", () => ctx.emu.householdAck())
  );
  wrap.append(
    el("p", "muted",
      "Drag the device. Touch its glass: tap pages, hold to acknowledge. " +
      "Then break the household on purpose — the glass must never lie about it."),
    styleWrap,
    grid,
    noteLine
  );
  return wrap;
}

// ── The Bench: the physical test bench around the board ─────────────────
// Cable, battery, switch, BOOT/RESET, and the hardwired lights — all
// interactable, all driving (or refusing to drive) the live firmware.
// Plus live diagnostics and the symptom-first debug flows.
function benchView(ctx, guideCtx, noteLine) {
  const bench = ctx.bench;
  const profile = ctx.dev.bench;
  const wrap = el("div", "bench");
  wrap.append(
    el("p", "muted",
      "The layer the firmware can't see: power, straps, and the lights " +
      "wired past the chip. Pull the cable mid-frame, brown out the " +
      "battery, park the ROM in download mode — the glass and the serial " +
      "log react exactly as a real bench would."),
    el("p", "bench-ribbon",
      "Honesty: the mask-ROM banner is staged by the bench (its text is " +
      "verbatim ESP32-S3); everything after it is the real firmware. The " +
      "PWR/CHG/DONE lights are hardwired to the rail and charge chip — " +
      "no firmware here or on your desk can turn them off.")
  );

  // ── The lights ────────────────────────────────────────────────────────
  const ledRow = el("div", "bench-leds");
  const ledEls = new Map();
  for (const led of profile.leds || []) {
    const cell = el("div", "bench-led");
    cell.title = led.note || "";
    const dot = el("div", `bench-led-dot led-${led.color}`);
    cell.append(dot, el("span", "bench-led-label", led.label));
    ledEls.set(led.id, dot);
    ledRow.append(cell);
  }
  const ledCap = el("p", "bench-cap",
    (profile.leds || []).map((l) => `${l.label} — ${l.note}`).join("  ·  "));

  // ── The controls ──────────────────────────────────────────────────────
  const controls = el("div", "bench-controls");
  const chip = (label, title, onClick) => {
    const b = el("button", "bench-chip");
    if (title) b.title = title;
    const lab = el("strong", null, label);
    const st = el("span", "bench-chip-state");
    b.append(lab, st);
    b.addEventListener("click", () => {
      onClick();
      sync();
    });
    return { b, st };
  };

  const usb = chip("USB-C cable", profile.power?.usb, () => bench.setUsb(!bench.usb));
  const bat = chip("battery", profile.power?.battery, () => bench.setBattery(!bench.batteryFitted));
  const soc = el("div", "bench-soc");
  const socFill = el("div", "bench-soc-fill");
  soc.append(socFill);
  bat.b.append(soc);
  const sw = chip(profile.power?.switch?.label || "ON/OFF", profile.power?.switch?.note,
    () => bench.setSwitch(!bench.switchOn));
  sw.b.classList.add("bench-switch");
  const bootBtn = chip(
    (profile.buttons || []).find((b) => b.id === "boot")?.label || "BOOT",
    (profile.buttons || []).find((b) => b.id === "boot")?.note,
    () => bench.setBootHeld(!bench.bootHeld));
  const resetBtn = el("button", "bench-chip bench-momentary");
  resetBtn.title = (profile.buttons || []).find((b) => b.id === "reset")?.note || "";
  resetBtn.append(
    el("strong", null, (profile.buttons || []).find((b) => b.id === "reset")?.label || "RESET"),
    el("span", "bench-chip-state", "press"));
  resetBtn.addEventListener("click", () => {
    resetBtn.classList.add("pressed");
    setTimeout(() => resetBtn.classList.remove("pressed"), 180);
    bench.pressReset();
    sync();
  });
  const ff = el("button", "ghost small bench-ff", "battery time ×60");
  ff.title = "bench knob: fast-forward charging and draining (not virtual time)";
  ff.addEventListener("click", () => {
    bench.rate = bench.rate === 1 ? 60 : 1;
    sync();
  });
  controls.append(usb.b, bat.b, sw.b, bootBtn.b, resetBtn, ff);

  // ── Live diagnostics ──────────────────────────────────────────────────
  const diag = el("dl", "bench-diag");
  const diagRow = (k) => {
    const dd = el("dd", null, "—");
    diag.append(el("dt", null, k), dd);
    return dd;
  };
  const dPower = diagRow("power");
  const dMode = diagRow("mode");
  const dUp = diagRow("uptime");
  const dBl = diagRow("backlight");
  const dFlush = diagRow("frames flushed");
  const dLink = diagRow("Wi-Fi / broker");
  const dMqtt = diagRow("MQTT session");
  const dNvs = diagRow("NVS");
  const dFw = diagRow("firmware train");
  dFw.textContent = state.registry?.fw_train || "—";

  const sync = () => {
    usb.b.classList.toggle("on", bench.usb);
    usb.st.textContent = bench.usb ? "plugged" : "unplugged";
    bat.b.classList.toggle("on", bench.batteryFitted);
    bat.st.textContent = bench.batteryFitted ? `fitted · ${Math.round(bench.soc)}%` : "removed";
    soc.style.visibility = bench.batteryFitted ? "visible" : "hidden";
    socFill.style.width = `${Math.round(bench.soc)}%`;
    socFill.classList.toggle("low", bench.soc < 15);
    sw.b.classList.toggle("on", bench.switchOn);
    sw.st.textContent = bench.switchOn ? "ON" : "OFF";
    bootBtn.b.classList.toggle("on", bench.bootHeld);
    bootBtn.st.textContent = bench.bootHeld ? "held" : "released";
    ff.classList.toggle("on", bench.rate !== 1);
    const leds = bench.leds();
    for (const [id, dot] of ledEls) {
      dot.classList.toggle("on", leds[id] === "on");
      dot.classList.toggle("flicker", leds[id] === "flicker");
    }
    const src = bench.source();
    dPower.textContent =
      src === "usb"
        ? `USB${bench.batteryFitted ? ` (battery ${Math.round(bench.soc)}%${bench.soc < 100 ? ", charging" : ", full"})` : ""}`
        : src === "battery"
          ? `battery · ${Math.round(bench.soc)}%`
          : "none — rail down";
    dMode.textContent =
      bench.mode === "run" ? "app running"
        : bench.mode === "download" ? "download mode (ROM waiting for a flasher)"
          : "off";
  };

  const refresh = async () => {
    sync();
    if (bench.mode !== "run" || !ctx.emu?.c) {
      dUp.textContent = dBl.textContent = dFlush.textContent = dMqtt.textContent = "—";
      dLink.textContent = bench.mode === "off" ? "—" : "radio unpowered until the app runs";
      dNvs.textContent = `${ctx.nvsImage.size} keys (flash — survives all of this)`;
      return;
    }
    const s = Math.max(0, (performance.now() - (ctx.bootAt || performance.now())) / 1000);
    dUp.textContent = `${Math.floor(s / 60)}m ${String(Math.floor(s % 60)).padStart(2, "0")}s (wall)`;
    try {
      const night = await ctx.emu.c.nightDuty();
      const level = await ctx.emu.c.backlight();
      dBl.textContent = night >= 0
        ? `night floor · 13-bit duty ${night}/8191`
        : `day ladder · ${level}/255`;
      dFlush.textContent = String(await ctx.emu.c.flushCount());
      dMqtt.textContent = (await ctx.emu.c.mqttConnected()) ? "connected" : "down / reconnecting";
    } catch {
      /* mid-reboot: the module is being replaced — next tick catches up */
    }
    dLink.textContent =
      `${ctx.emu.linkState.wifi ? "Wi-Fi up" : "Wi-Fi DOWN"} · ${ctx.emu.linkState.broker ? "broker up" : "broker DOWN"}`;
    dNvs.textContent = `${ctx.nvsImage.size} keys (flash — survives all of this)`;
  };
  const timer = setInterval(() => {
    if (!wrap.isConnected) {
      clearInterval(timer);
      return;
    }
    refresh();
  }, 600);
  ctx.dispose.push(() => clearInterval(timer));

  // ── Debug mode: the symptom-first bench flows ─────────────────────────
  const trouble = el("details", "bench-trouble");
  trouble.append(el("summary", null, "Troubleshoot — the bench debug flows"));
  trouble.append(fixView(guideCtx, BENCH_FIXES, noteLine));

  wrap.append(ledRow, ledCap, controls, diag, trouble);
  refresh();
  return wrap;
}

function wireView(serialLog, wireLog) {
  const wrap = el("div", "wireview");
  const t = el("nav", "subtabs");
  const bSerial = el("button", "tab on", "serial console");
  const bWire = el("button", "tab", "MQTT wire");
  t.append(bSerial, bWire);
  const holder = el("div", "wire-holder");
  holder.append(serialLog);
  bSerial.addEventListener("click", () => {
    bSerial.classList.add("on"); bWire.classList.remove("on");
    holder.innerHTML = ""; holder.append(serialLog);
  });
  bWire.addEventListener("click", () => {
    bWire.classList.add("on"); bSerial.classList.remove("on");
    holder.innerHTML = ""; holder.append(wireLog);
  });
  wrap.append(
    el("p", "muted", "The same USB-CDC boot log and MQTT traffic you'd see on a bench — because it's the same firmware."),
    t, holder
  );
  return wrap;
}

function personaView(dev) {
  const p = dev.persona || {};
  const wrap = el("div", "persona-view");
  const hero = el("div", "persona-hero");
  if (p.accent) hero.style.setProperty("--persona-accent", p.accent_color || "var(--canary)");
  hero.append(
    el("p", "persona-kicker", p.specialty || "Canary"),
    el("h3", null, p.name || dev.name),
    el("p", "persona-story", p.story || dev.tagline)
  );
  wrap.append(hero);
  if (p.shape || p.accent) {
    const look = el("p", "persona-look");
    look.append(
      el("strong", null, "How to recognize this bird: "),
      document.createTextNode([p.shape, p.accent ? `accent: ${p.accent}` : ""].filter(Boolean).join(" · "))
    );
    wrap.append(look);
  }
  if (p.good_for?.length) {
    const list = el("ul", "persona-goodfor");
    for (const item of p.good_for) list.append(el("li", null, item));
    wrap.append(el("h4", null, "What this canary is good at"), list);
  }
  if (p.catchphrase) wrap.append(el("p", "persona-quote", `“${p.catchphrase}”`));
  wrap.append(el("p", "muted",
    "Designed to be memorable, not manipulative: each character explains the real sensing boundary in plain language and keeps the privacy promise visible."));
  return wrap;
}

function specsView(dev) {
  const wrap = el("div", "specs");
  const dl = el("dl");
  const row = (k, v) => {
    dl.append(el("dt", null, k), el("dd", null, v));
  };
  row("Board", dev.board);
  if (dev.glass) row("Glass", `${dev.glass.panel} · touch ${dev.glass.touch}`);
  if (dev.body_mm) {
    // the caliper row: mm · decimal inch · fractional inch, tap to cycle
    // (same persisted setting the Assemble tab's parts list uses)
    const b = dev.body_mm;
    const dd = el("dd", "unit-cycle");
    dd.title = "tap to cycle mm / inches / all";
    const paint = () => {
      const mode = localStorage.getItem("scv-units") || "all";
      const f = (mm) => fmtLen(mm, mode);
      dd.textContent = b.d
        ? `Ø ${f(b.d)}  ×  ${f(b.depth)} deep · stand ${b.stand_tilt_deg}°`
        : `${f(b.w)}  ×  ${f(b.h)}  ×  ${f(b.depth)} · stand ${b.stand_tilt_deg}°`;
    };
    dd.addEventListener("click", () => {
      const cur = localStorage.getItem("scv-units") || "all";
      localStorage.setItem("scv-units", UNIT_MODES[(UNIT_MODES.indexOf(cur) + 1) % UNIT_MODES.length]);
      paint();
    });
    paint();
    dl.append(el("dt", null, "Body"), dd);
  }
  if (dev.senses?.length) row("Senses", dev.senses.join(" · "));
  if (dev.shows?.length) row("Shows", dev.shows.join(" · "));
  row("mDNS", dev.network.mdns);
  row("Web", dev.network.web);
  row("MQTT", dev.network.mqtt);
  row("Status", dev.status);
  wrap.append(dl);
  const links = el("p", "muted");
  links.append("Deeper: ");
  const all = [dev.enclosure, ...(dev.docs || [])].filter(Boolean);
  all.forEach((d, i) => {
    const a = el("a", null, d);
    a.href = `https://github.com/kmay89/securaCV/blob/main/${d}`;
    a.target = "_blank";
    a.rel = "noopener";
    links.append(a);
    if (i < all.length - 1) links.append(" · ");
  });
  wrap.append(links);
  return wrap;
}

// ── witness sheet: decoder cards (no glass to emulate — LEDs + chirps) ──
function buildWitnessSheet(ctx, side) {
  const dev = ctx.dev;
  const tabs = el("nav", "tabs");
  const panel = el("div", "panel");
  side.append(tabs, panel);

  const views = {
    Persona: () => personaView(dev),
    "Lights": () => {
      const w = el("div");
      w.append(el("p", "muted",
        "No screen — the witness answers in a count-coded LED grammar you can read aloud. " +
        "Watch the light for 60 seconds; the pattern names the problem."));
      const led = el("div", "led-demo");
      const dot = el("div", "led-dot");
      const cap = el("div", "led-cap", "choose a pattern");
      led.append(dot, cap);
      w.append(led);
      const list = el("div", "led-list");
      for (const g of LED_GRAMMAR) {
        const b = el("button", "led-row");
        b.append(el("span", "led-pat", g.pattern), el("span", "led-mean", g.meaning));
        b.addEventListener("click", () => playLed(dot, cap, g));
        list.append(b);
      }
      w.append(list);
      return w;
    },
    "Sounds": () => {
      const w = el("div");
      w.append(el("p", "muted",
        "With the optional piezo fitted, the same answers are audible. The grammar is the " +
        "product: a dead battery must never sound like an intruder."));
      const list = el("div", "led-list");
      let audio = null;
      for (const c of CHIRP_GRAMMAR) {
        const b = el("button", "led-row");
        b.append(el("span", "led-pat", c.name), el("span", "led-mean", `${c.sound} — ${c.meaning}`));
        b.addEventListener("click", () => {
          audio ||= new AudioContext();
          playChirp(audio, c.name);
        });
        list.append(b);
      }
      w.append(list);
      return w;
    },
    Enclosure: () => buildEnclosureLab(state.enclosures, dev.id, state.build, state.catalog),
    ...(state.boards?.device_board?.[dev.id]
      ? { Board: () => buildBoardLab(state.boards, dev.id) } : {}),
    ...(state.assembly?.devices?.[dev.id]
      ? { Assemble: () => buildAssemblyLab(state.assembly, state.build, dev.id) } : {}),
    "Build it": () => buildBuildIt(state.build, dev),
    "Joining": () => {
      const w = el("div", "joining");
      w.append(
        el("p", "body",
          "Power it on and point it at a display showing an Add-a-Canary code — " +
          "an unprovisioned canary with a camera auto-scans for 60-second windows, " +
          "forever. No phone, no session, no typing."),
        el("p", "body",
          "The code is SCV1: your Wi-Fi, the hub address, and a single-use token " +
          "that dies in 10 minutes. Every field is treated as hostile input — " +
          "length-capped, validated, copied, never interpreted."),
        el("p", "body",
          dev.id === "canary-sense"
            ? "This canary has no camera: it joins over BLE Improv with the display as commissioner, or through the phone captive portal."
            : "Prefer the phone? Join the device's own setup network and the captive portal walks you through — same grammar, different door.")
      );
      return w;
    },
    Specs: () => specsView(dev),
  };
  for (const name of Object.keys(views)) {
    const b = el("button", "tab", name);
    b.addEventListener("click", () => {
      for (const t of tabs.children) t.classList.remove("on");
      b.classList.add("on");
      panel.innerHTML = "";
      panel.append(views[name]());
    });
    tabs.append(b);
  }
  tabs.children[0].click();
}

let ledTimer = null;
function playLed(dot, cap, g) {
  cap.textContent = `${g.pattern} — ${g.meaning}`;
  if (ledTimer) { clearTimeout(ledTimer); ledTimer = null; }
  const seq = ledSequence(g.pattern);
  let i = 0;
  const step = () => {
    if (i >= seq.length) { i = 0; }
    const [on, ms] = seq[i++];
    dot.classList.toggle("on", on);
    ledTimer = setTimeout(step, ms);
  };
  step();
}

function playChirp(audio, name) {
  const play = (freq, at, ms, gain = 0.05) => {
    const o = audio.createOscillator();
    const g = audio.createGain();
    o.type = "square";
    o.frequency.value = freq;
    g.gain.value = gain;
    o.connect(g).connect(audio.destination);
    o.start(audio.currentTime + at / 1000);
    o.stop(audio.currentTime + (at + ms) / 1000);
  };
  switch (name) {
    case "CONFIRM": play(1800, 0, 90); break;
    case "SUCCESS": play(523, 0, 110); play(659, 130, 110); play(784, 260, 160); break;
    case "ERROR": play(880, 0, 120); play(660, 140, 120); play(440, 280, 200); break;
    case "ALERT": play(1200, 0, 90); play(1600, 120, 90); play(2000, 240, 140); break;
    case "TAMPER": for (let i = 0; i < 5; i++) play(3000, i * 160, 80, 0.06); break;
    case "SELFTEST_OK": play(2093, 0, 70, 0.02); play(2637, 90, 90, 0.02); break;
  }
}

main();
