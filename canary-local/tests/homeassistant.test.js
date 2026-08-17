// canary-local/tests/homeassistant.test.js — The Hub's honesty gate.
//
// The Hub page promises: no version written twice, no entity the doc
// doesn't promise, no command template that renders broken, no 3D part
// that doesn't exist. This test pins each promise:
//   · devices/homeassistant.json is consistent with its sources of truth
//     (manifest.json, hacs.json, registry.json, docs/homeassistant_setup.md)
//   · every assembly part resolves to a real procedural builder and every
//     step stages at least one part
//   · every terminal script expands with zero surviving {{templates}}
//   · the replay session enforces the guide's order
//   · the upstream snapshot carries a well-formed, self-reporting date
// (CI additionally regenerates the JSON and diffs — the drift gate.)

const { test } = require("node:test");
const assert = require("node:assert");
const { readFileSync } = require("node:fs");
const { join } = require("node:path");

const ROOT = join(__dirname, "..");
const REPO = join(ROOT, "..");
const data = JSON.parse(readFileSync(join(ROOT, "devices/homeassistant.json"), "utf8"));
const registry = JSON.parse(readFileSync(join(ROOT, "devices/registry.json"), "utf8"));
const manifest = JSON.parse(readFileSync(join(REPO, "custom_components/securacv/manifest.json"), "utf8"));
const hacs = JSON.parse(readFileSync(join(REPO, "hacs.json"), "utf8"));
const doc = readFileSync(join(REPO, "docs/homeassistant_setup.md"), "utf8");

const vars = {
  haos: data.upstream.haos_version,
  ha: data.upstream.ha_version,
  integration: data.integration.version,
  min_ha: data.integration.min_ha,
};

test("versions are single-sourced: JSON ↔ manifest/hacs/registry agree", () => {
  assert.strictEqual(data.integration.version, manifest.version);
  assert.strictEqual(data.integration.domain, manifest.domain);
  assert.strictEqual(data.integration.min_ha, hacs.homeassistant);
  assert.strictEqual(data.fw_train, registry.fw_train);
});

test("upstream snapshot is well-formed and self-dating", () => {
  assert.match(data.upstream.fetched_at, /^\d{4}-\d{2}-\d{2}$/);
  assert.match(String(data.upstream.haos_version), /^\d/);
  assert.match(String(data.upstream.ha_version), /^\d/);
  assert.ok(data.upstream.source.startsWith("https://"), "snapshot must name its source");
});

test("every demo entity is one the doc actually promises", () => {
  const catalog = new Set(data.entity_catalog.map((e) => e.name));
  for (const ent of data.ha_demo.entities) {
    if (ent.from_section === "ota") {
      assert.ok(doc.includes(`**${ent.name}**`), `OTA entity '${ent.name}' vanished from the doc`);
    } else {
      assert.ok(catalog.has(ent.name), `demo entity '${ent.name}' not in the parsed catalog`);
      assert.ok(doc.includes(`**${ent.name}**`), `entity '${ent.name}' not in the doc text`);
    }
    assert.ok(["sensor", "binary_sensor", "switch", "update"].includes(ent.kind), ent.kind);
  }
});

test("every MQTT topic in the JSON is in the doc's topic reference", () => {
  assert.ok(data.topics.length >= 6);
  for (const t of data.topics) assert.ok(doc.includes("`" + t.topic + "`"), t.topic);
});

test("the optional hub screen is a needs row that mirrors the setup guide", () => {
  // The 7" HDMI touchscreen is offered, never required: it must be in the
  // needs list marked optional (so a hub-with-a-screen builder sees it), it
  // must keep headless as the stated default, and — since from_doc claims the
  // guide as its source — the guide's own table must actually carry the row.
  const row = data.hardware.needs.find((n) => /touchscreen/i.test(n.item));
  assert.ok(row, "the optional touchscreen row exists in hardware.needs");
  assert.ok(/optional/i.test(row.item), "it says optional in the item itself");
  assert.ok(/headless by default/i.test(row.note), "headless stays the default");
  assert.ok(row.from_doc, "it cites the setup guide as its source");
  assert.ok(/touchscreen \(optional\)/i.test(doc), "…and the guide's table has it");
});

test("assembly: every part is a real procedural builder; every step staged", async () => {
  const { PARTS } = await import("../assets/hub-parts.js");
  const hw = data.hardware;
  const stepsWithParts = new Set();
  for (const p of hw.parts) {
    assert.strictEqual(p.source, "proc", `${p.id}: hub parts are procedural by design`);
    assert.strictEqual(typeof PARTS[p.part], "function", `unknown builder: ${p.part}`);
    assert.ok(p.step >= 0 && p.step < hw.steps.length, `${p.id}: step ${p.step} out of range`);
    stepsWithParts.add(p.step);
    const meshes = PARTS[p.part](p.params || {});
    assert.ok(Array.isArray(meshes) && meshes.length > 0, `${p.part}: builder returned nothing`);
    for (const m of meshes) {
      assert.ok(m.builder.pos.length > 0 && m.builder.idx.length > 0, `${p.part}: empty mesh`);
      assert.strictEqual(m.builder.pos.length, m.builder.nrm.length, `${p.part}: pos/nrm mismatch`);
    }
  }
  for (let i = 0; i < hw.steps.length; i++)
    assert.ok(stepsWithParts.has(i), `step ${i} ('${hw.steps[i].title}') stages no part`);
});

test("terminal scripts expand with no surviving {{templates}}", async () => {
  const { expandVars } = await import("../assets/hub-term.js");
  for (const ch of data.terminal.chapters) {
    assert.ok(["laptop", "ha-ssh"].includes(ch.host), `${ch.id}: unknown host ${ch.host}`);
    assert.ok(ch.steps.length > 0, `${ch.id}: empty chapter`);
    for (const s of [{ cmd: ch.intro }, ...ch.steps]) {
      for (const text of [s.cmd, ...(s.out || []), s.note || ""]) {
        const expanded = expandVars(text, vars);
        assert.ok(!expanded.includes("{{"),
          `${ch.id}: unresolved template survives expansion: ${expanded}`);
      }
    }
  }
});

test("chapter clipboard: every command expands, and a multi-line paste leads with the review warning", async () => {
  const { chapterCommands, chapterClipboard } = await import("../assets/hub-term.js");
  for (const ch of data.terminal.chapters) {
    const cmds = chapterCommands(ch, vars);
    assert.strictEqual(cmds.length, ch.steps.length, `${ch.id}: one clipboard line per step`);
    for (const c of cmds) assert.ok(!c.includes("{{"), `${ch.id}: unresolved template on the clipboard: ${c}`);
    const clip = chapterClipboard(ch, vars);
    const lines = clip.split("\n");
    // pasting multiple lines RUNS in most terminals — the payload must
    // open with bash-safe comments telling the user to review first
    assert.ok(lines[0].startsWith("# "), `${ch.id}: clipboard must lead with a comment`);
    assert.ok(clip.includes("Review each line"), `${ch.id}: clipboard must carry the review warning`);
    assert.deepStrictEqual(lines.slice(2), cmds, `${ch.id}: commands follow the warning verbatim`);
  }
});

test("the version-bearing surfaces actually use the live vars", async () => {
  // the anti-rot property in one assertion: a HA OS/Core release bump
  // (upstream refresh) must re-line the terminal without a script edit
  const all = JSON.stringify(data.terminal);
  assert.ok(all.includes("{{haos}}"), "flash chapter should template the HA OS version");
  assert.ok(all.includes("{{ha}}"), "broker chapter should template the HA core version");
  assert.ok(all.includes("{{integration}}"), "integration chapter should template the HACS version");
});

test("replay session enforces the guide's order and completes", async () => {
  const { createTermSession, expandVars, PROMPTS } = await import("../assets/hub-term.js");
  const ch = data.terminal.chapters[0];
  const sess = createTermSession(ch, vars);
  assert.strictEqual(sess.index, 0);
  assert.ok(PROMPTS[ch.host], "chapter host has a prompt");
  const first = sess.peek();
  assert.strictEqual(first.cmd, expandVars(ch.steps[0].cmd, vars), "peek shows step 0 first");
  let n = 0;
  while (!sess.done) {
    const s = sess.run();
    assert.ok(s && s.cmd.length > 0);
    n += 1;
    assert.ok(n <= ch.steps.length, "session must terminate");
  }
  assert.strictEqual(n, ch.steps.length);
  assert.strictEqual(sess.run(), null, "a finished chapter refuses to run more");
  sess.reset();
  assert.strictEqual(sess.index, 0, "reset rewinds");
});

test("staleness self-report math", async () => {
  const { daysOld } = await import("../assets/hub-term.js");
  assert.strictEqual(daysOld("2026-01-01", new Date("2026-01-31T12:00:00Z")), 30);
  assert.strictEqual(daysOld("2026-01-01", new Date("2026-01-01T02:00:00Z")), 0);
  assert.strictEqual(daysOld("not-a-date"), null, "garbage dates report null, not NaN");
});

test("the drill's trigger entity exists in the demo's entity list", () => {
  const names = new Set(data.ha_demo.entities.map((e) => e.name));
  assert.ok(names.has(data.ha_demo.drill.trigger_entity));
});

test("docs links point at files that exist", () => {
  const { existsSync } = require("node:fs");
  for (const p of Object.values(data.docs))
    assert.ok(existsSync(join(REPO, p)), `docs link target missing: ${p}`);
});
