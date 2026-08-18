// canary-local/tests/hub_wizard.test.js — the Hub setup wizard's content contract.
//
// The wizard exists so nobody gives up on Home Assistant + MQTT. This pins the
// promises that make that true: it explains BOTH words, every step has a
// concrete "Stuck?" way forward (no dead ends), the honest facts (minimum HA,
// integration version) flow in from the catalog, and the broker port is stated
// where the user must type it.

const { test } = require("node:test");
const assert = require("node:assert");
const { readFileSync } = require("node:fs");
const { join } = require("node:path");

const ROOT = join(__dirname, "..");
const ha = JSON.parse(readFileSync(join(ROOT, "devices/homeassistant.json"), "utf8"));
const mod = () => import("../assets/hub-setup-wizard.js");

const facts = {
  min_ha: ha.integration.min_ha,
  integration_version: ha.integration.version,
  doc: ha.docs.setup,
};

test("wizard opens with the goal — what you gain and why it's worth it", async () => {
  const { wizardSteps } = await mod();
  const first = wizardSteps(facts)[0];
  assert.strictEqual(first.id, "why", "the very first step sells the payoff");
  const blob = JSON.stringify(first).toLowerCase();
  // the motivation: the goal, the concrete gains, and the honesty hooks
  assert.ok(/you gain/.test(blob), "spells out what the user gains");
  assert.ok(/alert/.test(blob), "names the payoff: alerts");
  assert.ok(/no cloud|no account|no monthly fee|no subscription/.test(blob),
    "names why it's worth it: local-only, no fee");
  assert.ok(/what happened.*never who|records events, not faces/.test(blob),
    "keeps the privacy promise (events, not identities)");
});

test("wizard explains both Home Assistant AND MQTT in plain words", async () => {
  const { wizardSteps } = await mod();
  const what = wizardSteps(facts).find((s) => s.id === "what");
  assert.ok(what, "there is a 'what are these two words' step");
  assert.ok(/home assistant/i.test(what.what), "explains Home Assistant");
  assert.ok(/mqtt/i.test(what.what), "explains MQTT");
  // no cloud / no account — the reassurance that keeps people going
  assert.ok(/no cloud|nothing leaves|local/i.test(what.what));
});

test("every step has guidance and a no-dead-end 'Stuck?' path", async () => {
  const { wizardSteps } = await mod();
  const steps = wizardSteps(facts);
  assert.ok(steps.length >= 6, "a real multi-step walkthrough");
  for (const s of steps) {
    assert.ok(s.id && s.title && s.what, `${s.id || "?"}: needs id/title/what`);
    assert.ok(Array.isArray(s.stuck) && s.stuck.length,
      `${s.id}: every step must offer a way forward when stuck`);
  }
  // exactly one terminal "you did it" step
  assert.strictEqual(steps.filter((s) => s.done).length, 1);
});

test("the honest facts flow in from the catalog (no drift)", async () => {
  const { wizardSteps } = await mod();
  const text = JSON.stringify(wizardSteps(facts));
  assert.ok(text.includes(ha.integration.min_ha), "minimum HA version is shown");
  assert.ok(text.includes(ha.integration.version), "integration version is shown");
});

test("the broker port is stated where the user must type it", async () => {
  const { wizardSteps } = await mod();
  const steps = wizardSteps(facts);
  const point = steps.find((s) => s.id === "point-canary");
  assert.ok(point, "there is a 'point your Canary at the broker' step");
  const blob = JSON.stringify(point);
  assert.ok(blob.includes("1883"), "the standard MQTT port is spelled out");
  // and it names the four things a user actually enters
  assert.ok(point.values.some((v) => /host/i.test(v.label)));
  assert.ok(point.values.some((v) => /port/i.test(v.label)));
});

test("the broker-add step names Mosquitto (the one-click add-on)", async () => {
  const { wizardSteps } = await mod();
  const broker = wizardSteps(facts).find((s) => s.id === "broker");
  assert.ok(broker && /mosquitto/i.test(JSON.stringify(broker)), "names Mosquitto");
});

test("the optional hub screen is offered without undoing the headless promise", async () => {
  const { wizardSteps } = await mod();
  const steps = wizardSteps(facts);
  // The install step keeps the truth that started it all: no monitor needed.
  const install = steps.find((s) => s.id === "install-ha");
  assert.ok(/never needs a monitor/i.test(install.what),
    "headless stays the stated default");
  // And the finish line tells a screen-owner where the dashboard-on-the-hub
  // option lives (HAOSKiosk / the desktop app's display extra) — a copy sweep
  // must not silently drop the only mention of it.
  const done = steps.find((s) => s.done);
  const blob = JSON.stringify(done);
  assert.ok(/haos.kiosk/i.test(blob), "names the kiosk app for a hub screen");
  assert.ok(/headless hubs skip this/i.test(blob),
    "says plainly that skipping it loses nothing");
});
