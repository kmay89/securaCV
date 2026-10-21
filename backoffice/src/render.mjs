// Server-rendered HTML, one stylesheet, no client JavaScript. The admin is a
// set of plain pages and forms so it works from a phone, a text browser and
// an AI operator's fetch() alike.
import { escapeHtml as e } from './http.mjs';

export const money = (cents, cur = 'usd') => {
  const n = (Number(cents) || 0) / 100;
  return new Intl.NumberFormat('en-US', { style: 'currency', currency: cur.toUpperCase() }).format(n);
};
export const dt = (iso) => iso ? String(iso).replace('T', ' ').replace(/\.\d+Z$/, 'Z').replace(/:\d\dZ$/, ' UTC') : '—';
export const day = (iso) => iso ? String(iso).slice(0, 10) : '—';

const CSS = `
:root{--bg:#fbfaf7;--fg:#1b1b1b;--muted:#6b6b6b;--line:#e3dfd6;--accent:#b4820b;--ok:#2d7d46;--bad:#b23a3a;--warn:#b7791f;--card:#fff}
@media(prefers-color-scheme:dark){:root{--bg:#141414;--fg:#ececec;--muted:#a0a0a0;--line:#2c2c2c;--accent:#e2b23c;--card:#1d1d1d}}
*{box-sizing:border-box}body{margin:0;font:15px/1.5 system-ui,-apple-system,Segoe UI,sans-serif;background:var(--bg);color:var(--fg)}
a{color:var(--accent)}header{display:flex;gap:1rem;align-items:center;flex-wrap:wrap;padding:.6rem 1rem;border-bottom:1px solid var(--line);background:var(--card)}
header b{margin-right:.5rem}header nav a{margin-right:.8rem;text-decoration:none;color:var(--fg)}header nav a.on{color:var(--accent);font-weight:600}
main{max-width:1100px;margin:0 auto;padding:1rem}h1{font-size:1.4rem;margin:.4rem 0 1rem}h2{font-size:1.1rem;margin:1.4rem 0 .5rem}
table{border-collapse:collapse;width:100%;background:var(--card);font-size:14px}th,td{text-align:left;padding:.45rem .6rem;border-bottom:1px solid var(--line);vertical-align:top}th{color:var(--muted);font-weight:600}
tr:hover td{background:rgba(180,130,11,.05)}.num{text-align:right;font-variant-numeric:tabular-nums}
.cards{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:.8rem;margin:1rem 0}.card{background:var(--card);border:1px solid var(--line);border-radius:8px;padding:.8rem}
.card .k{color:var(--muted);font-size:12px;text-transform:uppercase;letter-spacing:.04em}.card .v{font-size:1.5rem;font-weight:600}
.pill{display:inline-block;padding:0 .5rem;border-radius:999px;font-size:12px;border:1px solid var(--line);background:var(--card)}
.ok{color:var(--ok)}.bad{color:var(--bad)}.warn{color:var(--warn)}.muted{color:var(--muted)}
form.inline{display:inline}form.box{background:var(--card);border:1px solid var(--line);border-radius:8px;padding:1rem;margin:1rem 0;max-width:720px}
label{display:block;margin:.5rem 0 .2rem;font-size:13px;color:var(--muted)}input,select,textarea{width:100%;padding:.45rem .5rem;border:1px solid var(--line);border-radius:6px;background:var(--bg);color:var(--fg);font:inherit}
textarea{min-height:8rem}button,.btn{padding:.45rem .9rem;border-radius:6px;border:1px solid var(--accent);background:var(--accent);color:#111;font:inherit;cursor:pointer;text-decoration:none}
button.quiet{background:transparent;color:var(--fg);border-color:var(--line)}.row{display:flex;gap:.8rem;flex-wrap:wrap}.row>*{flex:1;min-width:160px}
.flash{padding:.6rem .9rem;border-radius:6px;margin:.6rem 0;background:rgba(45,125,70,.1);border:1px solid var(--ok)}.flash.err{background:rgba(178,58,58,.1);border-color:var(--bad)}
pre{background:var(--card);border:1px solid var(--line);padding:.8rem;overflow:auto;border-radius:6px;font-size:13px}.msg{border:1px solid var(--line);border-radius:8px;padding:.7rem .9rem;margin:.6rem 0;background:var(--card);white-space:pre-wrap}
.msg.out{border-left:4px solid var(--accent)}.msg.note{border-left:4px solid var(--muted);opacity:.9}.msg .meta{font-size:12px;color:var(--muted);margin-bottom:.3rem}
footer{color:var(--muted);font-size:12px;padding:2rem 1rem;text-align:center}
`;

export const NAV = [
  ['/', 'Dashboard'], ['/orders', 'Orders'], ['/tickets', 'Support'], ['/contacts', 'Contacts'], ['/inventory', 'Inventory'],
  ['/devices', 'Devices'], ['/warranty', 'Warranty'], ['/invoices', 'Invoices'], ['/campaigns', 'Mail'], ['/books', 'Books'],
  ['/status-admin', 'Status'], ['/exceptions', 'Exceptions'], ['/settings', 'Settings'],
];

export function layout({ title, body, path = '/', flash = null, company = 'Errer Labs', nav = true }) {
  const navHtml = nav ? `<nav>${NAV.map(([p, l]) => `<a href="${p}" class="${path === p || (p !== '/' && path.startsWith(p)) ? 'on' : ''}">${l}</a>`).join('')}</nav>` : '';
  const flashHtml = flash ? `<div class="flash ${flash.kind === 'err' ? 'err' : ''}">${e(flash.text)}</div>` : '';
  return `<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><meta name="robots" content="noindex"><title>${e(title)} · ${e(company)} backoffice</title><style>${CSS}</style></head>
<body><header><b>${e(company)}</b>${navHtml}${nav ? '<form method="post" action="/logout" class="inline" style="margin-left:auto"><button class="quiet">Log out</button></form>' : ''}</header>
<main>${flashHtml}<h1>${e(title)}</h1>${body}</main><footer>backoffice · one SQLite file, no SaaS · <a href="/api">API</a> · <a href="/status">public status</a></footer></body></html>`;
}

export function table(rows, cols, { empty = 'Nothing here.' } = {}) {
  if (!rows.length) return `<p class="muted">${e(empty)}</p>`;
  const head = cols.map(c => `<th class="${c.num ? 'num' : ''}">${e(c.label)}</th>`).join('');
  const body = rows.map(r => `<tr>${cols.map(c => `<td class="${c.num ? 'num' : ''}">${c.html ? c.html(r) : e(c.get ? c.get(r) : r[c.key])}</td>`).join('')}</tr>`).join('');
  return `<table><thead><tr>${head}</tr></thead><tbody>${body}</tbody></table>`;
}

export const pill = (s, cls = '') => `<span class="pill ${cls}">${e(s)}</span>`;
export const statusPill = (s) => pill(s, /paid|open|active|ok|resolved|sent|shipped|delivered/.test(s) ? 'ok' : /fail|bad|past_due|overdue|urgent|declined/.test(s) ? 'bad' : /waiting|packed|queued|draft|monitoring/.test(s) ? 'warn' : '');

export function field(name, label, { type = 'text', value = '', required = false, options = null, placeholder = '', rows = 0 } = {}) {
  const id = `f_${name}`;
  let input;
  if (options) input = `<select id="${id}" name="${e(name)}">${options.map(o => { const [v, l] = Array.isArray(o) ? o : [o, o]; return `<option value="${e(v)}" ${String(v) === String(value) ? 'selected' : ''}>${e(l)}</option>`; }).join('')}</select>`;
  else if (type === 'textarea') input = `<textarea id="${id}" name="${e(name)}" ${rows ? `rows="${rows}"` : ''} placeholder="${e(placeholder)}" ${required ? 'required' : ''}>${e(value)}</textarea>`;
  else input = `<input id="${id}" type="${type}" name="${e(name)}" value="${e(value)}" placeholder="${e(placeholder)}" ${required ? 'required' : ''}>`;
  return `<div><label for="${id}">${e(label)}</label>${input}</div>`;
}

export const form = (action, csrf, inner, { submit = 'Save', cls = 'box', method = 'post' } = {}) =>
  `<form method="${method}" action="${e(action)}" class="${cls}"><input type="hidden" name="_csrf" value="${e(csrf)}">${inner}<div style="margin-top:.8rem"><button>${e(submit)}</button></div></form>`;

export const button = (action, csrf, label, { quiet = false, confirm = '' } = {}) =>
  `<form method="post" action="${e(action)}" class="inline"><input type="hidden" name="_csrf" value="${e(csrf)}"><button class="${quiet ? 'quiet' : ''}">${e(label)}</button></form>`;

export const cards = (items) => `<div class="cards">${items.map(([k, v, cls = '']) => `<div class="card"><div class="k">${e(k)}</div><div class="v ${cls}">${v}</div></div>`).join('')}</div>`;
