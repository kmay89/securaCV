// common/network/setup_portal.cpp — the shared headless setup portal.
//
// See setup_portal.h for the behavior contract and setup_portal_logic.h for
// the pure timing decisions (host-tested). This file is the I/O: SoftAP,
// A-only captive DNS, the wizard page, and the tested join. It is compiled
// directly by each adopting board's build (build_src_filter, same pattern as
// common/ota) with the board's include paths, so it must stay core-2.x AND
// core-3.x clean — no APIs that drifted between arduino-esp32 2.0.x (vision's
// C3/S3 envs) and 3.x (sense's C6 env).
//
// Everything stateful lives in one heap Ctx allocated at begin() and freed at
// teardown, so a provisioned board carries zero portal baggage at steady
// state — the display portal's rule, kept.

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <WebServer.h>
#include <Preferences.h>
#include <esp_random.h>
#include <string.h>

#include "network/provision_core.h"
#include "network/setup_portal.h"
#include "network/setup_portal_logic.h"
#include "network/wifi_join_policy.h"

namespace canary {
namespace net {

namespace {

constexpr size_t AP_PASS_LEN = 8;  // WPA2 minimum; printed in the serial log
constexpr int SCAN_MAX = 20;

enum class St : uint8_t { Waiting, Testing, Fail, Success };

struct ScanRow { char ssid[33]; int32_t rssi; bool secure; };

struct Ctx {
  WebServer server{80};
  WiFiUDP dns;
  SetupPortalConfig cfg{};
  SetupPortalTiming timing{};
  St st = St::Waiting;
  uint32_t st_since = 0;
  bool portal_seen = false;   // any GET / — the captive sheet actually popped
  bool phone_acked = false;   // /status poll observed our success
  uint32_t acked_at = 0;
  uint32_t success_at = 0;
  bool bg_join = false;       // the current Testing pass is the quiet retry
  uint32_t bg_last_try = 0;
  char join_ssid[33] = {0};
  char join_pass[65] = {0};
  char fail_reason[48] = {0};
  ScanRow scan[SCAN_MAX];
  int scan_n = 0;
  uint32_t scan_at = 0;       // millis of last completed sweep (0 = never)
  bool saw_station = false;   // any phone has EVER associated this session
  bool stuck_hinted = false;
  char product_json[64] = {0};  // pre-escaped product name for /scan
};

Ctx* g = nullptr;
bool s_joined_latch = false;

void enter(St st, uint32_t now) { g->st = st; g->st_since = now; }

// ── The wizard page ───────────────────────────────────────────────────────
// The display portal's page with the glass-only pieces removed (no time-zone
// picker — a headless sensor keeps no wall clock a human reads). The product
// name arrives via /scan JSON and lands with textContent, like the SSIDs —
// nothing from the radio or the config is ever parsed as HTML.
const char PORTAL_HTML[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<title>Set up your Canary</title>
<style>
:root{--bg:#000;--card:#141414;--edge:#2a2a2a;--txt:#f2f2f2;--mut:#9a9a9a;
--faint:#5c5c5c;--ok:#3fcf8e;--warn:#e0a83c;--r:14px}
*{margin:0;padding:0;box-sizing:border-box;-webkit-tap-highlight-color:transparent}
body{background:var(--bg);color:var(--txt);min-height:100vh;display:flex;
justify-content:center;padding:max(20px,env(safe-area-inset-top)) 16px 32px;
font:16px/1.45 -apple-system,BlinkMacSystemFont,system-ui,'Segoe UI',Roboto,sans-serif}
main{width:100%;max-width:420px}
.eyebrow{font-size:11px;letter-spacing:.24em;color:var(--faint);text-transform:uppercase;margin:8px 2px 6px}
h1{font-size:26px;font-weight:600;letter-spacing:-.02em;margin-bottom:4px}
.sub{color:var(--mut);font-size:15px;margin-bottom:22px}
.card{background:var(--card);border:1px solid var(--edge);border-radius:var(--r);overflow:hidden}
.row{display:flex;align-items:center;gap:12px;padding:15px 16px;border-bottom:1px solid var(--edge);
cursor:pointer;transition:background .18s}
.row:last-child{border-bottom:0}.row:active{background:#1d1d1d}
.row .name{flex:1;font-size:16px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.lock{color:var(--faint);font-size:13px}
.bars{display:flex;align-items:flex-end;gap:2px;height:14px}
.bars i{width:3.5px;background:var(--faint);border-radius:1px}
.bars i.on{background:var(--txt)}
.bars i:nth-child(1){height:5px}.bars i:nth-child(2){height:9px}.bars i:nth-child(3){height:14px}
.skl .name{background:#1e1e1e;border-radius:6px;height:14px;max-width:56%;
animation:sh 1.4s ease-in-out infinite}
@keyframes sh{0%,100%{opacity:.45}50%{opacity:1}}
.other{color:var(--mut);font-size:15px}
#sheet{margin-top:14px;background:var(--card);border:1px solid var(--edge);
border-radius:var(--r);padding:18px 16px;display:none}
#sheet.open{display:block;animation:up .24s ease-out}
@keyframes up{from{transform:translateY(10px);opacity:0}to{transform:none;opacity:1}}
@keyframes shake{0%,100%{transform:none}25%{transform:translateX(-6px)}75%{transform:translateX(6px)}}
#sheet.err{animation:shake .16s ease-in-out 2}
#picked{font-weight:600;margin-bottom:12px;font-size:17px}
.field{display:flex;align-items:center;background:#0c0c0c;border:1px solid var(--edge);
border-radius:10px;padding:2px 4px 2px 14px;margin-bottom:8px}
.field input{flex:1;background:none;border:0;outline:0;color:var(--txt);
font-size:16px;padding:11px 0}
.pw-masked{-webkit-text-security:disc;text-security:disc}
.field button{background:none;border:0;color:var(--mut);font-size:13px;padding:10px 12px;cursor:pointer}
#msg{min-height:20px;font-size:13.5px;color:var(--warn);margin:2px 2px 8px}
#join{width:100%;padding:14px;border:0;border-radius:10px;background:var(--txt);
color:#000;font-size:16px;font-weight:600;cursor:pointer;transition:opacity .18s}
#join:disabled{opacity:.55}
.spin{display:inline-block;width:15px;height:15px;border:2px solid #0003;
border-top-color:#000;border-radius:50%;vertical-align:-2px;margin-right:8px;
animation:rot .8s linear infinite}
@keyframes rot{to{transform:rotate(360deg)}}
#done{display:none;text-align:center;padding:34px 18px}
#done.open{display:block;animation:up .3s ease-out}
.check{width:64px;height:64px;margin:0 auto 18px}
.check circle{fill:none;stroke:var(--ok);stroke-width:2.5;stroke-dasharray:190;
stroke-dashoffset:190;animation:draw .5s ease-out forwards}
.check path{fill:none;stroke:var(--ok);stroke-width:3;stroke-linecap:round;
stroke-linejoin:round;stroke-dasharray:44;stroke-dashoffset:44;
animation:draw .35s .4s ease-out forwards}
@keyframes draw{to{stroke-dashoffset:0}}
#done h2{font-size:21px;font-weight:600;margin-bottom:8px}
#done p{color:var(--mut);font-size:15px}
.rescan{display:block;margin:16px auto 0;background:none;border:0;color:var(--faint);
font-size:13px;cursor:pointer;padding:6px}
footer{margin-top:26px;text-align:center;color:var(--faint);font-size:12px}
</style></head><body><main>
<div class="eyebrow">SecuraCV</div>
<h1 id="ttl">Connect your Canary</h1>
<p class="sub">Pick your home network.</p>
<div class="card" id="nets"></div>
<button class="rescan" id="rescan">Scan again</button>
<div id="sheet">
  <div id="picked"></div>
  <div class="field" id="ssidf" style="display:none">
    <input id="ssid" placeholder="Network name" autocapitalize="none" autocorrect="off">
  </div>
  <div class="field">
    <input id="pw" type="text" class="pw-masked" placeholder="Password" autocomplete="off" autocapitalize="none" autocorrect="off" spellcheck="false">
    <button id="eye" type="button">show</button>
  </div>
  <div id="msg"></div>
  <button id="join">Join</button>
</div>
<div id="done">
  <svg class="check" viewBox="0 0 64 64"><circle cx="32" cy="32" r="30"/>
  <path d="M20 33 L29 42 L45 24"/></svg>
  <h2>You're all set</h2>
  <p>It's joining your network now — the light and your fleet take it from
  here. Your phone will drop off this setup network by itself.</p>
</div>
<footer>No account · no cloud · your password goes only to this device</footer>
</main><script>
var sel=null,busy=false;
function $(i){return document.getElementById(i)}
function bars(r){var n=r>-60?3:r>-72?2:1,h='<span class="bars">';
for(var i=1;i<=3;i++)h+='<i'+(i<=n?' class="on"':'')+'></i>';return h+'</span>'}
function skeleton(){var h='';for(var i=0;i<3;i++)
h+='<div class="row skl"><div class="name"></div></div>';$('nets').innerHTML=h}
function renderNets(list){var c=$('nets');c.innerHTML='';
list.forEach(function(n){var d=document.createElement('div');d.className='row';
var nm=document.createElement('div');nm.className='name';nm.textContent=n.ssid;
d.appendChild(nm);
if(n.secure){var lk=document.createElement('span');lk.className='lock';
lk.textContent='🔒';d.appendChild(lk)}
d.insertAdjacentHTML('beforeend',bars(n.rssi));
d.onclick=function(){pick(n.ssid,false)};c.appendChild(d)});
var o=document.createElement('div');o.className='row';
o.innerHTML='<div class="name other">Join another network…</div>';
o.onclick=function(){pick('',true)};c.appendChild(o)}
function pick(ssid,manual){if(busy)return;sel=ssid;
$('ssidf').style.display=manual?'flex':'none';
$('picked').textContent=manual?'Other network':ssid;
$('msg').textContent='';$('pw').value='';
var s=$('sheet');s.classList.remove('err');s.classList.add('open');
(manual?$('ssid'):$('pw')).focus()}
function scan(force){skeleton();
fetch('/scan'+(force?'?force=1':'')).then(function(r){return r.json()})
.then(function(j){if(j.scanning){setTimeout(function(){scan(false)},900);return}
if(j.product)$('ttl').textContent='Connect your '+j.product;
renderNets(j.networks||[])})
.catch(function(){setTimeout(function(){scan(false)},1200)})}
$('rescan').onclick=function(){if(!busy)scan(true)};
$('eye').onclick=function(){var p=$('pw'),t=p.classList.contains('pw-masked');
p.classList.toggle('pw-masked',!t);$('eye').textContent=t?'hide':'show'};
$('join').onclick=function(){if(busy)return;
var ssid=$('ssidf').style.display!=='none'?$('ssid').value.trim():sel;
if(!ssid){$('msg').textContent='Enter the network name.';return}
busy=true;var b=$('join');b.disabled=true;
b.innerHTML='<span class="spin"></span>Joining…';$('msg').textContent='';
var body='ssid='+encodeURIComponent(ssid)+'&pass='+encodeURIComponent($('pw').value);
fetch('/join',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body})
.then(function(){poll()})
.catch(function(){fail('Lost the device — rejoin its setup network and retry.')})};
function poll(){fetch('/status').then(function(r){return r.json()})
.then(function(j){if(j.state==='success'){win()}
else if(j.state==='fail'){fail(j.reason||'Could not connect.')}
else setTimeout(poll,900)})
.catch(function(){setTimeout(poll,1200)})}
function tip(m){if(/password/i.test(m))return" — check for typos (it is case-sensitive)";
if(/not found/i.test(m))return" — note: this Canary only sees 2.4 GHz WiFi, so a 5 GHz-only network is invisible to it";
if(/connect/i.test(m))return" — try moving it closer to your router, then retry";
return""}
function fail(m){busy=false;var b=$('join');b.disabled=false;b.textContent='Join';
$('msg').textContent=m+tip(m);var s=$('sheet');s.classList.remove('err');
void s.offsetWidth;s.classList.add('err');$('pw').value='';$('pw').focus()}
function win(){$('sheet').classList.remove('open');
$('nets').style.display='none';$('rescan').style.display='none';
document.querySelector('.sub').style.display='none';$('done').classList.add('open')}
scan(false);
</script></body></html>)HTML";

// ── HTTP handlers ─────────────────────────────────────────────────────────

void send_portal() {
  g->portal_seen = true;
  // no-store: the captive sheet caches aggressively, and a cached copy from
  // a previous (aborted) session renders stale scan lists.
  g->server.sendHeader("Cache-Control", "no-store");
  g->server.send_P(200, "text/html", PORTAL_HTML);
}

void harvest_scan(int n) {
  g->scan_n = 0;
  for (int i = 0; i < n && g->scan_n < SCAN_MAX; i++) {
    const String ssid = WiFi.SSID(i);
    if (ssid.length() == 0) continue;
    // Dedupe repeated SSIDs (mesh APs): keep the strongest.
    bool dup = false;
    for (int k = 0; k < g->scan_n; k++) {
      if (ssid == g->scan[k].ssid) {
        if (WiFi.RSSI(i) > g->scan[k].rssi) g->scan[k].rssi = WiFi.RSSI(i);
        dup = true;
        break;
      }
    }
    if (dup) continue;
    ScanRow& r = g->scan[g->scan_n++];
    snprintf(r.ssid, sizeof(r.ssid), "%s", ssid.c_str());
    r.rssi = WiFi.RSSI(i);
    r.secure = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
  }
  // Strongest first — the user's own network is almost always on top.
  for (int i = 1; i < g->scan_n; i++) {
    for (int k = i; k > 0 && g->scan[k].rssi > g->scan[k - 1].rssi; k--) {
      ScanRow tmp = g->scan[k]; g->scan[k] = g->scan[k - 1]; g->scan[k - 1] = tmp;
    }
  }
  g->scan_at = millis();
  WiFi.scanDelete();
}

void send_scan_json() {
  // Worst-case escaped SSID: 32 chars, all control bytes -> 32*6 = 192 + NUL.
  // Undersizing silently omits that network from the list (display review
  // catch, kept).
  char row[256], esc[200];
  String out = "{\"networks\":[";
  for (int i = 0; i < g->scan_n; i++) {
    if (json_escape(g->scan[i].ssid, esc, sizeof(esc)) == 0 &&
        g->scan[i].ssid[0] != '\0') continue;
    snprintf(row, sizeof(row), "%s{\"ssid\":\"%s\",\"rssi\":%ld,\"secure\":%s}",
             i ? "," : "", esc, (long)g->scan[i].rssi,
             g->scan[i].secure ? "true" : "false");
    out += row;
  }
  out += "]";
  if (g->product_json[0] != '\0') {
    out += ",\"product\":\"";
    out += g->product_json;
    out += "\"";
  }
  out += "}";
  g->server.send(200, "application/json", out);
}

void handle_scan() {
  const int rc = WiFi.scanComplete();
  if (rc == WIFI_SCAN_RUNNING) {
    g->server.send(200, "application/json", "{\"scanning\":true}");
    return;
  }
  if (rc >= 0) harvest_scan(rc);
  const bool force = g->server.hasArg("force");
  if ((force || portal_scan_stale(millis(), g->scan_at, g->timing)) &&
      g->st != St::Testing) {
    // Never sweep during a join — a live scan handle can make WiFi.begin()
    // silently fail to associate (WAP lesson).
    WiFi.scanNetworks(/*async=*/true, /*hidden=*/false, /*passive=*/false, 300);
    g->server.send(200, "application/json", "{\"scanning\":true}");
    return;
  }
  send_scan_json();
}

void handle_join() {
  const String ssid = g->server.arg("ssid");
  const String pass = g->server.arg("pass");
  if (ssid.length() == 0 || ssid.length() > 32 || pass.length() > 64) {
    g->server.send(400, "application/json",
                   "{\"ok\":false,\"reason\":\"bad request\"}");
    return;
  }
  snprintf(g->join_ssid, sizeof(g->join_ssid), "%s", ssid.c_str());
  snprintf(g->join_pass, sizeof(g->join_pass), "%s", pass.c_str());
  g->fail_reason[0] = '\0';

  // WAP lessons, in order: clear ANY scan handle — running OR completed-but-
  // unharvested — clear stale STA state, then a non-blocking begin. The AP
  // stays up (AP_STA) so the phone keeps its connection and can poll /status
  // for the verdict. A phone join preempts any quiet background retry.
  {
    const int rc = WiFi.scanComplete();
    if (rc >= 0) harvest_scan(rc);        // caches + scanDelete()s
    else if (rc == WIFI_SCAN_RUNNING) WiFi.scanDelete();
  }
  WiFi.persistent(false);
  WiFi.disconnect(/*wifioff=*/false, /*eraseap=*/false);
  WiFi.begin(g->join_ssid, g->join_pass);
  Serial.printf("[SETUP] Join requested: \"%s\"\n", g->join_ssid);

  g->bg_join = false;
  enter(St::Testing, millis());
  g->server.send(200, "application/json", "{\"ok\":true}");
}

void handle_status() {
  char esc[96];
  char body[160];
  switch (g->st) {
    case St::Testing:
      snprintf(body, sizeof(body), "{\"state\":\"connecting\"}");
      break;
    case St::Success:
      // The phone has SEEN the verdict — start the linger beat from THIS
      // moment (display review catch: a poll delayed by the STA/AP channel
      // switch would otherwise tear the AP down in the same pass that sends
      // this response, re-creating the race).
      if (!g->phone_acked) {
        g->phone_acked = true;
        g->acked_at = millis();
      }
      snprintf(body, sizeof(body), "{\"state\":\"success\"}");
      break;
    case St::Fail:
      if (json_escape(g->fail_reason, esc, sizeof(esc)) == 0) esc[0] = '\0';
      snprintf(body, sizeof(body), "{\"state\":\"fail\",\"reason\":\"%s\"}", esc);
      break;
    default:
      snprintf(body, sizeof(body), "{\"state\":\"idle\"}");
      break;
  }
  g->server.send(200, "application/json", body);
}

void handle_not_found() {
  const String uri = g->server.uri();
  switch (classify_probe(uri.c_str())) {
    case Probe::Android204:
      g->server.send(204, "text/plain", "");
      return;
    case Probe::WindowsNcsi:
      g->server.send(200, "text/plain", "Microsoft NCSI");
      return;
    case Probe::WindowsConnect:
      g->server.send(200, "text/plain", "Microsoft Connect Test");
      return;
    case Probe::ApplePortal:
      // NOT Apple's Success token, so the Captive Network Assistant pops —
      // but via REDIRECT, not the page inline: serving the portal on
      // captive.apple.com rendered a blank sheet on a real iPhone (display
      // 4.3B bench); redirect-to-own-IP is the battle-tested CNA flow.
      break;
    case Probe::None:
      break;
  }
  g->server.sendHeader("Location", "http://192.168.4.1/", true);
  g->server.send(302, "text/plain", "");
}

// A-only / NODATA captive DNS (see provision_core.h for why), draining a
// burst per pass — iPhones fire several queries at once and impatient
// retries stack resolution latency onto the captive sheet.
void dns_pump() {
  for (int i = 0; i < 8; i++) {
    const int len = g->dns.parsePacket();
    if (len <= 0) return;
    uint8_t query[512];
    const int got = g->dns.read(query, sizeof(query));
    if (got < 12) continue;
    uint8_t resp[560];
    const IPAddress ap_ip = WiFi.softAPIP();
    const uint8_t ip[4] = {ap_ip[0], ap_ip[1], ap_ip[2], ap_ip[3]};
    const size_t n =
        dns_build_response(query, (size_t)got, ip, resp, sizeof(resp));
    if (n == 0) continue;
    g->dns.beginPacket(g->dns.remoteIP(), g->dns.remotePort());
    g->dns.write(resp, n);
    g->dns.endPacket();
  }
}

JoinFailure classify_status(wl_status_t st) {
  switch (st) {
    case WL_NO_SSID_AVAIL:  return JoinFailure::NotFound;
    case WL_CONNECT_FAILED: return JoinFailure::BadPassword;
    case WL_IDLE_STATUS:    return JoinFailure::NoAddress;
    default:                return JoinFailure::Unknown;
  }
}

// ── The setup AP's password: minted once, then STABLE for this unit ───────
// A phone remembers the SSID/password pair, not the key it was last shown;
// re-rolling the password under a stable SSID locks every phone that ever
// joined out with no prompt to correct it (the display's 2.4.9 iPhone
// lockout). So it persists in NVS beside the credentials the wizard writes,
// and when the store will not hold it, the SSID gets a per-session suffix so
// the pair stays consistent (a name the phone has never seen just prompts).
bool ap_pass_ok(const char* s) {
  if (s == nullptr || strlen(s) != AP_PASS_LEN) return false;
  for (size_t i = 0; i < AP_PASS_LEN; i++) {
    const unsigned char c = (unsigned char)s[i];
    if (c < 0x21 || c > 0x7e) return false;  // printable, no spaces
  }
  return true;
}

void mint_ap_password(char* out, size_t out_len) {
  uint8_t rnd[16];
  esp_fill_random(rnd, sizeof(rnd));
  render_password(rnd, sizeof(rnd), out, out_len);
}

bool load_or_mint_ap_password(char* out, size_t out_len) {
  Preferences prefs;
  if (!prefs.begin("securacv", /*readOnly=*/false)) {
    mint_ap_password(out, out_len);
    Serial.println("[SETUP] Settings store unavailable - setup password is per-boot.");
    return false;
  }
  const String stored = prefs.getString("ap_pass", "");
  if (ap_pass_ok(stored.c_str())) {
    snprintf(out, out_len, "%s", stored.c_str());
    prefs.end();
    return true;
  }
  mint_ap_password(out, out_len);
  // Trust the readback, not the call: putString reports a failed write by
  // returning 0, and a write that "succeeded" but does not stick fails the
  // same way for the user (the next boot advertises a key it no longer has).
  const bool wrote = prefs.putString("ap_pass", out) > 0 &&
                     prefs.getString("ap_pass", "") == out;
  prefs.end();
  if (!wrote) {
    Serial.println("[SETUP] Setup password could not be saved - per-boot this time.");
  }
  return wrote;
}

void teardown(bool joined) {
  g->server.stop();
  g->dns.stop();
  WiFi.softAPdisconnect(/*wifioff=*/true);
  WiFi.mode(WIFI_STA);
  if (joined) s_joined_latch = true;
  Serial.printf("[SETUP] Portal closed%s.\n",
                joined ? " - device is on the network" : "");
  delete g;
  g = nullptr;
}

}  // namespace

bool setup_portal_begin(const SetupPortalConfig& cfg) {
  if (g != nullptr) return true;  // idempotent while active

  g = new Ctx();
  g->cfg = cfg;
  {
    char esc[sizeof(g->product_json)];
    const char* name = cfg.product_name ? cfg.product_name : "Canary";
    if (json_escape(name, esc, sizeof(esc)) > 0) {
      snprintf(g->product_json, sizeof(g->product_json), "%s", esc);
    }
  }

  char ap_pass[AP_PASS_LEN + 1];
  const bool pass_durable = load_or_mint_ap_password(ap_pass, sizeof(ap_pass));

  // The name and the key are one promise, so they keep the same lifetime
  // (see the mint note above).
  const char* suffix =
      (cfg.id_suffix && cfg.id_suffix[0]) ? cfg.id_suffix : "CNRY";
  char ap_ssid[24];
  if (pass_durable) {
    snprintf(ap_ssid, sizeof(ap_ssid), "SecuraCV-%.4s", suffix);
  } else {
    uint8_t tag_rnd[8];
    esp_fill_random(tag_rnd, sizeof(tag_rnd));
    char tag[3];
    render_password(tag_rnd, sizeof(tag_rnd), tag, sizeof(tag));
    snprintf(ap_ssid, sizeof(ap_ssid), "SecuraCV-%.4s-%s", suffix, tag);
  }

  // persistent(false) BEFORE the first radio call: with Arduino persistence
  // on (the default), every mode change and softAP() commits esp_wifi config
  // to its own NVS store — which this fleet never reads (credentials live in
  // the "securacv" namespace) and which double-writes flash for nothing.
  WiFi.persistent(false);

  // No pre-AP sweep here, unlike the display portal: this loop() cannot block
  // for a 13-channel sweep (the adopters keep sensing while the portal is
  // up), and the dangerous thing was never scanning under AP_STA — it was a
  // LIVE SCAN HANDLE crossing the mode flip. So: make sure no handle exists,
  // flip, raise, and let the wizard's first /scan run the async sweep (the
  // page shows its skeleton shimmer for the ~2 s that costs).
  if (WiFi.scanComplete() == WIFI_SCAN_RUNNING) WiFi.scanDelete();
  WiFi.mode(WIFI_AP_STA);
  const uint8_t channel = cfg.ap_channel ? cfg.ap_channel : 1;
  if (!WiFi.softAP(ap_ssid, ap_pass, channel, /*hidden=*/0, /*max_conn=*/1)) {
    Serial.println("[SETUP] Setup network failed to start.");
    delete g;
    g = nullptr;
    return false;
  }

  Serial.printf("[SETUP] Setup network up: \"%s\"  password %s  (channel %u)\n",
                ap_ssid, ap_pass, (unsigned)channel);
  Serial.println("[SETUP] Join it from a phone - the setup page opens by itself"
                 " (or open http://192.168.4.1/).");

  g->dns.begin(53);
  g->server.on("/", HTTP_GET, send_portal);
  g->server.on("/scan", HTTP_GET, handle_scan);
  g->server.on("/join", HTTP_POST, handle_join);
  g->server.on("/status", HTTP_GET, handle_status);
  g->server.onNotFound(handle_not_found);
  g->server.begin();

  const uint32_t now = millis();
  g->bg_last_try = now;  // full cadence before the first quiet retry
  enter(St::Waiting, now);
  return true;
}

void setup_portal_loop(uint32_t now_ms) {
  if (g == nullptr) return;

  dns_pump();
  g->server.handleClient();

  const int stations = WiFi.softAPgetStationNum();
  if (stations > 0) g->saw_station = true;

  switch (g->st) {
    case St::Waiting:
      if (WiFi.status() == WL_CONNECTED) {
        // A join begun BEFORE the portal opened completed underneath us
        // (recovery raise racing an in-flight association). Route it through
        // the Testing success path as a quiet rejoin — same logging, same
        // short linger, same teardown.
        g->bg_join = true;
        enter(St::Testing, now_ms);
        break;
      }
      if (portal_stuck_hint_due(now_ms, g->st_since, g->saw_station,
                                g->stuck_hinted, g->timing)) {
        // A phone auto-rejoining with a stale saved password cannot be
        // detected from this side; name the one move that clears it.
        g->stuck_hinted = true;
        Serial.println("[SETUP] Nothing has joined the setup network yet. If a"
                       " phone refuses with no password prompt, forget this"
                       " network on the phone, then scan again.");
      }
      if (portal_background_retry_due(now_ms, g->cfg.have_saved_credentials,
                                      stations, /*join_in_flight=*/false,
                                      g->bg_last_try, g->timing) &&
          g->cfg.begin_saved != nullptr) {
        // Recovery: the saved network may simply be back (a router reboot).
        // Quiet retry underneath the empty portal; a success closes it with
        // no human involved.
        g->bg_last_try = now_ms;
        g->bg_join = true;
        WiFi.disconnect(/*wifioff=*/false, /*eraseap=*/false);
        g->cfg.begin_saved();
        enter(St::Testing, now_ms);
      }
      break;

    case St::Testing: {
      const wl_status_t ws = WiFi.status();
      if (ws == WL_CONNECTED) {
        if (!g->bg_join) {
          // Persist ONLY on success — a failed attempt must never leave
          // broken credentials in NVS (power-cycle restarts clean).
          if (g->cfg.save_credentials != nullptr &&
              !g->cfg.save_credentials(g->join_ssid, g->join_pass)) {
            Serial.println("[SETUP] Store refused the credentials - joined for"
                           " this boot; setup will ask again next boot.");
          }
          Serial.printf("[SETUP] Joined \"%s\"  IP=%s\n", g->join_ssid,
                        WiFi.localIP().toString().c_str());
        } else {
          Serial.printf("[SETUP] Saved network is back  IP=%s\n",
                        WiFi.localIP().toString().c_str());
        }
        g->success_at = now_ms;
        // A background rejoin has no phone to ack: pre-ack so only the short
        // beat runs before teardown (see setup_portal_logic.h).
        g->phone_acked = g->bg_join;
        g->acked_at = now_ms;
        enter(St::Success, now_ms);
      } else if (ws == WL_NO_SSID_AVAIL || ws == WL_CONNECT_FAILED ||
                 (int32_t)(now_ms - g->st_since) >
                     (int32_t)g->timing.sta_timeout_ms) {
        const JoinFailure f = classify_status(ws);
        snprintf(g->fail_reason, sizeof(g->fail_reason), "%s",
                 join_failure_label(f));
        WiFi.disconnect(/*wifioff=*/false, /*eraseap=*/false);
        if (g->bg_join) {
          // Quiet retry stays quiet: back to waiting, cadence intact.
          g->bg_join = false;
          g->fail_reason[0] = '\0';
          enter(St::Waiting, now_ms);
        } else {
          Serial.printf("[SETUP] %s\n", join_failure_detail(f));
          enter(St::Fail, now_ms);
        }
      }
      break;
    }

    case St::Fail:
      // Recovery is phone-side: the portal shows the reason inline and the
      // form re-arms; a new POST /join returns to Testing. If the phone
      // dropped off entirely, fall back to waiting (background retry
      // eligible again).
      if (stations == 0) enter(St::Waiting, now_ms);
      break;

    case St::Success:
      if (portal_teardown_due(now_ms, g->success_at, g->phone_acked,
                              g->acked_at, g->timing)) {
        teardown(/*joined=*/true);
        return;
      }
      break;
  }
}

bool setup_portal_active() { return g != nullptr; }

bool setup_portal_join_in_flight() { return g != nullptr && g->st == St::Testing; }

bool setup_portal_take_joined() {
  const bool j = s_joined_latch;
  s_joined_latch = false;
  return j;
}

void setup_portal_stop() {
  if (g == nullptr) return;
  teardown(/*joined=*/false);
}

}  // namespace net
}  // namespace canary
