/* The Lab shell — renders build-line.json into an adaptive nav:
   a source-list sidebar on big screens, a six-stage bottom tab bar on phones.
   No build step; pure ES module. Progress is remembered on-device only. */

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

/* ---- navigation ---- */
function navigate(id, push = true) {
  const view =
    id === "overview" || id === "start" || id === "all" ? id :
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
    if (sel && b.depths) {
      const sub = h("div", { class: "children lvl2" });
      b.depths.forEach((d, i) =>
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
    ),
  );
  const old = document.querySelector(".side");
  if (old) old.replaceWith(side); else document.querySelector(".shell").prepend(side);
}
const navItem = (id, sel, icoHtml, label) =>
  h("button", { class: "item" + (sel ? " sel" : ""), onclick: () => navigate(id) },
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
  const title = view === "bench" ? entry.bench.noun : view === "start" ? "Get started" : view === "all" ? "All benches" : "The Lab";
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
  else c.replaceChildren(benchView(entry));
}

function benchView(entry) {
  const { stage, track, bench } = entry;
  const di = depthSel[bench.slug] || 0;
  const depth = bench.depths ? bench.depths[di] : null;
  const real = depth?.real ?? bench.real;
  const openHref = depth?.lab || bench.lab;
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
    stage.fork ? h("div", { class: "forknote" }, "⑃ Stage 4 forks here — camera or radar. Both paths rejoin at Home.") : null,
    bench.depths ? h("div", { class: "seg", role: "tablist" },
      ...bench.depths.map((d, i) => h("button", { class: i === di ? "on" : "", role: "tab", "aria-selected": i === di,
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
      h("div", { class: "cnt" }, n + (n === 1 ? " bench" : " benches") + (s.fork ? " · forks" : "")));
  });
  return h("div", { class: "hero-o" },
    h("span", { class: "kick" }, h("span", { html: "●" }), " Running the real firmware"),
    h("h1", { html: "Meet your Canary <em>before</em> you meet your Canary." }),
    h("p", {}, "One guided build line, start to proof. Learn it, break it, fix it here — then set up the one on your desk right the first time. Works offline; nothing phones anywhere."),
    h("div", { class: "actions" },
      h("button", { class: "btn primary", onclick: () => navigate(firstOf(M.stages[0]).slug) },
        "Start the build line",
        h("span", { html: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round"><path d="M5 12h14M13 6l6 6-6 6"/></svg>' })),
      h("a", { class: "btn ghost", href: "index.html" }, "Explore in 3D →")),
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
  root.removeAttribute("data-loading");
  const shell = h("div", { class: "shell" }, h("div", { class: "main" }));
  root.replaceChildren(shell);
  navigate(location.hash.slice(1) || "overview", false);
  window.addEventListener("hashchange", () => navigate(location.hash.slice(1) || "overview", false));
}
boot();
