// Board-facts snapshot — the anti-rot guard for the Waveshare dash boards we
// build on (docs/hardware/waveshare_board_reference.md).
//
// Three things pinned:
//   1. the committed snapshot (board_facts.json) has the shape the reference
//      page and the firmware drift-lock rely on;
//   2. the parser (gen_board_facts.py) still extracts pins from the vendor
//      table shapes — run over a self-authored fixture, so a reshaped parser
//      fails here instead of silently writing an empty refresh;
//   3. our firmware pins.h RGB/LCD map still MATCHES the vendor snapshot for
//      every board — the "our model is exact" guarantee. If Waveshare's page
//      (via a freshness PR) or our pins.h ever drift apart, this fails.
//
// Run: node --test canary-local/tests/board_facts.test.mjs
import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { execFileSync } from "node:child_process";
import { fileURLToPath } from "node:url";

const ROOT = fileURLToPath(new URL("../..", import.meta.url));
const read = (p) => readFileSync(ROOT + p, "utf8");
const snap = JSON.parse(read("canary-local/devices/board_facts.json"));

// board id → its firmware pins.h
const PINS_H = {
  "waveshare-esp32s3-lcd43": "firmware/boards/waveshare-esp32s3-lcd43/pins/pins.h",
  "waveshare-esp32s3-lcd43b": "firmware/boards/waveshare-esp32s3-lcd43b/pins/pins.h",
  "waveshare-esp32s3-lcd43c": "firmware/boards/waveshare-esp32s3-lcd43c/pins/pins.h",
};

// The RGB + sync signals whose vendor GPIO must equal LCD_PIN_<signal> in
// pins.h. (The LCD table's signal column is clean on all three boards; other
// sections' labels vary, so the drift-lock centers on this universal core.)
const LCD_SIGNALS = [
  "R3", "R4", "R5", "R6", "R7", "G2", "G3", "G4", "G5", "G6", "G7",
  "B3", "B4", "B5", "B6", "B7", "DE", "VSYNC", "HSYNC", "PCLK",
];

function pinDefs(hText) {
  const out = {};
  for (const m of hText.matchAll(/#define\s+(LCD_PIN_[A-Z0-9]+)\s+(-?\d+)/g)) {
    out[m[1]] = Number(m[2]);
  }
  return out;
}

test("snapshot: schema + every board carries load-bearing facts", () => {
  assert.equal(snap.schema, "securacv-board-facts-1");
  const ids = Object.keys(snap.boards);
  assert.ok(ids.length >= 3, "expected the three 4.3 boards");
  for (const [id, b] of Object.entries(snap.boards)) {
    for (const k of ["vendor", "product", "source_url", "verified_utc", "facts"]) {
      assert.ok(b[k], `${id}: missing ${k}`);
    }
    assert.match(b.source_url, /^https:\/\/docs\.waveshare\.com\//, `${id}: vendor url`);
    assert.match(b.verified_utc, /^\d{4}-\d{2}-\d{2}$/, `${id}: verified_utc date`);
    assert.ok(b.facts.pins?.lcd?.length >= 20, `${id}: full LCD pin map`);
    assert.ok(b.facts.pins?.touch?.length >= 1, `${id}: touch pin map`);
    assert.match(String(b.facts.content_hash), /^sha256:[0-9a-f]{64}$/, `${id}: content_hash`);
  }
});

test("parser: still extracts pins from the vendor table shapes (fixture)", () => {
  const facts = JSON.parse(execFileSync("python3", [
    ROOT + "canary-local/tools/gen_board_facts.py",
    "--emit-facts", "--from-file",
    ROOT + "canary-local/tests/fixtures/board_facts_sample.html",
  ], { encoding: "utf8" }));
  // Both onboard styles (<b>…<br> and <strong>…:) parse.
  assert.deepEqual(facts.onboard.map((o) => o.part), ["EXAMPLE-SOC-1", "EXAMPLE-CODEC"]);
  // The LCD table + EXIO row.
  assert.deepEqual(facts.pins.lcd.find((r) => r.signal === "G3"), { pin: "GPIO90", signal: "G3", desc: "Green data bit 3 (fake)" });
  assert.ok(facts.pins.lcd.some((r) => r.pin === "EXIO2" && r.signal === "DISP"));
  // The "…Pin Mapping Table" summary style resolves to touch.
  assert.ok(facts.pins.touch.some((r) => r.pin === "GPIO92"));
  // The I2C summary whose DESCRIPTION mentions "touch" must NOT be
  // misclassified as touch — head-match only.
  assert.ok(facts.pins.i2c?.some((r) => r.pin === "GPIO93"), "i2c head-match");
});

test("drift-lock: firmware pins.h RGB/LCD map == the vendor snapshot, per board", () => {
  for (const [id, path] of Object.entries(PINS_H)) {
    const board = snap.boards[id];
    assert.ok(board, `${id} missing from snapshot`);
    const defs = pinDefs(read(path));
    const vendor = {};
    for (const r of board.facts.pins.lcd) {
      if (LCD_SIGNALS.includes(r.signal)) {
        const g = /^GPIO(\d+)$/.exec(r.pin);
        if (g) vendor[r.signal] = Number(g[1]);
      }
    }
    // Every RGB/sync signal the vendor lists must equal our LCD_PIN_<signal>.
    let checked = 0;
    for (const sig of LCD_SIGNALS) {
      if (!(sig in vendor)) continue;
      const key = "LCD_PIN_" + sig;
      assert.ok(key in defs, `${id}: pins.h is missing ${key}`);
      assert.equal(defs[key], vendor[sig],
        `${id}: ${key}=${defs[key]} but vendor ${sig}=GPIO${vendor[sig]} — our model drifted from the vendor pin map`);
      checked++;
    }
    assert.ok(checked >= 20, `${id}: expected to drift-lock ≥20 RGB/sync pins, checked ${checked}`);
  }
});
