// canary-local/assets/build-it.js — how to build it, without a second
// copy that rots. Everything rendered here is generated from the sources
// maintainers already edit (devices/build.json ← docs/hardware/bom_*.csv,
// the enclosure README's Assembly sections, sbom/README.md) and
// drift-gated in CI — the fun stays honest because the data can't go
// stale silently.

const el = (tag, cls, text) => {
  const n = document.createElement(tag);
  if (cls) n.className = cls;
  if (text != null) n.textContent = text;
  return n;
};

const GH = "https://github.com/kmay89/securaCV/blob/main/";

export function buildBuildIt(buildData, dev) {
  const wrap = el("div", "buildit");
  if (!buildData) {
    wrap.append(el("p", "muted", "Build data unavailable."));
    return wrap;
  }
  const d = buildData.devices?.[dev.id] || {};

  // ── BOM ──
  if (d.bom) {
    const bomHead = el("div", "bom-head");
    bomHead.append(el("h4", null, "Bill of materials"));
    const totals = el("span", "bom-totals");
    totals.append(
      el("strong", null, `$${d.bom.required_usd.toFixed(2)}`),
      document.createTextNode(" required · "),
      el("span", "muted", `$${d.bom.full_usd.toFixed(2)} with every option`)
    );
    bomHead.append(totals);
    wrap.append(bomHead);

    if (d.bom.pricing_as_of) {
      const asOf = el("p", "muted fineprint bom-asof");
      let line = `Supply-chain snapshot ${d.bom.pricing_as_of.slice(0, 10)}`;
      if (d.bom.required_usd_live != null &&
          d.bom.required_usd_live !== d.bom.required_usd) {
        line += ` — live required total $${d.bom.required_usd_live.toFixed(2)}`;
      }
      asOf.textContent = line + " · distributor-verified rows are marked live";
      wrap.append(asOf);
    }

    // ── Pick your build ──
    // Required rows are always in; every optional row is a checkbox. The
    // running total, and the order panel below, follow the selection —
    // and the CSV's own recipe rows become one-click presets, so "the
    // sealed outdoor build" is a single tap, not a parts-catalog hunt.
    const sel = new Set();
    const extOf = (r) => r.live
      ? r.live.unit_usd * (Number(r.qty) || 1) : (r.usd || 0);
    const reqBase = d.bom.required_usd_live != null
      ? d.bom.required_usd_live : d.bom.required_usd;

    const yourBuild = el("p", "bom-yourbuild");
    const updateBuild = () => {
      let total = reqBase;
      for (const r of d.bom.rows || []) {
        if (!r.required && sel.has(r.ref)) total += extOf(r);
      }
      yourBuild.textContent = "";
      yourBuild.append(
        el("strong", null, `Your build: $${total.toFixed(2)}`),
        el("span", "muted",
          sel.size ? ` — required parts + ${sel.size} option${
            sel.size === 1 ? "" : "s"}` : " — required parts only")
      );
    };
    wrap.append(yourBuild);

    const optToggle = el("label", "over-toggle bom-toggle");
    const cb = document.createElement("input");
    cb.type = "checkbox";
    optToggle.append(cb, document.createTextNode(" show optional parts"));

    if ((d.bom.recipes || []).length) {
      const chips = el("div", "bom-recipes");
      chips.append(el("span", "muted", "Build recipes:"));
      for (const rec of d.bom.recipes) {
        const b = el("button", "btn ghost bom-recipe", rec.label);
        b.type = "button";
        if (rec.note) b.title = rec.note;
        b.addEventListener("click", () => {
          sel.clear();
          for (const ref of rec.refs || []) sel.add(ref);
          if (sel.size) cb.checked = true;
          render();
          updateBuild();
          updateStat();
        });
        chips.append(b);
      }
      wrap.append(chips);
    }
    wrap.append(optToggle);

    const table = el("div", "bom-table");
    const render = () => {
      table.innerHTML = "";
      for (const r of d.bom.rows || []) {
        if (!r.required && !cb.checked && !sel.has(r.ref)) continue;
        const row = el("div", "bom-row" + (r.required ? "" : " bom-opt") +
                       (sel.has(r.ref) ? " bom-picked" : ""));
        const liveExt = r.live
          ? r.live.unit_usd * (Number(r.qty) || 1) : null;
        const top = el("div", "bom-row-top");
        if (!r.required) {
          const pick = document.createElement("input");
          pick.type = "checkbox";
          pick.className = "bom-pick";
          pick.checked = sel.has(r.ref);
          pick.title = "add to your build";
          pick.addEventListener("click", (ev) => ev.stopPropagation());
          pick.addEventListener("change", () => {
            if (pick.checked) sel.add(r.ref); else sel.delete(r.ref);
            row.classList.toggle("bom-picked", pick.checked);
            updateBuild();
            updateStat();
          });
          top.append(pick);
        }
        top.append(
          el("code", "bom-ref", r.ref),
          el("span", "bom-desc", r.desc),
          el("span", "bom-usd", liveExt != null ? `$${liveExt.toFixed(2)}`
                                : r.usd ? `$${r.usd.toFixed(2)}` : "—")
        );
        const meta = el("div", "bom-row-meta");
        meta.append(
          el("span", "chip chip-dim", r.category || "part"),
          el("span", null, `×${r.qty}`),
          r.required ? el("span", "chip chip-live", "required")
                     : el("span", "chip", "optional"),
          r.mpn ? el("span", "muted", `${r.mfr} · ${r.mpn}`) : ""
        );
        if (r.live) {
          meta.append(el("span", "chip chip-live",
            `live · ${r.live.src}` +
            (typeof r.live.stock === "number"
              ? ` · ${r.live.stock.toLocaleString()} in stock` : "")));
        }
        row.append(top, meta);
        if (r.notes) {
          const note = el("div", "bom-note", r.notes);
          note.hidden = true;
          row.append(note);
          row.addEventListener("click", () => { note.hidden = !note.hidden; });
          row.classList.add("bom-has-note");
        }
        table.append(row);
      }
    };
    cb.addEventListener("change", render);
    render();
    updateBuild();
    wrap.append(table);
    const src = el("p", "muted fineprint");
    src.append("Generated from ");
    const a = el("a", null, d.bom.source);
    a.href = GH + d.bom.source;
    a.target = "_blank";
    a.rel = "noopener";
    src.append(a, document.createTextNode(" — MPNs, distributor SKUs, lifecycle and RoHS columns live there."));
    wrap.append(src);

    // ── Order the parts ──
    // No unverified APIs, no accounts, no magic that can silently break:
    // one click copies distributor-ready "MPN,qty" lines for exactly the
    // build picked above and opens the distributor's own list/BOM page —
    // paste, and the cart prices itself.
    const buildRows = () => (d.bom.rows || []).filter(
      r => r.required || sel.has(r.ref));
    const orderRows = () => buildRows().filter(r => r.orderable);
    const orderLines = () => orderRows()
      .map(r => `${r.mpn},${r.qty}`).join("\n");

    const order = el("div", "bom-order");
    order.append(el("h4", null, "Order the parts"));
    const ostat = el("p", "muted fineprint");
    const updateStat = () => {
      const n = orderRows().length;
      const g = buildRows().length - n;
      ostat.textContent =
        `${n} orderable part number${n === 1 ? "" : "s"} in your build` +
        (g ? ` (+${g} generic item${g === 1 ? "" : "s"} — cables, screws, ` +
             `filament — any hardware store has them; they're in the CSV ` +
             `download)` : "") + `.`;
    };
    updateStat();

    const feedback = el("p", "muted fineprint");
    const manual = el("textarea", "bom-order-manual");
    manual.readOnly = true;
    manual.rows = 4;
    manual.hidden = true;

    const copyThenOpen = async (label, url) => {
      const text = orderLines();
      try {
        await navigator.clipboard.writeText(text);
        feedback.textContent =
          `✓ ${orderRows().length} part numbers copied — paste into ${label} ` +
          `(opened in a new tab) and the cart prices itself.`;
        manual.hidden = true;
      } catch {
        manual.value = text;
        manual.hidden = false;
        manual.select();
        feedback.textContent =
          `Clipboard blocked by the browser — the lines are below, select ` +
          `and copy, then paste into ${label}.`;
      }
      window.open(url, "_blank", "noopener");
    };

    const btns = el("div", "bom-order-btns");
    const mkBtn = (cls, label, fn) => {
      const b = el("button", `btn ${cls}`, label);
      b.type = "button";
      b.addEventListener("click", fn);
      return b;
    };
    btns.append(
      mkBtn("primary", "Copy list + open Digi-Key myLists", () =>
        copyThenOpen("Digi-Key myLists", "https://www.digikey.com/en/mylists")),
      mkBtn("ghost", "Copy list + open Mouser BOM tool", () =>
        copyThenOpen("Mouser's BOM tool", "https://www.mouser.com/bom/")),
      mkBtn("ghost", "Download CSV", () => {
        const rows = buildRows();
        const csv = ["MPN,Quantity,RefDes,Manufacturer,Description"]
          .concat(rows.map(r =>
            [r.mpn, r.qty, r.ref, r.mfr, `"${(r.desc || "").replace(/"/g, '""')}"`]
              .join(",")))
          .join("\n");
        const blob = new Blob([csv], { type: "text/csv" });
        const dl = document.createElement("a");
        dl.href = URL.createObjectURL(blob);
        dl.download = `securacv-${dev.id}-parts.csv`;
        dl.click();
        URL.revokeObjectURL(dl.href);
        feedback.textContent =
          `✓ CSV downloaded — both distributors' BOM tools accept it as an upload.`;
      })
    );
    order.append(ostat, btns, feedback, manual);
    wrap.append(order);
  } else if (d.bom_note) {
    const note = el("p", "ondevice");
    note.append(el("strong", null, "Honesty: "), document.createTextNode(d.bom_note));
    wrap.append(note);
  }

  // ── Assembly ──
  if (d.assembly) {
    wrap.append(el("h4", null, "Assembly"));
    const ol = el("ol", "asm-steps");
    for (const s of d.assembly.steps || []) ol.append(el("li", null, s));
    wrap.append(ol);
    const src2 = el("p", "muted fineprint");
    src2.append("From ");
    const a2 = el("a", null, d.assembly.source);
    a2.href = GH + "docs/hardware/enclosure/README.md";
    a2.target = "_blank";
    a2.rel = "noopener";
    src2.append(a2);
    wrap.append(src2);
  } else {
    const p = el("p", "muted");
    p.append("Assembly walk-through: see this device's docs (Specs tab) — the enclosure catalog carries per-case assembly for WAP and Vision today.");
    wrap.append(p);
  }

  // ── SBOM ──
  wrap.append(el("h4", null, "Software bill of materials"));
  const sp = el("p", "body", buildData.sbom?.note || "");
  wrap.append(sp);
  const sl = el("p", "muted fineprint");
  const a3 = el("a", null, "SBOM Generation workflow →");
  a3.href = buildData.sbom?.link || "#";
  a3.target = "_blank";
  a3.rel = "noopener";
  sl.append(a3);
  wrap.append(sl);

  return wrap;
}
