/* The Lab shell — renders build-line.json into an adaptive nav:
   a source-list sidebar on big screens, a six-stage bottom tab bar on phones.
   No build step; pure ES module. Progress is remembered on-device only. */

import { IN_APP, settingsView } from "./lab-settings.js";

const MANIFEST_URL = "build-line.json";
const SITE_ORIGIN = "https://securacv.com";
const PKEY = "scv-lab-progress";

const ICONS = {
  choose: '<circle cx="12" cy="12" r="9"/><path d="M15.5 8.5l-2 5-5 2 2-5z" fill="currentColor" stroke="none"/>',
  build: '<path d="M14.7 6.3a4 4 0 00-5.4 5.4l-6 6 2 2 6-6a4 4 0 005.4-5.4l-2.3 2.3-2-2z"/>',
  flash: '<path d="M13 2L4.5 13H11l-1 9 8.5-11H12z" fill="currentColor" stroke="none"/>',
  sense: '<path d="M2 12s3.5-6 10-6 10 6 10 6-3.5 6-10 6-10-6-10-6z"/><circle cx="12" cy="12" r="2.5" fill="currentColor" stroke="none"/>',
  home: '<path d="M3 11l9-7 9 7"/><path d="M5 10v9h14v-9"/>',
  prove: '<path d="M12 3l7 3v5c0 4.5-3 7.5-7 9-4-1.5-7-4.5-7-9V6z"/><path d="M9 12l2 2 4-4"/>',
};
const CHECK = '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="3.4" stroke-linecap="round" stroke-linejoin="round"><path d="M5 12l5 5L20 7"/></svg>';
const CHEV = '<svg class="chev" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" stroke-linejoin="round"><path d="M9 6l6 6-6 6"/></svg>';

let M, ROUTE = [], BY_SLUG = new Map();
let depthSel = {}; // benchSlug -> depth index

/* ---- tiny hyperscript ---- */
function h(tag, attrs, ...kids) {
  const n = document.createElement(tag);
  if (attrs) for (const [k, v] of Object.entries(attrs)) {
    if (v == null || v === false) continue;
    if (k === "class") n.className = v;
    else if (k === "html") n.innerHTML = v;
    else if (k.startsWith("on")) n.addEventListener(k.slice(2).toLowerCase(), v);
    else n.setAttribute(k, v === true ? "" : v);
  }
  for (const c of kids.flat()) {
    if (c == null || c === false) continue;
    n.append(c.nodeType ? c : document.createTextNode(String(c)));
  }
  return n;
}
const svg = (paths, cls) =>
  h("span", { class: cls, html: `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7">${paths}</svg>` });

/* ---- progress ---- */
const visited = () => { try { return new Set(JSON.parse(localStorage.getItem(PKEY) || "[]")); } catch { return new Set(); } };
function markVisited(id) { const s = visited(); s.add(id); try { localStorage.setItem(PKEY, JSON.stringify([...s])); } catch {} }

/* ---- model ---- */
function flatten() {
  ROUTE = []; BY_SLUG = new Map();
  for (const stage of M.stages) {
    const list = stage.tracks
      ? stage.tracks.flatMap(t => t.benches.map(b => ({ stage, track: t, bench: b })))
      : (stage.benches || []).map(b => ({ stage, bench: b }));
    for (const e of list) { ROUTE.push(e); BY_SLUG.set(e.bench.slug, e); }
  }
}
const firstOf = (stage) => (stage.tracks ? stage.tracks[0].benches[0] : stage.benches[0]);
const siteHref = (href) => (href && href.startsWith("/") ? SITE_ORIGIN + href : href);

// The depth options for a bench, guaranteeing the parent page is reachable.
// If a bench folds subpages (e.g. house → scenes) but no depth points back at
// the bench's own page, prepend a parent option so `bench.lab` is never orphaned.
function depthsFor(bench) {
  if (!bench.depths || !bench.depths.length) return null;
  if (bench.depths.some((d) => d.lab === bench.lab)) return bench.depths;
  return [{ label: bench.noun, lab: bench.lab, desc: bench.desc, real: bench.real }, ...bench.depths];
}

/* ---- navigation ---- */
// Turn a raw (user-controlled) hash into a known-safe route id. Following a
// manifest redirect first, then validating against an allowlist with
// Array.includes() — a sanitizer CodeQL recognizes — guarantees no untrusted
// value ever reaches navigation. Anything unknown becomes the overview.
let VALID_IDS = [];
function routeId(raw) {
  const id = M.redirects && Object.prototype.hasOwnProperty.call(M.redirects, raw) ? M.redirects[raw] : raw;
  return VALID_IDS.includes(id) ? id : "overview";
}

function navigate(id, push = true) {
  // `id` is always an allowlisted route (from routeId) or a literal bench slug
  // from a click handler. `view` is one of these literals and `hash` below is a
  // literal or a manifest slug — so nothing untrusted is echoed into location.
  const view =
    id === "start" ? "start" :
    id === "all" ? "all" :
    id === "settings" ? "settings" :
    BY_SLUG.has(id) ? "bench" : "overview";
  const entry = view === "bench" ? BY_SLUG.get(id) : null;
  if (entry) markVisited(entry.stage.id);

  renderContent(view, entry);
  renderSidebar(view, entry);   // reflect selection + expansion
  renderTabbar(entry);
  renderTopbar(view, entry);

  const hash = view === "bench" ? entry.bench.slug : view;
  if (push && location.hash.slice(1) !== hash) location.hash = hash;
  document.querySelector(".content")?.scrollTo(0, 0);
  window.scrollTo(0, 0);
}

/* ---- sidebar ---- */
function stageRow(stage, curEntry, done) {
  const isCur = curEntry && curEntry.stage.id === stage.id;
  const chev = h("span", { class: "chev" + (isCur ? " open" : ""), html:
    '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" stroke-linejoin="round"><path d="M9 6l6 6-6 6"/></svg>' });
  const row = h("button", {
    class: "item", "aria-expanded": isCur ? "true" : "false",
    onclick: () => navigate(firstOf(stage).slug),
  },
    chev,
    h("span", { class: "num" }, String(stage.n)),
    h("span", { class: "lb" }, stage.name),
    h("span", { class: "meta" }, done ? h("span", { class: "chk", html: CHECK }) : null),
  );

  const kids = h("div", { class: "children", hidden: !isCur });
  const benches = stage.tracks ? stage.tracks.flatMap(t => t.benches) : (stage.benches || []);
  for (const b of benches) {
    const sel = curEntry && curEntry.bench.slug === b.slug;
    kids.append(h("button", { class: "item" + (sel ? " sel" : ""), onclick: () => navigate(b.slug) },
      h("span", { class: "lb", html: benchLabel(b) }),
      h("span", { class: "meta" }, b.real ? h("span", { class: "rf", title: "boots real firmware" }) : null),
    ));
    const bd = depthsFor(b);
    if (sel && bd) {
      const sub = h("div", { class: "children lvl2" });
      bd.forEach((d, i) =>
        sub.append(h("button", { class: "item" + ((depthSel[b.slug] || 0) === i ? " sel" : ""), onclick: () => { depthSel[b.slug] = i; navigate(b.slug, false); } },
          h("span", { class: "lb" }, d.label))));
      kids.append(sub);
    }
  }
  return h("div", {}, row, kids);
}
function benchLabel(b) {
  const noun = b.noun.replace(/</g, "&lt;");
  return b.kind === "chooser" || /·/.test(noun) ? noun : noun;
}

function renderSidebar(view, entry) {
  const done = visited();
  const side = h("nav", { class: "side", "aria-label": "Lab contents" },
    h("div", { class: "side-head" },
      birdSVG(),
      h("b", { html: 'canary<span class="dot">.</span>local <small>/ the lab</small>' })),
    h("label", { class: "side-search" },
      h("span", { html: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><circle cx="11" cy="11" r="7"/><path d="M21 21l-4-4"/></svg>' }),
      h("input", { type: "search", placeholder: "Search benches…", oninput: onSearch, "aria-label": "Search benches" })),
    h("div", { class: "side-scroll" },
      navItem("overview", view === "overview", '<svg class="ic" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7"><path d="M3 9l9-6 9 6v10a2 2 0 01-2 2H5a2 2 0 01-2-2z"/><path d="M9 21V12h6v9"/></svg>', "Overview — the workshop"),
      navItem("start", view === "start", '<svg class="ic" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linejoin="round"><path d="M5 3l14 9-14 9z"/></svg>', "Get started"),
      h("div", { class: "side-sec" }, "The build line"),
      ...M.stages.map(s => stageRow(s, entry, done.has(s.id))),
      h("div", { class: "side-sec" }, "Reference"),
      navItem("all", view === "all", '<svg class="ic" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7"><path d="M4 6h16M4 12h16M4 18h16"/></svg>', "All benches, by stage"),
      navLink("site-map.html", '<svg class="ic" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7"><path d="M9 18l6-12M4 6h5v5H4zM15 13h5v5h-5z"/></svg>', "Complete site map"),
      // App only: the browser Lab has no updater and no update journal, so it
      // gets no Settings entry rather than one that answers "—" to everything.
      IN_APP ? h("div", { class: "side-sec" }, "This app") : null,
      IN_APP ? navItem("settings", view === "settings", '<svg class="ic" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7"><path d="M12 15a3 3 0 100-6 3 3 0 000 6z"/><path d="M19.4 15a1.65 1.65 0 00.33 1.82l.06.06a2 2 0 11-2.83 2.83l-.06-.06a1.65 1.65 0 00-1.82-.33 1.65 1.65 0 00-1 1.51V21a2 2 0 11-4 0v-.09A1.65 1.65 0 008 19.4a1.65 1.65 0 00-1.82.33l-.06.06a2 2 0 11-2.83-2.83l.06-.06a1.65 1.65 0 00.33-1.82 1.65 1.65 0 00-1.51-1H2a2 2 0 110-4h.09A1.65 1.65 0 004.6 8a1.65 1.65 0 00-.33-1.82l-.06-.06a2 2 0 112.83-2.83l.06.06A1.65 1.65 0 009 3.6V3a2 2 0 114 0v.09a1.65 1.65 0 001 1.51 1.65 1.65 0 001.82-.33l.06-.06a2 2 0 112.83 2.83l-.06.06A1.65 1.65 0 0019.4 9v.09a1.65 1.65 0 001.51 1H21a2 2 0 110 4h-.09a1.65 1.65 0 00-1.51 1z"/></svg>', "Updates & about") : null,
    ),
  );
  const old = document.querySelector(".side");
  if (old) old.replaceWith(side); else document.querySelector(".shell").prepend(side);
}
const navItem = (id, sel, icoHtml, label) =>
  h("button", { class: "item" + (sel ? " sel" : ""), onclick: () => navigate(id) },
    h("span", { html: icoHtml }), h("span", { class: "lb" }, label));
const navLink = (href, icoHtml, label) =>
  h("a", { class: "item", href },
    h("span", { html: icoHtml }), h("span", { class: "lb" }, label));

function onSearch(e) {
  const q = e.target.value.trim().toLowerCase();
  document.querySelectorAll(".side-scroll .children .item").forEach(it => {
    const hit = !q || it.textContent.toLowerCase().includes(q);
    it.style.display = hit ? "" : "none";
    if (q && hit) it.closest(".children")?.removeAttribute("hidden");
  });
}

/* ---- topbar (phone) ---- */
function renderTopbar(view, entry) {
  let bar = document.querySelector(".topbar");
  const title = view === "bench" ? entry.bench.noun : view === "start" ? "Get started"
    : view === "all" ? "All benches" : view === "settings" ? "Updates & about" : "The Lab";
  const content = [
    h("button", { class: "back", hidden: view === "overview", onclick: () => navigate("overview") },
      h("span", { html: "‹" }), " Lab"),
    h("span", { class: "ttl" }, title),
    h("span", { class: "off" }, "● offline"),
  ];
  if (!bar) { bar = h("div", { class: "topbar" }); document.querySelector(".main").prepend(bar); }
  bar.replaceChildren(...content);
}

/* ---- bottom tab bar (phone) ---- */
function renderTabbar(entry) {
  const done = visited();
  let bar = document.querySelector(".tabbar");
  const tabs = M.stages.map(s => {
    const on = entry && entry.stage.id === s.id;
    return h("button", { class: "tab" + (on ? " on" : ""), onclick: () => navigate(firstOf(s).slug), "aria-label": s.name },
      svg(ICONS[s.id] || ICONS.choose, ""),
      done.has(s.id) ? h("span", { class: "chkmini" }) : null,
      h("span", { class: "lb" }, s.name));
  });
  if (!bar) { bar = h("div", { class: "tabbar", role: "tablist" }); document.querySelector(".main").append(bar); }
  bar.replaceChildren(...tabs);
}

/* ---- content ---- */
function renderContent(view, entry) {
  let c = document.querySelector(".content");
  if (!c) { c = h("main", { class: "content" }); document.querySelector(".main").append(c); }
  if (view === "overview") c.replaceChildren(overviewView());
  else if (view === "start") c.replaceChildren(startView());
  else if (view === "all") c.replaceChildren(allView());
  else if (view === "settings") c.replaceChildren(settingsView());
  else c.replaceChildren(benchView(entry));
}

function benchView(entry) {
  const { stage, track, bench } = entry;
  const depths = depthsFor(bench);
  const di = depthSel[bench.slug] || 0;
  const depth = depths ? depths[di] : null;
  const real = depth?.real ?? bench.real;
  const openHref = depth ? depth.lab : bench.lab;
  const idx = ROUTE.indexOf(entry);
  const prev = ROUTE[idx - 1], next = ROUTE[idx + 1];

  return h("div", {},
    h("div", { class: "crumbs" },
      h("b", {}, "The build line"), h("span", { class: "sep" }, "›"),
      h("b", {}, stage.name),
      track ? [h("span", { class: "sep" }, "›"), h("b", {}, track.label.replace(" path", ""))] : null,
      h("span", { class: "sep" }, "›"), bench.noun),
    h("div", { class: "c-h" },
      h("h1", {}, bench.noun),
      real ? h("span", { class: "badge" }, "Real firmware") : null),
    (stage.options || stage.fork) ? h("div", { class: "forknote" }, "✦ " + (stage.optionsNote || "Options at this stage — pick the sense your Canary carries. The line runs straight on to Home.")) : null,
    depths ? h("div", { class: "seg", role: "tablist" },
      ...depths.map((d, i) => h("button", { class: i === di ? "on" : "", role: "tab", "aria-selected": i === di,
        onclick: () => { depthSel[bench.slug] = i; navigate(bench.slug, false); } }, d.label))) : null,
    h("p", { class: "lead" }, depth?.desc || bench.desc || ""),
    h("div", { class: "actions" },
      h("a", { class: "btn primary", href: openHref },
        "Open bench",
        h("span", { html: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round"><path d="M7 17L17 7M9 7h8v8"/></svg>' })),
      next ? h("button", { class: "btn ghost", onclick: () => navigate(next.bench.slug) }, "Walk the line →") : null),
    stage.site && stage.site.length ? h("div", { class: "sitelinks" },
      h("span", { class: "k" }, "read more on the site — "),
      ...stage.site.map(s => h("a", { class: "chip", href: siteHref(s.href), target: "_blank", rel: "noopener" }, s.noun))) : null,
    h("nav", { class: "booklet", "aria-label": "Sequential" },
      prev ? h("a", { class: "pn prev", href: "#" + prev.bench.slug, onclick: linkNav(prev.bench.slug) },
        h("div", { class: "k" }, h("span", { html: "‹" }), " Previous · " + prev.stage.name),
        h("div", { class: "t" }, prev.bench.noun)) : h("span", { class: "pn prev", hidden: true }),
      next ? h("a", { class: "pn next", href: "#" + next.bench.slug, onclick: linkNav(next.bench.slug) },
        h("div", { class: "k" }, "Next · " + next.stage.name + " ", h("span", { html: "›" })),
        h("div", { class: "t" }, next.bench.noun)) : h("span", { class: "pn next", hidden: true })),
  );
}
const linkNav = (slug) => (e) => { e.preventDefault(); navigate(slug); };

function overviewView() {
  const cards = M.stages.map(s => {
    const n = (s.tracks ? s.tracks.flatMap(t => t.benches) : s.benches).length;
    return h("button", { class: "scard", onclick: () => navigate(firstOf(s).slug) },
      h("div", { class: "top" },
        h("span", { class: "circ", style: `border-color:${s.accent};color:${s.accent}` }, String(s.n)),
        h("h3", {}, s.name)),
      h("div", { class: "verb" }, s.verb),
      h("div", { class: "cnt" }, n + (n === 1 ? " bench" : " benches") + ((s.options || s.fork) ? " · options" : "")));
  });
  return h("div", { class: "ov" },
    h("div", { class: "room-wrap" },
      h("iframe", { class: "room", src: "room.html", loading: "eager",
        title: "The Canary Lab — a workshop you can walk. Tap a station to open its bench." })),
    h("div", { class: "ov-cards-head" }, "Jump straight to a stage"),
    h("div", { class: "stage-cards" }, ...cards),
  );
}

function startView() {
  const o = M.onramp;
  return h("div", { class: "hero-o" },
    h("h1", {}, "Get started"),
    h("p", {}, o.desc),
    h("div", { class: "actions" },
      h("a", { class: "btn primary", href: o.lab }, "Open the setup guide"),
      h("button", { class: "btn ghost", onclick: () => navigate(firstOf(M.stages[0]).slug) }, "Or walk the build line →")),
  );
}

function allView() {
  const cols = M.stages.map(s => {
    const benches = s.tracks ? s.tracks.flatMap(t => t.benches) : s.benches;
    return h("div", { class: "allcol" },
      h("h4", {}, s.n + " · " + s.name),
      ...benches.map(b => h("a", { href: "#" + b.slug, onclick: linkNav(b.slug) },
        b.noun, b.real ? h("span", { class: "rf" }) : null)));
  });
  return h("div", {},
    h("div", { class: "crumbs" }, h("b", {}, "Reference")),
    h("div", { class: "c-h" }, h("h1", {}, "All benches, by stage")),
    h("div", { class: "allgrid" }, ...cols),
  );
}

function birdSVG() {
  return h("span", { class: "bird", html:
    '<svg class="bird" viewBox="0 0 64 64" aria-hidden="true"><circle cx="32" cy="36" r="17" fill="#FFD44F"/><circle cx="41" cy="24" r="10" fill="#FFD44F"/><circle cx="44.5" cy="22.5" r="1.8" fill="#141414"/><path d="M50 25.5 l7 2.2 -7 2.2 z" fill="#F08C2E"/><ellipse cx="26" cy="38" rx="8.5" ry="6" fill="#E3B33C"/></svg>' });
}

/* ---- boot ---- */
async function boot() {
  const root = document.getElementById("lab-shell");
  try {
    const res = await fetch(MANIFEST_URL, { cache: "no-cache" });
    if (!res.ok) throw new Error("manifest " + res.status);
    M = await res.json();
  } catch (err) {
    root.removeAttribute("data-loading");
    root.append(h("div", { style: "padding:40px;max-width:520px;margin:0 auto" },
      h("h1", {}, "Couldn't load the Lab"),
      h("p", { style: "color:var(--text-2)" }, "The build-line manifest didn't load (" + err.message + "). Every bench is still reachable from the index."),
      h("a", { class: "btn primary", href: "index.html" }, "Go to the index")));
    return;
  }
  flatten();
  VALID_IDS = ["overview", "start", "all", ...(IN_APP ? ["settings"] : []), ...ROUTE.map((e) => e.bench.slug)];
  root.removeAttribute("data-loading");
  const shell = h("div", { class: "shell" }, h("div", { class: "main" }));
  root.replaceChildren(shell);
  navigate(routeId(location.hash.slice(1)), false);
  window.addEventListener("hashchange", () => navigate(routeId(location.hash.slice(1)), false));
  // The embedded isometric room (Overview) posts a slug when a station's tool is
  // tapped; route it through the same allowlist sanitizer before navigating.
  window.addEventListener("message", (e) => {
    const d = e.data;
    if (d && d.source === "canary-room" && typeof d.slug === "string") navigate(routeId(d.slug));
  });
}
boot();
