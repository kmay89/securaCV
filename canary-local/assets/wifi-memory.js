// Remember the home Wi-Fi so a whole batch of Canaries provisions without
// re-typing it into every board. Two tiers, both honest to the flasher's
// promise that "what you type stays on this page and goes only to the chip":
//
//   · SESSION (default, no opt-in): held in memory for this tab only. Flash
//     board after board and each one auto-fills; it never touches disk and is
//     gone the moment you close or reload the page. Nothing is ever sent
//     anywhere — same guarantee as typing it fresh each time, minus the typing.
//   · PERSIST (explicit opt-in): also written to THIS browser on THIS computer
//     (localStorage), so it survives closing the tab. Local-only, never phoned
//     anywhere, one-click Forget. Off by default because it writes a secret to
//     disk — a deliberate convenience trade the user makes, not a default.
//
// A browser cannot read the OS's saved Wi-Fi password (sandbox); the native
// desktop flasher can upgrade the persistent store to the OS keychain.
//
// Pure + storage-injectable so the behaviour is proven in host tests
// (tests/wifi_memory.test.js) without a real localStorage.

const STORE_KEY = "scv-wifi";

/**
 * @param {Storage|null} storage  localStorage-shaped backend, or null when it
 *   is absent (Node import graph) or blocked (private mode) — then only the
 *   in-memory session tier works and nothing throws.
 */
export function makeWifiMemory(storage) {
  let session = null; // { ssid, pass } — this tab only, never written to disk

  const readDisk = () => {
    try {
      const raw = storage && storage.getItem(STORE_KEY);
      if (!raw) return null;
      const o = JSON.parse(raw);
      return o && typeof o.ssid === "string" && o.ssid
        ? { ssid: o.ssid, pass: typeof o.pass === "string" ? o.pass : "" }
        : null;
    } catch { return null; }
  };
  const writeDisk = (creds) => { try { storage.setItem(STORE_KEY, JSON.stringify(creds)); } catch { /* private mode / absent */ } };
  const clearDisk = () => { try { storage.removeItem(STORE_KEY); } catch { /* ignore */ } };

  const forget = () => { session = null; clearDisk(); };

  /**
   * Remember `creds` for auto-fill. Always kept for the session; when `persist`
   * is true it is also written to this browser (survives a reload), and when
   * false any prior disk copy is removed but the session copy stays. An empty
   * SSID forgets everything (there is nothing worth remembering).
   */
  const remember = (creds, persist) => {
    const ssid = creds && typeof creds.ssid === "string" ? creds.ssid.trim() : "";
    if (!ssid) { forget(); return; }
    session = { ssid, pass: (creds && creds.pass) || "" };
    if (persist) writeDisk(session); else clearDisk();
  };

  return {
    /** Creds to pre-fill: this session's if set, else the persisted one, else null. */
    recall: () => session || readDisk(),
    /** True if a copy is saved to this browser's disk (survives closing the tab). */
    isPersisted: () => !!readDisk(),
    remember,
    forget,
  };
}

// Default singleton over the browser's localStorage (absent/blocked → session
// tier only, never throws).
const browserStorage = (() => { try { return globalThis.localStorage || null; } catch { return null; } })();
export const wifiMemory = makeWifiMemory(browserStorage);
