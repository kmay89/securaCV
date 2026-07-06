/*
 * Unit tests for the pure data-shaping helpers in securacv-aim-card.js.
 * Run: node --test custom_components/securacv/www/securacv-aim-card.test.js
 *
 * Same dual-use pattern as securacv-timeline-card.test.js: the card file
 * guards its custom-element registration on `customElements`, so requiring it
 * under Node yields just the helper surface.
 */
"use strict";

const test = require("node:test");
const assert = require("node:assert/strict");

const {
  normalizeAimPayload,
  boxToCanvasRect,
  discoverSwitchEntity,
  statusLine,
} = require("./securacv-aim-card.js");

// ─── normalizeAimPayload ──────────────────────────────────────────────

test("normalizeAimPayload parses a detection frame", () => {
  const f = normalizeAimPayload({
    present: true, x: 60, y: 40, w: 80, h: 140, score: 87,
    vr: 1, vc: 2, rows: 3, cols: 3, fw: 240, fh: 240,
  });
  assert.equal(f.present, true);
  assert.deepEqual(f.box, { x: 60, y: 40, w: 80, h: 140 });
  assert.equal(f.score, 87);
  assert.equal(f.voxelR, 1);
  assert.equal(f.voxelC, 2);
  assert.equal(f.frameW, 240);
});

test("normalizeAimPayload: empty frame carries no box", () => {
  const f = normalizeAimPayload({
    present: false, x: 0, y: 0, w: 0, h: 0, score: 0,
    vr: -1, vc: -1, rows: 3, cols: 3, fw: 240, fh: 240,
  });
  assert.equal(f.present, false);
  assert.equal(f.box, null);
});

test("normalizeAimPayload rejects junk without throwing", () => {
  assert.equal(normalizeAimPayload(null), null);
  assert.equal(normalizeAimPayload("boxes"), null);
  assert.equal(normalizeAimPayload([1, 2, 3]), null);
  // present but degenerate box → present without a drawable box
  const f = normalizeAimPayload({ present: true, x: 5, y: 5, w: 0, h: 10 });
  assert.equal(f.present, true);
  assert.equal(f.box, null);
});

test("normalizeAimPayload defaults frame/grid dims when absent", () => {
  const f = normalizeAimPayload({ present: false });
  assert.equal(f.frameW, 240);
  assert.equal(f.frameH, 240);
  assert.equal(f.rows, 3);
  assert.equal(f.cols, 3);
});

// ─── boxToCanvasRect ──────────────────────────────────────────────────

test("boxToCanvasRect scales frame coords to canvas coords", () => {
  const frame = normalizeAimPayload({
    present: true, x: 60, y: 40, w: 80, h: 140,
    fw: 240, fh: 240,
  });
  const rect = boxToCanvasRect(frame, 480, 480); // 2x scale
  assert.deepEqual(rect, { x: 120, y: 80, w: 160, h: 280 });
});

test("boxToCanvasRect clamps boxes poking past the frame edge", () => {
  const frame = normalizeAimPayload({
    present: true, x: -20, y: 200, w: 100, h: 100,
    fw: 240, fh: 240,
  });
  const rect = boxToCanvasRect(frame, 240, 240); // 1x
  assert.equal(rect.x, 0);
  assert.equal(rect.w, 80);              // -20..80 clamps to 0..80
  assert.equal(rect.y, 200);
  assert.equal(rect.h, 40);              // 200+100 clamps to frame bottom
});

test("boxToCanvasRect: fully out-of-frame box yields null", () => {
  const frame = normalizeAimPayload({
    present: true, x: 300, y: 0, w: 50, h: 50, fw: 240, fh: 240,
  });
  assert.equal(boxToCanvasRect(frame, 240, 240), null);
});

// ─── discoverSwitchEntity ─────────────────────────────────────────────

test("discoverSwitchEntity prefers explicit config", () => {
  assert.equal(
    discoverSwitchEntity({}, "canary_vision_001", "switch.custom"),
    "switch.custom"
  );
});

test("discoverSwitchEntity finds the slugified aim switch", () => {
  const states = {
    "switch.securacv_canary_vision_canary_vision_001_aim_assist": {},
    "switch.securacv_canary_vision_canary_vision_001_auto_update": {},
    "switch.other_device_aim_assist": {},
  };
  assert.equal(
    discoverSwitchEntity(states, "canary_vision_001", null),
    "switch.securacv_canary_vision_canary_vision_001_aim_assist"
  );
});

test("discoverSwitchEntity returns null when nothing matches", () => {
  assert.equal(discoverSwitchEntity({}, "canary_vision_001", null), null);
  assert.equal(discoverSwitchEntity(null, "canary_vision_001", null), null);
});

test("discoverSwitchEntity never crosses prefix-colliding device ids", () => {
  // canary_vision_001 vs canary_vision_0010: a substring match would make
  // both switches candidates and could pair this card's aim topic with the
  // OTHER device's switch — aiming one camera while toggling another.
  const states = {
    "switch.securacv_canary_vision_canary_vision_0010_aim_assist": {},
    "switch.securacv_canary_vision_canary_vision_001_aim_assist": {},
  };
  assert.equal(
    discoverSwitchEntity(states, "canary_vision_001", null),
    "switch.securacv_canary_vision_canary_vision_001_aim_assist"
  );
  assert.equal(
    discoverSwitchEntity(states, "canary_vision_0010", null),
    "switch.securacv_canary_vision_canary_vision_0010_aim_assist"
  );
  // The slug must also be a whole segment: a bare-suffix id still matches…
  assert.equal(
    discoverSwitchEntity(
      { "switch.canary_vision_001_aim_assist": {} },
      "canary_vision_001",
      null
    ),
    "switch.canary_vision_001_aim_assist"
  );
  // …but an id where the slug is glued to other characters does not.
  assert.equal(
    discoverSwitchEntity(
      { "switch.xcanary_vision_001_aim_assist": {} },
      "canary_vision_001",
      null
    ),
    null
  );
});

// ─── statusLine ───────────────────────────────────────────────────────

test("statusLine covers the honest failure and live states", () => {
  assert.match(statusLine({}), /device_id/);
  assert.match(
    statusLine({ deviceId: "x", subscribeError: "not-admin" }),
    /admin/
  );
  assert.match(
    statusLine({ deviceId: "x", switchOn: false }),
    /switch/i
  );
  assert.match(
    statusLine({ deviceId: "x", switchOn: true, stale: true }),
    /Waiting/
  );
  assert.match(
    statusLine({
      deviceId: "x", switchOn: true, stale: false,
      frame: { present: true, score: 91 },
    }),
    /91%/
  );
  assert.match(
    statusLine({
      deviceId: "x", switchOn: true, stale: false,
      frame: { present: false },
    }),
    /no person/i
  );
});
