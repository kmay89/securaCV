/* lab-nav — the one bar every Lab page shares, rendered from build-line.json.
   No build step; pure ES module; one line per page to include it.

   Why it exists: every bench page used to hand-write its own "still on
   SecuraCV" bar and its own back-links, and they drifted — some pages had no
   way back to the Lab at all, and inside the native app (Tauri serves the
   staged bundle as the whole origin, with no browser chrome) an external link
   navigated the webview off the bundle and stranded the user with no Back
   button. This module upgrades the static bar in place from the SAME manifest
   the Lab shell, the isometric room, and the sitemap render from, so the nav
   cannot rot or disagree across pages, and it routes external links through
   the OS browser when running inside the app.

   Modes, decided at boot:
   - shell/minimal (lab.html itself, or an embedded frame): keep the bar as
     authored; only apply the app adaptations.
   - bench (page found in the manifest): rebuild the bar — back to the Lab in
     context, stage › bench breadcrumb, prev/next along the build line.
   - support (any other page carrying the bar, e.g. site-map.html): rebuild
     with back-to-Lab + brand, no crumbs.

   tests/lab_nav.test.js is the parity gate: every page the manifest reaches
   must include this module and the canonical static bar (the no-JS fallback). */

const MANIFEST_URL = "build-line.json";
const SITE_ORIGIN = "https://securacv.com";
const PKEY = "scv-lab-progress"; // shared with lab-shell.js — same key, same shape

const IN_APP = !!window.__TAURI__;

/* ---- tiny hyperscript (same idiom as lab-shell.js) ---- */
function h(tag, attrs, ...kids) {
  const n = document.createElement(tag);
  if (attrs) for (const [k, v] of Object.entries(attrs)) {
    if (v == null || v === false) continue;
    if (k === "class") n.className = v;
    else n.setAttribute(k, v === true ? "" : v);
  }
  for (const c of kids.flat()) {
    if (c == null || c === false) continue;
    n.append(c.nodeType ? c : document.createTextNode(String(c)));
  }
  return n;
}

/* ---- app adaptations (run on every page, every mode) ----
   1. Truthful tagline: "runs in your browser" is wrong inside the app.
   2. External links open in the OS browser via the opener plugin; the
      webview never leaves the bundled Lab. Capture phase so a page can't
      accidentally re-route an external link into the webview first. */
function appAdapt() {
  if (!IN_APP) return;
  document.documentElement.classList.add("scv-app");
  const where = document.querySelector(".scv-bar .scv-where");
  if (where) where.textContent = "canary.local — runs on this device, nothing uploaded";

  const opener = window.__TAURI__.opener;
  if (!opener || !opener.openUrl) return; // capability missing: leave links alone
  document.addEventListener("click", (e) => {
    const a = e.target && e.target.closest && e.target.closest("a[href]");
    if (!a) return;
    let url;
    try { url = new URL(a.getAttribute("href"), location.href); } catch { return; }
    if ((url.protocol === "http:" || url.protocol === "https:") && url.origin !== location.origin) {
      e.preventDefault();
      e.stopPropagation();
      opener.openUrl(url.href);
    }
  }, true);
}

/* ---- progress (shared with the shell's sidebar checkmarks) ---- */
function markVisited(stageId) {
  try {
    const s = new Set(JSON.parse(localStorage.getItem(PKEY) || "[]"));
    s.add(stageId);
    localStorage.setItem(PKEY, JSON.stringify([...s]));
  } catch { /* private mode: progress just isn't remembered */ }
}

/* ---- model: flatten the manifest into the walkable line ---- */
function flatten(M) {
  const route = [];
  for (const stage of M.stages) {
    const benches = stage.tracks
      ? stage.tracks.flatMap((t) => t.benches.map((b) => ({ stage, track: t, bench: b })))
      : (stage.benches || []).map((b) => ({ stage, bench: b }));
    route.push(...benches);
  }
  return route;
}

// Which manifest entry is THIS page? Bench pages match on bench.lab; folded
// depth pages (e.g. senselab.html under sense) match a depth and inherit the
// parent bench's place on the line.
function locate(M, route, file) {
  if (M.onramp && M.onramp.lab === file) return { onramp: true };
  for (const e of route) {
    if (e.bench.lab === file) return { entry: e };
    for (const d of e.bench.depths || []) {
      if (d.lab === file) return { entry: e, depth: d };
    }
  }
  return null;
}

const ellip = (s, n) => (s.length > n ? s.slice(0, n - 1).trimEnd() + "…" : s);

/* ---- the upgraded bar ---- */
function rebuildBar(bar, M, route, at) {
  const entry = at && at.entry;
  const slug = entry ? entry.bench.slug : at && at.onramp ? "start" : null;
  const whereText = IN_APP
    ? "canary.local — runs on this device, nothing uploaded"
    : "canary.local — runs in your browser, nothing uploaded";

  const crumbs = [];
  if (entry) {
    crumbs.push(
      h("span", { class: "scv-sep", "aria-hidden": "true" }, "/"),
      h("span", { class: "scv-crumb" }, entry.stage.n + " · " + entry.stage.name),
      h("span", { class: "scv-sep", "aria-hidden": "true" }, "›"),
    );
    if (at.depth) {
      crumbs.push(
        h("a", { class: "scv-crumb", href: entry.bench.lab }, entry.bench.noun),
        h("span", { class: "scv-sep", "aria-hidden": "true" }, "›"),
        h("b", { class: "scv-cur" }, ellip(at.depth.label, 34)),
      );
    } else {
      crumbs.push(h("b", { class: "scv-cur" }, entry.bench.noun));
    }
  } else if (at && at.onramp) {
    crumbs.push(
      h("span", { class: "scv-sep", "aria-hidden": "true" }, "/"),
      h("b", { class: "scv-cur" }, M.onramp.noun),
    );
  }

  const nav = h("span", { class: "scv-nav" });
  if (entry) {
    const i = route.indexOf(entry);
    const prev = route[i - 1], next = route[i + 1];
    if (prev) nav.append(h("a", { class: "scv-pn", href: prev.bench.lab, title: "Previous on the build line — " + prev.stage.name }, "‹ ", h("span", { class: "scv-pn-lb" }, prev.bench.noun)));
    if (next) nav.append(h("a", { class: "scv-pn", href: next.bench.lab, title: "Next on the build line — " + next.stage.name }, h("span", { class: "scv-pn-lb" }, next.bench.noun), " ›"));
  }
  nav.append(
    h("a", { href: SITE_ORIGIN + "/glossary" }, "Glossary"),
    h("a", { href: SITE_ORIGIN + "/canary" }, "Help"),
    h("span", { class: "scv-where" }, whereText),
  );

  const fresh = h("nav", { class: "scv-bar scv-bar-lab", "aria-label": "Lab navigation" },
    h("a", { class: "scv-back", href: slug ? "lab.html#" + slug : "lab.html" }, "‹ The Lab"),
    h("span", { class: "scv-sep", "aria-hidden": "true" }, "·"),
    h("a", { class: "scv-home", href: SITE_ORIGIN + "/" }, "SecuraCV"),
    ...crumbs,
    nav,
  );
  bar.replaceWith(fresh);
  if (entry) markVisited(entry.stage.id);
}

/* ---- boot ---- */
async function boot() {
  appAdapt();

  if (document.getElementById("lab-shell")) return;   // the shell has a sidebar

  const bar = document.querySelector("nav.scv-bar");
  if (!bar) return;

  // Embedded in the Lab shell (which frames a bench so it can be USED rather
  // than described): the shell already shows where you are and the way onward,
  // so a second bar inside the frame is duplicate chrome that also steals a
  // strip off every bench. Hide it and leave the rest of the page alone —
  // `document` is enough to mark, so a page with its own styles can key off it.
  if (window !== window.top) {
    document.documentElement.classList.add("scv-embedded");
    // `hidden` alone loses: every page styles `.scv-bar { display: flex }`, and
    // a class selector outranks the [hidden] UA rule, so the bar stays visible
    // while reporting hidden === true. An important inline style is the one
    // thing no page stylesheet can outrank.
    bar.hidden = true;
    bar.style.setProperty("display", "none", "important");
    return;
  }

  // Layout for the rebuilt bar rides its own stylesheet (a real file, not an
  // injected <style> block — flash.html's CSP allows only same-origin sheets).
  if (!document.querySelector('link[href$="lab-nav.css"]')) {
    document.head.append(h("link", { rel: "stylesheet", href: "assets/lab-nav.css" }));
  }

  let M;
  try {
    const res = await fetch(MANIFEST_URL, { cache: "no-cache" });
    if (!res.ok) throw new Error("manifest " + res.status);
    M = await res.json();
  } catch {
    return; // manifest unreachable: the static bar keeps working as authored
  }
  const route = flatten(M);
  const file = (location.pathname.split("/").pop() || "index.html");
  rebuildBar(bar, M, route, locate(M, route, file));
}
boot();
