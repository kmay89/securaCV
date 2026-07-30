// canary-local/assets/catalog-funnel.js — the guided "which case do I need?"
// funnel (Phase 4, CATALOG_ARCHITECTURE.md §6A + the §6D remix rail on the
// result). Two-to-three questions, each narrowing the next, over the generated
// manifest (devices/catalog.json): what are you building → where does it live →
// how do you mount it → ONE recommended case + variant, options pre-checked,
// the fit coupon offered, and a sideways "see also" rail. All reasoning is
// pure (funnelResult) and unit-tested; only a real page boots the UI.

import { productSummary, envLabel } from "./catalog-browse.js";

const el = (tag, cls, text) => {
  const n = document.createElement(tag);
  if (cls) n.className = cls;
  if (text != null) n.textContent = text;
  return n;
};

const GH = "https://github.com/kmay89/securaCV/blob/main/docs/hardware/enclosure/";

// The rugged, device-agnostic carriers — a witness that must live outdoors-
// exposed goes INTO one of these, whatever board it is.
const RUGGED = new Set(["field_case", "hammond_chassis", "relay_solar"]);

// ── the questions (static; the answers key into the manifest) ─────────────
export function funnelQuestions() {
  return [
    { id: "build", prompt: "What are you building?", options: [
      { value: "canary-vision", label: "A camera witness", hint: "semantic events, never frames" },
      { value: "canary-sense", label: "A presence sensor", hint: "radar — no camera, no mic" },
      { value: "canary-wap", label: "The portable witness", hint: "the WAP — the full option sheet" },
      { value: "display", label: "A display / dashboard", hint: "desk glass or the whole-house panel" },
      { value: "accessory", label: "A mount or accessory", hint: "brackets, docks, stands, carriers" },
    ] },
    { id: "place", prompt: "Where will it live?", options: [
      { value: "indoor", label: "Indoors", hint: "no weather sealing needed" },
      { value: "weather", label: "Outdoors, sheltered", hint: "eaves/porch — needs a seal + vent" },
      { value: "field", label: "Outdoors, exposed / rugged", hint: "the honest top of FDM — IP + drop intent" },
    ] },
    { id: "mount", prompt: "How will you mount it?", options: [
      { value: "desk", label: "On a desk or shelf", hint: "a stand or cradle" },
      { value: "wall", label: "On a wall", hint: "keyhole / bracket" },
      { value: "pole", label: "On a pole or vehicle", hint: "outdoor stud / clamp" },
      { value: "any", label: "Doesn't matter", hint: "handheld or undecided" },
    ] },
  ];
}

// ── predicates over a product summary ─────────────────────────────────────
const isRugged = (s) => RUGGED.has(s.id) || (s.env && s.env.cer >= 4);
const isWeather = (s) => !!s.env && ((s.env.cer || 0) >= 2 || !!s.env.ip);
const hasOpt = (p, id) => (p.options || []).some((o) => o.id === id);

function matchesBuild(s, build) {
  if (!build) return true;
  if (build === "accessory") return s.klass === "accessory" || s.devices.includes("_universal");
  if (build === "display")
    return s.devices.includes("canary-display-dash") || s.devices.includes("canary-display-watch");
  return s.devices.includes(build);
}

// A mount-fit score for ranking (higher = better fit for the chosen mount).
function mountScore(p, s, mount) {
  if (mount === "desk") return hasOpt(p, "opt_stand") ? 3 : 0;
  if (mount === "wall") return hasOpt(p, "mount_style") || hasOpt(p, "opt_mount") ? 2 : 0;
  if (mount === "pole") return (s.id === "relay_solar" || s.id === "vehicle_mount"
    || s.family === "universal") ? 2 : 0;
  return 0;
}

// The recommended variant + pre-checked options implied by the answers.
export function recommendFor(product, answers) {
  const variants = product.variants || [];
  const flavor = variants.find((v) => v.status === "released" && v.selects
    && Object.keys(v.selects).length) || variants.find((v) => v.status === "released")
    || variants[0] || null;
  const options = [];
  const want = (id) => hasOpt(product, id) && !options.includes(id) && options.push(id);
  if (answers.place === "weather" || answers.place === "field") { want("opt_seal"); want("opt_vent"); }
  if (answers.mount === "desk") want("opt_stand");
  return { variant: flavor ? flavor.id : null, options };
}

// The funnel's answer → one primary case + runners-up, honest about relaxation.
export function funnelResult(products, answers) {
  const { build, place, mount } = answers || {};
  const withS = products.map((p) => ({ p, s: productSummary(p) }));

  let pool = withS.filter(({ s }) => matchesBuild(s, build));
  let relaxed = false, note = "";

  if (place === "field") {
    const rugged = pool.filter(({ s }) => isRugged(s));
    if (rugged.length) { pool = rugged; }
    else {
      pool = withS.filter(({ s }) => isRugged(s));
      relaxed = true;
      note = "No rugged case is specific to that pick — these device-agnostic " +
        "carriers take any board.";
    }
  } else if (place === "weather") {
    const wet = pool.filter(({ s }) => isWeather(s));
    if (wet.length) { pool = wet; }
    else {
      pool = withS.filter(({ s }) => isWeather(s));
      relaxed = true;
      note = "No weather-rated case for that pick yet — here are the " +
        "weather-capable cases.";
    }
  }

  const scored = pool.map(({ p, s }) => {
    let score = 0;
    if (s.released) score += 4;                 // print-validated beats in-dev
    score += mountScore(p, s, mount);
    if (place === "indoor" && !s.env) score += 1; // indoor: unrated is fine/simpler
    if ((place === "weather" || place === "field") && s.env) score += 1;
    return { p, s, score };
  }).sort((a, b) => b.score - a.score
    || Number(b.s.released > 0) - Number(a.s.released > 0)
    || a.s.id.localeCompare(b.s.id));

  const primary = scored[0] ? scored[0].p : null;
  const reasons = [];
  if (primary) {
    const s = scored[0].s;
    if (build) reasons.push(matchesBuild(s, build) ? "fits what you're building" : "closest match");
    if (place === "weather") reasons.push(`weather-capable (${envLabel(primary)} · target)`);
    else if (place === "field") reasons.push(`rugged intent (${envLabel(primary)} · target)`);
    else reasons.push(s.env ? "sealed-capable but simple indoors" : "clean indoor build");
    if (mountScore(primary, s, mount) > 0) reasons.push(`good ${mount} mount`);
    if (s.released) reasons.push("print-validated");
  }
  return {
    primary,
    recommend: primary ? recommendFor(primary, answers || {}) : null,
    reasons,
    others: scored.slice(1, 6).map((x) => x.p),
    relaxed,
    note,
  };
}

// ── rendering (DOM) ───────────────────────────────────────────────────────
function optionLabel(product, id) {
  const o = (product.options || []).find((x) => x.id === id);
  return (o && (o.label && o.label !== id ? o.label : o.id)) || id;
}

function resultCard(product, recommend, reasons, fit) {
  const s = productSummary(product);
  const card = el("div", "fun-result");
  card.append(el("h3", null, s.title));
  const meta = el("div", "cat-meta");
  meta.append(el("span", "cat-fam", s.family));
  if (s.env) meta.append(el("span", "cat-badge", `${envLabel(product)} · target`));
  meta.append(el("span", "cat-tag", s.released ? "released" : "in development"));
  card.append(meta);
  if (reasons && reasons.length)
    card.append(el("p", "fun-why", "Why: " + reasons.join(" · ")));

  if (recommend && recommend.variant) {
    const v = (product.variants || []).find((x) => x.id === recommend.variant);
    if (v) card.append(el("p", "muted", `Recommended flavor: ${v.name}`));
  }
  if (recommend && recommend.options.length) {
    const opts = el("p", "fun-opts");
    opts.append(el("span", "muted", "Pre-checked: "));
    opts.append(document.createTextNode(
      recommend.options.map((id) => optionLabel(product, id)).join(", ")));
    card.append(opts);
  }
  if (fit && fit.coupon_scad) {
    const c = el("p", "muted");
    const a = el("a", null, "print the fit coupon first");
    a.href = GH + fit.coupon_scad; a.target = "_blank"; a.rel = "noopener";
    c.append(a, document.createTextNode(" — tune tolerances to your printer."));
    card.append(c);
  }

  const links = el("div", "cat-links");
  const scad = el("a", "cat-scad", "open in OpenSCAD ↗");
  scad.href = GH + s.scad; scad.target = "_blank"; scad.rel = "noopener";
  links.append(scad);
  const dev = s.devices.find((d) => d && d !== "_universal");
  if (dev) {
    const w = el("a", "cat-workshop", "configure in the workshop →");
    w.href = `workshop.html#${dev}`;
    links.append(w);
  }
  card.append(links);
  return card;
}

// §6D — the sideways remix / alternatives rail on the RESULT (never in the funnel)
function remixRail(product, others, byTitle) {
  const ids = new Set();
  for (const a of product.alternatives || []) ids.add(a);
  for (const o of others) ids.add(o.id);
  ids.delete(product.id);
  if (!ids.size) return null;
  const rail = el("div", "fun-rail");
  rail.append(el("h4", null, "See also"));
  const row = el("div", "fun-rail-row");
  for (const id of ids) {
    const chip = el("a", "fun-chip", byTitle.get(id) || id);
    chip.href = "catalog.html#p-" + id;
    row.append(chip);
  }
  rail.append(row);
  return rail;
}

export function buildCatalogFunnel(catalog, mount) {
  mount.innerHTML = "";
  if (!catalog || !Array.isArray(catalog.products)) {
    mount.append(el("p", "muted", "Catalog manifest unavailable."));
    return;
  }
  const products = catalog.products;
  const byTitle = new Map(products.map((p) => [p.id, (p.title || p.id)]));
  const answers = {};
  const questions = funnelQuestions();

  const qWrap = el("div", "fun-questions");
  const out = el("div", "fun-out");
  mount.append(qWrap, out);

  function renderResults() {
    out.innerHTML = "";
    if (!answers.build) return; // wait for at least the first answer
    const r = funnelResult(products, answers);
    if (r.relaxed && r.note) out.append(el("p", "fun-note", r.note));
    if (!r.primary) { out.append(el("p", "muted", "No case matches — try the browse.")); return; }
    out.append(el("h2", null, "Your case"));
    out.append(resultCard(r.primary, r.recommend, r.reasons, catalog.fit));
    const rail = remixRail(r.primary, r.others, byTitle);
    if (rail) out.append(rail);
    const browse = el("p", "fineprint");
    const b = el("a", null, "prefer to look? browse every case →");
    b.href = "catalog.html";
    browse.append(b);
    out.append(browse);
  }

  function renderQuestions() {
    qWrap.innerHTML = "";
    for (let i = 0; i < questions.length; i++) {
      const q = questions[i];
      // progressive: only show a question once the previous is answered
      if (i > 0 && !answers[questions[i - 1].id]) break;
      const block = el("fieldset", "fun-q");
      block.append(el("legend", null, `${i + 1}. ${q.prompt}`));
      for (const opt of q.options) {
        const b = el("button", "fun-opt" + (answers[q.id] === opt.value ? " on" : ""));
        b.append(el("strong", null, opt.label));
        if (opt.hint) b.append(el("span", "muted", opt.hint));
        b.addEventListener("click", () => {
          answers[q.id] = opt.value;
          // answering an earlier question invalidates later ones
          for (let j = i + 1; j < questions.length; j++) delete answers[questions[j].id];
          renderQuestions();
          renderResults();
        });
        block.append(b);
      }
      qWrap.append(block);
    }
  }

  renderQuestions();
  renderResults();
}

if (typeof document !== "undefined") {
  const mount = document.getElementById("find");
  if (mount) {
    fetch("devices/catalog.json")
      .then((r) => r.json())
      .then((cat) => buildCatalogFunnel(cat, mount))
      .catch((e) => {
        mount.append(el("p", "muted",
          `The finder could not load the catalog (${e.message}). It is generated ` +
          "into devices/catalog.json by canary-local/tools/gen_enclosures.py."));
      });
  }
}
