// Server-rendered HTML, one stylesheet, no client JavaScript. The admin is a
// set of plain pages and forms so it works from a phone, a text browser and
// an AI operator's fetch() alike.
import { escapeHtml } from './http.mjs';
const e = escapeHtml;

export const money = (cents, cur = 'usd') => {
  const n = (Number(cents) || 0) / 100;
  return new Intl.NumberFormat('en-US', { style: 'currency', currency: cur.toUpperCase() }).format(n);
};
// Times show in the operator's zone (OPERATOR_TZ); storage stays UTC ISO.
let TZ = 'UTC';
export function setTimezone(tz) { try { new Intl.DateTimeFormat('en-US', { timeZone: tz }); TZ = tz || 'UTC'; } catch { TZ = 'UTC'; } return TZ; }
export const timezone = () => TZ;
const parts = (d, tz = TZ) => Object.fromEntries(new Intl.DateTimeFormat('en-US', { timeZone: tz, year: 'numeric', month: '2-digit', day: '2-digit', hour: '2-digit', minute: '2-digit', hour12: false, timeZoneName: 'short' }).formatToParts(d).map(p => [p.type, p.value]));
export const dt = (iso) => { if (!iso) return '—'; const d = new Date(iso); if (Number.isNaN(d.getTime())) return String(iso); const p = parts(d); return `${p.year}-${p.month}-${p.day} ${p.hour === '24' ? '00' : p.hour}:${p.minute} ${p.timeZoneName}`; };
export const day = (iso) => { if (!iso) return '—'; const d = new Date(iso); if (Number.isNaN(d.getTime())) return String(iso).slice(0, 10); const p = parts(d); return `${p.year}-${p.month}-${p.day}`; };
// A wall-clock time typed in the operator's zone → the UTC instant.
export function zonedToUtc(local, tz = TZ) {
  const guess = Date.parse(local.endsWith('Z') ? local : local + 'Z'); if (Number.isNaN(guess)) return null;
  const offset = (t) => { const p = parts(new Date(t), tz); return Date.UTC(+p.year, +p.month - 1, +p.day, p.hour === '24' ? 0 : +p.hour, +p.minute) - t; };
  let t = guess - offset(guess); t = guess - offset(t); return new Date(t).toISOString();
}
export const hourIn = (tz = TZ, d = new Date()) => { const h = parts(d, tz).hour; return h === '24' ? 0 : Number(h); };

const CSS = `
:root{--bg:#f6f5f1;--fg:#1b1b1b;--muted:#6b6b6b;--line:#e3dfd6;--accent:#b4820b;--accent-ink:#1a1300;--ok:#2d7d46;--bad:#b23a3a;--warn:#b7791f;--card:#fff;--side:#fcfbf8;--shadow:0 1px 2px rgba(20,16,0,.04),0 4px 14px -6px rgba(20,16,0,.08);--s1:#b4820b;--s2:#2f6fb3;--s3:#2d7d46;--s4:#8a4fb0;--s5:#b23a3a}
@media(prefers-color-scheme:dark){:root{--bg:#131313;--fg:#ececec;--muted:#9d9d9d;--line:#2a2a2a;--accent:#e2b23c;--card:#1c1c1c;--side:#171717;--shadow:0 1px 2px rgba(0,0,0,.4),0 6px 18px -8px rgba(0,0,0,.6);--s1:#b07e18;--s2:#4a8ad6;--s3:#3f9b5c;--s4:#a070c8;--s5:#d05a5a}}
*{box-sizing:border-box}html{scroll-behavior:smooth}body{margin:0;font:15px/1.5 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif;background:var(--bg);color:var(--fg);-webkit-font-smoothing:antialiased}
a{color:var(--accent)}h1{font-size:1.55rem;letter-spacing:-.01em;margin:.2rem 0 1rem;font-weight:650}h2{font-size:1.02rem;margin:1.6rem 0 .55rem;font-weight:650}
/* the frame: a sidebar on wide screens, a top strip on narrow ones */
.frame{display:grid;grid-template-columns:1fr;min-height:100vh}@media(min-width:960px){.frame{grid-template-columns:236px 1fr}}
aside{background:var(--side);border-right:1px solid var(--line);padding:.9rem .8rem;position:sticky;top:0;align-self:start;max-height:100vh;overflow:auto}
@media(max-width:959px){aside{position:static;max-height:none;border-right:0;border-bottom:1px solid var(--line);padding:.6rem .8rem}}
.brand{display:flex;align-items:center;gap:.5rem;padding:.2rem .5rem .7rem;font-weight:700;letter-spacing:-.01em}.brand .pill{font-weight:600}
aside form.search{margin:0 0 .6rem}aside form.search input{width:100%;padding:.45rem .6rem;border-radius:8px;background:var(--card)}
nav .g{font-size:11px;letter-spacing:.08em;text-transform:uppercase;color:var(--muted);padding:.8rem .6rem .25rem}
nav a{display:block;padding:.34rem .6rem;border-radius:7px;text-decoration:none;color:var(--fg);font-size:14px}nav a:hover{background:rgba(180,130,11,.09)}nav a.on{background:var(--accent);color:var(--accent-ink);font-weight:600}
@media(max-width:959px){nav{display:flex;overflow-x:auto;gap:.1rem;padding-bottom:.3rem;scrollbar-width:thin}nav .g{display:none}nav a{white-space:nowrap}}
aside .foot{margin-top:.9rem;padding:.7rem .6rem 0;border-top:1px solid var(--line);display:flex;gap:.6rem;align-items:center;justify-content:space-between;font-size:12px;color:var(--muted)}
main{max-width:1180px;width:100%;margin:0 auto;padding:1.2rem 1.4rem 3rem}@media(max-width:959px){main{padding:1rem}}
table{border-collapse:collapse;width:100%;background:var(--card);font-size:14px;border-radius:10px;overflow:hidden;box-shadow:var(--shadow)}th,td{text-align:left;padding:.5rem .7rem;border-bottom:1px solid var(--line);vertical-align:top}th{color:var(--muted);font-weight:600;font-size:12.5px;letter-spacing:.02em;background:rgba(0,0,0,.015)}
tbody tr:last-child td{border-bottom:0}tr:hover td{background:rgba(180,130,11,.05)}.num{text-align:right;font-variant-numeric:tabular-nums}
.cards{display:grid;grid-template-columns:repeat(auto-fit,minmax(170px,1fr));gap:.8rem;margin:1rem 0}.card{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:.9rem 1rem;box-shadow:var(--shadow)}
.card .k{color:var(--muted);font-size:11.5px;text-transform:uppercase;letter-spacing:.06em}.card .v{font-size:1.5rem;font-weight:650;letter-spacing:-.01em;margin-top:.1rem}
.pill{display:inline-block;padding:.05rem .55rem;border-radius:999px;font-size:12px;border:1px solid var(--line);background:var(--card);white-space:nowrap}.pill.ok{border-color:rgba(45,125,70,.35);color:var(--ok);background:rgba(45,125,70,.08)}.pill.bad{border-color:rgba(178,58,58,.35);color:var(--bad);background:rgba(178,58,58,.08)}.pill.warn{border-color:rgba(183,121,31,.4);color:var(--warn);background:rgba(183,121,31,.1)}
.ok{color:var(--ok)}.bad{color:var(--bad)}.warn{color:var(--warn)}.muted{color:var(--muted)}
form.inline{display:inline}form.box{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:1rem 1.1rem;margin:1rem 0;max-width:760px;box-shadow:var(--shadow)}
label{display:block;margin:.5rem 0 .2rem;font-size:12.5px;color:var(--muted)}input,select,textarea{width:100%;padding:.5rem .6rem;border:1px solid var(--line);border-radius:8px;background:var(--bg);color:var(--fg);font:inherit}input:focus,select:focus,textarea:focus{outline:2px solid rgba(180,130,11,.35);outline-offset:1px;border-color:var(--accent)}
textarea{min-height:8rem}button,.btn{padding:.5rem 1rem;border-radius:8px;border:1px solid var(--accent);background:var(--accent);color:var(--accent-ink);font:inherit;font-weight:600;cursor:pointer;text-decoration:none;display:inline-block}button:hover,.btn:hover{filter:brightness(1.05)}
button.quiet,.btn.quiet{background:var(--card);color:var(--fg);border-color:var(--line);font-weight:500}button.big{font-size:1.05rem;padding:.7rem 1.3rem}.row{display:flex;gap:.8rem;flex-wrap:wrap}.row>*{flex:1;min-width:160px}
.flash{padding:.6rem .9rem;border-radius:8px;margin:.6rem 0;background:rgba(45,125,70,.1);border:1px solid rgba(45,125,70,.4)}.flash.err{background:rgba(178,58,58,.1);border-color:var(--bad)}
pre{background:var(--card);border:1px solid var(--line);padding:.8rem;overflow:auto;border-radius:8px;font-size:13px}.msg{border:1px solid var(--line);border-radius:10px;padding:.7rem .9rem;margin:.6rem 0;background:var(--card);white-space:pre-wrap;box-shadow:var(--shadow)}
.msg.out{border-left:4px solid var(--accent)}.msg.note{border-left:4px solid var(--muted);opacity:.9}.msg .meta{font-size:12px;color:var(--muted);margin-bottom:.3rem}
footer{color:var(--muted);font-size:12px;padding:2rem 1rem;text-align:center}
.demo{background:repeating-linear-gradient(135deg,#f6d66b,#f6d66b 12px,#f0c84a 12px,#f0c84a 24px);color:#2a2000;text-align:center;font-size:13px;font-weight:600;padding:.35rem 1rem}.demo a{color:#2a2000}
.scen{display:grid;grid-template-columns:repeat(auto-fit,minmax(300px,1fr));gap:.8rem}.scen form{margin:0}.scen button{width:100%;text-align:left}
.tabs{margin:.4rem 0 .6rem}.tabs a{display:inline-block;padding:.2rem .7rem;border-radius:999px;border:1px solid var(--line);margin:0 .3rem .4rem 0;text-decoration:none;color:var(--fg);font-size:13px;background:var(--card)}.tabs a.on{background:var(--accent);color:var(--accent-ink);border-color:var(--accent)}.tabs a b{font-weight:600}
.tag{display:inline-block;padding:0 .45rem;border-radius:4px;background:rgba(180,130,11,.14);font-size:12px;margin:0 .3rem .2rem 0;white-space:nowrap}
.tl{list-style:none;padding:0 0 0 .2rem;margin:.4rem 0 1rem;border-left:2px solid var(--line)}.tl li{position:relative;padding:.25rem 0 .35rem 1rem}.tl li::before{content:"";position:absolute;left:-7px;top:.72rem;width:10px;height:10px;border-radius:50%;background:var(--accent);border:2px solid var(--card)}.tl li.note::before{background:var(--muted)}.tl .meta{font-size:12px;color:var(--muted)}
.card .d{font-size:12px;margin-top:.2rem}.up{color:var(--ok)}.down{color:var(--bad)}
.viz .k{margin-bottom:.4rem}.chart{width:100%;height:auto;display:block;overflow:visible}.chart .grid{stroke:var(--line);stroke-width:1}.chart .axis{stroke:var(--muted);stroke-width:1}.chart .ax{fill:var(--muted);font-size:11px}
.chart .col .tip,.chart .col .xh,.chart .col .pt{opacity:0;pointer-events:none;transition:opacity .08s}.chart .col:hover .tip,.chart .col:hover .xh,.chart .col:hover .pt{opacity:1}.chart .col:hover .bar{opacity:.8}.chart .xh{stroke:var(--muted);stroke-dasharray:3 3}
.chart .tip rect{fill:var(--fg);opacity:.92}.chart .tip .t1{fill:var(--bg);font-size:11px}.chart .tip .t2{fill:var(--bg);font-size:12px;font-weight:600}
.viztable{margin-top:.4rem;font-size:12px}.viztable summary{cursor:pointer;color:var(--muted)}.viztable table{max-height:200px;display:block;overflow:auto;box-shadow:none}
.hb{display:grid;grid-template-columns:minmax(120px,28%) 1fr minmax(90px,auto);gap:.6rem;align-items:center;padding:.2rem 0;font-size:13px}.hl{white-space:nowrap;overflow:hidden;text-overflow:ellipsis}.ht{background:var(--bg);border-radius:3px;height:12px;overflow:hidden}.hf{height:100%;border-radius:3px}.hv{text-align:right;font-variant-numeric:tabular-nums;white-space:nowrap}
.spark{vertical-align:middle}.two{display:grid;grid-template-columns:repeat(auto-fit,minmax(320px,1fr));gap:.9rem;align-items:start}.todo li{margin:.2rem 0}
/* home */
.hero{display:flex;justify-content:space-between;align-items:flex-end;gap:1rem;flex-wrap:wrap;margin:.2rem 0 1rem}.hero h1{margin:0}.hero .lead{color:var(--muted);margin:.15rem 0 0;font-size:15px}
.hp{display:inline-flex;align-items:center;gap:.45rem;padding:.4rem .8rem;border-radius:999px;font-size:13px;font-weight:600;text-decoration:none;border:1px solid var(--line);background:var(--card);color:var(--fg);box-shadow:var(--shadow)}.hp::before{content:"";width:9px;height:9px;border-radius:50%;background:var(--ok);box-shadow:0 0 0 3px rgba(45,125,70,.18)}.hp.bad::before{background:var(--bad);box-shadow:0 0 0 3px rgba(178,58,58,.18)}.hp.warn::before{background:var(--warn);box-shadow:0 0 0 3px rgba(183,121,31,.2)}
.actions{display:grid;grid-template-columns:repeat(auto-fit,minmax(230px,1fr));gap:.8rem;margin:0 0 1.2rem}.act{display:flex;flex-direction:column;gap:.3rem;background:var(--card);border:1px solid var(--line);border-radius:12px;padding:.9rem 1rem;box-shadow:var(--shadow);text-decoration:none;color:var(--fg);transition:transform .08s,box-shadow .08s}.act:hover{transform:translateY(-1px);box-shadow:0 8px 22px -10px rgba(20,16,0,.25)}
.act .n{font-size:1.7rem;font-weight:700;letter-spacing:-.02em;line-height:1.1}.act .w{font-size:14px}.act .go{margin-top:auto;font-size:13px;font-weight:600;color:var(--accent)}.act.urgent{border-color:rgba(178,58,58,.45)}.act.urgent .n{color:var(--bad)}.act.calm{border-style:dashed;color:var(--muted)}
.kpis{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:.8rem;margin:0 0 .9rem}
details.more{margin:1rem 0}details.more summary{cursor:pointer;color:var(--muted);font-size:13px}details.more[open] summary{margin-bottom:.4rem}
.feed{list-style:none;margin:0;padding:0;background:var(--card);border:1px solid var(--line);border-radius:12px;box-shadow:var(--shadow)}.feed li{display:grid;grid-template-columns:150px 1fr;gap:.8rem;padding:.5rem .9rem;border-bottom:1px solid var(--line);font-size:14px}.feed li:last-child{border-bottom:0}.feed .meta{color:var(--muted);font-size:12.5px;white-space:nowrap}@media(max-width:600px){.feed li{grid-template-columns:1fr}}
/* the Decide queue */
.decide{display:grid;grid-template-columns:repeat(auto-fit,minmax(280px,1fr));gap:.8rem;margin:0 0 1.2rem}.dc{display:flex;flex-direction:column;gap:.25rem;background:var(--card);border:1px solid var(--line);border-left:4px solid var(--accent);border-radius:12px;padding:.85rem 1rem;box-shadow:var(--shadow)}
.dc .dk{font-size:11px;letter-spacing:.08em;text-transform:uppercase;color:var(--muted)}.dc .dt{font-weight:650;font-size:15px}.dc .dd{font-size:13.5px}.dc .dw{font-size:12.5px;color:var(--muted);font-style:italic}.dc .da{margin-top:auto;padding-top:.5rem;display:flex;gap:.5rem;align-items:center}.dc .da button,.dc .da .btn{padding:.35rem .8rem;font-size:13px}
.where{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:.55rem .9rem;font-size:14px}
.atts{display:flex;flex-wrap:wrap;gap:.6rem;margin-top:.5rem}.att{display:inline-flex;flex-direction:column;gap:.2rem;font-size:12px;color:var(--fg);text-decoration:none}.att img{max-width:220px;max-height:160px;border-radius:8px;border:1px solid var(--line);display:block}.att.file{padding:.3rem .6rem;border:1px solid var(--line);border-radius:8px;background:var(--bg)}
/* order status stepper */
.steps{display:flex;list-style:none;padding:0;margin:.6rem 0 1rem;gap:0;counter-reset:s;max-width:720px}.steps li{flex:1;position:relative;text-align:center;font-size:13px;color:var(--muted);padding-top:1.4rem}.steps li::before{counter-increment:s;content:counter(s);position:absolute;left:50%;top:0;transform:translateX(-50%);width:22px;height:22px;border-radius:50%;background:var(--card);border:2px solid var(--line);font-size:11px;line-height:18px;color:var(--muted);z-index:1}
.steps li::after{content:"";position:absolute;top:10px;left:50%;width:100%;height:2px;background:var(--line)}.steps li:last-child::after{display:none}.steps li.done{color:var(--fg)}.steps li.done::before{background:var(--ok);border-color:var(--ok);color:#fff;content:"✓"}.steps li.done::after{background:var(--ok)}.steps li.now{color:var(--fg);font-weight:650}.steps li.now::before{border-color:var(--accent);background:var(--accent);color:var(--accent-ink)}.steps li .when{display:block;font-size:11px;color:var(--muted);font-weight:400}
/* the benches */
.bench{display:grid;grid-template-columns:minmax(0,1.2fr) minmax(280px,.8fr);gap:1rem;align-items:start}@media(max-width:800px){.bench{grid-template-columns:1fr}}
.bench .card{padding:1.1rem 1.2rem}.progress{height:8px;background:var(--line);border-radius:999px;overflow:hidden;margin:.4rem 0 1rem}.progress i{display:block;height:100%;background:var(--ok);border-radius:999px}
.picklist{list-style:none;padding:0;margin:.4rem 0}.picklist li{display:flex;gap:.7rem;align-items:center;padding:.5rem 0;border-bottom:1px solid var(--line);font-size:15px}.picklist li:last-child{border-bottom:0}.picklist .box{width:22px;height:22px;border:2px solid var(--line);border-radius:5px;flex:none}.picklist .q{font-weight:700;min-width:2.2rem}.picklist .sku{color:var(--muted);font-size:13px}
.addr{font-size:1.05rem;line-height:1.45}.bench .big{width:100%;text-align:center;margin-top:.8rem}
/* health */
.checks{list-style:none;padding:0;margin:.6rem 0;background:var(--card);border:1px solid var(--line);border-radius:12px;box-shadow:var(--shadow)}.checks li{display:grid;grid-template-columns:26px 220px 1fr;gap:.7rem;padding:.55rem .9rem;border-bottom:1px solid var(--line);align-items:baseline;font-size:14px}.checks li:last-child{border-bottom:0}.checks .ic{font-weight:700}.checks .fix{color:var(--muted);font-size:12.5px}@media(max-width:700px){.checks li{grid-template-columns:26px 1fr}}
.slip{background:#fff;color:#000;max-width:720px;margin:0 auto 1rem;padding:1.2rem;border:1px solid #ccc;page-break-after:always}.slip table,.slip th,.slip td{color:#000;background:#fff;border-color:#ccc;box-shadow:none}.slip h2{margin:.6rem 0 .3rem}.slip .box{border:1px dashed #999;padding:.5rem;margin:.6rem 0;min-height:3rem}
@media print{aside,footer,.demo,.noprint,main>h1{display:none!important}body{background:#fff}.frame{display:block}main{max-width:none;padding:0}.slip{border:0;margin:0}}
`;

export const NAV_GROUPS = [
  ['', [['/', 'Home'], ['/pack', 'Pack bench'], ['/ship', 'Ship bench']]],
  ['Sell', [['/orders', 'Orders'], ['/products', 'Products'], ['/contacts', 'Customers'], ['/discounts', 'Discounts'], ['/marketing', 'Marketing'], ['/analytics', 'Analytics']]],
  ['Run', [['/tickets', 'Support'], ['/inventory', 'Inventory'], ['/purchasing', 'Purchasing'], ['/devices', 'Devices'], ['/warranty', 'Warranty'], ['/invoices', 'Invoices'], ['/campaigns', 'Mail']]],
  ['Trust', [['/books', 'Books'], ['/status-admin', 'Status'], ['/exceptions', 'Exceptions'], ['/health', 'Health'], ['/audit', 'Audit log'], ['/settings', 'Settings']]],
];
export const NAV = NAV_GROUPS.flatMap(([, items]) => items);

export function layout({ title, body, path = '/', flash = null, company = 'Errer Labs', nav = true, csrf = '', demo = false, demoResetMinutes = 0, q = '', h1 = true }) {
  const groups = demo ? [...NAV_GROUPS, ['Demo', [['/demo', 'Scenarios']]]] : NAV_GROUPS;
  const on = (p) => path === p || (p !== '/' && path.startsWith(p));
  const navHtml = nav ? `<nav aria-label="Sections">${groups.map(([g, items]) => `${g ? `<div class="g">${e(g)}</div>` : ''}${items.map(([p, l]) => `<a href="${p}" class="${on(p) ? 'on' : ''}">${l}</a>`).join('')}`).join('')}</nav>` : '';
  const flashHtml = flash ? `<div class="flash ${flash.kind === 'err' ? 'err' : ''}">${escapeHtml(flash.text)}</div>` : '';
  const side = nav ? `<aside><div class="brand">${e(company)}${demo ? ' <span class="pill warn">demo</span>' : ''}</div><form method="get" action="/search" class="search" role="search"><input type="search" name="q" value="${e(q)}" placeholder="Search anything…" aria-label="Search"></form>${navHtml}<div class="foot"><span>one SQLite file</span><form method="post" action="/logout" class="inline"><input type="hidden" name="_csrf" value="${e(csrf)}"><button class="quiet" style="padding:.25rem .6rem;font-size:12px">Log out</button></form></div></aside>` : '';
  return `<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><meta name="robots" content="noindex"><title>${e(title)} · ${e(company)} backoffice</title><style>${CSS}</style></head>
<body>${demo ? '<div class="demo">DEMO MODE — every name, order and dollar here is synthetic; nothing is sent, charged, or reported anywhere. <a href="/demo">Scenarios</a></div>' : ''}<div class="frame">${side}<div><main>${flashHtml}${h1 ? `<h1>${e(title)}</h1>` : ''}${body}</main>${nav ? '' : `<footer>backoffice · one SQLite file, no SaaS · <a href="/api">API</a> · <a href="/status">public status</a></footer>`}</div></div></body></html>`;
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
