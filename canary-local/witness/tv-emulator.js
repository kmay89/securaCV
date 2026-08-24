/* The Witness Wall — a drivable Apple TV emulator.
 *
 * There is no native tvOS binary to compile to WASM (the app is design-stage,
 * native SwiftUI), so this is a faithful, drivable emulator of the experience:
 * the tvOS focus engine (spatial D-pad navigation with lift + ring), an
 * on-screen remote (styled after the Apple TV remote), three profiles
 * (Home = the Witness Wall, Business = the Witness Board, Apartment = the
 * peephole), a live clock and timeline, and the real interactions — watch a
 * show with a doorbell over it, a heavy monitoring board, a peephole peek,
 * one-tap Incident capture, break-glass by quorum, and the ambient screensaver.
 *
 * It can also connect to a REAL SecuraCV kernel on the LAN and show your actual
 * Canaries (see the connect panel) — browsers can't mDNS-scan, so you point it
 * at the kernel and it fetches the fleet; otherwise it runs on demo data. When
 * the page CAN reach the LAN (served from the hub, over http, or inside an
 * app shell) it also keeps probing the well-known addresses by itself, and the
 * first kernel that answers becomes a "Go live" offer — the demo stays up
 * until the user flips the switch.
 *
 * No inline JS (the site CSP is script-src 'self'); loaded by witness-wall.html
 * alongside js/site.js. It only touches its own #tv subtree + the connect panel.
 */
import { highlightJSON } from './highlight.js';

const tv = document.getElementById('tv');
const stage = document.getElementById('stage');
if (tv && stage) {
  const reduce = window.matchMedia('(prefers-reduced-motion: reduce)').matches;
  const $ = (id) => document.getElementById(id);

  // ---- clock ----
  let simMin = 4 * 60 + 2;
  const shortFmt = (m) => { let h = Math.floor(m / 60) % 24, mm = ((m % 60) + 60) % 60, ap = h >= 12 ? 'PM' : 'AM', hh = h % 12 || 12; return `${hh}:${mm < 10 ? '0' : ''}${mm} ${ap}`; };
  const hexTime = (m) => { let h = Math.floor(m / 60) % 24, mm = ((m % 60) + 60) % 60, hh = h % 12 || 12; return `${hh}:${mm < 10 ? '0' : ''}${mm}`; };
  const randHex = (n) => { let s = '', c = '0123456789abcdef'; for (let i = 0; i < n; i++) s += c[Math.floor(Math.random() * 16)]; return s; };
  let chainHead = randHex(4) + '…' + randHex(4);

  // ---- state ----
  let edition = 'home';       // home | business | apartment
  let hostEdition = null;     // set via ?profile= or witness:profile — an embedding
                              // surface pins the wall to its use case, and a
                              // discovered fleet then updates THAT view in place
                              // instead of yanking the user back to home
  let homeMode = 'wall';      // wall | watching
  let bizDensity = 'glance';  // glance | monitor
  let filter = 'all';
  let liveFleet = null, liveHost = null, justAppeared = null;

  const homeEvents = [
    { t: '4:02', w: '<b>Motion</b> · Zone A' }, { t: '3:47', w: '<b>Presence</b> · Studio' },
    { t: '3:14', w: '<b>Door</b> · Front' }, { t: '2:58', w: '<b>Quiet</b> · daily digest' },
  ];
  const bizEvents = [
    { t: '11:52', w: '<b>Motion</b>', z: 'Register 2', kind: 'Motion' }, { t: '11:47', w: '<b>Presence</b>', z: 'Patio', kind: 'Presence' },
    { t: '11:39', w: '<b>Door</b>', z: 'Front', kind: 'Door' }, { t: '11:31', w: '<b>Motion</b>', z: 'Kitchen', kind: 'Motion' },
    { t: '11:20', w: '<b>Presence</b>', z: 'Walk-in', kind: 'Presence' }, { t: '11:08', w: '<b>Door</b>', z: 'Side', kind: 'Door' },
  ];
  const homeTiles = [{ n: 'Front Door' }, { n: 'Studio' }, { n: 'Garage' }, { n: 'Back Gate' }];
  const bizZones = [{ n: 'Register 1' }, { n: 'Register 2' }, { n: 'Front door', attn: true }, { n: 'Patio' }, { n: 'Kitchen line' }, { n: 'Walk-in' }];
  let incidents = [{ w: 'Chargeback dispute', t: '10:41 · Register 2' }, { w: 'After-hours motion', t: 'last night · Patio' }];
  const sites = [{ n: 'Downtown', open: 0 }, { n: 'Airport', open: 1, attn: true }, { n: 'Harbor', open: 0 }];
  const aptLog = [{ t: '3:02', w: 'Delivery · package left' }, { t: '1:18', w: 'Knock · no answer needed' }, { t: '11:40', w: 'Presence · hallway' }];
  let aptKnocking = false;

  const esc = (s) => String(s).replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
  function homeFleet() { return liveFleet ? liveFleet.map((d) => ({ n: d.name, off: d.online === false })) : homeTiles; }
  // For a business the real Canaries ARE the zones — when a live fleet is
  // connected, the Board's tiles and zone chips show it (offline = attention).
  function bizFleet() { return liveFleet ? liveFleet.map((d) => ({ n: d.name, off: d.online === false, attn: d.online === false })) : bizZones; }

  function tilesHTML(tiles) {
    const cls = ['', 'b', 'c', 'd'];
    return tiles.map((t, i) => `<div class="f tile ${cls[i % 4]}${t.off ? ' offline' : ''}${t.n === justAppeared ? ' just' : ''}" data-action="tile" data-name="${esc(t.n)}" tabindex="-1"><div class="n">${esc(t.n)}</div><div class="s"><span class="sd${t.off ? ' off' : ''}"></span>${t.off ? 'offline' : 'online · calm'}</div><span class="chip">chain ✓</span></div>`).join('');
  }
  const evHTML = (evs) => evs.map((e) => `<div class="ev${e.flag ? ' flag' : ''}" data-action="event"><span class="t">${e.t}</span><span class="w">${e.w}${e.z ? ' · ' + e.z : ''}</span><span class="seal">${e.flag ? '⚑ sealed ✓' : 'sealed ✓'}</span></div>`).join('');

  // ---------- renderers ----------
  function render() {
    if (edition === 'home' && homeMode === 'watching') return renderWatching();
    if (edition === 'home') return renderHome();
    if (edition === 'apartment') return renderApartment();
    return bizDensity === 'monitor' ? renderMonitor() : renderGlance();
    // (buildFocus called inside each)
  }

  function editionSync() {
    document.querySelectorAll('[data-action^="edition:"]').forEach((el) => el.setAttribute('data-on', el.getAttribute('data-action') === 'edition:' + edition ? '1' : '0'));
  }

  function renderHome() {
    const live = liveFleet ? `<span class="chip" style="margin-left:8px">● LIVE · ${esc(liveHost)}</span>` : '';
    stage.innerHTML =
      `<div class="col"><div class="col-label">${liveFleet ? 'Your fleet' : 'Your fleet · 4 Canaries'}${live}</div><div class="grid2">${tilesHTML(homeFleet())}</div>` +
      `<div class="actions">` +
        `<span class="f act" data-action="watch" tabindex="-1"><span class="k">▶</span>Watch a show</span>` +
        `<span class="f act" data-action="doorbell" tabindex="-1"><span class="k">🔔</span>Doorbell</span>` +
        `<span class="f act" data-action="breakglass" tabindex="-1"><span class="k">⌂</span>Break-glass</span>` +
        `<span class="f act" data-action="screensaver" tabindex="-1"><span class="k">✦</span>Screensaver</span>` +
      `</div></div>` +
      `<div class="col"><div class="col-label">Verified timeline</div><div class="events" id="events">${evHTML(homeEvents)}</div><div class="hashline">chain head · ed25519 · ${chainHead} ✓</div></div>`;
    editionSync(); buildFocus();
  }

  function renderWatching() {
    stage.innerHTML =
      `<div class="movie">` +
        `<span class="badge-np">Now playing</span>` +
        `<span class="witness-chip"><span class="sd"></span>witness: all clear ✓ · ${shortFmt(simMin)}</span>` +
        `<div class="m-meta"><div class="mtitle">The Lighthouse Keeper</div><div class="mrun">${hexTime(72 + (simMin % 20))}:44 · playing</div></div>` +
        `<div class="mbar"><span></span></div>` +
        `<div class="mhint actions">` +
          `<span class="f act" data-action="doorbell" tabindex="-1"><span class="k">🔔</span>Ring doorbell</span>` +
          `<span class="f act" data-action="stopwatch" tabindex="-1"><span class="k">◼</span>Stop watching</span>` +
        `</div>` +
      `</div>`;
    editionSync(); buildFocus();
  }

  function renderGlance() {
    const live = liveFleet ? `<span class="chip" style="margin-left:8px">● LIVE · ${esc(liveHost)}</span>` : '';
    stage.innerHTML =
      `<div class="col"><div class="modebar">` +
        `<span class="f mchip" data-action="density:glance" data-on="1" tabindex="-1">Glance</span>` +
        `<span class="f mchip" data-action="density:monitor" data-on="0" tabindex="-1">Monitor</span>` +
      `</div><div class="col-label">This location · all clear${live}</div><div class="grid2">${tilesHTML(bizFleet().slice(0, 4))}</div>` +
      `<div class="actions">` +
        `<span class="f act" data-action="incident" tabindex="-1"><span class="k">⚑</span>Incident capture</span>` +
        `<span class="f act" data-action="disputepack" tabindex="-1"><span class="k">⇪</span>Dispute pack</span>` +
        `<span class="f act" data-action="breakglass" tabindex="-1"><span class="k">⌂</span>Break-glass</span>` +
      `</div></div>` +
      `<div class="col"><div class="col-label">Verified timeline</div><div class="events">${evHTML(bizEvents.slice(0, 4))}</div><div class="hashline">chain head · ${chainHead} ✓ · open · staffed</div></div>`;
    editionSync(); buildFocus();
  }

  function renderMonitor() {
    const shown = bizEvents.filter((e) => filter === 'all' || e.kind === filter);
    const feed = shown.map((e) => `<div class="frow${e.flag ? ' attn' : ''}${e.acked ? ' acked' : ''} f" data-action="ack" data-id="${bizEvents.indexOf(e)}" tabindex="-1"><span class="ft">${e.t}</span><span class="fzone">${e.z || '—'}</span><span class="fw">${e.w}</span><span class="fack">${e.acked ? 'acked' : 'ack ✓'}</span></div>`).join('');
    const zsrc = bizFleet();
    const zones = zsrc.map((z) => `<div class="zchip${z.attn ? ' attn' : ''}"><span class="zd"></span>${esc(z.n)}</div>`).join('');
    const inc = incidents.length ? incidents.map((c, i) => `<div class="irow f" data-action="incident-review" data-id="${i}" tabindex="-1"><span class="il">${esc(c.w)}<br><span class="it">${esc(c.t)}</span></span><span class="rev">review →</span></div>`).join('') : '<div class="it" style="color:var(--tv-muted);font-family:var(--mono);font-size:.62rem">none open · all resolved</div>';
    const siteRows = sites.map((s) => `<div class="s2${s.attn ? ' attn' : ''}"><span><span class="sd"></span> ${esc(s.n)}</span><span class="cnt">${s.open} open</span><span class="cnt">chain ✓</span></div>`).join('');
    const chips = ['all', 'Motion', 'Door', 'Presence'].map((k) => `<span class="f fchip" data-action="filter:${k}" data-on="${filter === k ? '1' : '0'}" tabindex="-1">${k === 'all' ? 'All' : k}</span>`).join('');
    stage.innerHTML =
      `<div style="display:flex;flex-direction:column;min-height:0;flex:1">` +
      `<div class="modebar">` +
        `<span class="f mchip" data-action="density:glance" data-on="0" tabindex="-1">Glance</span>` +
        `<span class="f mchip" data-action="density:monitor" data-on="1" tabindex="-1">Monitor</span>` +
      `</div>` +
      `<div class="ops">` +
        `<div class="cell"><div class="k">Hours</div><div class="v open">Open · staffed</div><div class="sub">closes 2:00 AM</div></div>` +
        `<div class="cell"><div class="k">On shift</div><div class="v">A. Rivera</div><div class="sub">since 6:12 PM · handoff 2:00</div></div>` +
        `<div class="cell"><div class="k">Compliance</div><div class="v">214 sealed today</div><div class="sub">chain ✓ through ${shortFmt(simMin)}</div></div>` +
      `</div>` +
      `<div class="mon">` +
        `<div style="display:flex;flex-direction:column;min-height:0">` +
          `<div class="filters">${chips}` +
            `<span class="f fchip" data-action="ackall" data-on="0" tabindex="-1" style="margin-left:auto">Acknowledge all</span></div>` +
          `<div class="feed">${feed}</div>` +
        `</div>` +
        `<div class="rail">` +
          `<div class="panel"><h4>Zones <span class="ct">${zsrc.filter((z) => z.attn).length} attn</span></h4><div class="zones">${zones}</div></div>` +
          `<div class="panel"><h4>Open incidents <span class="ct">${incidents.length}</span></h4><div class="inc">${inc}</div></div>` +
          `<div class="panel"><h4>Sites <span class="ct">${sites.reduce((a, s) => a + s.open, 0)} open</span></h4><div class="sites2">${siteRows}</div></div>` +
        `</div>` +
      `</div>` +
      `<div class="actions" style="margin-top:10px">` +
        `<span class="f act" data-action="incident" tabindex="-1"><span class="k">⚑</span>Incident capture</span>` +
        `<span class="f act" data-action="breakglass" tabindex="-1"><span class="k">⌂</span>Break-glass</span>` +
      `</div></div>`;
    editionSync(); buildFocus();
  }

  function renderApartment() {
    stage.innerHTML =
      `<div class="apt">` +
        `<div class="door${aptKnocking ? ' knocking' : ''}">` +
          `<div class="big">${aptKnocking ? '🔔 Someone\'s at the door' : 'All quiet at the door'}</div>` +
          `<div class="status"><span class="sd"></span>${aptKnocking ? 'knock detected · peek when ready' : 'peephole Canary · witnessing'}</div>` +
          `<div class="batt">🔋 84% · ~4 months · no wiring</div>` +
          `<div class="cordnote">A battery Canary clips over the peephole — no outlet, no cord to run. It sleeps until it hears a knock, then wakes and tells this screen.</div>` +
        `</div>` +
        `<div class="aptside">` +
          `<div class="f peekbtn" data-action="peek" tabindex="-1"><span class="eye">👁</span><span class="lab">Peek at the door</span><span class="hint2">see who's there without getting up</span></div>` +
          `<div class="aptlog"><h4>Recent at the door</h4>${aptLog.map((l) => `<div class="lrow"><span class="lt">${l.t}</span><span>${esc(l.w)}</span></div>`).join('')}</div>` +
          `<div class="actions">` +
            `<span class="f act" data-action="knock" tabindex="-1"><span class="k">🔔</span>Knock (demo)</span>` +
            `<span class="f act" data-action="breakglass" tabindex="-1"><span class="k">⌂</span>Break-glass</span>` +
          `</div>` +
        `</div>` +
      `</div>`;
    editionSync(); buildFocus();
  }

  // ---------- focus engine ----------
  let focusables = [], current = null;
  function buildFocus() {
    focusables = Array.prototype.slice.call(document.querySelectorAll('.f')).filter((el) => el.offsetParent !== null && !el.closest('.pip:not(.show)') && !el.closest('.overlay:not(.show)'));
    if (current && focusables.indexOf(current) === -1) current = null;
    if (!current) current = document.querySelector('.tile.f') || document.querySelector('.peekbtn.f') || focusables[0];
    paint();
  }
  const paint = () => focusables.forEach((el) => el.classList.toggle('focused', el === current));
  const center = (el) => { const r = el.getBoundingClientRect(); return { x: r.left + r.width / 2, y: r.top + r.height / 2 }; };
  function move(dir) {
    if (!current) { current = focusables[0]; paint(); return; }
    const c = center(current); let best = null, bestScore = Infinity;
    focusables.forEach((el) => {
      if (el === current) return;
      const t = center(el), dx = t.x - c.x, dy = t.y - c.y; let primary, cross;
      if (dir === 'left') { if (dx > -6) return; primary = -dx; cross = Math.abs(dy); }
      else if (dir === 'right') { if (dx < 6) return; primary = dx; cross = Math.abs(dy); }
      else if (dir === 'up') { if (dy > -6) return; primary = -dy; cross = Math.abs(dx); }
      else { if (dy < 6) return; primary = dy; cross = Math.abs(dx); }
      const score = primary + cross * 2.2;
      if (score < bestScore) { bestScore = score; best = el; }
    });
    if (best) { current = best; paint(); }
  }

  // ---------- toast ----------
  const toast = $('toast'), toastMsg = $('toastMsg'); let toastT;
  function showToast(msg) { toastMsg.textContent = msg; toast.classList.add('show'); clearTimeout(toastT); toastT = setTimeout(() => toast.classList.remove('show'), 2400); }

  // ---------- break-glass ----------
  const overlay = $('overlay'), quorumEl = $('quorum'), sealbox = $('sealbox'), ovTitle = $('ovTitle'), ovBody = $('ovBody');
  let approvals = 0; const quorumNeed = 3; let overlayOpen = false, savedFocus = null;
  function openBreakGlass() {
    approvals = 0; overlayOpen = true; savedFocus = current; sealbox.style.display = 'none';
    const holders = edition === 'business' ? ['Mgr A', 'Mgr B', 'Owner'] : edition === 'apartment' ? ['You', 'Building', 'Guardian'] : ['You', 'Partner', 'Guardian'];
    ovTitle.textContent = edition === 'business' ? 'Break-glass — manager quorum' : 'Break-glass — quorum';
    ovBody.innerHTML = `Unsealing a sealed evidence window needs ${quorumNeed} of ${quorumNeed} approvals. Press <b>Approve</b> for each holder.`;
    quorumEl.innerHTML = holders.map((h, i) => `<div class="qdot" data-i="${i}">${h}</div>`).join('');
    overlay.classList.add('show');
    focusables = Array.prototype.slice.call(overlay.querySelectorAll('.f'));
    current = overlay.querySelector('[data-action="approve"]'); paint();
  }
  function approve() {
    if (approvals >= quorumNeed) return;
    const dot = quorumEl.querySelector(`.qdot[data-i="${approvals}"]`);
    if (dot) { dot.classList.add('ok'); dot.textContent = '✓'; }
    approvals++;
    if (approvals >= quorumNeed) { sealbox.style.display = 'block'; ovBody.innerHTML = 'Quorum met — the evidence window is unsealed, and the unseal itself is now a sealed event on the chain.'; chainHead = randHex(4) + '…' + randHex(4); showToast('Break-glass approved · unseal written to chain'); }
  }
  function closeOverlay() { overlay.classList.remove('show'); overlayOpen = false; render(); current = savedFocus; buildFocus(); }

  // ---------- doorbell / peek PiP ----------
  const pip = $('doorbell'), pipKind = $('pipKind'), pipSub = $('pipSub'), pipTime = $('pipTime'), pipParcel = $('pipParcel');
  let pipOpen = false, pipSavedFocus = null;
  const doorScripts = [
    { kind: 'Front Door', sub: 'Delivery · your food’s here', parcel: true },
    { kind: 'Front Door', sub: 'Someone’s at the door', parcel: false },
    { kind: 'Front Door', sub: 'Package left at the door', parcel: true },
  ];
  function showPip(script, expanded) {
    const s = script || doorScripts[Math.floor(Math.random() * doorScripts.length)];
    pipKind.textContent = s.kind; pipSub.textContent = s.sub; pipTime.textContent = shortFmt(simMin);
    pipParcel.style.display = s.parcel ? '' : 'none';
    pip.classList.toggle('expanded', !!expanded);
    if (!pipOpen) { pipSavedFocus = current; }
    pip.classList.add('show'); pipOpen = true;
    chime();
    focusables = Array.prototype.slice.call(pip.querySelectorAll('.f'));
    current = pip.querySelector('[data-action="pip-view"]'); paint();
  }
  function closePip() { pip.classList.remove('show', 'expanded'); pipOpen = false; aptKnocking = false; render(); current = pipSavedFocus; buildFocus(); }

  // ---------- incident / dispute ----------
  function captureIncident() {
    const zone = bizZones[Math.floor(Math.random() * bizZones.length)].n;
    bizEvents.unshift({ t: hexTime(simMin), w: '<b>Incident</b>', z: zone, kind: 'Motion', flag: true });
    if (bizEvents.length > 8) bizEvents.pop();
    incidents.unshift({ w: 'Captured incident', t: shortFmt(simMin) + ' · ' + zone });
    chainHead = randHex(4) + '…' + randHex(4);
    render();
    showToast('Incident sealed · ' + hexTime(simMin) + ' · ' + randHex(6));
  }

  // ---------- screensaver ----------
  const saver = $('saver'), saverCanvas = $('saverCanvas'), sctx = saverCanvas.getContext('2d');
  let saverOn = false, raf = null, motes = [];
  function sizeSaver() { const r = tv.getBoundingClientRect(); saverCanvas.width = r.width * devicePixelRatio; saverCanvas.height = r.height * devicePixelRatio; }
  function initMotes() { motes = []; const n = reduce ? 0 : 36; for (let i = 0; i < n; i++) motes.push({ x: Math.random() * saverCanvas.width, y: Math.random() * saverCanvas.height, r: (Math.random() * 1.6 + 0.5) * devicePixelRatio, vy: -(Math.random() * 0.25 + 0.05) * devicePixelRatio, a: Math.random() * 0.35 + 0.06, sage: Math.random() > 0.6 }); }
  function saverFrame() { sctx.clearRect(0, 0, saverCanvas.width, saverCanvas.height); for (const m of motes) { m.y += m.vy; if (m.y < -8) { m.y = saverCanvas.height + 8; m.x = Math.random() * saverCanvas.width; } sctx.beginPath(); sctx.arc(m.x, m.y, m.r, 0, 6.283); sctx.fillStyle = (m.sage ? 'rgba(116,198,161,' : 'rgba(244,197,68,') + m.a + ')'; sctx.fill(); } raf = requestAnimationFrame(saverFrame); }
  function openSaver() { saverOn = true; saver.classList.add('show'); saver.querySelector('.big').textContent = 'all is well'; saver.querySelector('.sub').textContent = 'fleet breathing · chain intact · ' + shortFmt(simMin); sizeSaver(); initMotes(); if (!reduce) { cancelAnimationFrame(raf); raf = requestAnimationFrame(saverFrame); } }
  function closeSaver() { saverOn = false; saver.classList.remove('show'); cancelAnimationFrame(raf); resetIdle(); }

  // ---------- idle ----------
  let idleT;
  function resetIdle() { clearTimeout(idleT); idleT = setTimeout(() => { if (!saverOn && !overlayOpen && !pipOpen && engaged && homeMode !== 'watching') openSaver(); }, 25000); }

  // ---------- alerts ----------
  const alertBanner = $('alertBanner');
  const alertPool = [{ k: 'Motion', z: 'Back Gate' }, { k: 'Motion', z: 'Register 2' }, { k: 'Presence', z: 'Patio' }, { k: 'Door', z: 'Side entrance' }, { k: 'Glass', z: 'Storefront' }];
  let alertsOn = false, severity = 'gentle', alertTimer = null, alertHideT = null, audioCtx = null;
  function warmAudio() { try { if (!audioCtx) audioCtx = new (window.AudioContext || window.webkitAudioContext)(); if (audioCtx.state === 'suspended') audioCtx.resume(); } catch (e) { /* no audio */ } }
  function chime() {
    // never chime from a hidden page — the alert loop keeps running, but only
    // a TV someone can actually see gets to make a sound
    if (severity === 'gentle' || !audioCtx || document.hidden) return;
    try { const o = audioCtx.createOscillator(), g = audioCtx.createGain(), t = audioCtx.currentTime; o.type = 'sine'; o.frequency.setValueAtTime(severity === 'assertive' ? 880 : 620, t); o.frequency.exponentialRampToValueAtTime(severity === 'assertive' ? 1175 : 784, t + 0.08); g.gain.setValueAtTime(0.0001, t); g.gain.exponentialRampToValueAtTime(severity === 'assertive' ? 0.09 : 0.05, t + 0.02); g.gain.exponentialRampToValueAtTime(0.0001, t + 0.5); o.connect(g); g.connect(audioCtx.destination); o.start(t); o.stop(t + 0.55); } catch (e) { /* no audio */ }
  }
  function fireAlert(kind) {
    simMin += 1;
    if (kind === 'doorbell') { showPip(); return; }
    const a = alertPool[Math.floor(Math.random() * alertPool.length)];
    $('alertKind').textContent = a.k; $('alertZone').textContent = a.z; $('alertTime').textContent = shortFmt(simMin);
    alertBanner.classList.remove('sev-gentle', 'sev-standard', 'sev-assertive');
    alertBanner.classList.add('sev-' + severity, 'show');
    chime();
    if (saverOn) { saver.classList.add('alerting'); saver.querySelector('.big').textContent = '⚠ ' + a.k + ' · ' + a.z; }
    clearTimeout(alertHideT);
    alertHideT = setTimeout(hideAlert, severity === 'assertive' ? 8000 : severity === 'standard' ? 6000 : 4000);
  }
  function hideAlert() { alertBanner.classList.remove('show'); if (saverOn) { saver.classList.remove('alerting'); saver.querySelector('.big').textContent = 'all is well'; } }

  const alertsToggle = $('alertsToggle'), sevGroup = $('severityGroup'), alertTest = $('alertTest');
  if (alertsToggle) alertsToggle.addEventListener('click', () => {
    alertsOn = !alertsOn; alertsToggle.setAttribute('aria-checked', alertsOn ? 'true' : 'false'); alertsToggle.querySelector('.lab').textContent = alertsOn ? 'Alerts on' : 'Alerts off';
    if (alertsOn) { warmAudio(); showToast('Alerts on · coarse events · ' + severity + ' tone'); fireAlert(); clearInterval(alertTimer); alertTimer = setInterval(() => fireAlert(Math.random() < 0.3 ? 'doorbell' : null), 12000); }
    else { clearInterval(alertTimer); alertTimer = null; hideAlert(); showToast('Alerts off'); }
  });
  if (sevGroup) sevGroup.addEventListener('click', (e) => { const b = e.target.closest('.sg'); if (!b) return; severity = b.getAttribute('data-sev'); sevGroup.querySelectorAll('.sg').forEach((x) => x.setAttribute('data-on', x === b ? '1' : '0')); });
  if (alertTest) alertTest.addEventListener('click', () => { warmAudio(); fireAlert(); });
  if (alertBanner) alertBanner.addEventListener('click', hideAlert);

  // ---------- select ----------
  function select() {
    if (!current) return;
    const a = current.getAttribute('data-action');
    if (a === 'edition:home') { edition = 'home'; homeMode = 'wall'; render(); showToast('Home · the Witness Wall'); }
    else if (a === 'edition:business') { edition = 'business'; render(); showToast('Business · the Witness Board'); }
    else if (a === 'edition:apartment') { edition = 'apartment'; render(); showToast('Apartment · the peephole'); }
    else if (a === 'density:glance') { bizDensity = 'glance'; render(); }
    else if (a === 'density:monitor') { bizDensity = 'monitor'; render(); showToast('Monitor · heavy ops view'); }
    else if (a && a.indexOf('filter:') === 0) { filter = a.slice(7); render(); }
    else if (a === 'ackall') { bizEvents.forEach((e) => { e.acked = true; }); render(); showToast('All events acknowledged'); }
    else if (a === 'ack') { const id = +current.getAttribute('data-id'); if (bizEvents[id]) bizEvents[id].acked = true; render(); showToast('Event acknowledged'); }
    else if (a === 'incident-review') { showToast('Dispute pack ready · signed timeline + sealed clip · verifies offline ✓'); }
    else if (a === 'tile') { showToast(current.getAttribute('data-name') + ' · online · chain intact'); }
    else if (a === 'event') { showToast('Event verified · signature ✓ · chain intact'); }
    else if (a === 'watch') { homeMode = 'watching'; render(); showToast('Now playing · your fleet keeps witnessing'); }
    else if (a === 'stopwatch') { homeMode = 'wall'; render(); showToast('Back to the Witness Wall'); }
    else if (a === 'doorbell') { warmAudio(); fireAlert('doorbell'); }
    else if (a === 'knock') { warmAudio(); aptKnocking = true; aptLog.unshift({ t: hexTime(simMin), w: 'Knock · Front Door' }); if (aptLog.length > 4) aptLog.pop(); render(); showToast('Someone’s knocking · Front Door'); showPip({ kind: 'Front Door', sub: 'Someone’s knocking — peek when ready', parcel: false }, false); }
    else if (a === 'peek') { warmAudio(); showPip({ kind: 'Front Door', sub: 'Live peek · nobody has to get up', parcel: false }, true); }
    else if (a === 'pip-view') { pip.classList.add('expanded'); pipSub.textContent = 'Coarse live view · two-way ready · raw stays on device'; }
    else if (a === 'pip-talk') { showToast('Talk · two-way audio (demo)'); }
    else if (a === 'pip-dismiss') { closePip(); }
    else if (a === 'incident') { captureIncident(); }
    else if (a === 'disputepack') { showToast('Dispute pack exported · verifies offline ✓'); }
    else if (a === 'breakglass') { openBreakGlass(); }
    else if (a === 'approve') { approve(); }
    else if (a === 'cancel') { closeOverlay(); }
    else if (a === 'screensaver') { openSaver(); }
  }

  // ---------- input ----------
  function handleNav(dir) {
    resetIdle();
    if (saverOn) { closeSaver(); return; }
    if (dir === 'menu') { if (pipOpen) { closePip(); return; } if (alertBanner.classList.contains('show')) { hideAlert(); return; } if (overlayOpen) { closeOverlay(); return; } if (edition === 'home' && homeMode === 'watching') { homeMode = 'wall'; render(); } return; }
    if (dir === 'saver') { openSaver(); return; }
    if (dir === 'select') { select(); return; }
    move(dir);
  }
  let engaged = false;
  const shell = document.querySelector('.tv-shell');
  if (shell) { shell.addEventListener('pointerenter', () => { engaged = true; }); shell.addEventListener('pointerleave', () => { if (document.activeElement !== tv) engaged = false; }); }
  tv.addEventListener('focus', () => { engaged = true; }); tv.addEventListener('blur', () => { engaged = false; });
  document.addEventListener('keydown', (e) => {
    if (!engaged) return;
    const map = { ArrowUp: 'up', ArrowDown: 'down', ArrowLeft: 'left', ArrowRight: 'right' };
    if (map[e.key]) { e.preventDefault(); handleNav(map[e.key]); }
    else if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); handleNav('select'); }
    else if (e.key === 'Escape') { e.preventDefault(); handleNav('menu'); }
    else if (e.key === 's' || e.key === 'S') { e.preventDefault(); handleNav('saver'); }
  });
  document.querySelectorAll('[data-nav]').forEach((b) => b.addEventListener('click', () => { engaged = true; handleNav(b.getAttribute('data-nav')); tv.focus(); }));
  const onClick = (e) => { const f = e.target.closest('.f'); if (!f) return; current = f; paint(); select(); resetIdle(); };
  stage.addEventListener('click', onClick); overlay.addEventListener('click', onClick); pip.addEventListener('click', onClick);
  const sw = document.querySelector('.switch'); if (sw) sw.addEventListener('click', (e) => { const f = e.target.closest('.f'); if (!f) return; current = f; paint(); select(); });
  window.addEventListener('resize', () => { if (saverOn) { sizeSaver(); initMotes(); } });

  // ---------- skins ----------
  const skinShell = document.querySelector('.tv-shell'), skinbar = $('skinbar');
  const SKINS = ['midnight', 'daylight', 'noir', 'sunset', 'terminal', 'paper'];
  function applySkin(name, save) {
    if (SKINS.indexOf(name) === -1) name = 'midnight';
    if (name === 'midnight') delete skinShell.dataset.skin; else skinShell.dataset.skin = name;
    if (skinbar) skinbar.querySelectorAll('.skinbtn').forEach((b) => b.setAttribute('aria-pressed', b.getAttribute('data-skin') === name ? 'true' : 'false'));
    if (save) { try { localStorage.setItem('scv-skin', name); } catch (e) { /* private mode */ } }
  }
  if (skinbar) skinbar.addEventListener('click', (e) => { const b = e.target.closest('.skinbtn'); if (b) applySkin(b.getAttribute('data-skin'), true); });

  // ---------- host-selected profile (use case) ----------
  // A surface embedding the wall — the Flasher, the Lab, a bar's install page —
  // can pin the edition to the use case it serves: ?profile=home|business|apartment
  // on the URL, the witness:profile message, or window.witnessWall.profile().
  // Pinning is what makes each surface "skippable to its use case": the wall
  // boots straight into the right view, and a discovered fleet updates THAT
  // view instead of yanking back to home. The on-screen edition chips still
  // work — pinning sets the default, it doesn't lock the user out.
  function applyProfile(name) {
    name = String(name || '').toLowerCase();
    if (name !== 'home' && name !== 'business' && name !== 'apartment') return;
    hostEdition = name;
    edition = name;
    if (name === 'home') homeMode = 'wall';
    render();
  }

  // ---------- a Canary appears (flash → shows up on the wall) ----------
  function appearDevice(dev, label) {
    if (!liveFleet) { liveFleet = homeTiles.map((t) => ({ name: t.n, online: true })); if (!liveHost) liveHost = 'your fleet'; setLive(true, liveHost); }
    if (liveFleet.some((d) => d.name === dev.name)) { showToast(dev.name + ' is already in the fleet'); return; }
    liveFleet.push({ name: dev.name, online: dev.online !== false });
    justAppeared = dev.name;
    // Jump to the home wall for the "it appeared!" moment — unless a host
    // pinned the edition to its use case; then the pinned view updates in place.
    if (!hostEdition && (edition !== 'home' || homeMode !== 'wall')) { edition = 'home'; homeMode = 'wall'; }
    render(); renderDevices();
    showToast('✨ ' + dev.name + ' just joined the fleet' + (label ? ' · ' + label : ''));
    setTimeout(() => { justAppeared = null; }, 1700);
  }
  const flashPool = ['Porch', 'Side Gate', 'Nursery', 'Workshop', 'Stockroom', 'Balcony'];
  function simulateFlash() {
    const base = liveFleet ? liveFleet.map((d) => d.name) : homeTiles.map((t) => t.n);
    const name = flashPool.find((n) => base.indexOf(n) === -1) || ('Canary ' + (base.length + 1));
    appearDevice({ name: name, online: true }, 'just flashed');
  }
  // Populate from a REAL fleet a host discovered on the LAN (the Flasher's
  // native side finds the just-flashed device and posts it here).
  function applyFleet(data, highlight) {
    const devs = Array.isArray(data) ? data : ((data && (data.devices || data.canaries || data.fleet)) || []);
    if (!devs.length) return;
    liveFleet = devs.map((x) => ({ name: String(x.name || x.id || x.hostname || 'Canary').slice(0, 40), online: x.online !== false }));
    liveHost = 'your LAN';
    setLive(true, liveHost);
    if (highlight) justAppeared = String(highlight).slice(0, 40);
    if (!hostEdition && (edition !== 'home' || homeMode !== 'wall')) { edition = 'home'; homeMode = 'wall'; }
    render(); renderDevices();
    showToast('✨ Found on your network · ' + liveFleet.length + ' Canaries');
    setTimeout(() => { justAppeared = null; }, 1700);
  }

  // ---------- connect to a real kernel ----------
  const kernelUrl = $('kernelUrl'), connectBtn = $('connectBtn'), demoBtn = $('demoBtn'), liveDemoBtn = $('liveDemoBtn'),
    goLiveBtn = $('goLiveBtn'), flashBtn = $('flashBtn'), cpStatus = $('cpStatus'), cpDevices = $('cpDevices'), cpJson = $('cpJson'), liveDot = $('liveDot'), liveText = $('liveText');
  function setStatus(msg, cls) { if (!cpStatus) return; cpStatus.textContent = msg; cpStatus.className = 'cp-status' + (cls ? ' ' + cls : ''); }
  function setLive(on, host) { if (!liveDot) return; liveDot.className = 'live-dot ' + (on ? 'live' : 'demo'); liveText.textContent = on ? ('Live — ' + (liveFleet ? liveFleet.length : 0) + ' Canaries from ' + host) : 'Demo fleet — not connected to a kernel'; }
  function renderDevices() { if (!cpDevices) return; cpDevices.innerHTML = liveFleet ? liveFleet.map((d) => `<span class="dv${d.online ? '' : ' off'}"><span class="d"></span>${esc(d.name)}${d.online ? '' : ' · offline'}</span>`).join('') : ''; }
  function showJson(data) { if (!cpJson) return; if (data) { cpJson.innerHTML = highlightJSON(data); cpJson.classList.add('show'); } else { cpJson.classList.remove('show'); cpJson.textContent = ''; } }

  let pollT = null, lastNames = null;
  function stopPoll() { if (pollT) { clearInterval(pollT); pollT = null; } lastNames = null; }
  function startPoll(url) {
    stopPoll();
    if (/\.json(\?|$)/.test(url)) return; // a static file has nothing to re-poll
    const endpoint = url.replace(/\/$/, '') + '/api/fleet';
    pollT = setInterval(async () => {
      try {
        const res = await fetch(endpoint, { mode: 'cors', headers: { accept: 'application/json' } });
        if (!res.ok) return;
        const data = await res.json();
        const devs = Array.isArray(data) ? data : (data.devices || data.canaries || data.fleet || []);
        const names = devs.map((d) => d.name || d.id || d.hostname || 'Canary');
        showJson(data);
        if (lastNames) names.filter((n) => lastNames.indexOf(n) === -1).forEach((n) => appearDevice({ name: n, online: true }, 'discovered'));
        lastNames = names;
      } catch (e) { /* transient */ }
    }, 4000);
  }

  async function connectFleet(url, auto, label) {
    if (!url) return;
    const shown = label || url;
    setStatus('Scanning ' + shown + ' …', 'scan');
    try {
      const endpoint = /\.json(\?|$)/.test(url) ? url : url.replace(/\/$/, '') + '/api/fleet';
      const ctrl = new AbortController(); const to = setTimeout(() => ctrl.abort(), 3500);
      const res = await fetch(endpoint, { signal: ctrl.signal, mode: 'cors', headers: { accept: 'application/json' } });
      clearTimeout(to);
      if (!res.ok) throw new Error('HTTP ' + res.status);
      const data = await res.json();
      const devices = Array.isArray(data) ? data : (data.devices || data.canaries || data.fleet || []);
      if (!devices.length) throw new Error('kernel returned no devices');
      liveFleet = devices.map((d) => ({ name: d.name || d.id || d.hostname || 'Canary', online: d.online !== false }));
      liveHost = shown; setLive(true, shown); render(); renderDevices(); showJson(data);
      setStatus('Connected — ' + liveFleet.length + ' Canaries from ' + shown + '. They drive the fleet above now.', 'ok');
      lastNames = liveFleet.map((d) => d.name); startPoll(url);
      try { if (!/\.json/.test(url)) localStorage.setItem('scv-kernel', url); } catch (e) { /* private mode */ }
      clearFound(); stopDetect();
    } catch (e) {
      liveFleet = null; liveHost = null; setLive(false); render(); renderDevices(); showJson(null); stopPoll();
      const mixed = location.protocol === 'https:' && /^http:\/\//i.test(url);
      if (auto) setStatus('No kernel found automatically — showing the demo fleet. Try the live demo kernel, or enter your kernel’s address.');
      else if (mixed) setStatus('Blocked: an https page can’t reach an http device on your LAN (mixed-content + the page’s security policy). Run the emulator on your LAN over http, serve it from the kernel/hub, or use the desktop app — see below.', 'err');
      else setStatus('Couldn’t reach a kernel there (' + (e && e.message || 'no response') + ') — showing the demo fleet.', 'err');
      if (auto) startDetect(false);   // fall back to watching the well-known addresses
    }
  }

  // ---------- auto-detection: live-watch the well-known addresses ----------
  // The same list every SecuraCV surface probes, in the same order (the tvOS
  // Wall, the desktop Flasher, the Lab's witness host): the hub/kernel
  // convention first, then the bare device — a fresh Canary answers
  // /api/fleet at http://canary.local (port 80, no hub needed), which is the
  // address the getting-started docs put in front of people. The real Wall
  // (tv/app.js) probes the identical list; tests/tv-wall.test.mjs pins the two.
  const WELL_KNOWN = ['http://canary.local:8099', 'http://canary.local'];
  // An https page can never fetch an http LAN device (mixed content), so
  // probing from the public site would only fail forever; the loop runs where
  // it can actually work — served from the hub, over http, or in an app shell.
  const canProbe = location.protocol !== 'https:';
  let detectT = null, detecting = false, foundKernel = null;

  async function probeKernel(url) {
    const ctrl = new AbortController(); const to = setTimeout(() => ctrl.abort(), 2500);
    try {
      const res = await fetch(url.replace(/\/$/, '') + '/api/fleet', { signal: ctrl.signal, mode: 'cors', headers: { accept: 'application/json' } });
      if (!res.ok) return null;
      const data = await res.json();
      const devices = Array.isArray(data) ? data : (data.devices || data.canaries || data.fleet || []);
      return devices.length ? { url: url, count: devices.length } : null;
    } catch (e) { return null; } finally { clearTimeout(to); }
  }

  // A kernel answered while the demo was up: don't yank the wall out from
  // under the user — light up the Go live switch and let THEM flip it. (Once
  // they have, the kernel is remembered and future visits reconnect quietly.)
  function offerLive(found) {
    if (foundKernel || liveFleet) return;
    foundKernel = found;
    const host = found.url.replace(/^https?:\/\//, '');
    const noun = found.count === 1 ? 'Canary' : 'Canaries';
    if (kernelUrl) kernelUrl.value = found.url;
    if (goLiveBtn) { goLiveBtn.textContent = '✨ Go live — ' + found.count + ' ' + noun + ' found'; goLiveBtn.hidden = false; }
    setStatus('✨ A kernel is answering at ' + found.url + ' — ' + found.count + ' ' + noun + ' ready to show. Press Go live to switch from the demo to your real fleet.', 'found');
    if (liveText) liveText.textContent = 'Kernel found at ' + host + ' — ready to go live';
    showToast('✨ Kernel found on your network · ' + host);
  }

  function clearFound() { foundKernel = null; if (goLiveBtn) goLiveBtn.hidden = true; }

  async function detectTick() {
    detectT = null;
    if (!detecting || liveFleet || foundKernel) return;
    if (document.hidden) { scheduleDetect(); return; }
    for (let i = 0; i < WELL_KNOWN.length; i++) {
      const found = await probeKernel(WELL_KNOWN[i]);
      if (!detecting || liveFleet || foundKernel) return;   // something connected mid-probe
      if (found) { offerLive(found); return; }
    }
    scheduleDetect();
  }

  function scheduleDetect() {
    if (!detecting || liveFleet || foundKernel) return;
    if (detectT) clearTimeout(detectT);
    detectT = setTimeout(detectTick, 12000);
  }

  function startDetect(announce) {
    if (!canProbe) return;
    detecting = true;
    if (announce) setStatus('Watching for a kernel on your network (canary.local) — showing the demo fleet until one answers.');
    detectTick();
  }
  function stopDetect() { detecting = false; if (detectT) { clearTimeout(detectT); detectT = null; } }
  document.addEventListener('visibilitychange', () => { if (!document.hidden && detecting && !detectT) detectTick(); });

  if (goLiveBtn) goLiveBtn.addEventListener('click', async () => {
    const target = foundKernel && foundKernel.url;
    if (!target) return;
    goLiveBtn.disabled = true;
    await connectFleet(target, false);
    goLiveBtn.disabled = false;
    clearFound();
    if (liveFleet) showToast('● LIVE · this is your real fleet now');
    else scheduleDetect();   // it vanished between the probe and the press
  });
  if (liveDemoBtn) liveDemoBtn.addEventListener('click', () => connectFleet('demo-fleet.json', false, 'demo kernel'));
  if (connectBtn) connectBtn.addEventListener('click', () => connectFleet(kernelUrl.value.trim(), false));
  if (flashBtn) flashBtn.addEventListener('click', simulateFlash);
  if (demoBtn) demoBtn.addEventListener('click', () => { liveFleet = null; liveHost = null; setLive(false); render(); renderDevices(); showJson(null); stopPoll(); clearFound(); setStatus('Using the demo fleet.'); try { localStorage.removeItem('scv-kernel'); } catch (e) { /* private mode */ } startDetect(false); });
  if (kernelUrl) kernelUrl.addEventListener('keydown', (e) => { if (e.key === 'Enter') connectFleet(kernelUrl.value.trim(), false); });

  // ---------- boot ----------
  let skin = 'midnight';
  try { skin = new URLSearchParams(location.search).get('skin') || localStorage.getItem('scv-skin') || 'midnight'; } catch (e) { /* ignore */ }
  applySkin(skin, false);
  try { applyProfile(new URLSearchParams(location.search).get('profile')); } catch (e) { /* ignore */ }
  render();
  setLive(false);
  resetIdle();
  // Reconnect a remembered/URL-specified kernel (the user already went live
  // once — no ceremony, just reconnect). Otherwise start the live watch on
  // the well-known addresses; the first kernel that answers becomes the
  // "Go live" offer. (On the public https site the watch is off by design —
  // mixed content makes it unwinnable — and the live demo kernel button
  // always works because it's same-origin.)
  let boot = null;
  try { boot = new URLSearchParams(location.search).get('kernel') || localStorage.getItem('scv-kernel'); } catch (e) { /* ignore */ }
  if (boot) { if (kernelUrl && !/\.json/.test(boot)) kernelUrl.value = boot; connectFleet(boot, true); }
  else startDetect(true);

  // ---------- embed API (host apps: Flasher / Lab) ----------
  // A host can drive the wall without knowing its internals — same-window via
  // window.witnessWall.*, or cross-window (iframe) via postMessage. The Flasher
  // calls appear() right after it flashes a device, so it shows up on the wall.
  window.witnessWall = {
    appear: (name, label) => { if (name) appearDevice({ name: String(name).slice(0, 40), online: true }, label || 'just flashed'); },
    connect: (url) => { if (url) connectFleet(String(url), false); },
    fleet: (data, highlight) => applyFleet(data, highlight),
    skin: (name) => applySkin(name, true),
    profile: (name) => applyProfile(name),
    simulateFlash,
  };
  window.addEventListener('message', (e) => {
    const d = e && e.data;
    if (!d || typeof d !== 'object') return;
    if (d.type === 'witness:appear' && d.name) window.witnessWall.appear(d.name, d.label);
    else if (d.type === 'witness:connect' && d.url) window.witnessWall.connect(d.url);
    else if (d.type === 'witness:fleet' && d.fleet) window.witnessWall.fleet(d.fleet, d.highlight);
    else if (d.type === 'witness:skin' && d.skin) window.witnessWall.skin(d.skin);
    else if (d.type === 'witness:profile' && d.profile) window.witnessWall.profile(d.profile);
  });
  // Tell an embedding host we're ready to receive appear()/connect() messages.
  // Posted twice — at module eval and again on window load — so a host that
  // attaches its listener between the two still gets the handshake.
  function announceReady() {
    try { if (window.parent && window.parent !== window) window.parent.postMessage({ type: 'witness:ready' }, '*'); } catch (e) { /* not embedded */ }
  }
  announceReady();
  window.addEventListener('load', announceReady);
}
