/* scenes.js — lifted out of scenes.html so the page can carry a strict Content-Security-Policy
   (script-src 'self', no inline, no hashes to re-pin on every edit; the policy table is
   canary-local/tools/gen_csp.py). Same code, same load order — only the file moved. */
(function () {
  "use strict";
  var $ = function (id) { return document.getElementById(id); };
  var reduce = window.matchMedia && window.matchMedia("(prefers-reduced-motion: reduce)").matches;

  // ── real crypto, over the real canary-witness canonicals ──
  // Every claim below is signed in your browser with Ed25519 over the
  // byte-exact canonical string the firmware signs and Home Assistant
  // re-derives to verify (device_signature.cpp / signature.py). Two real
  // families, each carrying only coarse scalars — never pixels, never a
  // face, never packet contents:
  //   · sense  — radar + camera witnesses (build_sense_canonical)
  //   · event  — Wi-Fi / CSI witness (build_event_canonical)
  // Real, not decoration — but a demo key, not a provisioned device.
  var te = new TextEncoder();
  function b64url(b) { return btoa(String.fromCharCode.apply(null, b)).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, ""); }
  function hex(b) { var o = ""; for (var i = 0; i < b.length; i++) o += b[i].toString(16).padStart(2, "0"); return o; }
  async function sha256Hex(s) { return hex(new Uint8Array(await crypto.subtle.digest("SHA-256", te.encode(s)))); }

  var W = { key: null, pubHex: null, fp: null, id: "sense-quiet-room", seq: 3, uptime: 1200, sig: null };
  var cryptoOK = null; // null=unknown; true/false after ensureKeys()

  async function ensureKeys() {
    if (cryptoOK !== null) return cryptoOK;
    try {
      W.key = await crypto.subtle.generateKey("Ed25519", true, ["sign", "verify"]);
      W.pubHex = hex(new Uint8Array(await crypto.subtle.exportKey("raw", W.key.publicKey)));
      W.fp = (await sha256Hex(W.pubHex)).slice(0, 16);
      cryptoOK = true;
    } catch (e) { cryptoOK = false; }
    return cryptoOK;
  }
  // securacv-canary-sig|v1|sense|<id>|<seq>|<event>|<presence>|<occupants>|<band>|<uptime>
  function senseCanon(c) {
    return "securacv-canary-sig|v1|sense|" + W.id + "|" + W.seq + "|" +
      c.event + "|" + c.presence + "|" + c.occupants + "|" + c.band + "|" + W.uptime;
  }
  // securacv-canary-sig|v1|event|<id>|<event_id>|<state>|<category>|<privacy>|<motion>|<breathing>|<bpm>
  function eventCanon(c) {
    return "securacv-canary-sig|v1|event|" + W.id + "|" + c.event_id + "|" +
      c.state + "|" + c.category + "|" + c.privacy + "|" + c.motion + "|" + c.breathing + "|" + c.bpm;
  }
  async function signClaim(c) {
    W.sig = new Uint8Array(await crypto.subtle.sign("Ed25519", W.key.privateKey, te.encode(scene.canonical(c))));
  }
  async function verifyClaim(c) {
    return crypto.subtle.verify("Ed25519", W.key.publicKey, W.sig, te.encode(scene.canonical(c)));
  }
  ensureKeys(); // warm the key up so it's ready by the time the scene trips

  // ── the scene model: each entry is self-contained; adding 3–4 is data ──
  var SCENES = [
    {
      id: "presence", n: 1, tab: "Presence", available: true,
      deviceId: "sense-quiet-room", seq: 3,
      room: "a quiet room",
      field: "radar", glyph: "radar",
      legend: '<i class="perch-dot dot-radar"></i> 60&nbsp;GHz radar · presence only',
      chip: "someone is here",
      armed: "scene armed — radar live, room empty",
      title: "Someone is here. Never who.",
      desc: "A 60 GHz radar witness on the wall feels a body enter — range and motion, nothing more. Watch what it does, watch what it says — then try to forge what it said.",
      never: '<b>No camera. No picture. No name.</b> The radar can resolve <em>that</em> someone is there and never <em>who</em> — identity is impossible by construction, not by promise. And the one small claim it does emit is <em>signed</em> — so you can trust the record without trusting us.',
      claim: { event: "presence_detected", presence: "present", occupants: "1", band: "near", displayOcc: 1, displayRange: 2.4 },
      canonical: senseCanon,
      display: function (c) { return "presence/detected  { occupants: " + c.displayOcc + ", range_m: " + c.displayRange + " }"; },
      beat: { kind: "forge", label: "⚠ try to forge it", altLabel: "↺ restore the true record", cls: "",
        note: "The bytes changed; the signature didn't. Anyone can catch it — no server needed." },
      forged: { event: "occupancy_changed", presence: "present", occupants: "2+", band: "mid", displayOcc: 2, displayRange: 1.2 },
    },
    {
      id: "vision", n: 2, tab: "Pixels stay put", available: true,
      deviceId: "vision-front-hall", seq: 5,
      room: "the front hall",
      field: "vision", glyph: "vision",
      legend: '<i class="perch-dot dot-camera"></i> on-device camera · a label, never a frame',
      chip: "a person",
      armed: "scene armed — camera live, hall empty",
      title: "A person. Never a picture.",
      desc: "An on-device camera sees a shape move in and classifies it right there on the lens module — person, at some confidence. The pixels are graded and thrown away before the host ever sees them; only a label leaves. Watch what it says, then try to grab the picture.",
      never: '<b>No image. No face. No box you could un-blur.</b> The frame is classified inside the camera module and discarded; the host only ever gets a word and a number. The claim is <em>signed</em> the same way the radar\'s is — same canonical, same key — so you can trust <em>that</em> a person was seen without there being any picture <em>of</em> them.',
      claim: { event: "presence_started", presence: "present", occupants: "1", band: "unknown", conf: 0.94, displayOcc: 1 },
      canonical: senseCanon,
      display: function (c) { return 'presence_started  { occupants: ' + c.displayOcc + ', range: "unknown" }'; },
      beat: { kind: "grab", label: "🔍 try to grab the picture", altLabel: "↺ close the packet", cls: "grab",
        note: "That's the whole packet — a word, a score, a box in coordinates. Not one pixel.",
        feed: "opened the packet — no image inside" },
      packet: function () {
        return "topic  securacv/" + W.id + "/events\n{\n" +
          '  "event":     "presence_started",\n' +
          '  "presence":  "present",\n' +
          '  "occupants": "1",\n' +
          '  "range":     "unknown",\n' +
          '  "conf":      0.94,\n' +
          '  "bbox":      [88, 40, 44, 120],\n' +
          '  "sig":       "ed25519:' + (W.sig ? b64url(W.sig).slice(0, 16) : "…") + '…"\n}';
      },
    },
    {
      id: "wifi", n: 3, tab: "The WiFi field", available: true,
      deviceId: "wap-living-room", seq: 12,
      room: "the living room",
      field: "wifi", glyph: "wifi",
      legend: '<i class="perch-dot dot-wifi"></i> Wi-Fi CSI · presence in the field, never the traffic',
      chip: "the field bends",
      armed: "scene armed — Wi-Fi field steady, room empty",
      title: "The room's radio bends. It never reads.",
      desc: "The access point already floods the room with Wi-Fi. A body walking in bends that field — and the witness reads only the shape of the bend, never a byte of what the radio carries. Watch what it says, then try to read the traffic.",
      never: '<b>No packets. No MACs. No SSIDs.</b> Wi-Fi sensing here is channel-state only — how the room\'s radio distorts around a body. It can\'t and doesn\'t inspect what any device is sending; the claim it signs is a handful of scalars, nothing that could name a person or a device — and it\'s <em>signed</em>, same key, same open canonical as the rest.',
      claim: { event_id: 12, state: "present", category: "event", privacy: "p1", motion: 71, breathing: 0, bpm: 0 },
      canonical: eventCanon,
      display: function (c) { return 'presence  { state: "' + c.state + '", motion: ' + c.motion + ' }'; },
      beat: { kind: "grab", label: "📡 try to read the traffic", altLabel: "↺ close the packet", cls: "grab",
        note: "Just scalars from the field's shape — a presence flag, a motion score, a privacy tier. No MACs, no SSIDs, not one packet. It never reads what the Wi-Fi carries, only how a body bends it.",
        feed: "opened the packet — no traffic inside" },
      packet: function () {
        return "topic  securacv/" + W.id + "/events\n{\n" +
          '  "event_id":  12,\n' +
          '  "state":     "present",\n' +
          '  "category":  "event",\n' +
          '  "privacy":   "p1",\n' +
          '  "motion":    71,\n' +
          '  "breathing": 0,\n' +
          '  "bpm":       0,\n' +
          '  "sig":       "ed25519:' + (W.sig ? b64url(W.sig).slice(0, 16) : "…") + '…"\n}';
      },
    },
    {
      id: "breath", n: 4, tab: "The breathing wave", available: true,
      deviceId: "sense-bedside", seq: 8,
      room: "the bedside",
      field: "breath", glyph: "breath",
      legend: '<i class="perch-dot dot-breath"></i> 60&nbsp;GHz radar · wellbeing, one person, up close',
      chip: "still here, breathing",
      armed: "scene armed — radar live, bedside empty",
      title: "Still breathing. Never watched.",
      desc: "The same 60 GHz radar, up close and still, can feel a chest rise and fall — a breathing rate, even a pulse — for exactly one person within about a meter. No camera, no mic. Watch what it says, then let a second person walk in.",
      never: '<b>Wellbeing, not medical — and only for one.</b> Vitals are hard-suppressed unless exactly one target is in range; multi-person BPM attribution is a line the design refuses to cross. It reads a chest through radio, never a face or a voice, and signs only the coarse record — the breath and heart numbers ride along unsigned, and vanish the moment they can\'t be trusted.',
      claim: { event: "presence_started", presence: "present", occupants: "1", band: "near" },
      suppressed: { event: "occupancy_changed", presence: "present", occupants: "2+", band: "near" },
      canonical: senseCanon,
      display: function (c) { return c.event + '  { occupants: ' + c.occupants + ', range: "near" }'; },
      vitalsOn: 'wellbeing · <b>breathing locked</b> · ~14 breaths/min · ~62 bpm',
      vitalsOff: 'wellbeing · <b>suppressed</b> — vitals need a single target',
      beat: { kind: "policy", label: "👥 add another person", altLabel: "↺ just one person again", cls: "",
        note: "Two people in range. The lock drops and the numbers go null — by construction. It won't put a heartbeat on anyone in a crowd." },
    },
  ];
  var scene = SCENES[0];
  function claimText(c) { return scene.display(c); }

  // ── element handles ──
  var tabs = $("scene-tabs"), feed = $("feed-list");
  var marker = $("marker"), visitor = $("visitor"), chip = $("chip"),
      payoff = $("payoff"), playBtn = $("play-btn"), wire = $("wire");
  var sigRow = $("sig-row"), sigStatus = $("sig-status"), sigVal = $("sig-val"),
      beatBox = $("beat"), beatBtn = $("beat-btn"), beatNote = $("beat-note"), vitals = $("vitals");
  var liveClaim = null; // the record currently on screen (may be tampered)

  // ── the witness feed ──
  function feedLine(text, kind) {
    var li = document.createElement("li");
    li.className = "feed-item" + (kind ? " " + kind : "");
    li.textContent = text;
    feed.insertBefore(li, feed.firstChild);
    while (feed.children.length > 6) feed.removeChild(feed.lastChild);
  }

  // ── build the scene tabs (the "one of many" model, made visible) ──
  SCENES.forEach(function (s) {
    var b = document.createElement("button");
    b.type = "button";
    b.className = "scene-tab" + (s.available ? "" : " soon");
    b.innerHTML = '<span class="n">' + s.n + '</span>' + s.tab +
      (s.available ? "" : ' <span class="tag">soon</span>');
    if (!s.available) { b.disabled = true; }
    else { b.addEventListener("click", function () { selectScene(s); }); }
    s._tab = b;
    tabs.appendChild(b);
  });

  // ── switch to a scene: paint copy, swap the room's field + glyph, reset ──
  function svgShow(el, show) { el.classList.toggle("scene-off", !show); } // SVG: [hidden] is unreliable; a class, not a style= attribute (the page's CSP has no 'unsafe-inline')
  function selectScene(s) {
    scene = s;
    W.id = s.deviceId; W.seq = s.seq;
    SCENES.forEach(function (o) { if (o._tab) o._tab.classList.toggle("on", o === s); });
    $("scene-title").textContent = s.title;
    $("scene-desc").textContent = s.desc;
    $("room-label").textContent = s.room;
    $("chip-text").textContent = s.chip;
    $("legend-txt").innerHTML = s.legend;
    $("never").innerHTML = s.never;
    // SVG elements have no `.hidden` IDL property — toggle the attribute
    svgShow($("field-radar"), s.field === "radar");
    svgShow($("field-vision"), s.field === "vision");
    svgShow($("field-wifi"), s.field === "wifi");
    svgShow($("field-breath"), s.field === "breath");
    svgShow(marker.querySelector(".glyph-radar"), s.glyph === "radar");
    svgShow(marker.querySelector(".glyph-vision"), s.glyph === "vision");
    svgShow(marker.querySelector(".glyph-wifi"), s.glyph === "wifi");
    svgShow(marker.querySelector(".glyph-breath"), s.glyph === "breath");
    feed.innerHTML = "";
    feedLine(s.armed, "feed-sys");
    reset();
  }

  function paintVerdict(ok) {
    sigRow.hidden = false;
    if (cryptoOK === false) {
      sigStatus.className = "sig-status";
      sigStatus.textContent = "ⓘ signed on the device";
      sigVal.textContent = "(this browser can't run Ed25519 in-page — on one that can, the claim is signed and verified live)";
      beatBox.hidden = true;
      return;
    }
    sigVal.textContent = "ed25519:" + b64url(W.sig).slice(0, 12) + "…  · key " + W.fp;
    sigStatus.className = "sig-status " + (ok ? "ok" : "bad");
    sigStatus.textContent = ok ? "✓ verified" : "✗ forged — signature doesn't match";
  }

  function showVitals(htmlText, off) {
    if (!htmlText) { vitals.hidden = true; return; }
    vitals.hidden = false;
    vitals.className = "vitals" + (off ? " off" : "");
    vitals.innerHTML = htmlText;
  }

  // ── the interactive beat, dispatched per scene ──
  var BEATS = {
    // forge (radar): rewrite the record and watch verification fail
    forge: {
      async on() {
        liveClaim = Object.assign({}, scene.forged);
        $("payoff-claim").textContent = claimText(liveClaim);
        paintVerdict(await verifyClaim(liveClaim));   // old sig vs. new bytes → false
        feedLine("record altered by hand → " + claimText(liveClaim), "feed-sys");
        beatNote.textContent = scene.beat.note;
      },
      async off() {
        liveClaim = Object.assign({}, scene.claim);
        $("payoff-claim").textContent = claimText(liveClaim);
        paintVerdict(await verifyClaim(liveClaim));
        feedLine("true record restored", "feed-sys");
        beatNote.textContent = "";
      },
    },
    // grab (camera / Wi-Fi): open the whole packet and find the feared thing
    // (a picture, network traffic) simply isn't in it
    grab: {
      async on() {
        wire.textContent = scene.packet();
        wire.hidden = false;
        beatNote.textContent = scene.beat.note;
        feedLine(scene.beat.feed, "feed-sys");
      },
      async off() { wire.hidden = true; beatNote.textContent = ""; },
    },
    // policy (breathing): a second person walks in — the device honestly
    // re-signs occupants:2+ and the vitals suppress. Both records verify;
    // the point is what it *won't* report, not a broken signature.
    policy: {
      async on() {
        liveClaim = Object.assign({}, scene.suppressed);
        $("payoff-claim").textContent = claimText(liveClaim);
        await signClaim(liveClaim);                    // a real, new signed record
        paintVerdict(await verifyClaim(liveClaim));     // still ✓ — honestly signed
        showVitals(scene.vitalsOff, true);
        feedLine("second target in range → vitals suppressed", "feed-sys");
        beatNote.textContent = scene.beat.note;
      },
      async off() {
        liveClaim = Object.assign({}, scene.claim);
        $("payoff-claim").textContent = claimText(liveClaim);
        await signClaim(liveClaim);
        paintVerdict(await verifyClaim(liveClaim));
        showVitals(scene.vitalsOn, false);
        feedLine("single target — breathing lock re-acquired", "feed-sys");
        beatNote.textContent = "";
      },
    },
  };

  beatBtn.addEventListener("click", async function () {
    var b = BEATS[scene.beat.kind];
    if (beatBox.dataset.state === "on") { await b.off(); beatBtn.textContent = scene.beat.label; beatBox.dataset.state = ""; }
    else { await b.on(); beatBtn.textContent = scene.beat.altLabel; beatBox.dataset.state = "on"; }
  });

  // ── play the scene ──
  var timers = [];
  function clearTimers() { timers.forEach(clearTimeout); timers = []; }
  function at(ms, fn) { timers.push(setTimeout(fn, ms)); }
  function moveVisitor(x, y) { visitor.setAttribute("transform", "translate(" + x + "," + y + ")"); }

  function reset() {
    clearTimers();
    marker.classList.remove("tripped");
    chip.classList.remove("show");
    payoff.classList.remove("show");
    sigRow.hidden = true;
    beatBox.hidden = true;
    beatBox.dataset.state = "";
    wire.hidden = true;
    vitals.hidden = true;
    beatNote.textContent = "";
    liveClaim = null;
    visitor.classList.add("hidden");
    moveVisitor(492, 352);
    visitor.style.transition = "none";
    playBtn.disabled = false;
    playBtn.textContent = "▶ play the scene";
  }

  function play() {
    reset();
    playBtn.disabled = true;
    playBtn.textContent = "▶ playing…";
    at(60, function () {
      visitor.classList.remove("hidden");
      visitor.style.transition = reduce ? "none" : "transform 1.2s ease-in-out";
      moveVisitor(430, 320);
    });
    at(1250, function () { moveVisitor(360, 292); });           // into the room
    // the trip — reveal the claim, then sign it for real a beat later
    at(2450, async function () {
      marker.classList.add("tripped");
      chip.classList.add("show");
      liveClaim = Object.assign({}, scene.claim);
      $("payoff-claim").textContent = claimText(liveClaim);
      feedLine(claimText(liveClaim), "feed-witness");
      at(400, function () { payoff.classList.add("show"); });
      // vitals are unsigned wellbeing metadata — show them regardless of
      // whether this browser can run Ed25519 in-page
      if (scene.vitalsOn) showVitals(scene.vitalsOn, false);
      var ready = await ensureKeys();
      if (ready) {
        await signClaim(liveClaim);
        paintVerdict(await verifyClaim(liveClaim));
        beatBtn.textContent = scene.beat.label;
        beatBtn.className = "beat-btn" + (scene.beat.cls ? " " + scene.beat.cls : "");
        beatBox.hidden = false;
        feedLine("claim signed  ✓", "feed-sys");
      } else {
        paintVerdict(false); // graceful "signed on the device" note
      }
    });
    at(2900, function () { moveVisitor(300, 300); });           // settles in
    at(6200, function () {
      playBtn.disabled = false;
      playBtn.textContent = "↺ replay the scene";
    });
  }

  playBtn.addEventListener("click", play);
  selectScene(SCENES[0]);
})();
