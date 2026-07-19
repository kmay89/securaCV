// canary-local/assets/hub-term.js — the bench terminal.
//
// The display emulator runs the real firmware; the CLI can't be "run"
// the same way (the commands flash cards and install OSes), so this is
// the honest next-best thing: a replay bench. Every command is the real
// one from the guide, typed out and answered with a recorded transcript,
// versions substituted live from the page's drift-gated snapshot — so a
// Home Assistant release bump re-lines this terminal without anyone
// editing a script. The window says "simulated" on its face.
//
// Split the chooser's way: a DOM-free core (templating + the ordered
// session state machine, tested in tests/homeassistant.test.js) and a
// small renderer that types.

// ── core (DOM-free) ─────────────────────────────────────────────────────

// {{var}} substitution. Unknown vars are left visible — the tests treat a
// surviving "{{" anywhere in an expanded script as a build-breaking bug,
// so a typo'd template can never quietly reach a visitor.
export function expandVars(str, vars) {
  return String(str).replace(/\{\{(\w+)\}\}/g, (m, k) =>
    Object.prototype.hasOwnProperty.call(vars, k) ? String(vars[k]) : m);
}

// days between an ISO date and "now" — the page's staleness self-report
export function daysOld(isoDate, now = new Date()) {
  const then = new Date(isoDate + "T00:00:00Z");
  if (Number.isNaN(then.getTime())) return null;
  return Math.max(0, Math.floor((now.getTime() - then.getTime()) / 86400000));
}

export const PROMPTS = {
  laptop: "you@laptop:~$",
  "ha-ssh": "[core-ssh ~]$",
};

// Ordered replay session for one chapter: commands only run in sequence
// (the guide's order IS the contract — flashing after booting is not a
// thing this bench will demonstrate).
export function createTermSession(chapter, vars) {
  let idx = 0;
  const total = chapter.steps.length;
  return {
    get index() { return idx; },
    get total() { return total; },
    get done() { return idx >= total; },
    peek() {
      if (idx >= total) return null;
      const s = chapter.steps[idx];
      return {
        cmd: expandVars(s.cmd, vars),
        out: (s.out || []).map((l) => expandVars(l, vars)),
        note: s.note ? expandVars(s.note, vars) : "",
      };
    },
    run() {
      const s = this.peek();
      if (s) idx += 1;
      return s;
    },
    reset() { idx = 0; },
  };
}

// ── renderer ────────────────────────────────────────────────────────────
const el = (tag, cls, text) => {
  const n = document.createElement(tag);
  if (cls) n.className = cls;
  if (text != null) n.textContent = text;
  return n;
};

export function buildTerminal(terminalData, vars) {
  const wrap = el("div", "hub-term");

  // chapter tabs
  const tabs = el("div", "subtabs hub-term-tabs");
  const win = el("div", "hub-term-win");
  const bar = el("div", "hub-term-bar");
  const dots = el("span", "hub-term-dots");
  dots.append(el("i"), el("i"), el("i"));
  const barTitle = el("span", "hub-term-title");
  const barSim = el("span", "hub-term-sim", "simulated bench — real commands, recorded output");
  bar.append(dots, barTitle, barSim);
  const scroll = el("div", "hub-term-scroll");
  const controls = el("div", "hub-term-controls");
  const btnNext = el("button", "primary small", "▶ type next command");
  const btnAll = el("button", "ghost small", "run the chapter");
  const btnReset = el("button", "ghost small", "clear");
  const hint = el("span", "muted fineprint", "or press Enter in the terminal");
  controls.append(btnNext, btnAll, btnReset, hint);
  win.append(bar, scroll, controls);

  const noteCard = el("p", "ondevice hub-term-note");
  wrap.append(tabs, win, noteCard);

  const sessions = new Map(); // chapter.id → session
  let chapter = null;
  let busy = false;

  const promptOf = (ch) => PROMPTS[ch.host] || "$";

  function print(cls, text) {
    const line = el("div", "hub-line " + cls);
    line.textContent = text;
    scroll.append(line);
    scroll.scrollTop = scroll.scrollHeight;
    return line;
  }

  function printPromptLine() {
    const line = el("div", "hub-line");
    const p = el("span", "hub-prompt", promptOf(chapter) + " ");
    const c = el("span", "hub-cmd");
    const caret = el("span", "hub-caret", "▋");
    line.append(p, c, caret);
    scroll.append(line);
    scroll.scrollTop = scroll.scrollHeight;
    return { line, c, caret };
  }

  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

  async function runNext({ instant = false } = {}) {
    if (busy || !chapter) return false;
    const sess = sessions.get(chapter.id);
    const step = sess.run();
    if (!step) return false;
    busy = true;
    btnNext.disabled = true;
    const { c, caret } = printPromptLine();
    if (instant) {
      c.textContent = step.cmd;
    } else {
      for (const ch of step.cmd) {
        c.textContent += ch;
        scroll.scrollTop = scroll.scrollHeight;
        if (!document.body.contains(scroll)) { busy = false; return false; }
        await sleep(ch === " " ? 26 : 13);
      }
      await sleep(160);
    }
    caret.remove();
    for (const lineText of step.out) {
      if (!instant) await sleep(70);
      print("hub-out", lineText);
    }
    if (step.note) {
      noteCard.textContent = step.note;
      if (!instant) await sleep(60);
    }
    if (sess.done) {
      print("hub-done", "— chapter complete ✓" +
        (nextChapterOf(chapter) ? "  (next: " + nextChapterOf(chapter).title + ")" : ""));
    }
    busy = false;
    refreshControls();
    return true;
  }

  function nextChapterOf(ch) {
    const i = terminalData.chapters.indexOf(ch);
    return terminalData.chapters[i + 1] || null;
  }

  function refreshControls() {
    const sess = sessions.get(chapter.id);
    const step = sess.peek();
    btnNext.disabled = !step || busy;
    btnNext.textContent = step ? "▶ " + truncate(step.cmd, 46) : "chapter complete ✓";
    btnAll.disabled = !step;
  }

  const truncate = (s, n) => (s.length > n ? s.slice(0, n - 1) + "…" : s);

  function openChapter(ch) {
    chapter = ch;
    if (!sessions.has(ch.id)) sessions.set(ch.id, createTermSession(ch, vars));
    [...tabs.children].forEach((t, i) =>
      t.classList.toggle("on", terminalData.chapters[i] === ch));
    barTitle.textContent = ch.host === "ha-ssh" ? "ssh — homeassistant" : "bash — your machine";
    scroll.innerHTML = "";
    print("hub-intro", "— " + expandVars(ch.intro, vars));
    // replay already-run steps instantly so switching tabs never loses work
    const sess = sessions.get(ch.id);
    const doneCount = sess.index;
    sess.reset();
    for (let i = 0; i < doneCount; i++) {
      const s = sess.run();
      print("hub-replay", promptOf(ch) + " " + s.cmd);
      for (const l of s.out) print("hub-out", l);
    }
    noteCard.textContent = doneCount
      ? (ch.steps[doneCount - 1].note ? expandVars(ch.steps[doneCount - 1].note, vars) : "")
      : "Click ▶ to type the first command.";
    refreshControls();
  }

  for (const ch of terminalData.chapters) {
    const t = el("button", "tab", ch.title);
    t.addEventListener("click", () => { if (!busy) openChapter(ch); });
    tabs.append(t);
  }

  btnNext.addEventListener("click", () => runNext());
  btnAll.addEventListener("click", async () => {
    while (await runNext()) { /* sequential, animated */ }
  });
  btnReset.addEventListener("click", () => {
    if (busy) return;
    sessions.get(chapter.id).reset();
    openChapter(chapter);
  });
  win.tabIndex = 0;
  win.addEventListener("keydown", (e) => {
    if (e.key === "Enter") { e.preventDefault(); runNext(); }
  });

  openChapter(terminalData.chapters[0]);
  return wrap;
}
