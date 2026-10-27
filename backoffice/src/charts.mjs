// Charts as inline SVG strings — no client script (the admin's CSP has
// none), so the hover layer is CSS: each column carries its own tooltip,
// shown on :hover. Thin marks, one axis, text in text tokens, a table view
// under every chart. Series colors are the --s1…--s5 tokens render.mjs
// defines for both color schemes (validated with the dataviz palette
// checker on the card surfaces).
import { escapeHtml as e } from './http.mjs';

const W = 720, PAD = { l: 44, r: 12, t: 14, b: 26 };
const nice = (max) => { if (max <= 0) return 1; const p = 10 ** Math.floor(Math.log10(max)); const m = max / p; const step = m <= 1 ? 0.25 : m <= 2 ? 0.5 : m <= 5 ? 1 : 2; return Math.ceil(m / step) * step * p; };
const fmtShort = (v, money) => money ? (v >= 100000 ? `$${Math.round(v / 100000) / 10}k` : `$${Math.round(v / 100)}`) : String(Math.round(v));

// Columns over time. `points`: [{ label, value }]; `money` formats cents.
export function columns({ points, title, money = false, height = 200, color = 'var(--s1)', fmt = null }) {
  const H = height, iw = W - PAD.l - PAD.r, ih = H - PAD.t - PAD.b;
  const max = nice(Math.max(0, ...points.map(p => p.value))); const n = Math.max(1, points.length);
  const slot = iw / n, bw = Math.max(2, Math.min(28, slot * 0.68));
  const y = (v) => PAD.t + ih - (v / max) * ih;
  const f = fmt || ((v) => money ? `$${(v / 100).toFixed(2)}` : String(v));
  const grid = [0, 0.5, 1].map(k => `<line x1="${PAD.l}" x2="${W - PAD.r}" y1="${y(max * k)}" y2="${y(max * k)}" class="grid"/><text x="${PAD.l - 6}" y="${y(max * k) + 4}" class="ax" text-anchor="end">${fmtShort(max * k, money)}</text>`).join('');
  const every = Math.ceil(n / 8);
  const cols = points.map((p, i) => {
    const x = PAD.l + i * slot + (slot - bw) / 2; const top = y(p.value); const h = Math.max(0, PAD.t + ih - top); const r = Math.min(4, bw / 2, h);
    const bar = h > 0 ? `<path class="bar" d="M${x},${top + r} a${r},${r} 0 0 1 ${r},-${r} h${bw - 2 * r} a${r},${r} 0 0 1 ${r},${r} V${PAD.t + ih} H${x} Z" fill="${color}"/>` : `<line x1="${x}" x2="${x + bw}" y1="${PAD.t + ih}" y2="${PAD.t + ih}" stroke="${color}" stroke-width="1"/>`;
    const tipX = Math.min(W - PAD.r - 70, Math.max(PAD.l + 70, x + bw / 2));
    return `<g class="col"><rect class="hit" x="${PAD.l + i * slot}" y="${PAD.t}" width="${slot}" height="${ih}" fill="transparent"/>${bar}${i % every === 0 ? `<text x="${x + bw / 2}" y="${H - 8}" class="ax" text-anchor="middle">${e(p.label.slice(5))}</text>` : ''}<g class="tip"><rect x="${tipX - 68}" y="${PAD.t}" width="136" height="34" rx="4"/><text x="${tipX}" y="${PAD.t + 14}" text-anchor="middle" class="t1">${e(p.label)}</text><text x="${tipX}" y="${PAD.t + 28}" text-anchor="middle" class="t2">${e(f(p.value))}</text></g></g>`;
  }).join('');
  return wrap(title, `<svg viewBox="0 0 ${W} ${H}" class="chart" role="img" aria-label="${e(title)}">${grid}<line x1="${PAD.l}" x2="${W - PAD.r}" y1="${PAD.t + ih}" y2="${PAD.t + ih}" class="axis"/>${cols}</svg>`, points, f);
}

// A line over time with its area, a hover crosshair and a marker per point.
export function line({ points, title, money = false, height = 200, color = 'var(--s1)', fmt = null }) {
  const H = height, iw = W - PAD.l - PAD.r, ih = H - PAD.t - PAD.b;
  const max = nice(Math.max(0, ...points.map(p => p.value))); const n = Math.max(1, points.length);
  const x = (i) => PAD.l + (n === 1 ? iw / 2 : i * iw / (n - 1)); const y = (v) => PAD.t + ih - (v / max) * ih;
  const f = fmt || ((v) => money ? `$${(v / 100).toFixed(2)}` : String(v));
  const path = points.map((p, i) => `${i ? 'L' : 'M'}${x(i).toFixed(1)},${y(p.value).toFixed(1)}`).join(' ');
  const area = `${path} L${x(n - 1).toFixed(1)},${PAD.t + ih} L${x(0).toFixed(1)},${PAD.t + ih} Z`;
  const grid = [0, 0.5, 1].map(k => `<line x1="${PAD.l}" x2="${W - PAD.r}" y1="${y(max * k)}" y2="${y(max * k)}" class="grid"/><text x="${PAD.l - 6}" y="${y(max * k) + 4}" class="ax" text-anchor="end">${fmtShort(max * k, money)}</text>`).join('');
  const every = Math.ceil(n / 8); const slot = iw / n;
  const cols = points.map((p, i) => { const tipX = Math.min(W - PAD.r - 70, Math.max(PAD.l + 70, x(i)));
    return `<g class="col"><rect class="hit" x="${x(i) - slot / 2}" y="${PAD.t}" width="${slot}" height="${ih}" fill="transparent"/><line class="xh" x1="${x(i)}" x2="${x(i)}" y1="${PAD.t}" y2="${PAD.t + ih}"/><circle class="pt" cx="${x(i)}" cy="${y(p.value)}" r="4" fill="${color}"/>${i % every === 0 ? `<text x="${x(i)}" y="${H - 8}" class="ax" text-anchor="middle">${e(p.label.slice(5))}</text>` : ''}<g class="tip"><rect x="${tipX - 68}" y="${PAD.t}" width="136" height="34" rx="4"/><text x="${tipX}" y="${PAD.t + 14}" text-anchor="middle" class="t1">${e(p.label)}</text><text x="${tipX}" y="${PAD.t + 28}" text-anchor="middle" class="t2">${e(f(p.value))}</text></g></g>`; }).join('');
  return wrap(title, `<svg viewBox="0 0 ${W} ${H}" class="chart" role="img" aria-label="${e(title)}">${grid}<path d="${area}" fill="${color}" opacity=".12"/><path d="${path}" fill="none" stroke="${color}" stroke-width="2" stroke-linejoin="round"/><line x1="${PAD.l}" x2="${W - PAD.r}" y1="${PAD.t + ih}" y2="${PAD.t + ih}" class="axis"/>${cols}</svg>`, points, f);
}

// Horizontal bars for a ranked list (top products, states): the label is the identity, so one color.
export function hbars({ rows, title, money = false, color = 'var(--s1)', fmt = null, max: maxIn = null }) {
  const f = fmt || ((v) => money ? `$${(v / 100).toFixed(2)}` : String(v));
  const max = maxIn ?? Math.max(1, ...rows.map(r => r.value));
  const body = rows.map(r => `<div class="hb"><div class="hl" title="${e(r.label)}">${e(r.label)}</div><div class="ht"><div class="hf" style="width:${Math.max(0.5, 100 * r.value / max).toFixed(1)}%;background:${color}"></div></div><div class="hv">${e(f(r.value))}${r.extra ? ` <span class="muted">${e(r.extra)}</span>` : ''}</div></div>`).join('');
  return `<div class="card viz"><div class="k">${e(title)}</div>${rows.length ? body : '<p class="muted">Nothing in this period.</p>'}</div>`;
}

// A tiny trend with no axes, for a table cell.
export const spark = (values, { color = 'var(--s1)', w = 120, h = 28 } = {}) => {
  const max = Math.max(1, ...values); const n = Math.max(1, values.length); const bw = w / n;
  return `<svg viewBox="0 0 ${w} ${h}" width="${w}" height="${h}" class="spark" aria-hidden="true">${values.map((v, i) => `<rect x="${(i * bw + 1).toFixed(1)}" y="${(h - (v / max) * (h - 2)).toFixed(1)}" width="${Math.max(1, bw - 2).toFixed(1)}" height="${((v / max) * (h - 2)).toFixed(1)}" fill="${color}" rx="1"/>`).join('')}</svg>`;
};

// A stat tile with its change against the previous period.
export function tile(label, value, { prev = null, cur = null, money = false, pct = false, invert = false } = {}) {
  let delta = '';
  if (prev != null && cur != null) {
    const d = prev === 0 ? (cur === 0 ? 0 : null) : (cur - prev) / Math.abs(prev);
    const good = invert ? (cur <= prev) : (cur >= prev);
    delta = d == null ? '<span class="muted">no earlier period</span>' : `<span class="${d === 0 ? 'muted' : good ? 'up' : 'down'}">${d > 0 ? '▲' : d < 0 ? '▼' : '•'} ${Math.abs(Math.round(d * 100))}%</span>`;
  }
  return `<div class="card"><div class="k">${e(label)}</div><div class="v">${value}</div>${delta ? `<div class="d">${delta}${delta.includes('no earlier') ? '' : ' <span class="muted">vs previous period</span>'}</div>` : ''}</div>`;
}

function wrap(title, svg, points, f) {
  const table = `<details class="viztable"><summary>Table</summary><table><thead><tr><th>Period</th><th class="num">Value</th></tr></thead><tbody>${points.map(p => `<tr><td>${e(p.label)}</td><td class="num">${e(f(p.value))}</td></tr>`).join('')}</tbody></table></details>`;
  return `<div class="card viz"><div class="k">${e(title)}</div>${svg}${table}</div>`;
}
