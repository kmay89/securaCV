// tv_html.h — GENERATED from tv/index.html by tv/gen_tv_html.py.
// Do not edit by hand: edit tv/index.html and re-run the generator.
// The Canary serves this at GET /tv — a 10-foot ambient security
// surface for any television on the home WiFi. Self-contained (no
// CDN, no internet): the LAN promise holds, same as mirror_html.h.
#pragma once
#include <pgmspace.h>

static const char TV_HTML[] PROGMEM = R"TVGLASS(<!doctype html>
<!-- tv/index.html — "Canary TV": a 10-foot ambient security surface for any
     television. Whole and self-contained (no CDN, no fonts, no internet: the
     LAN promise holds). Consumes the Canary's on-device /api/glass JSON, the
     same snapshot the wall glass and the phone mirror read — so no hub is
     required. A television on the home WiFi, pointed at a Canary, is a
     conformant Open Ambient Security Display (docs/standard/AMBIENT_DISPLAY_STANDARD.md).

     Deploy three ways, cheapest first:
       1. Smart-TV browser  → open http://<canary>.local/tv  (served by the Canary)
       2. Cast / HDMI stick → same URL in kiosk mode
       3. "Canary TV" dongle→ ships this page, boots straight into it

     Query options (all optional):
       ?src=http://192.168.1.42   data origin (default: same origin as this page)
       ?demo=1                    render a synthetic fleet, loudly labeled DEMO
       ?rooms=1                   group witnesses by room
       ?poll=2000                 poll interval ms (default 2000)
-->
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Canary TV</title>
<style>
/* Quiet Glass tokens (parity with mirror_html.h / display_ux_design.md). The
   live palette is overridden per-Character from /api/glass `pal` at runtime. */
:root{
  --bg:#0b0b0c; --card:#141414; --edge:#262626; --tx:#ededed; --mut:#8a8a8a;
  --yel:#FFD44F; --ok:#43A047; --notice:#03A9F4; --warn:#FB8C00; --bad:#E53935;
  --hero:#43A047;               /* recolored to the worst state each cycle */
  --heroink:#0b0b0c;
}
*{box-sizing:border-box;margin:0}
html,body{height:100%}
body{
  background:var(--bg); color:var(--tx);
  font:400 2.2vh/1.35 system-ui,-apple-system,Segoe UI,Roboto,sans-serif;
  overflow:hidden; user-select:none; -webkit-user-select:none;
}
/* Burn-in guard: the whole surface drifts a couple of TV pixels on a slow
   cycle so no element is ever truly static on an OLED for 24/7 runs.
   AD-Calm motion budget: this is enumerated, sub-perceptual, and pauses for
   nothing the eye tracks. */
@keyframes drift{
  0%{transform:translate(0,0)} 25%{transform:translate(6px,4px)}
  50%{transform:translate(2px,8px)} 75%{transform:translate(8px,2px)}
  100%{transform:translate(0,0)}
}
#stage{
  position:fixed; inset:0; display:flex; flex-direction:column;
  padding:4.5vh 5vw; gap:2.4vh; animation:drift 240s ease-in-out infinite;
  transition:filter .6s ease, opacity .6s ease;
}
/* AD-Calm night floor: red-shifted, dimmed, no 460–500nm. Driven by the
   `night` flag from the wall (which owns the real light schedule) with a
   local clock fallback so a standalone stick still dims itself. */
body.night #stage{ filter:sepia(.7) hue-rotate(-28deg) saturate(2.2) brightness(.5); }

/* ── Top strip: identity, clock, transport honesty ─────────────────── */
#top{ display:flex; align-items:center; gap:2vw; flex:0 0 auto; }
#brand{ display:flex; align-items:center; gap:1.2vw; font-weight:700; font-size:3.2vh; letter-spacing:.01em; }
/* the canary, in CSS — same silhouette / brand yellow as the wall */
.bird{ position:relative; width:5.2vh; height:4.6vh; flex:0 0 auto; }
.b{ position:absolute; border-radius:50%; }
.tail{ left:0; top:41%; width:17%; height:13%; background:#E3B33C }
.bd{ left:10%; top:33%; width:62%; height:61%; background:var(--yel) }
.wing{ left:19%; top:46%; width:31%; height:26%; background:#E3B33C }
.hd{ left:46%; top:11%; width:42%; height:48%; background:var(--yel) }
.beak{ left:82%; top:26%; width:16%; height:11%; background:#F08C2E; border-radius:12% }
#clock{ margin-left:auto; font-size:4vh; font-weight:600; font-variant-numeric:tabular-nums; letter-spacing:.02em; }
#clock small{ font-size:2vh; color:var(--mut); margin-left:.4vw; }

/* Transport / freshness banner — AD-Core: a dead transport is bannered
   within one render cycle; last-known is labeled as such. */
#banner{
  flex:0 0 auto; display:none; align-items:center; gap:1.2vw;
  padding:1.4vh 2vw; border-radius:1.4vh; font-size:2.4vh; font-weight:600;
  background:#2a1600; color:var(--warn); border:.3vh solid var(--warn);
}
#banner.show{ display:flex; }
#banner.lost{ background:#2a0d0d; color:var(--bad); border-color:var(--bad); }

/* ── Hero: worst state, glanceable in ≤1s at room distance ──────────── */
#hero{
  flex:1 1 auto; display:flex; align-items:center; gap:4vw;
  border-radius:2.2vh; padding:0 4vw;
  background:linear-gradient(135deg, color-mix(in srgb, var(--hero) 20%, var(--card)), var(--card));
  border:.35vh solid color-mix(in srgb, var(--hero) 55%, var(--edge));
  min-height:0;
}
#glyph{
  flex:0 0 auto; width:20vh; height:20vh; border-radius:50%;
  display:flex; align-items:center; justify-content:center;
  background:var(--hero); color:var(--heroink); font-size:11vh; font-weight:800;
}
/* AD-Calm: the alert breathing pulse — the ONLY hero motion, and only while
   an alert/tamper condition is unacknowledged. Everything else is at rest. */
@keyframes breathe{ 0%,100%{box-shadow:0 0 0 0 var(--hero)} 50%{box-shadow:0 0 0 2.4vh color-mix(in srgb,var(--hero) 40%,transparent)} }
#hero.alarm #glyph{ animation:breathe 2.6s ease-in-out infinite; }
#herotext{ min-width:0; }
#word{ font-size:9vh; font-weight:800; line-height:1; letter-spacing:-.01em; color:var(--hero); }
#word.dim{ color:var(--tx); }        /* "all quiet" reads as calm text, not green shout */
#sub{ font-size:3vh; color:var(--mut); margin-top:1.4vh; }
#trust{ font-size:2vh; color:var(--mut); margin-top:1vh; }

/* ── Roll-call: every witness, always with label + glyph, never color
       alone (AD-Core §2.2 / WCAG 1.4.1) ────────────────────────────── */
#fleet{ flex:0 0 auto; display:grid; gap:1.6vw; grid-auto-rows:1fr;
  grid-template-columns:repeat(auto-fit,minmax(20vw,1fr)); }
.w{
  background:var(--card); border:.3vh solid var(--edge); border-left:.9vh solid var(--mut);
  border-radius:1.4vh; padding:1.8vh 1.6vw; display:flex; flex-direction:column; gap:.6vh;
}
.w .nm{ font-size:2.9vh; font-weight:700; white-space:nowrap; overflow:hidden; text-overflow:ellipsis; }
.w .rm{ font-size:2vh; color:var(--mut); }
.w .st{ display:flex; align-items:center; gap:.8vw; font-size:2.3vh; font-weight:600; margin-top:.4vh; }
.w .st .ic{ font-size:2.6vh; }
.w.s0{ border-left-color:var(--ok) }      .w.s0 .st{ color:var(--ok) }
.w.s1{ border-left-color:var(--notice) }  .w.s1 .st{ color:var(--notice) }
.w.s2{ border-left-color:var(--warn) }    .w.s2 .st{ color:var(--warn) }
.w.s3,.w.s4{ border-left-color:var(--bad) } .w.s3 .st,.w.s4 .st{ color:var(--bad) }
.w.stale{ opacity:.72 } .w.lost{ opacity:.62 }
.roomhdr{ grid-column:1/-1; font-size:2vh; color:var(--mut); text-transform:uppercase;
  letter-spacing:.1em; margin-top:.6vh; }

/* ── Footer: count, source, honest provenance ──────────────────────── */
#foot{ flex:0 0 auto; display:flex; align-items:center; gap:2vw; font-size:2vh; color:var(--mut); }
#demo{ display:none; margin-left:auto; padding:.6vh 1.4vw; border-radius:1vh;
  background:#3a2b00; color:var(--yel); border:.25vh solid var(--yel); font-weight:700; letter-spacing:.08em; }
body.demo #demo{ display:inline-block; }
#src{ margin-left:auto; }
.pdescribe{ position:fixed; bottom:0; left:0; right:0; text-align:center;
  font-size:1.6vh; color:#3a3a3e; padding:.6vh; }
</style></head>
<body>
<div id="stage">
  <div id="top">
    <div id="brand">
      <span class="bird"><i class="b tail"></i><i class="b bd"></i><i class="b wing"></i><i class="b hd"></i><i class="b beak"></i></span>
      <span>Canary</span>
    </div>
    <div id="clock">--:--<small id="ampm"></small></div>
  </div>

  <div id="banner"><span id="banner-ic">⚠</span><span id="banner-tx"></span></div>

  <div id="hero">
    <div id="glyph">✓</div>
    <div id="herotext">
      <div id="word" class="dim">Starting…</div>
      <div id="sub">Reaching your Canaries…</div>
      <div id="trust"></div>
    </div>
  </div>

  <div id="fleet"></div>

  <div id="foot">
    <span id="count">—</span>
    <span id="demo">DEMO · NOT A LIVE FLEET</span>
    <span id="src"></span>
  </div>
</div>
<div class="pdescribe">Ambient security surface · witnessing without watching · no cameras, no cloud</div>

<script>
"use strict";
(function(){
  // ── options ──────────────────────────────────────────────────────────
  var q = new URLSearchParams(location.search);
  var DEMO = q.get("demo") === "1";
  var BYROOM = q.get("rooms") === "1";
  var POLL = Math.max(1000, parseInt(q.get("poll") || "2000", 10));
  // Data origin: default same-origin (page served BY the Canary → no CORS,
  // no config). Override for a hostable stick that points at a Canary IP.
  var SRC = (q.get("src") || "").replace(/\/$/, "");
  var GLASS = SRC ? SRC + "/api/glass" : "/api/glass";
  if (DEMO) document.body.classList.add("demo");

  // ── severity vocabulary (fleet_model.h Sev ladder, worst-last) ───────
  // Never color-only: each state carries word + glyph + border position.
  var SEV = [
    {key:"ok",     word:"All quiet",   ic:"✓", css:"s0", cvar:"--ok"},
    {key:"notice", word:"Activity",    ic:"•", css:"s1", cvar:"--notice"},
    {key:"warn",   word:"Needs a look",ic:"!", css:"s2", cvar:"--warn"},
    {key:"alert",  word:"Alert",       ic:"⚠", css:"s3", cvar:"--bad"},
    {key:"tamper", word:"Tamper",      ic:"⛨", css:"s4", cvar:"--bad"},
  ];
  // Per-witness liveness deadlines (AD-Core §2.1 reference: amber ≤3m, red
  // ≤10m). The firmware already folds these into `worst`; we re-derive the
  // per-chip label so silence is never rendered as calm.
  var STALE_S = 180, LOST_S = 600;

  var $ = function(id){ return document.getElementById(id); };
  function css(v){ return getComputedStyle(document.documentElement).getPropertyValue(v).trim(); }

  // ── freshness tracking (AD-Resilient) ────────────────────────────────
  var lastGood = 0;          // ms epoch of last successful snapshot
  var lastSnap = null;       // last snapshot we rendered
  var fails = 0;

  function fetchGlass(){
    if (DEMO){ render(demoSnap(), true); schedule(); return; }
    var ctrl = new AbortController();
    var to = setTimeout(function(){ ctrl.abort(); }, Math.min(POLL, 4000));
    fetch(GLASS, {cache:"no-store", signal:ctrl.signal})
      .then(function(r){ if(!r.ok) throw new Error(r.status); return r.json(); })
      .then(function(j){ clearTimeout(to); fails=0; lastGood=Date.now(); lastSnap=j; render(j,false); })
      .catch(function(){ clearTimeout(to); fails++; renderStale(); })
      .finally(schedule);
  }
  function schedule(){
    // gentle backoff when the Canary is unreachable — but keep showing the
    // last-known frame with its honest "signal lost" banner.
    var d = fails ? Math.min(POLL * Math.pow(1.6, Math.min(fails,5)), 30000) : POLL;
    setTimeout(fetchGlass, d);
  }

  function applyPalette(pal){
    if(!pal) return;
    var map = {bg:"--bg", cd:"--card", ed:"--edge", tx:"--tx", mu:"--mut",
               ok:"--ok", wa:"--warn", al:"--bad", si:"--notice"};
    for(var k in map){ if(pal[k]) document.documentElement.style.setProperty(map[k], "#"+pal[k]); }
  }

  function localNight(){
    var h = new Date().getHours();
    return h >= 22 || h < 6;    // fallback dim window for a standalone stick
  }

  function render(s, isDemo){
    applyPalette(s.pal);
    document.body.classList.toggle("night", !!s.night || localNight());

    // clock — prefer the wall's SNTP time; fall back to the browser's.
    var hh, mm;
    if (s.time_valid){ hh = s.hh; mm = s.mm; }
    else { var d = new Date(); hh = d.getHours(); mm = d.getMinutes(); }
    var h12 = ((hh + 11) % 12) + 1;
    $("clock").firstChild.nodeValue = h12 + ":" + String(mm).padStart(2,"0");
    $("ampm").textContent = hh < 12 ? "AM" : "PM";

    var worst = Math.max(0, Math.min(4, s.worst|0));
    var v = SEV[worst];
    var heroCol = css(v.cvar);
    document.documentElement.style.setProperty("--hero", heroCol);
    document.documentElement.style.setProperty("--heroink", worst===0? "#0b0b0c" : "#0b0b0c");
    $("glyph").textContent = v.ic;
    var word = worst===0 ? (s.aq || v.word) : v.word;
    var wordEl = $("word");
    wordEl.textContent = titlecase(word);
    wordEl.classList.toggle("dim", worst===0);
    wordEl.style.color = worst===0 ? "" : heroCol;
    $("hero").classList.toggle("alarm", worst>=3 && !s.acked);

    var n = (s.witnesses||[]).length;
    $("sub").textContent = worst===0
      ? (n ? "All " + n + " Canaries reporting" : "No Canaries yet")
      : summarize(s.witnesses);
    // AD-Core §2.5 / §5: this browser did NOT verify signatures on its own
    // silicon, so it must not say "verified". State the source honestly.
    $("trust").textContent = (s.acked ? "Acknowledged · " : "")
      + (s.hub ? "via your hub" : "reading a Canary directly — no hub")
      + (s.wifi===0 ? " · WiFi down" : "");

    renderFleet(s.witnesses || []);

    $("count").textContent = n + (n===1?" Canary":" Canaries");
    $("src").textContent = isDemo ? "" : ("source: " + (SRC || location.host));
    freshBanner(false);
  }

  function renderStale(){
    // Nothing new from the Canary. Keep the last frame but tell the truth.
    if (lastSnap) render(lastSnap, false);
    else { $("word").textContent = "No signal"; $("word").classList.remove("dim");
           $("word").style.color = css("--bad"); $("glyph").textContent = "⚠";
           $("sub").textContent = "Cannot reach a Canary yet"; }
    freshBanner(true);
  }

  function freshBanner(lost){
    var b = $("banner");
    if (!lost){ b.classList.remove("show","lost"); return; }
    var ageS = lastGood ? Math.round((Date.now()-lastGood)/1000) : 0;
    b.classList.add("show","lost");
    $("banner-ic").textContent = "⚠";
    $("banner-tx").textContent = lastGood
      ? "Signal lost — showing last known state from " + agoText(ageS)
      : "No connection to your Canaries — check WiFi";
  }

  function witnessLive(w){
    // Fold the per-witness age into a display sev + liveness label so a
    // silent witness degrades even if the snapshot's own sev lagged.
    var sev = Math.max(0, Math.min(4, w.sev|0));
    var age = w.age_s|0, live = "";
    if (age >= LOST_S){ sev = Math.max(sev, 3); live = "lost"; }
    else if (age >= STALE_S){ sev = Math.max(sev, 2); live = "stale"; }
    return {sev:sev, live:live, age:age};
  }

  function renderFleet(ws){
    var host = $("fleet"); host.innerHTML = "";
    var order = ws.slice();
    if (BYROOM) order.sort(function(a,b){ return (a.room||"~").localeCompare(b.room||"~"); });
    else order.sort(function(a,b){ return (witnessLive(b).sev) - (witnessLive(a).sev); });
    var curRoom = null;
    order.forEach(function(w){
      if (BYROOM && (w.room||"") !== curRoom){
        curRoom = w.room||"";
        var h = document.createElement("div"); h.className="roomhdr";
        h.textContent = curRoom || "Unassigned"; host.appendChild(h);
      }
      var L = witnessLive(w), v = SEV[L.sev];
      var el = document.createElement("div");
      el.className = "w " + v.css + (L.live? " "+L.live : "");
      var label = L.live==="lost" ? "Silent" : L.live==="stale" ? "Quiet a while"
                : L.sev===0 ? "Quiet" : v.word;
      var suffix = L.live ? " · last seen " + agoText(L.age)
                 : (w.br ? " · breathing" : w.wb ? " · present" : "");
      el.innerHTML =
        '<div class="nm"></div><div class="rm"></div>' +
        '<div class="st"><span class="ic"></span><span class="lb"></span></div>';
      el.querySelector(".nm").textContent = w.name || "Canary";
      el.querySelector(".rm").textContent = w.room || "—";
      el.querySelector(".ic").textContent = L.live==="lost" ? "⌛" : L.live==="stale" ? "…" : v.ic;
      el.querySelector(".lb").textContent = label + suffix;
      host.appendChild(el);
    });
  }

  // ── helpers ──────────────────────────────────────────────────────────
  function titlecase(s){ s=String(s||""); return s.charAt(0).toUpperCase()+s.slice(1); }
  function agoText(sec){
    if (sec < 60) return sec + "s ago";
    if (sec < 3600) return Math.round(sec/60) + "m ago";
    return Math.round(sec/3600) + "h ago";
  }
  function summarize(ws){
    ws = ws || [];
    var by = {};
    ws.forEach(function(w){ var L=witnessLive(w); if(L.sev>=2){ (by[w.name||w.room||"a Canary"]=1); } });
    var names = Object.keys(by);
    if (!names.length) return "Something needs a look";
    return names.slice(0,3).join(", ") + (names.length>3? " +"+(names.length-3) : "");
  }

  // ── demo fleet (loudly labeled; can never be mistaken for a real one,
  //    per docs/hardware/display_modes.md) ──────────────────────────────
  function demoSnap(){
    var t = new Date();
    return {
      flavor:"tv", night:0, time_valid:1, hh:t.getHours(), mm:t.getMinutes(),
      wifi:1, hub:0, worst:2, acked:0, aq:"All quiet",
      witnesses:[
        {name:"Front Door", room:"Entry",   sev:0, age_s:8,   wb:0, br:0},
        {name:"Kitchen",    room:"Kitchen",  sev:1, age_s:3,   wb:1, br:1},
        {name:"Garage",     room:"Garage",   sev:2, age_s:44,  wb:0, br:0},
        {name:"Backyard",   room:"Outside",  sev:0, age_s:220, wb:0, br:0},
        {name:"Nursery",    room:"Upstairs", sev:0, age_s:12,  wb:1, br:1},
      ],
    };
  }

  fetchGlass();
})();
</script>
</body></html>
)TVGLASS";
