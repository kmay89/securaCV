// canary-local/assets/onboard-phone.js — the phone that sets up a factory-
// fresh display, against the display's REAL first-boot portal.
//
// The display emulator compiles net/provision.cpp verbatim (emulator/
// build.sh), so a first meeting (no Wi-Fi ever stored) makes the firmware's
// own provision_needed() true and it raises its SoftAP, its captive DNS and
// its WebServer. This module is the phone standing next to it. What is whose:
//
//   · the phone chrome (the Wi-Fi list, the "Sign in" sheet, the buttons) is
//     the Lab's — the same staging wap-ui.js uses for the WAP;
//   · every answer is the firmware's: the SoftAP's name and key are read
//     back from the radio it configured (what its glass QR carries), the DNS
//     reply is its dns_build_response(), and every HTTP byte — GET /, /scan,
//     POST /join, /status, the captive redirect — comes from its own
//     WebServer routes running in wasm (emu-shell.js http()/dnsQuery());
//   · the captive page is PORTAL_HTML as GET / served it, in a sandboxed
//     <iframe srcdoc> with its <script> and one style= attribute removed.
//     That script is the one thing NOT run: the Lab's policy forbids inline
//     script (canary-local/README.md §9), so this sheet stands in for it —
//     it calls the same routes the same way (the /join body is built exactly
//     as the portal builds it), and its zone picker, network list, verdicts
//     and copy are read out of the firmware's own responses.
//
// DOM-free cores are exported and pinned in tests/onboard.test.js.

import { HOME_LAN, parseDnsReply } from "../emulator/web/emu-shell.js";

const el = (tag, cls, text) => {
  const n = document.createElement(tag);
  if (cls) n.className = cls;
  if (text != null) n.textContent = text;
  return n;
};
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// ── DOM-free cores ──────────────────────────────────────────────────────────

// The srcdoc transform, byte-for-byte the one gen_display_portal.py applies
// when it writes devices/display_portal.json (whose <style> hash fleet.html's
// CSP carries): script blocks out, to a fixed point, then style=/on*=
// attributes out of every start tag, also to a fixed point.
const SCRIPT_BLOCK_RE = /<script\b[^>]*>[\s\S]*?<\/script\b[^>]*>/gi;
const TAG_RE = /<[a-zA-Z][^>]*>/g;
const INLINE_ATTR_RE = /\s(?:style|on[a-z]+)\s*=\s*(?:"[^"]*"|'[^']*'|[^\s>]+)/gi;
export function stripPortal(html) {
  let s = String(html);
  for (;;) {
    const next = s.replace(SCRIPT_BLOCK_RE, "");
    if (next === s) break;
    s = next;
  }
  // Attributes too, to a fixed point: removing one can splice a new one
  // together (`<a  onx="1"onclick=2>` leaves `<a onclick=2>` after one pass).
  return s.replace(TAG_RE, (tag) => {
    for (;;) {
      const next = tag.replace(INLINE_ATTR_RE, "");
      if (next === tag) return tag;
      tag = next;
    }
  });
}

// The zone presets the portal's own script carries (`var TZS=[...]`: label,
// POSIX rule, the IANA names a phone reports) — read from the SERVED page.
export function portalTzPresets(html) {
  const m = /var TZS=(\[\[[\s\S]*?\]\]);/.exec(String(html));
  if (!m) return [];
  try {
    const rows = JSON.parse(m[1]);
    return Array.isArray(rows) ? rows.filter((r) => Array.isArray(r) && r.length === 3) : [];
  } catch {
    return [];
  }
}

// The portal's preselection: the first preset whose IANA list names the
// phone's zone; else the "keep current setting" sentinel (index 0 — the
// option list is [sentinel, ...presets]).
export function tzPreselect(presets, iana) {
  if (!iana) return 0;
  const i = presets.findIndex((r) => ("," + r[2] + ",").includes("," + iana + ","));
  return i >= 0 ? i + 1 : 0;
}

// The sentinel's label once /scan names the zone the display is on.
export function tzKeepLabel(presets, rule) {
  if (!rule) return "Keep current setting";
  const hit = presets.find((r) => r[1] === rule);
  return "Keep " + (hit ? hit[0] : rule);
}

// POST /join's body, built exactly as the portal's script builds it: an
// empty zone is OMITTED, never sent empty ("leave it as it is" must look
// identical on the wire to an older page with no picker).
export function joinBody(ssid, pass, tz) {
  return "ssid=" + encodeURIComponent(ssid) + "&pass=" + encodeURIComponent(pass) +
    (tz ? "&tz=" + encodeURIComponent(tz) : "");
}

// The portal's signal bars.
export function barsFor(rssi) {
  return rssi > -60 ? 3 : rssi > -72 ? 2 : 1;
}

// The phone's walk. Events come from the radio (softap-up/-down, joined,
// left) and from the firmware's HTTP answers (portal, status-*); the user's
// own taps are pick/submit.
export const PHASES = ["idle", "ap", "joined", "captive", "form", "joining", "failed", "done", "gone"];
export function phoneStep(phase, ev) {
  switch (ev) {
    case "softap-up": return phase === "idle" || phase === "gone" ? "ap" : phase;
    case "softap-down": return phase === "done" || phase === "gone" ? "gone" : "idle";
    case "joined": return phase === "ap" ? "joined" : phase;
    case "left": return ["joined", "captive", "form", "joining", "failed"].includes(phase) ? "ap" : phase;
    case "portal": return phase === "joined" ? "captive" : phase;
    case "pick": return ["captive", "form", "failed"].includes(phase) ? "form" : phase;
    case "submit": return phase === "form" || phase === "failed" ? "joining" : phase;
    case "status-success": return phase === "joining" ? "done" : phase;
    case "status-fail": return phase === "joining" ? "failed" : phase;
    case "reset": return "idle";
    default: return phase;
  }
}

// ── the phone ───────────────────────────────────────────────────────────────

/**
 * @param ctx  the display sheet's guide context: ctx.emu (the live
 *             CanaryEmulator, replaced on every reboot), ctx.meetAgain().
 *             Phone state rides ctx.onboard so a rebuilt view resumes it.
 * @param opts.note (text) — the sheet's note line
 */
export function buildOnboardPhone(ctx, { note = () => {} } = {}) {
  const wrap = el("div", "wap-phone-wrap ob-wrap");
  const phone = el("div", "wap-phone");
  const statusbar = el("div", "wap-phone-status");
  const netLabel = el("span", "wap-phone-net", "📶  HomeNet");
  statusbar.append(el("span", null, "9:41"), netLabel, el("span", null, "100%"));
  const screen = el("div", "wap-phone-screen");
  phone.append(statusbar, screen);
  const air = el("ol", "ob-air");
  const ribbon = el("p", "ondevice wap-note");
  ribbon.append(el("strong", null, "How to read this: "), document.createTextNode(
    "the phone is the Lab's; everything it shows is the display's. The setup network, the " +
    "DNS answer and every HTTP byte come from net/provision.cpp running in wasm — the page " +
    "in the sheet is its PORTAL_HTML as served. Its script is the one thing not run (this " +
    "page's policy forbids inline script); the sheet below it stands in, calling the same " +
    "routes. The list under the phone is every exchange between the two."));
  wrap.append(phone, ribbon, el("h4", "ob-air-h", "Between the phone and the display"), air);

  const fresh = () => ({
    emu: ctx.emu, phase: "idle", ap: null, portal: null, presets: [], nets: null,
    tzRule: "", picked: null, manual: false, reason: "", air: [], busy: false,
  });
  if (!ctx.onboard || ctx.onboard.emu !== ctx.emu) ctx.onboard = fresh();
  const st = () => ctx.onboard;

  const go = (ev) => {
    const s = st();
    const next = phoneStep(s.phase, ev);
    if (next !== s.phase) { s.phase = next; render(); }
  };
  const log = (text, cls = "") => {
    st().air.push({ text, cls });
    if (st().air.length > 40) st().air.shift();
    paintAir();
  };
  function paintAir() {
    air.innerHTML = "";
    for (const row of st().air) air.append(el("li", row.cls ? "ob-" + row.cls : null, row.text));
  }

  // ---- screens -------------------------------------------------------------
  // The captive sheet (page + stand-in) stays mounted across its own phases,
  // so the firmware's page is parsed once per load rather than per tap.
  let sheet = null; // { view, body, portal }
  function render() {
    const s = st();
    netLabel.textContent = s.phase === "idle" || s.phase === "ap" || s.phase === "gone"
      ? "📶  HomeNet" : "📶  " + (s.ap?.ssid || "SecuraCV");
    const sheetPhase = !["idle", "ap", "joined", "gone"].includes(s.phase);
    if (sheetPhase && sheet && sheet.portal === s.portal && screen.contains(sheet.view)) {
      return fillStandIn();
    }
    screen.innerHTML = "";
    sheet = null;
    ({ idle: screenIdle, ap: screenWifi, joined: screenJoining, gone: screenGone }[s.phase] ||
      screenSheet)();
  }

  function screenIdle() {
    const s = el("div", "wap-screen wap-screen-wait");
    const b = el("button", "primary small", "▶ factory-fresh first boot");
    b.addEventListener("click", () => ctx.meetAgain?.());
    s.append(el("div", "wap-big", "⌁"),
      el("p", "muted",
        "No setup network on the air: this display already knows its Wi-Fi, so its firmware " +
        "never raises one. A first meeting is a factory-fresh unit — no network stored — and " +
        "its own provision_needed() opens the portal."),
      b);
    screen.append(s);
  }

  function screenWifi() {
    const ap = st().ap;
    const s = el("div", "wap-screen");
    s.append(el("div", "wap-ios-title", "Wi-Fi"));
    const list = el("div", "wap-wifi-list");
    const own = el("button", "wap-wifi-row wap-wifi-own");
    own.append(el("span", "wap-wifi-name", ap.ssid), el("span", "wap-wifi-meta", "🔒 📶"));
    list.append(own);
    // The same neighborhood the display's radio hears (the home router is off
    // the air while the page's Wi-Fi switch is down).
    for (const n of ctx.emu?.lan || []) {
      if (n.home && ctx.emu.linkState?.wifi === false) continue;
      const r = el("button", "wap-wifi-row wap-wifi-dim");
      r.append(el("span", "wap-wifi-name", n.ssid),
        el("span", "wap-wifi-meta", (n.secure ? "🔒 " : "") + "▮".repeat(barsFor(n.rssi))));
      r.disabled = true;
      list.append(r);
    }
    const key = el("input", "wap-input pw-masked");
    key.placeholder = "Password (it's on the glass)";
    key.autocomplete = "off";
    key.spellcheck = false;
    const msg = el("p", "ob-msg");
    const join = el("button", "ghost small", "Join");
    const qr = el("button", "primary small", "📷 Scan the join QR on the glass");
    const tryJoin = (pass, how) => {
      const code = ctx.emu.phoneJoin(ap.ssid, pass);
      if (code === 1) {
        log(`phone → AP "${ap.ssid}": associated (${how}) — the AP's DHCP hands it 192.168.4.2`, "ok");
        go("joined");
        captive();
      } else {
        msg.textContent = code === -1 ? `Incorrect password for "${ap.ssid}"`
          : code === -2 ? "The display's setup network is full (one phone at a time)"
            : "That network is no longer on the air";
        log(`phone → AP "${ap.ssid}": refused (${msg.textContent})`, "warn");
      }
    };
    own.addEventListener("click", () => key.focus());
    join.addEventListener("click", () => tryJoin(key.value, "typed key"));
    qr.addEventListener("click", () => tryJoin(ap.pass, "QR"));
    s.append(list, key, join, qr, msg, el("p", "fineprint muted",
      `WPA2, ${ap.maxStations} phone at a time, channel ${ap.channel}. The QR on the glass carries ` +
      "this network's name and its 8-character key — the key is minted once per unit and kept, " +
      "so a phone that saved it can always rejoin."));
    screen.append(s);
  }

  function screenJoining() {
    const s = el("div", "wap-screen wap-screen-wait");
    s.append(el("div", "wap-spinner"), el("p", "muted", "Looking for a sign-in page…"));
    screen.append(s);
  }

  function screenGone() {
    const s = el("div", "wap-screen wap-screen-wait");
    s.append(el("div", "wap-check", "✓"),
      el("p", null, "The setup network is gone — the display took it down once this phone saw " +
        "the verdict, and the phone dropped back to its own Wi-Fi."),
      el("p", "muted", "Watch the glass: it is joining the network you picked and carrying on " +
        "with its boot. Its serial log (Wire tab) shows the join."));
    screen.append(s);
  }

  // The captive sheet: the firmware's page on top, the stand-in below.
  function screenSheet() {
    const s = st();
    const view = el("div", "wap-screen wap-screen-browser");
    const head = el("div", "wap-captive-head ob-sheet-head");
    head.append(el("span", null, "Sign in to " + (s.ap?.ssid || "SecuraCV")));
    const cancel = el("button", "ob-link", "Cancel");
    cancel.addEventListener("click", () => {
      ctx.emu.phoneLeave();
      log("phone left the setup network", "warn");
      go("left");
    });
    head.append(cancel);
    const frame = el("iframe", "wap-captive-frame ob-frame");
    frame.setAttribute("sandbox", "");
    frame.setAttribute("title", "the display's captive portal page (firmware HTML, script not run)");
    frame.srcdoc = stripPortal(s.portal || "");
    const body = el("div", "wap-wiz ob-standin");
    view.append(head, frame, body);
    screen.append(view);
    sheet = { view, body, portal: s.portal };
    fillStandIn();
  }

  function fillStandIn() {
    const s = st();
    const body = sheet.body;
    body.innerHTML = "";
    body.append(el("p", "fineprint muted ob-standin-note",
      "The page's script, stood in for by the Lab — same routes, same requests:"));
    if (s.phase === "done") return standDone(body);
    if (s.phase === "joining") return standJoining(body);
    if (s.phase === "form" || s.phase === "failed") return standForm(body);
    standList(body);
  }

  function standList(body) {
    const s = st();
    const nets = el("div", "wap-wiz-nets");
    if (!s.nets) {
      nets.append(el("p", "muted", "scanning…"));
      scan(false);
    } else {
      for (const n of s.nets) {
        const b = el("button", "wap-wiz-net");
        b.append(el("span", null, n.ssid),
          el("span", "wap-wifi-meta", (n.secure ? "🔒 " : "") + "▮".repeat(barsFor(n.rssi))));
        b.addEventListener("click", () => { s.picked = n.ssid; s.manual = false; s.reason = ""; go("pick"); });
        nets.append(b);
      }
      const other = el("button", "wap-wiz-net ob-other", "Join another network…");
      other.addEventListener("click", () => { s.picked = ""; s.manual = true; s.reason = ""; go("pick"); });
      nets.append(other);
    }
    const again = el("button", "ghost small", "Scan again");
    again.addEventListener("click", () => { if (!s.busy) { s.nets = null; scan(true); render(); } });
    body.append(nets, again);
  }

  function standForm(body) {
    const s = st();
    body.append(el("h4", "wap-wiz-h", s.manual ? "Other network" : s.picked));
    let ssidIn = null;
    if (s.manual) {
      ssidIn = el("input", "wap-input");
      ssidIn.placeholder = "Network name";
      ssidIn.autocapitalize = "none";
      body.append(ssidIn);
    }
    const pw = el("input", "wap-input pw-masked");
    pw.placeholder = "Password";
    pw.autocomplete = "off";
    pw.spellcheck = false;
    const tz = el("select", "wap-input ob-tz");
    tz.append(new Option(tzKeepLabel(s.presets, s.tzRule), ""));
    for (const r of s.presets) tz.append(new Option(r[0], r[1]));
    let guess = "";
    try { guess = Intl.DateTimeFormat().resolvedOptions().timeZone || ""; } catch {}
    tz.selectedIndex = tzPreselect(s.presets, guess);
    const tzRow = el("label", "ob-tz-row");
    tzRow.append(el("span", "muted", "Time zone"), tz);
    const msg = el("p", "ob-msg", s.reason || "");
    body.insertBefore(msg, ssidIn || null);
    const join = el("button", "primary", "Join");
    join.addEventListener("click", () => {
      const ssid = s.manual ? ssidIn.value.trim() : s.picked;
      if (!ssid) { msg.textContent = "Enter the network name."; return; }
      submit(ssid, pw.value, tz.value);
    });
    const back = el("button", "ghost small", "‹ networks");
    back.addEventListener("click", () => { s.phase = "captive"; render(); });
    body.append(pw, tzRow, join, back, el("p", "fineprint muted",
      `The household router in this scenario is "${HOME_LAN.ssid}" — its key is ` +
      `"${HOME_LAN.pass}". Try a wrong one first: the reason you get back is the firmware's.`));
  }

  function standJoining(body) {
    body.append(el("div", "wap-spinner"),
      el("p", "muted", "Joining… (the phone polls /status every 0.9 s, as the portal does)"));
  }

  function standDone(body) {
    const copy = portalCopy(st().portal || "");
    body.append(el("div", "wap-check", "✓"), el("h4", "wap-wiz-h", copy.doneTitle),
      el("p", "muted", copy.doneBody));
  }

  // ---- the firmware's routes -----------------------------------------------
  async function captive() {
    const emu = ctx.emu;
    // What a phone does the moment it joins: resolve the OS's captive-check
    // host through the AP's DNS, fetch the check URL, follow the redirect.
    const a = parseDnsReply(await emu.dnsQuery("captive.apple.com", 1));
    log(a ? `DNS A captive.apple.com → ${a.a ?? "no address"} (${a.ancount} answer, from the display's resolver)`
      : "DNS A captive.apple.com → no reply", a?.a ? "ok" : "warn");
    const aaaa = parseDnsReply(await emu.dnsQuery("captive.apple.com", 28));
    if (aaaa) log(`DNS AAAA captive.apple.com → ${aaaa.ancount ? aaaa.ancount + " answer(s)" : "no data (A-only captive DNS)"}`);
    const probe = await emu.http("GET", "/hotspot-detect.html");
    log(`GET captive.apple.com/hotspot-detect.html → ${probe.status || "no answer"}` +
      (probe.headers.location ? ` Location: ${probe.headers.location}` : ""), probe.status ? "" : "warn");
    const page = await emu.http("GET", "/");
    log(`GET / → ${page.status || "no answer"} ${page.contentType} · ${page.body.length} chars` +
      (page.headers["cache-control"] ? ` · Cache-Control: ${page.headers["cache-control"]}` : ""),
    page.status === 200 ? "ok" : "warn");
    if (st().emu !== emu) return;
    if (page.status !== 200) {
      // No sign-in page answered: a phone gives up on the network, and so
      // does this one (the firmware takes the glass back to its QR).
      emu.phoneLeave();
      log("no sign-in page answered — the phone left the setup network", "warn");
      go("left");
      return;
    }
    st().portal = page.body;
    st().presets = portalTzPresets(page.body);
    go("portal");
  }

  async function scan(force) {
    const s = st();
    if (s.scanning) return;
    s.scanning = true;
    try {
      for (let tries = 0; tries < 40; tries++) {
        const r = await ctx.emu.http("GET", "/scan" + (force ? "?force=1" : ""));
        force = false;
        if (st() !== s) return;
        let j = null;
        try { j = JSON.parse(r.body); } catch {}
        log(`GET /scan → ${r.status || "no answer"} ${j?.scanning ? "{scanning:true}" :
          j ? `${(j.networks || []).length} network(s), zone ${j.tz || "?"}` : ""}`);
        if (j && !j.scanning) {
          s.nets = j.networks || [];
          s.tzRule = j.tz || "";
          if (s.phase === "captive") render();
          return;
        }
        await sleep(900);
      }
    } finally {
      s.scanning = false;
    }
  }

  async function submit(ssid, pass, tz) {
    const s = st();
    s.busy = true;
    s.reason = "";
    go("submit");
    const r = await ctx.emu.http("POST", "/join", {
      body: joinBody(ssid, pass, tz), contentType: "application/x-www-form-urlencoded",
    });
    log(`POST /join ssid=${ssid}${tz ? " tz=" + tz : ""} → ${r.status || "no answer"} ${r.body}`,
      r.status === 200 ? "" : "warn");
    if (r.status !== 200) {
      let reason = "Lost the display — rejoin its network and retry.";
      try { reason = JSON.parse(r.body).reason || reason; } catch {}
      s.reason = reason;
      s.busy = false;
      go("status-fail");
      return;
    }
    for (;;) {
      await sleep(900);
      if (st() !== s) return;
      const q = await ctx.emu.http("GET", "/status");
      let j = null;
      try { j = JSON.parse(q.body); } catch {}
      if (!j) {
        if (!q.status) { log("GET /status → no answer (the setup network is closing)", "warn"); }
        continue;
      }
      if (j.state === "success") {
        log('GET /status → {"state":"success"} — the display joined; it lingers for this phone, then drops the AP', "ok");
        s.busy = false;
        go("status-success");
        note("joined — the display is taking its setup network down and carrying on with its boot");
        return;
      }
      if (j.state === "fail") {
        log(`GET /status → {"state":"fail","reason":"${j.reason}"}`, "warn");
        s.reason = j.reason || "Could not connect.";
        s.busy = false;
        go("status-fail");
        return;
      }
    }
  }

  // ---- the radio, polled ---------------------------------------------------
  const poll = setInterval(() => {
    if (!document.body.contains(wrap)) { clearInterval(poll); return; }
    if (ctx.onboard?.emu !== ctx.emu) { ctx.onboard = fresh(); paintAir(); render(); }
    const s = st();
    const ap = ctx.emu?.softAp?.() ?? null;
    if (ap && !s.ap) {
      s.ap = ap;
      log(`the display raised "${ap.ssid}" (WPA2, channel ${ap.channel}) — its setup network`, "ok");
      go("softap-up");
    } else if (!ap && s.ap) {
      log(`"${s.ap.ssid}" went off the air`, s.phase === "done" ? "ok" : "warn");
      s.ap = null;
      go("softap-down");
    }
  }, 400);

  paintAir();
  render();
  return wrap;
}

// The portal's own "done" copy, read out of the served page.
function portalCopy(html) {
  const doc = new DOMParser().parseFromString(stripPortal(html), "text/html");
  return {
    doneTitle: doc.querySelector("#done h2")?.textContent?.trim() || "",
    doneBody: (doc.querySelector("#done p")?.textContent || "").replace(/\s+/g, " ").trim(),
  };
}
