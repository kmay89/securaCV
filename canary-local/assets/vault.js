// canary-local/assets/vault.js — The Vault page driver.
//
// Renders vault.html entirely from devices/vault.json — the generated,
// drift-gated data (tools/gen_vault.py parses src/vault, src/break_glass,
// the firmware seal, and spec/invariants.md). The page hardcodes no constant,
// algorithm name, domain string or invariant: a code change re-lines every
// section, and CI fails if vault.json drifts from the source.
//
// Failure posture (same as The Hub): if the data can't load, degrade to a
// plain pointer at the design doc — never a blank screen.

import { buildSealWalk, buildQuorumDemo } from "./vault-ui.js";
import { buildTerminal } from "./hub-term.js";

const $ = (sel, root = document) => root.querySelector(sel);
const el = (tag, cls, text) => {
  const n = document.createElement(tag);
  if (cls) n.className = cls;
  if (text != null) n.textContent = text;
  return n;
};
const GH = "https://github.com/kmay89/securaCV/blob/main/";

async function main() {
  const mount = $("#vault");
  let data;
  try {
    const res = await fetch("devices/vault.json");
    if (!res.ok) throw new Error("HTTP " + res.status);
    data = await res.json();
    for (const k of ["concepts", "device_seal", "kernel_vault", "quorum", "invariants", "demo", "terminal"])
      if (!data[k]) throw new Error("missing section: " + k);
  } catch (e) {
    mount.append(el("p", "muted",
      "The Vault page's data failed to load (" + e.message + "). It's all written out in " +
      "docs/sealed_snapshot_vault.md and spec/invariants.md."));
    const a = el("a", null, "Open the design doc →");
    a.href = GH + "docs/sealed_snapshot_vault.md";
    mount.append(a);
    return;
  }

  renderConcepts(data.concepts);
  renderVault(data.device_seal);
  renderSealed(data.device_seal, data.kernel_vault);
  renderQuorum(data.quorum, data.demo);
  renderOperator(data.terminal, data.quorum, data.device_seal);
  renderInvariants(data.invariants);
  renderKeepGoing(data.docs);

  function section(id, kicker, title, lede) {
    const s = el("section", "hub-section");
    s.id = id;
    if (kicker) s.append(el("div", "hub-kicker", kicker));
    if (title) s.append(el("h2", null, title));
    if (lede) s.append(el("p", "hub-lede", lede));
    mount.append(s);
    return s;
  }

  // ── §concepts: the three ideas, up front ──
  function renderConcepts(concepts) {
    const s = section("concepts", "the three words", "Vault, Sealed, Quorum",
      "Three ideas that fit together: a lockbox the device can fill but not open, " +
      "a cryptographic wrapper that can't be edited without breaking, and a rule that " +
      "no one gets the evidence back out alone.");
    const grid = el("div", "vault-concepts");
    for (const c of concepts) {
      const card = el("div", "vault-concept");
      card.append(el("h3", null, c.title), el("p", "body", c.blurb));
      grid.append(card);
    }
    s.append(grid);
  }

  // ── §vault: write-only escrow ──
  function renderVault(ds) {
    const s = section("vault", "the lockbox", "A vault the device can't open",
      "On the Canary, sealed evidence is opt-in and event-triggered — a single frame on a " +
      "life-safety alarm, sealed against a key the device doesn't hold. It's write-only escrow: " +
      "put things in, never read them back.");
    const off = el("div", "ondevice vault-off");
    off.append(el("strong", null, "Everything is off by default. "), document.createTextNode(ds.defaults_note));
    s.append(off);

    const grid = el("div", "vault-two");
    // escrow explainer
    const left = el("div", "vault-two-col");
    left.append(el("h3", "vault-col-h", "Write-only escrow"), el("p", "body", ds.escrow),
      el("p", "muted fineprint", ds.review));
    // decision table
    const right = el("div", "vault-two-col");
    right.append(el("h3", "vault-col-h", "Fail-closed: when a frame is (not) captured"));
    const tbl = el("div", "vault-decisions");
    for (const d of ds.decision_table) {
      const row = el("div", "vault-decision" + (/CAPTURE/.test(d.decision) ? " cap" : ""));
      row.append(el("span", "vault-cond", d.condition), el("code", "vault-dec", d.decision));
      tbl.append(row);
    }
    right.append(tbl);
    grid.append(left, right);
    s.append(grid);

    const facts = el("div", "vault-facts");
    for (const [k, v] of [
      ["Storage", `newest ${ds.storage.keep_files} frames on the SD card (a ring); nothing pushed anywhere`],
      ["Cooldown", `${ds.storage.cooldown_default_s}s per trigger (default), ${ds.storage.max_ciphertext_kb}KB max per frame`],
      ["On the chain", ds.witness_note],
    ]) {
      const row = el("div", "vault-fact");
      row.append(el("span", "vault-fact-k", k), el("span", "vault-fact-v", v));
      facts.append(row);
    }
    s.append(facts);
  }

  // ── §sealed: the crypto walkthrough ──
  function renderSealed(ds, kv) {
    const s = section("sealed", "the wrapper", "How a seal is made",
      "A fresh key per frame, authenticated encryption, and a header that's signed along with " +
      "the image — so the trigger, the time and every byte are tamper-evident. Watch it built, " +
      "then flip a header byte and see it refuse to open.");
    s.append(buildSealWalk(ds));

    const kvBox = el("div", "vault-kernel");
    kvBox.append(el("h3", "vault-col-h", "The same idea in the witness kernel"));
    kvBox.append(el("p", "body",
      `The Rust kernel seals raw frames into "${kv.magic}" (${kv.envelope}) envelopes with ${kv.aead} — ` +
      kv.dek_model));
    const chips = el("div", "hub-chips");
    for (const [k, v] of [["envelope", kv.magic], ["AEAD", kv.aead], ["modes", kv.modes.join(" · ")], ["pinned by", kv.kat]]) {
      const c = el("span", "chip");
      c.append(el("span", "hub-chip-k", k + " "), el("strong", null, v));
      chips.append(c);
    }
    kvBox.append(chips, el("p", "muted fineprint", kv.wired + " " + kv.pq_note));
    s.append(kvBox);
  }

  // ── §quorum: the interactive break-glass demo ──
  function renderQuorum(quorum, demo) {
    const s = section("quorum", "break-glass", "No one opens it alone",
      quorum.model + " Below the threshold the vault stays shut; at the threshold a single-use, " +
      "audited token is minted. Try it — the signatures below are real.");
    s.append(buildQuorumDemo(demo, quorum));

    const guards = el("div", "vault-guards");
    guards.append(el("h3", "vault-col-h", "Why it can't be cheated"));
    const grid = el("div", "vault-guard-grid");
    for (const g of quorum.guardrails) {
      const card = el("div", "vault-guard");
      card.append(el("strong", null, g.title), el("span", "muted", g.detail));
      grid.append(card);
    }
    guards.append(grid);
    s.append(guards);
  }

  // ── §operator: the real tools ──
  function renderOperator(term, quorum, ds) {
    const s = section("operator", "the tools", "The operator's console",
      "None of this is a web toy: the commands below are the ones in the repo. There's also a real " +
      "4-step break-glass console (src/break_glass/breakglass.html) that signs Ed25519 in the browser, " +
      "the same way this page does.");
    s.append(buildTerminal(term, {}));

    const api = el("div", "vault-two");
    const c1 = el("div", "vault-two-col");
    c1.append(el("h3", "vault-col-h", "Device vault — HTTP"));
    const t1 = el("div", "vault-routes");
    for (const r of ds.http) {
      const row = el("div", "vault-route");
      row.append(el("code", "vault-m", r.method), el("code", "vault-p", r.path));
      t1.append(row);
    }
    c1.append(t1);
    const c2 = el("div", "vault-two-col");
    c2.append(el("h3", "vault-col-h", "Break-glass — HTTP + CLI"));
    const t2 = el("div", "vault-routes");
    for (const r of quorum.http) {
      const row = el("div", "vault-route");
      row.append(el("code", "vault-m", r.method), el("code", "vault-p", r.path));
      t2.append(row);
    }
    c2.append(t2, el("p", "muted fineprint", "CLI: " + quorum.cli.map((c) => "break_glass " + c).join(" · ")));
    api.append(c1, c2);
    s.append(api);
  }

  // ── §invariants ──
  function renderInvariants(invs) {
    const s = section("invariants", "the promises", "The two invariants behind it",
      "The vault isn't a feature bolted on — it's how two of the kernel's constitutional invariants " +
      "are kept.");
    const grid = el("div", "vault-invariants");
    for (const inv of invs) {
      const card = el("div", "vault-invariant");
      card.append(el("div", "vault-inv-id", "Invariant " + inv.id),
        el("h4", null, inv.title), el("p", "body", inv.text),
        el("code", "fineprint", inv.ref));
      grid.append(card);
    }
    s.append(grid);
  }

  // ── §keep going ──
  function renderKeepGoing(docs) {
    const s = section("more", "keep going", "The code + docs", null);
    const grid = el("div", "hub-links");
    const items = [
      ["Sealed snapshot design", "the device-side seal: crypto, .svlt layout, threat-model sign-off", docs.design],
      ["The invariants", "the constitutional definitions of No-Raw-Export and Break-Glass-by-Quorum", docs.invariants],
      ["The vault kernel", "seal_v2 / EnvelopeV2 / DEK-wrap, with an RFC-8439 known-answer test", docs.kernel_vault],
      ["Break-glass", "the M-of-N quorum, receipts, tokens, CLI and HTTP server", docs.break_glass],
      ["The operator console", "the real 4-step break-glass web console (in-browser Ed25519)", docs.console],
      ["unseal_snapshot.py", "gen-key / inspect / unseal — the off-device review tool", docs.unseal_tool],
    ];
    for (const [title, body, path] of items) {
      const a = el("a", "hub-link");
      a.href = GH + path;
      a.target = "_blank"; a.rel = "noopener noreferrer";
      a.append(el("strong", null, title), el("span", "muted", body), el("code", "fineprint", path));
      grid.append(a);
    }
    s.append(grid);
  }
}

main();
