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
// mailbox (sweep A80; the watch's, with no mailbox, offers no turn).
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
await openTab("Bench");
await chip("BOOT").click();
await chip("RST").click();
if ((await shade()) !== "waiting for download") await fail("the Nightlight's download mode not shown on the glass");
await openTab("Wire");
await waitSerial("ESP32-C3 ROM: BOOT (GPIO9) was low at reset, so it waits for a flasher");
const nlBoots2 = await sings();
await openTab("Bench");
await chip("BOOT").click();
await chip("RST").click();
await openTab("Wire");
await waitSings(nlBoots2);
if (await shade()) await fail("the Nightlight's glass still shaded after recovery from download mode");
if (/esp32s3|GPIO0\b/i.test(await serial())) await fail("the Nightlight's serial printed an ESP32-S3 banner or strap");
if (SHOTS) await page.screenshot({ path: `${SHOTS}/bench_nightlight_ok.png` });
await browser.close();
server.close();

if (errors.length) {
  console.error("BENCH_PROBE_FAIL: page errors:\n" + errors.slice(0, 8).join("\n"));
  process.exit(1);
}
console.log("BENCH_PROBE_OK power-pull, ride-through, brown-down, download-mode all behaved; the Nightlight twin booted on its card, USB its only power" +
  (NL_TURNS ? ", its glass turned from Try it" : ""));
process.exit(0);
