// Host for the vendored Witness Wall emulator (witness/witness.html — see
// witness/PROVENANCE.txt; the emulator itself is canonical in the website
// repo and byte-identical to the Flasher's copy).
//
// What this host adds, and only this:
//   - ?profile= / ?skin= forwarding, so a surface can boot the wall straight
//     into its use case (witness-wall.html?profile=business → the Witness
//     Board) without touching the shared emulator.
//   - In the DESKTOP LAB (Tauri shell): the native LAN discovery a browser
//     can't do. Each tick first browses mDNS for `_securacv._tcp`
//     (`fleet_scan`, the Flasher's twin — only where native_capabilities
//     reports `mdns`) and adds every board it hears to the candidate list,
//     then polls the `witness_discover` Rust command (reqwest; `.local`
//     resolves via the OS — Bonjour on macOS, avahi on Linux) and posts the
//     found fleet into the wall over the shared postMessage contract
//     (tvos/EMBED_IN_APPS.md) — the same controller shape as the Flasher's
//     (desktop/src/app.js). The kernel addresses stay first — the typed
//     base, then the well-known `canary.local:8099`, `canary.local:8799`
//     (the kernel's own API port) and `canary.local`, in the Apple TV's
//     order — and the browsed boards come only after them: the kernel
//     advertises no `_securacv._tcp`, and a WAP or display answers
//     /api/fleet with a one-board self-report, so a board tried first would
//     stand in for the kernel's whole fleet on every tick. All LAN traffic
//     goes through the Rust commands, never a page fetch.
//   - In a BROWSER: no scanning (browsers can't reach the LAN like that);
//     the emulator's own demo fleet + connect panel stand, and the status
//     line stays quiet rather than pretending.

const frame = document.getElementById("witness-frame");
const scanEl = document.getElementById("wall-scan");

// Forward the host page's own ?profile= / ?skin= into the emulator, so links
// like witness-wall.html?profile=business boot the right view.
try {
  const q = new URLSearchParams(location.search);
  const pass = new URLSearchParams();
  for (const k of ["profile", "skin", "kernel"]) { const v = q.get(k); if (v) pass.set(k, v); }
  if (frame && pass.toString()) frame.src = "witness/witness.html?" + pass.toString();
} catch (_) { /* the plain wall still boots */ }

// Native discovery — desktop Lab only (withGlobalTauri exposes __TAURI__).
const invoke = window.__TAURI__ && window.__TAURI__.core && window.__TAURI__.core.invoke;
if (invoke && frame) {
  const status = (msg, live) => { if (scanEl) { scanEl.textContent = msg; scanEl.classList.toggle("live", !!live); } };
  // "/" = our own origin: the wall is our iframe, and a LAN fleet (device
  // names, who is home) goes nowhere else even if something else were framed.
  const post = (m) => { try { if (frame.contentWindow) frame.contentWindow.postMessage(m, "/"); } catch (_) {} };
  // Asked once: only a build that registers fleet_scan says `mdns`, and only
  // one that runs the menu bar companion says `notifications` (desktop; a
  // mobile shell neither registers nor advertises them), so a missing
  // capability means "poll only", never a failing invoke every tick.
  let caps = null;
  const probeCaps = async () => {
    if (caps === null) {
      try { caps = (await invoke("native_capabilities")) || {}; }
      catch (_) { caps = {}; }
    }
    return caps;
  };
  // Where each browsed board might serve /api/fleet. IPv4 when the browse
  // resolved one (a bare IPv6 literal would need brackets, and a link-local
  // one a zone), else its own `.local` hostname. A board still on the old
  // port-1 "formality" advert serves its page on :80 (the Flasher's fleet
  // book probes it the same way).
  const boardBases = (sightings) => {
    const b = [];
    for (const s of sightings || []) {
      if (!s) continue;
      const addr = s.ip && !String(s.ip).includes(":") ? s.ip : s.host;
      if (!addr) continue;
      b.push(s.port && s.port !== 1 ? `http://${addr}:${s.port}` : `http://${addr}`);
    }
    return b;
  };
  const bases = (sightings) => {
    const b = [];
    try { const k = localStorage.getItem("scv-kernel"); if (k && /^http:\/\//i.test(k)) b.push(k); } catch (_) {}
    b.push("http://canary.local:8099", "http://canary.local:8799", "http://canary.local");
    // Boards last: witness_discover returns the FIRST /api/fleet that
    // answers, and a board's answer is its own one-row self-report.
    b.push(...boardBases(sightings));
    return [...new Set(b)];
  };
  // The menu bar companion polls the same kernel addresses in the
  // background: hand it the typed kernel base and the defaults (not the
  // browsed boards — it browses for itself), and only when they change.
  let companionSent = "";
  const syncCompanion = async () => {
    const kernelBases = bases([]);
    const key = JSON.stringify(kernelBases);
    if (key === companionSent) return;
    try { await invoke("companion_set_bases", { bases: kernelBases }); companionSent = key; }
    catch (_) { /* the companion keeps the addresses it had */ }
  };
  let inFlight = false, found = false, timer = null;
  const tick = async () => {
    if (inFlight) return schedule();
    inFlight = true;
    const { mdns, notifications } = await probeCaps();
    if (notifications) await syncCompanion();
    let sightings = [];
    if (mdns) {
      try { sightings = (await invoke("fleet_scan", { timeoutMs: 2500 })) || []; }
      catch (_) { /* no multicast here, or nobody announcing — the poll still runs */ }
    }
    let fleet = null;
    try { fleet = await invoke("witness_discover", { bases: bases(sightings) }); }
    catch (_) { /* nothing answering yet */ }
    inFlight = false;
    if (fleet) {
      const n = (fleet.devices || fleet.canaries || (Array.isArray(fleet) ? fleet : [])).length;
      post({ type: "witness:fleet", fleet });
      found = true;
      status("● Live — " + (n || "your") + " Canar" + (n === 1 ? "y" : "ies") + " on your network", true);
    } else if (!found && sightings.length) {
      // Heard, but nobody serves /api/fleet: sense/vision run no HTTP server,
      // so an announcement is not a fleet — say what is true.
      const n = sightings.length;
      status(n + " Canar" + (n === 1 ? "y" : "ies") + " announced on this network — none serves the fleet document yet.");
    } else if (!found) {
      status("Scanning your network for Canaries… nothing answering yet — flash one, or make sure a Canary is on this Wi-Fi.");
    }
    schedule();
  };
  const schedule = () => {
    if (timer) clearTimeout(timer);
    if (document.hidden) { timer = null; return; }
    timer = setTimeout(() => tick().catch(() => {}), 10000);
  };
  document.addEventListener("visibilitychange", () => { if (!document.hidden) tick().catch(() => {}); });
  // The wall announces witness:ready when its iframe boots — sync then, and
  // start now (whichever lands first wins; ticks self-serialize).
  window.addEventListener("message", (e) => { const d = e && e.data; if (d && d.type === "witness:ready") tick().catch(() => {}); });
  status("Scanning your network for Canaries…");
  tick().catch(() => {});
}
