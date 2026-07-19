// canary-local/assets/app.js — the canary.local page.
//
// A pairing-card gallery of the Canary family. Each card is a live 3D
// object; opening a display's card boots the REAL firmware (wasm) behind
// its glass and offers three ways in: a guided Tour, symptom-first
// Fix-it flows, and free play with the LAN switches. Witness devices
// (no glass) get their decoder cards — LED grammar, chirp meanings,
// setup path. Everything works offline; nothing phones anywhere.

import { DeviceScene, BUILDERS } from "./scene3d.js";
import { upgradeRealShape } from "./real-shapes.js";
import { buildEnclosureLab } from "./enclosure-lab.js";
import { buildBuildIt } from "./build-it.js";
import { buildBoardLab } from "./board-lab.js";
import { buildAssemblyLab } from "./assembly-lab.js";
import { CanaryEmulator, demoFleet } from "../emulator/web/emu-shell.js";
import { BenchPower, romBanner } from "../emulator/web/bench.js";
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
  state.boards = await fetch("devices/boards.json")
    .then((r) => r.json())
    .catch(() => null);
  state.assembly = await fetch("devices/assembly.json")
    .then((r) => r.json())
    .catch(() => null);
  $("#fw-train").textContent = `firmware train ${state.registry.fw_train}`;
  renderCards();
  // Deep link from the chooser: index.html#<device-id> opens its sheet.
  const target = decodeURIComponent(location.hash.slice(1));
  const dev = state.registry.devices.find((d) => d.id === target);
  if (dev) openSheet(dev);
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
    const chips = el("div", "card-chips");
    chips.append(
      el("span", "chip", dev.kind === "display" ? "shows"
        : dev.kind === "concept" ? "would sense" : "senses"),
      el("span", "chip chip-dim", dev.board.split("+")[0].trim())
    );
    if (dev.emulator) chips.append(el("span", "chip chip-live", "live firmware"));
    if (dev.kind === "concept") chips.append(el("span", "chip chip-soon", "coming soon"));
    card.append(cv, name, tag, chips);
    grid.append(card);

    const scene = new DeviceScene(cv, null);
    (BUILDERS[dev.id] || BUILDERS["canary-wap"])(scene);
    upgradeRealShape(scene, dev.id);
    scene.start();
    state.cards.set(dev.id, { scene });

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

  const views = {
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
    "The radio": () => {
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
    "Firmware plan": () => {
      const w = el("div");
      w.append(el("p", "muted",
        "Staged as a pending stub in the firmware tree — requirements first, code later, honestly labeled."));
      const ol = el("ol", "concept-plan");
      for (const step of c.plan || []) ol.append(el("li", null, step));
      w.append(ol);
      const a = el("a", null, "read the stub → firmware/projects/canary-fence-guard");
      a.href = "https://github.com/kmay89/securaCV/blob/main/firmware/projects/canary-fence-guard/README.md";
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

  // Web Audio chime (created lazily on first gesture per autoplay rules).
  let audio = null;
  const tone = (f) => {
    if (f <= 0) return;
    try {
      audio ||= new AudioContext();
      const o = audio.createOscillator();
      const g = audio.createGain();
      o.frequency.value = f;
      o.type = "square";
      g.gain.value = 0.03;
      o.connect(g).connect(audio.destination);
      o.start();
      o.stop(audio.currentTime + 0.09);
    } catch {}
  };

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
    Tour: () => tourView(guideProxy, DISPLAY_TOUR, noteLine),
    "Fix it": () => fixView(guideProxy, DISPLAY_FIXES, noteLine),
    "Try it": () => tryView(guideProxy, noteLine),
    ...(ctx.bench ? { Bench: () => benchView(ctx, guideProxy, noteLine) } : {}),
    Wire: () => wireView(serialLog, wireLog),
    Enclosure: () => buildEnclosureLab(state.enclosures, dev.id),
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

  const grid = el("div", "try-grid");
  grid.append(
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

function specsView(dev) {
  const wrap = el("div", "specs");
  const dl = el("dl");
  const row = (k, v) => {
    dl.append(el("dt", null, k), el("dd", null, v));
  };
  row("Board", dev.board);
  if (dev.glass) row("Glass", `${dev.glass.panel} · touch ${dev.glass.touch}`);
  if (dev.body_mm) {
    const b = dev.body_mm;
    row("Body", b.d ? `Ø${b.d} × ${b.depth} mm · stand ${b.stand_tilt_deg}°`
                    : `${b.w} × ${b.h} × ${b.depth} mm · stand ${b.stand_tilt_deg}°`);
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
  const all = [dev.enclosure, ...(dev.docs || [])];
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
    Enclosure: () => buildEnclosureLab(state.enclosures, dev.id),
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
