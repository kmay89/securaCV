// canary-local/tests/start.test.js — the Get Started guide's honesty gate.
//
// The page promises: no version written twice, no install string the
// README doesn't carry, no command template that renders broken, no doc
// link to a file that doesn't exist, and a Linux flash path that IS the
// Hub bench's chapter rather than a drifting copy. This test pins each
// promise; CI additionally regenerates start.json and diffs (drift gate).

const { test } = require("node:test");
const assert = require("node:assert");
const { readFileSync, existsSync } = require("node:fs");
const { join } = require("node:path");

const ROOT = join(__dirname, "..");
const REPO = join(ROOT, "..");
const data = JSON.parse(readFileSync(join(ROOT, "devices/start.json"), "utf8"));
const hub = JSON.parse(readFileSync(join(ROOT, "devices/homeassistant.json"), "utf8"));
const registry = JSON.parse(readFileSync(join(ROOT, "devices/registry.json"), "utf8"));
const readme = readFileSync(join(REPO, "README.md"), "utf8");

const vars = {
  haos: data.upstream.haos_version,
  ha: data.upstream.ha_version,
  integration: data.integration.version,
  min_ha: data.integration.min_ha,
};

const OS_IDS = new Set(["all", "mac", "win", "linux"]);

test("versions are single-sourced from the Hub's snapshot", () => {
  assert.deepStrictEqual(data.upstream, hub.upstream);
  assert.strictEqual(data.integration.version, hub.integration.version);
  assert.strictEqual(data.integration.min_ha, hub.integration.min_ha);
  assert.strictEqual(data.fw_train, registry.fw_train);
});

test("exactly three computers, each with an id the renderer knows", () => {
  assert.strictEqual(data.oses.length, 3);
  for (const o of data.oses) {
    assert.ok(OS_IDS.has(o.id) && o.id !== "all", `unknown OS id ${o.id}`);
    assert.ok(o.label && o.glyph, `${o.id}: needs label + glyph`);
  }
});

test("install strings on the page are the README's own", () => {
  assert.ok(readme.includes(data.readme.frigate_addons_repo), "Frigate repo URL vanished from README");
  assert.ok(readme.includes(data.readme.securacv_repo), "SecuraCV repo URL vanished from README");
  assert.ok(readme.includes(data.readme.curl_line), "curl one-liner vanished from README");
});

test("every mission is well-formed and every template expands clean", async () => {
  const { expandVars } = await import("../assets/hub-term.js");
  assert.ok(data.missions.length >= 3, "at least three ways in");
  const missionIds = new Set(data.missions.map((m) => m.id));
  for (const m of data.missions) {
    assert.ok(m.chapters.length > 0, `${m.id}: empty mission`);
    for (const ch of m.chapters) {
      assert.ok(ch.steps.length > 0, `${m.id}/${ch.id}: empty chapter`);
      for (const step of ch.steps) {
        const keys = Object.keys(step.variants);
        assert.ok(keys.length > 0, `${m.id}/${ch.id}: step '${step.title}' has no variants`);
        for (const k of keys) assert.ok(OS_IDS.has(k), `${m.id}/${ch.id}: unknown variant '${k}'`);
        if (step.next_mission) assert.ok(missionIds.has(step.next_mission),
          `${m.id}: next_mission '${step.next_mission}' doesn't exist`);
        for (const v of Object.values(step.variants)) {
          const texts = [
            ...(v.bullets || []),
            ...(v.cmds || []).flatMap((c) => [c.cmd, ...(c.out || []), c.note || ""]),
            ...(v.copies || []).map((c) => c.text),
            v.danger || "",
          ];
          for (const t of texts) {
            const x = expandVars(t, vars);
            assert.ok(!x.includes("{{"), `${m.id}/${ch.id}: unresolved template: ${x}`);
          }
        }
      }
    }
  }
});

test("the Linux flash path IS the Hub bench's chapter, not a copy", () => {
  const hubFlash = hub.terminal.chapters.find((c) => c.id === "flash");
  const mission = data.missions.find((m) => m.id === "hub");
  const step = mission.chapters.find((c) => c.id === "flash").steps[0];
  const linux = step.variants.linux;
  assert.deepStrictEqual(
    linux.cmds.map((c) => c.cmd),
    hubFlash.steps.map((s) => s.cmd),
    "linux flash commands must match homeassistant.json verbatim");
  assert.ok(linux.danger, "the dd path must carry its warning");
});

test("every referenced doc path exists in the repo", () => {
  const paths = new Set(Object.values(data.docs));
  for (const m of data.missions)
    for (const ch of m.chapters)
      for (const s of ch.steps) if (s.doc) paths.add(s.doc);
  for (const p of paths)
    assert.ok(existsSync(join(REPO, p)), `doc path does not exist: ${p}`);
});

test("progress model: toggle, persist, reset — on a plain store", async () => {
  const { createProgress, stepKeys, variantFor } = await import("../assets/start.js");
  const mem = new Map();
  const store = { get: (k) => mem.get(k), set: (k, v) => mem.set(k, v) };
  const p = createProgress("test", store);
  assert.strictEqual(p.count(), 0);
  assert.strictEqual(p.toggle("0:0"), true);
  assert.strictEqual(p.has("0:0"), true);
  const p2 = createProgress("test", store);
  assert.strictEqual(p2.has("0:0"), true, "progress persists through the store");
  p2.reset();
  assert.strictEqual(createProgress("test", store).count(), 0, "reset clears");

  const mission = data.missions[0];
  const keys = stepKeys(mission);
  const total = mission.chapters.reduce((n, c) => n + c.steps.length, 0);
  assert.strictEqual(keys.length, total, "one key per step");

  const step = { variants: { all: { bullets: ["x"] }, linux: { cmds: [] } } };
  assert.strictEqual(variantFor(step, "linux"), step.variants.linux);
  assert.strictEqual(variantFor(step, "mac"), step.variants.all, "falls back to 'all'");
});
