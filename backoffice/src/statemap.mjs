// A tile map of the states: one square per state on a grid, colored by a
// value. No geometry fetched, no tiles, nothing leaves the page — the
// honest map for a company that will not send a customer's address to a
// map provider. Sequential: one hue, light to dark, five steps.
import { escapeHtml as e } from './http.mjs';

const GRID = { AK: [0, 0], ME: [10, 0], VT: [9, 1], NH: [10, 1], WA: [0, 2], ID: [1, 2], MT: [2, 2], ND: [3, 2], MN: [4, 2], IL: [5, 2], WI: [6, 2], MI: [7, 2], NY: [8, 2], RI: [9, 2], MA: [10, 2],
  OR: [0, 3], NV: [1, 3], WY: [2, 3], SD: [3, 3], IA: [4, 3], IN: [5, 3], OH: [6, 3], PA: [7, 3], NJ: [8, 3], CT: [9, 3], CA: [0, 4], UT: [1, 4], CO: [2, 4], NE: [3, 4], MO: [4, 4], KY: [5, 4], WV: [6, 4], VA: [7, 4], MD: [8, 4], DE: [9, 4],
  AZ: [1, 5], NM: [2, 5], KS: [3, 5], AR: [4, 5], TN: [5, 5], NC: [6, 5], SC: [7, 5], DC: [8, 5], OK: [3, 6], LA: [4, 6], MS: [5, 6], AL: [6, 6], GA: [7, 6], HI: [0, 7], TX: [3, 7], FL: [8, 7] };
const STEPS = ['rgba(180,130,11,.10)', 'rgba(180,130,11,.28)', 'rgba(180,130,11,.46)', 'rgba(180,130,11,.68)', 'rgba(180,130,11,.92)'];

export function stateTileMap(values, { title = 'By state', fmt = (v) => String(v), sub = null } = {}) {
  const vals = Object.values(values).filter(v => v > 0).sort((a, b) => a - b);
  const q = (v) => { if (!v) return -1; let i = 0; for (const cut of [0.2, 0.4, 0.6, 0.8]) if (v > vals[Math.min(vals.length - 1, Math.floor(cut * vals.length))]) i++; return Math.min(4, i); };
  const S = 54, G = 4, W = 11 * S, H = 8 * S;
  const tiles = Object.entries(GRID).map(([st, [c, r]]) => { const v = values[st] || 0; const k = q(v); return `<g class="col"><rect x="${c * S + G / 2}" y="${r * S + G / 2}" width="${S - G}" height="${S - G}" rx="6" fill="${k < 0 ? 'var(--line)' : STEPS[k]}" stroke="var(--card)" stroke-width="1"/><text x="${c * S + S / 2}" y="${r * S + S / 2 - 3}" text-anchor="middle" font-size="12" font-weight="600" fill="${k >= 3 ? '#1a1300' : 'var(--fg)'}">${st}</text><text x="${c * S + S / 2}" y="${r * S + S / 2 + 12}" text-anchor="middle" font-size="10.5" fill="${k >= 3 ? '#1a1300' : 'var(--muted)'}">${v ? e(fmt(v)) : ''}</text><title>${st}: ${e(fmt(v))}${sub?.[st] ? ` · ${e(sub[st])}` : ''}</title></g>`; }).join('');
  const rows = Object.entries(values).filter(([, v]) => v > 0).sort((a, b) => b[1] - a[1]);
  return `<div class="card viz"><div class="k">${e(title)}</div><svg viewBox="0 0 ${W} ${H}" class="chart" role="img" aria-label="${e(title)}">${tiles}</svg><div class="muted" style="font-size:12px;margin-top:.3rem">Darker is more. A tile map, so every state is the same size and nothing is fetched from a map provider.</div><details class="viztable"><summary>Table</summary><table><thead><tr><th>State</th><th class="num">Value</th></tr></thead><tbody>${rows.map(([st, v]) => `<tr><td>${st}</td><td class="num">${e(fmt(v))}</td></tr>`).join('')}</tbody></table></details></div>`;
}
