// Track a two-port Canary Vision bring-up so the flasher can insist on BOTH
// flashes and never let you walk away half-done. A Vision is two chips on two
// ports — the ESP32 (Vision firmware) and the Grove Vision AI V2 / WE2 camera
// module (its person-detection model) — flashed in two separate connect cycles.
// This remembers which of the two you've done so the done screen can say
// "1 of 2 — now the other port" and celebrate only when both are in.
//
// Scope is this bring-up on this tab: held in sessionStorage so an accidental
// reload between ports doesn't lose your place, and gone when the tab closes.
// It holds NO secrets — just two booleans — so there's no disk-privacy trade
// (unlike wifi-memory's opt-in persist tier). Degrades to in-memory when
// sessionStorage is absent (Node import graph) or blocked (private mode).
//
// Starting a fresh pair is automatic: once both ports are done, the next flash
// begins a new pair (you're onto the next board), so a batch just works.
//
// Pure + storage-injectable so the behaviour is proven in host tests
// (tests/vision_session.test.js) without a real sessionStorage.

const STORE_KEY = "scv-vision";

/**
 * @param {Storage|null} storage  sessionStorage-shaped backend, or null when it
 *   is absent/blocked — then only the in-memory tier works and nothing throws.
 */
export function makeVisionSession(storage) {
  let mem = { esp32: false, we2: false }; // in-memory mirror / fallback

  const read = () => {
    try {
      const raw = storage && storage.getItem(STORE_KEY);
      if (!raw) return { ...mem };
      const o = JSON.parse(raw);
      return { esp32: !!(o && o.esp32), we2: !!(o && o.we2) };
    } catch { return { ...mem }; }
  };
  const write = (parts) => {
    mem = { esp32: !!parts.esp32, we2: !!parts.we2 };
    try { if (storage) storage.setItem(STORE_KEY, JSON.stringify(mem)); } catch { /* private mode / absent */ }
  };

  /** The two flags flashed so far this pair — { esp32, we2 }. */
  const parts = () => read();

  /**
   * Record that `part` ("esp32" | "we2") is now flashed. If the previous pair
   * was already complete, this starts a fresh pair (you've moved on to the next
   * board) so the count doesn't stay stuck at "2 of 2". Unknown parts are ignored.
   */
  const markDone = (part) => {
    if (part !== "esp32" && part !== "we2") return read();
    const cur = read();
    const base = cur.esp32 && cur.we2 ? { esp32: false, we2: false } : cur;
    base[part] = true;
    write(base);
    return { ...base };
  };

  /** Forget the current pair (start over). */
  const reset = () => { write({ esp32: false, we2: false }); };

  return { parts, markDone, reset };
}

// Default singleton over the browser's sessionStorage (absent/blocked → in-memory
// tier only, never throws).
const browserSession = (() => { try { return globalThis.sessionStorage || null; } catch { return null; } })();
export const visionSession = makeVisionSession(browserSession);
