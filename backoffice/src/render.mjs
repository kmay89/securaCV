// Server-rendered HTML, one stylesheet, no client JavaScript. The admin is a
// set of plain pages and forms so it works from a phone, a text browser and
// an AI operator's fetch() alike.
import { escapeHtml, token, NAV_SCRIPT } from './http.mjs';
import { can } from './auth.mjs';
const e = escapeHtml;

export const money = (cents, cur = 'usd') => {
  const n = (Number(cents) || 0) / 100;
  // A currency code the formatter does not know (a bad import, a Stripe test value) never takes the page down: the amount shows with its code.
  try { return new Intl.NumberFormat('en-US', { style: 'currency', currency: String(cur || 'usd').toUpperCase() }).format(n); } catch { return `${n.toFixed(2)} ${String(cur || '').toUpperCase()}`; }
};
// Times show in the operator's zone (OPERATOR_TZ); storage stays UTC ISO.
let TZ = 'UTC';
export const currentTz = () => TZ;
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
:root{--bg:#f6f5f1;--fg:#27251f;--muted:#716d64;--line:#e7e3da;--line2:#efece5;--accent:#b4820b;--accent-ink:#1a1300;--link:#8a650a;--ok:#2f7a48;--bad:#b03a3a;--warn:#a8701a;--card:#fffefb;--side:#fbfaf6;--field:#fff;--shadow:0 1px 2px rgba(40,32,0,.03),0 6px 18px -12px rgba(40,32,0,.12);--s1:#b4820b;--s2:#2f6fb3;--s3:#2d7d46;--s4:#8a4fb0;--s5:#b23a3a;--tint:rgba(180,130,11,.09)}
@media(prefers-color-scheme:dark){:root{--bg:#151517;--fg:#e3e1dc;--muted:#9c9892;--line:#2a2a2e;--line2:#232326;--accent:#d2a230;--accent-ink:#1a1300;--link:#e1b85c;--ok:#5cb37a;--bad:#e07070;--warn:#d9a24a;--card:#1d1d20;--side:#19191c;--field:#141416;--shadow:0 1px 2px rgba(0,0,0,.35),0 8px 24px -14px rgba(0,0,0,.6);--s1:#c6952a;--s2:#5a95dd;--s3:#4fa96c;--s4:#ab7fd0;--s5:#d66868;--tint:rgba(210,162,48,.12)}}
*{box-sizing:border-box}html{scroll-behavior:smooth}body{margin:0;font:15px/1.55 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif;background:var(--bg);color:var(--fg);-webkit-font-smoothing:antialiased;text-rendering:optimizeLegibility}
a{color:var(--link);text-decoration-thickness:1px;text-underline-offset:2px}a:hover{color:var(--fg)}
h1{font-size:1.5rem;letter-spacing:-.015em;margin:.1rem 0 1.1rem;font-weight:650;line-height:1.25}h2{font-size:.98rem;margin:2rem 0 .65rem;font-weight:650;letter-spacing:-.005em}h2:first-child{margin-top:.4rem}p{margin:.55rem 0}code{font-size:.92em;background:var(--line2);padding:.05rem .35rem;border-radius:4px}pre code{background:none;padding:0}
/* the frame: a sidebar on wide screens, a top strip on narrow ones */
.frame{display:grid;grid-template-columns:1fr;min-height:100vh}.frame>div{min-width:0}@media(min-width:800px){.frame{grid-template-columns:196px 1fr}.frame.solo{grid-template-columns:1fr}}@media(min-width:1000px){.frame{grid-template-columns:232px 1fr}}.solo main{max-width:720px}
aside{background:var(--side);border-right:1px solid var(--line);padding:1rem .8rem;position:sticky;top:0;align-self:start;max-height:100vh;overflow:auto}
@media(max-width:799px){.frame{grid-template-rows:auto 1fr}aside{position:static;max-height:none;border-right:0;border-bottom:1px solid var(--line);padding:.5rem .8rem .2rem;display:grid;grid-template-columns:1fr auto;grid-template-areas:"brand foot" "search search" "nav nav";align-items:center;column-gap:.6rem}.frame aside .brand{grid-area:brand;padding:.1rem .2rem .3rem;white-space:nowrap}.frame aside form.search{grid-area:search;margin:0 0 .4rem}.frame aside nav{grid-area:nav}.frame aside .foot{grid-area:foot;margin:0;padding:0;border:0;min-width:0;white-space:nowrap;justify-content:flex-end}}
.brand{display:flex;align-items:center;gap:.5rem;padding:.2rem .6rem .8rem;font-weight:700;letter-spacing:-.01em;font-size:15px}.brand .pill{font-weight:600}
aside form.search{margin:0 0 .7rem}aside form.search input{width:100%;padding:.45rem .7rem;border-radius:9px;background:var(--field);font-size:13.5px}
nav .g{font-size:12px;letter-spacing:.1em;text-transform:uppercase;color:var(--muted);padding:.9rem .6rem .3rem;font-weight:600}
nav a{display:block;padding:.36rem .65rem;border-radius:8px;text-decoration:none;color:var(--fg);font-size:13.5px;line-height:1.4}nav a:hover{background:var(--line2);color:var(--fg)}nav a.on{background:var(--tint);color:var(--fg);font-weight:650;box-shadow:inset 3px 0 0 var(--accent)}
@media(max-width:799px){nav{display:flex;overflow-x:auto;gap:.15rem;margin:0 -.8rem;padding:0 .8rem .2rem;scrollbar-width:none;-webkit-overflow-scrolling:touch;scroll-padding:0 .8rem}nav::-webkit-scrollbar{display:none}nav .g{display:none}nav a{white-space:nowrap;display:inline-flex;align-items:center;min-height:44px;padding:.3rem .85rem;font-size:15px;scroll-snap-align:center}nav a.on{box-shadow:inset 0 -3px 0 var(--accent);border-radius:8px 8px 0 0}}
aside .foot{margin-top:1rem;padding:.8rem .6rem 0;border-top:1px solid var(--line);display:flex;gap:.6rem;align-items:center;justify-content:space-between;font-size:12px;color:var(--muted)}
main{max-width:1120px;width:100%;min-width:0;margin:0 auto;padding:1.5rem 2rem 4rem}@media(max-width:799px){main{padding:1rem 1rem calc(3rem + env(safe-area-inset-bottom))}}
table{border-collapse:separate;border-spacing:0;width:100%;background:var(--card);font-size:13.5px;border-radius:12px;border:1px solid var(--line);overflow:hidden}th,td{text-align:left;padding:.55rem .8rem;border-bottom:1px solid var(--line2);vertical-align:top}th{color:var(--muted);font-weight:600;font-size:12px;letter-spacing:.06em;text-transform:uppercase;background:var(--side)}
tbody tr:last-child td{border-bottom:0}tbody tr:hover td{background:var(--side)}.num{text-align:right;font-variant-numeric:tabular-nums}td{font-variant-numeric:tabular-nums}.nw{white-space:nowrap}.tw{overflow-x:auto;max-width:100%;border-radius:12px}.tw table{min-width:100%}
details.filters{margin:.4rem 0 .8rem}details.filters>summary{font-size:13px;color:var(--muted);cursor:pointer;padding:.2rem 0}details.filters[open]>summary{margin-bottom:.2rem}details.filters .row{margin:0 0 .4rem}
.cards{display:grid;grid-template-columns:repeat(auto-fit,minmax(min(168px,100%),1fr));gap:.75rem;margin:1rem 0}.card{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:.95rem 1.05rem;box-shadow:var(--shadow)}
.card .k{color:var(--muted);font-size:12px;text-transform:uppercase;letter-spacing:.08em;font-weight:600}.card .v{font-size:1.4rem;font-weight:650;letter-spacing:-.015em;margin-top:.15rem;line-height:1.2}
.pill{display:inline-block;padding:.08rem .55rem;border-radius:999px;font-size:12px;font-weight:500;border:1px solid var(--line);background:var(--side);white-space:nowrap;vertical-align:middle}.pill.ok{border-color:transparent;color:var(--ok);background:rgba(47,122,72,.1)}.pill.bad{border-color:transparent;color:var(--bad);background:rgba(176,58,58,.1)}.pill.warn{border-color:transparent;color:var(--warn);background:rgba(168,112,26,.12)}
.ok{color:var(--ok)}.bad{color:var(--bad)}.warn{color:var(--warn)}.muted{color:var(--muted)}
/* the fact strip under a detail page's title */
.meta{display:flex;flex-wrap:wrap;gap:.2rem 1.6rem;margin:-.3rem 0 1.1rem;padding:.7rem 1rem;background:var(--card);border:1px solid var(--line);border-radius:12px;font-size:13.5px}.meta .m{display:flex;flex-direction:column;gap:0;min-width:0}.meta .k{font-size:12px;text-transform:uppercase;letter-spacing:.08em;color:var(--muted);font-weight:600}.meta .v{line-height:1.45}
form.inline{display:inline}form.box{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:1.05rem 1.2rem 1.1rem;margin:.9rem 0 1.1rem;max-width:760px;box-shadow:var(--shadow)}form.box>b{display:block;margin-bottom:.1rem;font-size:14.5px}
label{display:block;margin:.6rem 0 .25rem;font-size:12.5px;color:var(--muted);font-weight:500}input,select,textarea{width:100%;padding:.5rem .65rem;border:1px solid var(--line);border-radius:9px;background:var(--field);color:var(--fg);font:inherit;font-size:14px}input:focus,select:focus,textarea:focus{outline:2px solid var(--tint);outline-offset:1px;border-color:var(--accent)}
textarea{min-height:6.5rem;line-height:1.5}button,.btn{padding:.48rem .95rem;border-radius:9px;border:1px solid var(--accent);background:var(--accent);color:var(--accent-ink);font:inherit;font-size:14px;font-weight:600;cursor:pointer;text-decoration:none;display:inline-block;line-height:1.35}button:hover,.btn:hover{filter:brightness(1.06)}
button.quiet,.btn.quiet{background:transparent;color:var(--fg);border-color:var(--line);font-weight:500}button.quiet:hover,.btn.quiet:hover{background:var(--side);filter:none}button.big{font-size:1.05rem;padding:.7rem 1.3rem}.row{display:flex;gap:.8rem;flex-wrap:wrap}.row>*{flex:1;min-width:min(160px,100%)}
.flash{padding:.6rem .95rem;border-radius:10px;margin:.4rem 0 .9rem;background:rgba(47,122,72,.09);border:1px solid rgba(47,122,72,.25);font-size:14px}.flash.err{background:rgba(176,58,58,.08);border-color:rgba(176,58,58,.3)}
pre{background:var(--card);border:1px solid var(--line);padding:.8rem .95rem;overflow:auto;border-radius:10px;font-size:12.5px;line-height:1.5}.msg{border:1px solid var(--line);border-radius:12px;padding:.75rem 1rem;margin:.65rem 0;background:var(--card);white-space:pre-wrap;box-shadow:var(--shadow);font-size:14.5px;line-height:1.55}
.msg.out{border-left:3px solid var(--accent)}.msg.note{border-left:3px solid var(--line);background:var(--side)}.msg .meta{display:block;font-size:12px;color:var(--muted);margin:0 0 .35rem;padding:0;border:0;background:none}
footer{color:var(--muted);font-size:12px;padding:2rem 1rem;text-align:center}
.demo{background:#f6e7b6;color:#4a3700;text-align:center;font-size:12.5px;font-weight:500;padding:.32rem 1rem;border-bottom:1px solid #e9d48f}.demo a{color:#4a3700}@media(prefers-color-scheme:dark){.demo{background:#3a2f0e;color:#f0dc9a;border-bottom-color:#5a4a1a}.demo a{color:#f0dc9a}}
.scen{display:grid;grid-template-columns:repeat(auto-fit,minmax(min(300px,100%),1fr));gap:.8rem}.scen form{margin:0}.scen button{width:100%;text-align:left}
.tabs{margin:.2rem 0 .8rem;display:flex;flex-wrap:wrap;gap:.3rem}.tabs a{display:inline-block;padding:.22rem .75rem;border-radius:999px;border:1px solid var(--line);text-decoration:none;color:var(--fg);font-size:13px;background:var(--card)}.tabs a:hover{background:var(--side)}.tabs a.on{background:var(--fg);color:var(--bg);border-color:var(--fg)}.tabs a b{font-weight:600;opacity:.75;margin-left:.2rem}
.tag{display:inline-block;padding:0 .45rem;border-radius:5px;background:var(--tint);font-size:12px;margin:0 .3rem .2rem 0;white-space:nowrap}
.tl{list-style:none;padding:0 0 0 .2rem;margin:.4rem 0 1rem;border-left:2px solid var(--line)}.tl li{position:relative;padding:.3rem 0 .4rem 1rem;font-size:14px}.tl li::before{content:"";position:absolute;left:-7px;top:.78rem;width:10px;height:10px;border-radius:50%;background:var(--accent);border:2px solid var(--card)}.tl li.note::before{background:var(--muted)}.tl .meta{display:inline;font-size:12px;color:var(--muted);padding:0;margin:0;border:0;background:none}
.card .d{font-size:12px;margin-top:.25rem}.up{color:var(--ok)}.down{color:var(--bad)}
.viz .k{margin-bottom:.45rem}.chart{width:100%;height:auto;display:block;overflow:visible}.chart .grid{stroke:var(--line);stroke-width:1}.chart .axis{stroke:var(--muted);stroke-width:1}.chart .ax{fill:var(--muted);font-size:12px}
.chart .col .tip,.chart .col .xh,.chart .col .pt{opacity:0;pointer-events:none;transition:opacity .08s}.chart .col:hover .tip,.chart .col:hover .xh,.chart .col:hover .pt{opacity:1}.chart .col:hover .bar{opacity:.8}.chart .xh{stroke:var(--muted);stroke-dasharray:3 3}
.chart .tip rect{fill:var(--fg);opacity:.92}.chart .tip .t1{fill:var(--bg);font-size:12px}.chart .tip .t2{fill:var(--bg);font-size:12px;font-weight:600}
.viztable{margin-top:.4rem;font-size:12px}.viztable summary{cursor:pointer;color:var(--muted)}.viztable table{max-height:200px;display:block;overflow:auto;box-shadow:none}
.hb{display:grid;grid-template-columns:minmax(120px,28%) 1fr minmax(90px,auto);gap:.6rem;align-items:center;padding:.22rem 0;font-size:13px}.hl{white-space:nowrap;overflow:hidden;text-overflow:ellipsis}.ht{background:var(--line2);border-radius:3px;height:10px;overflow:hidden}.hf{height:100%;border-radius:3px}.hv{text-align:right;font-variant-numeric:tabular-nums;white-space:nowrap}
.spark{vertical-align:middle}.two{display:grid;grid-template-columns:repeat(auto-fit,minmax(min(320px,100%),1fr));gap:1rem;align-items:start}.todo li{margin:.2rem 0}
/* home */
.hero{display:flex;justify-content:space-between;align-items:flex-end;gap:1rem;flex-wrap:wrap;margin:.2rem 0 1.1rem}.hero h1{margin:0}.hero .lead{color:var(--muted);margin:.2rem 0 0;font-size:14.5px}
.hp{display:inline-flex;align-items:center;gap:.45rem;padding:.4rem .85rem;border-radius:999px;font-size:13px;font-weight:600;text-decoration:none;border:1px solid var(--line);background:var(--card);color:var(--fg);box-shadow:var(--shadow)}.hp::before{content:"";width:8px;height:8px;border-radius:50%;background:var(--ok);box-shadow:0 0 0 3px rgba(47,122,72,.18)}.hp.bad::before{background:var(--bad);box-shadow:0 0 0 3px rgba(176,58,58,.18)}.hp.warn::before{background:var(--warn);box-shadow:0 0 0 3px rgba(168,112,26,.2)}
.actions{display:grid;grid-template-columns:repeat(auto-fit,minmax(min(230px,100%),1fr));gap:.75rem;margin:0 0 1.2rem}.act{display:flex;flex-direction:column;gap:.3rem;background:var(--card);border:1px solid var(--line);border-radius:14px;padding:.95rem 1.05rem;box-shadow:var(--shadow);text-decoration:none;color:var(--fg);transition:transform .08s,box-shadow .08s}.act:hover{transform:translateY(-1px);box-shadow:0 10px 24px -14px rgba(40,32,0,.3);color:var(--fg)}
.act .n{font-size:1.6rem;font-weight:700;letter-spacing:-.02em;line-height:1.1}.act .w{font-size:13.5px}.act .go{margin-top:auto;font-size:12.5px;font-weight:600;color:var(--link)}.act.urgent{border-color:rgba(176,58,58,.35)}.act.urgent .n{color:var(--bad)}.act.calm{border-style:dashed;color:var(--muted);box-shadow:none}
.kpis{display:grid;grid-template-columns:repeat(auto-fit,minmax(min(150px,100%),1fr));gap:.75rem;margin:0 0 1rem}
details.more{margin:1rem 0}details.more summary{cursor:pointer;color:var(--muted);font-size:13px}details.more[open] summary{margin-bottom:.4rem}details>summary{cursor:pointer}
.feed{list-style:none;margin:0;padding:0;background:var(--card);border:1px solid var(--line);border-radius:14px;box-shadow:var(--shadow)}.feed li{display:grid;grid-template-columns:160px 1fr;gap:.8rem;padding:.55rem 1rem;border-bottom:1px solid var(--line2);font-size:13.5px}.feed li:last-child{border-bottom:0}.feed .meta{display:block;color:var(--muted);font-size:12px;white-space:nowrap;padding:0;margin:0;border:0;background:none}@media(max-width:600px){.feed li{grid-template-columns:1fr}}
/* the Decide queue */
.decide{display:grid;grid-template-columns:repeat(auto-fit,minmax(min(280px,100%),1fr));gap:.75rem;margin:0 0 1.2rem}.dc{display:flex;flex-direction:column;gap:.25rem;background:var(--card);border:1px solid var(--line);border-left:3px solid var(--accent);border-radius:14px;padding:.9rem 1.05rem;box-shadow:var(--shadow)}
.dc .dk{font-size:12px;letter-spacing:.1em;text-transform:uppercase;color:var(--muted);font-weight:600}.dc .dt{font-weight:650;font-size:14.5px;line-height:1.35}.dc .dd{font-size:13.5px}.dc .dw{font-size:12.5px;color:var(--muted);font-style:italic}.dc .da{margin-top:auto;padding-top:.55rem;display:flex;gap:.5rem;align-items:center}.dc .da button,.dc .da .btn{padding:.33rem .8rem;font-size:13px}
.where{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:.6rem .95rem;font-size:14px}
.atts{display:flex;flex-wrap:wrap;gap:.6rem;margin-top:.5rem}.att{display:inline-flex;flex-direction:column;gap:.2rem;font-size:12px;color:var(--fg);text-decoration:none}.att img{max-width:220px;max-height:160px;border-radius:8px;border:1px solid var(--line);display:block}.att.file{padding:.3rem .6rem;border:1px solid var(--line);border-radius:8px;background:var(--side)}
/* order status stepper */
.steps{display:flex;list-style:none;padding:0;margin:.4rem 0 1.1rem;gap:0;counter-reset:s;max-width:720px}.steps li{flex:1;position:relative;text-align:center;font-size:12.5px;color:var(--muted);padding-top:1.45rem}.steps li::before{counter-increment:s;content:counter(s);position:absolute;left:50%;top:0;transform:translateX(-50%);width:22px;height:22px;border-radius:50%;background:var(--card);border:2px solid var(--line);font-size:12px;line-height:18px;color:var(--muted);z-index:1}
.steps li::after{content:"";position:absolute;top:10px;left:50%;width:100%;height:2px;background:var(--line)}.steps li:last-child::after{display:none}.steps li.done{color:var(--fg)}.steps li.done::before{background:var(--ok);border-color:var(--ok);color:#fff;content:"✓"}.steps li.done::after{background:var(--ok)}.steps li.now{color:var(--fg);font-weight:650}.steps li.now::before{border-color:var(--accent);background:var(--accent);color:var(--accent-ink)}.steps li .when{display:block;font-size:12px;color:var(--muted);font-weight:400}
/* the benches */
.bench{display:grid;grid-template-columns:minmax(0,1.2fr) minmax(280px,.8fr);gap:1rem;align-items:start}@media(max-width:800px){.bench{grid-template-columns:1fr}}
.bench .card{padding:1.1rem 1.2rem}.progress{height:7px;background:var(--line2);border-radius:999px;overflow:hidden;margin:.4rem 0 1rem}.progress i{display:block;height:100%;background:var(--ok);border-radius:999px}
.picklist{list-style:none;padding:0;margin:.4rem 0}.picklist li{display:flex;gap:.7rem;align-items:center;padding:.5rem 0;border-bottom:1px solid var(--line2);font-size:15px}.picklist li:last-child{border-bottom:0}.picklist .box{width:22px;height:22px;border:2px solid var(--line);border-radius:6px;flex:none}.picklist .q{font-weight:700;min-width:2.2rem}.picklist .sku{color:var(--muted);font-size:13px}
.addr{font-size:1.05rem;line-height:1.45}.bench .big{width:100%;text-align:center;margin-top:.8rem}.pick{display:flex;gap:.8rem;align-items:center;padding:.65rem .7rem;border:1px solid var(--line);border-radius:9px;margin:.4rem 0;cursor:pointer;font-size:15.5px;background:var(--card)}.pick input{width:24px;height:24px;margin:0;flex:none;accent-color:var(--ok)}.pick .q{font-weight:700;min-width:2.4rem;font-size:1.1rem}.pick .sku{color:var(--muted);font-size:13px}.pick:has(:checked){background:rgba(47,122,72,.09);border-color:var(--ok)}.pick:has(:checked) .q{color:var(--ok)}.gate:has(.pick input:not(:checked)):not(:has(.anyway:checked)) .big,.gate:has(.need:not(:checked)):not(:has(.anyway:checked)) .big{opacity:.4;pointer-events:none}.boxcard{display:flex;justify-content:space-between;align-items:center;gap:1rem;border:2px solid var(--fg);border-radius:10px;padding:.7rem 1rem;margin:.5rem 0}.boxcard .nm{font-weight:700;font-size:1.15rem}.boxcard .oz{font-size:2.1rem;font-weight:700;letter-spacing:-.02em;white-space:nowrap}.boxcard .oz small{font-size:1rem;font-weight:500;color:var(--muted)}.labels{display:grid;grid-template-columns:repeat(auto-fill,minmax(min(230px,100%),1fr));gap:.5rem}.label{display:flex;gap:.5rem;align-items:center;border:1px dashed var(--line);border-radius:6px;padding:.4rem;background:#fff;color:#000;break-inside:avoid}.label svg{flex:none}.label .t{font-size:13px;line-height:1.35}.label .muted{color:#555}.scan{max-width:520px;margin:0 auto}.scan h1{font-size:1.7rem;word-break:break-all}.scan .big{width:100%;font-size:1.15rem;padding:.9rem 1rem;margin:.8rem 0 .4rem}.slip .qr svg{display:block}@media print{.label{border-color:#bbb}.labels{gap:.25rem}}.walk{list-style:none;padding:0;margin:0}.wt{border:1px solid var(--line);border-radius:10px;padding:.8rem 1rem;margin:.5rem 0;background:var(--card)}.wt.next{border-color:var(--accent)}.wt.bad{border-color:var(--bad)}.wt.todo{opacity:.75}.wt-h{display:flex;gap:.6rem;align-items:center}.wt-h .n{display:inline-block;min-width:1.6rem;height:1.6rem;line-height:1.6rem;text-align:center;border-radius:999px;background:var(--side);font-weight:700;font-size:12px}.wt-r{margin-top:.5rem;border-top:1px solid var(--line2);padding-top:.5rem}.wt-r dl{display:grid;grid-template-columns:max-content 1fr;gap:.15rem .8rem;margin:0 0 .4rem;font-size:13.5px}.wt-r dt{color:var(--muted)}.wt-r dd{margin:0;word-break:break-word}.wt-checks{list-style:none;padding:0;margin:0;font-size:13.5px}.wt-checks li{margin:.1rem 0}.wt-checks li.ok{color:var(--ok)}.wt-checks li.bad{color:var(--bad)}.anyway-box{margin-top:.7rem}.anyway-box summary{cursor:pointer;font-size:13.5px}.pills form.inline{display:inline-flex;align-items:center;gap:.2rem;margin:0 .3rem .3rem 0}button.x{padding:.05rem .45rem;line-height:1.2;font-size:14px}
/* health */
.checks{list-style:none;padding:0;margin:.6rem 0;background:var(--card);border:1px solid var(--line);border-radius:14px;box-shadow:var(--shadow)}.checks li{display:grid;grid-template-columns:26px 220px 1fr;gap:.7rem;padding:.6rem 1rem;border-bottom:1px solid var(--line2);align-items:baseline;font-size:14px}.checks li:last-child{border-bottom:0}.checks .ic{font-weight:700}.checks .fix{color:var(--muted);font-size:12.5px}@media(max-width:700px){.checks li{grid-template-columns:26px 1fr}}
.slip{background:#fff;color:#000;max-width:720px;margin:0 auto 1rem;padding:1.2rem;border:1px solid #ccc;page-break-after:always}.slip table,.slip th,.slip td{color:#000;background:#fff;border-color:#ccc;box-shadow:none}.slip h2{margin:.6rem 0 .3rem}.slip .box{border:1px dashed #999;padding:.5rem;margin:.6rem 0;min-height:3rem}

/* touch: a finger, not a cursor — 44px targets, 16px fields (iOS zooms anything smaller), the page type a step larger */
@media(pointer:coarse),(max-width:799px){body{font-size:16px}input,select,textarea{font-size:16px;min-height:44px}input[type=checkbox],input[type=radio]{min-height:0;width:22px;height:22px}.pick input{width:26px;height:26px}button,.btn{min-height:44px;padding:.55rem 1.05rem;font-size:15px}button.x{min-height:32px;min-width:32px;padding:.1rem .5rem}aside .foot button.quiet{min-height:36px;padding:.3rem .7rem!important}.tabs a{min-height:40px;display:inline-flex;align-items:center;font-size:14px}summary{min-height:40px;display:flex;align-items:center}aside form.search input{font-size:16px}nav a{min-height:40px;display:flex;align-items:center}.hp{min-height:40px}}
/* tables of five or more columns fold into cards on a phone: the first named column leads, every other cell carries its own label */
@media(max-width:639px){.tw.fold{overflow:visible;border-radius:0}.fold table{display:block;width:100%;background:none;border:0;border-radius:0;box-shadow:none}.fold thead{display:none}.fold tbody{display:grid;gap:.6rem}.fold tr{display:grid;grid-template-columns:1fr 1fr;gap:.2rem .9rem;background:var(--card);border:1px solid var(--line);border-radius:12px;padding:.75rem .95rem .8rem;box-shadow:var(--shadow)}.fold td{display:block;border:0;padding:.05rem 0;white-space:normal;text-align:left;min-width:0;font-size:14px;overflow-wrap:anywhere}.fold td::before{content:attr(data-l);display:block;color:var(--muted);font-size:12px;text-transform:uppercase;letter-spacing:.06em;font-weight:600;line-height:1.3}.fold td.lead{grid-column:1/-1;font-size:16px;font-weight:600;padding-bottom:.15rem}.fold td.lead::before{display:none}.fold td.sel{grid-column:1/-1;padding:0 0 .2rem}.fold td.sel::before{display:none}.fold td:empty{display:none}.fold tbody tr:hover td{background:none}}
@media print{aside,footer,.demo,.noprint,main>h1{display:none!important}body{background:#fff}.frame{display:block}main{max-width:none;padding:0}.slip{border:0;margin:0}}
`;

export const NAV_GROUPS = [
  ['', [['/', 'Home'], ['/pack', 'Pack bench'], ['/ship', 'Ship bench']]],
  ['Sell', [['/orders', 'Orders'], ['/products', 'Products'], ['/contacts', 'Customers'], ['/discounts', 'Discounts'], ['/marketing', 'Marketing'], ['/analytics', 'Analytics']]],
  ['Run', [['/tickets', 'Support'], ['/inventory', 'Inventory'], ['/purchasing', 'Purchasing'], ['/devices', 'Devices'], ['/warranty', 'Warranty'], ['/invoices', 'Invoices'], ['/campaigns', 'Mail']]],
  ['Trust', [['/books', 'Books'], ['/status-admin', 'Status'], ['/exceptions', 'Exceptions'], ['/health', 'Health'], ['/audit', 'Audit log'], ['/users', 'People'], ['/settings', 'Settings']]],
];
export const NAV = NAV_GROUPS.flatMap(([, items]) => items);

export function layout({ title, body, path = '/', flash = null, company = 'Errer Labs', nav = true, csrf = '', demo = false, demoResetMinutes = 0, q = '', h1 = true, user = null }) {
  const groups = (demo ? [...NAV_GROUPS, ['Demo', [['/demo', 'Scenarios']]]] : NAV_GROUPS).map(([g, items]) => [g, user ? items.filter(([p]) => can(user.role, 'GET', p)) : items]).filter(([, items]) => items.length);
  const on = (p) => path === p || (p !== '/' && path.startsWith(p));
  const navHtml = nav ? `<nav aria-label="Sections">${groups.map(([g, items]) => `${g ? `<div class="g">${e(g)}</div>` : ''}${items.map(([p, l]) => `<a href="${p}" class="${on(p) ? 'on' : ''}">${l}</a>`).join('')}`).join('')}</nav>` : '';
  const flashHtml = flash ? `<div class="flash ${flash.kind === 'err' ? 'err' : ''}">${escapeHtml(flash.text)}</div>` : '';
  const side = nav ? `<aside><div class="brand">${e(company)}${demo ? ' <span class="pill warn">demo</span>' : ''}</div><form method="get" action="/search" class="search" role="search"><input type="search" name="q" value="${e(q)}" placeholder="Search anything…" aria-label="Search"></form>${navHtml}<div class="foot"><span>${user ? `<a href="/me" title="your account">${e(user.name)}</a> · ${e(user.role)}` : 'one SQLite file'}</span><form method="post" action="/logout" class="inline"><input type="hidden" name="_csrf" value="${e(csrf)}"><button class="quiet" style="padding:.25rem .6rem;font-size:12px">Log out</button></form></div></aside>` : '';
  return `<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover"><meta name="robots" content="noindex"><title>${e(title)} · ${e(company)} backoffice</title><style>${CSS}</style></head>
<body>${demo ? '<div class="demo">Demo — every name, order and dollar is synthetic; nothing is sent, charged or reported anywhere. <a href="/demo">Scenarios</a></div>' : ''}<div class="frame ${nav ? '' : 'solo'}">${side}<div><main>${flashHtml}${h1 ? `<h1>${e(title)}</h1>` : ''}${body}</main>${nav ? '' : `<footer>backoffice · one SQLite file, no SaaS · <a href="/api">API</a> · <a href="/status">public status</a></footer>`}</div></div>${nav ? `<script>${NAV_SCRIPT}</script>` : ''}</body></html>`;
}

// Short, identifying columns never wrap (a number, a date, a status); the table scrolls sideways instead of folding them.
const NOWRAP = /^(#|status|paid|when|total|batch|opened|updated|sla due|created|last login|expected|sent|issued|due|qty|δ|unit|line|price|flashed|renews|ms|90 d|every|received|ordered|date|month|amount|kind|pri|role|scope|from|to|last order|first order|since|tracking|serial|mac|sku|finished sku|code|number|on hand|reorder at|unit cost|on order|cogs\/unit|per unit|buildable now|uses|orders|units|spent|tickets|marketing|supplier|board|firmware|order|email|to|replies|revenue)$/i;
export function table(rows, cols, { empty = 'Nothing here.' } = {}) {
  if (!rows.length) return `<p class="muted">${e(empty)}</p>`;
  const lead = cols.findIndex(c => c.label);   // the first named column leads a folded card (a checkbox column has no name)
  const cls = (c, i) => [c.num ? 'num' : '', (c.nowrap ?? NOWRAP.test(c.label)) && !c.wrap ? 'nw' : '', i === lead ? 'lead' : '', c.label ? '' : 'sel'].filter(Boolean).join(' ');
  const head = cols.map((c, i) => `<th class="${cls(c, i)}">${e(c.label)}</th>`).join('');
  const body = rows.map(r => `<tr>${cols.map((c, i) => `<td class="${cls(c, i)}" data-l="${e(c.label)}">${c.html ? c.html(r) : e(c.get ? c.get(r) : r[c.key])}</td>`).join('')}</tr>`).join('');
  return `<div class="tw${cols.length >= 5 ? ' fold' : ''}"><table><thead><tr>${head}</tr></thead><tbody>${body}</tbody></table></div>`;
}

export const pill = (s, cls = '') => `<span class="pill ${cls}">${e(String(s ?? '').replace(/_/g, ' '))}</span>`;
export const statusPill = (s) => pill(s, /paid|open|active|ok|resolved|sent|shipped|delivered/.test(s) ? 'ok' : /fail|bad|past_due|overdue|urgent|declined/.test(s) ? 'bad' : /waiting|packed|queued|draft|monitoring/.test(s) ? 'warn' : '');

export function field(name, label, { type = 'text', value = '', required = false, options = null, placeholder = '', rows = 0, inputmode = null } = {}) {
  const id = `f_${name}`;
  let input;
  if (options) input = `<select id="${id}" name="${e(name)}">${options.map(o => { const [v, l] = Array.isArray(o) ? o : [o, o]; return `<option value="${e(v)}" ${String(v) === String(value) ? 'selected' : ''}>${e(l)}</option>`; }).join('')}</select>`;
  else if (type === 'textarea') input = `<textarea id="${id}" name="${e(name)}" ${rows ? `rows="${rows}"` : ''} placeholder="${e(placeholder)}" ${required ? 'required' : ''}>${e(value)}</textarea>`;
  else {
    // a phone opens the keyboard the field needs: digits for counts and money, a decimal pad for a scale reading,
    // no autocorrect or capitalization on serials, SKUs, tracking numbers and codes
    // (a "code" is letters as often as digits — a discount code, a canned-reply key — so it gets the identifier
    // treatment; a caller that knows better passes inputmode, as the login page's six-digit authenticator code does)
    const hint = inputmode ? `inputmode="${e(inputmode)}"` : type === 'number' ? 'inputmode="decimal"' : /^(cents|qty|delta|count|units|oz|zip|quantity|stock|cap|reorder_at|on_hand)$/.test(name) ? 'inputmode="numeric"' : type === 'email' ? 'inputmode="email" autocomplete="email" autocapitalize="none"' : /serial|sku|tracking|mac|token|slug|number|code|ref$/.test(name) ? 'autocapitalize="none" autocorrect="off" spellcheck="false"' : '';
    input = `<input id="${id}" type="${type}" name="${e(name)}" value="${e(value)}" placeholder="${e(placeholder)}" ${required ? 'required' : ''} ${hint}>`;
  }
  return `<div><label for="${id}">${e(label)}</label>${input}</div>`;
}

export const form = (action, csrf, inner, { submit = 'Save', cls = 'box', method = 'post' } = {}) =>
  `<form method="${method}" action="${e(action)}" class="${cls}"><input type="hidden" name="_csrf" value="${e(csrf)}">${method === 'post' ? once() : ''}${inner}<div style="margin-top:.8rem"><button>${e(submit)}</button></div></form>`;
// A submission token: the server runs a form once per token, so a double click or a repeated
// POST records one order, one refund, one stock move — not two.
export const once = () => `<input type="hidden" name="_once" value="${token(12)}">`;

export const button = (action, csrf, label, { quiet = false, confirm = '' } = {}) =>
  `<form method="post" action="${e(action)}" class="inline"><input type="hidden" name="_csrf" value="${e(csrf)}">${once()}<button class="${quiet ? 'quiet' : ''}">${e(label)}</button></form>`;

export const tags = (t) => String(t || '').split(',').filter(Boolean).map(x => `<span class="tag">${e(x)}</span>`).join('');
export const tabs = (items, current) => `<div class="tabs">${items.map(([href, label, n, key]) => `<a href="${e(href)}" class="${(key ?? label) === current ? 'on' : ''}">${e(label)}${n != null ? ` <b>${n}</b>` : ''}</a>`).join('')}</div>`;
// The fact strip under a detail page's title: labeled facts, wrapped, no dots to parse.
export const meta = (items) => `<div class="meta">${items.filter(i => i && i[1] != null && i[1] !== '').map(([k, v]) => `<div class="m"><span class="k">${e(k)}</span><span class="v">${v}</span></div>`).join('')}</div>`;
export const cards = (items) => `<div class="cards">${items.map(([k, v, cls = '']) => `<div class="card"><div class="k">${e(k)}</div><div class="v ${cls}">${v}</div></div>`).join('')}</div>`;
