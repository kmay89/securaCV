// canary-local/tests/operator.test.js — the Operator's Bench honesty gate.
//
// Like tests/vault.test.js, this cross-checks devices/operator.json against its
// single source of truth — the break-glass operator CLI (src/break_glass/cli.rs)
// and the quorum rules (src/break_glass/core.rs). Every command, flag, guard and
// recorded message the page shows must still exist in the code it claims, so a
// CLI rename or a drifted message fails here, not in front of an operator.

const { test } = require("node:test");
const assert = require("node:assert");
const { readFileSync } = require("node:fs");
const { join } = require("node:path");

const ROOT = join(__dirname, "..");
const REPO = join(ROOT, "..");
const read = (p) => readFileSync(p, "utf8");

const data = JSON.parse(read(join(ROOT, "devices/operator.json")));
const cliRs = read(join(REPO, "src/break_glass/cli.rs"));
const coreRs = read(join(REPO, "src/break_glass/core.rs"));
const opGuide = read(join(REPO, "docs/operator_guide.md"));

// ── 1. shape + sanity floors ───────────────────────────────────────────────
test("operator.json has every section the page needs", () => {
  for (const k of ["commands", "concepts", "ceremony", "doctor", "drill", "docs"])
    assert.ok(data[k], "missing section: " + k);
  assert.deepStrictEqual(data.commands.map((c) => c.name), ["init", "trustee enroll", "drill", "doctor"]);
  assert.strictEqual(data.ceremony.steps.length, 6);
  assert.ok(data.concepts.length >= 5, "concepts too thin");
  for (const c of data.commands) assert.ok(c.flags.length >= 2, "command " + c.name + " has no flags");
});

// ── 2. the four commands + their guards trace to src/break_glass/cli.rs ─────
test("the operator commands are the CLI's own", () => {
  for (const anchor of ["Init {", "TrusteeCommand", "Enroll {", "Drill {", "Doctor {",
    "fn cmd_init", "fn cmd_trustee_enroll", "fn cmd_drill", "fn cmd_doctor"])
    assert.ok(cliRs.includes(anchor), "CLI surface drifted: " + anchor);
  // enroll modes + the guards the page describes
  assert.ok(cliRs.includes("--public-key") && cliRs.includes("--generate"), "enroll modes drifted");
  assert.ok(cliRs.includes("minted a signing key"), "generate-mode message drifted");
  assert.ok(cliRs.includes("already enrolled"), "duplicate-id guard drifted");
  assert.ok(cliRs.includes("quorum independence requires distinct keys"), "duplicate-key guard drifted");
  assert.ok(cliRs.includes("no setup in progress"), "enroll-before-init guard drifted");
});

// ── 3. the draft-state store really is kept apart from the committed policy ──
test("the draft is separate and the commit path always validates", () => {
  assert.ok(cliRs.includes("SetupDraft"), "draft struct drifted");
  assert.ok(cliRs.includes("setup-draft.json"), "draft file naming drifted");
  assert.ok(cliRs.includes("set_break_glass_policy"), "commit path drifted");
  // core validate() rejects an empty roster (why the draft has to be separate)
  assert.ok(coreRs.includes("quorum must include at least one trustee"), "validate empty-roster guard drifted");
  assert.ok(coreRs.includes("trustees_used.len() >= policy.n"), "grant rule drifted");
  // the concept blurb must say the draft holds no secrets
  const draftConcept = data.concepts[0];
  assert.ok(/<db>\.setup-draft\.json/.test(draftConcept.blurb), "draft file name not shown");
});

// ── 4. quorum bounds trace to core.rs ──────────────────────────────────────
test("MAX_TRUSTEES is the kernel's own", () => {
  assert.ok(coreRs.includes("MAX_TRUSTEES: usize = " + data.max_trustees), "MAX_TRUSTEES drifted");
});

// ── 5. every recorded ceremony line traces to a real CLI message ───────────
test("the recorded ceremony output matches the shipped CLI's behavior", () => {
  const cer = data.ceremony;
  assert.strictEqual(cer.threshold, 2);
  assert.strictEqual(cer.target, 3);
  // the policy commits ONCE, when the roster is complete (step index 3 = 3rd
  // enrollment) — never at the threshold; the CLI's enroll path commits only
  // when trustees.len() == target_trustees.
  assert.strictEqual(cer.steps[1].policy, null, "1 trustee must not commit a policy");
  assert.strictEqual(cer.steps[2].policy, null, "2 of 3 must not commit — the roster is incomplete");
  assert.strictEqual(cer.steps[3].policy, "2-of-3", "policy commits once, at the complete roster");
  assert.ok(cliRs.includes("draft.trustees.len() == draft.target_trustees"), "commit-at-complete-roster rule drifted");
  const liveLines = cer.steps.flatMap((s) => s.out.filter((l) => l.includes("quorum policy is live")));
  assert.deepStrictEqual(liveLines, ["  ✓ quorum policy is live: 2-of-3"], "exactly one commit line, at the target");
  // the rosters grow monotonically to the target
  assert.deepStrictEqual(cer.steps.map((s) => s.roster.length), [0, 1, 2, 3, 3, 3]);
  // the load-bearing message strings must exist verbatim in the CLI
  for (const needle of ["device identity pinned", "quorum policy is live", "DRILL PASSED",
    "Result: HEALTHY", "device public key pinned", "the master key is plaintext on disk"])
    assert.ok(cliRs.includes(needle), "CLI message drifted: " + JSON.stringify(needle));
});

// ── 6. the drill really is a sandbox that burns its token ──────────────────
test("the drill's sandbox + single-use claims trace to the CLI", () => {
  assert.ok(cliRs.includes("Nothing real is touched"), "drill sandbox promise drifted");
  assert.ok(cliRs.includes("consume_break_glass_token_durably"), "drill single-use burn drifted");
  assert.ok(data.drill.exercises.length >= 4, "drill steps too thin");
});

// ── 7. the operator guide documents the guided setup the page teaches ──────
test("the operator guide has the guided-setup section", () => {
  assert.ok(opGuide.includes("Guided setup"), "operator guide guided-setup section missing");
  assert.ok(opGuide.includes("trustee enroll"), "operator guide enroll reference missing");
});
