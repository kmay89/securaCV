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

    const optToggle = el("label", "over-toggle bom-toggle");
    const cb = document.createElement("input");
    cb.type = "checkbox";
    optToggle.append(cb, document.createTextNode(" show optional parts"));
    wrap.append(optToggle);

    const table = el("div", "bom-table");
    const render = () => {
      table.innerHTML = "";
      for (const r of d.bom.rows || []) {
        if (!r.required && !cb.checked) continue;
        const row = el("div", "bom-row" + (r.required ? "" : " bom-opt"));
        const top = el("div", "bom-row-top");
        top.append(
          el("code", "bom-ref", r.ref),
          el("span", "bom-desc", r.desc),
          el("span", "bom-usd", r.usd ? `$${r.usd.toFixed(2)}` : "—")
        );
        const meta = el("div", "bom-row-meta");
        meta.append(
          el("span", "chip chip-dim", r.category || "part"),
          el("span", null, `×${r.qty}`),
          r.required ? el("span", "chip chip-live", "required")
                     : el("span", "chip", "optional"),
          r.mpn ? el("span", "muted", `${r.mfr} · ${r.mpn}`) : ""
        );
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
    wrap.append(table);
    const src = el("p", "muted fineprint");
    src.append("Generated from ");
    const a = el("a", null, d.bom.source);
    a.href = GH + d.bom.source;
    a.target = "_blank";
    a.rel = "noopener";
    src.append(a, document.createTextNode(" — MPNs, distributor SKUs, lifecycle and RoHS columns live there."));
    wrap.append(src);
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
