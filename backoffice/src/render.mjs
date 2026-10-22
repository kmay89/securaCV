// Server-rendered HTML, one stylesheet, no client JavaScript. The admin is a
// set of plain pages and forms so it works from a phone, a text browser and
// an AI operator's fetch() alike.
import { escapeHtml } from './http.mjs';
const e = escapeHtml;

export const money = (cents, cur = 'usd') => {
  const n = (Number(cents) || 0) / 100;
  return new Intl.NumberFormat('en-US', { style: 'currency', currency: cur.toUpperCase() }).format(n);
};
export const dt = (iso) => iso ? String(iso).replace('T', ' ').replace(/\.\d+Z$/, 'Z').replace(/:\d\dZ$/, ' UTC') : '—';
export const day = (iso) => iso ? String(iso).slice(0, 10) : '—';

const CSS = `
:root{--bg:#fbfaf7;--fg:#1b1b1b;--muted:#6b6b6b;--line:#e3dfd6;--accent:#b4820b;--ok:#2d7d46;--bad:#b23a3a;--warn:#b7791f;--card:#fff;--s1:#b4820b;--s2:#2f6fb3;--s3:#2d7d46;--s4:#8a4fb0;--s5:#b23a3a}
@media(prefers-color-scheme:dark){:root{--bg:#141414;--fg:#ececec;--muted:#a0a0a0;--line:#2c2c2c;--accent:#e2b23c;--card:#1d1d1d;--s1:#b07e18;--s2:#4a8ad6;--s3:#3f9b5c;--s4:#a070c8;--s5:#d05a5a}}
*{box-sizing:border-box}body{margin:0;font:15px/1.5 system-ui,-apple-system,Segoe UI,sans-serif;background:var(--bg);color:var(--fg)}
a{color:var(--accent)}header{padding:.5rem 1rem 0;border-bottom:1px solid var(--line);background:var(--card)}header .top{display:flex;gap:.8rem;align-items:center;flex-wrap:wrap}
header b{margin-right:.2rem}header nav{display:flex;flex-wrap:wrap;gap:0 .9rem;padding:.4rem 0 .5rem}header nav a{text-decoration:none;color:var(--fg);white-space:nowrap}header nav a.on{color:var(--accent);font-weight:600}
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
.demo{background:repeating-linear-gradient(135deg,#f6d66b,#f6d66b 12px,#f0c84a 12px,#f0c84a 24px);color:#2a2000;text-align:center;font-size:13px;font-weight:600;padding:.35rem 1rem}.demo a{color:#2a2000}
.scen{display:grid;grid-template-columns:repeat(auto-fit,minmax(300px,1fr));gap:.8rem}.scen form{margin:0}.scen button{width:100%;text-align:left}
header form.search{margin-left:auto;display:flex;gap:.3rem}header form.search input{width:min(300px,60vw);padding:.3rem .5rem}
.tabs{margin:.4rem 0 .6rem}.tabs a{display:inline-block;padding:.2rem .7rem;border-radius:999px;border:1px solid var(--line);margin:0 .3rem .4rem 0;text-decoration:none;color:var(--fg);font-size:13px;background:var(--card)}.tabs a.on{background:var(--accent);color:#111;border-color:var(--accent)}.tabs a b{font-weight:600}
.tag{display:inline-block;padding:0 .45rem;border-radius:4px;background:rgba(180,130,11,.14);font-size:12px;margin:0 .3rem .2rem 0;white-space:nowrap}
.tl{list-style:none;padding:0 0 0 .2rem;margin:.4rem 0 1rem;border-left:2px solid var(--line)}.tl li{position:relative;padding:.25rem 0 .35rem 1rem}.tl li::before{content:"";position:absolute;left:-7px;top:.72rem;width:10px;height:10px;border-radius:50%;background:var(--accent);border:2px solid var(--card)}.tl li.note::before{background:var(--muted)}.tl .meta{font-size:12px;color:var(--muted)}
.card .d{font-size:12px;margin-top:.2rem}.up{color:var(--ok)}.down{color:var(--bad)}
.viz .k{margin-bottom:.4rem}.chart{width:100%;height:auto;display:block;overflow:visible}.chart .grid{stroke:var(--line);stroke-width:1}.chart .axis{stroke:var(--muted);stroke-width:1}.chart .ax{fill:var(--muted);font-size:11px}
.chart .col .tip,.chart .col .xh,.chart .col .pt{opacity:0;pointer-events:none;transition:opacity .08s}.chart .col:hover .tip,.chart .col:hover .xh,.chart .col:hover .pt{opacity:1}.chart .col:hover .bar{opacity:.8}.chart .xh{stroke:var(--muted);stroke-dasharray:3 3}
.chart .tip rect{fill:var(--fg);opacity:.92}.chart .tip .t1{fill:var(--bg);font-size:11px}.chart .tip .t2{fill:var(--bg);font-size:12px;font-weight:600}
.viztable{margin-top:.4rem;font-size:12px}.viztable summary{cursor:pointer;color:var(--muted)}.viztable table{max-height:200px;display:block;overflow:auto}
.hb{display:grid;grid-template-columns:minmax(120px,28%) 1fr minmax(90px,auto);gap:.6rem;align-items:center;padding:.2rem 0;font-size:13px}.hl{white-space:nowrap;overflow:hidden;text-overflow:ellipsis}.ht{background:var(--bg);border-radius:3px;height:12px;overflow:hidden}.hf{height:100%;border-radius:3px}.hv{text-align:right;font-variant-numeric:tabular-nums;white-space:nowrap}
.spark{vertical-align:middle}.two{display:grid;grid-template-columns:repeat(auto-fit,minmax(320px,1fr));gap:.8rem;align-items:start}.todo li{margin:.2rem 0}
.slip{background:#fff;color:#000;max-width:720px;margin:0 auto 1rem;padding:1.2rem;border:1px solid #ccc;page-break-after:always}.slip table,.slip th,.slip td{color:#000;background:#fff;border-color:#ccc}.slip h2{margin:.6rem 0 .3rem}.slip .box{border:1px dashed #999;padding:.5rem;margin:.6rem 0;min-height:3rem}
@media print{header,footer,.demo,.noprint,main>h1{display:none!important}body{background:#fff}main{max-width:none;padding:0}.slip{border:0;margin:0}}
`;

export const NAV = [
  ['/', 'Home'], ['/orders', 'Orders'], ['/products', 'Products'], ['/contacts', 'Customers'], ['/discounts', 'Discounts'], ['/analytics', 'Analytics'],
  ['/tickets', 'Support'], ['/inventory', 'Inventory'], ['/devices', 'Devices'], ['/warranty', 'Warranty'], ['/invoices', 'Invoices'], ['/campaigns', 'Mail'], ['/books', 'Books'],
  ['/status-admin', 'Status'], ['/exceptions', 'Exceptions'], ['/settings', 'Settings'],
];

export function layout({ title, body, path = '/', flash = null, company = 'Errer Labs', nav = true, csrf = '', demo = false, demoResetMinutes = 0, q = '' }) {
  const items = demo ? [...NAV, ['/demo', 'Demo scenarios']] : NAV;
  const navHtml = nav ? `<nav>${items.map(([p, l]) => `<a href="${p}" class="${path === p || (p !== '/' && path.startsWith(p)) ? 'on' : ''}">${l}</a>`).join('')}</nav>` : '';
  const flashHtml = flash ? `<div class="flash ${flash.kind === 'err' ? 'err' : ''}">${escapeHtml(flash.text)}</div>` : '';
  const search = nav ? `<form method="get" action="/search" class="search" role="search"><input type="search" name="q" value="${e(q)}" placeholder="Search orders, customers, serials…" aria-label="Search"></form>` : '';
  return `<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><meta name="robots" content="noindex"><title>${e(title)} · ${e(company)} backoffice</title><style>${CSS}</style></head>
<body>${demo ? '<div class="demo">DEMO MODE — every name, order and dollar here is synthetic; nothing is sent, charged, or reported anywhere. <a href="/demo">Scenarios</a></div>' : ''}<header><div class="top"><b>${e(company)}</b>${demo ? ' <span class="pill warn">demo</span>' : ''}${search}${nav ? `<form method="post" action="/logout" class="inline"><input type="hidden" name="_csrf" value="${e(csrf)}"><button class="quiet">Log out</button></form>` : ''}</div>${navHtml}</header>
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

export const tags = (t) => String(t || '').split(',').filter(Boolean).map(x => `<span class="tag">${e(x)}</span>`).join('');
export const tabs = (items, current) => `<div class="tabs">${items.map(([href, label, n, key]) => `<a href="${e(href)}" class="${(key ?? label) === current ? 'on' : ''}">${e(label)}${n != null ? ` <b>${n}</b>` : ''}</a>`).join('')}</div>`;
export const cards = (items) => `<div class="cards">${items.map(([k, v, cls = '']) => `<div class="card"><div class="k">${e(k)}</div><div class="v ${cls}">${v}</div></div>`).join('')}</div>`;
