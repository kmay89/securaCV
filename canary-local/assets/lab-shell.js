/* The Lab shell — renders build-line.json into an adaptive nav:
   a source-list sidebar on big screens, a six-stage bottom tab bar on phones.
   No build step; pure ES module. Progress is remembered on-device only. */

import { SETTINGS_AVAILABLE, probeSettings, settingsView } from "./lab-settings.js";

const MANIFEST_URL = "build-line.json";
const SITE_ORIGIN = "https://securacv.com";
// True exactly where lab-nav.js hides the "still on SecuraCV" strip (it sets
// .scv-app on the same condition). The two useful links off that strip move
// into the sidebar only here — on the website the strip is still showing, and
// listing them in both places would be the duplication this pass is removing.
const IN_APP = !!(window.__TAURI__ && window.__TAURI__.core);
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

let M, ROUTE = [], BY_SLUG = new Map(), PAGE_ROUTE = new Map();
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
  ROUTE = []; BY_SLUG = new Map(); PAGE_ROUTE = new Map();
  for (const stage of M.stages) {
    const list = stage.tracks
      ? stage.tracks.flatMap(t => t.benches.map(b => ({ stage, track: t, bench: b })))
      : (stage.benches || []).map(b => ({ stage, bench: b }));
    for (const e of list) {
      ROUTE.push(e); BY_SLUG.set(e.bench.slug, e);
      // bench.lab/depth.lab read backwards: which route SHOWS a given page. A
      // framed bench links to its neighbors by file name, and this is how one
      // of those turns back into a place in the sidebar.
      PAGE_ROUTE.set(e.bench.lab, { id: e.bench.slug });
      const ds = depthsFor(e.bench);
      if (ds) ds.forEach((d, i) => PAGE_ROUTE.set(d.lab, { id: e.bench.slug, depth: i }));
    }
  }
  if (M.onramp) PAGE_ROUTE.set(M.onramp.lab, { id: "start" });
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

// A depth label in the manifest is authored as "Name — what it is", and the
// tail can be a whole sentence ("Dev — the Proving Ground: test your flashed
// Sense, live or emulated"). That reads well in a paragraph and terribly in a
// tab strip: three of them in a 420px row all truncate to "D…". The head alone
// is the ladder the author meant — Basic / Advanced / Dev — so tabs and tree
// rows show that, and the full text stays one hover away.
const depthTab = (d) => d.label.split(" — ")[0];
const depthTitle = (d) => d.label + (d.desc ? "\n" + d.desc : "");

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
    // Sub-rows for the depths that are genuinely OTHER pages. A depth whose
    // `lab` is the bench's own page is the bench — listing it here produced
    // the "Meet the fleet › Meet the fleet" duplicate, because depthsFor()
    // synthesizes exactly such an entry so the parent stays reachable. The
    // bench row above already selects it, and the content's segmented control
    // still names it, so the tree stays one row per destination.
    const bd = depthsFor(b);
    if (sel && bd) {
      const sub = h("div", { class: "children lvl2" });
      bd.forEach((d, i) => {
        if (d.lab === b.lab) return;
        sub.append(h("button", { class: "item" + ((depthSel[b.slug] || 0) === i ? " sel" : ""), title: depthTitle(d), onclick: () => { depthSel[b.slug] = i; navigate(b.slug, false); } },
          h("span", { class: "lb" }, depthTab(d))));
      });
      if (sub.childElementCount) kids.append(sub);
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
      navItem("family", view === "family", '<svg class="ic" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7"><rect x="3" y="3" width="8" height="6" rx="1"/><rect x="13" y="3" width="8" height="10" rx="1"/><rect x="3" y="13" width="8" height="8" rx="1"/><rect x="13" y="17" width="8" height="4" rx="1"/></svg>', "SecuraCV everywhere"),
      // The two references people actually reach for. On the website these sit
      // in the "still on SecuraCV" strip along the top, which earns its place
      // there (you are on a different origin, this is the way back). In the app
      // that strip is a second bar above the sidebar saying what the sidebar
      // already says, so it is hidden and its two useful links live here.
      IN_APP ? navLink(SITE_ORIGIN + "/glossary", '<svg class="ic" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7"><path d="M4 5.5A2.5 2.5 0 016.5 3H19v15H6.5A2.5 2.5 0 004 20.5z"/><path d="M8 7h7M8 11h7"/></svg>', "Glossary", true) : null,
      IN_APP ? navLink(SITE_ORIGIN + "/canary", '<svg class="ic" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7"><circle cx="12" cy="12" r="9"/><path d="M9.5 9.5a2.5 2.5 0 113.5 2.3V13"/><path d="M12 16.5v.5" stroke-linecap="round"/></svg>', "Your Canary — help", true) : null,
      // App only: the browser Lab has no updater and no update journal, so it
      // gets no Settings entry rather than one that answers "—" to everything.
      SETTINGS_AVAILABLE ? h("div", { class: "side-sec" }, "This app") : null,
      SETTINGS_AVAILABLE ? navItem("settings", view === "settings", '<svg class="ic" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7"><path d="M12 15a3 3 0 100-6 3 3 0 000 6z"/><path d="M19.4 15a1.65 1.65 0 00.33 1.82l.06.06a2 2 0 11-2.83 2.83l-.06-.06a1.65 1.65 0 00-1.82-.33 1.65 1.65 0 00-1 1.51V21a2 2 0 11-4 0v-.09A1.65 1.65 0 008 19.4a1.65 1.65 0 00-1.82.33l-.06.06a2 2 0 11-2.83-2.83l.06-.06a1.65 1.65 0 00.33-1.82 1.65 1.65 0 00-1.51-1H2a2 2 0 110-4h.09A1.65 1.65 0 004.6 8a1.65 1.65 0 00-.33-1.82l-.06-.06a2 2 0 112.83-2.83l.06.06A1.65 1.65 0 009 3.6V3a2 2 0 114 0v.09a1.65 1.65 0 001 1.51 1.65 1.65 0 001.82-.33l.06-.06a2 2 0 112.83 2.83l-.06.06A1.65 1.65 0 0019.4 9v.09a1.65 1.65 0 001.51 1H21a2 2 0 110 4h-.09a1.65 1.65 0 00-1.51 1z"/></svg>', "Updates & about") : null,
    ),
  );
  const old = document.querySelector(".side");
  if (old) old.replaceWith(side); else document.querySelector(".shell").prepend(side);
}
const navItem = (id, sel, icoHtml, label) =>
  h("button", { class: "item" + (sel ? " sel" : ""), onclick: () => navigate(id) },
    h("span", { html: icoHtml }), h("span", { class: "lb" }, label));
// `external` marks a link off this origin. In the app, lab-nav.js's capture-
// phase handler sends those to the OS browser so the window never leaves the
// bundled Lab; on the website the target is what opens a new tab.
const navLink = (href, icoHtml, label, external) =>
  h("a", { class: "item", href, target: external ? "_blank" : null,
           rel: external ? "noopener" : null },
    h("span", { html: icoHtml }), h("span", { class: "lb" }, label),
    external ? h("span", { class: "ext", "aria-hidden": "true", html:
      '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M7 17L17 7M9 7h8v8"/></svg>' }) : null);

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
    : view === "all" ? "All benches" : view === "family" ? "SecuraCV everywhere"
    : view === "settings" ? "Updates & about" : "The Lab";
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
  else if (view === "family") c.replaceChildren(familyView());
  else if (view === "settings") c.replaceChildren(settingsView());
  else c.replaceChildren(benchView(entry));
  // A bench fills the window; the reading views keep their measure. Toggled
  // here rather than with `.content:has(.bench-wrap)` because the app supports
  // macOS back to 10.15 (tauri.conf.json), whose WebView predates :has() — the
  // selector would silently no-op there and leave the bench in a 1020px column.
  c.classList.toggle("content-full", view === "bench");
  // On a phone the depth strip scrolls sideways rather than clipping labels,
  // so the tab you are actually on can start off-screen. Bring it into the
  // strip after the paint that put it there.
  const on = c.querySelector(".bench-bar .seg button.on");
  if (on && on.scrollIntoView) on.scrollIntoView({ inline: "nearest", block: "nearest" });
}

/* A bench IS the bench. Selecting one used to land on a card describing it,
   with an "Open bench" button that navigated away — a click-through that left
   most of a large window empty and made the app feel like a table of contents.
   Now the bench itself fills the pane, the way the isometric room already
   filled the Overview, and the shell keeps only a slim toolbar above it:
   where you are, which depth you're reading, and the way onward.

   Everything the old card duplicated is gone: "Open bench" (you're already
   there), "Walk the line →" and the prev/next booklet (both are the › arrow),
   and the standalone description (each bench opens with its own). */
const ARROW = (d) =>
  `<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round"><path d="${d}"/></svg>`;

function benchView(entry) {
  const { stage, track, bench } = entry;
  const depths = depthsFor(bench);
  const di = Math.min(depthSel[bench.slug] || 0, depths ? depths.length - 1 : 0);
  const depth = depths ? depths[di] : null;
  const real = depth?.real ?? bench.real;
  const src = depth ? depth.lab : bench.lab;
  const idx = ROUTE.indexOf(entry);
  const prev = ROUTE[idx - 1], next = ROUTE[idx + 1];

  const step = (e, label, glyph, cls) =>
    e ? h("button", { class: "stepper " + cls, title: `${label}: ${e.bench.noun}`,
          "aria-label": `${label}: ${e.bench.noun}`, onclick: () => navigate(e.bench.slug) },
        h("span", { html: ARROW(glyph) }))
      : h("button", { class: "stepper " + cls, disabled: true, "aria-hidden": "true" },
          h("span", { html: ARROW(glyph) }));

  const bar = h("header", { class: "bench-bar" },
    h("div", { class: "bb-where" },
      h("div", { class: "crumbs" },
        h("b", {}, stage.n + " · " + stage.name),
        track ? [h("span", { class: "sep" }, "›"), track.label.replace(" path", "")] : null),
      h("div", { class: "bb-title" },
        h("h1", {}, bench.noun),
        real ? h("span", { class: "badge" }, "Real firmware") : null)),
    // Depths are how one bench teaches at two levels (overview vs. deep lab).
    // Kept as a segmented control because switching should feel like changing
    // channel on the same bench, not like navigating somewhere else.
    depths && depths.length > 1
      ? h("div", { class: "seg", role: "tablist", "aria-label": "Depth" },
          ...depths.map((d, i) => h("button", { class: i === di ? "on" : "", role: "tab",
            "aria-selected": i === di ? "true" : "false", title: depthTitle(d),
            onclick: () => { depthSel[bench.slug] = i; navigate(bench.slug, false); } }, depthTab(d))))
      : null,
    h("div", { class: "bb-go" },
      step(prev, "Previous", "M15 6l-6 6 6 6", "prev"),
      step(next, "Next", "M9 6l6 6-6 6", "next"),
      // Website only. A new tab is a real place in a browser; in the app the
      // window IS the Lab, and a `target="_blank"` there would ask Tauri for a
      // second webview that no capability grants — the button would sit there
      // looking live and do nothing. Better absent than dead.
      IN_APP ? null
        : h("a", { class: "stepper out", href: src, target: "_blank", rel: "noopener",
            title: "Open this bench in its own tab", "aria-label": "Open in a new tab" },
            h("span", { html: ARROW("M7 17L17 7M9 7h8v8") }))),
  );

  // The bench, live. Same-origin, so it inherits the app's theme and the
  // permissions it needs — serial for the flasher, camera for the eyes bench,
  // microphone for the acoustic ones — which a bare iframe would withhold.
  const frame = h("iframe", {
    class: "bench-frame", src, title: bench.noun, loading: "eager",
    allow: "serial; camera; microphone; usb; bluetooth; clipboard-write",
  });
  // Second line of defense, and the one that does not depend on the frame
  // cooperating: if the framed page ends up somewhere other than where we put
  // it — a link that got past the hand-off, a page that navigates itself, a
  // bench that somehow shipped without lab-nav.js — the shell resyncs to where
  // the frame actually IS rather than keep captioning the bench it left.
  // `dataset.at` is written BEFORE the resync so this can never re-enter.
  frame.dataset.at = src;
  frame.addEventListener("load", () => {
    let here;
    try { here = frame.contentWindow.location.href; } catch { return; } // same-origin, but never assume
    const file = here.split("#")[0].split("?")[0].split("/").pop();
    if (file === frame.dataset.at) return;
    frame.dataset.at = file;
    followFramed(here);
  });

  return h("div", { class: "bench-wrap" },
    bar,
    (stage.options || stage.fork)
      ? h("div", { class: "forknote" }, "✦ " + (stage.optionsNote || "Options at this stage — pick the sense your Canary carries. The line runs straight on to Home."))
      : null,
    frame,
  );
}
const linkNav = (slug) => (e) => { e.preventDefault(); navigate(slug); };

/* ---- the framed bench hands its local links up to here ----
   lab-nav.js, running inside the frame, refuses same-origin destinations and
   posts them here instead. Without that the frame navigates alone and the
   shell keeps describing the bench you left; with it, following a link inside
   a bench moves the whole app — sidebar, crumbs, depth control, prev/next and
   the address hash — the same as clicking that bench in the sidebar. */
function followFramed(href) {
  let url;
  try { url = new URL(href, location.href); } catch { return false; }
  if (url.origin !== location.origin) return false;
  const file = url.pathname.split("/").pop() || "index.html";
  // The "‹ The Lab" back-link every bench carries. Inside the frame it loads a
  // second complete Lab shell into the first; up here it is one route change,
  // and lab.html#slug lands on that bench. routeId() is the allowlist, so an
  // unknown slug becomes the overview rather than reaching navigate() raw.
  if (file === "lab.html") { navigate(routeId(url.hash.slice(1))); return true; }
  const r = PAGE_ROUTE.get(file);
  if (!r) return false;
  if (r.depth != null) depthSel[r.id] = r.depth;
  navigate(r.id);
  return true;
}

window.addEventListener("message", (e) => {
  if (e.origin !== location.origin) return;
  const d = e.data;
  if (!d || d.source !== "scv-lab" || d.type !== "navigate") return;
  const frame = document.querySelector(".bench-frame");
  if (!frame || e.source !== frame.contentWindow) return; // only the bench actually on screen
  // No place for it here: tell the frame to go on its own. The destination is
  // never sent back — the frame still holds the href it read from its own DOM.
  if (!followFramed(d.href)) e.source.postMessage({ source: "scv-lab-shell", type: "proceed" }, location.origin);
});

// The ink that reads best on a #rrggbb accent, by WCAG contrast: dark on the
// bright accents (canary yellow ≈1.85:1 under white), white on the deep ones.
function accentInk(hex) {
  const lin = (v) => { v /= 255; return v <= 0.03928 ? v / 12.92 : Math.pow((v + 0.055) / 1.055, 2.4); };
  const c = hex.replace("#", "");
  const L = 0.2126 * lin(parseInt(c.slice(0, 2), 16)) + 0.7152 * lin(parseInt(c.slice(2, 4), 16)) + 0.0722 * lin(parseInt(c.slice(4, 6), 16));
  return 1.05 / (L + 0.05) >= (L + 0.05) / 0.058 ? "#fff" : "#1f1608";
}

function overviewView() {
  const done = visited();
  // Each card owns its stage's accent as --ac and its place in line as --i —
  // the CSS does the rest: colored number badge, hover glow and shine sweep in
  // that color, and a staggered entrance that walks the line 1 → 6.
  const cards = M.stages.map((s, i) => {
    const n = (s.tracks ? s.tracks.flatMap(t => t.benches) : s.benches).length;
    const isDone = done.has(s.id);
    return h("button", { class: "scard" + (isDone ? " done" : ""),
        style: `--ac:${s.accent};--ac-ink:${accentInk(s.accent)};--i:${i}`, onclick: () => navigate(firstOf(s).slug) },
      h("span", { class: "sc-shine", "aria-hidden": "true" }),
      h("div", { class: "top" },
        h("span", { class: "circ" }, isDone ? h("span", { class: "sc-chk", html: CHECK }) : String(s.n)),
        h("h3", {}, s.name),
        h("span", { class: "sc-go", "aria-hidden": "true", html:
          '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" stroke-linejoin="round"><path d="M5 12h14M13 6l6 6-6 6"/></svg>' })),
      h("div", { class: "verb" }, s.verb),
      h("div", { class: "cnt" }, n + (n === 1 ? " bench" : " benches") + ((s.options || s.fork) ? " · options" : "")));
  });
  const nDone = M.stages.filter(s => done.has(s.id)).length;
  return h("div", { class: "ov" },
    h("div", { class: "ov-hero" },
      h("div", { class: "ov-glow", "aria-hidden": "true" }),
      h("div", { class: "room-wrap" },
        h("iframe", { class: "room", src: "room.html", loading: "eager",
          title: "The Canary Lab — a workshop you can walk. Tap a station to open its bench." }))),
    h("div", { class: "ov-line" },
      h("div", { class: "ov-cards-head" }, "Jump straight to a stage"),
      h("div", { class: "ov-prog", role: "img",
          "aria-label": nDone + " of " + M.stages.length + " stages visited" },
        ...M.stages.map(s => h("span", { class: "pip" + (done.has(s.id) ? " lit" : ""), style: `--ac:${s.accent}` })),
        h("span", { class: "pt" }, nDone + "/" + M.stages.length + " visited"))),
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
        b.noun, b.real ? h("span", { class: "rf" }) : null)),
      // The stage's pages on securacv.com. They used to hang off each bench
      // card, which is gone now that a bench opens as itself — but they are a
      // property of the STAGE, not of any one bench, so this per-stage index is
      // where they belong, and listing them once beats repeating them under
      // every bench in the stage.
      s.site && s.site.length
        ? h("div", { class: "sitelinks" },
            h("span", { class: "k" }, "on securacv.com"),
            h("div", {}, ...s.site.map(x => h("a", { class: "chip", href: siteHref(x.href),
              target: "_blank", rel: "noopener" }, x.noun))))
        : null);
  });
  return h("div", {},
    h("div", { class: "crumbs" }, h("b", {}, "Reference")),
    h("div", { class: "c-h" }, h("h1", {}, "All benches, by stage")),
    h("div", { class: "allgrid" }, ...cols),
  );
}

/* The family, consumed live. devices/ecosystem.json is the ONE place the
   SecuraCV family is named — each surface's canonical URL and an honest
   availability status (see canary-local/tests/ecosystem.test.js, the drift
   gate over the hand-mirrored consumers). This view deliberately owns no
   copy that could rot: names, links and availability all come off the JSON
   at render time, and the status line is DERIVED from the status word — so
   the day a status flips (store-pending → shipping), this page tells the
   new story with no edit. Only the status vocabulary itself is mirrored
   here, and the gate pins it to the map's. */
const FAMILY_STATUS_COPY = {
  "shipping": "Shipping — install it today.",
  "in-browser": "No install — it runs right here in the browser.",
  "store-pending": "Built and tested; store availability pending.",
};
function familyView() {
  const grid = h("div", { class: "allgrid" },
    h("p", {}, "Reading the family map…"));
  fetch("devices/ecosystem.json", { cache: "no-cache" })
    .then((r) => { if (!r.ok) throw new Error("ecosystem " + r.status); return r.json(); })
    .then((eco) => {
      grid.replaceChildren(...Object.entries(eco.surfaces).map(([id, s]) =>
        h("div", { class: "allcol" },
          h("h4", {}, s.name),
          h("div", { class: "sitelinks" },
            h("span", { class: "k" }, FAMILY_STATUS_COPY[s.status] || s.status),
            h("div", {}, h("a", { class: "chip", href: s.url, target: "_blank",
              rel: "noopener" }, "Open"))))));
    })
    .catch(() => {
      grid.replaceChildren(h("p", {},
        "The family map didn't load. Every surface is still reachable from the complete site map."));
    });
  return h("div", {},
    h("div", { class: "crumbs" }, h("b", {}, "Reference")),
    h("div", { class: "c-h" }, h("h1", {}, "SecuraCV everywhere")),
    h("p", {}, "Every window onto the same fleet — the Lab, the apps, the Wall, the hub. One machine-readable map names them all, read live by this page."),
    grid);
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
  // Ask the native shell whether this build self-updates BEFORE the first
  // render, so the sidebar and the route allowlist are built against the real
  // answer rather than a stale default. Resolves false on the website and on
  // iOS/iPadOS (App Store updates), true only on the desktop app.
  await probeSettings();
  VALID_IDS = ["overview", "start", "all", "family", ...(SETTINGS_AVAILABLE ? ["settings"] : []), ...ROUTE.map((e) => e.bench.slug)];
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
