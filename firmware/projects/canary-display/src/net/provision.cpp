// src/net/provision.cpp — first-boot onboarding: SoftAP + captive portal.
//
// See provision.h for the flow and the no-dead-ends contract. Everything
// stateful lives inside provision_run()'s scope so a provisioned display
// carries zero onboarding baggage at steady state — the WebServer, the DNS
// socket, and the scan cache all free on return.
#include <config.h>

// Library includes live ABOVE the feature gate on purpose: PlatformIO's LDF
// (deep+ mode) evaluates preprocessor conditionals using build flags only —
// FEATURE_ONBOARDING is defined in the flavor config HEADER, which the LDF
// can't see, so a gated include would leave the bundled WebServer library
// unregistered and the build failing on WebServer.h (the LittleFS lesson,
// again). Unconditional includes cost nothing when the feature is off — the
// linker drops the unused objects.
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <WebServer.h>

#if defined(FEATURE_ONBOARDING) && FEATURE_ONBOARDING

#include <esp_random.h>
#include <lvgl.h>

#include "canary/net/provision.h"
#include "canary/net/provision_core.h"
#include "canary/runtime_config.h"
#include "canary/hal/display.h"
#include "canary/ui/onboard_ui.h"
#include "canary/log.h"
#include "identity/device_pseudonym.h"

namespace canary::net {

namespace {

// ── Choreography (see onboard_ui.h for the motion budget) ────────────────
constexpr uint32_t HELLO_MS          = 2600;   // welcome beat before the QR
constexpr uint32_t HINT_AFTER_MS     = 4000;   // phone joined, portal never hit
constexpr uint32_t STA_TIMEOUT_MS    = 30000;  // WAP lesson: 15 s reads as
                                               // "wrong password" at range edge
constexpr uint32_t AP_LINGER_MAX_MS  = 25000;  // success: wait for phone's ack…
constexpr uint32_t AP_LINGER_ACK_MS  = 1600;   // …then this beat, then teardown
constexpr uint32_t IDLE_DIM_MS       = 480000; // 8 min unattended -> dim glass
constexpr uint32_t SCAN_TTL_MS       = 300000; // scan cache freshness (5 min)

enum class St : uint8_t { Hello, Waiting, PhoneHere, Testing, Fail, Success };

// False when the panel failed to init: every LVGL touchpoint no-ops and the
// wizard runs serial-guided (the portal itself is glass-independent).
bool s_glass = false;

void ui_stage(canary::ui::ObStage st, const char* detail) {
  if (s_glass) canary::ui::onboard_ui_stage(st, detail);
}
void ui_hint(const char* line) {
  if (s_glass) canary::ui::onboard_ui_hint(line);
}
void ui_pump() {
  if (s_glass) lv_timer_handler();
}

struct ScanRow { char ssid[33]; int32_t rssi; bool secure; };
constexpr int SCAN_MAX = 20;

struct Ctx {
  WebServer server{80};
  WiFiUDP dns;
  St st = St::Hello;
  uint32_t st_since = 0;
  bool portal_seen = false;      // any GET / — the captive sheet actually popped
  bool phone_acked = false;      // /status poll observed our success
  uint32_t acked_at = 0;         // when that first success poll landed
  uint32_t success_at = 0;
  char join_ssid[33] = {0};
  char join_pass[65] = {0};
  char fail_reason[48] = {0};
  ScanRow scan[SCAN_MAX];
  int scan_n = 0;
  uint32_t scan_at = 0;          // millis of last completed sweep (0 = never)
  bool dimmed = false;
};
Ctx* g = nullptr;                // scoped to provision_run(); never steady-state

void enter(St st, uint32_t now) { g->st = st; g->st_since = now; }

// ── The portal page ───────────────────────────────────────────────────────
// Self-contained (the phone has no internet on this AP): system font stack,
// Quiet Glass dark palette, no frameworks. The network list arrives as JSON
// and lands via textContent — a hostile SSID stays inert text.
const char PORTAL_HTML[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<title>Set up your display</title>
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
<h1>Connect your display</h1>
<p class="sub">Pick your home network.</p>
<div class="card" id="nets"></div>
<button class="rescan" id="rescan">Scan again</button>
<div id="sheet">
  <div id="picked"></div>
  <div class="field" id="ssidf" style="display:none">
    <input id="ssid" placeholder="Network name" autocapitalize="none" autocorrect="off">
  </div>
  <div class="field">
    <input id="pw" type="password" placeholder="Password" autocapitalize="none" autocorrect="off">
    <button id="eye" type="button">show</button>
  </div>
  <div id="msg"></div>
  <button id="join">Join</button>
</div>
<div id="done">
  <svg class="check" viewBox="0 0 64 64"><circle cx="32" cy="32" r="30"/>
  <path d="M20 33 L29 42 L45 24"/></svg>
  <h2>You're all set</h2>
  <p>The display is joining your canaries now — watch it take it from here.
  Your phone will drop off this setup network by itself.</p>
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
renderNets(j.networks||[])})
.catch(function(){setTimeout(function(){scan(false)},1200)})}
$('rescan').onclick=function(){if(!busy)scan(true)};
$('eye').onclick=function(){var p=$('pw'),t=p.type==='password';
p.type=t?'text':'password';$('eye').textContent=t?'hide':'show'};
$('join').onclick=function(){if(busy)return;
var ssid=$('ssidf').style.display!=='none'?$('ssid').value.trim():sel;
if(!ssid){$('msg').textContent='Enter the network name.';return}
busy=true;var b=$('join');b.disabled=true;
b.innerHTML='<span class="spin"></span>Joining…';$('msg').textContent='';
var body='ssid='+encodeURIComponent(ssid)+'&pass='+encodeURIComponent($('pw').value);
fetch('/join',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body})
.then(function(){poll()})
.catch(function(){fail('Lost the display — rejoin its network and retry.')})};
function poll(){fetch('/status').then(function(r){return r.json()})
.then(function(j){if(j.state==='success'){win()}
else if(j.state==='fail'){fail(j.reason||'Could not connect.')}
else setTimeout(poll,900)})
.catch(function(){setTimeout(poll,1200)})}
function fail(m){busy=false;var b=$('join');b.disabled=false;b.textContent='Join';
$('msg').textContent=m;var s=$('sheet');s.classList.remove('err');
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
  // a previous (aborted) onboarding session renders stale scan lists.
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
  // Undersizing here silently omits that network from the list (review catch).
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
  out += "]}";
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
  const bool stale = (g->scan_at == 0) ||
                     ((int32_t)(millis() - g->scan_at) > (int32_t)SCAN_TTL_MS);
  if ((force || stale) && g->st != St::Testing) {
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
  // unharvested (leftover results alive at begin() cause silent association
  // failures; review catch) — clear stale STA state, then a non-blocking
  // begin. Harvesting a completed sweep keeps the cache fresh for free.
  // The AP stays up (AP_STA) so the phone keeps its connection and can poll
  // /status for the verdict.
  {
    const int rc = WiFi.scanComplete();
    if (rc >= 0) harvest_scan(rc);        // caches + scanDelete()s
    else if (rc == WIFI_SCAN_RUNNING) WiFi.scanDelete();
  }
  WiFi.persistent(false);
  WiFi.disconnect(/*wifioff=*/false, /*eraseap=*/false);
  WiFi.begin(g->join_ssid, g->join_pass);

  enter(St::Testing, millis());
  ui_stage(canary::ui::ObStage::Connecting, g->join_ssid);
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
      // moment, not from entering Success (review catch: a poll delayed by
      // the STA/AP channel switch would otherwise tear the AP down in the
      // same pass that sends this response, re-creating the race).
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
      // Deliberately NOT Apple's Success token, so the Captive Network
      // Assistant sheet pops — but answer with a REDIRECT, not the page
      // itself: serving the portal inline on captive.apple.com rendered a
      // blank sheet on a real iPhone (4.3B bench), while redirect-to-own-IP
      // is the battle-tested CNA flow — the sheet lands on our actual
      // origin, so the page and its /scan /join /status fetches all agree.
      break;
    case Probe::None:
      break;
  }
  // Anything else: send the browser home.
  g->server.sendHeader("Location", "http://192.168.4.1/", true);
  g->server.send(302, "text/plain", "");
}

// A-only / NODATA captive DNS (see provision_core.h for why).
void dns_pump() {
  // Drain the queue, not one packet: iPhones burst several queries at once
  // (A + HTTPS types, plus impatient retries), and each wizard pass also
  // pays for a full LVGL frame — one-answer-per-pass stacked seconds of
  // resolution latency onto the captive sheet (bench: it sat blank).
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

const char* sta_failure_reason(wl_status_t st) {
  switch (st) {
    case WL_NO_SSID_AVAIL: return "Network not found";
    case WL_CONNECT_FAILED: return "Wrong password";
    default: return "Couldn't connect";
  }
}

}  // namespace

bool provision_needed() {
  return canary::cfg::wifi_is_placeholder();
}

void provision_run(bool glass_ok) {
  Ctx ctx;
  g = &ctx;
  s_glass = glass_ok;

  // Device-unique setup identity: SSID suffix from the salted, MAC-free
  // pseudonym (stable, matches the fleet's SecuraCV-XXXX convention);
  // password random per session — it is DISPLAYED on the glass and inside
  // the join QR, so ephemeral beats memorable, and nothing derivable from
  // published identifiers ever gates the AP.
  char token[device_pseudonym::HEX_LEN + 1] = {0};
  device_pseudonym::device_id_hex(token, sizeof(token));
  char ap_ssid[20];
  snprintf(ap_ssid, sizeof(ap_ssid), "SecuraCV-%.4s", token[0] ? token : "GLAS");
  uint8_t rnd[16];
  esp_fill_random(rnd, sizeof(rnd));
  char ap_pass[9];
  render_password(rnd, sizeof(rnd), ap_pass, sizeof(ap_pass));

  log_header("SETUP");
  canary::dbg_serial().printf("First boot - onboarding AP \"%s\"  password %s\n",
                              ap_ssid, ap_pass);
  canary::dbg_serial().println("Portal at http://192.168.4.1/ once joined.");

  // Pre-AP scan (WAP lesson): sweep BEFORE any phone can attach, so the
  // first portal load gets an instant list. It runs behind the Hello scene —
  // the welcome beat and the radio sweep share the same ~2.5 s.
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  WiFi.scanNetworks(/*async=*/true, /*hidden=*/false, /*passive=*/false, 300);

  if (s_glass) canary::ui::onboard_ui_create(ap_ssid, ap_pass);

  const uint32_t t0 = millis();
  while ((int32_t)(millis() - t0) < (int32_t)HELLO_MS) {
    ui_pump();
    delay(5);
  }
  const int pre = WiFi.scanComplete();
  if (pre >= 0) harvest_scan(pre);

  // Raise the AP. Channel 1, max 1 client (hardening), WPA2.
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(ap_ssid, ap_pass, /*channel=*/1, /*hidden=*/0, /*max_conn=*/1);
  ctx.dns.begin(53);

  ctx.server.on("/", HTTP_GET, send_portal);
  ctx.server.on("/scan", HTTP_GET, handle_scan);
  ctx.server.on("/join", HTTP_POST, handle_join);
  ctx.server.on("/status", HTTP_GET, handle_status);
  ctx.server.onNotFound(handle_not_found);
  ctx.server.begin();

  enter(St::Waiting, millis());
  ui_stage(canary::ui::ObStage::Join, nullptr);

  for (;;) {
    const uint32_t now = millis();
    dns_pump();
    ctx.server.handleClient();

    const int stations = WiFi.softAPgetStationNum();

    switch (ctx.st) {
      case St::Hello:
        break;  // unreachable (Hello ran above); kept for enum completeness

      case St::Waiting:
        if (stations > 0) {
          enter(St::PhoneHere, now);
          ui_stage(canary::ui::ObStage::PhoneJoined, nullptr);
          if (ctx.dimmed && s_glass) {
            canary::hal::backlight_set(CD_BRIGHT_DAY);
            ctx.dimmed = false;
          }
        } else if (s_glass && !ctx.dimmed &&
                   (int32_t)(now - ctx.st_since) > (int32_t)IDLE_DIM_MS) {
          // Unattended for a long while: politeness dim. Any phone joining
          // (or a touch) restores full brightness — never a dead end.
          canary::hal::backlight_set(CD_BRIGHT_AMBIENT);
          ctx.dimmed = true;
        } else if (s_glass && ctx.dimmed && canary::hal::touch_read().touched) {
          canary::hal::backlight_set(CD_BRIGHT_DAY);
          ctx.dimmed = false;
        }
        break;

      case St::PhoneHere:
        if (stations == 0 && !ctx.portal_seen) {
          // Phone gave up before the portal opened — back to the QR.
          enter(St::Waiting, now);
          ui_stage(canary::ui::ObStage::Join, nullptr);
        } else if (!ctx.portal_seen &&
                   (int32_t)(now - ctx.st_since) > (int32_t)HINT_AFTER_MS) {
          // Captive sheet never popped (some Androids with "stay connected"
          // prompts, or a dismissed iOS sheet): give the manual path,
          // quietly. "browser", not "Safari" — this glass onboards Android
          // phones too (review catch). Short form on the watch: the round
          // face clips long bottom captions.
#ifdef CD_FLAVOR_WATCH
          ui_hint("no page? open 192.168.4.1");
#else
          ui_hint("no page? open your browser: 192.168.4.1");
#endif
        }
        break;

      case St::Testing: {
        const wl_status_t ws = WiFi.status();
        if (ws == WL_CONNECTED) {
          // Persist ONLY on success — a failed attempt must never leave
          // broken credentials in NVS (power-cycle restarts clean).
          canary::cfg::set_wifi_credentials(ctx.join_ssid, ctx.join_pass);
          ctx.success_at = now;
          ctx.phone_acked = false;
          enter(St::Success, now);
          ui_stage(canary::ui::ObStage::Success, nullptr);
          log_header("SETUP");
          canary::dbg_serial().printf("Joined \"%s\"  IP=%s\n", ctx.join_ssid,
                                      WiFi.localIP().toString().c_str());
        } else if (ws == WL_NO_SSID_AVAIL || ws == WL_CONNECT_FAILED ||
                   (int32_t)(now - ctx.st_since) > (int32_t)STA_TIMEOUT_MS) {
          snprintf(ctx.fail_reason, sizeof(ctx.fail_reason), "%s",
                   sta_failure_reason(ws));
          WiFi.disconnect(/*wifioff=*/false, /*eraseap=*/false);
          enter(St::Fail, now);
          ui_stage(canary::ui::ObStage::Fail, ctx.fail_reason);
        }
        break;
      }

      case St::Fail:
        // Recovery is phone-side: the portal shows the reason inline and the
        // form re-arms. A new POST /join moves us back to Testing. If the
        // phone dropped off entirely, fall back to the QR scene.
        if (stations == 0) {
          enter(St::Waiting, now);
          ui_stage(canary::ui::ObStage::Join, nullptr);
        }
        break;

      case St::Success: {
        // Hold the AP so the phone's /status poll can win the STA channel-
        // change race and render its own "done" (WAP lesson: tearing down
        // instantly makes every SUCCESSFUL join look failed on the phone).
        // The glass is the primary channel; the linger is for the phone's.
        const bool acked_beat =
            ctx.phone_acked &&
            (int32_t)(now - ctx.acked_at) > (int32_t)AP_LINGER_ACK_MS;
        const bool cap =
            (int32_t)(now - ctx.success_at) > (int32_t)AP_LINGER_MAX_MS;
        if (acked_beat || cap) {
          ctx.server.stop();
          ctx.dns.stop();
          WiFi.softAPdisconnect(/*wifioff=*/true);
          WiFi.mode(WIFI_STA);
          if (s_glass) {
            canary::ui::onboard_ui_finish();
            // Let the handoff cross-fade play before setup() continues.
            const uint32_t t1 = millis();
            while ((int32_t)(millis() - t1) < 500) {
              lv_timer_handler();
              delay(5);
            }
          }
          log_line("SETUP", "Onboarding complete.");
          g = nullptr;
          return;
        }
        break;
      }
    }

    if (s_glass) canary::ui::onboard_ui_tick(now);
    ui_pump();
    delay(4);
  }
}

}  // namespace canary::net

#endif  // FEATURE_ONBOARDING
