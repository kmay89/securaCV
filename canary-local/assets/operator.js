// canary-local/assets/operator.js — The Operator's Bench page driver.
//
// Renders operator.html entirely from devices/operator.json — the generated,
// drift-gated data (tools/gen_operator.py parses src/break_glass/cli.rs + core).
// The page hardcodes no command, flag or message: a CLI change re-lines every
// section, and CI fails if operator.json drifts from the source.
//
// Failure posture (same as the other pages): if the data can't load, degrade to
// a plain pointer at the operator guide — never a blank screen.

const $ = (sel, root = document) => root.querySelector(sel);
const el = (tag, cls, text) => {
  const n = document.createElement(tag);
  if (cls) n.className = cls;
  if (text != null) n.textContent = text;
  return n;
};
const GH = "https://github.com/kmay89/securaCV/blob/main/";

async function main() {
  const mount = $("#operator");
  let data;
  try {
    const res = await fetch("devices/operator.json");
    if (!res.ok) throw new Error("HTTP " + res.status);
    data = await res.json();
    for (const k of ["commands", "concepts", "ceremony", "doctor", "drill"])
      if (!data[k]) throw new Error("missing section: " + k);
  } catch (e) {
    mount.append(el("p", "muted",
      "The Operator's Bench data failed to load (" + e.message + "). It's all written " +
      "out in docs/operator_guide.md and docs/design/vault_operator_ux_v1_1.md."));
    const a = el("a", null, "Open the operator guide →");
    a.href = GH + "docs/operator_guide.md";
    mount.append(a);
    return;
  }

  if (data.lede) mount.append(el("p", "op-lede body", data.lede));
  renderCommands(data.commands);
  renderConcepts(data.concepts);
  renderCeremony(data.ceremony);
  renderDoctor(data.doctor);
  renderDrill(data.drill);
  renderKeepGoing(data.docs, data.max_trustees);

  function section(id, kicker, title, lede) {
    const s = el("section", "hub-section");
    s.id = id;
    if (kicker) s.append(el("div", "hub-kicker", kicker));
    if (title) s.append(el("h2", null, title));
    if (lede) s.append(el("p", "hub-lede", lede));
    mount.append(s);
    return s;
  }

  // ── §commands: the four, as cards ──
  function renderCommands(cmds) {
    const s = section("commands", "four commands", "The whole lifecycle",
      "Setting up and operating a break-glass vault is four commands — two to build it, " +
      "two to trust it.");
    const grid = el("div", "op-cmds");
    for (const c of cmds) {
      const card = el("div", "op-cmd");
      const head = el("div", "op-cmd-head");
      head.append(el("code", "op-cmd-name", c.name), el("span", "op-cmd-one", c.one_line));
      card.append(head, el("code", "op-cmd-usage", c.usage), el("p", "body", c.what));
      const flags = el("div", "op-flags");
      for (const f of c.flags) {
        const row = el("div", "op-flag");
        row.append(el("code", "op-flag-k", f.flag), el("span", "op-flag-v", f.desc));
        flags.append(row);
      }
      card.append(flags);
      grid.append(card);
    }
    s.append(grid);
  }

  // ── §concepts ──
  function renderConcepts(concepts) {
    const s = section("concepts", "how it holds together", "Five things worth knowing",
      "The ideas that make the four commands safe: why the draft is kept apart, when the " +
      "gate goes live, and what the health check is honest about.");
    const grid = el("div", "vault-concepts");
    for (const c of concepts) {
      const card = el("div", "vault-concept");
      card.append(el("h3", null, c.title), el("p", "body", c.blurb));
      grid.append(card);
    }
    s.append(grid);
  }

  // ── §ceremony: the interactive centerpiece ──
  // A terminal that accumulates steps 0..i beside a live "vault state" panel —
  // watch the roster fill and the committed policy flip from draft → live the
  // instant the quorum is valid. Recorded output, straight from the binary.
  function renderCeremony(cer) {
    const s = section("ceremony", "walk it", "From zero to a rehearsed quorum", cer.note);
    const wrap = el("div", "op-ceremony");
    const term = el("div", "op-term");
    const state = el("div", "op-state");
    wrap.append(term, state);

    const ctl = el("div", "op-ctl");
    const prev = el("button", "op-btn", "◀ Prev");
    const dots = el("div", "op-dots");
    const next = el("button", "op-btn op-btn-primary", "Next ▶");
    ctl.append(prev, dots, next);
    const note = el("p", "note op-note");
    s.append(wrap, ctl, note);

    const steps = cer.steps;
    let i = 0;
    const dotEls = steps.map((_, k) => {
      const d = el("button", "op-dot", String(k + 1));
      d.setAttribute("aria-label", "step " + (k + 1));
      d.onclick = () => { i = k; paint(); };
      dots.append(d);
      return d;
    });

    function paint() {
      // terminal: steps 0..i, current one highlighted
      term.textContent = "";
      for (let k = 0; k <= i; k++) {
        const st = steps[k];
        const block = el("div", "op-term-block" + (k === i ? " on" : ""));
        block.append(el("div", "op-term-cmd", "$ " + st.cmd));
        for (const line of st.out) block.append(el("div", "op-term-out", line));
        term.append(block);
      }
      term.scrollTop = term.scrollHeight;

      // live vault state for step i
      const st = steps[i];
      state.textContent = "";
      state.append(
        el("div", "op-state-h", "Vault state"),
        el("div", "op-state-target", "Quorum target: " + cer.threshold + "-of-" + cer.target),
        el("div", "op-state-k", "Trustees enrolled"),
      );
      const roster = el("div", "op-roster");
      for (let r = 0; r < cer.target; r++) {
        const name = st.roster[r];
        const chip = el("div", "op-tr" + (name ? " on" : ""), name ? name : "—");
        roster.append(chip);
      }
      state.append(roster, el("div", "op-state-k", "Committed policy"));
      const live = !!st.policy;
      state.append(el("div", "op-badge " + (live ? "op-badge-live" : "op-badge-draft"),
        live ? "live · " + st.policy : "draft · no policy yet"));

      note.textContent = st.note;
      dotEls.forEach((d, k) => d.classList.toggle("on", k === i));
      prev.disabled = i === 0;
      next.disabled = i === steps.length - 1;
    }
    prev.onclick = () => { if (i > 0) { i--; paint(); } };
    next.onclick = () => { if (i < steps.length - 1) { i++; paint(); } };
    paint();
  }

  // ── §doctor ──
  function renderDoctor(doc) {
    const s = section("doctor", "trust it", "What doctor checks",
      "A read-only health check that " + doc.gates_deploy);
    const list = el("div", "op-checks");
    for (const c of doc.checks) {
      const row = el("div", "op-check");
      const body = el("div", "op-check-body");
      if (c.ok && c.ok !== "—") body.append(el("div", "op-check-line op-ok", "✓ " + c.ok));
      if (c.warn) body.append(el("div", "op-check-line op-warn", "⚠ " + c.warn));
      if (c.fail) body.append(el("div", "op-check-line op-fail", "✗ fails on: " + c.fail));
      row.append(el("div", "op-check-sec", c.section), body);
      list.append(row);
    }
    s.append(list);
  }

  // ── §drill ──
  function renderDrill(drill) {
    const s = section("drill", "rehearse it", "The drill is a real rehearsal",
      "Before you trust the vault, prove it. " + drill.why);
    const box = el("div", "ondevice op-drill");
    box.append(el("strong", null, "Sandbox: "), document.createTextNode(drill.sandbox));
    s.append(box);
    const ol = el("ol", "op-drill-steps");
    for (const e of drill.exercises) ol.append(el("li", null, e));
    s.append(ol);
  }

  // ── §keep going ──
  function renderKeepGoing(docs, maxTrustees) {
    const s = section("keep-going", "keep going", "The code behind the bench", null);
    const p = el("p", "body");
    p.append(document.createTextNode(
      "Up to " + maxTrustees + " trustees, no built-in defaults, every commit validated. " +
      "What a vault, a seal and a quorum actually are — with a live break-glass demo — lives on "));
    const a = el("a", null, "The Vault");
    a.href = "vault.html";
    p.append(a, document.createTextNode(". The code and docs:"));
    s.append(p);
    const chips = el("div", "hub-chips");
    for (const [label, href] of [
      ["operator CLI", docs.cli],
      ["operator guide", docs.operator_guide],
      ["v1.1 design", docs.design],
    ]) {
      const c = el("a", "hub-chip");
      c.href = GH + href;
      c.textContent = label;
      chips.append(c);
    }
    s.append(chips);
  }
}

main();
