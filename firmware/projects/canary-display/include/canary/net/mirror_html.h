// include/canary/net/mirror_html.h — the display's own web page, whole and
// self-contained (no CDN, no fonts, no internet: the LAN promise holds).
// Served by glass_web.cpp. Browser-rendered, so typography is free here —
// the LVGL glyph rules do not apply.
#pragma once
#include <pgmspace.h>

static const char MIRROR_HTML[] PROGMEM = R"RAW(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Your Canary Display</title>
<style>
:root{--bg:#0b0b0c;--card:#141414;--edge:#262626;--tx:#ededed;--mut:#8a8a8a;
--yel:#FFD44F;--ok:#43A047;--warn:#FB8C00;--bad:#E53935;--blu:#03A9F4}
*{box-sizing:border-box;margin:0}
body{background:var(--bg);color:var(--tx);font:15px/1.5 system-ui,sans-serif;
padding:16px;max-width:760px;margin:0 auto}
h1{font-size:20px;display:flex;align-items:center;gap:10px}
h2{font-size:15px;color:var(--mut);margin:22px 0 8px;text-transform:uppercase;
letter-spacing:.08em}
.dot{width:10px;height:10px;border-radius:50%;background:var(--bad)}
.dot.on{background:var(--ok)}
.sub{color:var(--mut);font-size:13px}
.stage{perspective:900px;display:flex;justify-content:center;padding:22px 0 8px;
touch-action:none}
.dev{position:relative;transform-style:preserve-3d;
transform:rotateX(-8deg) rotateY(14deg);transition:transform .08s linear}
.face{position:absolute;inset:0;backface-visibility:hidden;border-radius:inherit}
.glass{background:var(--bg);border:10px solid #1b1b1d;overflow:hidden;
box-shadow:0 22px 44px rgba(0,0,0,.55)}
.backp{background:#151517;border:10px solid #1b1b1d;transform:rotateY(180deg);
display:flex;align-items:center;justify-content:center;color:#3a3a3e;
font-size:12px;letter-spacing:.1em}
.dash .dev{width:330px;height:206px;border-radius:14px}
.watch .dev{width:230px;height:230px;border-radius:50%}
.mir{position:absolute;inset:0;padding:12px;display:flex;flex-direction:column;
transition:filter .5s}
.night .mir{filter:sepia(.7) hue-rotate(-28deg) saturate(2.4) brightness(.55)}
.mtop{display:flex;justify-content:space-between;font-size:11px;color:var(--mut)}
.mhero{flex:1;display:flex;flex-direction:column;align-items:center;
justify-content:center;gap:4px;text-align:center}
.mword{font-size:26px;font-weight:700}
.msub{font-size:11px;color:var(--mut)}
.mrow{display:flex;gap:5px;flex-wrap:wrap;justify-content:center}
.chip{font-size:9px;padding:2px 7px;border-radius:9px;border:1px solid var(--edge);
color:var(--mut)}
.chip.s2{border-color:var(--warn);color:var(--warn)}
.chip.s3,.chip.s4{border-color:var(--bad);color:var(--bad)}
.mfoot{font-size:9px;color:#4a4a4a;text-align:center}
.hint{text-align:center;color:var(--mut);font-size:12px;margin-top:6px}
/* the bird, in CSS — same silhouette, same brand yellow */
.bird{position:relative;width:52px;height:46px;margin:0 auto;
animation:breath 2.8s ease-in-out infinite}
.b{position:absolute;border-radius:50%}
.tail{left:0;top:19px;width:9px;height:6px;background:#E3B33C}
.body{left:5px;top:15px;width:32px;height:28px;background:var(--yel)}
.wing{left:10px;top:21px;width:16px;height:12px;background:#E3B33C}
.head{left:24px;top:5px;width:22px;height:22px;background:var(--yel)}
.beak{left:43px;top:12px;width:8px;height:5px;background:#F08C2E;
border-radius:2px}
.eye{left:34px;top:9px;width:6px;height:6px;background:#1a1a1a}
.shine{left:36px;top:10px;width:2px;height:2px;background:#fff}
.blush{left:30px;top:16px;width:7px;height:4px;background:#F2A38F;opacity:.8}
@keyframes breath{50%{transform:translateY(2px)}}
.bird.worried .eye,.bird.searching .eye,.bird.calling .eye{height:5px;top:10px}
.bird.worried .wing,.bird.calling .wing{top:19px}
.bird.asleep .eye{height:2px;top:12px;border-radius:1px}
.bird.asleep .shine{display:none}
.bird.asleep{animation-duration:5s}
.bird.hidden{display:none}
.bird.searching{transform:translateX(4px)}
.bird.calling .beak{height:8px}
.card{background:var(--card);border:1px solid var(--edge);border-radius:12px;
padding:14px;margin:10px 0}
label{display:flex;justify-content:space-between;align-items:center;gap:12px;
padding:7px 0;font-size:14px}
input[type=range]{flex:1;max-width:46%;accent-color:var(--yel)}
select,button{background:#1d1d1f;color:var(--tx);border:1px solid var(--edge);
border-radius:8px;padding:5px 10px;font-size:13px}
details{border-bottom:1px solid var(--edge);padding:9px 0}
summary{cursor:pointer;font-weight:600}
details p,details li{color:var(--mut);font-size:13.5px;margin-top:6px}
.moods td{padding:3px 10px 3px 0;font-size:13px;color:var(--mut)}
.moods td:first-child{color:var(--tx)}
.foot{color:#4a4a4a;font-size:12px;margin:18px 0 6px;text-align:center}
</style></head><body>
<h1><span class="dot" id="live"></span>Your Canary Display
  <span class="sub" id="clock" style="margin-left:auto"></span></h1>
<p class="sub">A live mirror of the glass on your wall. Spin it. Everything
here stays inside your home network.</p>

<div class="stage" id="stage"><div class="dev" id="dev">
  <div class="face glass"><div class="mir" id="mir">
    <div class="mtop"><span id="mwifi"></span><span id="mclock"></span></div>
    <div class="mhero">
      <div class="bird idle" id="bird"><i class="b tail"></i><i class="b body"></i>
        <i class="b wing"></i><i class="b head"></i><i class="b beak"></i>
        <i class="b eye"></i><i class="b shine"></i><i class="b blush"></i></div>
      <div class="mword" id="mword">…</div>
      <div class="msub" id="msub"></div>
      <div class="mrow" id="mrow"></div>
    </div>
    <div class="mfoot">status display &middot; not a life-safety device</div>
  </div></div>
  <div class="face backp">SECURACV &middot; CANARY DISPLAY</div>
</div></div>
<p class="hint">drag to turn the display over</p>

<h2>Master settings</h2>
<div class="card" id="set">
  <!-- TWO brightness rows, and exactly ONE of them is ever shown. This glass
       may dim its backlight (day_pct) or, where the backlight is a binary
       expander line, dim by drawing a scrim (bright_pct). Serving both would
       put a slider on this page that does nothing you can see — which is
       precisely what this page did on a 4.3"/7" panel until now. The settings
       fetch below picks the row the device actually reported. -->
  <label id="day_row">Day brightness <input type="range" id="day_pct" min="20" max="100"
    step="5"><span class="sub" id="day_pctv"></span></label>
  <label id="bright_row" style="display:none">Day brightness <input type="range"
    id="bright_pct" min="50" max="100" step="5"><span class="sub" id="bright_pctv"></span></label>
  <label>Night glow (level) <input type="range" id="night_step" min="1"
    max="10"><span class="sub" id="night_stepv"></span></label>
  <label>Night screen <select id="night_screen">
    <option value="0">glow</option>
    <option value="1" id="ns_off">off, tap peeks</option></select></label>
  <label>Warm night colors <select id="red_shift">
    <option value="1">on</option><option value="0">off</option></select></label>
  <label>Night peek length <select id="peek_s"><option>3</option>
    <option>5</option><option>10</option></select></label>
  <label>Night starts <select id="night_start_hh"></select></label>
  <label>Night ends <select id="night_end_hh"></select></label>
  <label>Time zone <select id="tz"></select></label>
  <p class="sub" id="tzsub"></p>
  <p class="sub">Changes apply on the glass right away, exactly as if you
  had set them there.</p>
</div>

<h2>About this device</h2>
<div class="card">
  <div id="receipt" style="display:grid;grid-template-columns:auto 1fr;
    gap:3px 16px;font-size:13.5px"></div>
  <p class="sub" style="margin-top:8px">It can: <span id="caps"></span></p>
</div>

<h2>Serial monitor</h2>
<div class="card">
  <label style="justify-content:flex-start"><input type="checkbox"
    id="logpause"> pause</label>
  <pre id="log" style="background:#000;border:1px solid var(--edge);
    border-radius:8px;padding:10px;font-size:11.5px;line-height:1.45;
    max-height:260px;overflow:auto;color:#9fdf9f"></pre>
  <p class="sub">The same lines the USB cable would show — no cable needed.</p>
</div>

<h2>The road ahead</h2>
<div class="card">
<p class="sub" style="margin-bottom:8px">Every canary you add teaches this
display something new. Open like a 3D printer, not walled like a garden —
anything that speaks the fleet's language can join.</p>
<ul style="color:var(--mut);font-size:13.5px;margin-left:18px">
<li><b style="color:var(--tx)">Find my things</b> — canaries already hear
Bluetooth tags and phones; soon this page will say "your keys are near the
Kitchen canary."</li>
<li><b style="color:var(--tx)">Room rhythms</b> — quiet trends per room,
so an unusual day is visible before it is a problem.</li>
<li><b style="color:var(--tx)">Time from the sky</b> — a canary with GPS
becomes the house's own atomic clock, no internet needed.</li>
<li><b style="color:var(--tx)">More senses</b> — radar, air, sound-shape:
new witnesses appear here the moment they join.</li>
</ul>
</div>

<h2>Help — reading your display</h2>
<div class="card">
<details open><summary>The one big word</summary>
<p>The center of the glass always shows the one thing that matters: the
time when all is quiet, or a status word when something needs you. Green
means proved and calm; orange means something is late; red means act.</p></details>
<details><summary>The little bird</summary>
<p>The canary is a gauge, not a decoration — every pose is honest:</p>
<table class="moods">
<tr><td>Calm, breathing</td><td>everything answered and proved</td></tr>
<tr><td>Worried</td><td>a connection is down or trouble is brewing</td></tr>
<tr><td>Leaning, looking out</td><td>a canary is late — it is looking for it</td></tr>
<tr><td>Beak open</td><td>a canary is lost — it is calling for it</td></tr>
<tr><td>Asleep</td><td>night; stillness is the point</td></tr>
<tr><td>Gone</td><td>a real alert owns the screen — the bird returns
when it is handled</td></tr></table></details>
<details><summary>Touch</summary>
<p>Tap: wake the glass / turn the page. Press and hold: acknowledge an
alert, or open settings from the settings page. At night a tap gives a
short dim peek instead of full brightness.</p></details>
<details><summary>Adding a canary</summary>
<p>Open "add a canary" on the glass — it shows a code. Point the new
canary's camera at it from about 15 cm. It joins your WiFi, finds this
display, and chirps when it is in.</p></details>
<details><summary>If the display shows "No WiFi"</summary>
<ul><li>Displays only see 2.4 GHz networks - a 5 GHz-only network is
invisible to them.</li>
<li>Moved or new router? Run setup again from the settings page on the
glass.</li>
<li>No WiFi is not a failure: any canaries already paired keep talking to
the display directly.</li></ul></details>
</div>
<p class="foot">Served by the display itself &middot; nothing leaves your
home &middot; SecuraCV</p>

<script>
var SEVW=["All quiet","Notice","Quiet too long","Alert","Tamper","Alert"];
var SEVC=["var(--ok)","var(--blu)","var(--warn)","var(--bad)","var(--bad)","var(--bad)"];
var MOOD=["hidden","idle","idle","worried","worried","asleep","searching","calling"];
function $(i){return document.getElementById(i)}
// 3D drag
var rx=-8,ry=14,drag=null,dev=$("dev");
function look(){dev.style.transform="rotateX("+rx+"deg) rotateY("+ry+"deg)"}
var st=$("stage");
st.addEventListener("pointerdown",function(e){drag=[e.clientX,e.clientY,ry,rx];
st.setPointerCapture(e.pointerId)});
st.addEventListener("pointermove",function(e){if(!drag)return;
ry=drag[2]+(e.clientX-drag[0])*.45;rx=Math.max(-70,Math.min(70,
drag[3]-(e.clientY-drag[1])*.45));look()});
st.addEventListener("pointerup",function(){drag=null});
// hour selects
["night_start_hh","night_end_hh"].forEach(function(id){var s=$(id);
for(var h=0;h<24;h++){var o=document.createElement("option");o.value=h;
o.textContent=(h%12||12)+(h<12?" am":" pm");s.appendChild(o)}});
// The per-boot CSRF token the device requires on state-changing POSTs. It
// arrives in /api/settings (same-origin only — that response carries no CORS
// header, so a cross-origin page cannot read it) and rides X-CSRF-Token on
// every write below. Empty until the first /api/settings load resolves, which
// is well before any settings control can be touched.
var CSRF="";
// Time zone: the POSIX rules carry their own DST transitions, so a zone
// picked once stays right across the spring and fall changes. Values match
// the table the auto-learner maps to (net/tz_auto.cpp) — keep them in step.
var TZS=[["New York (US Eastern)","EST5EDT,M3.2.0,M11.1.0"],
["Chicago (US Central)","CST6CDT,M3.2.0,M11.1.0"],
["Denver (US Mountain)","MST7MDT,M3.2.0,M11.1.0"],
["Phoenix (no DST)","MST7"],
["Los Angeles (US Pacific)","PST8PDT,M3.2.0,M11.1.0"],
["Anchorage","AKST9AKDT,M3.2.0,M11.1.0"],["Honolulu","HST10"],
["Mexico City","CST6"],["Bogota","<-05>5"],
["Sao Paulo / Buenos Aires","<-03>3"],
["London / Dublin","GMT0BST,M3.5.0/1,M10.5.0"],
["Central Europe","CET-1CEST,M3.5.0,M10.5.0/3"],
["Eastern Europe","EET-2EEST,M3.5.0/3,M10.5.0/4"],["Moscow","MSK-3"],
["Dubai","<+04>-4"],["India","IST-5:30"],["China","CST-8"],
["Hong Kong","HKT-8"],["Singapore","<+08>-8"],["Perth","AWST-8"],
["Tokyo","JST-9"],["Seoul","KST-9"],["Brisbane","AEST-10"],
["Sydney / Melbourne","AEST-10AEDT,M10.1.0,M4.1.0/3"],
["Auckland","NZST-12NZDT,M9.5.0,M4.1.0/3"],["UTC","UTC0"]];
(function(){var s=$("tz");TZS.forEach(function(z){
var o=document.createElement("option");o.value=z[1];o.textContent=z[0];
s.appendChild(o)});
s.addEventListener("change",function(){
fetch("/api/tz?v="+encodeURIComponent(s.value),{method:"POST",
headers:{"X-CSRF-Token":CSRF}})
.then(function(r){$("tzsub").textContent=r.ok?
"Saved. The clock is on this zone now, daylight saving included.":
"The display would not take that zone."})})})();
// settings wiring: one knob per change, the glass validates
function send(k,v){fetch("/api/set?k="+k+"&v="+v,{method:"POST",
headers:{"X-CSRF-Token":CSRF}})}
["day_pct","bright_pct","night_step"].forEach(function(id){var e=$(id);
e.addEventListener("input",function(){$(id+"v").textContent=e.value});
e.addEventListener("change",function(){send(id,e.value)})});
["night_screen","red_shift","peek_s","night_start_hh","night_end_hh"]
.forEach(function(id){$(id).addEventListener("change",function(){
send(id,$(id).value)})});
fetch("/api/settings").then(function(r){return r.json()}).then(function(s){
if(s.csrf!==undefined)CSRF=s.csrf;
["day_pct","bright_pct","night_step","night_screen","red_shift","peek_s",
"night_start_hh","night_end_hh"].forEach(function(id){
if(s[id]!==undefined)$(id).value=s[id];
var v=$(id+"v");if(v)v.textContent=$(id).value});
// Show the zone actually in force. A rule this list doesn't carry is still
// a valid zone — say what it is rather than silently showing the wrong
// entry, and leave the picker unset so nothing is claimed on its behalf.
if(s.tz!==undefined){var t=$("tz");t.value=s.tz;
if(t.value!==s.tz){t.selectedIndex=-1;
$("tzsub").textContent="Set to "+s.tz+" (not one of the presets)."}
else if(s.tz==="UTC0"){$("tzsub").textContent=
"This display is on UTC — pick your zone or the clock reads the "+
"wrong hour and night mode starts at the wrong time."}}
// Presence is the tell, exactly as it is for the app: a glass that reports
// bright_pct dims by scrim, and its day_pct would be a dead control.
if(s.bright_pct!==undefined){$("day_row").style.display="none";
$("bright_row").style.display="";
if(s.bright_min_pct!==undefined)$("bright_pct").min=s.bright_min_pct}});
// live mirror
function pad(n){return(n<10?"0":"")+n}
function tick(){fetch("/api/glass").then(function(r){return r.json()})
.then(function(g){
document.body.className=(g.flavor==="watch"?"watch":"dash")+
(g.night?" night":"");
// The wall's Character re-skins the mirror (absent on older firmware).
if(g.pal){var PM={bg:"bg",cd:"card",ed:"edge",tx:"tx",mu:"mut",
ok:"ok",wa:"warn",al:"bad",si:"blu"};
for(var pk in PM)if(g.pal[pk])
document.documentElement.style.setProperty("--"+PM[pk],"#"+g.pal[pk]);}
$("ns_off").textContent=g.flavor==="dash"?"off, tap wakes":"off, tap peeks";
$("live").className="dot"+(g.wifi?" on":"");
var t=g.time_valid?pad(g.hh)+":"+pad(g.mm):"--:--";
$("clock").textContent=t;$("mclock").textContent=t;
$("mwifi").textContent=g.wifi?(g.hub?"":"no hub"):"no wifi";
var n=g.witnesses.length;
if(n===0){$("mword").textContent=g.time_valid?t:"Listening";
$("msub").textContent="no canaries yet";}
// worst<=1 (Ok/Notice) is the wall's calm branch — same threshold, same voice
else if(g.worst<=1){$("mword").textContent=g.time_valid?t:(g.aq||"All quiet");
$("msub").textContent=(g.aql||"all quiet")+" • "+n+(n===1?" canary":" canaries");}
else{$("mword").textContent=SEVW[g.worst]||"Alert";
$("msub").textContent=g.acked?"acknowledged":"press and hold the glass to acknowledge";}
$("mword").style.color=n&&g.worst>1?SEVC[g.worst]:"var(--tx)";
$("bird").className="bird "+(MOOD[g.bird]||"idle");
var row=$("mrow");row.innerHTML="";
g.witnesses.forEach(function(w){var c=document.createElement("span");
c.className="chip s"+w.sev;c.textContent=(w.room?w.room+" ":"")+w.name;
row.appendChild(c)});
}).catch(function(){$("live").className="dot"})}
tick();setInterval(tick,2000);look();
// the receipt
function fmtUp(s){var d=Math.floor(s/86400),h=Math.floor(s%86400/3600),
m=Math.floor(s%3600/60);return(d?d+"d ":"")+h+"h "+m+"m"}
function device(){fetch("/api/device").then(function(r){return r.json()})
.then(function(d){
var rows=[["Firmware",d.fw],["Kind",d.flavor==="watch"?
"round watch glass":"wall panel"],["Brain",d.chip+" • "+d.cores+
" cores @ "+d.mhz+" MHz"],["Storage",d.flash_mb+" MB flash • "+
Math.round(d.psram_kb/1024)+" MB extra memory"],["Memory free",
d.heap_kb+" KB now (low point "+d.heap_min_kb+" KB)"],["Awake for",
fmtUp(d.up_s)],["Name",d.id],["Network",d.ssid?d.ssid+" • "+d.ip+
" • signal "+d.signal+" dB":"not connected"]];
var g=$("receipt");g.innerHTML="";
rows.forEach(function(r){var k=document.createElement("span");
k.style.color="var(--mut)";k.textContent=r[0];
var v=document.createElement("span");v.textContent=r[1];
g.appendChild(k);g.appendChild(v)});
$("caps").textContent=d.caps.join(" • ")})}
device();setInterval(device,10000);
// serial monitor
function logs(){if($("logpause").checked)return;
fetch("/api/log").then(function(r){return r.text()}).then(function(t){
var p=$("log");var stick=p.scrollTop+p.clientHeight>=p.scrollHeight-8;
p.textContent=t;if(stick)p.scrollTop=p.scrollHeight})}
logs();setInterval(logs,2000);
</script></body></html>
)RAW";
