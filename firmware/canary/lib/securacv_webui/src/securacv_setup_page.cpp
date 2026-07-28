/*
 * SecuraCV Canary — first-boot setup wizard page (captive portal)
 *
 * This is the page a phone sees the moment it joins the Canary's setup
 * Wi-Fi: the OS's captive-portal probe is answered with it (see the captive
 * handlers in securacv_network.cpp), so it pops up on its own — the user
 * never has to know to open a browser.
 *
 * It is deliberately TINY and self-contained: captive-portal mini-browsers
 * (the iOS Captive Network Assistant sheet) are stripped-down webviews that
 * choke on the full dashboard SPA (see canary-wap's setup_page_html.h for
 * the blank-white-screen lesson). Plain HTML + a little vanilla JS against
 * three endpoints — /api/wifi/scan, /api/wifi/connect, /api/wifi/status —
 * is all it needs, and all of it works inside the sheet.
 *
 * The Wi-Fi password field is NOT type=password on purpose: iOS reads a
 * password field on an unfamiliar page as "sign-up form" and covers it with
 * a strong-NEW-password suggestion — exactly wrong for typing an existing
 * Wi-Fi key. It's a text input masked with -webkit-text-security instead,
 * with autofill/autocorrect off (same fix as the dashboard's field).
 *
 * `__CV_TOKEN__` is replaced at serve time with the device's API bearer
 * token, same as the dashboard (see handle_ui / send_html_with_token).
 *
 * Copyright (c) 2026 ERRERlabs / Karl May
 * License: Apache-2.0
 */

#include "securacv_webui.h"

const char CANARY_SETUP_HTML[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<title>SecuraCV Canary — Setup</title>
<meta name="theme-color" content="#0b0d10">
<style>
:root{--bg:#0b0d10;--fg:#f0f2f5;--muted:#a0a8b0;--accent:#7cdcff;--gold:#f5c542;--ok:#4cd964;--err:#ff6b6b;--card:rgba(255,255,255,.05);--line:rgba(255,255,255,.10)}
*{box-sizing:border-box}
html,body{margin:0;min-height:100vh;min-height:100dvh;background:radial-gradient(1200px 800px at 50% -100px,#1a2030 0%,var(--bg) 60%) fixed;color:var(--fg);font:16px/1.5 -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,Helvetica,Arial,sans-serif}
body{display:flex;justify-content:center;padding:20px 16px 40px}
main{max-width:480px;width:100%}
.brand{display:flex;align-items:center;gap:10px;margin:6px 0 14px}
.brand .bird{font-size:26px}
.brand b{font-size:17px;letter-spacing:-.01em}
.brand .tag{margin-left:auto;font-size:11px;font-weight:700;text-transform:uppercase;letter-spacing:.08em;color:var(--gold);border:1px solid rgba(245,197,66,.4);border-radius:99px;padding:3px 10px}
.card{background:var(--card);border:1px solid var(--line);border-radius:20px;padding:20px 18px;margin-bottom:14px}
h1{margin:0 0 6px;font-size:21px;font-weight:650;letter-spacing:-.02em}
.lead{margin:0 0 4px;color:var(--muted);font-size:14px}
.keep{margin:10px 0 0;font-size:12.5px;color:var(--muted);border-left:3px solid rgba(124,220,255,.5);padding-left:10px}
.nets{margin:14px 0 0;display:flex;flex-direction:column;gap:8px}
.net{display:flex;align-items:center;gap:10px;width:100%;text-align:left;background:rgba(255,255,255,.04);border:1px solid var(--line);border-radius:14px;padding:13px 14px;color:var(--fg);font:inherit;font-size:15px;cursor:pointer}
.net:active{background:rgba(124,220,255,.12)}
.net .sig{margin-left:auto;color:var(--accent);font-size:12px;letter-spacing:2px;flex:none}
.net .lock{color:var(--muted);font-size:12px;flex:none}
.scanning{color:var(--muted);font-size:14px;padding:12px 0}
.linkish{background:none;border:none;color:var(--accent);font:inherit;font-size:14px;padding:8px 0;cursor:pointer;text-decoration:underline;text-underline-offset:3px}
label{display:block;font-size:13px;color:var(--muted);margin:14px 0 6px}
input.txt{width:100%;padding:13px 14px;border-radius:14px;border:1px solid var(--line);background:rgba(0,0,0,.35);color:var(--fg);font-size:16px}
input.txt:focus{outline:none;border-color:var(--accent)}
.pw-wrap{position:relative}
.pw-masked{-webkit-text-security:disc;text-security:disc}
.pw-show{position:absolute;right:6px;top:50%;transform:translateY(-50%);background:none;border:none;color:var(--muted);font-size:13px;padding:8px;cursor:pointer}
.hint{margin:8px 0 0;font-size:12.5px;color:var(--muted)}
.btn{display:block;width:100%;margin-top:16px;padding:14px;border:none;border-radius:14px;background:var(--accent);color:#04222e;font:inherit;font-size:16px;font-weight:650;cursor:pointer}
.btn:disabled{opacity:.45}
.status{display:none;margin-top:14px;font-size:14px;color:var(--muted)}
.status .spin{display:inline-block;width:14px;height:14px;border:2px solid rgba(124,220,255,.3);border-top-color:var(--accent);border-radius:50%;margin-right:8px;vertical-align:-2px;animation:sp 1s linear infinite}
@keyframes sp{to{transform:rotate(360deg)}}
.err{display:none;margin-top:12px;font-size:14px;color:var(--err)}
#done{display:none}
.big-ok{font-size:40px;margin:2px 0 8px}
.steps{list-style:none;margin:12px 0 0;padding:0;font-size:14px;text-align:left}
.steps li{display:flex;gap:10px;align-items:flex-start;padding:7px 0}
.steps li b{flex:none;width:22px;height:22px;line-height:22px;text-align:center;border-radius:50%;background:rgba(76,217,100,.16);color:var(--ok);font-size:12px}
.steps .addr{color:var(--accent);font-weight:700;font-family:ui-monospace,SFMono-Regular,Menlo,monospace;user-select:all}
.foot{margin-top:6px;color:var(--muted);font-size:11px;text-align:center}
.foot strong{color:var(--fg)}
@media (prefers-color-scheme:light){:root{--bg:#f4f5f7;--fg:#0b0d10;--muted:#5b6470;--card:rgba(255,255,255,.92);--line:rgba(0,0,0,.08)}html,body{background:radial-gradient(1200px 800px at 50% -100px,#dfe5ee 0%,var(--bg) 60%) fixed}.foot strong{color:#0b0d10}}
</style>
</head>
<body>
<main>
<div class="brand"><span class="bird">🐤</span><b>SecuraCV Canary</b><span class="tag">Setup wizard</span></div>

<section class="card" id="wizard">
<h1>Put your Canary on your Wi-Fi</h1>
<p class="lead">Pick the Wi-Fi network this Canary should join — the same home network your phone normally uses. It keeps watch from there.</p>
<p class="keep">Keep this window open until it says done — closing it early stops the setup.</p>

<div id="scanbox">
<div class="nets" id="nets"><div class="scanning" id="scanmsg">Looking for nearby networks…</div></div>
<button class="linkish" id="rescan" type="button">Scan again</button>
<button class="linkish" id="manual" type="button">My network isn't listed</button>
<div id="manualbox" style="display:none">
<label for="ssid">Network name</label>
<input class="txt" id="ssid" autocomplete="off" autocapitalize="none" autocorrect="off" spellcheck="false" placeholder="Exactly as your router shows it">
<p class="hint">Canaries speak 2.4 GHz Wi-Fi. If your network is 5 GHz-only, give it a 2.4 GHz name too, then come back.</p>
</div>
</div>

<div id="passbox" style="display:none">
<label id="passlabel" for="pass">Wi-Fi password</label>
<div class="pw-wrap">
<input class="txt pw-masked" id="pass" autocomplete="off" autocapitalize="none" autocorrect="off" spellcheck="false" placeholder="Your existing Wi-Fi password">
<button class="pw-show" id="showpw" type="button">Show</button>
</div>
<p class="hint">The password you already use for this network — the one saved in your phone. Nothing new to invent.</p>
<button class="btn" id="join" type="button">Join this network</button>
</div>

<p class="status" id="status"><span class="spin"></span><span id="statustext"></span></p>
<p class="err" id="err"></p>
</section>

<section class="card" id="done" style="text-align:center">
<div class="big-ok">✅</div>
<h1 id="donetitle">Your Canary is on your Wi-Fi</h1>
<p class="lead" id="donelead"></p>
<ol class="steps">
<li><b>1</b><span>Tap <strong>Done</strong> (top corner) to close this window.</span></li>
<li><b>2</b><span>Put your phone back on your home Wi-Fi — it usually hops back by itself.</span></li>
<li><b>3</b><span>Open <span class="addr">canary.local</span> in your browser any time — that's your Canary's home page<span id="ipalt"></span>.</span></li>
</ol>
<details id="hubbox" style="text-align:left;margin-top:14px">
<summary style="cursor:pointer;color:var(--accent);font-size:14px">Have a SecuraCV hub? Point your Canary at it (optional)</summary>
<p class="hint">These defaults are right for a SecuraCV hub (Home Assistant with the Mosquitto broker add-on). No hub yet? Skip this — you can do it any time from canary.local.</p>
<label for="mhost">Hub address</label>
<input class="txt" id="mhost" value="homeassistant.local" autocomplete="off" autocapitalize="none" autocorrect="off" spellcheck="false">
<label for="mport">Port</label>
<input class="txt" id="mport" value="1883" inputmode="numeric" autocomplete="off">
<label for="muser">Hub username (only if your broker asks for one)</label>
<input class="txt" id="muser" autocomplete="off" autocapitalize="none" autocorrect="off" spellcheck="false" placeholder="Optional">
<label for="mpass">Hub password</label>
<div class="pw-wrap">
<input class="txt pw-masked" id="mpass" autocomplete="off" autocapitalize="none" autocorrect="off" spellcheck="false" placeholder="Optional">
<button class="pw-show" id="showmpw" type="button">Show</button>
</div>
<button class="btn" id="mqttsave" type="button">Save hub settings</button>
<p class="hint" id="mqttmsg" style="display:none"></p>
<button class="btn" id="rebootnow" type="button" style="display:none;background:var(--gold)">Restart the Canary now</button>
</details>
</section>

<p class="foot"><strong>SecuraCV Canary.</strong> Local-only by default. Nothing leaves your home.</p>
</main>
<script>
var CV_TOKEN='__CV_TOKEN__';
function hdrs(json){var h={};if(CV_TOKEN&&CV_TOKEN.charAt(0)!=='_')h['Authorization']='Bearer '+CV_TOKEN;if(json)h['Content-Type']='application/json';return h}
function $(id){return document.getElementById(id)}
var chosen='';

function sigDots(rssi){return rssi>-55?'●●●':rssi>-70?'●●○':'●○○'}
function renderNets(list){
  var box=$('nets');box.innerHTML='';
  if(!list.length){box.innerHTML='<div class="scanning">No networks found yet — try Scan again, or type yours below.</div>';return}
  // Strongest first, one row per name.
  var seen={},out=[];
  list.sort(function(a,b){return b.rssi-a.rssi});
  list.forEach(function(n){if(n.ssid&&!seen[n.ssid]){seen[n.ssid]=1;out.push(n)}});
  out.forEach(function(n){
    var b=document.createElement('button');b.type='button';b.className='net';
    var lock=n.encryption==='open'?'':'<span class="lock">🔒</span>';
    b.innerHTML='<span></span>'+lock+'<span class="sig">'+sigDots(n.rssi)+'</span>';
    b.firstChild.textContent=n.ssid;
    b.addEventListener('click',function(){pick(n.ssid)});
    box.appendChild(b);
  });
}
function scan(){
  $('scanmsg')&&($('scanmsg').textContent='Looking for nearby networks…');
  fetch('/api/wifi/scan',{headers:hdrs()}).then(function(r){return r.json()}).then(function(d){
    renderNets((d&&d.networks)||[]);
  }).catch(function(){renderNets([])});
}
function pick(ssid){
  chosen=ssid;
  $('ssid').value=ssid;
  $('passbox').style.display='block';
  $('passlabel').textContent='Wi-Fi password for “'+ssid+'”';
  $('err').style.display='none';
  $('pass').focus();
}
$('rescan').addEventListener('click',scan);
$('manual').addEventListener('click',function(){
  $('manualbox').style.display='block';
  $('passbox').style.display='block';
  $('ssid').focus();
});
$('ssid').addEventListener('input',function(){chosen=$('ssid').value.trim();$('passlabel').textContent=chosen?'Wi-Fi password for “'+chosen+'”':'Wi-Fi password'});
$('showpw').addEventListener('click',function(){
  var p=$('pass');
  if(p.classList.contains('pw-masked')){p.classList.remove('pw-masked');this.textContent='Hide'}
  else{p.classList.add('pw-masked');this.textContent='Show'}
});
var polls=0,timer=null;
function showDone(ip){
  $('wizard').style.display='none';
  $('done').style.display='block';
  $('donelead').textContent=chosen?'It joined “'+chosen+'” and is settling onto its perch.':'It joined your network and is settling onto its perch.';
  if(ip)$('ipalt').innerHTML=' (or <span class="addr"></span>)',$('ipalt').querySelector('.addr').textContent=ip;
}
function poll(){
  fetch('/api/wifi/status',{headers:hdrs()}).then(function(r){return r.json()}).then(function(d){
    if(d&&d.sta_connected){clearInterval(timer);showDone(d.sta_ip);return}
    if(++polls>=25){clearInterval(timer);fail('It couldn’t join — nine times out of ten that’s a typo in the password. Check it and try again.')}
  }).catch(function(){
    if(++polls>=25){clearInterval(timer);fail('Lost touch with the Canary — stay on its SecuraCV Wi-Fi network and try again.')}
  });
}
function fail(msg){
  $('status').style.display='none';
  $('err').textContent=msg;$('err').style.display='block';
  $('join').disabled=false;
}
$('join').addEventListener('click',function(){
  var ssid=chosen||$('ssid').value.trim();
  if(!ssid){fail('Pick a network above (or type its name) first.');return}
  chosen=ssid;
  $('err').style.display='none';
  $('join').disabled=true;
  $('status').style.display='block';
  $('statustext').textContent='Sending it to “'+ssid+'” — takes about half a minute…';
  fetch('/api/wifi/connect',{method:'POST',headers:hdrs(true),body:JSON.stringify({ssid:ssid,password:$('pass').value})})
    .then(function(r){return r.json()}).then(function(d){
      if(!d||!d.ok){fail((d&&d.error)?('That didn’t save: '+d.error):'That didn’t save — try again.');return}
      polls=0;timer=setInterval(poll,2000);
    }).catch(function(){fail('Couldn’t reach the Canary — stay on its SecuraCV Wi-Fi network and try again.')});
});
// Optional "point it at your hub" step on the success screen. The prefills
// (homeassistant.local, 1883) are exactly right for a SecuraCV hub — Home
// Assistant OS running the Mosquitto broker add-on.
$('showmpw').addEventListener('click',function(){
  var p=$('mpass');
  if(p.classList.contains('pw-masked')){p.classList.remove('pw-masked');this.textContent='Hide'}
  else{p.classList.add('pw-masked');this.textContent='Show'}
});
function mqttMsg(t){$('mqttmsg').textContent=t;$('mqttmsg').style.display='block'}
$('mqttsave').addEventListener('click',function(){
  var host=$('mhost').value.trim();
  if(!host){mqttMsg('Type the hub address first (homeassistant.local is the usual one).');return}
  var body={host:host,port:parseInt($('mport').value,10)||1883,enabled:true};
  if($('muser').value.trim())body.username=$('muser').value.trim();
  if($('mpass').value)body.password=$('mpass').value;
  $('mqttsave').disabled=true;
  fetch('/api/mqtt/config',{method:'POST',headers:hdrs(true),body:JSON.stringify(body)})
    .then(function(r){return r.json()}).then(function(d){
      $('mqttsave').disabled=false;
      if(d&&d.ok){mqttMsg('Saved — it takes effect after the Canary restarts.');$('rebootnow').style.display='block'}
      else{mqttMsg('Couldn’t save'+((d&&d.error)?(' ('+d.error+')'):'')+' — you can set this any time from canary.local.')}
    }).catch(function(){$('mqttsave').disabled=false;mqttMsg('This Canary doesn’t have the hub bridge turned on — set it up later from canary.local.')});
});
$('rebootnow').addEventListener('click',function(){
  this.disabled=true;
  fetch('/api/reboot',{method:'POST',headers:hdrs()}).catch(function(){});
  mqttMsg('Restarting — its SecuraCV network will vanish for a minute while it comes back up on your home Wi-Fi. You can close this window.');
});
// Already set up (page reopened)? Jump straight to the finish line.
fetch('/api/wifi/status',{headers:hdrs()}).then(function(r){return r.json()}).then(function(d){
  if(d&&d.sta_connected){showDone(d.sta_ip)}else{scan()}
}).catch(scan);
</script>
</body>
</html>
)HTML";
