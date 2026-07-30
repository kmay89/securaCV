// canary-local/assets/catalog-browse.js — the enclosure catalog, browsable.
//
// Phase 3b of docs/hardware/enclosure/CATALOG_ARCHITECTURE.md (§6B, faceted
// browse): a gallery over ALL products in the generated manifest
// (devices/catalog.json), with facets that narrow — the "which case?" browse
// path the per-device workshop can't give (it only configures the five devices
// with firmware + a BOM). Everything here is read straight from the manifest,
// so it can't drift from the SCADs.
//
// The pure helpers (facetsFor / applyFacets / productSummary) are exported and
// unit-tested (tests/catalog_browse.test.js); only a real page boots the UI.

const el = (tag, cls, text) => {
  const n = document.createElement(tag);
  if (cls) n.className = cls;
  if (text != null) n.textContent = text;
  return n;
};

const GH = "https://github.com/kmay89/securaCV/blob/main/docs/hardware/enclosure/";
const ENC_BASE = "../docs/hardware/enclosure/";

// ── facet model (pure) ───────────────────────────────────────────────────

// The environment label for a product (design intent — never "verified").
export function envLabel(product) {
  const e = product.env;
  if (!e) return "unrated";
  if (typeof e.cer === "number") return `CER-${e.cer}`;
  if (e.ip) return "sealed";
  return "rated";
}

// A one-line, render-ready summary of a product (what a card needs).
export function productSummary(product) {
  const variants = product.variants || [];
  const flavors = variants.filter((v) => v.selects && Object.keys(v.selects).length);
  const preview = variants.find((v) => v.preview);
  return {
    id: product.id,
    scad: product.scad,
    title: product.title || product.id,
    family: product.family || "universal",
    devices: product.device_compat || [],
    klass: product.class || "primary",
    env: product.env || null,
    envLabel: envLabel(product),
    released: variants.filter((v) => v.status === "released").length,
    variants: variants.length,
    flavors: flavors.length,
    userOptions: (product.counts && product.counts.user_options) || 0,
    alternatives: product.alternatives || [],
    preview: preview ? preview.preview : null,
  };
}

// Derive the facet groups + their values (with product counts) from the
// manifest. Each group is {key, label, values:[{value, label, count}]}.
export function facetsFor(products) {
  const sums = products.map(productSummary);
  const tally = (fn) => {
    const m = new Map();
    for (const s of sums) for (const v of [].concat(fn(s))) {
      if (v == null) continue;
      m.set(v, (m.get(v) || 0) + 1);
    }
    return m;
  };
  const grp = (key, label, fn, order) => {
    const m = tally(fn);
    let values = [...m.entries()].map(([value, count]) => ({ value, count }));
    values.sort(order || ((a, b) => b.count - a.count || String(a.value).localeCompare(b.value)));
    return { key, label, values };
  };
  return [
    grp("family", "Family", (s) => s.family),
    grp("device", "Fits device", (s) => s.devices),
    grp("env", "Environment", (s) => s.envLabel),
    grp("status", "Status",
      (s) => (s.released ? "released" : "in development")),
    grp("type", "Type", (s) => s.klass),
    grp("configurable", "Options",
      (s) => (s.userOptions ? "configurable" : "fixed")),
  ];
}

// Narrow the product list to the active facet selections. Semantics: AND
// across groups, OR within a group (McMaster-style). `active` is
// {groupKey: Set(values)}; an empty/absent set for a group means "any".
export function applyFacets(products, active) {
  const testers = {
    family: (s) => [s.family],
    device: (s) => s.devices,
    env: (s) => [s.envLabel],
    status: (s) => [s.released ? "released" : "in development"],
    type: (s) => [s.klass],
    configurable: (s) => [s.userOptions ? "configurable" : "fixed"],
  };
  return products.filter((p) => {
    const s = productSummary(p);
    for (const [key, sel] of Object.entries(active || {})) {
      if (!sel || !sel.size) continue;
      const mine = testers[key] ? testers[key](s) : [];
      if (!mine.some((v) => sel.has(v))) return false;
    }
    return true;
  });
}

// ── rendering (DOM) ──────────────────────────────────────────────────────

function card(product, byId) {
  const s = productSummary(product);
  const c = el("article", "cat-card");
  c.id = "p-" + s.id;
  if (s.preview) {
    const img = el("img", "cat-thumb");
    img.src = ENC_BASE + s.preview;
    img.alt = s.title;
    img.loading = "lazy";
    c.append(img);
  }
  const body = el("div", "cat-body");
  body.append(el("h3", null, s.title));
  const meta = el("div", "cat-meta");
  meta.append(el("span", "cat-fam", s.family));
  if (s.env) {
    const b = el("span", "cat-badge", `${s.envLabel} · target`);
    b.title = "design intent, not a verified rating (field_ratings.md)";
    meta.append(b);
  }
  meta.append(el("span", "cat-tag", s.released ? "released" : "in development"));
  if (s.userOptions) meta.append(el("span", "cat-tag", `${s.userOptions} options`));
  if (s.flavors) meta.append(el("span", "cat-tag", `${s.flavors} flavors`));
  body.append(meta);

  const links = el("div", "cat-links");
  const scad = el("a", "cat-scad", "open in OpenSCAD ↗");
  scad.href = GH + s.scad;
  scad.target = "_blank";
  scad.rel = "noopener";
  links.append(scad);
  // a workshop device it maps to? offer the guided configurator
  const dev = s.devices.find((d) => d && d !== "_universal");
  if (dev) {
    const w = el("a", "cat-workshop", "configure in the workshop →");
    w.href = `workshop.html#${dev}`;
    links.append(w);
  }
  body.append(links);

  if (s.alternatives.length) {
    const alt = el("div", "cat-alts");
    alt.append(el("span", "muted", "see also: "));
    s.alternatives.forEach((a, i) => {
      if (byId.has(a)) {
        const link = el("a", null, byId.get(a).title || a);
        link.href = "#p-" + a;
        alt.append(link);
      } else {
        alt.append(el("span", null, a));
      }
      if (i < s.alternatives.length - 1) alt.append(document.createTextNode(" · "));
    });
    body.append(alt);
  }
  c.append(body);
  return c;
}

export function buildCatalogBrowse(catalog, mount) {
  mount.innerHTML = "";
  if (!catalog || !Array.isArray(catalog.products)) {
    mount.append(el("p", "muted", "Catalog manifest unavailable."));
    return;
  }
  const products = catalog.products;
  const byId = new Map(products.map((p) => [p.id, productSummary(p)]));
  const active = {};

  const facetBar = el("aside", "cat-facets");
  const grid = el("div", "cat-grid");
  const count = el("p", "cat-count muted");

  function render() {
    const shown = applyFacets(products, active);
    count.textContent = `${shown.length} of ${products.length} cases`;
    grid.innerHTML = "";
    for (const p of shown) grid.append(card(p, byId));
    if (!shown.length) grid.append(el("p", "muted", "No cases match those facets."));
  }

  facetBar.append(el("h2", null, "Narrow it down"));
  for (const group of facetsFor(products)) {
    const g = el("details", "cat-fgroup");
    g.open = true;
    g.append(el("summary", null, group.label));
    for (const { value, count: n } of group.values) {
      const row = el("label", "cat-facet");
      const box = el("input");
      box.type = "checkbox";
      box.addEventListener("change", () => {
        active[group.key] = active[group.key] || new Set();
        if (box.checked) active[group.key].add(value);
        else active[group.key].delete(value);
        render();
      });
      row.append(box, el("span", "cat-facet-lbl", String(value)),
        el("span", "cat-facet-n muted", String(n)));
      g.append(row);
    }
    facetBar.append(g);
  }
  const clear = el("button", "small", "clear all");
  clear.addEventListener("click", () => {
    for (const k of Object.keys(active)) active[k] = new Set();
    for (const b of facetBar.querySelectorAll("input")) b.checked = false;
    render();
  });
  facetBar.append(clear);

  const layout = el("div", "cat-layout");
  const main = el("div", "cat-main");
  main.append(count, grid);
  layout.append(facetBar, main);
  mount.append(layout);
  render();
}

if (typeof document !== "undefined") {
  const mount = document.getElementById("catalog");
  if (mount) {
    fetch("devices/catalog.json")
      .then((r) => r.json())
      .then((cat) => buildCatalogBrowse(cat, mount))
      .catch((e) => {
        mount.append(el("p", "muted",
          `The catalog could not load (${e.message}). It is generated into ` +
          "devices/catalog.json by canary-local/tools/gen_enclosures.py."));
      });
  }
}
