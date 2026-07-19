// canary-local/assets/chooser.js — the four-question front door.
//
// Renders QUESTIONS, live-scores CANDIDATES on every answer, and shows
// the top matches as pairing-style cards: 3D-lite (catalog preview
// image), the pitch, the honest status chip, and doors onward — the
// device's live card on index.html, the printable parts, the BOM.
import {
  QUESTIONS,
  score,
  companion,
} from "./chooser-data.js";

const el = (tag, cls, text) => {
  const n = document.createElement(tag);
  if (cls) n.className = cls;
  if (text != null) n.textContent = text;
  return n;
};

const GH = "https://github.com/kmay89/securaCV/blob/main/";
const answers = {};
let encData = null;

const quiz = document.getElementById("quiz");
const qwrap = el("div", "questions");
const results = el("div", "results");
quiz.append(qwrap, results);

for (const q of QUESTIONS) {
  const card = el("section", "q");
  card.append(el("h3", null, q.title));
  const opts = el("div", "q-opts");
  for (const o of q.options) {
    const b = el("button", "q-opt", o.label);
    b.addEventListener("click", () => {
      if (q.multi) {
        const cur = new Set(answers[q.id] || []);
        cur.has(o.id) ? cur.delete(o.id) : cur.add(o.id);
        answers[q.id] = [...cur];
        b.classList.toggle("on");
      } else {
        answers[q.id] = o.id;
        for (const x of opts.children) x.classList.remove("on");
        b.classList.add("on");
      }
      render();
    });
    opts.append(b);
  }
  if (q.multi) card.append(el("p", "q-hint", "pick any that apply"));
  card.append(opts);
  qwrap.append(card);
}

function statusChip(status) {
  const c = el("span", `chip ${status === "released" ? "chip-live" : "chip-dev"}`);
  c.textContent = status === "released" ? "released · print-validated" : "in development";
  return c;
}

function render() {
  results.innerHTML = "";
  const ranked = score(answers);
  if (!Object.keys(answers).length) return;
  if (!ranked.length) {
    results.append(el("p", "muted",
      "Nothing honestly fits those constraints yet — loosen one, or open an issue: this catalog grows toward real needs."));
    return;
  }
  results.append(el("h2", null, "Your match" + (ranked.length > 1 ? "es" : "")));
  if (Object.keys(answers).length >= 3) {
    requestAnimationFrame(() =>
      results.scrollIntoView({ behavior: "smooth", block: "nearest" }));
  }
  for (const c of ranked.slice(0, 3)) {
    const card = el("div", "match");
    const head = el("div", "match-head");
    head.append(el("h3", null, c.title), statusChip(c.status));
    card.append(head);
    card.append(el("p", "body", c.pitch));
    const encSet = encData?.sets.find((s) => s.id === c.enclosure);
    if (encSet?.preview) {
      const img = new Image();
      img.src = "../docs/hardware/enclosure/" + encSet.preview;
      img.alt = `${encSet.name} printed parts`;
      img.className = "match-preview";
      img.loading = "lazy";
      card.append(img);
    }
    for (const n of c.notes || []) {
      const od = el("p", "ondevice");
      od.append(el("strong", null, "Honesty: "), document.createTextNode(n));
      card.append(od);
    }
    const doors = el("p", "match-doors");
    const meet = el("a", "primary small door", "meet it live →");
    meet.href = `index.html#${c.device}`;
    doors.append(meet);
    const spec = el("a", "door", "spec it in the Workshop →");
    spec.href = `workshop.html#${c.device}`;
    doors.append(spec);
    if (encSet) {
      for (const p of encSet.parts.filter((p) => !p.preview_mesh).slice(0, 4)) {
        const a = el("a", null, `${p.name}.stl`);
        a.href = "../docs/hardware/enclosure/" + p.file;
        a.download = p.file;
        doors.append(a);
      }
      if (encSet.scad) {
        const a = el("a", null, "configure (.scad)");
        a.href = GH + "docs/hardware/enclosure/" + encSet.scad;
        a.target = "_blank";
        a.rel = "noopener";
        doors.append(a);
      }
    }
    const bomFile = {
      "canary-wap": "bom_canary_wap.csv",
      "canary-vision": "bom_canary_vision.csv",
      "canary-display-watch": "bom_canary_display.csv",
      "canary-display-dash": "bom_canary_display.csv",
    }[c.device];
    if (bomFile) {
      const a = el("a", null, "BOM");
      a.href = GH + "docs/hardware/" + bomFile;
      a.target = "_blank";
      a.rel = "noopener";
      doors.append(a);
    }
    card.append(doors);
    results.append(card);
  }
  const comp = companion(answers, ranked);
  if (comp) {
    const card = el("div", "match match-comp");
    card.append(el("h3", null, "…and give the fleet a face"));
    card.append(el("p", "body", `Every household above pairs naturally with a display: ${comp.why}.`));
    const meet = el("a", "primary small door", "meet the display →");
    meet.href = `index.html#${comp.device}`;
    card.append(meet);
    results.append(card);
  }
  results.append(el("p", "muted fineprint",
    "Recommendations favor released hardware on ties, never hide status, and hard-respect your privacy lines — a \"no cameras\" answer removes every camera device, full stop."));
}

fetch("devices/enclosures.json")
  .then((r) => r.json())
  .then((d) => { encData = d; render(); })
  .catch(() => render());
