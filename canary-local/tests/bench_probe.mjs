// canary-local/tests/bench_probe.mjs — CI probe for the physical bench:
// serve the repo, open the watch's device sheet, and work the power plane
// like a hand would — pull USB and ride the battery, flip the switch and
// watch the rail die, restore power and watch the ROM banner + real boot,
// park the ROM in download mode with BOOT+RESET and recover. Then the
// Nightlight's card (sweep A54): its twin boots, Try it shows the firmware's
// Character ring, and its bench is the C3-LCD-1.47's (USB the only power, no
// battery or switch, no lights listed, the backlight cap stated, and on a dist
// built since sweep A61 the twin clipping its backlight there too), and its
// Try it bench turns the running glass through the firmware's rotation
// mailbox (sweep A80; the watch's, with no mailbox, offers no turn). On the
// boards whose firmware reads BOOT while it runs, the bench's press control
// reaches the twin's button (sweep A99): held 3 s on the watch, the firmware
// says so on its serial, and every press the page makes is released, on the
// twin it pressed — dragged off, a real finger slid off (before it lifts),
// another tab replacing the bench mid-press, the window left, the page
// hidden, RESET mid-press, a double press, touch then mouse, the keyboard;
// held past 10 s the firmware's factory reset runs and the bench restarts
// the twin on its home network, as the control's title says; no press
// reaches a ROM in download mode, the dash (whose firmware never reads BOOT)
// offers none, and a dist without emu_button offers one that cannot press.
// The Nightlight's screen-dark flow stages its download-mode guess in the
// bench's own words, and its glass says them too (sweep A101).
//
// CI runs this probe on the COMMITTED dist, before the wasm job's in-job
// rebuild (canary-local.yml), so a step that needs a binding the committed
// bytes lack skips by name (BENCH_PROBE_SKIP[...]) instead of failing.
//
//   node canary-local/tests/bench_probe.mjs [--shots DIR]
//
// The serial console lives on the Wire tab (the pane is only in the DOM
// while its tab is open), so the probe hops Bench ↔ Wire exactly like a
// person cross-checking the log — which also exercises tab switching.
//
// Uses playwright (or playwright-core with PW_EXECUTABLE set).
import { createServer } from "node:http";
import { readFile, readdir } from "node:fs/promises";
import { extname, join, dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { lookup } from "./probe_server.mjs";
import { readTurnedSources, runtimeTurns } from "./turned_glass.mjs";

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), "../..");
const MIME = {
  ".html": "text/html", ".js": "text/javascript", ".json": "application/json",
  ".css": "text/css", ".stl": "application/octet-stream",
  ".svg": "image/svg+xml", ".png": "image/png", ".glb": "model/gltf-binary",
};

const pw = await (async () => {
  try { return await import("playwright"); }
  catch { return await import("playwright-core"); }
})();

const shotsIdx = process.argv.indexOf("--shots");
const SHOTS = shotsIdx > 0 ? process.argv[shotsIdx + 1] : null;

// Allowlist, not sanitization (same stance as the sibling probes).
const SERVABLE = new Map();
async function allow(dirRel) {
  for (const f of await readdir(join(ROOT, dirRel))) {
    if (extname(f) in MIME) {
      SERVABLE.set(`/${dirRel}/${f}`, join(ROOT, dirRel, f));
    }
  }
}
await allow("canary-local");
await allow("canary-local/assets");
await allow("canary-local/devices");
await allow("canary-local/boards");
await allow("canary-local/enclosures/preview");
await allow("canary-local/emulator/web");
await allow("canary-local/emulator/dist");
await allow("canary-local/models");
await allow("docs/hardware/enclosure");

// Sweep A61: a nightlight dist built since the twin's display HAL clips the
// backlight where the board's does exports emu_backlight_cap_pct, and on it
// the bench row must say the twin clips at the board's 50% and read no level
// past the cap's byte; on a dist built before it, the row must say the twin
// does not. The bundle names its exports, so the probe asks the bytes it
// serves which kind they are.
const NL_BUNDLE = await readFile(join(ROOT, "canary-local/emulator/dist/canary-display-nightlight.js"), "latin1");
const TWIN_CLIPS = NL_BUNDLE.includes("_emu_backlight_cap_pct");
const NL_CAP_PCT = 50;                          // the bench block's cap_pct (bench.test.js holds it to config.h)
const NL_CAP_BYTE = Math.floor((255 * NL_CAP_PCT) / 100);

// Sweep A80: the Nightlight's Try it bench turns the running glass through
// the firmware's rotation mailbox, on a dist that carries the binding (F243).
// The turns and the shapes they give are read from the sources (turned_glass:
// io/orientation.h's Orient, the pin map's panel), never typed here: the
// side turn stands the panel on its edge, the upright one is its own shape.
const NL_TURNS = NL_BUNDLE.includes("_emu_request_rotation");
const [NL_RT] = runtimeTurns(await readTurnedSources(ROOT, (file, enc) => readFile(file, enc)));
const NL_SIDE = { rot: NL_RT.from.rotation, glass: NL_RT.from.glass };     // Orient::R90
const NL_UPRIGHT = { rot: NL_RT.to.rotation, glass: NL_RT.to.glass };      // Orient::R0

const server = createServer(async (req, res) => {
  const path = lookup(SERVABLE, req.url);   // decodes inside its own try: /%E0 is a 404 (A52)
  if (!path) { res.writeHead(404); res.end(); return; }
  try {
    const data = await readFile(path);
    res.writeHead(200, { "content-type": MIME[extname(path)] || "application/octet-stream" });
    res.end(data);
  } catch { res.writeHead(404); res.end(); }
});
await new Promise((ok) => server.listen(0, "127.0.0.1", ok));
const port = server.address().port;

const browser = await pw.chromium.launch(
  process.env.PW_EXECUTABLE ? { executablePath: process.env.PW_EXECUTABLE } : {}
);
const page = await browser.newPage({ viewport: { width: 1280, height: 800 } });
const errors = [];
page.on("pageerror", (e) => errors.push(String(e)));

// Sweep A99: every call the page makes to a twin's emu_button, as
// [module, level], recorded by wrapping each display factory the registry
// names before the page's scripts run (the bundle's top-level var assigns
// through the setter). sessionStorage "probe:noButton" serves a module with
// no _emu_button, as a dist built before it would be.
const REGISTRY = JSON.parse(await readFile(join(ROOT, "canary-local/devices/registry.json"), "utf8"));
const FACTORIES = [...new Set(REGISTRY.devices.filter((d) => d.emulator).map((d) => d.emulator.factory))];
await page.addInitScript((names) => {
  window.__emuButton = [];
  let modules = 0;
  for (const name of names) {
    let real;
    Object.defineProperty(window, name, {
      configurable: true,
      get: () => real,
      set: (f) => {
        real = typeof f !== "function" ? f : async (...args) => {
          const M = await f(...args);
          const id = ++modules;
          if (sessionStorage.getItem("probe:noButton") === "1") delete M._emu_button;
          const cwrap = M.cwrap;
          M.cwrap = (sym, ...rest) => {
            const fn = cwrap(sym, ...rest);
            return sym !== "emu_button" ? fn : (d) => { window.__emuButton.push([id, d]); return fn(d); };
          };
          return M;
        };
      },
    });
  }
}, FACTORIES);
const buttonLog = () => page.evaluate(() => window.__emuButton.slice());
// Every module's calls alternate press, release, starting with a press and
// ending released: no press without its release, none doubled.
const allReleased = (log) => {
  const by = new Map();
  for (const [id, d] of log) by.set(id, [...(by.get(id) || []), d]);
  for (const [id, seq] of by) {
    if (!seq.every((d, i) => d === (i % 2 === 0 ? 1 : 0)) || seq.length % 2) return `module ${id}: ${seq.join(",")}`;
  }
  return null;
};
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const fail = async (msg) => {
  if (SHOTS) await page.screenshot({ path: `${SHOTS}/bench_fail.png` }).catch(() => {});
  console.error("BENCH_PROBE_FAIL:", msg);
  process.exit(1);
};
const openTab = async (name) => {
  await page.locator(".tabs .tab", { hasText: name }).first().click();
};
const serial = () =>
  page.evaluate(() => document.querySelector("pre.log")?.textContent || "");
const waitSerial = async (needle, timeout = 90000) => {
  await page
    .waitForFunction(
      (n) => (document.querySelector("pre.log")?.textContent || "").includes(n),
      needle,
      { timeout }
    )
    .catch(async () => fail(`serial never said ${JSON.stringify(needle)}`));
};
const sings = async () =>
  (await serial()).split("The canary is singing").length - 1;
const waitSings = async (above) => {
  await page
    .waitForFunction(
      (n) =>
        (document.querySelector("pre.log")?.textContent || "").split(
          "The canary is singing"
        ).length - 1 > n,
      above,
      { timeout: 90000 }
    )
    .catch(async () => fail(`firmware never booted again (past ${above} boots)`));
};
const chip = (label) => page.locator(".bench-chip", { hasText: label }).first();
const pressCtl = () => page.locator(".bench-press");
const pressState = () => page.locator(".bench-press .bench-chip-state").textContent();
// A pointer the probe makes up (a touch, or a mouse it does not move), sent
// straight to the press control: the real mouse stays where it is.
const pointAt = (type, pointerType) =>
  page.evaluate(([t, pt]) => {
    const b = document.querySelector(".bench-press");
    b.dispatchEvent(new PointerEvent(t, { bubbles: true, cancelable: true, pointerId: pt === "touch" ? 7 : 1, pointerType: pt, button: 0, isPrimary: true }));
  }, [type, pointerType]);
const serialCount = async (needle) => (await serial()).split(needle).length - 1;
// A real touch, through the browser's own input pipeline (review of A99): a
// touch is captured to the element it went down on, so its pointerleave
// waits for the lift; pointAt()'s made-up PointerEvents never are.
const cdp = await page.context().newCDPSession(page);
const touch = (type, x, y) =>
  cdp.send("Input.dispatchTouchEvent", { type, touchPoints: type === "touchEnd" ? [] : [{ x, y, id: 1 }] });
// What the Try it view heard from the twin about turning its glass (A80):
// "yes", "no", or null if it never heard.
const turnAnswer = async (who) =>
  page
    .waitForSelector(".turn-rail[data-turns]", { state: "attached", timeout: 15000 })   // an empty rail is not "visible"
    .then(() => page.getAttribute(".turn-rail", "data-turns"))
    .catch(() => fail(`${who}'s Try it never heard from the twin whether its glass turns`));
const shade = () =>
  page.evaluate(() => {
    const s = document.querySelector(".glass-shade");
    return s?.classList.contains("on") ? s.textContent : null;
  });

// ── Open the watch sheet; let the firmware boot ─────────────────────────
await page.goto(`http://127.0.0.1:${port}/canary-local/fleet.html#canary-display-watch`);
await page.waitForSelector(".tabs .tab", { timeout: 30000 });
await openTab("Wire");
await waitSerial("The canary is singing");
if (!(await serial()).includes("ESP-ROM:esp32s3-20210327"))
  await fail("power-on ROM banner missing before the app's own boot log");

// ── The Bench tab: lights on, app running ───────────────────────────────
await openTab("Bench");
await page.waitForSelector(".bench-led-dot", { timeout: 10000 });
// the honesty ribbon is the watch's own (sweep A59 review): its S3's ROM, its
// download lines not called quoted, and its one hardwired light (CHG)
const ribbon = () => page.evaluate(() => document.querySelector(".bench-ribbon")?.textContent || "");
{
  const r = await ribbon();
  if (!r.includes("this board's ESP32-S3:") || /ESP32-C3|verbatim/.test(r)) await fail(`the watch's ribbon is not its S3's: ${r}`);
  if (!r.includes("download-mode lines are the bench's own")) await fail(`the watch's ribbon calls its download lines quoted: ${r}`);
  if (!r.includes("The CHG light is hardwired") || /\b(PWR|DONE|USER)\b/.test(r)) await fail(`the watch's ribbon names lights it has not: ${r}`);
}
// review of A128: the charge-flicker flow is the watch's — the registry flags
// its CHG flicker_without_battery, and the bench shows it with no battery
{
  const fixes = await page.locator(".bench-trouble .fix summary").allTextContents();
  if (!fixes.includes("The CHG light is flickering")) await fail(`the watch's troubleshooter lost the charge-flicker flow: ${fixes.join(" | ")}`);
}
if ((await page.locator(".bench-led-dot.on, .bench-led-dot.flicker").count()) < 1)
  await fail("no LED lit on a powered, charging board");
await page
  .waitForFunction(
    () =>
      [...document.querySelectorAll(".bench-diag dd")].some(
        (d) => d.textContent === "app running"
      ),
    null,
    { timeout: 10000 }
  )
  .catch(() => fail("diagnostics never reported the app running"));

// ── Pull USB: the battery rides through, the firmware never notices ─────
await chip("USB-C cable").click();
await openTab("Wire");
await waitSerial("riding the battery");
if (await shade()) await fail("battery ride-through darkened the glass");

// ── Switch OFF on battery: the rail dies for real ───────────────────────
await openTab("Bench");
await chip("ON/OFF").click();
await page
  .waitForFunction(
    () => document.querySelector(".glass-shade")?.classList.contains("on"),
    null,
    { timeout: 5000 }
  )
  .catch(() => fail("rail down but the glass never went honest-dark"));
await openTab("Wire");
if (!(await serial()).includes("rail down"))
  await fail("rail-down never hit the serial log");

// ── Power restored: ROM banner, then the REAL firmware boots again ──────
const boots1 = await sings();
await openTab("Bench");
await chip("USB-C cable").click();
await openTab("Wire");
await waitSerial("power restored");
await waitSings(boots1);
if (await shade()) await fail("glass still shaded after power restore");

// ── A committed setting survives a bench power-cycle ────────────────────
// (Pins the NVS-preseeds-before-power-on ordering: the restored flash
// must be in place before setup() reads it, or the Character resets.)
await openTab("Try it");
await page.waitForSelector(".style-chip.on", { timeout: 15000 });
// the watch's firmware has no rotation mailbox (A80): no turn is offered
if ((await turnAnswer("the watch")) !== "no" || (await page.locator(".turn-row").count()))
  await fail("the watch's Try it offers to turn a glass its firmware cannot turn at runtime");
const target = page.locator(".style-chip:not(.on)").first();
const targetId = await target.getAttribute("data-id");
await target.click();
await new Promise((res) => setTimeout(res, 3500)); // COMMIT_DEBOUNCE_MS (2000) + margin
await openTab("Wire");
const boots3 = await sings();
await openTab("Bench");
await chip("USB-C cable").click(); // switch is OFF from earlier: rail dies
await page
  .waitForFunction(
    () => document.querySelector(".glass-shade")?.classList.contains("on"),
    null,
    { timeout: 5000 }
  )
  .catch(() => fail("USB pull with switch OFF should kill the rail"));
await chip("USB-C cable").click(); // power restored → true re-boot
await openTab("Wire");
await waitSings(boots3);
await openTab("Try it");
await page
  .waitForFunction(
    (id) =>
      document
        .querySelector(`.style-chip[data-id="${id}"]`)
        ?.classList.contains("on"),
    targetId,
    { timeout: 15000 }
  )
  .catch(() => fail("Character choice did not survive the bench power-cycle"));

// ── BOOT held + RESET: the ROM parks in download mode ───────────────────
await openTab("Bench");
await chip("BOOT").click();
await chip("RESET").click();
if ((await shade()) !== "waiting for download")
  await fail("download mode not shown on the glass");
await openTab("Wire");
await waitSerial("waiting for download");

// ── Release BOOT + RESET: the app comes back ────────────────────────────
const boots2 = await sings();
await openTab("Bench");
await chip("BOOT").click();
await chip("RESET").click();
await openTab("Wire");
await waitSings(boots2);
await openTab("Bench");
if (await shade()) await fail("glass still shaded after recovery from download mode");

// ── BOOT pressed for the running firmware (sweep A99) ───────────────────
// The watch's firmware reads BOOT while it runs: the press control sits
// beside the strap toggle, live while the app runs, and a 3 s hold reaches
// the firmware's own reset-hold warning (main.cpp: "[input] BOOT held 3 s").
// Each press the page makes is released, on the twin it pressed.
await page
  .waitForFunction(() => [...document.querySelectorAll(".bench-diag dd")].some((d) => d.textContent === "app running"), null, { timeout: 10000 })
  .catch(() => fail("the watch's app never ran again after download mode"));
if ((await pressCtl().count()) !== 1) await fail("the watch's bench offers no press of BOOT, on a board whose firmware reads it");
await page
  .waitForFunction(() => document.querySelector(".bench-press") && !document.querySelector(".bench-press").disabled, null, { timeout: 10000 })
  .catch(async () => fail(`the watch's press control is not live while the app runs (${await pressState()})`));
{
  const order = await page.evaluate(() => [...document.querySelectorAll(".bench-controls > button")].map((b) => b.className));
  const at = order.findIndex((c) => c.includes("bench-press"));
  if (at < 1 || !/bench-chip/.test(order[at - 1]) || !/bench-momentary/.test(order[at + 1] || "")) await fail(`the press is not beside the strap toggle, before RESET: ${order.join(" | ")}`);
}
const watchBoots = await sings();
const box = await pressCtl().boundingBox();
const on = { x: box.x + box.width / 2, y: box.y + box.height / 2 };
const away = { x: box.x + box.width / 2, y: box.y + box.height + 160 };
// The button's calls since `from`, as levels: a step that lets go before
// the hand does (the window left, the page hidden) is read mid-step.
const levelsSince = async (from) => (await buttonLog()).slice(from).map(([, d]) => d);
const letGoAlready = async (what, from, settle = 120) => {
  if (settle) await sleep(settle);
  const got = await levelsSince(from);
  if (JSON.stringify(got) !== "[1,0]") await fail(`${what}: the twin's button saw ${JSON.stringify(got)} before the hand let go, not [1,0]`);
};
const step = async (what, act, want) => {
  const before = (await buttonLog()).length;
  await act();
  await sleep(120);
  const log = await buttonLog();
  const got = log.slice(before).map(([, d]) => d);
  if (JSON.stringify(got) !== JSON.stringify(want)) await fail(`${what}: the twin's button saw ${JSON.stringify(got)}, not ${JSON.stringify(want)}`);
  const bad = allReleased(log);
  if (bad && want.length % 2 === 0) await fail(`${what}: a press is not released (${bad})`);
  if (want.length % 2 === 0 && (await pressState()) === "pressed") await fail(`${what}: the control still says pressed`);
};
// hold it: the firmware's own warning lands on its serial
await step("a 3 s hold", async () => {
  await page.mouse.move(on.x, on.y);
  await page.mouse.down();
  await sleep(150);
  if ((await pressState()) !== "pressed" || (await pressCtl().getAttribute("aria-pressed")) !== "true")
    await fail(`a held press is not shown pressed (${await pressState()})`);
  await sleep(4300);    // the reset hold warns at 3 s; its factory reset is 10 s away
  await page.mouse.up();
}, [1, 0]);
await openTab("Wire");
await waitSerial("[input] BOOT held 3 s -> keep holding to 10 s for a factory reset", 10000);
if ((await serial()).includes("[input] BOOT held 10 s")) await fail("a 4.5 s hold reached the firmware's factory reset");
console.log("bench_probe: held 4.5 s, the watch's firmware said " + JSON.stringify("[input] BOOT held 3 s -> keep holding to 10 s for a factory reset"));
await openTab("Bench");
await page.waitForFunction(() => !document.querySelector(".bench-press")?.disabled, null, { timeout: 5000 }).catch(() => fail("the press control is not live again"));
// dragged off the chip: let go there
await step("a press dragged off the chip", async () => {
  await page.mouse.move(on.x, on.y);
  await page.mouse.down();
  await page.mouse.move(away.x, away.y, { steps: 4 });
  await page.mouse.up();
}, [1, 0]);
// a finger slid off the chip: let go as it leaves, before it lifts, as the
// mouse does (review of A99: it held BOOT until the lift)
await step("a finger slid off the chip", async () => {
  const from = (await buttonLog()).length;
  await touch("touchStart", on.x, on.y);
  await sleep(120);
  if ((await pressState()) !== "pressed") await fail(`a real touch on the press control did not press (${await pressState()})`);
  await touch("touchMove", on.x, (on.y + away.y) / 2);
  await touch("touchMove", away.x, away.y);
  await letGoAlready("a finger slid off the chip", from);
  if ((await pressState()) === "pressed") await fail("a finger slid off the chip: the control still says pressed before the lift");
  await touch("touchEnd");
}, [1, 0]);
// another tab replacing the bench while a finger holds the press: let go as
// the view goes, not on the old view's next tick (read in the same task as
// the click, so no timer can have run between them)
await step("the Wire tab opened while a finger holds the press", async () => {
  const from = (await buttonLog()).length;
  await touch("touchStart", on.x, on.y);
  await sleep(120);
  if ((await pressState()) !== "pressed") await fail(`a real touch on the press control did not press (${await pressState()})`);
  const got = await page.evaluate((n) => {
    [...document.querySelectorAll(".tabs .tab")].find((t) => t.textContent === "Wire").click();
    return window.__emuButton.slice(n).map(([, d]) => d);
  }, from);
  if (JSON.stringify(got) !== "[1,0]")
    await fail(`the Wire tab replaced the bench while a finger held the press: the twin's button saw ${JSON.stringify(got)} as the view went, not [1,0]`);
  await touch("touchEnd");
  await openTab("Bench");
  await page.waitForSelector(".bench-press", { timeout: 10000 });
}, [1, 0]);
await page.waitForFunction(() => !document.querySelector(".bench-press")?.disabled, null, { timeout: 5000 }).catch(() => fail("the press control is not live again"));
// the window loses focus mid-press (another tab, another app): let go
await step("a press, then the window left", async () => {
  const from = (await buttonLog()).length;
  await page.mouse.move(on.x, on.y);
  await page.mouse.down();
  await page.evaluate(() => window.dispatchEvent(new Event("blur")));
  await letGoAlready("a press, then the window left", from);
  await page.mouse.up();
}, [1, 0]);
// the page hidden mid-press: let go
await step("a press, then the page hidden", async () => {
  const from = (await buttonLog()).length;
  await page.mouse.down();
  await page.evaluate(() => {
    Object.defineProperty(document, "hidden", { configurable: true, get: () => true });
    document.dispatchEvent(new Event("visibilitychange"));
    delete document.hidden;
  });
  await letGoAlready("a press, then the page hidden", from);
  await page.mouse.up();
}, [1, 0]);
// a double press is two presses, each let go
await step("a double press", async () => {
  await page.mouse.click(on.x, on.y);
  await page.mouse.click(on.x, on.y);
}, [1, 0, 1, 0]);
// a touch, then the mouse: one press each, each let go
await step("a touch, then the mouse", async () => {
  await pointAt("pointerdown", "touch");
  await pointAt("pointerup", "touch");
  await pointAt("pointerleave", "touch");
  await page.mouse.click(on.x, on.y);
}, [1, 0, 1, 0]);
// two pointers at once: the second press is refused, and a release lets go
await step("a touch while the mouse holds it", async () => {
  await page.mouse.down();
  await pointAt("pointerdown", "touch");
  await pointAt("pointerup", "touch");
  await page.mouse.up();
}, [1, 0]);
// the keyboard: Space held is a press, its release lets go; a repeat is not a second press
await step("Space held on the control", async () => {
  await pressCtl().focus();
  await page.keyboard.down(" ");
  await page.keyboard.down(" ");
  await sleep(200);
  await page.keyboard.up(" ");
}, [1, 0]);
await page.mouse.move(away.x, away.y);
await page.evaluate(() => document.activeElement?.blur());   // so RESET's click cannot let go by taking the focus
// RESET mid-press (a second finger on RESET): the bench lets go first, on
// the twin that is going away (at once: pressReset() releases before it
// resets), and the new twin boots unpressed
await step("RESET mid-press", async () => {
  const from = (await buttonLog()).length;
  await pointAt("pointerdown", "touch");
  if ((await pressState()) !== "pressed") await fail("a touch on the press control did not press");
  await chip("RESET").click();
  await letGoAlready("RESET mid-press", from, 0);
  const [a, b] = (await buttonLog()).slice(from);
  if (a[0] !== b[0]) await fail(`RESET mid-press: pressed on twin ${a[0]}, released on twin ${b[0]}`);
  await pointAt("pointerup", "touch");
}, [1, 0]);
await openTab("Wire");
await waitSings(watchBoots);
await openTab("Bench");
// download mode: the control is not live, and nothing reaches the twin
await chip("BOOT").click();
await chip("RESET").click();
if ((await shade()) !== "waiting for download") await fail("download mode not shown on the glass");
await page.waitForFunction(() => document.querySelector(".bench-press")?.disabled, null, { timeout: 5000 })
  .catch(() => fail("the press control is live in download mode"));
if ((await pressState()) !== "ROM in download mode") await fail(`the press control in download mode says ${await pressState()}`);
await step("a press in download mode", async () => {
  await pressCtl().click({ force: true });
  await pointAt("pointerdown", "touch");
  await pointAt("pointerup", "touch");
}, []);
const watchBoots2 = await (async () => { await openTab("Wire"); return sings(); })();
await openTab("Bench");
await chip("BOOT").click();
await chip("RESET").click();
await openTab("Wire");
await waitSings(watchBoots2);
{
  const log = await buttonLog();
  const bad = allReleased(log);
  if (bad) await fail(`a twin was left with BOOT pressed: ${bad}`);
  console.log(`bench_probe: ${log.length / 2} presses on the watch's bench, each released on the twin it pressed`);
}
// held past 10 s: the firmware runs its factory reset and restarts, and the
// bench restarts it from the page's copy of its flash (no erase reaches it),
// so it comes back on its home network: what the control's title says
// (review of A99: the title promised a lasting erase)
const RESET_LINE = "[input] BOOT held 10 s -> factory reset (credentials + settings, identity kept)";
const FORGOT = "[CFG] WiFi credentials forgotten - setup opens on next boot.";
const resets0 = await serialCount(RESET_LINE);
await openTab("Bench");
await page.waitForFunction(() => !document.querySelector(".bench-press")?.disabled, null, { timeout: 10000 }).catch(() => fail("the press control is not live again"));
{
  const title = (await pressCtl().getAttribute("title")) || "";
  if (!title.includes("this bench restarts it from the page's copy of its flash, which no erase reaches, so it comes back on its home network"))
    await fail(`the press's title does not say what the bench does after the 10 s hold: ${title}`);
}
await step("a hold past 10 s", async () => {
  await page.mouse.move(on.x, on.y);
  await page.mouse.down();
  await sleep(11000);   // main.cpp's factory reset fires at 10 s
  await page.mouse.up();
}, [1, 0]);
await openTab("Wire");
await page
  .waitForFunction(([reset, forgot]) => {
    const t = document.querySelector("pre.log")?.textContent || "";
    const at = t.lastIndexOf(forgot);
    return t.includes(reset) && at >= 0 && t.indexOf("[WiFi] Connected (emulated).", at) > at && t.indexOf("The canary is singing", at) > at;
  }, [RESET_LINE, FORGOT], { timeout: 90000 })
  .catch(async () => fail(`held past 10 s, the serial never said the factory reset and then the twin back on its home network: ${(await serial()).slice(-1500)}`));
if ((await serialCount(RESET_LINE)) !== resets0 + 1) await fail("held past 10 s, the firmware's factory reset did not run once");
console.log("bench_probe: held 11 s, the watch's firmware ran its factory reset and the bench restarted it on its home network, as the press's title says");
{
  const bad = allReleased(await buttonLog());
  if (bad) await fail(`a twin was left with BOOT pressed after the 10 s hold: ${bad}`);
}
await openTab("Bench");

if (SHOTS) await page.screenshot({ path: `${SHOTS}/bench_ok.png` });

// ── The Nightlight's twin (sweep A54) ───────────────────────────────────
// Its card boots the nightlight flavor CI built (F204), offers Try it with
// the firmware's own Character ring, and its bench is the C3-LCD-1.47's:
// USB-C the only power (no battery, no switch to gate one), so the battery
// chip is inert, a click on it fits nothing, the troubleshooter offers no
// step that stages a battery, pulling the cable drops the rail at once, and
// the backlight row says the cap the board's HAL enforces and this twin's
// does not. Its ROM is the ESP32-C3's (sweep A59): the power-on banner is
// the C3's quoted one, BOOT held through RST parks it with GPIO9 named, and
// no S3 banner is ever printed on the card. A fresh document: the page reads
// its #card only at load.
await page.goto("about:blank");
await page.goto(`http://127.0.0.1:${port}/canary-local/fleet.html#canary-nightlight`);
await page.waitForSelector(".tabs .tab", { timeout: 30000 });
const tabs = await page.locator(".tabs .tab").allTextContents();
for (const t of ["Try it", "Bench"]) {
  if (!tabs.includes(t)) await fail(`the Nightlight's sheet offers no "${t}" tab (tabs: ${tabs.join(", ")})`);
}
await openTab("Wire");
const nlBoots0 = await sings();
await waitSerial("The canary is singing");
await waitSerial("backlight duty capped at 50%");   // the firmware's own boot line, on the twin
if (!(await serial()).includes("ESP-ROM:esp32c3-api1-20210207\nBuild:Feb  7 2021\nrst:0x1 (POWERON)"))
  await fail("the Nightlight's power-on banner is not the ESP32-C3's");
await openTab("Try it");
await page.waitForSelector(".style-chip.on", { timeout: 15000 })
  .catch(() => fail("the Nightlight's Try it never showed the firmware's Character ring"));
// ── Turn the Nightlight's glass while it runs (sweep A80) ──
// On the committed dist, before CI's rebuild: a dist with the binding offers
// the four turns, and a chip turns the glass through the mailbox — the canvas
// takes the turned shape and the lit chip is the turn the HAL announced; the
// glass is stood back upright before the bench steps below. A dist without
// the binding offers no turn, and the step says so by name.
{
  const answer = await turnAnswer("the Nightlight");
  if (!NL_TURNS) {
    if (answer !== "no" || (await page.locator(".turn-row").count()))
      await fail("the Nightlight's Try it offers a turn on a dist without emu_request_rotation");
    console.log("BENCH_PROBE_SKIP[nightlight turn]: this nightlight dist has no emu_request_rotation " +
      "(built before sweep F243), so its Try it offers no turn; the step runs once CI's rebuilt dist carries the binding");
  } else {
    if (answer !== "yes") await fail("the Nightlight's Try it offers no turn, on a dist with emu_request_rotation");
    const chips = await page.locator(".turn-row .turn-chip").count();
    if (chips !== 4) await fail(`the Nightlight's Try it offers ${chips} turns, not Orient's four`);
    const turned = (to) =>
      page
        .waitForFunction(({ rot, w, h }) => {
          const g = document.querySelector("canvas.glass");
          const on = [...document.querySelectorAll(".turn-chip.on")].map((b) => b.dataset.rot);
          return g && g.width === w && g.height === h && on.length === 1 && on[0] === String(rot);
        }, { rot: to.rot, w: to.glass.w, h: to.glass.h }, { timeout: 15000 })
        .then(() => true)
        .catch(() => false);
    if (!(await turned(NL_UPRIGHT)))
      await fail(`the Nightlight did not boot upright on its card (a ${NL_UPRIGHT.glass.w}x${NL_UPRIGHT.glass.h} glass, the upright chip lit)`);
    for (const to of [NL_SIDE, NL_UPRIGHT]) {
      await page.locator(`.turn-chip[data-rot="${to.rot}"]`).click();
      if (!(await turned(to))) {
        const now = await page.evaluate(() => {
          const g = document.querySelector("canvas.glass");
          return `${g?.width}x${g?.height}, lit ${[...document.querySelectorAll(".turn-chip.on")].map((b) => b.dataset.rot).join(",") || "none"}`;
        });
        await fail(`a turn to rotation ${to.rot} never gave the ${to.glass.w}x${to.glass.h} glass with its chip lit (now ${now})`);
      }
      const said = await page.locator(".note").first().textContent();
      if (!/^asked the firmware to stand it .*: its loop turns the panel on its next pass/.test(said || ""))
        await fail(`the turn to rotation ${to.rot} was not said in the note line: ${said}`);
    }
    // the lit chip again (review): asked, nothing turns, and the note says so
    await page.locator(`.turn-chip[data-rot="${NL_UPRIGHT.rot}"]`).click();
    const same = await page.locator(".note").first().textContent();
    if (!/^it already stands upright .*: the firmware keeps the glass as it is, and nothing tumbles$/.test(same || ""))
      await fail(`a click on the lit chip promised a turn: ${same}`);
    if (!(await turned(NL_UPRIGHT))) await fail("a click on the lit chip moved the glass off upright");
    console.log(`bench_probe: the Nightlight's Try it turned the running glass ${NL_UPRIGHT.glass.w}x${NL_UPRIGHT.glass.h} → ` +
      `${NL_SIDE.glass.w}x${NL_SIDE.glass.h} (rotation ${NL_SIDE.rot}) and back (rotation ${NL_UPRIGHT.rot})`);
  }
}
await openTab("Bench");
await page
  .waitForFunction(
    () => [...document.querySelectorAll(".bench-diag dd")].some((d) => d.textContent === "app running"),
    null,
    { timeout: 10000 }
  )
  .catch(() => fail("the Nightlight's bench never reported the app running"));
if (await page.locator(".bench-led-dot").count()) await fail("the Nightlight's bench lists a light no file settles");
{
  // the ribbon is the C3's story, with no S3 and no lights in it (sweep A59 review)
  const r = await ribbon();
  if (!r.includes("this board's ESP32-C3:")) await fail(`the Nightlight's ribbon does not name its ESP32-C3: ${r}`);
  if (/ESP32-S3|esp32s3|verbatim|\b(PWR|CHG|DONE)\b/.test(r)) await fail(`the Nightlight's ribbon tells the S3's story: ${r}`);
}
if (!/none on this board/.test(await chip("battery").textContent())) await fail("the Nightlight's battery chip does not say the board has none");
if (!(await chip("battery").isDisabled())) await fail("the Nightlight's battery chip can be pressed, on a board with no battery path");
// a hand at the chip anyway: nothing fits, the chip keeps saying so
await chip("battery").click({ force: true });
if (!/none on this board/.test(await chip("battery").textContent())) await fail("a click fitted a battery the Nightlight has not got");
// and the troubleshooter offers no step that stages a battery it cannot fit
await page.locator(".bench-trouble summary").first().click();
const nlFixes = await page.locator(".bench-trouble .fix summary").allTextContents();
if (!nlFixes.length) await fail("the Nightlight's bench offers no troubleshooting flows at all");
if (nlFixes.some((t) => /unplugged the cable/.test(t))) await fail("the Nightlight's bench offers the battery ride-through flow");
if (/rides the battery|ride-through/.test(await page.locator(".bench-trouble").textContent()))
  await fail("a Nightlight troubleshooting step still talks of riding a battery");
// sweep A128: its bench block lists no light, so nothing the page offers it
// names one (no label, no power or charge light, no light at all), and the
// screen-dark flow says its symptom without the PWR light it does not list
{
  // the labels and LED as the registry spells them (upper case: "done",
  // "user" and "led" are ordinary words too), "light(s)" in any case
  const trouble = await page.locator(".bench-trouble").textContent();
  const named = [...(trouble.match(/\b(?:PWR|CHG|DONE|USER|LEDs?)\b/g) || []), ...(trouble.match(/\blights?\b/gi) || [])];
  if (named.length) await fail(`the Nightlight's troubleshooter names lights its bench block does not list: ${[...new Set(named)].join(", ")}`);
  if (!nlFixes.includes("Screen dark")) await fail(`the Nightlight's screen-dark flow is not worded for a board with no PWR light: ${nlFixes.join(" | ")}`);
}
// its screen-dark flow keeps a download-mode guess, told in the bench's own
// words for a chip with no download banner the Lab quotes (sweep A101), and
// staging it parks the C3's ROM, which RST alone recovers
{
  const dark = page.locator(".bench-trouble .fix", { has: page.locator("summary", { hasText: "Screen dark" }) }).first();
  await dark.locator("summary").click();
  const titles = await dark.locator(".fix-step h4").allTextContents();
  if (JSON.stringify(titles) !== JSON.stringify(["1. First guess: it's asleep, not broken", "2. Second guess: the ROM is waiting for a flasher"]))
    await fail(`the Nightlight's screen-dark flow is not its two guesses: ${titles.join(" | ")}`);
  const guess = dark.locator(".fix-step").nth(1);
  const words = await guess.textContent();
  if (/waiting for download|ESP-ROM|rst:0x|DOWNLOAD\(USB/.test(words)) await fail(`the Nightlight's download-mode guess quotes a ROM line its C3 does not print: ${words}`);
  if (!words.includes("If BOOT (GPIO9) was held") || !words.includes("the bench's own note")) await fail(`the Nightlight's guess is not its own: ${words}`);
  await openTab("Wire");
  const notes0 = await serialCount("ESP32-C3 ROM: BOOT (GPIO9) was low at reset, so it waits for a flasher");
  const nlBootsA = await sings();
  await openTab("Bench");
  await page.locator(".bench-trouble summary").first().click().catch(() => {});
  if (!(await page.locator(".bench-trouble").evaluate((d) => d.open))) await page.locator(".bench-trouble summary").first().click();
  const dark2 = page.locator(".bench-trouble .fix", { has: page.locator("summary", { hasText: "Screen dark" }) }).first();
  if (!(await dark2.evaluate((d) => d.open))) await dark2.locator("summary").click();
  await dark2.locator(".fix-step").nth(1).locator("button", { hasText: "show me on the glass" }).click();
  // its glass says the bench's words, not the S3's banner line (review of
  // A101: bench.js downloadShade(); the S3 cards keep "waiting for download")
  if ((await shade()) !== "ROM waiting for a flasher") await fail(`the Nightlight's download-mode guess did not park its ROM in its own words (the glass says ${JSON.stringify(await shade())})`);
  if (!(await pressCtl().isDisabled())) await fail("the Nightlight's press control is live in download mode");
  await openTab("Wire");
  if ((await serialCount("ESP32-C3 ROM: BOOT (GPIO9) was low at reset, so it waits for a flasher")) !== notes0 + 1)
    await fail("staging the Nightlight's guess did not print the C3's own download note on the serial");
  await openTab("Bench");
  await chip("RST").click();
  await openTab("Wire");
  await waitSings(nlBootsA);
  console.log("bench_probe: the Nightlight's screen-dark flow staged its download-mode guess in the bench's own words, and RST alone recovered");
  await openTab("Bench");
  // its firmware reads BOOT too: the press is offered, live, and let go
  await page.waitForFunction(() => document.querySelector(".bench-press") && !document.querySelector(".bench-press").disabled, null, { timeout: 10000 })
    .catch(() => fail("the Nightlight's press control is not live while its app runs"));
  const before = (await buttonLog()).length;
  await pressCtl().click();
  await sleep(120);
  const got = (await buttonLog()).slice(before).map(([, d]) => d);
  if (JSON.stringify(got) !== "[1,0]") await fail(`a click on the Nightlight's press reached its twin as ${JSON.stringify(got)}`);
}
if (!(await chip("— (no battery path)").count())) await fail("the Nightlight's bench offers a switch with a battery path to gate");
const capRow = TWIN_CLIPS
  ? "^(?:day ladder · (\\d+)/255|night floor · 13-bit duty (\\d+)/8191) · capped at 50% duty, as the board's HAL caps it$"
  : " · the board caps it at 50% duty, this twin does not$";
const capRead = await page
  .waitForFunction(
    (src) => [...document.querySelectorAll(".bench-diag dd")].map((d) => d.textContent).find((t) => new RegExp(src).test(t)) || null,
    capRow,
    { timeout: 10000 }
  )
  .then((h) => h.jsonValue())
  .catch(() => null);
if (!capRead) {
  await fail(TWIN_CLIPS ? "the Nightlight's backlight row does not say the twin clips at the board's 50% cap"
    : "the Nightlight's backlight row does not say the board's 50% cap");
} else if (TWIN_CLIPS) {
  // the level the twin reads is the byte the board would write: never past the cap
  const m = new RegExp(capRow).exec(capRead);
  const day = m[1] !== undefined ? Number(m[1]) : null;
  const night = m[2] !== undefined ? Number(m[2]) : null;
  if (day !== null && day > NL_CAP_BYTE) await fail(`the Nightlight's twin glows at ${day}/255, past the board's cap byte ${NL_CAP_BYTE}`);
  if (night !== null && night >> 5 > NL_CAP_BYTE) await fail(`the Nightlight's twin's night duty ${night} writes past the board's cap byte ${NL_CAP_BYTE}`);
  console.log(`bench_probe: the Nightlight twin clips its backlight (${capRead.trim()})`);
} else {
  console.log("bench_probe: this nightlight dist was built before the twin clipped its backlight (no emu_backlight_cap_pct); the row says so");
}
await chip("USB-C cable").click();
await page
  .waitForFunction(() => document.querySelector(".glass-shade")?.classList.contains("on"), null, { timeout: 5000 })
  .catch(() => fail("USB pulled from a board with no battery, and the rail stayed up"));
await openTab("Wire");
if (!(await serial()).includes("rail down (usb-out)")) await fail("the Nightlight's rail-down never hit the serial log");
const nlBoots1 = await sings();
if (nlBoots1 <= nlBoots0) await fail("the Nightlight's firmware never booted on its card");
await openTab("Bench");
await chip("USB-C cable").click();
await openTab("Wire");
await waitSings(nlBoots1);
if (await shade()) await fail("the Nightlight's glass still shaded after USB came back");
// BOOT (GPIO9) held through RST: the C3's ROM waits for a flasher, said in its
// own strap story; release BOOT, RST again, and the firmware boots.
const nlNotes = await serialCount("ESP32-C3 ROM: BOOT (GPIO9) was low at reset, so it waits for a flasher");
await openTab("Bench");
await chip("BOOT").click();
await chip("RST").click();
if ((await shade()) !== "ROM waiting for a flasher") await fail(`the Nightlight's download mode not shown on the glass in its own words (it says ${JSON.stringify(await shade())})`);
await openTab("Wire");
await page
  .waitForFunction(
    ([n, k]) => (document.querySelector("pre.log")?.textContent || "").split(n).length - 1 > k,
    ["ESP32-C3 ROM: BOOT (GPIO9) was low at reset, so it waits for a flasher", nlNotes],
    { timeout: 90000 }
  )
  .catch(() => fail("BOOT held through RST never printed the C3's own download note"));
const nlBoots2 = await sings();
await openTab("Bench");
await chip("BOOT").click();
await chip("RST").click();
await openTab("Wire");
await waitSings(nlBoots2);
if (await shade()) await fail("the Nightlight's glass still shaded after recovery from download mode");
if (/esp32s3|GPIO0\b/i.test(await serial())) await fail("the Nightlight's serial printed an ESP32-S3 banner or strap");
if (SHOTS) await page.screenshot({ path: `${SHOTS}/bench_nightlight_ok.png` });
{
  const bad = allReleased(await buttonLog());
  if (bad) await fail(`a Nightlight twin was left with BOOT pressed: ${bad}`);
}

// ── No press where the firmware never reads BOOT (sweep A99) ─────────────
// The dash's firmware names no BOOT_BUTTON_PIN (GPIO0 is an RGB data line
// there): its bench has the strap toggle and no press control.
await page.goto("about:blank");
await page.goto(`http://127.0.0.1:${port}/canary-local/fleet.html#canary-display-dash`);
await page.waitForSelector(".tabs .tab", { timeout: 30000 });
await openTab("Bench");
await page.waitForSelector(".bench-chip", { timeout: 10000 });
if (await pressCtl().count()) await fail("the dash's bench offers a press of BOOT, on a board whose firmware never reads it");
if (!(await chip("BOOT").count())) await fail("the dash's bench lost its strap toggle");
// sweep A128: the dash keeps its PWR flows, and is offered no USER LED (the
// watch's), which its bench block does not list
{
  const fixes = await page.locator(".bench-trouble .fix summary").allTextContents();
  for (const want of ["The PWR light is always on — can I turn it off?", "Screen dark, but the PWR light is on", "Nothing at all — no PWR light, no glass"])
    if (!fixes.includes(want)) await fail(`the dash's troubleshooter lost "${want}": ${fixes.join(" | ")}`);
  if (/\bUSER\b/.test(await page.locator(".bench-trouble").textContent())) await fail("the dash's troubleshooter names a USER LED its bench block does not list");
  // and not the charge-flicker flow: the registry flags no flicker on its
  // CHG, which the bench shows dark with USB in and no battery (review of A128)
  if (fixes.includes("The CHG light is flickering")) await fail(`the dash's troubleshooter offers the charge-flicker flow, though its CHG never flickers on the bench: ${fixes.join(" | ")}`);
}

// ── A dist without emu_button (sweep A99) ───────────────────────────────
// Served a watch module with no _emu_button, as a dist built before it: the
// control is there, inert, and says so; nothing reaches the twin.
await page.goto("about:blank");
await page.goto(`http://127.0.0.1:${port}/canary-local/fleet.html`);
await page.evaluate(() => sessionStorage.setItem("probe:noButton", "1"));
await page.goto("about:blank");
await page.goto(`http://127.0.0.1:${port}/canary-local/fleet.html#canary-display-watch`);
await page.waitForSelector(".tabs .tab", { timeout: 30000 });
await openTab("Wire");
await waitSerial("The canary is singing");
await openTab("Bench");
await page
  .waitForFunction(() => document.querySelector(".bench-press .bench-chip-state")?.textContent === "not in this emulator build", null, { timeout: 10000 })
  .catch(async () => fail(`a dist without emu_button: the press control says ${await pressState()}`));
if (!(await pressCtl().isDisabled())) await fail("a dist without emu_button offers a live press");
await pressCtl().click({ force: true });
await sleep(150);
if ((await buttonLog()).length) await fail(`a dist without emu_button was pressed: ${JSON.stringify(await buttonLog())}`);
await page.evaluate(() => sessionStorage.removeItem("probe:noButton"));
console.log("bench_probe: the dash offers no press; a dist without emu_button offers an inert one that says so");
await browser.close();
server.close();

if (errors.length) {
  console.error("BENCH_PROBE_FAIL: page errors:\n" + errors.slice(0, 8).join("\n"));
  process.exit(1);
}
console.log("BENCH_PROBE_OK power-pull, ride-through, brown-down, download-mode all behaved; BOOT pressed for the running firmware " +
  "and let go on every path, held past 10 s as its title says; the Nightlight twin booted on its card, USB its only power" +
  (NL_TURNS ? ", its glass turned from Try it" : ""));
process.exit(0);
