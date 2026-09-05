// Host for the vendored Witness Wall emulator (witness/witness.html — see
// witness/PROVENANCE.txt; the emulator itself is canonical in the website
// repo and byte-identical to the Flasher's copy).
//
// What this host adds, and only this:
//   - ?profile= / ?skin= forwarding, so a surface can boot the wall straight
//     into its use case (witness-wall.html?profile=business → the Witness
//     Board) without touching the shared emulator.
//   - In the DESKTOP LAB (Tauri shell): the native LAN discovery a browser
//     can't do. It polls the `witness_discover` Rust command (reqwest;
//     `.local` resolves via the OS — Bonjour on macOS, avahi on Linux) and
//     posts the found fleet into the wall over the shared postMessage
//     contract (tvos/EMBED_IN_APPS.md) — the same controller shape as the
//     Flasher's (desktop/src/app.js). All LAN traffic goes through the Rust
//     command, never a page fetch.
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
  const bases = () => {
    const b = [];
    try { const k = localStorage.getItem("scv-kernel"); if (k && /^http:\/\//i.test(k)) b.push(k); } catch (_) {}
    b.push("http://canary.local:8099", "http://canary.local");
    return b;
  };
  let inFlight = false, found = false, timer = null;
  const tick = async () => {
    if (inFlight) return schedule();
    inFlight = true;
    let fleet = null;
    try { fleet = await invoke("witness_discover", { bases: bases() }); }
    catch (_) { /* nothing answering yet */ }
    inFlight = false;
    if (fleet) {
      const n = (fleet.devices || fleet.canaries || (Array.isArray(fleet) ? fleet : [])).length;
      post({ type: "witness:fleet", fleet });
      found = true;
      status("● Live — " + (n || "your") + " Canar" + (n === 1 ? "y" : "ies") + " on your network", true);
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
