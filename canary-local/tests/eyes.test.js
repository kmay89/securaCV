// canary-local/tests/eyes.test.js — the webcam bench's honesty gate.
//
// Three jobs, mirroring tests/vision.test.js and tests/audio.test.js:
//  1. Pin the stand-in sensor's pure logic (motion cells → blobs → boxes) on
//     synthetic frames, so the only new code — the sensor side of the
//     boundary — has webcam-free proof.
//  2. Drive the COMMITTED Vision firmware wasm with boxes produced by that
//     sensor from a synthetic walk, and assert the firmware's own event
//     order: presence_started → dwell_started → presence_ended. The page has
//     no JavaScript decision mirror that can drift.
//  3. Drift-lock the page's safeguards (standing disclaimer, camera-privacy
//     copy, real-firmware chip, registration in build-line.json, CI wiring)
//     so they cannot quietly disappear.

const { test } = require("node:test");
const assert = require("node:assert");
const { readFileSync } = require("node:fs");
const { join } = require("node:path");

const ROOT = join(__dirname, "..");
const REPO = join(ROOT, "..");
const read = (p) => readFileSync(p, "utf8");

const visionFactory = require(join(ROOT, "emulator/dist/canary-vision-core.js"));

async function sensor() {
  return import("../assets/eyes-bench.js");
}
async function firmwareCore() {
  const { createVisionFirmwareCore } = await import("../emulator/web/vision-core.js");
  return createVisionFirmwareCore(visionFactory);
}

// Synthetic frames: W×H gray canvas with an optional TEXTURED square at
// (x, y). The texture translates with the square — the way clothing moves
// with a person — so a shift changes the square's interior, not just its
// edges. (A flat square would exhibit the aperture problem: only its leading
// and trailing edges differ, which no real mover does.)
const W = 240, H = 240, SQ = 80;
function frame(square) {
  const g = new Uint8ClampedArray(W * H).fill(20);
  if (square) {
    for (let y = square.y; y < square.y + SQ; y++) {
      for (let x = square.x; x < square.x + SQ; x++) {
        if (x >= 0 && x < W && y >= 0 && y < H) {
          const xr = x - square.x, yr = y - square.y;
          g[y * W + x] = 90 + ((xr * 31 + yr * 17) % 6) * 27;
        }
      }
    }
  }
  return g;
}

// ── 1. the stand-in sensor, pure ───────────────────────────────────────────

test("a moved square becomes one box that covers it", async () => {
  const { detectBoxes } = await sensor();
  const boxes = detectBoxes(frame({ x: 40, y: 40 }), frame({ x: 60, y: 40 }), W, H);
  assert.strictEqual(boxes.length, 1);
  const b = boxes[0];
  // The change spans old+new square extents: x ∈ [40, 140), y ∈ [40, 120).
  assert.ok(b.x <= 40 && b.x + b.w >= 130, `box misses the motion: ${JSON.stringify(b)}`);
  assert.ok(b.y <= 40 && b.y + b.h >= 110, `box misses the motion: ${JSON.stringify(b)}`);
  assert.ok(b.score >= 70, "a solid person-sized mover must clear the firmware threshold: " + b.score);
  assert.ok(b.score <= 99);
});

test("a still frame yields no boxes; so does the very first frame", async () => {
  const { detectBoxes } = await sensor();
  const a = frame({ x: 40, y: 40 });
  assert.strictEqual(detectBoxes(a, a, W, H).length, 0);
  assert.strictEqual(detectBoxes(null, a, W, H).length, 0);
});

test("single-cell flicker is sensor noise, not a report", async () => {
  const { detectBoxes } = await sensor();
  const a = frame(null);
  const b = frame(null);
  // one bright 10×10 cell — below minCells once blobbed
  for (let y = 100; y < 110; y++) for (let x = 100; x < 110; x++) b[y * W + x] = 220;
  assert.strictEqual(detectBoxes(a, b, W, H).length, 0);
});

test("two separated movers become two boxes, merged only if overlapping", async () => {
  const { detectBoxes, mergeBoxes, iouTopLeft } = await sensor();
  const prev = frame(null);
  const curr = frame(null);
  for (let y = 20; y < 70; y++) for (let x = 20; x < 70; x++) curr[y * W + x] = 220;
  for (let y = 160; y < 210; y++) for (let x = 160; x < 210; x++) curr[y * W + x] = 220;
  const two = detectBoxes(prev, curr, W, H);
  assert.strictEqual(two.length, 2, JSON.stringify(two));
  // merge is IoU-gated, top-left convention (what vision_emu_push_box takes)
  const a = { x: 0, y: 0, w: 100, h: 100, score: 90 };
  const b = { x: 10, y: 10, w: 100, h: 100, score: 80 };
  assert.ok(iouTopLeft(a, b) > 0.5);
  assert.strictEqual(mergeBoxes([a, b], 0.35).length, 1);
  assert.strictEqual(mergeBoxes([a, { ...b, x: 200, y: 200 }], 0.35).length, 2);
});

test("sensor memory holds a stopped blob briefly, decaying, then lets go", async () => {
  const { holdBoxes, SENSOR } = await sensor();
  const box = { x: 10, y: 10, w: 50, h: 50, score: 90 };
  let m = holdBoxes(null, [box], 1000);
  assert.strictEqual(m.boxes.length, 1);
  const heldEarly = holdBoxes(m.memory, [], 1000 + 300);
  assert.strictEqual(heldEarly.boxes.length, 1);
  assert.ok(heldEarly.boxes[0].score < 90, "held score must decay");
  const heldLate = holdBoxes(m.memory, [], 1000 + SENSOR.holdMs + 1);
  assert.strictEqual(heldLate.boxes.length, 0);
});

// ── 2. sensor boxes drive the real firmware, end to end ────────────────────

test("a synthetic walk fires the firmware's own events, in order", async () => {
  const { detectBoxes, holdBoxes } = await sensor();
  const core = await firmwareCore();
  const cfg = core.configure({
    person_target: 0, score_min: 70, lost_timeout_ms: 500, dwell_start_ms: 1000,
  });
  assert.strictEqual(cfg.score_min, 70, "firmware refused the test config");

  const events = [];
  let prevGray = null;
  let memory = null;
  const step = (nowMs, square) => {
    const gray = frame(square);
    const fresh = detectBoxes(prevGray, gray, W, H);
    prevGray = gray;
    const held = holdBoxes(memory, fresh, nowMs);
    memory = held.memory;
    const boxes = held.boxes.map((b) => ({
      x: b.x, y: b.y, w: b.w, h: b.h, score: b.score, target: cfg.person_target,
    }));
    const tick = core.tick(nowMs, boxes);
    if (tick.event) events.push(tick.event);
    return tick;
  };

  // Someone walks in and keeps shifting for 1.6 s (motion every tick)…
  let now = 0;
  step(now, { x: 20, y: 80 });                     // first frame: no prev, no boxes
  for (let i = 1; i <= 16; i++) {
    now = i * 100;
    step(now, { x: 20 + i * 6, y: 80 });
  }
  assert.deepStrictEqual(events, ["presence_started", "dwell_started"],
    "walking in and staying must raise presence then dwell");

  // …then freezes: the sensor's short memory expires, and after the
  // firmware's lost timeout the firmware — not the sensor — ends presence.
  const frozen = { x: 20 + 16 * 6, y: 80 };
  for (let i = 17; i <= 40 && !events.includes("presence_ended"); i++) {
    now = i * 100;
    step(now, frozen);
  }
  assert.ok(events.includes("presence_ended"),
    "stillness past hold + lost timeout must end presence: " + JSON.stringify(events));
});

test("boxes below the firmware score threshold are the firmware's to refuse", async () => {
  const core = await firmwareCore();
  const cfg = core.configure({
    person_target: 0, score_min: 70, lost_timeout_ms: 500, dwell_start_ms: 1000,
  });
  const weak = [{ x: 100, y: 100, w: 40, h: 80, score: cfg.score_min - 1, target: 0 }];
  const t = core.tick(0, weak);
  assert.strictEqual(t.sample.person_now, false, "a below-threshold box must not read as presence");
  assert.strictEqual(t.event, null);
});

// ── 3. the page's safeguards cannot quietly disappear ──────────────────────

const pageHtml = read(join(ROOT, "eyes.html"));
const benchJs = read(join(ROOT, "assets/eyes-bench.js"));
const buildLine = JSON.parse(read(join(ROOT, "build-line.json")));
const workflow = read(join(REPO, ".github/workflows/canary-local.yml"));

test("the page ships its standing disclaimer and honesty footer", () => {
  assert.match(pageHtml, /not a security device/i);
  assert.match(pageHtml, /never recorded, stored, or sent anywhere/i);
  assert.match(pageHtml, /presence_fsm\.cpp/);
  assert.ok(pageHtml.includes('src="emulator/dist/canary-vision-core.js"'),
    "the page must load the committed firmware core");
  assert.ok(pageHtml.includes('src="assets/eyes-bench.js"'));
});

test("the driver keeps the camera safeguards", () => {
  assert.match(benchJs, /never recorded, saved, or uploaded/i);
  assert.match(benchJs, /real firmware wasm/, "the runtime chip text is a promise");
  assert.match(benchJs, /pagehide/, "the camera must release when the page hides");
  assert.ok(!/fetch\(\s*["']http/.test(benchJs), "no network path beyond the local stamp");
});

test("the bench is on the build line, marked real", () => {
  const vision = buildLine.stages
    .find((s) => s.id === "sense").tracks
    .find((t) => t.track === "vision").benches
    .find((b) => b.slug === "vision");
  const depth = (vision.depths || []).find((d) => d.lab === "eyes.html");
  assert.ok(depth, "eyes.html must be registered under The Vision in build-line.json");
  assert.strictEqual(depth.real, true, "the bench boots real firmware wasm — say so");
});

test("CI runs this gate and the browser probe", () => {
  assert.ok(workflow.includes("canary-local/tests/eyes.test.js"),
    "eyes.test.js is not wired into canary-local.yml");
  assert.ok(workflow.includes("canary-local/tests/eyes_probe.mjs"),
    "eyes_probe.mjs is not wired into canary-local.yml");
});
